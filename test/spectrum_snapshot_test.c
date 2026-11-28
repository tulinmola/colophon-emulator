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
static uint8_t bytes[SPECTRUM_SNAPSHOT_SNA_48K_SIZE];

/* Where the program below counts its interrupts, as an index into the RAM. */
#define COUNTER (0x8000 - SPECTRUM_RAM_BASE)

/* Where the program below halts to wait for an interrupt. A machine caught
   waiting there is written with its program counter on the HALT, which runs
   again on restore. */
#define WAIT_HALT 0x001D

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
      0x76,                   /* HALT, at WAIT_HALT */
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
  TEST_EQUAL(SPECTRUM_SNAPSHOT_SNA_48K_SIZE, 49179);
  TEST_EQUAL(spectrum_snapshot_sna_size(&saved), 49179);

  const char *problem = NULL;
  TEST_CHECK(
      !spectrum_snapshot_load_sna(&saved, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE - 1, &problem));
  TEST_CHECK(problem != NULL);
  TEST_CHECK(
      !spectrum_snapshot_load_sna(&saved, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE + 1, &problem));

  /* A 16K machine is not one of these, in either direction. */
  static uint8_t small[SPECTRUM_RAM_16K];
  power_on(&saved, small, SPECTRUM_RAM_16K);
  TEST_EQUAL(spectrum_snapshot_sna_size(&saved), 0);
  TEST_CHECK(!spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  TEST_CHECK(!spectrum_snapshot_load_sna(&saved, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));

  /* Nor is a +2, which has the 128K format instead and refuses this one;
     and a +2 handed no more RAM than a 48K fits neither. */
  static uint8_t banks[SPECTRUM_RAM_128K];
  spectrum_init(&saved, SPECTRUM_PLUS2, banks, SPECTRUM_RAM_128K, rom);
  TEST_EQUAL(spectrum_snapshot_sna_size(&saved), SPECTRUM_SNAPSHOT_SNA_128K_SIZE);
  TEST_CHECK(!spectrum_snapshot_load_sna(&saved, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  spectrum_init(&saved, SPECTRUM_PLUS2, banks, SPECTRUM_RAM_48K, rom);
  TEST_EQUAL(spectrum_snapshot_sna_size(&saved), 0);
  TEST_CHECK(!spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
}

static void a_half_done_instruction_cannot_be_saved(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  spectrum_tick(&saved); /* one T-state in, so the fetch is unfinished */
  TEST_CHECK(!z80_instruction_complete(&saved.cpu));
  const char *problem = NULL;
  TEST_CHECK(!spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  spectrum_finish_instruction(&saved);
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
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
  TEST_CHECK(!spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  TEST_CHECK(problem != NULL && strstr(problem, "holding the clock") != NULL);

  spectrum_finish_instruction(&saved);
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
}

/* The format has nowhere to put a hold in progress, so a machine given a
   snapshot stops serving whatever the machine it replaced was serving. */
static void a_hold_does_not_survive_a_load(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

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

  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_EQUAL(restored.held_ticks, 0);
}

static void a_machine_survives_the_round_trip(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);

  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));

  TEST_EQUAL(restored.cpu.a, saved.cpu.a);
  TEST_EQUAL(restored.cpu.f, saved.cpu.f);
  TEST_EQUAL((restored.cpu.b << 8) | restored.cpu.c, 0x1234);
  TEST_EQUAL((restored.cpu.d << 8) | restored.cpu.e, 0x5678);
  TEST_EQUAL((restored.cpu.ixh << 8) | restored.cpu.ixl, 0xBBAA);
  TEST_EQUAL((restored.cpu.iyh << 8) | restored.cpu.iyl, 0xDDCC);
  TEST_EQUAL(restored.cpu.hl_, 0x2211);
  TEST_EQUAL(restored.cpu.pc, WAIT_HALT);
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
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  for (int tick = 0; tick < 1000; tick++) {
    spectrum_tick(&restored);
  }
  TEST_EQUAL(restored.ula.frame_tick, 1000); /* loaded into a machine mid-frame */
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
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
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

  power_on(&running, running_ram, SPECTRUM_RAM_48K);
  spectrum_connect_monitor(&running, running_picture);
  run(&running, 17);
  while (running.ula.frame_tick != 1843) {
    spectrum_tick(&running);
  }
  TEST_CHECK(spectrum_snapshot_load_sna(&running, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_EQUAL(running.ula.frame_count, 0);

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  memcpy(fresh_picture, running_picture, sizeof fresh_picture); /* the same leftovers */
  spectrum_connect_monitor(&restored, fresh_picture);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));

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
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

  /* The machine is as it was. */
  TEST_EQUAL(saved.cpu.sp, sp);
  TEST_EQUAL(saved.cpu.pc, pc);
  TEST_EQUAL(spectrum_peek(&saved, (uint16_t)(sp - 2)), below[0]);
  TEST_EQUAL(spectrum_peek(&saved, (uint16_t)(sp - 1)), below[1]);

  /* The snapshot is not: its stack pointer is two lower, and where those
     bytes were it holds the program counter, on the HALT the machine waits
     at. */
  uint32_t at = 0x1B + (uint32_t)(sp - 2 - SPECTRUM_RAM_BASE);
  TEST_EQUAL(bytes[0x17] | (bytes[0x18] << 8), (uint16_t)(sp - 2));
  TEST_EQUAL(bytes[at], (uint8_t)WAIT_HALT);
  TEST_EQUAL(bytes[at + 1], (uint8_t)(WAIT_HALT >> 8));

  /* And loading pops them straight back off. */
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_EQUAL(restored.cpu.sp, sp);
  TEST_EQUAL(restored.cpu.pc, WAIT_HALT);
}

/* A stack with no room beneath it cannot be written down at all. */
static void a_stack_at_the_bottom_of_memory_cannot_be_written(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  spectrum_finish_instruction(&saved);
  const char *problem = NULL;
  saved.cpu.sp = SPECTRUM_RAM_BASE + 1;
  TEST_CHECK(!spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  saved.cpu.sp = SPECTRUM_RAM_BASE + 2;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

  /* With SP at 0x0000 the push wraps round to 0xFFFF and 0xFFFE, which are
     RAM; at 0x0001 its high byte would land on the ROM at 0x0000. */
  saved.cpu.sp = 0x0001;
  TEST_CHECK(!spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  saved.cpu.sp = 0x0000;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_EQUAL(restored.cpu.sp, 0x0000);
  TEST_EQUAL(restored.cpu.pc, saved.cpu.pc);
}

/* Interrupts come back through RETN's own rule rather than from a field. */
static void the_header_carries_one_interrupt_flag_and_restores_both(void) {
  power_on(&saved, saved_ram, SPECTRUM_RAM_48K);
  run_to_the_wait_loop(&saved, 3);
  TEST_CHECK(saved.cpu.iff2); /* halted in the wait loop, interrupts on */
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13] & 0x04, 0x04);

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_CHECK(restored.cpu.iff1);
  TEST_CHECK(restored.cpu.iff2);

  /* And a machine caught with interrupts disabled comes back disabled. */
  bytes[0x13] = 0x00;
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
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
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13], 0x05);

  /* After a non-maskable interrupt taken with interrupts enabled, and before
     its RETN, the two differ; bit 0 takes IFF2, as the RETN would, so MAME
     resumes what this reader resumes. */
  saved.cpu.iff1 = false;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13], 0x05);

  saved.cpu.iff1 = saved.cpu.iff2 = false;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));
  TEST_EQUAL(bytes[0x13], 0x00);

  /* A snapshot written to the format, by another emulator or by this one
     before bit 0 was, carries bit 2 alone and resumes enabled. */
  bytes[0x13] = 0x04;
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_CHECK(restored.cpu.iff1);
  TEST_CHECK(restored.cpu.iff2);

  bytes[0x13] = 0x01;
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
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
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, bytes, sizeof bytes, &problem));

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
  TEST_EQUAL(0x1B + SPECTRUM_RAM_48K, SPECTRUM_SNAPSHOT_SNA_48K_SIZE);
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
    TEST_CHECK(
        !spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
    TEST_CHECK(problem != NULL);
  }

  /* The lowest one that works is the bottom of the RAM itself. */
  bytes[0x17] = (uint8_t)SPECTRUM_RAM_BASE;
  bytes[0x18] = (uint8_t)(SPECTRUM_RAM_BASE >> 8);
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
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
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
  TEST_EQUAL(restored.ula.border, 7);
  TEST_CHECK(!restored.ula.microphone);
  TEST_CHECK(!restored.ula.speaker);
}

