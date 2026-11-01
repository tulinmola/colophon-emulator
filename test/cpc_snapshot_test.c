/*
 * cpc_snapshot_test — a CPC written down and picked up again.
 *
 * The round trip is the strong test here: every field the format carries
 * has to survive being written and read, and a machine restored from its
 * own snapshot has to go on computing the same thing. A reader alone could
 * agree with a writer that was wrong in the same place; a reader that is
 * asked to resume a running program cannot.
 */
#include <stdlib.h>
#include <string.h>

#include "cpc_snapshot.h"
#include "test.h"

static uint8_t ram[0x20000];
static uint8_t other_ram[0x20000];
static uint8_t lower_rom[0x4000];
static uint8_t upper_rom[0x4000];
static uint8_t bytes[CPC_SNAPSHOT_HEADER_SIZE + 0x20000];
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
  TEST_CHECK(!cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  cpc_finish_instruction(&cpc);
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
}

static void a_snapshot_is_refused_unless_it_is_one(void) {
  power_on(&cpc, ram, sizeof ram);
  const char *problem = NULL;
  static const uint8_t rubbish[CPC_SNAPSHOT_HEADER_SIZE + 16] = {0};
  TEST_CHECK(!cpc_snapshot_load(&cpc, rubbish, sizeof rubbish, &problem));
  TEST_CHECK(problem != NULL);

  memcpy(bytes, "MV - SNA", 8);
  memset(bytes + 8, 0, CPC_SNAPSHOT_HEADER_SIZE - 8);
  bytes[0x10] = 1;
  TEST_CHECK(!cpc_snapshot_load(&cpc, bytes, CPC_SNAPSHOT_HEADER_SIZE, &problem));
  TEST_CHECK(!cpc_snapshot_load(&cpc, bytes, 4, &problem));

  bytes[0x10] = 9; /* a version from the future */
  bytes[0x6B] = 64;
  TEST_CHECK(!cpc_snapshot_load(&cpc, bytes, sizeof bytes, &problem));
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
  cpc.psg.chip_selected = true;
  cpc.psg.registers[7] = 0x3F;
  cpc.crtc.address_register = 12;

  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  TEST_EQUAL(cpc_snapshot_size(&cpc), CPC_SNAPSHOT_HEADER_SIZE + 0x20000);

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));

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
  TEST_CHECK(restored.psg.chip_selected == cpc.psg.chip_selected);
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
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  power_on(&twin, twin_ram, sizeof twin_ram);
  TEST_CHECK(cpc_snapshot_load(&twin, bytes, sizeof bytes, &problem));

  /* The restored machine picks up the count where it was left. */
  TEST_EQUAL(restored.ram[0x9000], counter_when_saved);

  run(&restored, 20000);
  run(&twin, 20000);
  TEST_EQUAL(twin.cpu.a, restored.cpu.a);
  TEST_EQUAL(twin.cpu.pc, restored.cpu.pc);
  TEST_EQUAL(twin.ram[0x9000], restored.ram[0x9000]);
  TEST_CHECK(restored.ram[0x9000] != counter_when_saved); /* it kept going */
}

/* A snapshot cannot install a register value the chip could not hold. The
   restore goes in through the CRTC's own bus, so the writable-bit masks
   apply exactly as they do to a write from the CPU — and the register the
   snapshot had selected survives being set aside to do it. */
static void the_crtc_keeps_only_the_bits_it_has(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  bytes[0x42] = 9;         /* the selected register */
  bytes[0x43 + 4] = 0xFF;  /* R4 is seven bits wide */
  bytes[0x43 + 8] = 0xFF;  /* R8's writable bits have a hole in them */
  bytes[0x43 + 16] = 0xAA; /* R16 is a lightpen latch, read-only */

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.crtc.registers[4], 0x7F);
  TEST_EQUAL(restored.crtc.registers[8], 0xF3);
  TEST_EQUAL(restored.crtc.registers[16], 0x00);
  TEST_EQUAL(restored.crtc.address_register, 9);
}

/* The format holds the sound chip's registers and not its generators. A
   load starts them as reset does, whatever the machine held, so that two
   machines restored from one snapshot sound alike; and a single decay that
   had ended long before the snapshot is not heard again. */
static void a_snapshot_starts_the_sound_afresh_and_lets_an_ended_envelope_rest(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  static const uint8_t writes[][2] = {{7, 0x3F}, {8, 0x10}, {11, 1}, {12, 0}, {13, 0x00}};
  for (size_t index = 0; index < sizeof writes / sizeof writes[0]; index++) {
    ay38910_access(&cpc.psg, AY38910_SELECT, writes[index][0]);
    ay38910_access(&cpc.psg, AY38910_WRITE, writes[index][1]);
  }
  run(&cpc, 4L * 16 * 16 * 2);
  TEST_EQUAL(cpc.psg.output[0], 0);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));

  power_on(&restored, other_ram, sizeof other_ram);
  run(&restored, 12345);
  restored.psg.tone_counter[1] = 777;
  restored.psg.noise_register = 0x1234;
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.psg.output[0], 0);
  TEST_EQUAL(restored.psg.divider, 0);
  TEST_EQUAL(restored.psg.tone_counter[1], 0);
  TEST_EQUAL(restored.psg.noise_register, 0);
  for (int clock = 0; clock < 1000; clock++) {
    ay38910_tick(&restored.psg);
    TEST_EQUAL(restored.psg.output[0], 0);
  }
}

