/*
 * spectrum.c — the machine wiring: the memory map, the I/O decode, and the
 * clock the ULA stops.
 */
#include "spectrum.h"

#include <stddef.h>
#include <string.h>

/* What the data bus reads where nothing drives it: the board pulls it up,
   through the 8K2 resistors Chris Smith found on Sinclair's schematics
   ("Clock Alignment and Stability"). */
#define IDLE_BUS 0xFF

/* Where the processor's address lands in the RAM, at or above
   SPECTRUM_RAM_BASE. A 48K's RAM is one run from there; a +2 shows bank 5
   at 0x4000, bank 2 at 0x8000 and at 0xC000 the bank its register names
   (the 128 service manual, §4). */
static uint32_t ram_offset(const spectrum_t *spectrum, uint16_t address) {
  if (spectrum->model == SPECTRUM_48K) {
    return (uint32_t)address - SPECTRUM_RAM_BASE;
  }
  static const uint8_t fixed_bank[] = {0, 5, 2};
  unsigned window = address / SPECTRUM_BANK_SIZE;
  unsigned bank = window == 3 ? spectrum->bank_register & SPECTRUM_BANK_PAGE : fixed_bank[window];
  return bank * SPECTRUM_BANK_SIZE + address % SPECTRUM_BANK_SIZE;
}

/* Bit 4 of the bank register is the ROM chip's A14, so on a 48K, whose
   register stays clear, this is the address itself. */
static uint32_t rom_offset(const spectrum_t *spectrum, uint16_t address) {
  return (spectrum->bank_register & SPECTRUM_BANK_ROM) ? address + 0x4000u : address;
}

static uint8_t read_memory(const spectrum_t *spectrum, uint16_t address) {
  if (address < SPECTRUM_RAM_BASE) {
    return spectrum->rom[rom_offset(spectrum, address)];
  }
  uint32_t offset = ram_offset(spectrum, address);
  /* Nothing answers above the RAM that is fitted. No measurement of what a
     16K reads there was found; the Sinclair Wiki expects the floating bus ("The
     same effect is likely to be seen when reading unattached memory, such
     as reading the upper 32K on a 16K machine"), and the idle bus is
     assumed. */
  return offset < spectrum->ram_size ? spectrum->ram[offset] : IDLE_BUS;
}

/* A write to the ROM lands nowhere: there is no RAM beneath it to fall
   through to, as there is on a CPC. */
static void write_memory(spectrum_t *spectrum, uint16_t address, uint8_t value) {
  if (address < SPECTRUM_RAM_BASE) {
    return;
  }
  uint32_t offset = ram_offset(spectrum, address);
  if (offset < spectrum->ram_size) {
    spectrum->ram[offset] = value;
  }
}

/* The ULA's address lines reach one bank of sixteen kilobytes and no
   further, whatever the processor is looking at, so this bound is the
   chip's and not a guess about what it will ask for. On a 48K that bank is
   the first of its RAM, and on a +2 bank 5, or bank 7 while bit 3 of the
   bank register is set. */
static uint8_t read_video(const spectrum_t *spectrum, uint16_t address) {
  uint32_t offset = (uint32_t)address - SPECTRUM_RAM_BASE;
  if (offset >= SPECTRUM_BANK_SIZE) {
    return IDLE_BUS;
  }
  if (spectrum->model == SPECTRUM_PLUS2) {
    unsigned bank = (spectrum->bank_register & SPECTRUM_BANK_SCREEN) ? 7 : 5;
    offset += bank * SPECTRUM_BANK_SIZE;
  }
  return offset < spectrum->ram_size ? spectrum->ram[offset] : IDLE_BUS;
}