static uint8_t plus2_saved_ram[SPECTRUM_RAM_128K];
static uint8_t plus2_restored_ram[SPECTRUM_RAM_128K];
static uint8_t plus2_bytes[SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE];

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
  TEST_EQUAL(spectrum_snapshot_sna_size(&saved), SPECTRUM_SNAPSHOT_SNA_128K_SIZE);
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));

  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  memset(plus2_restored_ram, 0xEE, sizeof plus2_restored_ram);
  /* A write to the register the machine was still to latch is not carried
     over the load, where it would latch whatever the bus held. */
  restored.latching_bank_register = true;
  restored.pins = z80_set_data(restored.pins, 0x10);
  TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                        &problem));
  TEST_EQUAL(restored.cpu.pc, WAIT_HALT);
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
  TEST_EQUAL(plus2_bytes[49179] | plus2_bytes[49180] << 8, WAIT_HALT);
}

/* Neither format can say the processor is halted, so a machine caught
   halted is written with its program counter on its HALT, as Fuse writes
   one (z80.h), and comes back running the HALT again: halted where it was,
   and not past the HALT.
   Interrupts are off here, so nothing but the snapshot could move it on. */
static void a_halted_machine_comes_back_halted(void) {
  static const uint8_t program[] = {
      0xF3,             /* DI          */
      0x31, 0x00, 0x80, /* LD SP,&8000 */
      0x76,             /* HALT        */
      0x3E, 0x42,       /* LD A,&42    */
      0x18, 0xFE,       /* JR $        */
  };
  const spectrum_model models[] = {SPECTRUM_48K, SPECTRUM_PLUS2};
  const uint32_t ram_sizes[] = {SPECTRUM_RAM_48K, SPECTRUM_RAM_128K};
  for (size_t index = 0; index < 2; index++) {
    memset(rom, 0, sizeof rom);
    memcpy(rom, program, sizeof program);
    spectrum_init(&saved, models[index], plus2_saved_ram, ram_sizes[index], rom);
    const char *problem = NULL;

    /* Not halted, it is written where it stands: here past the DI. */
    spectrum_tick(&saved);
    spectrum_finish_instruction(&saved);
    TEST_EQUAL(saved.cpu.pc, 0x0001);
    TEST_CHECK(!saved.cpu.halted);
    TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
    spectrum_init(&restored, models[index], plus2_restored_ram, ram_sizes[index], rom);
    TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes,
                                          spectrum_snapshot_sna_size(&saved), &problem));
    TEST_EQUAL(restored.cpu.pc, saved.cpu.pc);

    run_to_the_wait_loop(&saved, 1);
    TEST_CHECK(saved.cpu.halted);
    TEST_EQUAL(saved.cpu.pc, 0x0005); /* where its idle fetches go */
    TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));

    spectrum_init(&restored, models[index], plus2_restored_ram, ram_sizes[index], rom);
    TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes,
                                          spectrum_snapshot_sna_size(&saved), &problem));
    TEST_EQUAL(restored.cpu.pc, 0x0004);
    run(&restored, 2);
    TEST_CHECK(restored.cpu.halted);
    TEST_EQUAL(restored.cpu.pc, 0x0005);
    TEST_EQUAL(restored.cpu.a, saved.cpu.a); /* LD A,&42 never ran */
  }
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
    TEST_EQUAL(spectrum_snapshot_sna_size(&saved), SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE);
    TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
    TEST_CHECK(memcmp(plus2_bytes + copy, plus2_bytes + third, SPECTRUM_BANK_SIZE) == 0);
    power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
    TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes,
                                          SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE, &problem));
    TEST_CHECK(memcmp(plus2_restored_ram, plus2_saved_ram, SPECTRUM_RAM_128K) == 0);

    plus2_bytes[third + 100] ^= 0xFF;
    problem = NULL;
    TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes,
                                           SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE, &problem));
    TEST_CHECK(problem != NULL && strstr(problem, "differ") != NULL);
  }
}

