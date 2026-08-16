/*
 * crtc_test — the chip alone, held against the Compendium's frame.
 *
 * The registers carry the values the CPC firmware programs — the numbers
 * the Compendium builds its standard frame from (ch. 6.1.5) — and the
 * outputs are recorded for two whole frames: 312 scanlines of 64 characters,
 * 19968 characters each frame, sync for sync.
 */
#include <string.h>

#include "crtc.h"
#include "test.h"

static crtc_t crtc;

/* The standard 50Hz values (Compendium ch. 6.1.5): R0-R9, R12-R13. */
#define FRAME_TICKS 19968
#define SCANLINE 64

static void write_register(int reg, uint8_t value) {
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, (uint8_t)reg));
  crtc_access(&crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, value));
}

static void program_standard(void) {
  static const uint8_t values[14] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0};
  crtc_init(&crtc);
  for (int reg = 0; reg < 14; reg++) {
    write_register(reg, values[reg]);
  }
}

static uint64_t recorded[2 * FRAME_TICKS];

static void record_two_frames(void) {
  program_standard();
  for (int tick = 0; tick < 2 * FRAME_TICKS; tick++) {
    recorded[tick] = crtc_tick(&crtc);
  }
}

static uint64_t at(int scanline, int character) {
  return recorded[(size_t)scanline * SCANLINE + (size_t)character];
}

static void run_characters(int count) {
  for (int character = 0; character < count; character++) {
    crtc_tick(&crtc);
  }
}

/* Whole scanlines from wherever C0 stands, which is a line start in every
   test that has not moved R0. */
static void run_scanlines(int count) { run_characters(count * SCANLINE); }

/* Stop on the first character of the row named, however long the chip takes
   to get there; 4000 scanlines is a dozen frames and a failed loop. */
static bool run_to_row(uint8_t row) {
  for (int character = 0; character < 4000 * SCANLINE; character++) {
    if (crtc.c4 == row && crtc.c9 == 0 && crtc.c0 == 0) {
      return true;
    }
    crtc_tick(&crtc);
  }
  return false;
}

/* Scanlines from one frame start to the next. */
static long frame_scanlines(void) {
  long ticks = 0;
  do {
    crtc_tick(&crtc);
    ticks++;
  } while (crtc.c0 != 0 || crtc.c4 != 0 || crtc.c9 != 0);
  return ticks / SCANLINE;
}

static void reset_state(void) {
  crtc_init(&crtc);
  TEST_EQUAL(crtc.c0, 0);
  TEST_EQUAL(crtc.c9, 0);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.address_register, 0);
  TEST_CHECK(!crtc.hsync);
  TEST_CHECK(!crtc.vsync);
}

static void select_wears_five_bits(void) {
  crtc_init(&crtc);
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 0xEC));
  TEST_EQUAL(crtc.address_register, 0x0C);
}

static void writes_wear_the_documented_widths(void) {
  crtc_init(&crtc);
  write_register(4, 0xFF);
  TEST_EQUAL(crtc.registers[4], 0x7F);
  write_register(12, 0xFF);
  TEST_EQUAL(crtc.registers[12], 0x3F);
  write_register(9, 0xFF);
  TEST_EQUAL(crtc.registers[9], 0x1F);
  write_register(16, 0x55); /* lightpen latch: writes fall on deaf ears */
  TEST_EQUAL(crtc.registers[16], 0);
  write_register(20, 0x55); /* no such register */
}

static void type0_reads_r12_to_r17_and_nothing_else(void) {
  program_standard();
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 12));
  TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0x30);
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 4));
  TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0);
  /* A status read: type 0 has no status register, the bus stays floating —
     whatever the machine drove passes through. */
  uint64_t floating = crtc_set_data(0, 0x77) | CRTC_CS | CRTC_RW;
  TEST_EQUAL(crtc_data(crtc_access(&crtc, floating)), 0x77);
}

static void unselected_chip_ignores_the_bus(void) {
  crtc_init(&crtc);
  crtc_access(&crtc, crtc_set_data(0, 7)); /* no CS */
  TEST_EQUAL(crtc.address_register, 0);
}