/* Every zero in the top half of the address selects a half-row, and the
   selected ones are wire-ANDed together — so an address with several bits
   low reads several half-rows at once, and one with none reads no keys.
   Bits 5 and 7 carry nothing and read high on every 48K board: "Bits 5 and
   7 as read by INning from Port 0xfe are always one" (World of Spectrum, 48K
   reference). The Sinclair Wiki's "Keyboard" page says instead that the upper
   two bits read 1 on an issue 2 and 0 on later ones; that is the issue
   difference misattributed, because what the issues differ over is bit 6 —
   how much of a write to port &FE comes back on a read — and we follow the
   reference. A +2 is taken to read the same, for want of a source on one.
   Bit 6 is the EAR socket, which is spectrum_t's `ear` here and hears
   nothing this machine writes. */
static uint8_t read_keyboard(const spectrum_t *spectrum, uint16_t port) {
  uint8_t keys = 0x1F;
  for (uint8_t half_row = 0; half_row < SPECTRUM_HALF_ROWS; half_row++) {
    if ((port & (0x0100u << half_row)) == 0) {
      keys &= keyboard_line(&spectrum->keyboard, half_row);
    }
  }
  bool ear = spectrum->tape != NULL ? tape_level(spectrum->tape) : spectrum->ear;
  return (uint8_t)(keys | 0xA0 | (ear ? 0x40 : 0x00));
}

/* The ULA answers every port with A0 low, on both models. */
static bool ula_answers(uint16_t port) { return (port & 0x0001) == 0; }

/* The +2's paging logic decodes its two ports from A15 and A1 alone, on a
   read or a write: BANK with both low and PSG with A15 high (JL's
   equations). */
static bool bank_register_answers(const spectrum_t *spectrum, uint16_t port) {
  return spectrum->model == SPECTRUM_PLUS2 && (port & 0x8002) == 0x0000;
}

static bool psg_answers(const spectrum_t *spectrum, uint16_t port) {
  return spectrum->model == SPECTRUM_PLUS2 && (port & 0x8002) == 0x8000;
}

/* BDIR is a write and BC1 is A14 (the 128 service manual, §5.6.2), with BC2
   tied high (Worsley's +2 redraw). The manual's table has no row for a read
   with A14 low, which leaves it no function at all. */
static ay38910_function psg_function(uint16_t port, bool writing) {
  bool bc1 = (port & 0x4000) != 0;
  if (writing) {
    return bc1 ? AY38910_SELECT : AY38910_WRITE;
  }
  return bc1 ? AY38910_READ : AY38910_INACTIVE;
}

/* A port nothing answers is read through the resistors between the two
   halves of the data bus, so it gives whatever is on the ULA's half. The
   sound chip is on the processor's half (Worsley's +2 redraw), so where it
   and the ULA both answer, the processor reads the chip; that is the
   drawing's reading, and nothing found measures it. */
static uint8_t io_read(spectrum_t *spectrum, uint16_t port) {
  uint8_t data = ula_answers(port) ? read_keyboard(spectrum, port) : spectrum->ula_bus;
  if (psg_answers(spectrum, port)) {
    data = ay38910_access(&spectrum->psg, psg_function(port, false), data);
  }
  return data;
}

static void io_write(spectrum_t *spectrum, uint16_t port, uint8_t data) {
  if (ula_answers(port)) {
    ula_write(&spectrum->ula, data);
  }
  if (psg_answers(spectrum, port)) {
    ay38910_access(&spectrum->psg, psg_function(port, true), data);
  }
}

/* The register's clock is held high by its own bit 5 once that is set
   (Worsley's +2 redraw), so it takes nothing more until reset. */
static void latch_bank_register(spectrum_t *spectrum, uint8_t data) {
  if ((spectrum->bank_register & SPECTRUM_BANK_LOCK) == 0) {
    spectrum->bank_register = data & 0x3F;
  }
}

/* Each model's ULA and clock. The 48K's ULA is early, the timing the
   published tables are for: "Some machines use the timings given in this
   FAQ, while others are one T state later for all timings" (the FAQ's 48K
   reference, cited in ula.h). The +2's is late, as the grey +2s that pass
   Butler's tests run (ula.h). */
static const struct {
  ula_type ula;
  uint32_t clock_hertz;
} models[] = {
    [SPECTRUM_48K] = {ULA_5C_6C_EARLY, SPECTRUM_48K_CLOCK_HERTZ},
    [SPECTRUM_PLUS2] = {ULA_7K010_LATE, SPECTRUM_PLUS2_CLOCK_HERTZ},
};