/* A locked register comes back locked, and the byte's top two bits, which
   reach no flip-flop, come back as nothing. */
static void a_plus2s_lock_comes_back(void) {
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x21);
  run_to_the_wait_loop(&saved, 1);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  plus2_bytes[49181] |= 0xC0;
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                        &problem));
  TEST_EQUAL(restored.bank_register, 0x21);
  TEST_CHECK(spectrum_in_48k_mode(&restored));
}

/* The format holds no register of the sound chip, so it comes back as reset
   leaves it, with what the board presents to port A still presented. */
static void a_plus2s_sound_chip_comes_back_reset(void) {
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x00);
  run_to_the_wait_loop(&saved, 1);
  const char *problem = NULL;
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  ay38910_access(&restored.psg, AY38910_SELECT, 8);
  ay38910_access(&restored.psg, AY38910_WRITE, 0x0F);
  TEST_EQUAL(restored.psg.registers[8], 0x0F);
  ay38910_present_port_a(&restored.psg, 0x5A);
  TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                        &problem));
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
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);

  /* A length that is neither of the two. */
  TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes,
                                         SPECTRUM_SNAPSHOT_SNA_128K_SIZE - 1, &problem));
  /* The register naming bank 5 in a file too short to hold it twice. */
  plus2_bytes[49181] = 0x05;
  TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                         &problem));
  /* And a file long enough to hold a bank twice naming a bank it need not. */
  plus2_bytes[49181] = 0x00;
  TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes,
                                         SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE, &problem));
  /* TR-DOS paged in. */
  plus2_bytes[49182] = 1;
  problem = NULL;
  TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                         &problem));
  TEST_CHECK(problem != NULL && strstr(problem, "TR-DOS") != NULL);
  plus2_bytes[49182] = 0;
  TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                        &problem));

  /* A 48K refuses a 128K snapshot by its length alone. */
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                         &problem));
}

/* The .z80 format, read and never written: each file below is built by
   hand from Lunter's manual and the World of Spectrum page that carries it
   on (spectrum_snapshot.h). */

#define Z80_FILE_ROOM 140000
static uint8_t z80_file[Z80_FILE_ROOM];
static size_t z80_length;

static void put16(size_t at, uint16_t value) {
  z80_file[at] = (uint8_t)value;
  z80_file[at + 1] = (uint8_t)(value >> 8);
}

/* A first header whose every register differs from every other, so that
   two read into each other's places show: A &11, F &22, BC &3344, HL
   &5566, SP &99AA, I &BB, R &4C with bit 7 set by byte 12, which also
   holds border 5, DE &CCDD, BC' &EEFF, DE' &1357, HL' &2468, A' &5A, F'
   &A5, IY &7788, IX &F00F, IFF1 set and IFF2 clear, interrupt mode 2. */
static void begin_z80(uint16_t first_header_pc, uint8_t flags) {
  static const uint8_t header[30] = {
      0x11, 0x22, 0x44, 0x33, 0x66, 0x55, 0x00, 0x00, 0xAA, 0x99, 0xBB, 0x4C, 0x00, 0xDD, 0xCC,
      0xFF, 0xEE, 0x57, 0x13, 0x68, 0x24, 0x5A, 0xA5, 0x88, 0x77, 0x0F, 0xF0, 0x01, 0x00, 0x02,
  };
  memset(z80_file, 0, sizeof z80_file);
  memcpy(z80_file, header, sizeof header);
  put16(6, first_header_pc);
  z80_file[12] = flags;
  z80_length = sizeof header;
}

/* Versions 2 and 3: the first header's PC left 0, then the additional
   header's length, the program counter and the hardware. */
static void begin_additional(uint16_t additional, uint8_t hardware) {
  begin_z80(0x0000, 0x0B);
  put16(30, additional);
  put16(32, 0x4321);
  z80_file[34] = hardware;
  z80_length = 32 + (size_t)additional;
}

static void append(const uint8_t *data, size_t length) {
  memcpy(z80_file + z80_length, data, length);
  z80_length += length;
}

/* A block as the file stores it: the length field, the page, the bytes. */
static void add_block(uint8_t page, uint16_t length_field, const uint8_t *stored,
                      size_t stored_length) {
  put16(z80_length, length_field);
  z80_file[z80_length + 2] = page;
  z80_length += 3;
  append(stored, stored_length);
}

/* A page of a pattern its number sets, stored raw, under 0xFFFF. */
static uint8_t page_pattern(unsigned page, size_t at) {
  return (uint8_t)((size_t)page * 0x10 + at % 13);
}

static void add_raw_page(uint8_t page) {
  static uint8_t data[SPECTRUM_BANK_SIZE];
  for (size_t at = 0; at < SPECTRUM_BANK_SIZE; at++) {
    data[at] = page_pattern(page, at);
  }
  add_block(page, 0xFFFF, data, SPECTRUM_BANK_SIZE);
}