static void hsync_falls_where_r2_and_r3_put_it(void) {
  record_two_frames();
  int pulses = 0;
  for (int tick = 1; tick < 2 * FRAME_TICKS; tick++) {
    if ((recorded[tick] & CRTC_HSYNC) && !(recorded[tick - 1] & CRTC_HSYNC)) {
      pulses++;
    }
  }
  TEST_EQUAL(pulses, 2 * 312);
  /* Width 14 (R3 low nibble), starting at character 46 (R2). */
  TEST_CHECK(!(recorded[45] & CRTC_HSYNC));
  TEST_CHECK(recorded[46] & CRTC_HSYNC);
  TEST_CHECK(recorded[59] & CRTC_HSYNC);
  TEST_CHECK(!(recorded[60] & CRTC_HSYNC));
}

static void vsync_holds_eight_scanlines_from_row_30(void) {
  record_two_frames();
  int rises = 0;
  for (int tick = 1; tick < 2 * FRAME_TICKS; tick++) {
    if ((recorded[tick] & CRTC_VSYNC) && !(recorded[tick - 1] & CRTC_VSYNC)) {
      rises++;
    }
  }
  TEST_EQUAL(rises, 2);
  /* Row 30 (R7) begins at scanline 240; width 8 (R3 high nibble). */
  TEST_CHECK(!(at(239, 63) & CRTC_VSYNC));
  TEST_CHECK(at(240, 0) & CRTC_VSYNC);
  TEST_CHECK(at(247, 63) & CRTC_VSYNC);
  TEST_CHECK(!(at(248, 0) & CRTC_VSYNC));
}

static void display_covers_40_by_200(void) {
  record_two_frames();
  TEST_CHECK(recorded[0] & CRTC_DISPTMG);
  TEST_CHECK(recorded[39] & CRTC_DISPTMG);
  TEST_CHECK(!(recorded[40] & CRTC_DISPTMG));
  TEST_CHECK(at(199, 0) & CRTC_DISPTMG);
  TEST_CHECK(!(at(200, 0) & CRTC_DISPTMG));
  long displayed = 0;
  for (int tick = 0; tick < FRAME_TICKS; tick++) {
    if (recorded[tick] & CRTC_DISPTMG) {
      displayed++;
    }
  }
  TEST_EQUAL(displayed, 40L * 200L);
}

static void the_video_pointer_walks_the_documented_rows(void) {
  record_two_frames();
  /* Frame start: MA = R12/R13, RA = 0. */
  TEST_EQUAL(crtc_ma(recorded[0]), 0x3000);
  TEST_EQUAL(crtc_ra(recorded[0]), 0);
  /* Every scanline of a row replays the row's addresses; RA counts the
     scanlines. */
  TEST_EQUAL(crtc_ma(at(7, 0)), 0x3000);
  TEST_EQUAL(crtc_ra(at(7, 0)), 7);
  /* Rows advance by R1 characters. */
  TEST_EQUAL(crtc_ma(at(8, 0)), 0x3000 + 40);
  TEST_EQUAL(crtc_ra(at(8, 0)), 0);
  TEST_EQUAL(crtc_ma(at(24 * 8, 0)), 0x3000 + 24 * 40);
  /* MA keeps counting past the displayed area. */
  TEST_EQUAL(crtc_ma(recorded[63]), 0x3000 + 63);
}

static void the_frame_locks_at_19968(void) {
  record_two_frames();
  for (int tick = 0; tick < FRAME_TICKS; tick++) {
    TEST_CHECK(recorded[tick] == recorded[tick + FRAME_TICKS]);
    if (recorded[tick] != recorded[tick + FRAME_TICKS]) {
      return; /* one report says it all */
    }
  }
  TEST_EQUAL(crtc_ma(recorded[FRAME_TICKS]), 0x3000);
}

