/*
 * spectrum_snapshot.c — the header, field by field, and the banks behind it.
 */
#include "spectrum_snapshot.h"

#include <string.h>

/* Where each field sits, in the order the format lists them. Named rather
   than counted, because an off-by-one here is a machine that resumes with
   the wrong accumulator and no obvious reason why. */
#define AT_I 0x00
#define AT_SHADOW_HL 0x01
#define AT_SHADOW_DE 0x03
#define AT_SHADOW_BC 0x05
#define AT_SHADOW_AF 0x07
#define AT_HL 0x09
#define AT_DE 0x0B
#define AT_BC 0x0D
#define AT_IY 0x0F
#define AT_IX 0x11
#define AT_INTERRUPT 0x13 /* bit 2 holds IFF2; the format has no bit for IFF1 */
#define AT_R 0x14
#define AT_AF 0x15
#define AT_SP 0x17
#define AT_INTERRUPT_MODE 0x19
#define AT_BORDER 0x1A
#define AT_RAM 0x1B

/* The 128K format's three banks laid out where the 48K format has its RAM:
   5, 2 and the one paged at 0xC000; and its own fields behind them. */
#define AT_BANK_5 ((size_t)AT_RAM)
#define AT_BANK_2 ((size_t)AT_RAM + SPECTRUM_BANK_SIZE)
#define AT_PAGED_BANK ((size_t)AT_RAM + (size_t)2 * SPECTRUM_BANK_SIZE)
#define AT_PC 49179
#define AT_BANK_REGISTER 49181
#define AT_TR_DOS 49182
#define AT_REMAINING_BANKS 49183

#define IFF2_BIT 0x04
/* Not the format's but MAME's, which takes IFF1 from it. Written with IFF2,
   which is what the RETN a snapshot resumes with puts in IFF1; never read. */
#define MAME_IFF1_BIT 0x01

#define BANKS 8

/* The SNA formats give the interrupt mode a byte and the .z80 two bits,
   room for modes no instruction sets. */
#define INTERRUPT_MODE_REFUSAL "names an interrupt mode the processor has not got"

static uint16_t read16(const uint8_t *bytes, size_t at) {
  return (uint16_t)(bytes[at] | (bytes[at + 1] << 8));
}

static void write16(uint8_t *bytes, size_t at, uint16_t value) {
  bytes[at] = (uint8_t)value;
  bytes[at + 1] = (uint8_t)(value >> 8);
}

/* The 48K format has no bank register and no sound chip, and holds 48K. */
static bool describes_48k(const spectrum_t *spectrum) {
  return spectrum->model == SPECTRUM_48K && spectrum->ram_size == SPECTRUM_RAM_48K;
}

static bool describes_128k(const spectrum_t *spectrum) {
  return spectrum->model == SPECTRUM_PLUS2 && spectrum->ram_size == SPECTRUM_RAM_128K;
}

static uint8_t *bank(const spectrum_t *spectrum, unsigned number) {
  return spectrum->ram + (size_t)number * SPECTRUM_BANK_SIZE;
}

/* The bank paged at 0xC000 is written third even when it is 5 or 2, which
   are written first and second, so that one is written twice. */
static bool holds_a_bank_twice(unsigned paged) { return paged == 5 || paged == 2; }

static size_t size_of_128k(unsigned paged) {
  return holds_a_bank_twice(paged) ? SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE
                                   : SPECTRUM_SNAPSHOT_SNA_128K_SIZE;
}

size_t spectrum_snapshot_sna_size(const spectrum_t *spectrum) {
  if (describes_48k(spectrum)) {
    return SPECTRUM_SNAPSHOT_SNA_48K_SIZE;
  }
  if (describes_128k(spectrum)) {
    return size_of_128k(spectrum->bank_register & SPECTRUM_BANK_PAGE);
  }
  return 0;
}

/* A machine with nothing left in flight — a snapshot is only ever taken
   between instructions with nothing owed — and its beam at `frame_tick`;
   the flashing characters at the start of their count and the monitor
   waiting for the next frame's sync, as on a machine just switched on
   (spectrum_snapshot.h). */
static void begin_frame(spectrum_t *spectrum, uint32_t frame_tick) {
  z80_init(&spectrum->cpu);
  ula_seek(&spectrum->ula, frame_tick);
  spectrum->ula.frame_count = 0;
  spectrum_connect_monitor(spectrum, spectrum->monitor.framebuffer);
  spectrum->held_ticks = 0;
  spectrum->port_charges_left = 0;
  spectrum->reading_port = false;
  spectrum->latching_bank_register = false;
}