/* A page of one byte, compressed to 65 runs: 260 bytes. */
static void add_compressed_page(uint8_t page, uint8_t value) {
  uint8_t runs[65 * 4];
  for (size_t run_index = 0; run_index < 65; run_index++) {
    runs[run_index * 4] = 0xED;
    runs[run_index * 4 + 1] = 0xED;
    runs[run_index * 4 + 2] = run_index < 64 ? 255 : 64;
    runs[run_index * 4 + 3] = value;
  }
  add_block(page, sizeof runs, runs, sizeof runs);
}

static void add_48k_pages(void) {
  add_raw_page(4);
  add_raw_page(5);
  add_raw_page(8);
}

static void add_128k_pages(void) {
  for (uint8_t page = 3; page <= 10; page++) {
    add_raw_page(page);
  }
}

static bool load_z80(spectrum_t *machine) {
  const char *problem = NULL;
  bool loaded = spectrum_snapshot_load_z80(machine, z80_file, z80_length, &problem);
  TEST_CHECK(loaded == (problem == NULL));
  return loaded;
}

/* A machine whose state a refused file must leave alone: its program run a
   few frames in, and kept to compare. */
static spectrum_t before;
static uint8_t before_ram[SPECTRUM_RAM_128K];

static void keep(const spectrum_t *machine) {
  before = *machine;
  memcpy(before_ram, machine->ram, machine->ram_size);
}

/* The registers a snapshot restores, compared by name: the processor's
   struct holds more than they, and padding no comparison may read. */
static bool same_registers(const z80_t *one, const z80_t *other) {
  return one->a == other->a && one->f == other->f && one->b == other->b && one->c == other->c &&
         one->d == other->d && one->e == other->e && one->h == other->h && one->l == other->l &&
         one->ixh == other->ixh && one->ixl == other->ixl && one->iyh == other->iyh &&
         one->iyl == other->iyl && one->af_ == other->af_ && one->bc_ == other->bc_ &&
         one->de_ == other->de_ && one->hl_ == other->hl_ && one->sp == other->sp &&
         one->pc == other->pc && one->i == other->i && one->r == other->r &&
         one->iff1 == other->iff1 && one->iff2 == other->iff2 && one->im == other->im;
}

static bool unchanged(const spectrum_t *machine) {
  return same_registers(&machine->cpu, &before.cpu) &&
         machine->ula.frame_tick == before.ula.frame_tick &&
         machine->bank_register == before.bank_register &&
         memcmp(machine->psg.registers, before.psg.registers, sizeof before.psg.registers) == 0 &&
         memcmp(machine->ram, before_ram, machine->ram_size) == 0;
}

/* Run part of a frame first, so that a machine wrongly restarted, its
   registers reset and its beam sent to the top, would not pass for one
   left alone. */
static void refuses_and_leaves_alone(spectrum_t *machine) {
  for (int tick = 0; tick < 1000; tick++) {
    spectrum_tick(machine);
  }
  keep(machine);
  TEST_CHECK(!load_z80(machine));
  TEST_CHECK(unchanged(machine));
}

/* Version 1, uncompressed: every register where the manual's table puts
   it, R's bit 7 from bit 0 of byte 12, the border from bits 1 to 3, both
   interrupt flip-flops, and the 48K that follows whole. */
static void a_z80_header_is_read_field_by_field(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  run(&restored, 2);
  begin_z80(0x1234, 0x0B);
  for (size_t at = 0; at < SPECTRUM_RAM_48K; at++) {
    z80_file[z80_length + at] = (uint8_t)(at % 251);
  }
  z80_length += SPECTRUM_RAM_48K;
  TEST_CHECK(load_z80(&restored));
  const z80_t *cpu = &restored.cpu;
  TEST_EQUAL(cpu->a, 0x11);
  TEST_EQUAL(cpu->f, 0x22);
  TEST_EQUAL((cpu->b << 8) | cpu->c, 0x3344);
  TEST_EQUAL((cpu->h << 8) | cpu->l, 0x5566);
  TEST_EQUAL(cpu->pc, 0x1234);
  TEST_EQUAL(cpu->sp, 0x99AA);
  TEST_EQUAL(cpu->i, 0xBB);
  TEST_EQUAL(cpu->r, 0xCC);
  TEST_EQUAL((cpu->d << 8) | cpu->e, 0xCCDD);
  TEST_EQUAL(cpu->bc_, 0xEEFF);
  TEST_EQUAL(cpu->de_, 0x1357);
  TEST_EQUAL(cpu->hl_, 0x2468);
  TEST_EQUAL(cpu->af_, 0x5AA5);
  TEST_EQUAL((cpu->iyh << 8) | cpu->iyl, 0x7788);
  TEST_EQUAL((cpu->ixh << 8) | cpu->ixl, 0xF00F);
  TEST_CHECK(cpu->iff1);
  TEST_CHECK(!cpu->iff2);
  TEST_EQUAL(cpu->im, 2);
  TEST_EQUAL(restored.ula.border, 5);
  TEST_EQUAL(restored.ula.frame_tick, 0);
  TEST_EQUAL(restored_ram[0], 0);
  TEST_EQUAL(restored_ram[SPECTRUM_RAM_48K - 1], (SPECTRUM_RAM_48K - 1) % 251);
}

/* The manual's own examples: "ED 6*00 is not encoded into ED ED ED 06 00
   but into ED 00 ED ED 05 00", and "even two ED's are encoded into ED ED
   02 ED"; then runs to fill the 48K, and "The block is terminated by an
   end marker, 00 ED ED 00". */
