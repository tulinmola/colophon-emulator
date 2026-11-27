/*
 * spectrum_snapshot_test — a machine written out and read back.
 *
 * The round trip is the test that matters: a machine caught mid-flight,
 * written, loaded into a second machine, and the two run on side by side to
 * see whether they stay together. What the format cannot carry shows up as
 * the two machines parting company.
 */
#include <string.h>

#include "spectrum_snapshot.h"
#include "test.h"

static spectrum_t saved;
static spectrum_t restored;
static uint8_t rom[SPECTRUM_PLUS2_ROM_SIZE]; /* room for either model's */
static uint8_t saved_ram[SPECTRUM_RAM_48K];
static uint8_t restored_ram[SPECTRUM_RAM_48K];
static uint8_t bytes[SPECTRUM_SNAPSHOT_48K_SIZE];

/* Where the program below counts its interrupts, as an index into the RAM. */
#define COUNTER (0x8000 - SPECTRUM_RAM_BASE)

/* A program in the ROM that counts frames into RAM, so a resumed machine has
   something to keep doing and somewhere for the difference to show. */
static void power_on(spectrum_t *machine, uint8_t *ram, uint32_t ram_size) {
  static const uint8_t program[] = {
      0xED, 0x56,             /* IM 1        */
      0x21, 0x00, 0x80,       /* LD HL,&8000 */
      0x01, 0x34, 0x12,       /* LD BC,&1234 */
      0x11, 0x78, 0x56,       /* LD DE,&5678 */
      0xDD, 0x21, 0xAA, 0xBB, /* LD IX,&BBAA */
      0xFD, 0x21, 0xCC, 0xDD, /* LD IY,&DDCC */
      0xD9,                   /* EXX         */
      0x21, 0x11, 0x22,       /* LD HL,&2211 */
      0xD9,                   /* EXX         */
      0x3E, 0x02, 0xD3, 0xFE, /* LD A,2 : OUT (&FE),A — border 2 */
      0xFB,                   /* EI          */
      0x76,                   /* HALT        */
      0x18, 0xFD,             /* JR -3       */
  };
  static const uint8_t handler[] = {0x34, 0xFB, 0xED, 0x4D}; /* INC (HL) : EI : RETI */
  memset(rom, 0x00, sizeof rom);
  memcpy(rom, program, sizeof program);
  memcpy(rom + 0x0038, handler, sizeof handler);
  memset(ram, 0x00, ram_size);
  spectrum_init(machine, SPECTRUM_48K, ram, ram_size, rom);
}

static void run(spectrum_t *machine, long frames) {
  for (long frame = 0; frame < frames; frame++) {
    for (uint32_t tick = 0; tick < machine->ula.frame.ticks_per_frame; tick++) {
      spectrum_tick(machine);
    }
  }
}

/* Stop the machine where the program waits rather than wherever the frame
   count happens to land, so that a snapshot taken here has no handler in
   flight and the arithmetic afterwards is exact. */
static void run_to_the_wait_loop(spectrum_t *machine, long frames) {
  run(machine, frames);
  for (uint32_t guard = 0; guard < 2 * machine->ula.frame.ticks_per_frame; guard++) {
    if (machine->cpu.halted && z80_instruction_complete(&machine->cpu)) {
      return;
    }
    spectrum_tick(machine);
  }
}