void spectrum_init(spectrum_t *spectrum, spectrum_model model, uint8_t *ram, uint32_t ram_size,
                   const uint8_t *rom) {
  memset(spectrum, 0, sizeof *spectrum);
  spectrum->model = model;
  z80_init(&spectrum->cpu);
  ula_init(&spectrum->ula, models[model].ula);
  keyboard_init(&spectrum->keyboard);
  ay38910_init(&spectrum->psg);
  /* Nothing is plugged into the keypad or the RS232 socket, and port A is
     taken to read high on every line: the chip pulls each one up (GI's data
     manual, p. 10), and what the board's line receivers make of an empty
     socket was not found. The ROM then finds no keypad. */
  ay38910_present_port_a(&spectrum->psg, 0xFF);
  spectrum->ram = ram;
  spectrum->ram_size = ram_size;
  spectrum->rom = rom;
}

uint32_t spectrum_clock_hertz(const spectrum_t *spectrum) {
  return models[spectrum->model].clock_hertz;
}

void spectrum_connect_monitor(spectrum_t *spectrum, uint8_t *framebuffer) {
  const ula_frame *frame = &spectrum->ula.frame;
  uint16_t width = (uint16_t)(frame->ticks_per_line * ULA_SAMPLES_PER_TICK);
  /* Half of the line's own sync, which is where the middle of that pulse
     lands once the beam is timed from it. */
  uint16_t line_sync_centre = (uint16_t)(frame->retrace_ticks * ULA_SAMPLES_PER_TICK / 2);
  /* A sync held this long means the frame is over. No measurement of what a
     television needs was in hand, so this is chosen rather than found: one
     line's worth of samples, comfortably longer than a line's own sync and
     far shorter than the eight lines the frame's runs to. */
  uint16_t frame_sync_samples = width;
  monitor_init(&spectrum->monitor, framebuffer, width, frame->lines_per_frame, frame_sync_samples,
               line_sync_centre);
}

/* Whether the ULA wants the bus for an address, which it sees by its top
   two lines alone and wants when they read 01. On a 48K they are the
   processor's A15 and A14. On a +2 the paging logic drives them, and shows
   the ULA an odd bank at 0xC000 as 01 (JL's equations: ULA15 = A15·/A14 +
   A15·A14·/B0, ULA14 = /A15·A14 + A15·A14·B0, B0 the register's bit 0); a
   48K's register stays clear, so the one rule serves both. */
static bool in_contended_memory(const spectrum_t *spectrum, uint16_t address) {
  unsigned window = address / SPECTRUM_BANK_SIZE;
  return window == 1 || (window == 3 && (spectrum->bank_register & 0x01) != 0);
}

/* Which of a port access's four T-states are charged, one bit each, first
   one lowest ("Contended I/O"). In the page's notation a charge is weighed
   where its T-state would begin and comes before it, so a T-state that is
   charged runs where the chip owes the processor nothing. */
static uint8_t port_charges(const spectrum_t *spectrum, uint16_t port) {
  const bool looks_contended = in_contended_memory(spectrum, port);
  const bool ula_port = ula_answers(port);
  const bool charged[] = {looks_contended, looks_contended || ula_port,
                          looks_contended && !ula_port, looks_contended && !ula_port};
  uint8_t rows = 0;
  for (size_t tstate = 0; tstate < sizeof charged / sizeof charged[0]; tstate++) {
    rows = (uint8_t)(rows | (charged[tstate] ? 1u << tstate : 0u));
  }
  return rows;
}

/* The beam's own work, which the processor's clock has no bearing on. */
static void paint(spectrum_t *spectrum) {
  uint16_t address = 0;
  spectrum->ula_bus = IDLE_BUS;
  if (ula_fetch(&spectrum->ula, &address)) {
    spectrum->ula_bus = read_video(spectrum, address);
    ula_latch(&spectrum->ula, spectrum->ula_bus);
  }
  uint8_t samples[ULA_SAMPLES_PER_TICK];
  ula_video(&spectrum->ula, samples);
  monitor_receive(&spectrum->monitor, samples, ULA_SAMPLES_PER_TICK, ula_csync(&spectrum->ula));
}