static void a_version_1_file_expands_as_the_manual_shows(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_z80(0x1234, 0x0B | 0x20);
  static const uint8_t examples[] = {0xED, 0x00, 0xED, 0xED, 0x05, 0x00, 0xED,
                                     0xED, 0x02, 0xED, 0xED, 0xED, 0x05, 0xAA};
  append(examples, sizeof examples);
  size_t left = SPECTRUM_RAM_48K - 7 - 2 - 5;
  while (left > 0) {
    uint8_t count = left > 255 ? 255 : (uint8_t)left;
    const uint8_t run_of_77[] = {0xED, 0xED, count, 0x77};
    append(run_of_77, sizeof run_of_77);
    left -= count;
  }
  static const uint8_t end[] = {0x00, 0xED, 0xED, 0x00};
  append(end, sizeof end);
  TEST_CHECK(load_z80(&restored));
  static const uint8_t expanded[] = {0xED, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0xED, 0xED, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  TEST_CHECK(memcmp(restored_ram, expanded, sizeof expanded) == 0);
  TEST_EQUAL(restored_ram[sizeof expanded], 0x77);
  TEST_EQUAL(restored_ram[SPECTRUM_RAM_48K - 1], 0x77);

  /* With other bytes where its end marker should be, or a byte short of
     48K, it is refused. */
  z80_file[z80_length - 1] = 0x01;
  refuses_and_leaves_alone(&restored);
  z80_file[z80_length - 1] = 0x00;
  z80_file[34] = 0x04; /* the first run's count, 5 → 4 */
  refuses_and_leaves_alone(&restored);
}

/* "if byte 12 is 255, it has to be regarded as being 1": R's bit 7 set,
   border 0, and the data not compressed. */
static void byte_12_of_255_is_read_as_1(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_z80(0x1234, 0xFF);
  memset(z80_file + z80_length, 0xED, SPECTRUM_RAM_48K);
  z80_length += SPECTRUM_RAM_48K;
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored.cpu.r, 0xCC);
  TEST_EQUAL(restored.ula.border, 0);
  TEST_EQUAL(restored_ram[100], 0xED);
  /* An uncompressed 48K that is not 48K long is refused. */
  z80_length -= 1;
  refuses_and_leaves_alone(&restored);
}

/* "In 48K mode, pages 4,5 and 8 are saved": 8 at 0x4000, 4 at 0x8000 and
   5 at 0xC000, in whatever order the file holds them, and the program
   counter from the additional header. */
static void a_48k_file_places_its_pages_by_number(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_additional(23, 0);
  add_raw_page(5);
  add_raw_page(8);
  add_raw_page(4);
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored.cpu.pc, 0x4321);
  TEST_EQUAL(restored_ram[1], page_pattern(8, 1));
  TEST_EQUAL(restored_ram[SPECTRUM_BANK_SIZE + 1], page_pattern(4, 1));
  TEST_EQUAL(restored_ram[2 * SPECTRUM_BANK_SIZE + 1], page_pattern(5, 1));
  TEST_EQUAL(restored.ula.frame_tick, 0); /* version 2 has no counter */
}

/* The hardware byte from 3 to 6 means different machines in the two
   versions: 3 is a 128K in version 2 and a 48K with the M.G.T. in version
   3; 4 a 128K with the Interface 1, and a bare 128K. */
static void the_two_versions_number_the_machines_differently(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_additional(23, 3);
  add_128k_pages();
  refuses_and_leaves_alone(&restored);
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  TEST_CHECK(load_z80(&restored));

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_additional(54, 3);
  add_48k_pages();
  TEST_CHECK(load_z80(&restored));

  for (uint16_t additional = 23; additional <= 54; additional += 31) {
    power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
    begin_additional(additional, 4);
    add_128k_pages();
    TEST_CHECK(load_z80(&restored));
  }
}

/* A +2's file: every bank where its page puts it, the bank register's six
   bits, and the sound chip's registers through its own bus — so that its
   masks apply — with the register the file names selected last. */
static void a_plus2_comes_back_with_its_banks_paging_and_sound(void) {
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  ay38910_present_port_a(&restored.psg, 0x5A);
  begin_additional(54, 12);
  z80_file[35] = 0xF7;
  z80_file[38] = 0x0B;
  for (int index = 0; index < 16; index++) {
    z80_file[39 + index] = (uint8_t)(0xF0 | index);
  }
  add_128k_pages();
  TEST_CHECK(load_z80(&restored));
  for (unsigned bank_number = 0; bank_number < 8; bank_number++) {
    TEST_EQUAL(plus2_restored_ram[bank_number * SPECTRUM_BANK_SIZE + 5],
               page_pattern(bank_number + 3, 5));
  }
  TEST_EQUAL(restored.bank_register, 0x37);
  TEST_EQUAL(restored.psg.registers[0], 0xF0);
  TEST_EQUAL(restored.psg.registers[1], 0x01); /* four bits of a coarse tone */
  TEST_EQUAL(restored.psg.registers[8], 0x18); /* five of an amplitude */
  TEST_EQUAL(restored.psg.selected, 0x0B);
  TEST_CHECK(restored.psg.chip_selected);
  TEST_EQUAL(restored.psg.port_a_input, 0x5A);

  /* A chip saved unselected, its last address's high four bits not 0000,
     comes back unselected, and ignores a write until it is selected. */
  z80_file[38] = 0x1B;
  TEST_CHECK(load_z80(&restored));
  TEST_CHECK(!restored.psg.chip_selected);
  uint8_t held = restored.psg.registers[restored.psg.selected];
  ay38910_access(&restored.psg, AY38910_WRITE, (uint8_t)(held ^ 0x01));
  TEST_EQUAL(restored.psg.registers[restored.psg.selected], held);
}

/* Bit 7 of byte 37: "any 48K machine becomes a 16K machine, any 128K
   machines becomes a +2". A 16K's file is written with a 48K's pages, of
   which only 8 is its RAM. */
