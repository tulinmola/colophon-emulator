/*
 * cpc.c — the machine wiring: memory map and I/O decode.
 */
#include <stddef.h>

#include "cpc.h"

/* A socket with nothing in it leaves the data bus floating; reads return &FF
   by convention. */
static uint8_t absent_rom[0x4000];

static void remap(cpc_t *cpc) {
  /* The eight PAL configurations: which 16K RAM bank answers each quadrant
     of the CPU address space — "The Gate Array" (Grim), MMR table, with the
     6128's single 64K page. */
  static const uint8_t banking[8][4] = {
      {0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
      {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3},
  };
  const uint8_t *banks = banking[cpc->mmr & 0x07];
  for (int quadrant = 0; quadrant < 4; quadrant++) {
    uint8_t *bank = cpc->ram + (size_t)banks[quadrant] * 0x4000;
    cpc->read_page[quadrant] = bank;
    cpc->write_page[quadrant] = bank;
  }
  /* An enabled ROM answers reads; writes keep falling through to the RAM
     beneath it — "The Gate Array", Upper ROM section. */
  if (cpc->lower_rom_enabled) {
    cpc->read_page[0] = cpc->lower_rom;
  }
  if (cpc->upper_rom_enabled) {
    const uint8_t *rom = cpc->upper_roms[cpc->upper_rom_number];
    if (rom == NULL) {
      rom = cpc->upper_roms[0]; /* absent numbers resolve to ROM 0 */
    }
    cpc->read_page[3] = rom == NULL ? absent_rom : rom;
  }
}

static void io_write(cpc_t *cpc, uint16_t address, uint8_t data) {
  /* Devices decode single address bits, so one write can reach several at
     once; each test below is independent — "I/O port allocation" (Rison &
     Thacker). */
  bool pal_fitted = cpc->ram_size >= 0x20000;
  if ((address & 0x8000) == 0 && (data & 0xC0) == 0xC0 && pal_fitted) {
    cpc->mmr = data;
    remap(cpc);
  }
  if ((address & 0xC000) == 0x4000) {
    /* The Gate Array examines the data byte; bit 5 has no effect on a real
       CPC's dispatch ("The Gate Array"). Pen and ink commands (bits 7-6 =
       00, 01), RMR's mode bits 1-0 and interrupt-counter bit 4 wait for the
       Gate Array module. */
    if ((data & 0xC0) == 0x80) {
      cpc->upper_rom_enabled = (data & 0x08) == 0;
      cpc->lower_rom_enabled = (data & 0x04) == 0;
      remap(cpc);
    }
  }
  if ((address & 0x2000) == 0) {
    cpc->upper_rom_number = data;
    remap(cpc);
  }
}

/* The board wires the CRTC bus from the address lines: RS is A8 and R/W is
   A9, so &BC00-&BF00 are select, write, status and read — "The CRTC" (Grim),
   I/O ports. The chip is strobed by any I/O request when A14 is low: a CPU
   read of a write port still makes it latch the bus, which floats — &FF by
   our convention. */
static uint64_t crtc_bus(cpc_t *cpc, uint16_t address, uint8_t data) {
  uint64_t pins = CRTC_CS | crtc_set_data(0, data);
  if (address & 0x0100) {
    pins |= CRTC_RS;
  }
  if (address & 0x0200) {
    pins |= CRTC_RW;
  }
  return crtc_access(&cpc->crtc, pins);
}

void cpc_init(cpc_t *cpc, uint8_t *ram, uint32_t ram_size, const uint8_t *lower_rom) {
  *cpc = (cpc_t){0};
  z80_init(&cpc->cpu);
  crtc_init(&cpc->crtc);
  cpc->ram = ram;
  cpc->ram_size = ram_size;
  cpc->lower_rom = lower_rom;
  cpc->mmr = 0xC0;
  cpc->lower_rom_enabled = true;
  cpc->upper_rom_enabled = true;
  for (int index = 0; index < 0x4000; index++) {
    absent_rom[index] = 0xFF;
  }
  remap(cpc);
}

void cpc_set_upper_rom(cpc_t *cpc, uint8_t number, const uint8_t *rom) {
  cpc->upper_roms[number] = rom;
  remap(cpc);
}

uint64_t cpc_tick(cpc_t *cpc) {
  if (cpc->clock_phase == 0) {
    cpc->crtc_pins = crtc_tick(&cpc->crtc);
  }
  cpc->clock_phase = (uint8_t)((cpc->clock_phase + 1) & 3);

  uint64_t pins = z80_tick(&cpc->cpu, cpc->pins);
  if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
    uint16_t address = z80_address(pins);
    pins = z80_set_data(pins, cpc->read_page[address >> 14][address & 0x3FFF]);
  } else if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
    uint16_t address = z80_address(pins);
    cpc->write_page[address >> 14][address & 0x3FFF] = z80_data(pins);
  } else if ((pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
    uint16_t address = z80_address(pins);
    io_write(cpc, address, z80_data(pins));
    if (!(address & 0x4000)) {
      crtc_bus(cpc, address, z80_data(pins));
    }
  } else if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
    /* The bus floats at &FF by convention; a device that drives it
       overwrites. On hardware the Gate Array would execute the floating
       byte as a command ("The Gate Array"); &FF dispatches to the
       write-only PAL, so even that is silence. */
    uint16_t address = z80_address(pins);
    uint8_t data = 0xFF;
    if (!(address & 0x4000)) {
      data = crtc_data(crtc_bus(cpc, address, data));
    }
    pins = z80_set_data(pins, data);
  }
  cpc->pins = pins;
  return pins;
}

uint8_t cpc_peek(const cpc_t *cpc, uint16_t address) {
  return cpc->read_page[address >> 14][address & 0x3FFF];
}

void cpc_poke(cpc_t *cpc, uint16_t address, uint8_t value) {
  cpc->write_page[address >> 14][address & 0x3FFF] = value;
}