static void run_processor(spectrum_t *spectrum) {
  /* The ULA's interrupt line runs to the processor, and is the only thing
     that ever interrupts it. */
  uint64_t bus = spectrum->pins;
  if (ula_interrupt(&spectrum->ula)) {
    bus |= Z80_INT;
  } else {
    bus &= ~Z80_INT;
  }

  /* The processor takes a port's byte on the read's last T-state, the one
     after the request, and the ULA may have held the clock between the two,
     so the byte it takes is the one on the bus here. */
  if (spectrum->reading_port) {
    bus = z80_set_data(bus, io_read(spectrum, z80_address(bus)));
    spectrum->reading_port = false;
  }
  /* That T-state is the access's last, and the bank register takes the
     byte as it ends, from whichever side drove it: the ULA's fetch on this
     T-state has already gone to the old screen, and its next goes to the
     new. */
  if (spectrum->latching_bank_register) {
    latch_bank_register(spectrum, z80_data(bus));
    spectrum->latching_bank_register = false;
  }

  z80_cycle cycle = z80_next_cycle(&spectrum->cpu);
  uint64_t pins = z80_tick(&spectrum->cpu, bus);
  if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
    /* Interrupt acknowledge. Nothing answers it, so the byte is what the
       ULA's half of the bus holds, which is idle above the picture, where
       an acknowledge falls; in mode 1, which is the only mode the firmware
       uses, it is ignored anyway. */
    pins = z80_set_data(pins, spectrum->ula_bus);
  } else if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
    pins = z80_set_data(pins, read_memory(spectrum, z80_address(pins)));
  } else if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
    write_memory(spectrum, z80_address(pins), z80_data(pins));
  } else if ((pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
    io_write(spectrum, z80_address(pins), z80_data(pins));
    spectrum->latching_bank_register = bank_register_answers(spectrum, z80_address(pins));
  } else if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
    /* The port's byte is on the bus from the request on, and is taken on
       the next T-state, where it is answered again. */
    pins = z80_set_data(pins, io_read(spectrum, z80_address(pins)));
    spectrum->reading_port = true;
    spectrum->latching_bank_register = bank_register_answers(spectrum, z80_address(pins));
  }

  spectrum->pins = pins;

  /* The address is on the bus now and the beam has not moved, so this is
     both the address the ULA weighs and the position it weighs it at. */
  switch (cycle) {
    /* These ULAs weigh the address and not the request, so a T-state that
       only holds one costs what one that acts on it costs. */
    case Z80_CYCLE_MEMORY:
    case Z80_CYCLE_INTERNAL:
      spectrum->held_ticks = in_contended_memory(spectrum, z80_address(pins))
                                 ? ula_contention(&spectrum->ula, spectrum->ula.frame_tick)
                                 : 0;
      break;
    /* The first T-state's charge is taken after it, at the position it
       ran at: nothing happens on it that a hold either side could move. */
    case Z80_CYCLE_PORT:
      spectrum->port_charges_left = port_charges(spectrum, z80_address(pins));
      spectrum->held_ticks = (spectrum->port_charges_left & 1)
                                 ? ula_contention(&spectrum->ula, spectrum->ula.frame_tick)
                                 : 0;
      spectrum->port_charges_left = (uint8_t)(spectrum->port_charges_left >> 1);
      break;
    /* No evidence here can say whether an acknowledge is charged: the
       interrupt is held across a frame's first few dozen T-states and the
       chip wants the bus only from the picture's first line, so an
       acknowledge always falls in the frame sync above the picture, where
       nothing is owed under any rule. */
    case Z80_CYCLE_INTERRUPT:
    case Z80_CYCLE_NONE:
      break;
  }

  /* The next T-state of a port access, weighed where it would begin: the
     third, which gives or asks for the byte, and the fourth, which takes it,
     both come after their holds. */
  if (spectrum->port_charges_left & 1) {
    uint32_t next = spectrum->ula.frame_tick + 1 + spectrum->held_ticks;
    spectrum->held_ticks = (uint8_t)(spectrum->held_ticks + ula_contention(&spectrum->ula, next));
  }
  spectrum->port_charges_left = (uint8_t)(spectrum->port_charges_left >> 1);
}

