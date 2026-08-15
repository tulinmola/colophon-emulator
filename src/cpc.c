/*
 * cpc.c — the machine wiring: memory map and I/O decode.
 */
#include <stddef.h>

#include "cpc.h"

/* A socket with nothing in it leaves the data bus floating; reads return &FF
   by convention. */
static uint8_t absent_rom[0x4000];

void cpc_remap(cpc_t *cpc) {
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
  if (cpc->gate_array.lower_rom_enabled) {
    cpc->read_page[0] = cpc->lower_rom;
  }
  if (cpc->gate_array.upper_rom_enabled) {
    const uint8_t *rom = cpc->upper_roms[cpc->upper_rom_number];
    if (rom == NULL) {
      rom = cpc->upper_roms[0]; /* absent numbers resolve to ROM 0 */
    }
    cpc->read_page[3] = rom == NULL ? absent_rom : rom;
  }
}

/* Port B is wired to the outside world and to the CRTC: bit 7 the cassette,
   bit 6 the printer's ready line inverted, bit 5 the expansion port, bit 4
   the refresh-rate link, bits 3-1 the manufacturer's, and bit 0 the CRTC's
   VSYNC straight through ("8255 PPI"). Nothing is connected to the cassette
   or the printer here, and both float high. */
static void present_port_b(cpc_t *cpc) {
  uint8_t levels = 0xE0;
  if (cpc->fifty_hz) {
    levels |= 0x10;
  }
  levels |= (uint8_t)(cpc->manufacturer << 1);
  if (cpc->crtc_pins & CRTC_VSYNC) {
    levels |= 0x01;
  }
  ppi_present(&cpc->ppi, PPI_PORT_B, levels);
}

/* Port C's low nibble selects a keyboard line and its top two bits are the
   PSG's BDIR and BC1 ("8255 PPI"). */
static void run_psg(cpc_t *cpc) {
  uint8_t port_c = ppi_output_of(&cpc->ppi, PPI_PORT_C);
  psg_present_port_a(&cpc->psg, keyboard_line(&cpc->keyboard, port_c & 0x0F));
  psg_function function = (psg_function)(port_c >> 6);
  uint8_t bus = psg_access(&cpc->psg, function, ppi_output_of(&cpc->ppi, PPI_PORT_A));
  ppi_present(&cpc->ppi, PPI_PORT_A, bus);
}