static void a_48k_snapshot_is_the_one_length_it_can_be(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  TEST_EQUAL(SPECTRUM_SNAPSHOT_48K_SIZE, 49179);
  TEST_EQUAL(spectrum_snapshot_size(&saved), 49179);

  const char *problem = NULL;
  TEST_CHECK(!spectrum_snapshot_load(&saved, bytes, SPECTRUM_SNAPSHOT_48K_SIZE - 1, &problem));
  TEST_CHECK(problem != NULL);
  TEST_CHECK(!spectrum_snapshot_load(&saved, bytes, SPECTRUM_SNAPSHOT_48K_SIZE + 1, &problem));

  /* A 16K machine is not one of these, in either direction. */
  static uint8_t small[SPECTRUM_RAM_16K];
  power_on(&saved, small, SPECTRUM_RAM_16K);
  TEST_EQUAL(spectrum_snapshot_size(&saved), 0);
  TEST_CHECK(!spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  TEST_CHECK(!spectrum_snapshot_load(&saved, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));

  /* Nor is a +2, which has the 128K format instead and refuses this one;
     and a +2 handed no more RAM than a 48K fits neither. */
  static uint8_t banks[SPECTRUM_RAM_128K];
  spectrum_init(&saved, SPECTRUM_PLUS2, banks, SPECTRUM_RAM_128K, rom);
  TEST_EQUAL(spectrum_snapshot_size(&saved), SPECTRUM_SNAPSHOT_128K_SIZE);
  TEST_CHECK(!spectrum_snapshot_load(&saved, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  spectrum_init(&saved, SPECTRUM_PLUS2, banks, SPECTRUM_RAM_48K, rom);
  TEST_EQUAL(spectrum_snapshot_size(&saved), 0);
  TEST_CHECK(!spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
}

static void a_half_done_instruction_cannot_be_saved(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  spectrum_tick(&saved); /* one T-state in, so the fetch is unfinished */
  TEST_CHECK(!z80_instruction_complete(&saved.cpu));
  const char *problem = NULL;
  TEST_CHECK(!spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  spectrum_finish_instruction(&saved);
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
}

/* And neither can a machine whose processor has finished while the ULA still
   holds its clock: the format has nowhere to put the T-states still owed, so
   a snapshot taken there would resume a machine that had skipped them. */
static void a_hold_still_owed_cannot_be_saved_either(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  /* LDI reading from outside the screen and writing into it, arranged so its
     last internal T-state lands where the chip owes six. */
  static const uint8_t block[] = {0xED, 0xA0};
  memcpy(rom + 0x0100, block, sizeof block);
  saved.cpu.pc = 0x0100;
  saved.cpu.h = 0x80;
  saved.cpu.d = 0x40;
  ula_seek(&saved.ula, 14321);
  do {
    spectrum_tick(&saved);
  } while (!z80_instruction_complete(&saved.cpu));

  TEST_EQUAL(saved.held_ticks, 6);
  TEST_CHECK(!spectrum_instruction_complete(&saved));
  const char *problem = NULL;
  TEST_CHECK(!spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  TEST_CHECK(problem != NULL && strstr(problem, "holding the clock") != NULL);

  spectrum_finish_instruction(&saved);
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
}

/* The format has nowhere to put a hold in progress, so a machine given a
   snapshot stops serving whatever the machine it replaced was serving. */
static void a_hold_does_not_survive_a_load(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  /* Stand the other machine in the middle of a charged access first. */
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  rom[0x0100] = 0x34; /* INC (HL), with HL in the screen */
  restored.cpu.pc = 0x0100;
  restored.cpu.h = 0x40;
  restored.cpu.l = 0x00;
  ula_seek(&restored.ula, 14331);
  for (uint32_t guard = 0; guard < restored.ula.frame.ticks_per_frame && restored.held_ticks == 0;
       guard++) {
    spectrum_tick(&restored);
  }
  TEST_CHECK(restored.held_ticks > 0);

  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_EQUAL(restored.held_ticks, 0);
}

static void a_machine_survives_the_round_trip(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);

  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));

  TEST_EQUAL(restored.cpu.a, saved.cpu.a);
  TEST_EQUAL(restored.cpu.f, saved.cpu.f);
  TEST_EQUAL((restored.cpu.b << 8) | restored.cpu.c, 0x1234);
  TEST_EQUAL((restored.cpu.d << 8) | restored.cpu.e, 0x5678);
  TEST_EQUAL((restored.cpu.ixh << 8) | restored.cpu.ixl, 0xBBAA);
  TEST_EQUAL((restored.cpu.iyh << 8) | restored.cpu.iyl, 0xDDCC);
  TEST_EQUAL(restored.cpu.hl_, 0x2211);
  TEST_EQUAL(restored.cpu.pc, saved.cpu.pc);
  TEST_EQUAL(restored.cpu.sp, saved.cpu.sp); /* the push is undone by the pop */
  TEST_EQUAL(restored.cpu.i, saved.cpu.i);
  TEST_EQUAL(restored.cpu.r, saved.cpu.r);
  TEST_EQUAL(restored.cpu.im, saved.cpu.im);
  TEST_CHECK(restored.cpu.iff1 == saved.cpu.iff1);
  TEST_CHECK(restored.cpu.iff2 == saved.cpu.iff2);
  TEST_EQUAL(restored.ula.border, 2);

  /* The RAM comes back whole but for the two bytes the format spends on the
     program counter, which is the defect and not a mistake. */
  uint32_t spent = (uint32_t)(saved.cpu.sp - 2 - SPECTRUM_RAM_BASE);
  int differences = 0;
  for (uint32_t at = 0; at < SPECTRUM_RAM_48K; at++) {
    if (restored_ram[at] != saved_ram[at]) {
      differences++;
      TEST_CHECK(at == spent || at == spent + 1);
    }
  }
  TEST_CHECK(differences <= 2);
}

/* What the format does not carry is where the beam was. A restored machine
   picks the program up exactly, and begins its frame again from the top —
   so the two are not in step and cannot be run side by side. What can be
   asked of the restored one is that it goes on doing the work. */
static void a_restored_machine_goes_on_running_from_a_fresh_frame(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  TEST_CHECK(saved.ula.frame_tick != 0); /* caught mid-frame */
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  for (int tick = 0; tick < 1000; tick++) {
    spectrum_tick(&restored);
  }
  TEST_EQUAL(restored.ula.frame_tick, 1000); /* loaded into a machine mid-frame */
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_EQUAL(restored.ula.frame_tick, 0); /* and set down at the top of one */

  uint8_t counted = restored_ram[COUNTER];
  TEST_CHECK(counted != 0); /* the original had been counting */
  run(&restored, 5);
  TEST_EQUAL(restored_ram[COUNTER], counted + 5); /* one interrupt a frame, still */
}

/* A machine loaded while it runs draws from then on as one loaded just after
   it was switched on, wherever in its frame the load finds it. Here that is
   just after the frame's sync has ended and before the next line's sync,
   where a monitor left as it was takes the new frame's sync for the rest of
   the old one and paints the whole first frame a line low; and seventeen
   frames in, where the count FLASH keeps time by would carry over. */
static spectrum_t running;
static uint8_t running_ram[SPECTRUM_RAM_48K];
static uint8_t running_picture[SPECTRUM_LARGEST_FRAMEBUFFER];
static uint8_t fresh_picture[SPECTRUM_LARGEST_FRAMEBUFFER];

static void a_load_draws_as_on_a_machine_just_switched_on(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  power_on(&running, running_ram, SPECTRUM_RAM_48K);
  spectrum_connect_monitor(&running, running_picture);
  run(&running, 17);
  while (running.ula.frame_tick != 1843) {
    spectrum_tick(&running);
  }
  TEST_CHECK(spectrum_snapshot_load(&running, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_EQUAL(running.ula.frame_count, 0);

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  memcpy(fresh_picture, running_picture, sizeof fresh_picture); /* the same leftovers */
  spectrum_connect_monitor(&restored, fresh_picture);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));

  run(&running, 1);
  run(&restored, 1);
  TEST_CHECK(memcmp(running_picture, fresh_picture, sizeof fresh_picture) == 0);
}

/* The format's own defect, declared rather than hidden: the snapshot spends
   the two bytes below the stack pointer on the program counter, and the
   machine it describes is not touched. */
static void the_program_counter_costs_two_bytes_of_the_snapshot(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  uint16_t sp = saved.cpu.sp;
  uint16_t pc = saved.cpu.pc;
  uint8_t below[2] = {spectrum_peek(&saved, (uint16_t)(sp - 2)),
                      spectrum_peek(&saved, (uint16_t)(sp - 1))};

  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  /* The machine is as it was. */
  TEST_EQUAL(saved.cpu.sp, sp);
  TEST_EQUAL(saved.cpu.pc, pc);
  TEST_EQUAL(spectrum_peek(&saved, (uint16_t)(sp - 2)), below[0]);
  TEST_EQUAL(spectrum_peek(&saved, (uint16_t)(sp - 1)), below[1]);

  /* The snapshot is not: its stack pointer is two lower, and where those
     bytes were it holds the program counter. */
  uint32_t at = 0x1B + (uint32_t)(sp - 2 - SPECTRUM_RAM_BASE);
  TEST_EQUAL(bytes[0x17] | (bytes[0x18] << 8), (uint16_t)(sp - 2));
  TEST_EQUAL(bytes[at], (uint8_t)pc);
  TEST_EQUAL(bytes[at + 1], (uint8_t)(pc >> 8));

  /* And loading pops them straight back off. */
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_EQUAL(restored.cpu.sp, sp);
  TEST_EQUAL(restored.cpu.pc, pc);
}

/* A stack with no room beneath it cannot be written down at all. */
static void a_stack_at_the_bottom_of_memory_cannot_be_written(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  spectrum_finish_instruction(&saved);
  const char *problem = NULL;
  saved.cpu.sp = SPECTRUM_RAM_BASE + 1;
  TEST_CHECK(!spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  saved.cpu.sp = SPECTRUM_RAM_BASE + 2;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  /* With SP at 0x0000 the push wraps round to 0xFFFF and 0xFFFE, which are
     RAM; at 0x0001 its high byte would land on the ROM at 0x0000. */
  saved.cpu.sp = 0x0001;
  TEST_CHECK(!spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  saved.cpu.sp = 0x0000;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_EQUAL(restored.cpu.sp, 0x0000);
  TEST_EQUAL(restored.cpu.pc, saved.cpu.pc);
}

/* Interrupts come back through RETN's own rule rather than from a field. */
static void the_header_carries_one_interrupt_flag_and_restores_both(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  TEST_CHECK(saved.cpu.iff2); /* halted in the wait loop, interrupts on */
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13] & 0x04, 0x04);

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_CHECK(restored.cpu.iff1);
  TEST_CHECK(restored.cpu.iff2);

  /* And a machine caught with interrupts disabled comes back disabled. */
  bytes[0x13] = 0x00;
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_CHECK(!restored.cpu.iff1);
  TEST_CHECK(!restored.cpu.iff2);
}

/* MAME takes IFF1 from bit 0 of the interrupt byte, which the format leaves
   undefined, so the flag is written there as well; reading still follows
   the format, and bit 0 alone enables nothing. */
static void the_interrupt_flag_is_written_where_mame_reads_it_too(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  spectrum_finish_instruction(&saved);
  const char *problem = NULL;
  saved.cpu.iff1 = saved.cpu.iff2 = true;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13], 0x05);

  /* After a non-maskable interrupt taken with interrupts enabled, and before
     its RETN, the two differ; bit 0 takes IFF2, as the RETN would, so MAME
     resumes what this reader resumes. */
  saved.cpu.iff1 = false;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13], 0x05);

  saved.cpu.iff1 = saved.cpu.iff2 = false;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13], 0x00);

  /* A snapshot written to the format, by another emulator or by this one
     before bit 0 was, carries bit 2 alone and resumes enabled. */
  bytes[0x13] = 0x04;
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_CHECK(restored.cpu.iff1);
  TEST_CHECK(restored.cpu.iff2);

  bytes[0x13] = 0x01;
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_CHECK(!restored.cpu.iff1);
  TEST_CHECK(!restored.cpu.iff2);
}

