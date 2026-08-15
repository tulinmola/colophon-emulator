/*
 * cpc_test — the memory map, driven through the bus.
 *
 * Every test runs a real program from a fabricated 16K lower ROM: the OUTs
 * that reprogram the map are executed by the CPU, not simulated around it.
 * RAM is pre-filled with HALT (&76), so the moment a banking switch pulls
 * the program out from under the CPU, the next fetch halts the machine and
 * the test inspects the aftermath.
 */
#include <string.h>

#include "cpc.h"
#include "test.h"

static uint8_t ram[0x20000];
static uint8_t lower_rom[0x4000];
static uint8_t basic_rom[0x4000];
static uint8_t extra_rom[0x4000];
static cpc_t cpc;

static void power_on(uint32_t ram_size) {
  memset(ram, 0x76, sizeof ram);
  memset(lower_rom, 0x76, sizeof lower_rom);
  memset(basic_rom, 0, sizeof basic_rom);
  memset(extra_rom, 0, sizeof extra_rom);
  cpc_init(&cpc, ram, ram_size, lower_rom);
  cpc_set_upper_rom(&cpc, 0, basic_rom);
}

static void rom_program(const uint8_t *code, size_t length) { memcpy(lower_rom, code, length); }

static size_t bank_start(int bank) { return (size_t)bank * 0x4000; }

static bool run_to_halt(void) {
  for (int ticks = 0; ticks < 10000; ticks++) {
    if (cpc_tick(&cpc) & Z80_HALT) {
      return true;
    }
  }
  return false;
}

static void run_ticks(long count) {
  for (long tick = 0; tick < count; tick++) {
    cpc_tick(&cpc);
  }
}

/* Append a select-and-write pair for one CRTC register. */
static size_t append_crtc_write(uint8_t *program, size_t length, uint8_t reg, uint8_t value) {
  program[length++] = 0x01; /* LD BC,&BC00+reg */
  program[length++] = reg;
  program[length++] = 0xBC;
  program[length++] = 0xED; /* OUT (C),C */
  program[length++] = 0x49;
  program[length++] = 0x01; /* LD BC,&BD00+value */
  program[length++] = value;
  program[length++] = 0xBD;
  program[length++] = 0xED; /* OUT (C),C */
  program[length++] = 0x49;
  return length;
}

/* The standard 50Hz screen, as the firmware programs it: every register of
   its table that is not already zero from reset. */
static size_t append_standard_screen(uint8_t *program, size_t length) {
  length = append_crtc_write(program, length, 0, 63);
  length = append_crtc_write(program, length, 1, 40);
  length = append_crtc_write(program, length, 2, 46);
  length = append_crtc_write(program, length, 3, 0x8E);
  length = append_crtc_write(program, length, 4, 38);
  length = append_crtc_write(program, length, 6, 25);
  length = append_crtc_write(program, length, 7, 30);
  length = append_crtc_write(program, length, 9, 7);
  length = append_crtc_write(program, length, 12, 0x30);
  return length;
}

/* One command byte to the Gate Array. */
static size_t append_gate_array_write(uint8_t *program, size_t length, uint8_t command) {
  program[length++] = 0x01; /* LD BC,&7F00+command */
  program[length++] = command;
  program[length++] = 0x7F;
  program[length++] = 0xED; /* OUT (C),C */
  program[length++] = 0x49;
  return length;
}

static void reset_shows_both_roms_and_the_base_map(void) {
  power_on(sizeof ram);
  lower_rom[0x123] = 0x11;
  basic_rom[0x234] = 0x22;
  ram[0x123] = 0x99;                 /* hidden beneath the lower ROM */
  ram[bank_start(1) + 0x111] = 0x44; /* configuration 0: bank 1 at &4000 */
  ram[bank_start(2) + 0x222] = 0x55; /* bank 2 at &8000 */
  TEST_EQUAL(cpc_peek(&cpc, 0x0123), 0x11);
  TEST_EQUAL(cpc_peek(&cpc, 0xC234), 0x22);
  TEST_EQUAL(cpc_peek(&cpc, 0x4111), 0x44);
  TEST_EQUAL(cpc_peek(&cpc, 0x8222), 0x55);
  TEST_EQUAL(cpc.mmr, 0xC0);
}