/* Every register the header holds but the stack pointer, and the frame
   begun at its top. */
static void read_header(spectrum_t *spectrum, const uint8_t *bytes) {
  z80_t *cpu = &spectrum->cpu;
  begin_frame(spectrum, 0);
  cpu->i = bytes[AT_I];
  cpu->hl_ = read16(bytes, AT_SHADOW_HL);
  cpu->de_ = read16(bytes, AT_SHADOW_DE);
  cpu->bc_ = read16(bytes, AT_SHADOW_BC);
  cpu->af_ = read16(bytes, AT_SHADOW_AF);
  uint16_t hl = read16(bytes, AT_HL);
  uint16_t de = read16(bytes, AT_DE);
  uint16_t bc = read16(bytes, AT_BC);
  uint16_t iy = read16(bytes, AT_IY);
  uint16_t ix = read16(bytes, AT_IX);
  cpu->h = (uint8_t)(hl >> 8);
  cpu->l = (uint8_t)hl;
  cpu->d = (uint8_t)(de >> 8);
  cpu->e = (uint8_t)de;
  cpu->b = (uint8_t)(bc >> 8);
  cpu->c = (uint8_t)bc;
  cpu->iyh = (uint8_t)(iy >> 8);
  cpu->iyl = (uint8_t)iy;
  cpu->ixh = (uint8_t)(ix >> 8);
  cpu->ixl = (uint8_t)ix;
  cpu->r = bytes[AT_R];
  uint16_t af = read16(bytes, AT_AF);
  cpu->a = (uint8_t)(af >> 8);
  cpu->f = (uint8_t)af;
  cpu->im = bytes[AT_INTERRUPT_MODE];

  /* The header carries IFF2 alone. A real restart is a RETN, which copies
     IFF2 into IFF1; doing that here rather than executing one leaves the
     machine where a RETN would have left it, without the two T-states and
     without needing the ROM to hold one. */
  cpu->iff2 = (bytes[AT_INTERRUPT] & IFF2_BIT) != 0;
  cpu->iff1 = cpu->iff2;

  ula_write(&spectrum->ula, bytes[AT_BORDER] & 0x07);
}

static bool load_48k(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                     const char **problem) {
  if (length == SPECTRUM_SNAPSHOT_SNA_128K_SIZE ||
      length == SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE) {
    *problem = "is a 128K snapshot, and this machine is a 48K";
    return false;
  }
  if (length != SPECTRUM_SNAPSHOT_SNA_48K_SIZE) {
    *problem = "is not 49179 bytes, which is the only length a 48K snapshot has";
    return false;
  }
  if (!describes_48k(spectrum)) {
    *problem = "describes a 48K machine, and this one has not 48K fitted";
    return false;
  }
  uint16_t stack = read16(bytes, AT_SP);
  if (stack < SPECTRUM_RAM_BASE || stack == 0xFFFF) {
    *problem = "puts its stack where the program counter cannot have been pushed";
    return false;
  }
  if (bytes[AT_INTERRUPT_MODE] > 2) {
    *problem = INTERRUPT_MODE_REFUSAL;
    return false;
  }

  read_header(spectrum, bytes);
  memcpy(spectrum->ram, bytes + AT_RAM, SPECTRUM_RAM_48K);
  /* And the program counter off the machine's own stack, where writing it
     put it. */
  z80_t *cpu = &spectrum->cpu;
  cpu->pc = (uint16_t)(spectrum_peek(spectrum, stack) |
                       (spectrum_peek(spectrum, (uint16_t)(stack + 1)) << 8));
  cpu->sp = (uint16_t)(stack + 2);
  return true;
}