static void c9_runs_to_its_own_top_when_r9_drops_below_it(void) {
  /* A limit written under the counter watching it does not stop the row:
     C9 counts to 31 and loops back before it can match again (ch.
     10.3.1.1). Eight bits of counter would take it to 255 instead, and a
     row 256 scanlines long is a frame that never ends. */
  program_standard();
  run_scanlines(3);
  TEST_EQUAL(crtc.c9, 3);
  run_characters(10); /* past C0=2, where the last line is decided */
  write_register(9, 1);

  uint8_t highest = 0;
  int scanlines_until_the_row_ends = 0;
  for (int scanline = 1; scanline <= 40 && crtc.c4 == 0; scanline++) {
    run_scanlines(1);
    if (crtc.c9 > highest) {
      highest = crtc.c9;
    }
    scanlines_until_the_row_ends = scanline;
  }
  TEST_EQUAL(highest, 31);
  TEST_EQUAL(crtc.c4, 1);
  TEST_EQUAL(scanlines_until_the_row_ends, 31);
}

static void c4_runs_to_its_own_top_when_r4_drops_below_it(void) {
  /* The same for the row counter, which spans seven bits: it climbs to 127
     and loops rather than ending the frame where R4 now stands (ch. 12.1).
     R5 is 0 here, which is what leaves the counter to the long way round. */
  program_standard();
  TEST_CHECK(run_to_row(10));
  run_characters(10);
  write_register(4, 3);

  uint8_t highest = 0;
  for (int scanline = 0; scanline < 8 * 130; scanline++) {
    run_scanlines(1);
    if (crtc.c4 > highest) {
      highest = crtc.c4;
    }
    if (crtc.c4 == 0) {
      break;
    }
  }
  TEST_EQUAL(highest, 127);
  TEST_EQUAL(crtc.c4, 0);
}

static void the_vertical_adjustment_brings_c4_back_from_past_r4(void) {
  /* The overflow above is written "excluding vertical adjustment": with R5
     set, the line where C9 meets R9 begins the adjustment whatever C4 has
     climbed to, and finishing it returns C4 to 0 (ch. 12.1, 12.2). This is
     how a split screen resynchronises after moving R4 under its own row
     counter, instead of waiting out 117 rows. */
  program_standard();
  write_register(5, 10);
  TEST_CHECK(run_to_row(10));
  run_characters(10);
  write_register(4, 3);

  int scanlines = 0;
  while (crtc.c4 != 0 && scanlines < 200) {
    run_scanlines(1);
    scanlines++;
  }
  TEST_EQUAL(crtc.c4, 0);
  /* The rest of row 10, then C9 climbing from 8 to R5. */
  TEST_EQUAL(scanlines, 10);
}

static void the_last_line_holds_once_it_is_decided(void) {
  /* C4 and C9 are held against their limits while C0 is 0 or 1, and the
     chip stops asking after that: a register written later in the line can
     no longer take the state back (ch. 10.3.1.2, 12.2). */
  program_standard();
  TEST_CHECK(run_to_row(38)); /* R4: the frame's last row */
  run_scanlines(7);           /* its last scanline, where C9 meets R9 */
  TEST_EQUAL(crtc.c9, 7);
  run_characters(10);
  write_register(4, 0); /* C4 is no longer R4, and it no longer matters */
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.c9, 0);
}

static void a_late_write_can_still_end_the_frame(void) {
  /* The other direction of the same rule: a write after C0=1 that brings
     C4 and R4 together does set the state (ch. 10.3.1.2). */
  program_standard();
  TEST_CHECK(run_to_row(5));
  run_scanlines(7);
  run_characters(10);
  write_register(4, 5);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
}

static void the_sixty_hertz_table_makes_a_262_line_frame(void) {
  /* The firmware's other table, at &5D5 of the 6128 OS ROM: 32 rows of 8
     scanlines and six adjustment lines (ch. 11.2.2). */
  program_standard();
  TEST_EQUAL(frame_scanlines(), 312);
  write_register(4, 31);
  write_register(5, 6);
  write_register(7, 27);
  TEST_EQUAL(frame_scanlines(), 262);
  TEST_EQUAL(frame_scanlines(), 262);
}

static void one_vsync_per_equality_of_c4_and_r7(void) {
  /* R7 given the value C4 already holds starts a VSYNC where the beam
     stands. The same equality cannot start a second: C4 must move, or R7
     must be written again (ch. 16.3, 16.4.1). */
  program_standard();
  write_register(9, 31); /* rows long enough to hold a whole VSYNC */
  TEST_CHECK(run_to_row(1));
  TEST_CHECK(!crtc.vsync);

  write_register(7, 1);
  crtc_tick(&crtc);
  TEST_CHECK(crtc.vsync);
  run_scanlines(8); /* R3's high nibble */
  TEST_CHECK(!crtc.vsync);
  run_scanlines(10); /* still row 1, and still no second VSYNC */
  TEST_CHECK(!crtc.vsync);

  write_register(7, 1);
  crtc_tick(&crtc);
  TEST_CHECK(crtc.vsync);
}