static void programs_fetch_from_the_lower_rom(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x3E,
      0x42, /* LD A,&42 */
      0x76, /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.cpu.a, 0x42);
}

static void write_falls_through_the_lower_rom(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x3E, 0x99,       /* LD A,&99 */
      0x32, 0x02, 0x00, /* LD (&0002),A — into the range the ROM occupies */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(ram[0x0002], 0x99);
  TEST_EQUAL(cpc_peek(&cpc, 0x0002), 0x32); /* the read still sees the ROM */
}

static void disabling_the_lower_rom_reveals_ram(void) {
  power_on(sizeof ram);
  ram[0] = 0xA7;
  const uint8_t program[] = {
      0x01, 0x84, 0x7F, /* LD BC,&7F84 — RMR: lower ROM off, upper on */
      0xED, 0x49,       /* OUT (C),C; the next fetch reads RAM and halts */
  };
  rom_program(program, sizeof program);
  TEST_EQUAL(cpc_peek(&cpc, 0), 0x01);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc_peek(&cpc, 0), 0xA7);
}

static void configuration_maps_the_documented_banks(void) {
  /* The eight configurations, restated from "The Gate Array" MMR table
     independently of the copy remap() holds. */
  static const uint8_t banking[8][4] = {
      {0, 1, 2, 3}, {0, 1, 2, 7}, {4, 5, 6, 7}, {0, 3, 2, 7},
      {0, 4, 2, 3}, {0, 5, 2, 3}, {0, 6, 2, 3}, {0, 7, 2, 3},
  };
  for (int configuration = 0; configuration < 8; configuration++) {
    power_on(sizeof ram);
    for (int bank = 0; bank < 8; bank++) {
      ram[bank_start(bank)] = (uint8_t)(0xB0 + bank);
    }
    const uint8_t program[] = {
        0x01, (uint8_t)(0xC0 + configuration),
        0x7F,       /* LD BC,&7FC0+c */
        0xED, 0x49, /* OUT (C),C — the PAL banks */
        0x0E, 0x8C, /* LD C,&8C */
        0xED, 0x49, /* OUT (C),C — RMR: both ROMs off; the next fetch halts */
    };
    rom_program(program, sizeof program);
    TEST_CHECK(run_to_halt());
    for (int quadrant = 0; quadrant < 4; quadrant++) {
      TEST_EQUAL(cpc_peek(&cpc, (uint16_t)(quadrant * 0x4000)),
                 0xB0 + banking[configuration][quadrant]);
    }
  }
}

static void write_reaches_the_configured_bank(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0xC4, 0x7F, /* LD BC,&7FC4 — configuration 4: bank 4 at &4000 */
      0xED, 0x49,       /* OUT (C),C */
      0x3E, 0x5A,       /* LD A,&5A */
      0x32, 0x00, 0x40, /* LD (&4000),A */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(ram[bank_start(4)], 0x5A);
  TEST_EQUAL(ram[bank_start(1)], 0x76); /* bank 1, now hidden, untouched */
}

static void upper_rom_selects_and_absent_numbers_fall_back(void) {
  power_on(sizeof ram);
  cpc_set_upper_rom(&cpc, 7, extra_rom);
  basic_rom[0x10] = 0xBA;
  extra_rom[0x10] = 0xE7;
  const uint8_t select_seven[] = {
      0x01, 0x07, 0xDF, /* LD BC,&DF07 */
      0xED, 0x49,       /* OUT (C),C */
      0x76,             /* HALT */
  };
  rom_program(select_seven, sizeof select_seven);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc_peek(&cpc, 0xC010), 0xE7);

  power_on(sizeof ram);
  cpc_set_upper_rom(&cpc, 7, extra_rom);
  basic_rom[0x10] = 0xBA;
  const uint8_t select_absent[] = {
      0x01, 0x2A, 0xDF, /* LD BC,&DF2A — ROM &2A is not fitted */
      0xED, 0x49,       /* OUT (C),C */
      0x76,             /* HALT */
  };
  rom_program(select_absent, sizeof select_absent);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc_peek(&cpc, 0xC010), 0xBA);
}

