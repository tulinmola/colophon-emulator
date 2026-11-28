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
  return holds_a_bank_twice(paged) ? SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE
                                   : SPECTRUM_SNAPSHOT_128K_SIZE;
}

size_t spectrum_snapshot_size(const spectrum_t *spectrum) {
  if (describes_48k(spectrum)) {
    return SPECTRUM_SNAPSHOT_48K_SIZE;
  }
  if (describes_128k(spectrum)) {
    return size_of_128k(spectrum->bank_register & SPECTRUM_BANK_PAGE);
  }
  return 0;
}

/* Every register the header holds but the stack pointer, with nothing left
   in flight — a snapshot is only ever taken between instructions with
   nothing owed — and the frame begun as on a machine just switched on
   (spectrum_snapshot.h). */
static void read_header(spectrum_t *spectrum, const uint8_t *bytes) {
  z80_t *cpu = &spectrum->cpu;
  z80_init(cpu);
  ula_seek(&spectrum->ula, 0);
  spectrum->ula.frame_count = 0;
  spectrum_connect_monitor(spectrum, spectrum->monitor.framebuffer);
  spectrum->held_ticks = 0;
  spectrum->port_charges_left = 0;
  spectrum->reading_port = false;
  spectrum->latching_bank_register = false;
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
  cpu->im = bytes[AT_INTERRUPT_MODE] & 0x03;

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
  if (length == SPECTRUM_SNAPSHOT_128K_SIZE || length == SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE) {
    *problem = "is a 128K snapshot, and this machine is a 48K";
    return false;
  }
  if (length != SPECTRUM_SNAPSHOT_48K_SIZE) {
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
  if (length == SPECTRUM_SNAPSHOT_48K_SIZE) {
    *problem = "is a 48K snapshot, and this machine is a +2";
    return false;
  }
  if (length != SPECTRUM_SNAPSHOT_128K_SIZE && length != SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE) {
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

bool spectrum_snapshot_load(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                            const char **problem) {
  return spectrum->model == SPECTRUM_PLUS2 ? load_128k(spectrum, bytes, length, problem)
                                           : load_48k(spectrum, bytes, length, problem);
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
  memset(bytes, 0, SPECTRUM_SNAPSHOT_48K_SIZE);
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

bool spectrum_snapshot_save(const spectrum_t *spectrum, uint8_t *bytes, size_t capacity,
                            const char **problem) {
  size_t size = spectrum_snapshot_size(spectrum);
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