static void the_r1_border_holds_until_the_line_begins_again(void) {
  /* The display opens where the line begins and shuts where C0 meets R1,
     and neither is a comparison standing (ch. 6.1.3, 17.1). Moving R1 out
     of C0's way afterwards cannot reopen the border, which is what makes
     ch. 17.3's trick work: R1 is moved during the border so the video
     pointer is carried forward without the data being shown. */
  program_standard();
  TEST_CHECK(run_to_row(1));
  run_characters(45); /* past R1=40, so the border has begun */
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  write_register(1, 50);
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  /* The next line opens it again. */
  run_scanlines(1);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);

  /* And R1 put below C0 cannot shut it: the equality never comes round. */
  program_standard();
  TEST_CHECK(run_to_row(1));
  run_characters(20);
  write_register(1, 10);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
  run_characters(30);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

static void the_r6_border_is_shut_for_the_whole_frame(void) {
  /* Where C4 meets R6 the border is immediate and final; only a new frame
     opens it, and while it is shut R1 has no say (ch. 18.2.1, 18.2.2). */
  program_standard();
  TEST_CHECK(run_to_row(26)); /* past R6=25 */
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  write_register(6, 30);
  run_scanlines(1);
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  /* The frame's first row has it open again. */
  TEST_CHECK(run_to_row(0));
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);

  /* R6 put below C4 cannot shut it either. */
  program_standard();
  TEST_CHECK(run_to_row(10));
  write_register(6, 5);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

static void an_r1_of_zero_leaves_the_line_displayed(void) {
  /* Both conditions land on the same character, and the document gives the
     opening priority (ch. 18.3.1). */
  program_standard();
  write_register(1, 0);
  TEST_CHECK(run_to_row(1));
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

static void a_c0_that_overflowed_does_not_open_the_display(void) {
  /* Only the C0 that returns to 0 from R0 opens it. One that got there by
     running past 255 does not (ch. 17.1). */
  program_standard();
  TEST_CHECK(run_to_row(1));
  run_characters(45); /* the R1 border has begun */
  write_register(0, 5);
  while (crtc.c0 != 0) {
    crtc_tick(&crtc);
  }
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  /* The next line runs its length, and that one does open it. */
  while (crtc.c0 != 0) {
    crtc_tick(&crtc);
  }
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(select_wears_five_bits);
  TEST_RUN(writes_wear_the_documented_widths);
  TEST_RUN(type0_reads_r12_to_r17_and_nothing_else);
  TEST_RUN(unselected_chip_ignores_the_bus);
  TEST_RUN(hsync_falls_where_r2_and_r3_put_it);
  TEST_RUN(vsync_holds_eight_scanlines_from_row_30);
  TEST_RUN(display_covers_40_by_200);
  TEST_RUN(the_video_pointer_walks_the_documented_rows);
  TEST_RUN(the_frame_locks_at_19968);
  TEST_RUN(c9_runs_to_its_own_top_when_r9_drops_below_it);
  TEST_RUN(c4_runs_to_its_own_top_when_r4_drops_below_it);
  TEST_RUN(the_vertical_adjustment_brings_c4_back_from_past_r4);
  TEST_RUN(the_last_line_holds_once_it_is_decided);
  TEST_RUN(a_late_write_can_still_end_the_frame);
  TEST_RUN(the_sixty_hertz_table_makes_a_262_line_frame);
  TEST_RUN(one_vsync_per_equality_of_c4_and_r7);
  TEST_RUN(the_r1_border_holds_until_the_line_begins_again);
  TEST_RUN(the_r6_border_is_shut_for_the_whole_frame);
  TEST_RUN(an_r1_of_zero_leaves_the_line_displayed);
  TEST_RUN(a_c0_that_overflowed_does_not_open_the_display);
  return TEST_REPORT("crtc");
}