static void an_empty_upper_socket_reads_high(void) {
  memset(ram, 0, sizeof ram);
  memset(lower_rom, 0x76, sizeof lower_rom);
  cpc_init(&cpc, ram, sizeof ram, lower_rom);
  TEST_EQUAL(cpc_peek(&cpc, 0xC000), 0xFF);
  TEST_EQUAL(cpc_peek(&cpc, 0xFFFF), 0xFF);
}

static void a_64k_machine_ignores_banking_commands(void) {
  power_on(0x10000);
  ram[bank_start(1) + 0x20] = 0x64;
  const uint8_t program[] = {
      0x01, 0xC2, 0x7F, /* LD BC,&7FC2 — configuration 2, were a PAL fitted */
      0xED, 0x49,       /* OUT (C),C */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.mmr, 0xC0);
  TEST_EQUAL(cpc_peek(&cpc, 0x4020), 0x64);
}

static void one_write_reaches_the_pal_and_the_rom_latch(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0x00, 0x00, /* LD BC,&0000 — A15=0 for the PAL, A13=0 for the latch */
      0x3E, 0xC1,       /* LD A,&C1 */
      0xED, 0x79,       /* OUT (C),A */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.mmr, 0xC1);
  TEST_EQUAL(cpc.upper_rom_number, 0xC1);
  TEST_CHECK(cpc.gate_array.lower_rom_enabled);
  TEST_CHECK(cpc.gate_array.upper_rom_enabled);
  /* A14 is low too, so the CRTC heard the same write as a select. */
  TEST_EQUAL(cpc.crtc.address_register, 0xC1 & 0x1F);
}

static void the_gate_array_needs_address_bit_14(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0x00, 0x00, /* LD BC,&0000 — not a Gate Array address */
      0x3E, 0x8C,       /* LD A,&8C — an RMR command, both ROMs off */
      0xED, 0x79,       /* OUT (C),A */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_CHECK(cpc.gate_array.lower_rom_enabled);
  TEST_CHECK(cpc.gate_array.upper_rom_enabled);
  TEST_EQUAL(cpc_peek(&cpc, 0), 0x01); /* still the ROM's first byte */
}

static void the_crtc_learns_the_firmware_table(void) {
  power_on(sizeof ram);
  /* The values the 6128 firmware programs after boot, written 15 down to 0
     the way the ROM writes them (Compendium ch. 4.1; observed in our own
     boot trace). */
  static const uint8_t table[16] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 48, 0x00, 0xC0, 0};
  uint8_t program[161];
  size_t length = 0;
  for (int reg = 15; reg >= 0; reg--) {
    length = append_crtc_write(program, length, (uint8_t)reg, table[reg]);
  }
  program[length++] = 0x76; /* HALT */
  rom_program(program, length);
  TEST_CHECK(run_to_halt());
  for (int reg = 0; reg < 14; reg++) {
    TEST_EQUAL(cpc.crtc.registers[reg], table[reg]);
  }
  /* R14 is six bits wide: the firmware's &C0 falls off the ends. */
  TEST_EQUAL(cpc.crtc.registers[14], 0);
}

static void the_cpu_reads_the_crtc_back(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0x0C, 0xBC, /* LD BC,&BC0C — select R12 */
      0xED, 0x49,       /* OUT (C),C */
      0x01, 0x30, 0xBD, /* LD BC,&BD30 — R12 = &30 */
      0xED, 0x49,       /* OUT (C),C */
      0x01, 0x00, 0xBF, /* LD BC,&BF00 — the read port */
      0xED, 0x78,       /* IN A,(C) */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.cpu.a, 0x30);
}