static bool load_128k(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                      const char **problem) {
  if (length == SPECTRUM_SNAPSHOT_SNA_48K_SIZE) {
    *problem = "is a 48K snapshot, and this machine is a +2";
    return false;
  }
  if (length != SPECTRUM_SNAPSHOT_SNA_128K_SIZE &&
      length != SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE) {
    *problem = "is neither 131103 nor 147487 bytes, the two lengths a 128K snapshot has";
    return false;
  }
  if (!describes_128k(spectrum)) {
    *problem = "describes a 128K machine, and this one has not 128K fitted";
    return false;
  }
  const unsigned paged = bytes[AT_BANK_REGISTER] & SPECTRUM_BANK_PAGE;
  if (length != size_of_128k(paged)) {
    *problem = holds_a_bank_twice(paged)
                   ? "pages in bank 5 or 2, which it must then hold twice, and is a bank short"
                   : "pages in neither bank 5 nor bank 2, and is a bank too long";
    return false;
  }
  if (bytes[AT_TR_DOS] != 0) {
    *problem = "has the TR-DOS ROM paged in, which a +2 has not got";
    return false;
  }
  if (bytes[AT_INTERRUPT_MODE] > 2) {
    *problem = INTERRUPT_MODE_REFUSAL;
    return false;
  }
  /* The format does not say which copy of a bank held twice to believe, and
     a file whose two copies differ contradicts itself. */
  if (holds_a_bank_twice(paged)) {
    size_t first = paged == 5 ? AT_BANK_5 : AT_BANK_2;
    if (memcmp(bytes + first, bytes + AT_PAGED_BANK, SPECTRUM_BANK_SIZE) != 0) {
      *problem = "holds bank 5 or 2 twice, and the two copies differ";
      return false;
    }
  }

  read_header(spectrum, bytes);
  z80_t *cpu = &spectrum->cpu;
  cpu->sp = read16(bytes, AT_SP);
  cpu->pc = read16(bytes, AT_PC);
  /* The register's six bits; a byte's top two reach nothing (spectrum.h). */
  spectrum->bank_register = bytes[AT_BANK_REGISTER] & 0x3F;

  memcpy(bank(spectrum, 5), bytes + AT_BANK_5, SPECTRUM_BANK_SIZE);
  memcpy(bank(spectrum, 2), bytes + AT_BANK_2, SPECTRUM_BANK_SIZE);
  memcpy(bank(spectrum, paged), bytes + AT_PAGED_BANK, SPECTRUM_BANK_SIZE);
  size_t at = AT_REMAINING_BANKS;
  for (unsigned number = 0; number < BANKS; number++) {
    if (number != 5 && number != 2 && number != paged) {
      memcpy(bank(spectrum, number), bytes + at, SPECTRUM_BANK_SIZE);
      at += SPECTRUM_BANK_SIZE;
    }
  }

  /* No register of the sound chip is in the format, so it comes back as
     reset leaves it, with whatever the board presents to its port. */
  uint8_t presented = spectrum->psg.port_a_input;
  ay38910_init(&spectrum->psg);
  ay38910_present_port_a(&spectrum->psg, presented);
  return true;
}

bool spectrum_snapshot_load_sna(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                                const char **problem) {
  return spectrum->model == SPECTRUM_PLUS2 ? load_128k(spectrum, bytes, length, problem)
                                           : load_48k(spectrum, bytes, length, problem);
}

/* The .z80 format's first header, thirty bytes in the order Lunter's manual
   lists them, and the additional header of versions 2 and 3 behind it, at
   the offsets the manual counts from the start of the file. */
#define V1_HEADER_SIZE 30
#define V1_AT_A 0
#define V1_AT_F 1
#define V1_AT_BC 2
#define V1_AT_HL 4
#define V1_AT_PC 6 /* zero in versions 2 and 3, which keep it further on */
#define V1_AT_SP 8
#define V1_AT_I 10
#define V1_AT_R 11
#define V1_AT_FLAGS 12
#define V1_AT_DE 13
#define V1_AT_SHADOW_BC 15
#define V1_AT_SHADOW_DE 17
#define V1_AT_SHADOW_HL 19
#define V1_AT_SHADOW_A 21
#define V1_AT_SHADOW_F 22
#define V1_AT_IY 23
#define V1_AT_IX 25
#define V1_AT_IFF1 27
#define V1_AT_IFF2 28
#define V1_AT_INTERRUPT_MODE 29 /* bits 0 and 1; the rest are the emulator's settings */
#define ADDITIONAL_AT_LENGTH 30
#define ADDITIONAL_AT_PC 32
#define ADDITIONAL_AT_HARDWARE 34
#define ADDITIONAL_AT_BANK_REGISTER 35
#define ADDITIONAL_AT_INTERFACE_1_PAGED 36
#define ADDITIONAL_AT_FLAGS 37
#define ADDITIONAL_AT_PSG_SELECTED 38
#define ADDITIONAL_AT_PSG_REGISTERS 39
#define ADDITIONAL_AT_LOW_T_STATES 55
#define ADDITIONAL_AT_HIGH_T_STATES 57
#define ADDITIONAL_AT_MGT_PAGED 59

#define V1_FLAGS_R_BIT_7 0x01
#define V1_FLAGS_BORDER 0x0E
#define V1_FLAGS_COMPRESSED 0x20
#define ADDITIONAL_FLAGS_MODIFY_HARDWARE 0x80