/* A round trip cannot catch a field written and read at the same wrong
   offset, so the header is checked against the published table instead: the
   offsets are transcribed a second time, from
   https://worldofspectrum.org/faq/reference/formats.htm. */
static void the_header_is_laid_out_as_the_format_says(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  spectrum_finish_instruction(&saved);
  z80_t *cpu = &saved.cpu;
  cpu->i = 0x01;
  cpu->hl_ = 0x0302;
  cpu->de_ = 0x0504;
  cpu->bc_ = 0x0706;
  cpu->af_ = 0x0908;
  cpu->h = 0x0B;
  cpu->l = 0x0A;
  cpu->d = 0x0D;
  cpu->e = 0x0C;
  cpu->b = 0x0F;
  cpu->c = 0x0E;
  cpu->iyh = 0x11;
  cpu->iyl = 0x10;
  cpu->ixh = 0x13;
  cpu->ixl = 0x12;
  cpu->iff2 = true;
  cpu->r = 0x15;
  cpu->a = 0x17;
  cpu->f = 0x16;
  cpu->sp = 0xC002;
  cpu->im = 2;
  ula_write(&saved.ula, 0x05);

  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, bytes, sizeof bytes, &problem));

  TEST_EQUAL(bytes[0x00], 0x01);                        /* I           */
  TEST_EQUAL(bytes[0x01] | (bytes[0x02] << 8), 0x0302); /* HL'         */
  TEST_EQUAL(bytes[0x03] | (bytes[0x04] << 8), 0x0504); /* DE'         */
  TEST_EQUAL(bytes[0x05] | (bytes[0x06] << 8), 0x0706); /* BC'         */
  TEST_EQUAL(bytes[0x07] | (bytes[0x08] << 8), 0x0908); /* AF'         */
  TEST_EQUAL(bytes[0x09] | (bytes[0x0A] << 8), 0x0B0A); /* HL          */
  TEST_EQUAL(bytes[0x0B] | (bytes[0x0C] << 8), 0x0D0C); /* DE          */
  TEST_EQUAL(bytes[0x0D] | (bytes[0x0E] << 8), 0x0F0E); /* BC          */
  TEST_EQUAL(bytes[0x0F] | (bytes[0x10] << 8), 0x1110); /* IY          */
  TEST_EQUAL(bytes[0x11] | (bytes[0x12] << 8), 0x1312); /* IX          */
  TEST_EQUAL(bytes[0x13] & 0x04, 0x04);                 /* IFF2, bit 2 */
  TEST_EQUAL(bytes[0x14], 0x15);                        /* R           */
  TEST_EQUAL(bytes[0x15] | (bytes[0x16] << 8), 0x1716); /* AF          */
  TEST_EQUAL(bytes[0x17] | (bytes[0x18] << 8), 0xC000); /* SP, pushed  */
  TEST_EQUAL(bytes[0x19], 2);                           /* IntMode     */
  TEST_EQUAL(bytes[0x1A], 5);                           /* BorderColor */
  TEST_EQUAL(0x1B + SPECTRUM_RAM_48K, SPECTRUM_SNAPSHOT_48K_SIZE);
}