static void io_write(cpc_t *cpc, uint16_t address, uint8_t data) {
  /* Devices decode single address bits, so one write can reach several at
     once; each test below is independent — "I/O port allocation" (Rison &
     Thacker). */
  bool pal_fitted = cpc->ram_size >= 0x20000;
  if ((address & 0x8000) == 0 && (data & 0xC0) == 0xC0 && pal_fitted) {
    cpc->mmr = data;
    cpc_remap(cpc);
  }
  if ((address & 0xC000) == 0x4000) {
    gate_array_write(&cpc->gate_array, data);
    if ((data & 0xC0) == 0x80) {
      cpc_remap(cpc); /* RMR may have moved the ROM enables */
    }
  }
  if ((address & 0x2000) == 0) {
    cpc->upper_rom_number = data;
    cpc_remap(cpc);
  }
  if ((address & 0x0800) == 0) {
    /* The PPI, its two low address lines choosing the port. Writing any of
       them can change what the PSG is being told, so the chip is run
       afterwards. */
    ppi_write(&cpc->ppi, (ppi_selection)((address >> 8) & 0x03), data);
    run_psg(cpc);
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
  gate_array_init(&cpc->gate_array);
  ppi_init(&cpc->ppi);
  psg_init(&cpc->psg);
  keyboard_init(&cpc->keyboard);
  cpc->fifty_hz = true;
  cpc->manufacturer = CPC_MANUFACTURER_AMSTRAD;
  cpc->ram = ram;
  cpc->ram_size = ram_size;
  cpc->lower_rom = lower_rom;
  cpc->mmr = 0xC0;
  for (int index = 0; index < 0x4000; index++) {
    absent_rom[index] = 0xFF;
  }
  cpc_remap(cpc);
}

void cpc_set_upper_rom(cpc_t *cpc, uint8_t number, const uint8_t *rom) {
  cpc->upper_roms[number] = rom;
  cpc_remap(cpc);
}

void cpc_connect_monitor(cpc_t *cpc, uint8_t *framebuffer) {
  monitor_init(&cpc->monitor, framebuffer, CPC_FRAMEBUFFER_WIDTH, CPC_FRAMEBUFFER_HEIGHT,
               CPC_FRAME_SYNC_SAMPLES);
}

void cpc_set_links(cpc_t *cpc, bool fifty_hz, uint8_t manufacturer) {
  cpc->fifty_hz = fifty_hz;
  cpc->manufacturer = manufacturer & 0x07;
}

/* The CRTC's address lines do not reach the RAM in order. The board sends
   MA13 and MA12 to A15 and A14, the three raster lines to A13-A11, and the
   low ten of MA to A10-A1, leaving A0 for the Gate Array to toggle between
   the two bytes of the character ("Screen memory addressess"). That is why
   a character row lives in eight blocks two kilobytes apart. */
static uint16_t video_address(uint64_t crtc_pins) {
  uint16_t ma = crtc_ma(crtc_pins);
  uint8_t ra = crtc_ra(crtc_pins);
  return (uint16_t)(((ma & 0x3000) << 2) | ((ra & 0x07) << 11) | ((ma & 0x03FF) << 1));
}

uint64_t cpc_tick(cpc_t *cpc) {
  gate_array_advance_phase(&cpc->gate_array);
  if (gate_array_character_clock(&cpc->gate_array)) {
    cpc->crtc_pins = crtc_tick(&cpc->crtc);
    gate_array_tick(&cpc->gate_array, (cpc->crtc_pins & CRTC_HSYNC) != 0,
                    (cpc->crtc_pins & CRTC_VSYNC) != 0);
    /* The video hardware reads the base 64K and nothing else: no ROM, no
       banked RAM, whatever the CPU is looking at ("The Gate Array", MMR). */
    uint16_t address = video_address(cpc->crtc_pins);
    uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER];
    gate_array_video(&cpc->gate_array, (cpc->crtc_pins & CRTC_DISPTMG) != 0, cpc->ram[address],
                     cpc->ram[address | 1], samples);
    monitor_receive(&cpc->monitor, samples, GATE_ARRAY_SAMPLES_PER_CHARACTER,
                    gate_array_csync(&cpc->gate_array));
  }

  /* The Gate Array's INT line runs to the CPU; the machine holds it until
     the acknowledge below drops it. READY runs to the CPU's WAIT, which is
     what keeps the CPU off the RAM the video hardware is using. */
  uint64_t bus = cpc->pins;
  if (gate_array_interrupt(&cpc->gate_array)) {
    bus |= Z80_INT;
  } else {
    bus &= ~Z80_INT;
  }
  if (gate_array_ready(&cpc->gate_array)) {
    bus |= Z80_WAIT;
  } else {
    bus &= ~Z80_WAIT;
  }

  uint64_t pins = z80_tick(&cpc->cpu, bus);
  if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
    /* Interrupt acknowledge: the Gate Array drops INT and kills R52's bit
       5; the data bus floats, &FF by convention (in mode 1 the byte is
       ignored; the Compendium ch. 27.5 finds it undetermined on hardware). */
    gate_array_interrupt_acknowledged(&cpc->gate_array);
    pins = z80_set_data(pins, 0xFF);
  } else if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
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
    if (!(address & 0x0800)) {
      present_port_b(cpc);
      run_psg(cpc);
      data = ppi_read(&cpc->ppi, (ppi_selection)((address >> 8) & 0x03));
    }
    pins = z80_set_data(pins, data);
  }
  cpc->pins = pins;
  return pins;
}

void cpc_finish_instruction(cpc_t *cpc) {
  /* Longer than any instruction can take, wait states and all, so a CPU
     wedged by a machine that never releases WAIT cannot hang the caller. */
  for (int guard = 0; guard < 256 && !z80_instruction_complete(&cpc->cpu); guard++) {
    cpc_tick(cpc);
  }
}

uint8_t cpc_peek(const cpc_t *cpc, uint16_t address) {
  return cpc->read_page[address >> 14][address & 0x3FFF];
}

void cpc_poke(cpc_t *cpc, uint16_t address, uint8_t value) {
  cpc->write_page[address >> 14][address & 0x3FFF] = value;
}
