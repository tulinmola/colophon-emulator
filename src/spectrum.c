/*
 * spectrum.c — the machine wiring: memory map and I/O decode.
 */
#include "spectrum.h"

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
   Bits 5 and 7 carry nothing and read high, which is what an issue 2 board
   does: "On Issue 2 Spectrums, the upper 2 bits always read 1, whereas on
   other issue Spectrums the upper 2 bits read 0. Bit 5 can vary depending
   on the last thing played on the tape" ("Keyboard", Sinclair Wiki). A
   later board is therefore not this, and the handful of 1983 games that
   read those bits without masking would know the difference. */
static uint8_t read_keyboard(const spectrum_t *spectrum, uint16_t port) {
  uint8_t keys = 0x1F;
  for (uint8_t half_row = 0; half_row < SPECTRUM_HALF_ROWS; half_row++) {
    if ((port & (0x0100u << half_row)) == 0) {
      keys &= keyboard_line(&spectrum->keyboard, half_row);
    }
  }
  return (uint8_t)(keys | 0xA0 | (spectrum->ear ? 0x40 : 0x00));
}

/* The ULA is the only thing fitted, and it answers every port with A0 low. */
static uint8_t io_read(const spectrum_t *spectrum, uint16_t port) {
  return (port & 0x0001) ? FLOATING : read_keyboard(spectrum, port);
}

static void io_write(spectrum_t *spectrum, uint16_t port, uint8_t data) {
  if ((port & 0x0001) == 0) {
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

uint64_t spectrum_tick(spectrum_t *spectrum) {
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

  /* The ULA's interrupt line runs to the processor, and is the only thing
     that ever interrupts it. */
  uint64_t bus = spectrum->pins;
  if (ula_interrupt(&spectrum->ula)) {
    bus |= Z80_INT;
  } else {
    bus &= ~Z80_INT;
  }

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
  ula_tick(&spectrum->ula);
  return pins;
}

void spectrum_finish_instruction(spectrum_t *spectrum) {
  /* Longer than the longest instruction, so the loop is bounded whatever
     state the processor is in. */
  for (int guard = 0; guard < 256 && !z80_instruction_complete(&spectrum->cpu); guard++) {
    spectrum_tick(spectrum);
  }
}

uint8_t spectrum_peek(const spectrum_t *spectrum, uint16_t address) {
  return read_memory(spectrum, address);
}

void spectrum_poke(spectrum_t *spectrum, uint16_t address, uint8_t value) {
  write_memory(spectrum, address, value);
}