/* A file is not trusted about where its stack was: the program counter has
   to have been pushed somewhere the machine can read it back from. */
static void a_stack_the_program_counter_could_not_reach_is_refused(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  spectrum_finish_instruction(&saved);
  const char *problem = NULL;
  memset(bytes, 0, sizeof bytes);

  const uint16_t impossible[] = {0x0000, 0x3FFF, SPECTRUM_RAM_BASE - 1, 0xFFFF};
  for (size_t index = 0; index < sizeof impossible / sizeof impossible[0]; index++) {
    bytes[0x17] = (uint8_t)impossible[index];
    bytes[0x18] = (uint8_t)(impossible[index] >> 8);
    problem = NULL;
    TEST_CHECK(!spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
    TEST_CHECK(problem != NULL);
  }

  /* The lowest one that works is the bottom of the RAM itself. */
  bytes[0x17] = (uint8_t)SPECTRUM_RAM_BASE;
  bytes[0x18] = (uint8_t)(SPECTRUM_RAM_BASE >> 8);
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
}

/* The border byte is three bits wide. The bits above it are the microphone
   and the loudspeaker on a live port, and a snapshot must not reach them. */
static void the_border_byte_cannot_drive_the_speaker(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  const char *problem = NULL;
  memset(bytes, 0, sizeof bytes);
  bytes[0x17] = (uint8_t)0x00; /* a stack at &C000 */
  bytes[0x18] = (uint8_t)0xC0;
  bytes[0x1A] = 0xFF;
  TEST_CHECK(spectrum_snapshot_load(&restored, bytes, SPECTRUM_SNAPSHOT_48K_SIZE, &problem));
  TEST_EQUAL(restored.ula.border, 7);
  TEST_CHECK(!restored.ula.microphone);
  TEST_CHECK(!restored.ula.speaker);
}

static uint8_t plus2_saved_ram[SPECTRUM_RAM_128K];
static uint8_t plus2_restored_ram[SPECTRUM_RAM_128K];
static uint8_t plus2_bytes[SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE];

/* A +2 running the same program from its first ROM, with every bank of its
   RAM filled with a pattern of its own so that a bank in the wrong place
   shows, and the bank register set as given. */
static void power_on_plus2_paging(spectrum_t *machine, uint8_t *ram, uint8_t bank_register) {
  power_on(machine, ram, SPECTRUM_RAM_48K); /* the ROM's program */
  for (size_t at = 0; at < SPECTRUM_RAM_128K; at++) {
    ram[at] = (uint8_t)(at / SPECTRUM_BANK_SIZE * 0x11 + at % 7);
  }
  spectrum_init(machine, SPECTRUM_PLUS2, ram, SPECTRUM_RAM_128K, rom);
  machine->bank_register = bank_register;
}

static void a_plus2_survives_the_round_trip_whole(void) {
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x03);
  run_to_the_wait_loop(&saved, 3);
  const char *problem = NULL;
  TEST_EQUAL(spectrum_snapshot_size(&saved), SPECTRUM_SNAPSHOT_128K_SIZE);
  TEST_CHECK(spectrum_snapshot_save(&saved, plus2_bytes, sizeof plus2_bytes, &problem));

  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  memset(plus2_restored_ram, 0xEE, sizeof plus2_restored_ram);
  /* A write to the register the machine was still to latch is not carried
     over the load, where it would latch whatever the bus held. */
  restored.latching_bank_register = true;
  restored.pins = z80_set_data(restored.pins, 0x10);
  TEST_CHECK(spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));
  TEST_EQUAL(restored.cpu.pc, saved.cpu.pc);
  TEST_EQUAL(restored.cpu.sp, saved.cpu.sp); /* nothing pushed: the PC has a field */
  TEST_EQUAL((restored.cpu.b << 8) | restored.cpu.c, 0x1234);
  TEST_EQUAL(restored.cpu.hl_, 0x2211);
  TEST_CHECK(restored.cpu.iff1 == saved.cpu.iff1);
  TEST_EQUAL(restored.bank_register, 0x03);
  TEST_EQUAL(restored.ula.border, 2);
  /* Every bank comes back where it was, and no byte is spent. */
  TEST_CHECK(memcmp(plus2_restored_ram, plus2_saved_ram, SPECTRUM_RAM_128K) == 0);
  spectrum_tick(&restored);
  TEST_EQUAL(restored.bank_register, 0x03);

  /* The banks are laid out as the format says: 5, 2 and the paged one, then
     the rest in ascending order. */
  const unsigned order[] = {5, 2, 3, 0, 1, 4, 6, 7};
  for (size_t index = 0; index < 8; index++) {
    size_t at = 27 + index * SPECTRUM_BANK_SIZE + (index >= 3 ? 4 : 0);
    if (memcmp(plus2_bytes + at, plus2_saved_ram + (size_t)order[index] * SPECTRUM_BANK_SIZE,
               SPECTRUM_BANK_SIZE) != 0) {
      TEST_FAIL("block %zu of the snapshot is not bank %u", index, order[index]);
    }
  }
  TEST_EQUAL(plus2_bytes[49181], 0x03);
  TEST_EQUAL(plus2_bytes[49182], 0);
  TEST_EQUAL(plus2_bytes[49179] | plus2_bytes[49180] << 8, saved.cpu.pc);
}

