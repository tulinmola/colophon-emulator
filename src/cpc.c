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

/* The nibble selects sixteen lines and the board wires ten, so the six above
   them are open and read &FF. Which lines exist is the board's to know, not
   the matrix's. */
static uint8_t selected_line(const cpc_t *cpc, uint8_t line) {
  return line < CPC_KEYBOARD_LINES ? keyboard_line(&cpc->keyboard, line) : 0xFF;
}

/* Port C's low nibble selects a keyboard line and its top two bits are the
   PSG's BDIR and BC1 ("8255 PPI"). */
static void run_psg(cpc_t *cpc) {
  uint8_t port_c = ppi_output_of(&cpc->ppi, PPI_PORT_C);
  psg_present_port_a(&cpc->psg, selected_line(cpc, port_c & 0x0F));
  psg_function function = (psg_function)(port_c >> 6);
  uint8_t bus = psg_access(&cpc->psg, function, ppi_output_of(&cpc->ppi, PPI_PORT_A));
  ppi_present(&cpc->ppi, PPI_PORT_A, bus);
}

/* The board wires the CRTC bus from the address lines: RS is A8 and R/W is
   A9, so &BC00-&BF00 are select, write, status and read — "The CRTC" (Grim),
   I/O ports. The direction comes from the address and not from the CPU, so
   the chip is strobed by any I/O request when A14 is low, and a CPU read of
   one of the two write ports latches whatever the address bus carried. */
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

/* Devices decode single address bits, so one access can reach several at
   once; every test in the two functions below is independent, and their
   order is the address lines' and carries no meaning — "I/O port
   allocation" (Rison & Thacker). */