#define RUN_MARK 0xED
#define RUN_SIZE 4
#define BLOCK_HEADER_SIZE 3
#define RAW_BLOCK_LENGTH 0xFFFF

static const uint8_t end_marker[] = {0x00, 0xED, 0xED, 0x00};

/* The memory a file's hardware byte describes. */
typedef enum { HOLDS_16K, HOLDS_48K, HOLDS_128K } holding;

/* The interface whose ROM a file may have been taken with paged in. */
typedef enum { NO_INTERFACE, INTERFACE_1, MGT_INTERFACE } interface;

#define UNDEFINED_HARDWARE "names hardware the format does not define"

/* The machine a hardware byte names, or a sentence refusing it. From 3 to
   6 the two versions name different machines; from 7 up the numbers are
   other emulators' additions, found in either version. */
static const char *describe_hardware(bool version_2, uint8_t hardware, bool modified,
                                     holding *memory, interface *attached) {
  *attached = NO_INTERFACE;
  switch (hardware) {
    case 0:
      *memory = HOLDS_48K;
      break;
    case 1:
      *memory = HOLDS_48K;
      *attached = INTERFACE_1;
      break;
    case 2:
      return "is a snapshot of a 48K with a SamRam, a board not built here";
    case 3:
      *memory = version_2 ? HOLDS_128K : HOLDS_48K;
      *attached = version_2 ? NO_INTERFACE : MGT_INTERFACE;
      break;
    case 4:
      *memory = HOLDS_128K;
      *attached = version_2 ? INTERFACE_1 : NO_INTERFACE;
      break;
    case 5:
    case 6:
      if (version_2) {
        return UNDEFINED_HARDWARE;
      }
      *memory = HOLDS_128K;
      *attached = hardware == 5 ? INTERFACE_1 : MGT_INTERFACE;
      break;
    case 7:
    case 8:
      return "is a snapshot of a +3 or a +2A, which page their memory as no machine here does";
    case 9:
      return "is a snapshot of a Pentagon, a machine not built here";
    case 10:
      return "is a snapshot of a Scorpion, a machine not built here";
    case 11:
      return "is a snapshot of a Didaktik Kompakt, a machine not built here";
    case 12:
      *memory = HOLDS_128K;
      break;
    case 13:
      return "is a snapshot of a +2A, which pages its memory as no machine here does";
    case 14:
    case 15:
    case 128:
      return "is a snapshot of a Timex, a machine not built here";
    default:
      return UNDEFINED_HARDWARE;
  }
  if (modified && *memory == HOLDS_48K) {
    *memory = HOLDS_16K;
  }
  return NULL;
}

/* Why this machine is not the one the file describes, or NULL when it is. */
static const char *misfit(const spectrum_t *spectrum, holding memory) {
  switch (memory) {
    case HOLDS_128K:
      if (spectrum->model != SPECTRUM_PLUS2) {
        return "is a 128K snapshot, and this machine is a 48K";
      }
      return describes_128k(spectrum)
                 ? NULL
                 : "describes a 128K machine, and this one has not 128K fitted";
    case HOLDS_48K:
      if (spectrum->model == SPECTRUM_PLUS2) {
        return "is a 48K snapshot, and this machine is a +2";
      }
      return describes_48k(spectrum) ? NULL
                                     : "describes a 48K machine, and this one has not 48K fitted";
    case HOLDS_16K:
      if (spectrum->model == SPECTRUM_PLUS2) {
        return "is a 16K snapshot, and this machine is a +2";
      }
      return spectrum->ram_size == SPECTRUM_RAM_16K
                 ? NULL
                 : "describes a 16K machine, and this one has more than 16K fitted";
  }
  return NULL;
}

/* The pages each holding is written with, as bits: a 48K's 4, 5 and 8, its
   RAM from 0x8000, 0xC000 and 0x4000, and a 128K's 3 to 10, its banks 0 to
   7. A 16K is written as a 48K, of whose pages only 8 is its RAM. */
#define PAGES_48K ((1U << 4) | (1U << 5) | (1U << 8))
#define PAGES_128K (0xFFU << 3)
#define LAST_PAGE 10

static unsigned pages_written(holding memory) {
  return memory == HOLDS_128K ? PAGES_128K : PAGES_48K;
}

static unsigned pages_needed(holding memory) {
  return memory == HOLDS_16K ? 1U << 8 : pages_written(memory);
}

/* Where a page goes in the machine's RAM, or NULL for one it has no RAM
   behind. */