/* Paged at 0xC000, bank 5 or 2 is written a second time in third place, and
   the snapshot is a bank longer. A snapshot whose two copies differ
   contradicts itself, and is refused. */
static void a_plus2_paging_bank_5_or_2_holds_it_twice(void) {
  const uint8_t repeated[] = {5, 2};
  for (size_t index = 0; index < 2; index++) {
    const uint8_t paged = repeated[index];
    const size_t copy = 27 + (size_t)(paged == 5 ? 0 : 1) * SPECTRUM_BANK_SIZE;
    const size_t third = 27 + (size_t)2 * SPECTRUM_BANK_SIZE;
    power_on_plus2_paging(&saved, plus2_saved_ram, paged);
    run_to_the_wait_loop(&saved, 1);
    const char *problem = NULL;
    TEST_EQUAL(spectrum_snapshot_size(&saved), SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE);
    TEST_CHECK(spectrum_snapshot_save(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
    TEST_CHECK(memcmp(plus2_bytes + copy, plus2_bytes + third, SPECTRUM_BANK_SIZE) == 0);
    power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
    TEST_CHECK(spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE,
                                      &problem));
    TEST_CHECK(memcmp(plus2_restored_ram, plus2_saved_ram, SPECTRUM_RAM_128K) == 0);

    plus2_bytes[third + 100] ^= 0xFF;
    problem = NULL;
    TEST_CHECK(!spectrum_snapshot_load(&restored, plus2_bytes,
                                       SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE, &problem));
    TEST_CHECK(problem != NULL && strstr(problem, "differ") != NULL);
  }
}

/* A locked register comes back locked, and the byte's top two bits, which
   reach no flip-flop, come back as nothing. */
static void a_plus2s_lock_comes_back(void) {
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x21);
  run_to_the_wait_loop(&saved, 1);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  plus2_bytes[49181] |= 0xC0;
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  TEST_CHECK(spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));
  TEST_EQUAL(restored.bank_register, 0x21);
  TEST_CHECK(spectrum_in_48k_mode(&restored));
}

