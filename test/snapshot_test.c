/*
 * snapshot_test — save a machine, restore it, insist it is the same one.
 *
 * The round trip is the strong test here: every field the format carries
 * has to survive being written and read, and a machine restored from its
 * own snapshot has to go on computing the same thing. A reader alone could
 * agree with a writer that was wrong in the same place; a reader that is
 * asked to resume a running program cannot.
 */
#include <stdlib.h>
#include <string.h>

#include "snapshot.h"
#include "test.h"

static uint8_t ram[0x20000];
static uint8_t other_ram[0x20000];
static uint8_t lower_rom[0x4000];
static uint8_t upper_rom[0x4000];
static uint8_t bytes[SNAPSHOT_HEADER_SIZE + 0x20000];
static cpc_t cpc;
static cpc_t restored;

static void power_on(cpc_t *machine, uint8_t *memory, uint32_t size) {
  cpc_init(machine, memory, size, lower_rom);
  cpc_set_upper_rom(machine, 0, upper_rom);
}

/* A program that keeps changing something we can compare: it counts in a
   loop, writing each step to a fixed address. */
static void load_a_running_program(void) {
  static const uint8_t program[] = {
      0x3E, 0x00,       /* LD A,0        */
      0x3C,             /* INC A         */
      0x32, 0x00, 0x90, /* LD (&9000),A  */
      0x18, 0xFA,       /* JR back to the INC */
  };
  memset(lower_rom, 0, sizeof lower_rom);
  memcpy(lower_rom, program, sizeof program);
}

static void run(cpc_t *machine, long ticks) {
  for (long tick = 0; tick < ticks; tick++) {
    cpc_tick(machine);
  }
  cpc_finish_instruction(machine);
}

/* Saving mid-instruction is refused, because the format cannot describe it
   and a machine restored from such a snapshot resumes into the middle of an
   instruction it never began. */
