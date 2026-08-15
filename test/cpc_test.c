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
  TEST_CHECK(cpc.lower_rom_enabled);
  TEST_CHECK(cpc.upper_rom_enabled);
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
  TEST_CHECK(cpc.lower_rom_enabled);
  TEST_CHECK(cpc.upper_rom_enabled);
  TEST_EQUAL(cpc_peek(&cpc, 0), 0x01); /* still the ROM's first byte */
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
  TEST_RUN(poke_lands_beneath_the_rom);
  return TEST_REPORT("cpc");
}