/* The format holds no register of the sound chip, so it comes back as reset
   leaves it, with what the board presents to port A still presented. */
static void a_plus2s_sound_chip_comes_back_reset(void) {
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x00);
  run_to_the_wait_loop(&saved, 1);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  ay38910_access(&restored.psg, AY38910_SELECT, 8);
  ay38910_access(&restored.psg, AY38910_WRITE, 0x0F);
  TEST_EQUAL(restored.psg.registers[8], 0x0F);
  ay38910_present_port_a(&restored.psg, 0x5A);
  TEST_CHECK(spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));
  TEST_EQUAL(restored.psg.registers[8], 0x00);
  TEST_EQUAL(restored.psg.output[0], 0);
  TEST_EQUAL(restored.psg.port_a_input, 0x5A);
}

/* A 128K snapshot that contradicts itself, or asks for what a +2 has not
   got, is refused; and a 48K refuses every 128K snapshot. */
static void a_128k_snapshot_that_cannot_be_a_plus2s_is_refused(void) {
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x00);
  run_to_the_wait_loop(&saved, 1);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);

  /* A length that is neither of the two. */
  TEST_CHECK(
      !spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE - 1, &problem));
  /* The register naming bank 5 in a file too short to hold it twice. */
  plus2_bytes[49181] = 0x05;
  TEST_CHECK(
      !spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));
  /* And a file long enough to hold a bank twice naming a bank it need not. */
  plus2_bytes[49181] = 0x00;
  TEST_CHECK(!spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE,
                                     &problem));
  /* TR-DOS paged in. */
  plus2_bytes[49182] = 1;
  problem = NULL;
  TEST_CHECK(
      !spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));
  TEST_CHECK(problem != NULL && strstr(problem, "TR-DOS") != NULL);
  plus2_bytes[49182] = 0;
  TEST_CHECK(spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));

  /* A 48K refuses a 128K snapshot by its length alone. */
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      !spectrum_snapshot_load(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_128K_SIZE, &problem));
}