static void a_machine_tick_is_a_quarter_character(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0x00, 0xBC, /* LD BC,&BC00 — select R0 */
      0xED, 0x49,       /* OUT (C),C */
      0x01, 0x3F, 0xBD, /* LD BC,&BD3F — R0 = 63 */
      0xED, 0x49,       /* OUT (C),C */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  uint8_t c0_at_halt = cpc.crtc.c0;
  for (int tick = 0; tick < 4 * 64; tick++) {
    cpc_tick(&cpc);
  }
  /* 256 CPU ticks are 64 characters: one whole scanline, C0 back where it
     stood. */
  TEST_EQUAL(cpc.crtc.c0, c0_at_halt);
}

static void pens_and_inks_reach_the_gate_array(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0x05, 0x7F, /* LD BC,&7F05 — PENR: pen 5 */
      0xED, 0x49,       /* OUT (C),C */
      0x01, 0x54, 0x7F, /* LD BC,&7F54 — INKR: colour &14 */
      0xED, 0x49,       /* OUT (C),C */
      0x01, 0x10, 0x7F, /* LD BC,&7F10 — PENR: the border */
      0xED, 0x49,       /* OUT (C),C */
      0x01, 0x4B, 0x7F, /* LD BC,&7F4B — INKR: colour &0B */
      0xED, 0x49,       /* OUT (C),C */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.gate_array.inks[5], 0x14);
  TEST_EQUAL(cpc.gate_array.inks[16], 0x0B);
}

static void the_gate_array_interrupts_six_times_a_frame(void) {
  power_on(sizeof ram);
  /* The interrupt handler, at the mode-1 vector inside our fabricated
     ROM. */
  const uint8_t handler[] = {
      0x3C, /* INC A — count the interrupt */
      0xFB, /* EI */
      0xC9, /* RET */
  };
  memcpy(lower_rom + 0x38, handler, sizeof handler);
  uint8_t body[200];
  size_t length = 0;
  body[length++] = 0x31; /* LD SP,&C000 — the stack must live below the
                            upper ROM: pushes fall through to RAM anywhere,
                            but pops beneath an enabled ROM read the ROM */
  body[length++] = 0x00;
  body[length++] = 0xC0;
  length = append_standard_screen(body, length);
  body[length++] = 0xAF; /* XOR A */
  body[length++] = 0xED; /* IM 1 */
  body[length++] = 0x56;
  body[length++] = 0xFB; /* EI */
  body[length++] = 0x76; /* HALT */
  body[length++] = 0x18; /* JR back to the HALT */
  body[length++] = 0xFD;
  memcpy(lower_rom + 0x100, body, length);
  const uint8_t entry[] = {0xC3, 0x00, 0x01}; /* JP &0100 */
  memcpy(lower_rom, entry, sizeof entry);
  /* Two and a half frames of 312 lines at 256 T-states the line: at 300Hz
     that is 14 or 15 interrupts. */
  run_ticks(200000);
  TEST_CHECK(cpc.cpu.a >= 12 && cpc.cpu.a <= 16);
}

static void an_unheard_interrupt_is_held(void) {
  power_on(sizeof ram);
  uint8_t body[200];
  size_t length = append_standard_screen(body, 0);
  body[length++] = 0x76; /* HALT, interrupts never enabled */
  memcpy(lower_rom + 0x100, body, length);
  const uint8_t entry[] = {0xC3, 0x00, 0x01}; /* JP &0100 */
  memcpy(lower_rom, entry, sizeof entry);
  run_ticks(100000);
  TEST_CHECK(cpc.cpu.halted);
  TEST_CHECK(cpc.gate_array.interrupt_request); /* maintained, unheard */
}

static uint8_t framebuffer[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT];

/* Fill the screen with a full-brightness pattern, program the standard
   screen in mode 2, and run frames. Pen 1 is the only ink of its colour on
   the tube, so where it lands is where the display is. */