static uint8_t *page_in_ram(const spectrum_t *spectrum, holding memory, unsigned page) {
  if (memory == HOLDS_128K) {
    return bank(spectrum, page - 3);
  }
  switch (page) {
    case 8:
      return spectrum->ram;
    case 4:
      return memory == HOLDS_48K ? spectrum->ram + SPECTRUM_BANK_SIZE : NULL;
    case 5:
      return memory == HOLDS_48K ? spectrum->ram + (size_t)2 * SPECTRUM_BANK_SIZE : NULL;
    default:
      return NULL;
  }
}

/* Lunter's compression: ED ED, a count and a byte stand for that many of
   the byte, and every other byte for itself. With `out` NULL, says whether
   `length` bytes expand to exactly `room`; given `out`, expands them there,
   and is only ever given bytes that were first found to fill it exactly. */
static bool expand(const uint8_t *in, size_t length, uint8_t *out, size_t room) {
  size_t consumed = 0;
  size_t produced = 0;
  while (consumed < length) {
    if (in[consumed] == RUN_MARK && length - consumed >= 2 && in[consumed + 1] == RUN_MARK) {
      if (length - consumed < RUN_SIZE) {
        return false;
      }
      if (out != NULL) {
        memset(out + produced, in[consumed + 3], in[consumed + 2]);
      }
      produced += in[consumed + 2];
      consumed += RUN_SIZE;
    } else {
      if (out != NULL) {
        out[produced] = in[consumed];
      }
      produced++;
      consumed++;
    }
  }
  return produced == room;
}

/* One block of a version 2 or 3 file: the page it holds and its bytes as
   the file stores them. */
typedef struct {
  unsigned page;
  const uint8_t *stored;
  size_t length;
  bool raw;
} block;

/* The block at `*at`, which is moved past it, or a sentence refusing it. A
   length of 0xFFFF marks a page stored raw; one of exactly 16384 is raw
   only when it does not expand to one page (spectrum_snapshot.h). */
static const char *take_block(const uint8_t *bytes, size_t length, size_t *at, block *taken) {
  if (length - *at < BLOCK_HEADER_SIZE) {
    return "ends inside the header of a block";
  }
  uint16_t stored_length = read16(bytes, *at);
  taken->page = bytes[*at + 2];
  taken->stored = bytes + *at + BLOCK_HEADER_SIZE;
  taken->length = stored_length == RAW_BLOCK_LENGTH ? SPECTRUM_BANK_SIZE : stored_length;
  if (length - *at - BLOCK_HEADER_SIZE < taken->length) {
    return "ends inside a block";
  }
  taken->raw = stored_length == RAW_BLOCK_LENGTH ||
               (stored_length == SPECTRUM_BANK_SIZE &&
                !expand(taken->stored, taken->length, NULL, SPECTRUM_BANK_SIZE));
  if (!taken->raw && !expand(taken->stored, taken->length, NULL, SPECTRUM_BANK_SIZE)) {
    return "holds a block that does not expand to exactly one 16K page";
  }
  *at += BLOCK_HEADER_SIZE + taken->length;
  return NULL;
}

/* Every block from `at` to the end of the file: each page the holding is
   written with once, none it is not, and every one it needs. Given a
   machine, it also writes each page into the RAM behind it, and is given
   one only once the blocks have been walked without and found sound. */
static const char *walk_blocks(const uint8_t *bytes, size_t length, size_t at, holding memory,
                               spectrum_t *spectrum) {
  unsigned seen = 0;
  while (at < length) {
    block taken;
    const char *problem = take_block(bytes, length, &at, &taken);
    if (problem != NULL) {
      return problem;
    }
    if (taken.page > LAST_PAGE || (pages_written(memory) & (1U << taken.page)) == 0) {
      return "holds a page the machine it describes is not written with";
    }
    if ((seen & (1U << taken.page)) != 0) {
      return "holds the same page twice";
    }
    seen |= 1U << taken.page;
    uint8_t *destination = spectrum == NULL ? NULL : page_in_ram(spectrum, memory, taken.page);
    if (destination != NULL && taken.raw) {
      memcpy(destination, taken.stored, SPECTRUM_BANK_SIZE);
    } else if (destination != NULL) {
      expand(taken.stored, taken.length, destination, SPECTRUM_BANK_SIZE);
    }
  }
  if ((seen & pages_needed(memory)) != pages_needed(memory)) {
    return "lacks a page of the machine's memory";
  }
  return NULL;
}

/* Where version 3's T-state counter puts the beam, or the top of the frame
   when the counter is outside the range it counts in. The low count runs
   down through each quarter of the frame and the high one counts the
   quarters from 3 just after the interrupt; the T-state they name is one
   less than the frame tick here (spectrum_snapshot.h). */