int main(void) {
  TEST_RUN(a_48k_snapshot_is_the_one_length_it_can_be);
  TEST_RUN(the_header_is_laid_out_as_the_format_says);
  TEST_RUN(a_stack_the_program_counter_could_not_reach_is_refused);
  TEST_RUN(the_border_byte_cannot_drive_the_speaker);
  TEST_RUN(a_half_done_instruction_cannot_be_saved);
  TEST_RUN(a_hold_still_owed_cannot_be_saved_either);
  TEST_RUN(a_machine_survives_the_round_trip);
  TEST_RUN(a_hold_does_not_survive_a_load);
  TEST_RUN(a_restored_machine_goes_on_running_from_a_fresh_frame);
  TEST_RUN(a_load_draws_as_on_a_machine_just_switched_on);
  TEST_RUN(the_program_counter_costs_two_bytes_of_the_snapshot);
  TEST_RUN(a_stack_at_the_bottom_of_memory_cannot_be_written);
  TEST_RUN(the_header_carries_one_interrupt_flag_and_restores_both);
  TEST_RUN(the_interrupt_flag_is_written_where_mame_reads_it_too);
  TEST_RUN(a_plus2_survives_the_round_trip_whole);
  TEST_RUN(a_plus2_paging_bank_5_or_2_holds_it_twice);
  TEST_RUN(a_plus2s_lock_comes_back);
  TEST_RUN(a_plus2s_sound_chip_comes_back_reset);
  TEST_RUN(a_128k_snapshot_that_cannot_be_a_plus2s_is_refused);
  return TEST_REPORT("spectrum snapshot");
}
