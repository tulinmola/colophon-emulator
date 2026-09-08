/*
 * spectrum.c — the machine wiring: the memory map, the I/O decode, and the
 * clock the ULA stops.
 */
#include "spectrum.h"

#include <stddef.h>
#include <string.h>

/* Nothing answers above the RAM that is fitted, and the bus floats there. */
#define FLOATING 0xFF

/* Below the RAM's base this underflows, which every caller guards against
   first by knowing the ROM answers there. */
static uint32_t ram_offset(uint16_t address) { return (uint32_t)address - SPECTRUM_RAM_BASE; }

static uint8_t read_memory(const spectrum_t *spectrum, uint16_t address) {
  if (address < SPECTRUM_ROM_SIZE) {
    return spectrum->rom[address];
  }
  uint32_t offset = ram_offset(address);
  return offset < spectrum->ram_size ? spectrum->ram[offset] : FLOATING;
}

/* A write to the ROM lands nowhere: there is no RAM beneath it to fall
   through to, as there is on a CPC. */
static void write_memory(spectrum_t *spectrum, uint16_t address, uint8_t value) {
  if (address < SPECTRUM_ROM_SIZE) {
    return;
  }
  uint32_t offset = ram_offset(address);
  if (offset < spectrum->ram_size) {
    spectrum->ram[offset] = value;
  }
}

/* The ULA's address lines reach the first sixteen kilobytes of RAM and no
   further, whatever the processor is looking at, so this bound is the chip's
   and not a guess about what it will ask for. */
static uint8_t read_video(const spectrum_t *spectrum, uint16_t address) {
  uint32_t offset = ram_offset(address);
  if (offset >= SPECTRUM_RAM_16K || offset >= spectrum->ram_size) {
    return FLOATING;
  }
  return spectrum->ram[offset];
}

/* Every zero in the top half of the address selects a half-row, and the
   selected ones are wire-ANDed together — so an address with several bits
   low reads several half-rows at once, and one with none reads no keys.
   Bits 5 and 7 carry nothing and read high on every board: "Bits 5 and 7 as
   read by INning from Port 0xfe are always one" (World of Spectrum, 48K
   reference). The Sinclair Wiki's "Keyboard" page says instead that the upper
   two bits read 1 on an issue 2 and 0 on later ones; that is the issue
   difference misattributed, because what the issues differ over is bit 6 —
   how much of a write to port &FE comes back on a read — and we follow the
   reference. Bit 6 is the EAR socket, which is spectrum_t's `ear` here and
   hears nothing this machine writes. */
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

/* The ULA is the only thing fitted, and it answers every port with A0 low. */
static bool ula_answers(uint16_t port) { return (port & 0x0001) == 0; }

static uint8_t io_read(const spectrum_t *spectrum, uint16_t port) {
  return ula_answers(port) ? read_keyboard(spectrum, port) : FLOATING;
}

static void io_write(spectrum_t *spectrum, uint16_t port, uint8_t data) {
  if (ula_answers(port)) {
    ula_write(&spectrum->ula, data);
  }
}

void spectrum_init(spectrum_t *spectrum, uint8_t *ram, uint32_t ram_size, const uint8_t *rom) {
  memset(spectrum, 0, sizeof *spectrum);
  z80_init(&spectrum->cpu);
  ula_init(&spectrum->ula);
  keyboard_init(&spectrum->keyboard);
  spectrum->ram = ram;
  spectrum->ram_size = ram_size;
  spectrum->rom = rom;
}

void spectrum_connect_monitor(spectrum_t *spectrum, uint8_t *framebuffer) {
  monitor_init(&spectrum->monitor, framebuffer, SPECTRUM_FRAMEBUFFER_WIDTH,
               SPECTRUM_FRAMEBUFFER_HEIGHT, SPECTRUM_FRAME_SYNC_SAMPLES, SPECTRUM_LINE_SYNC_CENTRE);
}

/* The sixteen kilobytes the ULA's address lines reach, which on a 16K
   machine is all the RAM there is. */
static bool in_contended_memory(uint16_t address) {
  return address >= SPECTRUM_RAM_BASE && address < SPECTRUM_RAM_BASE + SPECTRUM_RAM_16K;
}

/* Which of a port access's four T-states are charged, one bit each, first
   one lowest ("Contended I/O"). */
static uint8_t port_charges(uint16_t port) {
  const bool looks_contended = in_contended_memory(port);
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
  uint16_t display_address = 0;
  uint16_t attribute_address = 0;
  uint8_t display = 0;
  uint8_t attribute = 0;
  if (ula_fetch(&spectrum->ula, &display_address, &attribute_address)) {
    display = read_video(spectrum, display_address);
    attribute = read_video(spectrum, attribute_address);
  }
  uint8_t samples[ULA_SAMPLES_PER_TICK];
  ula_video(&spectrum->ula, display, attribute, samples);
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

  z80_cycle cycle = z80_next_cycle(&spectrum->cpu);
  uint64_t pins = z80_tick(&spectrum->cpu, bus);
  if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
    /* Interrupt acknowledge. Nothing drives the bus, so the byte is the
       floating 0xFF; in mode 1, which is the only mode the firmware uses, it
       is ignored anyway. */
    pins = z80_set_data(pins, FLOATING);
  } else if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
    pins = z80_set_data(pins, read_memory(spectrum, z80_address(pins)));
  } else if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
    write_memory(spectrum, z80_address(pins), z80_data(pins));
  } else if ((pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
    io_write(spectrum, z80_address(pins), z80_data(pins));
  } else if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
    pins = z80_set_data(pins, io_read(spectrum, z80_address(pins)));
  }

  spectrum->pins = pins;

  /* The address is on the bus now and the beam has not moved, so this is
     both the address the ULA weighs and the position it weighs it at. */
  switch (cycle) {
    /* These ULAs weigh the address and not the request, so a T-state that
       only holds one costs what one that acts on it costs. */
    case Z80_CYCLE_MEMORY:
    case Z80_CYCLE_INTERNAL:
      spectrum->held_ticks =
          in_contended_memory(z80_address(pins)) ? ula_contention(spectrum->ula.frame_tick) : 0;
      break;
    case Z80_CYCLE_PORT:
      spectrum->port_charges_left = port_charges(z80_address(pins));
      break;
    /* No evidence here can say whether an acknowledge is charged: the
       interrupt is held across the first 32 T-states of a frame and the chip
       wants the bus from 14335, so an acknowledge always falls in the top
       border, where nothing is owed under any rule. */
    case Z80_CYCLE_INTERRUPT:
    case Z80_CYCLE_NONE:
      break;
  }

  if (spectrum->port_charges_left & 1) {
    spectrum->held_ticks = ula_contention(spectrum->ula.frame_tick);
  }
  spectrum->port_charges_left = (uint8_t)(spectrum->port_charges_left >> 1);
}

void spectrum_insert_tape(spectrum_t *spectrum, tape_t *tape) { spectrum->tape = tape; }

uint64_t spectrum_tick(spectrum_t *spectrum) {
  paint(spectrum);
  if (spectrum->tape != NULL) {
    tape_tick(spectrum->tape);
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