static void the_modify_hardware_bit_makes_a_16k_or_a_plus2(void) {
  begin_additional(54, 0);
  z80_file[37] = 0x80;
  add_48k_pages();
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  refuses_and_leaves_alone(&restored);
  power_on(&restored, restored_ram, SPECTRUM_RAM_16K);
  restored_ram[SPECTRUM_RAM_16K] = 0x99; /* past the RAM a 16K has */
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored_ram[1], page_pattern(8, 1));
  TEST_EQUAL(restored_ram[SPECTRUM_RAM_16K], 0x99);

  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_additional(54, 4);
  z80_file[37] = 0x80;
  add_128k_pages();
  TEST_CHECK(load_z80(&restored));
}

/* Each machine takes only its own: a 48K's file is refused by a +2 and a
   128K's by a 48K, and the machines no model here builds by both. */
static void a_file_of_another_machine_is_refused(void) {
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_z80(0x1234, 0x0B);
  z80_length += SPECTRUM_RAM_48K;
  refuses_and_leaves_alone(&restored);
  begin_additional(54, 0);
  add_48k_pages();
  refuses_and_leaves_alone(&restored);

  static const uint8_t others[] = {2, 7, 8, 9, 10, 11, 13, 14, 15, 16, 128, 255};
  for (size_t index = 0; index < sizeof others; index++) {
    begin_additional(54, others[index]);
    add_128k_pages();
    power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
    refuses_and_leaves_alone(&restored);
    begin_additional(54, others[index]);
    add_48k_pages();
    power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
    refuses_and_leaves_alone(&restored);
  }
  /* Version 2 never gave 5 or 6 a meaning. */
  for (uint8_t hardware = 5; hardware <= 6; hardware++) {
    begin_additional(23, hardware);
    add_128k_pages();
    power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
    refuses_and_leaves_alone(&restored);
  }
}

/* A file taken with an interface's ROM paged in was running a ROM no
   machine here has; one taken with it out runs as on the bare machine. */
static void an_interface_rom_paged_in_is_refused(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_additional(23, 1);
  add_48k_pages();
  TEST_CHECK(load_z80(&restored));
  z80_file[36] = 0xFF; /* "Contains 0xff if Interface I rom paged" */
  refuses_and_leaves_alone(&restored);

  begin_additional(54, 3);
  add_48k_pages();
  z80_file[59] = 0xFF; /* "0xff if MGT Rom paged" */
  refuses_and_leaves_alone(&restored);

  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_additional(54, 5);
  add_128k_pages();
  z80_file[36] = 0xFF;
  refuses_and_leaves_alone(&restored);
  begin_additional(54, 6);
  add_128k_pages();
  z80_file[59] = 0xFF;
  refuses_and_leaves_alone(&restored);
  begin_additional(23, 4); /* version 2's 128K with the Interface 1 */
  add_128k_pages();
  z80_file[36] = 0xFF;
  refuses_and_leaves_alone(&restored);
  begin_additional(54, 4); /* version 3's bare 128K, where the byte means nothing */
  add_128k_pages();
  z80_file[36] = 0xFF;
  TEST_CHECK(load_z80(&restored));
}

/* 0xFFFF marks a page stored raw. A length of 16384 is raw in WinZ80's
   files, which store a page raw when compressing it saves nothing, and
   compressed in at least one other writer's, so it is read compressed only
   when it expands to exactly one page. */
static void a_raw_page_is_told_from_a_compressed_one(void) {
  static uint8_t data[SPECTRUM_BANK_SIZE];
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_additional(54, 0);
  /* Page 8 raw under 0xFFFF, a run's mark and all. */
  memset(data, 0x31, sizeof data);
  memcpy(data, (const uint8_t[]){0xED, 0xED, 0x05, 0xAA}, 4);
  add_block(8, 0xFFFF, data, sizeof data);
  /* Page 4 under 16384, with a run that would make it a byte too long. */
  add_block(4, SPECTRUM_BANK_SIZE, data, sizeof data);
  /* Page 5 under 16384, with a run of four that keeps it exactly a page. */
  data[2] = 0x04;
  add_block(5, SPECTRUM_BANK_SIZE, data, sizeof data);
  TEST_CHECK(load_z80(&restored));
  TEST_CHECK(memcmp(restored_ram, (const uint8_t[]){0xED, 0xED, 0x05, 0xAA}, 4) == 0);
  TEST_CHECK(
      memcmp(restored_ram + SPECTRUM_BANK_SIZE, (const uint8_t[]){0xED, 0xED, 0x05, 0xAA}, 4) == 0);
  TEST_CHECK(memcmp(restored_ram + (size_t)2 * SPECTRUM_BANK_SIZE,
                    (const uint8_t[]){0xAA, 0xAA, 0xAA, 0xAA, 0x31}, 5) == 0);
}