void spectrum_insert_tape(spectrum_t *spectrum, tape_t *tape) { spectrum->tape = tape; }

bool spectrum_in_48k_mode(const spectrum_t *spectrum) {
  return spectrum->model == SPECTRUM_48K || (spectrum->bank_register & SPECTRUM_BANK_LOCK) != 0;
}

uint64_t spectrum_tick(spectrum_t *spectrum) {
  paint(spectrum);
  if (spectrum->tape != NULL) {
    tape_tick(spectrum->tape);
  }
  /* The +2's sound chip is clocked from the ULA's pin C (Worsley's +2
     redraw) at half the processor's rate: "The basic clock frequency of the
     circuit is 1.7734MHz (to 0.01%)", says the manual of the +3, whose
     sound chip answers at the same two ports (chapter 8, part 30,
     https://worldofspectrum.org/ZXSpectrum128+3Manual/chapter8pt30.html).
     Contention is the ULA stopping the processor's own clock input, so it is
     taken to leave this one running. Where its edges fall against the frame
     was not found; they fall here on the even T-states. */
  if (spectrum->model == SPECTRUM_PLUS2 && spectrum->ula.frame_tick % 2 == 0) {
    ay38910_tick(&spectrum->psg);
  }
  if (spectrum->held_ticks > 0) {
    spectrum->held_ticks--;
  } else {
    run_processor(spectrum);
  }
  ula_tick(&spectrum->ula);
  return spectrum->pins;
}

bool spectrum_instruction_complete(const spectrum_t *spectrum) {
  return z80_instruction_complete(&spectrum->cpu) && spectrum->held_ticks == 0;
}

void spectrum_finish_instruction(spectrum_t *spectrum) {
  /* Longer than the longest instruction, the charges it can earn along a
     line of the picture and an interrupt taken at the end of it, so the loop
     is bounded whatever state the machine is in. */
  for (int guard = 0; guard < 256; guard++) {
    if (spectrum_instruction_complete(spectrum)) {
      return;
    }
    spectrum_tick(spectrum);
  }
}

/* What each key carries: the letter or digit printed on it, the same in
   capitals under CAPS SHIFT, and the red legend under SYMBOL SHIFT. Keys
   whose legend is a token rather than a character — <=, THEN, AT — and the
   pound sign, which no ASCII holds, are left blank in that last column.
   Positions and legends from "Sinclair ZX Specifications" (Martin Korth),
   https://www.problemkaputt.de/zxdocs.htm, Spectrum Keyboard Assignment. */
typedef struct {
  keyboard_key key;
  char plain;
  char capital;
  char symbol;
} legend;