/* Each shape that ends or holds comes back from a snapshot at the level it
   rested at and stays there; one that repeats moves again. */
static void a_resting_envelope_rests_after_a_snapshot(void) {
  static const uint8_t shapes[] = {0x00, 0x04, 0x09, 0x0B, 0x0D, 0x0F, 0x0A};
  for (size_t index = 0; index < sizeof shapes / sizeof shapes[0]; index++) {
    load_a_running_program();
    power_on(&cpc, ram, sizeof ram);
    const uint8_t writes[][2] = {{7, 0x3F}, {8, 0x10}, {11, 1}, {12, 0}, {13, shapes[index]}};
    for (size_t write = 0; write < sizeof writes / sizeof writes[0]; write++) {
      ay38910_access(&cpc.psg, AY38910_SELECT, writes[write][0]);
      ay38910_access(&cpc.psg, AY38910_WRITE, writes[write][1]);
    }
    run(&cpc, 4L * 16 * 16 * 3);
    uint8_t rested = cpc.psg.output[0];
    const char *problem = NULL;
    TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
    power_on(&restored, other_ram, sizeof other_ram);
    TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
    bool repeats = shapes[index] == 0x0A;
    bool moved = false;
    for (int clock = 0; clock < 1000; clock++) {
      ay38910_tick(&restored.psg);
      moved = moved || restored.psg.output[0] != rested;
    }
    TEST_CHECK(moved == repeats);
  }
}

/* The PSG's registers go in through its own bus as the 6845's do, so its
   writable-bit masks apply here too. */
static void the_psg_keeps_only_the_bits_it_has(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  bytes[0x5A] = 6;
  bytes[0x5B + 1] = 0xFF;  /* the high half of a tone period: four bits */
  bytes[0x5B + 6] = 0xFF;  /* the noise period: five */
  bytes[0x5B + 13] = 0xFF; /* the envelope shape: four */
  bytes[0x5B + 14] = 0xA5; /* port A's output latch: all eight */

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.psg.registers[1], 0x0F);
  TEST_EQUAL(restored.psg.registers[6], 0x1F);
  TEST_EQUAL(restored.psg.registers[13], 0x0F);
  TEST_EQUAL(restored.psg.registers[14], 0xA5);
  TEST_EQUAL(restored.psg.selected, 6);
  TEST_CHECK(restored.psg.chip_selected);
}

/* A PSG left unselected by an address above 15 is written as the register
   it last selected, plus 16 (cpc_snapshot.h), and comes back unselected
   with that register latched. */
static void an_unselected_psg_survives_the_round_trip(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  ay38910_access(&cpc.psg, AY38910_SELECT, 9);
  ay38910_access(&cpc.psg, AY38910_SELECT, 0x13);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x5A], 0x19);

  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_CHECK(!restored.psg.chip_selected);
  TEST_EQUAL(restored.psg.selected, 9);
}

/* CPCEC writes the whole latched byte at &5A (cpc_snapshot.h), so any byte
   above 15 is read as a chip left unselected. */
static void a_psg_address_above_fifteen_loads_unselected(void) {
  load_a_running_program();
  power_on(&cpc, ram, sizeof ram);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  for (int address = 0; address < 256; address++) {
    bytes[0x5A] = (uint8_t)address;
    power_on(&restored, other_ram, sizeof other_ram);
    TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
    TEST_CHECK(restored.psg.chip_selected == (address < 16));
    TEST_EQUAL(restored.psg.selected, address & 0x0F);
  }
}

/* Runs a program from the lower ROM, saves the machine, restores it into
   the other, and runs that as long again. */
static void save_and_restore_running(const uint8_t *program, size_t length) {
  memset(lower_rom, 0, sizeof lower_rom);
  memcpy(lower_rom, program, length);
  power_on(&cpc, ram, sizeof ram);
  run(&cpc, 2000);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  run(&restored, 2000);
}

/* A machine saved while port C still gives the PSG a function goes on
   giving it, at every access to the PPI, what port A holds. Here port C
   gives SELECT with &13 on port A, after register 5 was selected, and a
   loop reads port B. */
static void a_psg_left_unselected_mid_select_stays_so(void) {
  static const uint8_t program[] = {
      0xF3,                         /* DI */
      0x01, 0x82, 0xF7, 0xED, 0x49, /* port A an output */
      0x01, 0x05, 0xF4, 0xED, 0x49, /* register 5 on port A... */
      0x01, 0xC0, 0xF6, 0xED, 0x49, /* ...selected */
      0x01, 0x00, 0xF6, 0xED, 0x49, /* inactive */
      0x01, 0x13, 0xF4, 0xED, 0x49, /* &13 on port A... */
      0x01, 0xC0, 0xF6, 0xED, 0x49, /* ...and SELECT left standing */
      0x06, 0xF5,                   /* LD B,&F5 */
      0xED, 0x78,                   /* IN A,(C) */
      0x18, 0xFC,                   /* JR back to the IN */
  };
  save_and_restore_running(program, sizeof program);
  TEST_CHECK(!cpc.psg.chip_selected);
  TEST_CHECK(!restored.psg.chip_selected);
  TEST_EQUAL(restored.psg.selected, 5);
}