static void a_half_done_instruction_cannot_be_saved(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  const char *problem = NULL;
  for (int tick = 0; tick < 200; tick++) {
    cpc_tick(&cpc);
    if (!z80_instruction_complete(&cpc.cpu)) {
      break;
    }
  }
  TEST_CHECK(!z80_instruction_complete(&cpc.cpu));
  TEST_CHECK(!snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  cpc_finish_instruction(&cpc);
  TEST_CHECK(snapshot_save(&cpc, bytes, sizeof bytes, &problem));
}

static void a_snapshot_is_refused_unless_it_is_one(void) {
  power_on(&cpc, ram, sizeof ram);
  const char *problem = NULL;
  static const uint8_t rubbish[SNAPSHOT_HEADER_SIZE + 16] = {0};
  TEST_CHECK(!snapshot_load(&cpc, rubbish, sizeof rubbish, &problem));
  TEST_CHECK(problem != NULL);

  memcpy(bytes, "MV - SNA", 8);
  memset(bytes + 8, 0, SNAPSHOT_HEADER_SIZE - 8);
  bytes[0x10] = 1;
  TEST_CHECK(!snapshot_load(&cpc, bytes, SNAPSHOT_HEADER_SIZE, &problem));
  TEST_CHECK(!snapshot_load(&cpc, bytes, 4, &problem));

  bytes[0x10] = 9; /* a version from the future */
  bytes[0x6B] = 64;
  TEST_CHECK(!snapshot_load(&cpc, bytes, sizeof bytes, &problem));
}

static void a_machine_survives_the_round_trip(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  /* Give every part of the machine something distinctive to remember. */
  run(&cpc, 40000);
  cpc.cpu.ixh = 0xAB;
  cpc.cpu.iyl = 0xCD;
  cpc.cpu.af_ = 0x1234;
  cpc.cpu.bc_ = 0x5678;
  cpc.cpu.de_ = 0x9ABC;
  cpc.cpu.hl_ = 0xDEF0;
  cpc.cpu.i = 0x37;
  cpc.cpu.im = 2;
  cpc.cpu.iff1 = true;
  cpc.cpu.iff2 = false;
  gate_array_write(&cpc.gate_array, 0x10);      /* the border... */
  gate_array_write(&cpc.gate_array, 0x40 | 26); /* ...goes white */
  gate_array_write(&cpc.gate_array, 0x05);      /* pen 5... */
  gate_array_write(&cpc.gate_array, 0x40 | 11); /* ...goes bright white */
  cpc.psg.selected = 7;
  cpc.psg.registers[7] = 0x3F;
  cpc.crtc.address_register = 12;

  const char *problem = NULL;
  TEST_CHECK(snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  TEST_EQUAL(snapshot_size(&cpc), SNAPSHOT_HEADER_SIZE + 0x20000);

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(snapshot_load(&restored, bytes, sizeof bytes, &problem));

  TEST_EQUAL(restored.cpu.a, cpc.cpu.a);
  TEST_EQUAL(restored.cpu.f, cpc.cpu.f);
  TEST_EQUAL(restored.cpu.b, cpc.cpu.b);
  TEST_EQUAL(restored.cpu.c, cpc.cpu.c);
  TEST_EQUAL(restored.cpu.d, cpc.cpu.d);
  TEST_EQUAL(restored.cpu.e, cpc.cpu.e);
  TEST_EQUAL(restored.cpu.h, cpc.cpu.h);
  TEST_EQUAL(restored.cpu.l, cpc.cpu.l);
  TEST_EQUAL(restored.cpu.sp, cpc.cpu.sp);
  TEST_EQUAL(restored.cpu.pc, cpc.cpu.pc);
  TEST_EQUAL(restored.cpu.i, cpc.cpu.i);
  TEST_EQUAL(restored.cpu.im, cpc.cpu.im);
  TEST_EQUAL(restored.cpu.ixh, cpc.cpu.ixh);
  TEST_EQUAL(restored.cpu.iyl, cpc.cpu.iyl);
  TEST_EQUAL(restored.cpu.af_, cpc.cpu.af_);
  TEST_EQUAL(restored.cpu.bc_, cpc.cpu.bc_);
  TEST_EQUAL(restored.cpu.de_, cpc.cpu.de_);
  TEST_EQUAL(restored.cpu.hl_, cpc.cpu.hl_);
  TEST_CHECK(restored.cpu.iff1 == cpc.cpu.iff1);
  TEST_CHECK(restored.cpu.iff2 == cpc.cpu.iff2);

  TEST_EQUAL(restored.gate_array.pen, cpc.gate_array.pen);
  TEST_EQUAL(restored.gate_array.inks[5], cpc.gate_array.inks[5]);
  TEST_EQUAL(restored.gate_array.inks[16], cpc.gate_array.inks[16]);
  TEST_EQUAL(restored.gate_array.mode, cpc.gate_array.mode);
  TEST_CHECK(restored.gate_array.lower_rom_enabled == cpc.gate_array.lower_rom_enabled);
  TEST_CHECK(restored.gate_array.upper_rom_enabled == cpc.gate_array.upper_rom_enabled);
  TEST_EQUAL(restored.mmr, cpc.mmr);
  TEST_EQUAL(restored.upper_rom_number, cpc.upper_rom_number);

  TEST_EQUAL(restored.crtc.address_register, cpc.crtc.address_register);
  for (int index = 0; index < 18; index++) {
    TEST_EQUAL(restored.crtc.registers[index], cpc.crtc.registers[index]);
  }
  TEST_EQUAL(restored.psg.selected, cpc.psg.selected);
  TEST_EQUAL(restored.psg.registers[7], cpc.psg.registers[7]);
  TEST_EQUAL(restored.ppi.control, cpc.ppi.control);

  if (memcmp(restored.ram, cpc.ram, 0x20000) != 0) {
    TEST_FAIL("the RAM came back different");
  }
}

/* Two machines restored from one snapshot must go on computing the same
   thing, for as long as anyone cares to run them.

   Note what is *not* claimed: that a restored machine agrees tick for tick
   with the one it was taken from. Version 1 carries the CRTC's registers
   but none of its counters, nor the Gate Array's interrupt counter, so the
   restored machine's interrupts fall elsewhere and the two part company —
   measured at 4440 T-states for a machine restored at the firmware prompt.
   Hand those counters over as well and the two run identically for as long
   as anyone cares to watch, which is what version 3 has room to record. */
static void machines_restored_from_one_snapshot_agree(void) {
  static cpc_t twin;
  static uint8_t twin_ram[0x20000];
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  run(&cpc, 40000);
  uint8_t counter_when_saved = cpc.ram[0x9000];
  TEST_CHECK(counter_when_saved != 0); /* or the program never ran */

  const char *problem = NULL;
  TEST_CHECK(snapshot_save(&cpc, bytes, sizeof bytes, &problem));

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(snapshot_load(&restored, bytes, sizeof bytes, &problem));
  power_on(&twin, twin_ram, sizeof twin_ram);
  TEST_CHECK(snapshot_load(&twin, bytes, sizeof bytes, &problem));

  /* The restored machine picks up the count where it was left. */
  TEST_EQUAL(restored.ram[0x9000], counter_when_saved);

  run(&restored, 20000);
  run(&twin, 20000);
  TEST_EQUAL(twin.cpu.a, restored.cpu.a);
  TEST_EQUAL(twin.cpu.pc, restored.cpu.pc);
  TEST_EQUAL(twin.ram[0x9000], restored.ram[0x9000]);
  TEST_CHECK(restored.ram[0x9000] != counter_when_saved); /* it kept going */
}

/* A snapshot restores the memory map, not just the registers behind it. */
static void the_memory_map_comes_back_with_it(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  memset(ram, 0, sizeof ram);
  ram[0x4000] = 0x11;                      /* bank 1, normally at &4000 */
  ram[(size_t)7 * 0x4000] = 0x77;          /* bank 7 */
  cpc.mmr = 0xC7;                          /* configuration 7 puts bank 7 there */
  gate_array_write(&cpc.gate_array, 0x8C); /* both ROMs off */
  cpc_remap(&cpc);
  TEST_EQUAL(cpc_peek(&cpc, 0x4000), 0x77);

  const char *problem = NULL;
  TEST_CHECK(snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.mmr, 0xC7);
  TEST_EQUAL(cpc_peek(&restored, 0x4000), 0x77);
  TEST_EQUAL(cpc_peek(&restored, 0x0000), 0x00); /* the ROM stayed off */
}

static void a_64k_snapshot_loads_into_a_64k_machine(void) {
  load_a_running_program();
  power_on(&cpc, ram, 0x10000);
  run(&cpc, 40000);
  const char *problem = NULL;
  TEST_CHECK(snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  TEST_EQUAL(snapshot_size(&cpc), SNAPSHOT_HEADER_SIZE + 0x10000);
  TEST_EQUAL(bytes[0x6B], 64);

  power_on(&restored, other_ram, 0x10000);
  TEST_CHECK(snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.cpu.pc, cpc.cpu.pc);

  /* And a 128K snapshot will not fit in it. */
  power_on(&cpc, ram, sizeof ram);
  TEST_CHECK(snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  power_on(&restored, other_ram, 0x10000);
  TEST_CHECK(!snapshot_load(&restored, bytes, sizeof bytes, &problem));
}

int main(void) {
  TEST_RUN(a_snapshot_is_refused_unless_it_is_one);
  TEST_RUN(a_machine_survives_the_round_trip);
  TEST_RUN(a_half_done_instruction_cannot_be_saved);
  TEST_RUN(machines_restored_from_one_snapshot_agree);
  TEST_RUN(the_memory_map_comes_back_with_it);
  TEST_RUN(a_64k_snapshot_loads_into_a_64k_machine);
  return TEST_REPORT("snapshot");
}