/* Any other length must expand to exactly one page, using every byte. */
static void a_block_that_is_not_exactly_a_page_is_refused(void) {
  static uint8_t data[SPECTRUM_BANK_SIZE + 8];
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  struct {
    size_t length;
    uint8_t tail[4];
    size_t tail_length;
  } const cases[] = {
      {100, {0}, 0},                    /* far short */
      {SPECTRUM_BANK_SIZE - 3, {0}, 0}, /* a page less three, no runs */
      {8, {0xED, 0xED, 0x05}, 3},       /* a run cut short */
      {8, {0x00, 0x01}, 2},             /* a page, and bytes left over */
  };
  for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
    begin_additional(54, 0);
    add_raw_page(4);
    add_raw_page(5);
    size_t length = cases[index].length;
    memset(data, 0x42, sizeof data);
    if (cases[index].tail_length == 2) {
      /* 64 runs of 255 and one of 64: exactly a page, then two bytes. */
      length = 0;
      for (int run_index = 0; run_index < 65; run_index++) {
        const uint8_t run_bytes[] = {0xED, 0xED, (uint8_t)(run_index < 64 ? 255 : 64), 0x42};
        memcpy(data + length, run_bytes, 4);
        length += 4;
      }
    }
    memcpy(data + length, cases[index].tail, cases[index].tail_length);
    length += cases[index].tail_length;
    add_block(8, (uint16_t)length, data, length);
    refuses_and_leaves_alone(&restored);
  }
  /* A run cut short at the block's end, where the byte the file holds
     next would complete the page. */
  begin_additional(54, 0);
  add_raw_page(4);
  add_raw_page(5);
  size_t length = 0;
  for (int run_index = 0; run_index < 64; run_index++) {
    const uint8_t run_bytes[] = {0xED, 0xED, 255, 0x42};
    memcpy(data + length, run_bytes, 4);
    length += 4;
  }
  memset(data + length, 0x42, 59); /* 16379 bytes so far */
  length += 59;
  memcpy(data + length, (const uint8_t[]){0xED, 0xED, 5}, 3);
  length += 3;
  add_block(8, (uint16_t)length, data, length);
  z80_file[z80_length] = 0x42; /* the byte past the file */
  refuses_and_leaves_alone(&restored);
  /* And a run that would carry past the page. */
  begin_additional(54, 0);
  add_raw_page(4);
  add_raw_page(5);
  length = 0;
  for (int run_index = 0; run_index < 66; run_index++) {
    const uint8_t run_bytes[] = {0xED, 0xED, 255, 0x42};
    memcpy(data + length, run_bytes, 4);
    length += 4;
  }
  add_block(8, (uint16_t)length, data, length);
  refuses_and_leaves_alone(&restored);
}

/* Every page the machine is written with, each once, and no other; and a
   file that ends inside a block refused. */
static void each_page_is_there_once(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_additional(54, 0);
  add_48k_pages();
  add_raw_page(8);
  refuses_and_leaves_alone(&restored);
  begin_additional(54, 0);
  add_raw_page(4);
  add_raw_page(8);
  refuses_and_leaves_alone(&restored);
  static const uint8_t foreign[] = {0, 1, 2, 3, 6, 7, 9, 10, 11};
  for (size_t index = 0; index < sizeof foreign; index++) {
    begin_additional(54, 0);
    add_48k_pages();
    add_raw_page(foreign[index]);
    refuses_and_leaves_alone(&restored);
  }
  begin_additional(54, 0);
  add_48k_pages();
  z80_length += 2; /* a block's header cut short */
  refuses_and_leaves_alone(&restored);
  const char *problem = NULL;
  TEST_CHECK(!spectrum_snapshot_load_z80(&restored, z80_file, z80_length, &problem));
  TEST_CHECK(problem != NULL && strstr(problem, "header of a block") != NULL);
  z80_length -= 100; /* the last block cut short */
  refuses_and_leaves_alone(&restored);

  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_additional(54, 4);
  add_128k_pages();
  z80_length -= 3 + SPECTRUM_BANK_SIZE; /* page 10, bank 7, gone */
  refuses_and_leaves_alone(&restored);
}

/* The additional header's length names the version: 23, or 21, which
   Lunter's z80dump also takes for the second, with fourteen of the sound
   chip's registers; 54, or 55 with a byte for the +3's paging port that a
   +2 does without. */
static void the_additional_header_has_four_lengths(void) {
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_additional(21, 3);
  for (int index = 0; index < 14; index++) {
    z80_file[39 + index] = 0x0E;
  }
  add_128k_pages();
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored.psg.registers[13], 0x0E);
  TEST_EQUAL(restored.psg.registers[14], 0x00);
  TEST_EQUAL(plus2_restored_ram[5], page_pattern(3, 5));

  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_additional(55, 4);
  z80_file[86] = 0x04;
  add_128k_pages();
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(plus2_restored_ram[5], page_pattern(3, 5));

  static const uint16_t others[] = {0, 22, 24, 31, 53, 56, 1000};
  for (size_t index = 0; index < sizeof others / sizeof others[0]; index++) {
    begin_additional(others[index], 4);
    add_128k_pages();
    refuses_and_leaves_alone(&restored);
  }
  /* A file that ends inside a header is refused for it, before any of the
     header's bytes past its end are read. */
  const char *problem = NULL;
  for (size_t length = 29; length < 32 + 54; length += 7) {
    begin_additional(54, 4);
    z80_length = length;
    keep(&restored);
    TEST_CHECK(!spectrum_snapshot_load_z80(&restored, z80_file, z80_length, &problem));
    TEST_CHECK(problem != NULL && strstr(problem, length < 30 ? "thirty" : "additional") != NULL);
    TEST_CHECK(unchanged(&restored));
  }
}

/* Version 3's counter: "The hi T state counter counts up modulo 4. Just
   after the ULA generates its once-in-every-20-ms interrupt, it is 3 ...
   the low T state counter counts down from 17471 to 0 (17726 in 128K
   modes)". The T-state it names is Fuse's count, one less than the frame
   tick here (ula.h); out of range, the frame starts at its top. */