/* The same with WRITE left standing and &55 on port A: register 0 is given
   &55 again at each access, before the snapshot and after it. */
static void a_psg_mid_write_is_written_the_same_byte(void) {
  static const uint8_t program[] = {
      0xF3,                         /* DI */
      0x01, 0x82, 0xF7, 0xED, 0x49, /* port A an output */
      0x01, 0x00, 0xF4, 0xED, 0x49, /* register 0 on port A... */
      0x01, 0xC0, 0xF6, 0xED, 0x49, /* ...selected */
      0x01, 0x00, 0xF6, 0xED, 0x49, /* inactive */
      0x01, 0x55, 0xF4, 0xED, 0x49, /* &55 on port A... */
      0x01, 0x80, 0xF6, 0xED, 0x49, /* ...and WRITE left standing */
      0x06, 0xF5,                   /* LD B,&F5 */
      0xED, 0x78,                   /* IN A,(C) */
      0x18, 0xFC,                   /* JR back to the IN */
  };
  save_and_restore_running(program, sizeof program);
  TEST_EQUAL(cpc.psg.registers[0], 0x55);
  TEST_EQUAL(restored.psg.registers[0], 0x55);
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
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  power_on(&restored, other_ram, sizeof other_ram);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.mmr, 0xC7);
  TEST_EQUAL(cpc_peek(&restored, 0x4000), 0x77);
  TEST_EQUAL(cpc_peek(&restored, 0x0000), 0x00); /* the ROM stayed off */
}

static void a_64k_snapshot_loads_into_a_64k_machine(void) {
  load_a_running_program();
  power_on(&cpc, ram, 0x10000);
  run(&cpc, 40000);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  TEST_EQUAL(cpc_snapshot_size(&cpc), CPC_SNAPSHOT_HEADER_SIZE + 0x10000);
  TEST_EQUAL(bytes[0x6B], 64);

  power_on(&restored, other_ram, 0x10000);
  TEST_CHECK(cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
  TEST_EQUAL(restored.cpu.pc, cpc.cpu.pc);

  /* And a 128K snapshot will not fit in it. */
  power_on(&cpc, ram, sizeof ram);
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  power_on(&restored, other_ram, 0x10000);
  TEST_CHECK(!cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
}

/* Without a PAL, a banking command dies on the empty socket, so a snapshot
   that records one was taken on a machine of another shape. It is refused,
   and the machine it was offered to is left as it was. */
static void a_64k_machine_refuses_a_banked_snapshot(void) {
  load_a_running_program();
  power_on(&cpc, ram, 0x10000);
  run(&cpc, 40000);
  const char *problem = NULL;
  TEST_CHECK(cpc_snapshot_save(&cpc, bytes, sizeof bytes, &problem));
  power_on(&restored, other_ram, 0x10000);
  TEST_CHECK(restored.cpu.pc != cpc.cpu.pc); /* a half-done load would show */

  /* Configurations 1-7 each map a bank above 3; &08 maps nothing new, but
     selects page 1, which no 64K machine can latch. */
  static const uint8_t banked[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  for (size_t index = 0; index < sizeof banked; index++) {
    bytes[0x41] = banked[index];
    problem = NULL;
    TEST_CHECK(!cpc_snapshot_load(&restored, bytes, sizeof bytes, &problem));
    TEST_CHECK(problem != NULL);
  }
  TEST_EQUAL(restored.cpu.pc, 0x0000);
  TEST_EQUAL(restored.mmr, 0xC0);
}

int main(void) {
  TEST_RUN(a_snapshot_is_refused_unless_it_is_one);
  TEST_RUN(a_machine_survives_the_round_trip);
  TEST_RUN(a_half_done_instruction_cannot_be_saved);
  TEST_RUN(machines_restored_from_one_snapshot_agree);
  TEST_RUN(the_crtc_keeps_only_the_bits_it_has);
  TEST_RUN(the_psg_keeps_only_the_bits_it_has);
  TEST_RUN(a_snapshot_starts_the_sound_afresh_and_lets_an_ended_envelope_rest);
  TEST_RUN(a_resting_envelope_rests_after_a_snapshot);
  TEST_RUN(an_unselected_psg_survives_the_round_trip);
  TEST_RUN(a_psg_address_above_fifteen_loads_unselected);
  TEST_RUN(a_psg_left_unselected_mid_select_stays_so);
  TEST_RUN(a_psg_mid_write_is_written_the_same_byte);
  TEST_RUN(the_memory_map_comes_back_with_it);
  TEST_RUN(a_64k_snapshot_loads_into_a_64k_machine);
  TEST_RUN(a_64k_machine_refuses_a_banked_snapshot);
  return TEST_REPORT("cpc snapshot");
}