static const legend legends[] = {
    {SPECTRUM_KEY(0, 1), 'z', 'Z', ':'},   {SPECTRUM_KEY(0, 2), 'x', 'X', '\0'},
    {SPECTRUM_KEY(0, 3), 'c', 'C', '?'},   {SPECTRUM_KEY(0, 4), 'v', 'V', '/'},
    {SPECTRUM_KEY(1, 0), 'a', 'A', '\0'},  {SPECTRUM_KEY(1, 1), 's', 'S', '\0'},
    {SPECTRUM_KEY(1, 2), 'd', 'D', '\0'},  {SPECTRUM_KEY(1, 3), 'f', 'F', '\0'},
    {SPECTRUM_KEY(1, 4), 'g', 'G', '\0'},  {SPECTRUM_KEY(2, 0), 'q', 'Q', '\0'},
    {SPECTRUM_KEY(2, 1), 'w', 'W', '\0'},  {SPECTRUM_KEY(2, 2), 'e', 'E', '\0'},
    {SPECTRUM_KEY(2, 3), 'r', 'R', '<'},   {SPECTRUM_KEY(2, 4), 't', 'T', '>'},
    {SPECTRUM_KEY(3, 0), '1', '\0', '!'},  {SPECTRUM_KEY(3, 1), '2', '\0', '@'},
    {SPECTRUM_KEY(3, 2), '3', '\0', '#'},  {SPECTRUM_KEY(3, 3), '4', '\0', '$'},
    {SPECTRUM_KEY(3, 4), '5', '\0', '%'},  {SPECTRUM_KEY(4, 0), '0', '\0', '_'},
    {SPECTRUM_KEY(4, 1), '9', '\0', ')'},  {SPECTRUM_KEY(4, 2), '8', '\0', '('},
    {SPECTRUM_KEY(4, 3), '7', '\0', '\''}, {SPECTRUM_KEY(4, 4), '6', '\0', '&'},
    {SPECTRUM_KEY(5, 0), 'p', 'P', '"'},   {SPECTRUM_KEY(5, 1), 'o', 'O', ';'},
    {SPECTRUM_KEY(5, 2), 'i', 'I', '\0'},  {SPECTRUM_KEY(5, 3), 'u', 'U', '\0'},
    {SPECTRUM_KEY(5, 4), 'y', 'Y', '\0'},  {SPECTRUM_KEY(6, 1), 'l', 'L', '='},
    {SPECTRUM_KEY(6, 2), 'k', 'K', '+'},   {SPECTRUM_KEY(6, 3), 'j', 'J', '-'},
    {SPECTRUM_KEY(6, 4), 'h', 'H', '^'},   {SPECTRUM_KEY(7, 0), ' ', '\0', '\0'},
    {SPECTRUM_KEY(7, 2), 'm', 'M', '.'},   {SPECTRUM_KEY(7, 3), 'n', 'N', ','},
    {SPECTRUM_KEY(7, 4), 'b', 'B', '*'},
};

keyboard_key spectrum_key_for_character(char character, spectrum_shift *shift) {
  size_t count = sizeof legends / sizeof legends[0];
  for (size_t index = 0; index < count; index++) {
    if (legends[index].plain == character) {
      *shift = SPECTRUM_NO_SHIFT;
      return legends[index].key;
    }
  }
  for (size_t index = 0; index < count; index++) {
    if (legends[index].capital == character && character != '\0') {
      *shift = SPECTRUM_WITH_CAPS_SHIFT;
      return legends[index].key;
    }
  }
  for (size_t index = 0; index < count; index++) {
    if (legends[index].symbol == character && character != '\0') {
      *shift = SPECTRUM_WITH_SYMBOL_SHIFT;
      return legends[index].key;
    }
  }
  return KEYBOARD_NO_KEY;
}

uint8_t spectrum_peek(const spectrum_t *spectrum, uint16_t address) {
  return read_memory(spectrum, address);
}

void spectrum_poke(spectrum_t *spectrum, uint16_t address, uint8_t value) {
  write_memory(spectrum, address, value);
}

/* The ULA's sound pin after a write, in tenths of a millivolt, by bit 4 and
   then bit 3 of it. A 48K's is pin 28, and this is Putnik's measurement of
   an Issue 3 board, "without input signal on EAR". A +2's is the 7K010's
   MIC pin, which carries both bits as pin 28 does ("MIC/beeper", Hühn, in
   spectrum.h; and of the 128's ULA, a write of bit 4 "effectively boosts
   the MIC output", the 128 service manual, §5.6). No measurement of it was
   found, and it is taken to stand where pin 28 does. */
static const uint16_t sound_pin[2][2] = {{3400, 6600}, {35600, 37000}};

static uint16_t sound_pin_level(const spectrum_t *spectrum) {
  return sound_pin[spectrum->ula.speaker ? 1 : 0][spectrum->ula.microphone ? 1 : 0];
}