static void the_beam_resumes_where_the_counter_puts_it(void) {
  const struct {
    uint16_t low;
    uint8_t high;
    uint32_t frame_tick;
  } cases[] = {
      {17471, 3, 1}, {0, 3, 17472}, {17471, 0, 17473}, {1000, 1, 2 * 17472 + 16472},
      {0, 2, 0},     {17472, 3, 0}, {100, 4, 0},       {0xFFFF, 0, 0},
  };
  for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
    power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
    run(&restored, 1);
    begin_additional(54, 0);
    put16(55, cases[index].low);
    z80_file[57] = cases[index].high;
    add_48k_pages();
    TEST_CHECK(load_z80(&restored));
    TEST_EQUAL(restored.ula.frame_tick, cases[index].frame_tick);
  }
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  begin_additional(54, 4);
  put16(55, 17726);
  z80_file[57] = 3;
  add_128k_pages();
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored.ula.frame_tick, 1);
  put16(55, 0);
  z80_file[57] = 2;
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored.ula.frame_tick, 0);

  /* Version 2 has no counter. Where version 3 keeps one, a 128K's file
     often holds its first block's header — page 3, compressed to a few
     hundred bytes — which read as a counter would be in range. */
  begin_additional(23, 3);
  add_compressed_page(3, 0x5E);
  for (uint8_t page = 4; page <= 10; page++) {
    add_raw_page(page);
  }
  TEST_CHECK(load_z80(&restored));
  TEST_EQUAL(restored.ula.frame_tick, 0);
  TEST_EQUAL(plus2_restored_ram[SPECTRUM_BANK_SIZE - 1], 0x5E);
}

/* What the header keeps for the emulator that wrote it — issue 2,
   interrupt frequency, video synchronisation, the joystick, R and LDIR
   emulation, a sound chip on a 48K, the µSpeech, the Fuller box — and the
   bytes from 58 that writers fill with junk change nothing. */
static void the_emulators_settings_change_nothing(void) {
  static spectrum_t plain;
  static uint8_t plain_ram[SPECTRUM_RAM_48K];
  power_on(&plain, plain_ram, SPECTRUM_RAM_48K);
  begin_additional(55, 0);
  put16(55, 1000);
  z80_file[57] = 1;
  add_48k_pages();
  TEST_CHECK(load_z80(&plain));

  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  z80_file[29] = 0xFE; /* interrupt mode 2 still, every setting on */
  z80_file[37] = 0x4F;
  for (size_t at = 38; at < 55; at++) {
    z80_file[at] = 0xED;
  }
  for (size_t at = 58; at <= 86; at++) {
    if (at != 59) {
      z80_file[at] = 0xED;
    }
  }
  TEST_CHECK(load_z80(&restored));
  TEST_CHECK(same_registers(&restored.cpu, &plain.cpu));
  TEST_EQUAL(restored.ula.frame_tick, plain.ula.frame_tick);
  TEST_CHECK(memcmp(restored_ram, plain_ram, SPECTRUM_RAM_48K) == 0);
}

/* Two bits give the interrupt mode, and the SNA formats a byte: room for a
   mode the processor has not got, which both readers refuse. */
static void an_interrupt_mode_the_processor_has_not_got_is_refused(void) {
  power_on(&restored, restored_ram, SPECTRUM_RAM_48K);
  begin_z80(0x1234, 0x0B);
  z80_length += SPECTRUM_RAM_48K;
  z80_file[29] = 0x43;
  refuses_and_leaves_alone(&restored);
  z80_file[29] = 0x42;
  TEST_CHECK(load_z80(&restored));

  const char *problem = NULL;
  memset(bytes, 0, sizeof bytes);
  bytes[0x17] = 0x00; /* a stack at &C000 */
  bytes[0x18] = 0xC0;
  for (uint8_t mode = 3; mode <= 4; mode++) {
    bytes[0x19] = mode;
    keep(&restored);
    TEST_CHECK(
        !spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));
    TEST_CHECK(unchanged(&restored));
  }
  bytes[0x19] = 2;
  TEST_CHECK(
      spectrum_snapshot_load_sna(&restored, bytes, SPECTRUM_SNAPSHOT_SNA_48K_SIZE, &problem));

  /* And a +2's. */
  power_on_plus2_paging(&saved, plus2_saved_ram, 0x00);
  run_to_the_wait_loop(&saved, 1);
  TEST_CHECK(spectrum_snapshot_save_sna(&saved, plus2_bytes, sizeof plus2_bytes, &problem));
  power_on_plus2_paging(&restored, plus2_restored_ram, 0x00);
  run(&restored, 1);
  for (uint8_t mode = 3; mode <= 4; mode++) {
    plus2_bytes[0x19] = mode;
    keep(&restored);
    TEST_CHECK(!spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                           &problem));
    TEST_CHECK(unchanged(&restored));
  }
  plus2_bytes[0x19] = 1;
  TEST_CHECK(spectrum_snapshot_load_sna(&restored, plus2_bytes, SPECTRUM_SNAPSHOT_SNA_128K_SIZE,
                                        &problem));
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
  TEST_RUN(a_halted_machine_comes_back_halted);
  TEST_RUN(a_z80_header_is_read_field_by_field);
  TEST_RUN(a_version_1_file_expands_as_the_manual_shows);
  TEST_RUN(byte_12_of_255_is_read_as_1);
  TEST_RUN(a_48k_file_places_its_pages_by_number);
  TEST_RUN(the_two_versions_number_the_machines_differently);
  TEST_RUN(a_plus2_comes_back_with_its_banks_paging_and_sound);
  TEST_RUN(the_modify_hardware_bit_makes_a_16k_or_a_plus2);
  TEST_RUN(a_file_of_another_machine_is_refused);
  TEST_RUN(an_interface_rom_paged_in_is_refused);
  TEST_RUN(a_raw_page_is_told_from_a_compressed_one);
  TEST_RUN(a_block_that_is_not_exactly_a_page_is_refused);
  TEST_RUN(each_page_is_there_once);
  TEST_RUN(the_additional_header_has_four_lengths);
  TEST_RUN(the_beam_resumes_where_the_counter_puts_it);
  TEST_RUN(the_emulators_settings_change_nothing);
  TEST_RUN(an_interrupt_mode_the_processor_has_not_got_is_refused);
  return TEST_REPORT("spectrum snapshot");
}