static uint32_t counted_frame_tick(const spectrum_t *spectrum, const uint8_t *bytes) {
  uint32_t frame = spectrum->ula.frame.ticks_per_frame;
  uint32_t quarter = frame / 4;
  uint32_t low = read16(bytes, ADDITIONAL_AT_LOW_T_STATES);
  uint32_t high = bytes[ADDITIONAL_AT_HIGH_T_STATES];
  if (high > 3 || low >= quarter) {
    return 0;
  }
  uint32_t counted = (high + 1) % 4 * quarter + (quarter - 1 - low);
  return (counted + 1) % frame;
}

/* Every register the first header holds, the border with them. */
static void read_v1_registers(spectrum_t *spectrum, const uint8_t *bytes, uint8_t flags) {
  z80_t *cpu = &spectrum->cpu;
  uint16_t bc = read16(bytes, V1_AT_BC);
  uint16_t de = read16(bytes, V1_AT_DE);
  uint16_t hl = read16(bytes, V1_AT_HL);
  uint16_t iy = read16(bytes, V1_AT_IY);
  uint16_t ix = read16(bytes, V1_AT_IX);
  cpu->a = bytes[V1_AT_A];
  cpu->f = bytes[V1_AT_F];
  cpu->b = (uint8_t)(bc >> 8);
  cpu->c = (uint8_t)bc;
  cpu->d = (uint8_t)(de >> 8);
  cpu->e = (uint8_t)de;
  cpu->h = (uint8_t)(hl >> 8);
  cpu->l = (uint8_t)hl;
  cpu->iyh = (uint8_t)(iy >> 8);
  cpu->iyl = (uint8_t)iy;
  cpu->ixh = (uint8_t)(ix >> 8);
  cpu->ixl = (uint8_t)ix;
  cpu->af_ = (uint16_t)((bytes[V1_AT_SHADOW_A] << 8) | bytes[V1_AT_SHADOW_F]);
  cpu->bc_ = read16(bytes, V1_AT_SHADOW_BC);
  cpu->de_ = read16(bytes, V1_AT_SHADOW_DE);
  cpu->hl_ = read16(bytes, V1_AT_SHADOW_HL);
  cpu->sp = read16(bytes, V1_AT_SP);
  cpu->i = bytes[V1_AT_I];
  cpu->r = (uint8_t)((bytes[V1_AT_R] & 0x7F) | ((flags & V1_FLAGS_R_BIT_7) << 7));
  cpu->iff1 = bytes[V1_AT_IFF1] != 0;
  cpu->iff2 = bytes[V1_AT_IFF2] != 0;
  cpu->im = bytes[V1_AT_INTERRUPT_MODE] & 0x03;
  ula_write(&spectrum->ula, (uint8_t)((flags & V1_FLAGS_BORDER) >> 1));
}

/* Through the chip's own bus, as a CPC's snapshot restores its chip
   (cpc_snapshot.c): the registers written, the envelope settled as
   ay38910.h settles one, and the address the file names written last,
   which leaves the chip unselected where it was saved so. A CPC's format
   keeps the chip's latch, and sends its low four bits first; this byte is
   the last address the program wrote, which a chip it deselected never
   latched, so it is sent once, as it was. */
static void restore_psg(spectrum_t *spectrum, const uint8_t *bytes, unsigned registers) {
  uint8_t presented = spectrum->psg.port_a_input;
  ay38910_init(&spectrum->psg);
  ay38910_present_port_a(&spectrum->psg, presented);
  for (unsigned index = 0; index < registers; index++) {
    ay38910_access(&spectrum->psg, AY38910_SELECT, (uint8_t)index);
    ay38910_access(&spectrum->psg, AY38910_WRITE, bytes[ADDITIONAL_AT_PSG_REGISTERS + index]);
  }
  ay38910_settle_envelope(&spectrum->psg);
  ay38910_access(&spectrum->psg, AY38910_SELECT, bytes[ADDITIONAL_AT_PSG_SELECTED]);
}