static void draw_a_full_screen(void) {
  power_on(sizeof ram);
  memset(framebuffer, 0xEE, sizeof framebuffer); /* no colour code is &EE */
  memset(ram + 0xC000, 0xFF, 0x4000);
  cpc_connect_monitor(&cpc, framebuffer);
  uint8_t body[200];
  size_t length = append_standard_screen(body, 0);
  length = append_gate_array_write(body, length, 0x00); /* PENR: pen 0 */
  length = append_gate_array_write(body, length, 0x40 | GATE_ARRAY_BLACK);
  length = append_gate_array_write(body, length, 0x01);      /* PENR: pen 1 */
  length = append_gate_array_write(body, length, 0x40 | 11); /* bright white */
  length = append_gate_array_write(body, length, 0x10);      /* PENR: border */
  length = append_gate_array_write(body, length, 0x40 | 4);  /* blue */
  length = append_gate_array_write(body, length, 0x8A);      /* RMR: mode 2 */
  body[length++] = 0x18;                                     /* JR $ */
  body[length++] = 0xFE;
  rom_program(body, length);
  run_ticks(3L * 19968 * 4); /* three frames of 19968 characters */
}

static void the_display_lands_where_the_syncs_put_it(void) {
  draw_a_full_screen();
  int left = CPC_FRAMEBUFFER_WIDTH;
  int right = -1;
  int top = CPC_FRAMEBUFFER_HEIGHT;
  int bottom = -1;
  long white = 0;
  for (int y = 0; y < CPC_FRAMEBUFFER_HEIGHT; y++) {
    for (int x = 0; x < CPC_FRAMEBUFFER_WIDTH; x++) {
      if (framebuffer[(size_t)y * CPC_FRAMEBUFFER_WIDTH + x] != 11) {
        continue;
      }
      white++;
      if (x < left) {
        left = x;
      }
      if (x > right) {
        right = x;
      }
      if (y < top) {
        top = y;
      }
      if (y > bottom) {
        bottom = y;
      }
    }
  }
  /* 40 characters of 16 pixel clocks, 25 rows of 8 lines. */
  TEST_EQUAL(white, 640L * 200L);
  TEST_EQUAL(right - left + 1, 640);
  TEST_EQUAL(bottom - top + 1, 200);
  /* The beam starts its line when the Gate Array's sync does, two
     characters after the CRTC's HSYNC at R2=46, and the picture arrives a
     microsecond behind the address that fetched it. */
  TEST_EQUAL(left, (64 - 48 + 1) * 16);
  /* The beam's rows begin 48 characters into the CRTC's lines, so a line's
     display falls in the row its predecessor opened. */
  TEST_EQUAL(top, 70);
}

static void the_border_surrounds_the_display(void) {
  draw_a_full_screen();
  TEST_EQUAL(framebuffer[70 * CPC_FRAMEBUFFER_WIDTH + 271], 4);  /* left of it */
  TEST_EQUAL(framebuffer[70 * CPC_FRAMEBUFFER_WIDTH + 912], 4);  /* right of it */
  TEST_EQUAL(framebuffer[69 * CPC_FRAMEBUFFER_WIDTH + 272], 4);  /* above it */
  TEST_EQUAL(framebuffer[270 * CPC_FRAMEBUFFER_WIDTH + 272], 4); /* below it */
}

static void the_beam_sweeps_every_line_below_the_flyback(void) {
  draw_a_full_screen();
  /* Rows 0 to 25 hold the frame flyback, where the beam's path is not a
     full line and its corners are never swept — as on a tube, and blanked
     black in any case. Everything below is covered edge to edge. */
  long unpainted = 0;
  for (int y = 26; y < CPC_FRAMEBUFFER_HEIGHT; y++) {
    for (int x = 0; x < CPC_FRAMEBUFFER_WIDTH; x++) {
      if (framebuffer[(size_t)y * CPC_FRAMEBUFFER_WIDTH + x] == 0xEE) {
        unpainted++;
      }
    }
  }
  TEST_EQUAL(unpainted, 0);
}