/* The speaker hangs off pin 28 through D9 into TR7, a ZTX450 emitter
   follower, with nothing else in the branch (the Issue 3 circuit diagram),
   so it stands two silicon junctions below the pin. Each is taken at the
   textbook 0.7V, an assumption: no measurement of them was found. Bit 3
   alone leaves the pin below them, and the little current they pass there,
   the service manual's "barely sufficient to drive the loudspeaker"
   (§5.4.3), is left out. */
#define SPEAKER_DROP 14000

static uint16_t loudspeaker(const spectrum_t *spectrum) {
  uint16_t pin = sound_pin_level(spectrum);
  return pin > SPEAKER_DROP ? (uint16_t)(pin - SPEAKER_DROP) : 0;
}

/* The current each of the +2's sound chip's channels pushes into TR6's
   base, in tenths of a microamp, by its level. A channel reaches the base
   through 1K8 (Worsley's +2 redraw), and the base is taken to stand at the
   transistor's base-emitter voltage where the stage rests: 0.62V at 2mA,
   typical in ON Semiconductor's datasheet (spectrum.h), carried by the
   diode law at 25°C to the 3.68mA the stage rests at, 0.636V. A level's
   current is where its output's curve meets that load. Couriersud measured
   each level's voltage into four loads and open, five points on its curve,
   and the curve is taken as straight between the two points the meeting
   lies between. Level 0 drives nothing. */
static const uint16_t base_current[16] = {
    0, 68, 98, 145, 212, 310, 439, 676, 793, 1297, 1827, 2300, 3003, 3789, 4642, 5575,
};

/* TR6's collector, GI SOUND, in tenths of a millivolt: where it rests with
   no current in, and how far it falls for each milliamp in. The 560R from
   collector to base holds the base nearly still, so a current into the
   base flows on through that resistor, and the collector falls by the
   voltage across it, less what the base-emitter voltage climbs as the
   transistor carries more. Worked out from the board's resistors, with that
   voltage following the diode law from the datasheet's 0.62V at 2mA, the
   fall from no current to all three channels at their loudest is straight
   within 3%, and is taken as straight. The gain is taken at 500, typical
   for the BC239C, the only BC239 the datasheet lists; anywhere in the
   family's 120 to 800 moves either figure by at most 2%. */
#define GI_SOUND_RESTS 26174
#define GI_SOUND_FALL_PER_MILLIAMP 5096
#define TENTHS_OF_A_MICROAMP_PER_MILLIAMP 10000

/* The MC1376's audio input, pin 5, is fed GI SOUND through 18K and the
   ULA's MIC and EAR pins through 68K each, so into no current it stands at
   their
   three voltages, each weighed by its resistor's conductance: 68, 18 and 18
   parts in 104. The EAR pin is the tape's way in, held at nothing by 390R
   to ground while no tape plays. */
#define GI_SOUND_PARTS 68
#define MIC_PARTS 18
#define EAR_PARTS 18
#define EAR_WITHOUT_A_TAPE 0

static uint16_t television(const spectrum_t *spectrum) {
  uint32_t current = 0;
  for (int channel = 0; channel < AY38910_CHANNELS; channel++) {
    current += base_current[spectrum->psg.output[channel] & 0x0F];
  }
  uint32_t fall = (current * GI_SOUND_FALL_PER_MILLIAMP + TENTHS_OF_A_MICROAMP_PER_MILLIAMP / 2) /
                  TENTHS_OF_A_MICROAMP_PER_MILLIAMP;
  uint32_t gi_sound = GI_SOUND_RESTS - fall;
  uint32_t parts = GI_SOUND_PARTS + MIC_PARTS + EAR_PARTS;
  uint32_t weighed = GI_SOUND_PARTS * gi_sound + MIC_PARTS * sound_pin_level(spectrum) +
                     EAR_PARTS * EAR_WITHOUT_A_TAPE;
  return (uint16_t)((weighed + parts / 2) / parts);
}

uint16_t spectrum_sound(const spectrum_t *spectrum) {
  return spectrum->model == SPECTRUM_PLUS2 ? television(spectrum) : loudspeaker(spectrum);
}