static bool load_version_1(spectrum_t *spectrum, const uint8_t *bytes, size_t length, uint8_t flags,
                           const char **problem) {
  const char *refusal = misfit(spectrum, HOLDS_48K);
  if (refusal != NULL) {
    *problem = refusal;
    return false;
  }
  const uint8_t *memory = bytes + V1_HEADER_SIZE;
  size_t stored = length - V1_HEADER_SIZE;
  bool compressed = (flags & V1_FLAGS_COMPRESSED) != 0;
  if (compressed) {
    if (stored < sizeof end_marker ||
        memcmp(memory + stored - sizeof end_marker, end_marker, sizeof end_marker) != 0) {
      *problem = "is compressed and does not end with the end marker 00 ED ED 00";
      return false;
    }
    stored -= sizeof end_marker;
    if (!expand(memory, stored, NULL, SPECTRUM_RAM_48K)) {
      *problem = "does not expand to exactly 48K before its end marker";
      return false;
    }
  } else if (stored != SPECTRUM_RAM_48K) {
    *problem = "is not compressed and does not hold exactly 48K";
    return false;
  }

  begin_frame(spectrum, 0);
  read_v1_registers(spectrum, bytes, flags);
  spectrum->cpu.pc = read16(bytes, V1_AT_PC);
  if (compressed) {
    expand(memory, stored, spectrum->ram, SPECTRUM_RAM_48K);
  } else {
    memcpy(spectrum->ram, memory, SPECTRUM_RAM_48K);
  }
  return true;
}

static bool load_versions_2_and_3(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                                  uint8_t flags, const char **problem) {
  if (length < ADDITIONAL_AT_PC) {
    *problem = "ends inside its additional header";
    return false;
  }
  /* Its length is the version: 23 for the second, or 21, which Lunter's
     z80dump also takes for the second, with fourteen of the sound chip's
     registers; 54 for the third, or 55 with the +3's paging port. */
  uint16_t additional = read16(bytes, ADDITIONAL_AT_LENGTH);
  bool version_2 = additional == 21 || additional == 23;
  if (!version_2 && additional != 54 && additional != 55) {
    *problem = "has an additional header of a length the format does not define";
    return false;
  }
  size_t blocks_at = ADDITIONAL_AT_PC + (size_t)additional;
  if (length < blocks_at) {
    *problem = "ends inside its additional header";
    return false;
  }
  holding memory;
  interface attached;
  const char *refusal = describe_hardware(
      version_2, bytes[ADDITIONAL_AT_HARDWARE],
      (bytes[ADDITIONAL_AT_FLAGS] & ADDITIONAL_FLAGS_MODIFY_HARDWARE) != 0, &memory, &attached);
  if (refusal == NULL) {
    refusal = misfit(spectrum, memory);
  }
  if (refusal == NULL && attached == INTERFACE_1 && bytes[ADDITIONAL_AT_INTERFACE_1_PAGED] != 0) {
    refusal = "has the Interface 1's ROM paged in, and no interface is built here";
  }
  if (refusal == NULL && attached == MGT_INTERFACE && bytes[ADDITIONAL_AT_MGT_PAGED] != 0) {
    refusal = "has the MGT disc interface's ROM paged in, and no interface is built here";
  }
  if (refusal == NULL) {
    refusal = walk_blocks(bytes, length, blocks_at, memory, NULL);
  }
  if (refusal != NULL) {
    *problem = refusal;
    return false;
  }

  begin_frame(spectrum, version_2 ? 0 : counted_frame_tick(spectrum, bytes));
  read_v1_registers(spectrum, bytes, flags);
  spectrum->cpu.pc = read16(bytes, ADDITIONAL_AT_PC);
  walk_blocks(bytes, length, blocks_at, memory, spectrum);
  if (memory == HOLDS_128K) {
    /* The register's six bits; a byte's top two reach nothing (spectrum.h). */
    spectrum->bank_register = bytes[ADDITIONAL_AT_BANK_REGISTER] & 0x3F;
    restore_psg(spectrum, bytes, additional == 21 ? 14 : 16);
  }
  return true;
}

bool spectrum_snapshot_load_z80(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                                const char **problem) {
  if (length < V1_HEADER_SIZE) {
    *problem = "is shorter than the format's thirty-byte header";
    return false;
  }
  if ((bytes[V1_AT_INTERRUPT_MODE] & 0x03) == 3) {
    *problem = INTERRUPT_MODE_REFUSAL;
    return false;
  }
  /* "Because of compatibility, if byte 12 is 255, it has to be regarded as
     being 1" (the manual). */
  uint8_t flags = bytes[V1_AT_FLAGS] == 0xFF ? 0x01 : bytes[V1_AT_FLAGS];
  return read16(bytes, V1_AT_PC) != 0
             ? load_version_1(spectrum, bytes, length, flags, problem)
             : load_versions_2_and_3(spectrum, bytes, length, flags, problem);
}