static void the_screen_is_read_from_the_base_ram_alone(void) {
  draw_a_full_screen();
  /* Bank in the extension over &C000 and blank what the CPU now sees
     there: the video hardware keeps reading the base 64K underneath. */
  uint8_t body[200];
  size_t length = append_gate_array_write(body, 0, 0xC2); /* MMR: banks 4-7 */
  body[length++] = 0x18;
  body[length++] = 0xFE;
  memcpy(lower_rom, body, length);
  cpc.cpu.pc = 0;
  memset(ram + bank_start(7), 0x00, 0x4000);
  memset(framebuffer, 0xEE, sizeof framebuffer);
  run_ticks(2L * 19968 * 4);
  TEST_EQUAL(framebuffer[71 * CPC_FRAMEBUFFER_WIDTH + 272], 11);
}

/* One OUT to a PPI port: LD BC,&Fx00+value; OUT (C),C. */
static size_t append_ppi_write(uint8_t *program, size_t length, uint8_t port, uint8_t value) {
  program[length++] = 0x01;
  program[length++] = value;
  program[length++] = port;
  program[length++] = 0xED;
  program[length++] = 0x49;
  return length;
}

/* The keyboard scan exactly as cpctech documents it: name register 14, go
   inactive, turn port A around, choose a line, read, turn back. */
static void the_documented_scan_reads_a_line(void) {
  power_on(sizeof ram);
  keyboard_press(&cpc.keyboard, KEYBOARD_KEY(6, 3)); /* T */
  uint8_t body[80];
  size_t length = 0;
  length = append_ppi_write(body, length, 0xF7, 0x82); /* port A output */
  length = append_ppi_write(body, length, 0xF4, 14);   /* the register index */
  length = append_ppi_write(body, length, 0xF6, 0xC0); /* select it */
  length = append_ppi_write(body, length, 0xF6, 0x00); /* inactive */
  length = append_ppi_write(body, length, 0xF7, 0x92); /* port A input */
  length = append_ppi_write(body, length, 0xF6, 0x06); /* line 6 */
  length = append_ppi_write(body, length, 0xF6, 0x46); /* read, line 6 */
  body[length++] = 0x01;                               /* LD BC,&F400 */
  body[length++] = 0x00;
  body[length++] = 0xF4;
  body[length++] = 0xED; /* IN A,(C) */
  body[length++] = 0x78;
  body[length++] = 0x76; /* HALT */
  rom_program(body, length);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.cpu.a, 0xF7); /* bit 3 down, the rest released */
}

static void an_unpressed_keyboard_reads_high_through_the_chips(void) {
  power_on(sizeof ram);
  uint8_t body[80];
  size_t length = 0;
  length = append_ppi_write(body, length, 0xF7, 0x82);
  length = append_ppi_write(body, length, 0xF4, 14);
  length = append_ppi_write(body, length, 0xF6, 0xC0);
  length = append_ppi_write(body, length, 0xF6, 0x00);
  length = append_ppi_write(body, length, 0xF7, 0x92);
  length = append_ppi_write(body, length, 0xF6, 0x42); /* read, line 2 */
  body[length++] = 0x01;
  body[length++] = 0x00;
  body[length++] = 0xF4;
  body[length++] = 0xED;
  body[length++] = 0x78;
  body[length++] = 0x76;
  rom_program(body, length);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.cpu.a, 0xFF);
}