static void io_write(cpc_t *cpc, uint16_t address, uint8_t data) {
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
  if ((address & 0x4000) == 0) {
    crtc_bus(cpc, address, data);
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
  if (cpc->disc_interface && (address & 0x0480) == 0) {
    /* A10 and A7 low reach the disc interface; A8 low is the motor port,
       A8 high the controller, whose data register takes a write whichever
       way A0 lies ("Floppy disc controller and Floppy disc drives"). */
    if (address & 0x0100) {
      upd765_write(&cpc->fdc, (upd765_selection)(address & 0x01), data);
    } else {
      drive_set_motor(&cpc->drives[0], (data & 0x01) != 0);
      drive_set_motor(&cpc->drives[1], (data & 0x01) != 0);
    }
  }
}

/* The bus floats at &FF by convention, and the last device to drive it wins.
   On hardware the Gate Array would execute the floating byte as a command
   ("The Gate Array"); &FF dispatches to the write-only PAL, so even that is
   silence. */
static uint8_t io_read(cpc_t *cpc, uint16_t address, bool first_tick) {
  uint8_t data = 0xFF;
  if ((address & 0x4000) == 0) {
    /* The CRTC is given no direction line here, so a read of a write port
       writes: whatever the CPU happened to put on the address bus lands in
       the selected register. For IN A,(n) that byte is A, which is the
       documented three-microsecond way to write a register (Compendium ch.
       4.4.2); for IN r,(C) it is B, which the document leaves undefined. */
    data = crtc_data(crtc_bus(cpc, address, (uint8_t)(address >> 8)));
  }
  if ((address & 0x0800) == 0) {
    present_port_b(cpc);
    run_psg(cpc);
    data = ppi_read(&cpc->ppi, (ppi_selection)((address >> 8) & 0x03));
  }
  if (cpc->disc_interface && (address & 0x0580) == 0x0100) {
    /* The controller's status register with A0 low, its data register
       with A0 high; the motor port reads as nothing. The chip takes a read
       when RD falls and holds its answer while the Gate Array stretches
       the cycle: a second read would hand over the next byte. */
    if (first_tick) {
      cpc->fdc_bus = upd765_read(&cpc->fdc, (upd765_selection)(address & 0x01));
    }
    data = cpc->fdc_bus;
  }
  return data;
}

void cpc_init(cpc_t *cpc, uint8_t *ram, uint32_t ram_size, const uint8_t *lower_rom) {
  *cpc = (cpc_t){0};
  z80_init(&cpc->cpu);
  crtc_init(&cpc->crtc);
  gate_array_init(&cpc->gate_array);
  ppi_init(&cpc->ppi);
  psg_init(&cpc->psg);
  keyboard_init(&cpc->keyboard);
  drive_init(&cpc->drives[0], 1);
  drive_init(&cpc->drives[1], 2);
  upd765_init(&cpc->fdc);
  upd765_attach(&cpc->fdc, 0, &cpc->drives[0]);
  upd765_attach(&cpc->fdc, 1, &cpc->drives[1]);
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

void cpc_fit_disc_interface(cpc_t *cpc, bool fitted) { cpc->disc_interface = fitted; }

void cpc_insert_disc(cpc_t *cpc, uint8_t drive, floppy_t *floppy) {
  drive_insert(&cpc->drives[drive & 0x01], floppy);
}

void cpc_connect_monitor(cpc_t *cpc, uint8_t *framebuffer) {
  monitor_init(&cpc->monitor, framebuffer, CPC_FRAMEBUFFER_WIDTH, CPC_FRAMEBUFFER_HEIGHT,
               CPC_FRAME_SYNC_SAMPLES, CPC_LINE_SYNC_CENTRE);
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
uint16_t cpc_video_address(const cpc_t *cpc) {
  uint16_t ma = crtc_ma(cpc->crtc_pins);
  uint8_t ra = crtc_ra(cpc->crtc_pins);
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
    uint16_t address = cpc_video_address(cpc);
    uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER];
    gate_array_video(&cpc->gate_array, (cpc->crtc_pins & CRTC_DISPTMG) != 0, cpc->ram[address],
                     cpc->ram[address | 1], samples);
    monitor_receive(&cpc->monitor, samples, GATE_ARRAY_SAMPLES_PER_CHARACTER,
                    gate_array_csync(&cpc->gate_array));
    /* The controller counts in microseconds, which is the character
       clock, and turns the drives. */
    if (cpc->disc_interface) {
      upd765_tick(&cpc->fdc);
    }
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

  /* The Gate Array's WAIT holds an I/O cycle over several ticks with its
     pins up throughout. A device is written once, when WR first falls;
     one that changes on being read is read once too, when RD does. */
  uint64_t before = cpc->pins;
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
    if ((before & (Z80_IORQ | Z80_WR)) != (Z80_IORQ | Z80_WR)) {
      io_write(cpc, z80_address(pins), z80_data(pins));
    }
  } else if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
    bool first_tick = (before & (Z80_IORQ | Z80_RD)) != (Z80_IORQ | Z80_RD);
    pins = z80_set_data(pins, io_read(cpc, z80_address(pins), first_tick));
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

/* The UK layout, positions from the matrix table in "Reading the keyboard
   and Joysticks". Keys that carry no character — cursors, function keys,
   Copy, Caps Lock, the joystick lines — are absent. */
typedef struct {
  keyboard_key key;
  char plain;
  char shifted;
} legend;

static const legend legends[] = {
    {CPC_KEY(2, 1), '[', '{'},
    {CPC_KEY(2, 3), ']', '}'},
    {CPC_KEY(2, 6), '\\', '`'},
    {CPC_KEY(3, 0), '^', '\0'}, /* shift gives the pound sign, not ASCII */
    {CPC_KEY(3, 1), '-', '='},
    {CPC_KEY(3, 2), '@', '|'},
    {CPC_KEY(3, 3), 'p', 'P'},
    {CPC_KEY(3, 4), ';', '+'},
    {CPC_KEY(3, 5), ':', '*'},
    {CPC_KEY(3, 6), '/', '?'},
    /* The source crosses these two keys, printing "> ," here and "< ." on
       line 4 bit 7; the firmware's own translation puts the full stop
       here and the comma there. */
    {CPC_KEY(3, 7), '.', '>'},
    {CPC_KEY(4, 0), '0', '_'},
    {CPC_KEY(4, 1), '9', ')'},
    {CPC_KEY(4, 2), 'o', 'O'},
    {CPC_KEY(4, 3), 'i', 'I'},
    {CPC_KEY(4, 4), 'l', 'L'},
    {CPC_KEY(4, 5), 'k', 'K'},
    {CPC_KEY(4, 6), 'm', 'M'},
    {CPC_KEY(4, 7), ',', '<'},
    {CPC_KEY(5, 0), '8', '('},
    {CPC_KEY(5, 1), '7', '\''},
    {CPC_KEY(5, 2), 'u', 'U'},
    {CPC_KEY(5, 3), 'y', 'Y'},
    {CPC_KEY(5, 4), 'h', 'H'},
    {CPC_KEY(5, 5), 'j', 'J'},
    {CPC_KEY(5, 6), 'n', 'N'},
    {CPC_KEY(5, 7), ' ', ' '},
    {CPC_KEY(6, 0), '6', '&'},
    {CPC_KEY(6, 1), '5', '%'},
    {CPC_KEY(6, 2), 'r', 'R'},
    {CPC_KEY(6, 3), 't', 'T'},
    {CPC_KEY(6, 4), 'g', 'G'},
    {CPC_KEY(6, 5), 'f', 'F'},
    {CPC_KEY(6, 6), 'b', 'B'},
    {CPC_KEY(6, 7), 'v', 'V'},
    {CPC_KEY(7, 0), '4', '$'},
    {CPC_KEY(7, 1), '3', '#'},
    {CPC_KEY(7, 2), 'e', 'E'},
    {CPC_KEY(7, 3), 'w', 'W'},
    {CPC_KEY(7, 4), 's', 'S'},
    {CPC_KEY(7, 5), 'd', 'D'},
    {CPC_KEY(7, 6), 'c', 'C'},
    {CPC_KEY(7, 7), 'x', 'X'},
    {CPC_KEY(8, 0), '1', '!'},
    {CPC_KEY(8, 1), '2', '"'},
    {CPC_KEY(8, 3), 'q', 'Q'},
    {CPC_KEY(8, 5), 'a', 'A'},
    {CPC_KEY(8, 7), 'z', 'Z'},
};

keyboard_key cpc_key_for_character(char character, bool *shifted) {
  for (size_t index = 0; index < sizeof legends / sizeof legends[0]; index++) {
    if (legends[index].plain == character) {
      *shifted = false;
      return legends[index].key;
    }
  }
  for (size_t index = 0; index < sizeof legends / sizeof legends[0]; index++) {
    if (legends[index].shifted == character && character != '\0') {
      *shifted = true;
      return legends[index].key;
    }
  }
  return KEYBOARD_NO_KEY;
}

uint8_t cpc_peek(const cpc_t *cpc, uint16_t address) {
  return cpc->read_page[address >> 14][address & 0x3FFF];
}

void cpc_poke(cpc_t *cpc, uint16_t address, uint8_t value) {
  cpc->write_page[address >> 14][address & 0x3FFF] = value;
}