/* Every register the header holds, with the stack pointer given. */
static void write_header(const spectrum_t *spectrum, uint8_t *bytes, uint16_t stack) {
  const z80_t *cpu = &spectrum->cpu;
  bytes[AT_I] = cpu->i;
  write16(bytes, AT_SHADOW_HL, cpu->hl_);
  write16(bytes, AT_SHADOW_DE, cpu->de_);
  write16(bytes, AT_SHADOW_BC, cpu->bc_);
  write16(bytes, AT_SHADOW_AF, cpu->af_);
  write16(bytes, AT_HL, (uint16_t)((cpu->h << 8) | cpu->l));
  write16(bytes, AT_DE, (uint16_t)((cpu->d << 8) | cpu->e));
  write16(bytes, AT_BC, (uint16_t)((cpu->b << 8) | cpu->c));
  write16(bytes, AT_IY, (uint16_t)((cpu->iyh << 8) | cpu->iyl));
  write16(bytes, AT_IX, (uint16_t)((cpu->ixh << 8) | cpu->ixl));
  bytes[AT_INTERRUPT] = cpu->iff2 ? (IFF2_BIT | MAME_IFF1_BIT) : 0x00;
  bytes[AT_R] = cpu->r;
  write16(bytes, AT_AF, (uint16_t)((cpu->a << 8) | cpu->f));
  write16(bytes, AT_SP, stack);
  bytes[AT_INTERRUPT_MODE] = cpu->im;
  bytes[AT_BORDER] = spectrum->ula.border;
}

static bool save_48k(const spectrum_t *spectrum, uint8_t *bytes, const char **problem) {
  const z80_t *cpu = &spectrum->cpu;
  /* The format has nowhere for the program counter, so it is written where a
     PUSH would have put it and the two bytes that were there are lost. They
     are lost in the snapshot and not in the machine: a running machine is
     not damaged by being written down. */
  uint16_t stack = (uint16_t)(cpu->sp - 2);
  if (stack < SPECTRUM_RAM_BASE || stack == 0xFFFF) {
    *problem = "has its stack where the program counter cannot be pushed whole into the RAM";
    return false;
  }
  memset(bytes, 0, SPECTRUM_SNAPSHOT_SNA_48K_SIZE);
  write_header(spectrum, bytes, stack);
  memcpy(bytes + AT_RAM, spectrum->ram, SPECTRUM_RAM_48K);
  write16(bytes, AT_RAM + (size_t)(stack - SPECTRUM_RAM_BASE), z80_resume_address(cpu));
  return true;
}

static void save_128k(const spectrum_t *spectrum, uint8_t *bytes) {
  const z80_t *cpu = &spectrum->cpu;
  const unsigned paged = spectrum->bank_register & SPECTRUM_BANK_PAGE;
  memset(bytes, 0, size_of_128k(paged));
  write_header(spectrum, bytes, cpu->sp);
  memcpy(bytes + AT_BANK_5, bank(spectrum, 5), SPECTRUM_BANK_SIZE);
  memcpy(bytes + AT_BANK_2, bank(spectrum, 2), SPECTRUM_BANK_SIZE);
  memcpy(bytes + AT_PAGED_BANK, bank(spectrum, paged), SPECTRUM_BANK_SIZE);
  write16(bytes, AT_PC, z80_resume_address(cpu));
  bytes[AT_BANK_REGISTER] = spectrum->bank_register;
  bytes[AT_TR_DOS] = 0;
  size_t at = AT_REMAINING_BANKS;
  for (unsigned number = 0; number < BANKS; number++) {
    if (number != 5 && number != 2 && number != paged) {
      memcpy(bytes + at, bank(spectrum, number), SPECTRUM_BANK_SIZE);
      at += SPECTRUM_BANK_SIZE;
    }
  }
}

bool spectrum_snapshot_save_sna(const spectrum_t *spectrum, uint8_t *bytes, size_t capacity,
                                const char **problem) {
  size_t size = spectrum_snapshot_sna_size(spectrum);
  if (size == 0) {
    *problem = "fits neither format: a 48K must have 48K fitted, and a +2 128K";
    return false;
  }
  if (capacity < size) {
    *problem = "needs more room to be written into than the caller gave";
    return false;
  }
  if (!spectrum_instruction_complete(spectrum)) {
    *problem = z80_instruction_complete(&spectrum->cpu)
                   ? "would lose the T-states the ULA is still holding the clock for"
                   : "cannot be taken mid-instruction";
    return false;
  }
  if (spectrum->model == SPECTRUM_PLUS2) {
    save_128k(spectrum, bytes);
    return true;
  }
  return save_48k(spectrum, bytes, problem);
}