static void port_b_carries_the_links_and_the_vsync(void) {
  power_on(sizeof ram);
  const uint8_t program[] = {
      0x01, 0x00, 0xF5, /* LD BC,&F500 — port B */
      0xED, 0x78,       /* IN A,(C) */
      0x76,             /* HALT */
  };
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  /* 50Hz and Amstrad, with the cassette, printer and expansion floating
     high. Bit 0 is the CRTC's VSYNC passed straight through, whatever it
     happens to be: an unprogrammed CRTC has R7 at zero and so never leaves
     its VSYNC, which is a degenerate frame but a real one. */
  TEST_EQUAL(cpc.cpu.a & 0x10, 0x10);
  TEST_EQUAL((cpc.cpu.a >> 1) & 0x07, CPC_MANUFACTURER_AMSTRAD);
  TEST_EQUAL(cpc.cpu.a & 0x01, (cpc.crtc_pins & CRTC_VSYNC) ? 1 : 0);

  power_on(sizeof ram);
  cpc_set_links(&cpc, false, 5); /* 60Hz, Schneider */
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(cpc.cpu.a & 0x10, 0);
  TEST_EQUAL((cpc.cpu.a >> 1) & 0x07, 5);
}

static void port_b_follows_the_crtc_into_vsync(void) {
  power_on(sizeof ram);
  uint8_t body[200];
  size_t length = append_standard_screen(body, 0);
  /* Poll port B until VSYNC arrives, then halt. */
  size_t loop = length;
  body[length++] = 0x01; /* LD BC,&F500 */
  body[length++] = 0x00;
  body[length++] = 0xF5;
  body[length++] = 0xED; /* IN A,(C) */
  body[length++] = 0x78;
  body[length++] = 0xE6; /* AND 1 */
  body[length++] = 0x01;
  body[length++] = 0x28; /* JR Z,loop */
  uint8_t displacement = (uint8_t)(loop - (length + 1));
  body[length++] = displacement;
  body[length++] = 0x76; /* HALT */
  rom_program(body, length);
  for (int ticks = 0; ticks < 400000; ticks++) {
    if (cpc_tick(&cpc) & Z80_HALT) {
      break;
    }
  }
  TEST_CHECK(cpc.cpu.halted);
  TEST_CHECK((cpc.crtc_pins & CRTC_VSYNC) != 0);
}

static void poke_lands_beneath_the_rom(void) {
  power_on(sizeof ram);
  lower_rom[0] = 0xAB;
  cpc_poke(&cpc, 0, 0x12);
  TEST_EQUAL(ram[0], 0x12);
  TEST_EQUAL(cpc_peek(&cpc, 0), 0xAB);
}

int main(void) {
  TEST_RUN(reset_shows_both_roms_and_the_base_map);
  TEST_RUN(programs_fetch_from_the_lower_rom);
  TEST_RUN(write_falls_through_the_lower_rom);
  TEST_RUN(disabling_the_lower_rom_reveals_ram);
  TEST_RUN(configuration_maps_the_documented_banks);
  TEST_RUN(write_reaches_the_configured_bank);
  TEST_RUN(upper_rom_selects_and_absent_numbers_fall_back);
  TEST_RUN(an_empty_upper_socket_reads_high);
  TEST_RUN(a_64k_machine_ignores_banking_commands);
  TEST_RUN(one_write_reaches_the_pal_and_the_rom_latch);
  TEST_RUN(the_gate_array_needs_address_bit_14);
  TEST_RUN(the_crtc_learns_the_firmware_table);
  TEST_RUN(the_cpu_reads_the_crtc_back);
  TEST_RUN(a_machine_tick_is_a_quarter_character);
  TEST_RUN(pens_and_inks_reach_the_gate_array);
  TEST_RUN(the_gate_array_interrupts_six_times_a_frame);
  TEST_RUN(an_unheard_interrupt_is_held);
  TEST_RUN(the_display_lands_where_the_syncs_put_it);
  TEST_RUN(the_border_surrounds_the_display);
  TEST_RUN(the_beam_sweeps_every_line_below_the_flyback);
  TEST_RUN(the_screen_is_read_from_the_base_ram_alone);
  TEST_RUN(the_documented_scan_reads_a_line);
  TEST_RUN(an_unpressed_keyboard_reads_high_through_the_chips);
  TEST_RUN(port_b_carries_the_links_and_the_vsync);
  TEST_RUN(port_b_follows_the_crtc_into_vsync);
  TEST_RUN(poke_lands_beneath_the_rom);
  return TEST_REPORT("cpc");
}
