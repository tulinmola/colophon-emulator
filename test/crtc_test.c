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

/* Whole scanlines from wherever C0 stands. C0 names the character just
   drawn, so a run leaves the alignment it found. */
static void run_scanlines(int count) { run_characters(count * SCANLINE); }

/* Stop having drawn the first character of the row named, however long the
   chip takes to get there; 4000 scanlines is a dozen frames and a failed
   loop. */
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
  /* The counters name the character just drawn, and at power-on they name
     none — which is why the frame's first character cannot be told from the
     state before it by C0, C4 and C9 alone. Runs to that character, then
     counts the ticks back round to it. */
  while (!crtc.has_drawn_a_character || crtc.c0 != 0 || crtc.c4 != 0 || crtc.c9 != 0) {
    crtc_tick(&crtc);
  }
  long ticks = 0;
  do {
    crtc_tick(&crtc);
    ticks++;
  } while (crtc.c0 != 0 || crtc.c4 != 0 || crtc.c9 != 0);
  return ticks / SCANLINE;
}

/* What the next VSYNC to rise looked like: the character C0 named when it
   rose, the row it fell in and the raster address that row had reached,
   whether the chip called the frame odd, and how many characters it stayed
   up counting the one it rose on. */
typedef struct {
  uint8_t character;
  uint8_t row;
  uint8_t raster;
  bool odd_frame;
  long characters;
} vsync_seen;

/* Gives up after four frames. */
static bool next_vsync(vsync_seen *seen) {
  for (long tick = 0; tick < 4L * FRAME_TICKS; tick++) {
    bool standing = crtc.vsync;
    uint64_t pins = crtc_tick(&crtc);
    if (standing || (pins & CRTC_VSYNC) == 0) {
      continue;
    }
    seen->character = crtc.c0;
    seen->row = crtc.c4;
    seen->raster = crtc_ra(pins);
    seen->odd_frame = crtc.parity_frame;
    seen->characters = 1;
    while (crtc.vsync && seen->characters < 18L * SCANLINE) {
      crtc_tick(&crtc);
      if (crtc.vsync) {
        seen->characters++;
      }
    }
    return true;
  }
  return false;
}

/* A scanline as the Compendium's counting tables print one: the row, the
   counter, and the raster address the chip put on RA (ch. 19.8.1). */
typedef struct {
  uint8_t c4;
  uint8_t c9;
  uint8_t raster;
} recorded_line;

/* An R8 UPDATE where those tables mark one: a value written inside the line
   at the given index. */
typedef struct {
  int line;
  uint8_t value;
} r8_update;

#define MAX_RECORDED_LINES 24

/* Records the head of each of the next scanlines, entering on the character
   C0 names 0 and leaving on the one after the last line recorded. The
   raster address is read a character into the line, where it is the same as
   at its head: C9 moves only where a line ends. */
static void record_lines(recorded_line *lines, int count, const r8_update *updates,
                         int update_count) {
  for (int index = 0; index < count; index++) {
    lines[index].c4 = crtc.c4;
    lines[index].c9 = crtc.c9;
    lines[index].raster = crtc_ra(crtc_tick(&crtc));
    for (int update = 0; update < update_count; update++) {
      if (updates[update].line == index) {
        write_register(8, updates[update].value);
      }
    }
    run_characters(SCANLINE - 1);
  }
}

/* Against a table transcribed from the Compendium: C4, C9, C9-VMA a row.
   Reports the first line that parts company and stops, because after one
   the rest say the same thing again. */
static void check_lines(const char *table, const recorded_line *got, const uint8_t (*want)[3],
                        int count) {
  for (int index = 0; index < count; index++) {
    if (got[index].c4 == want[index][0] && got[index].c9 == want[index][1] &&
        got[index].raster == want[index][2]) {
      continue;
    }
    TEST_FAIL("%s, line %d: C4=%u C9=%u C9-VMA=%u, where the table says %u, %u, %u", table, index,
              got[index].c4, got[index].c9, got[index].raster, want[index][0], want[index][1],
              want[index][2]);
    return;
  }
}

/* Stands on the first character of a frame of the parity asked for, with the
   standard values but for R9. Callers name the parity through the two
   wrappers below rather than by a bare true or false. */
static void stand_on_a_frame(bool odd_frame, uint8_t r9) {
  program_standard();
  write_register(9, r9);
  frame_scanlines();
  if (crtc.parity_frame != odd_frame) {
    frame_scanlines();
  }
  TEST_CHECK(crtc.parity_frame == odd_frame);
  TEST_CHECK(crtc.c0 == 0 && crtc.c4 == 0 && crtc.c9 == 0);
}

static void stand_on_an_even_frame(uint8_t r9) { stand_on_a_frame(false, r9); }
static void stand_on_an_odd_frame(uint8_t r9) { stand_on_a_frame(true, r9); }

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
  TEST_EQUAL(crtc.c9, 2); /* the scanline just drawn, the third of the row */
  run_characters(10);     /* past C0=2, where the last line is decided */
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

static void a_late_write_cannot_end_the_frame(void) {
  /* The other direction of the same rule: the test is not repeated once C0
     is past 1, so a write that brings C4 and R4 together later in the line
     leaves the row counter to go on climbing (ch. 12.2). */
  program_standard();
  TEST_CHECK(run_to_row(5));
  run_scanlines(7);
  run_characters(10);
  write_register(4, 5);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 6);
}

/* R5 raised on the last line while C0 has not passed 2 is considered and
   the lines it asks for are added; raised later it is not (ch. 11.2.2,
   11.4.2, 13.2.1). A write lands the microsecond after the character it was
   made in, so this one is made in the character C0 names 2. */
static void an_r5_asked_for_in_time_adds_its_lines(void) {
  program_standard();
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  run_characters(2);
  write_register(5, 4);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 39);
}

/* And the disarming, which the case above cannot see: with no adjustment
   left to spend, R4 moved later in the line changes nothing, because the
   last line it stood in front of was never taken away (ch. 12.2, 13.2.5). */
static void a_cancelled_r5_leaves_the_last_line_standing(void) {
  program_standard();
  write_register(5, 4);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  run_characters(1);
  write_register(5, 0);
  run_characters(9);
  write_register(4, 3);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.c9, 0);
}

/* The Compendium tabulates a type 0 adjustment of its own (ch. 11.2.1):
   R4=10, R5=16, R9=3, and sixteen lines all carrying C4=11 while C9 runs 0
   to 15 — C4 incremented once whatever R5 holds, and both counters back to
   0 after (ch. 13.2.4). Transcribed from the table rather than from us. */
static void the_documented_sixteen_line_adjustment(void) {
  program_standard();
  write_register(4, 10);
  write_register(5, 16);
  write_register(9, 3);
  TEST_CHECK(run_to_row(10));
  run_scanlines(3);
  for (int line = 0; line < 16; line++) {
    run_scanlines(1);
    TEST_EQUAL(crtc.c4, 11);
    TEST_EQUAL(crtc.c9, line);
  }
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.c9, 0);
}

/* A last line whose comparison no longer holds, because R9 moved under it
   at C0=1, spends itself on an adjustment instead of ending the frame, and
   no R5 above 0 is wanted for that (ch. 10.3.1.2). The numbers are the
   Compendium's own third example in ch. 11.2.2 — C4 and C9 at 38 and 7 with
   R9 taken to 6 — and so is the answer: "The next line is then C4==38,
   C9==8." C9 climbs past R9 where without the rule both would return to 0,
   and C4 stays where it is because the row never reached its last line. */
static void r9_moved_at_c0_1_makes_the_line_an_adjustment(void) {
  program_standard();
  write_register(5, 0);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  run_characters(1);
  write_register(9, 6);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 38);
  TEST_EQUAL(crtc.c9, 8);
}

/* R4 is the other half of the same rule, and moving it does the same. */
static void r4_moved_at_c0_1_makes_the_line_an_adjustment(void) {
  program_standard();
  write_register(5, 0);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  run_characters(1);
  write_register(4, 20);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 38);
  TEST_EQUAL(crtc.c9, 8);
}

/* And the deadline the other way about: a move the chip sees after C0 has
   passed 2 can neither take the last line back nor make an adjustment of
   it, so the frame ends where it stood (ch. 10.3.1.2, 12.2). */
static void a_late_write_cannot_begin_an_adjustment(void) {
  program_standard();
  write_register(5, 0);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  run_characters(2);
  write_register(4, 20);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.c9, 0);
}

/* An R5 of 0 is a quantity of no lines, so a frame whose adjustment is
   cancelled ends where it would have ended without one (ch. 13.2.4). */
static void an_r5_cancelled_in_time_adds_no_line(void) {
  program_standard();
  write_register(5, 4);
  TEST_EQUAL(frame_scanlines(), 316);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  run_characters(1);
  write_register(5, 0);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
}

/* Either interlace mode adds one line to the end of the even frames — the
   parity R6 anticipated, which turns odd where C4 reaches R6 on an even one
   (ch. 11.9, 19.5.2, 19.6.1). So a pair of frames runs 312 lines and 313,
   the 19968 and 20032 microseconds of ch. 19.3.1, averaging the 20000 an
   interlaced field wants. That chapter calls the long one the odd frame,
   counting from one VSYNC to the next; these frames are counted from one
   C4=C9=C0=0 to the next, and the line falls after the VSYNC, which is what
   ch. 19.3.1 means by the odd frame inheriting it. */
static void an_interlace_mode_adds_a_line_to_the_even_frames(void) {
  program_standard();
  TEST_EQUAL(frame_scanlines(), 312);
  TEST_EQUAL(frame_scanlines(), 312);

  write_register(8, 1);
  TEST_CHECK(!crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 313);
  TEST_CHECK(crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 312);
  TEST_CHECK(!crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 313);

  /* It comes after the lines R5 asks for, and C4 is incremented once for
     all of them together (ch. 11.9, 19.6.1). */
  write_register(5, 6);
  TEST_EQUAL(frame_scanlines(), 318);
  TEST_EQUAL(frame_scanlines(), 319);
  write_register(5, 0);
  TEST_EQUAL(frame_scanlines(), 312);

  /* R6 put above R4 while ParityR6 stands odd freezes it odd, because C4
     can no longer reach it — and then every frame takes a line, rather than
     every other (ch. 19.6.1). */
  TEST_CHECK(run_to_row(26));
  TEST_CHECK(crtc.parity_r6);
  write_register(6, 40);
  TEST_EQUAL(frame_scanlines(), 313);
  TEST_EQUAL(frame_scanlines(), 313);
  TEST_EQUAL(frame_scanlines(), 313);
}

/* On an even frame in either interlace mode the VSYNC is a MID-VSYNC: the
   C4/R7 equality does not raise it where it falls but where C0 reaches
   R0/2, which is the half line the second field is raised by. It begins
   away from the head of a line, so R3's eight are counted from the head of
   the next one and the rest of this one runs on top of them (ch. 16.4.1,
   19.7.1, 19.7.2). */
static void an_interlace_mode_holds_the_even_frames_vsync_back(void) {
  vsync_seen seen;

  program_standard();
  TEST_CHECK(next_vsync(&seen));
  TEST_EQUAL(seen.character, 0);
  TEST_EQUAL(seen.characters, 8 * SCANLINE);

  write_register(8, 1);
  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(seen.odd_frame);
  TEST_EQUAL(seen.character, 0);
  TEST_EQUAL(seen.characters, 8 * SCANLINE);

  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(!seen.odd_frame);
  TEST_EQUAL(seen.character, 31); /* R0/2, R0 being 63 */
  TEST_EQUAL(seen.characters, 8 * SCANLINE + SCANLINE - 31);
}

/* An R6 of 0 puts the parity's turn on the frame's own first character,
   where ParityFrame is settled too. ParityFrame is settled first and
   ParityR6 is read from it, so ParityR6 answers the frame that has just
   begun rather than the one that just ended, and the line still falls on
   the even frame (ch. 19.5.2, 19.6.1). Four scanlines a frame, so the extra
   one is a fifth. */
static void an_r6_of_zero_keeps_the_line_on_the_even_frame(void) {
  program_standard();
  write_register(4, 0);
  write_register(5, 0);
  write_register(6, 0);
  write_register(9, 3);
  write_register(8, 1);
  crtc_tick(&crtc);
  TEST_CHECK(!crtc.parity_frame);

  TEST_EQUAL(frame_scanlines(), 5);
  TEST_CHECK(crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 4);
  TEST_CHECK(!crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 5);
}

/* The interlace line is not the adjustment's to give back. A frame that has
   had its line has had it, however the adjustment carrying it ends, and a
   frame that has not must still get one — so what spends the line is
   renewed at a frame's first character and nowhere else. A chip that
   renewed it on the disarm would let one frame take two lines; one that
   never renewed it would withhold a later frame's without a word (ch. 11.9,
   13.2.1, 19.6.1). */
static void a_disarmed_adjustment_cannot_take_a_second_interlace_line(void) {
  program_standard();
  write_register(4, 0);
  write_register(5, 0);
  write_register(6, 0);
  write_register(9, 3);
  write_register(8, 1);
  crtc_tick(&crtc); /* the priming tick, which draws no character */
  int characters = 0;

  run_scanlines(4);
  characters += 4 * SCANLINE;
  TEST_CHECK(crtc.in_vertical_adjustment);
  TEST_CHECK(crtc.interlace_line_given);
  TEST_EQUAL(crtc.c4, 1); /* R4+1, incremented once for the whole of it */

  /* R4 and R9 moved onto the counters in time for the character C0 names 3,
     which takes the adjustment back under a line already given. R8 is taken
     back with them, because the disarm tests it too (ch. 13.2.1). */
  write_register(4, 1);
  write_register(9, 0);
  write_register(8, 0);
  run_characters(4);
  characters += 4;
  TEST_CHECK(!crtc.in_vertical_adjustment);
  TEST_CHECK(crtc.interlace_line_given);

  /* So the frame ends with the one line it was owed — its own four and the
     interlace line — where a chip that renewed the flag on the disarm would
     ask again and go round for another. */
  for (int character = 0; character < 4 * SCANLINE; character++) {
    if (crtc.c0 == 0 && crtc.c4 == 0 && crtc.c9 == 0) {
      break;
    }
    crtc_tick(&crtc);
    characters++;
  }
  TEST_EQUAL(characters, 5 * SCANLINE);
  TEST_CHECK(!crtc.interlace_line_given);
}

/* Stops on the last line a frame has, with R5's own adjustment lines
   already behind it and the frame's parity even, which is where ch. 11.9
   puts its question. The two frames run first settle the parity, which
   alternates whether or not interlace is asked for. */
static void stand_on_the_last_line_of_an_even_frame(uint8_t r5) {
  program_standard();
  write_register(5, r5);
  TEST_EQUAL(frame_scanlines(), 312 + r5);
  TEST_EQUAL(frame_scanlines(), 312 + r5);
  TEST_CHECK(!crtc.parity_frame);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7 + r5);
}

/* Ch. 11.9 gives the interlace line a deadline of its own, and a later one
   than the three microseconds R5 is read in: "The adjustment condition
   (interlace mode (IVM/non-IVM) activated and even frame) is evaluated on
   the last line of a frame, when C0=R0, and only if R8 contains the right
   value on the last line." So a write in force during that character
   decides the frame, and one made during it arrives a microsecond late. */
static void the_interlace_line_is_asked_for_at_c0_r0(void) {
  stand_on_the_last_line_of_an_even_frame(0);
  run_characters(62);
  TEST_EQUAL(crtc.c0, 62);
  write_register(8, 1); /* in force from C0=63, which is R0 */
  run_characters(2);
  TEST_EQUAL(crtc.c0, 0);
  TEST_EQUAL(crtc.c4, 39); /* the interlace line, C4 incremented once */

  stand_on_the_last_line_of_an_even_frame(0);
  run_characters(63);
  TEST_EQUAL(crtc.c0, 63);
  write_register(8, 1); /* in force from C0=0, a microsecond too late */
  run_characters(1);
  TEST_EQUAL(crtc.c0, 0);
  TEST_EQUAL(crtc.c4, 0); /* and the frame ended where it stood */
}

/* "This latest line can be one of the adjustment lines displayed via R5. It
   is therefore possible to update R8 on one of the lines displayed via R5
   to activate or deactivate the treatment of the interlace line" (ch.
   11.9). The question is put afresh on every line, so what answers it is
   the last line before the one it would add — a line R5's own window shut
   several lines before. */
static void an_adjustment_line_still_decides_the_interlace_line(void) {
  /* Asked for on the last of R5's lines, and given. */
  stand_on_the_last_line_of_an_even_frame(2);
  TEST_EQUAL(crtc.c4, 39);
  TEST_EQUAL(crtc.c9, 1);
  run_characters(62);
  write_register(8, 1);
  run_characters(2);
  TEST_EQUAL(crtc.c9, 2); /* the interlace line, after R5's */

  /* And taken back there, though it stood through every line before. R8 is
     set before the frames are counted, so the even one runs its 312 lines,
     R5's two and the interlace line, and the odd one goes without. */
  program_standard();
  write_register(5, 2);
  write_register(8, 1);
  TEST_CHECK(!crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 315);
  TEST_EQUAL(frame_scanlines(), 314);
  TEST_CHECK(!crtc.parity_frame);
  TEST_CHECK(run_to_row(38));
  run_scanlines(9);
  run_characters(62);
  write_register(8, 0);
  run_characters(2);
  TEST_EQUAL(crtc.c4, 0); /* the frame ended with R5's lines and no more */

  /* And the deadline is the same one here as on a line of R5's own: asked
     for during the last adjustment line's own C0=R0, it is asked too late,
     though the line it would add begins on the very next character. */
  stand_on_the_last_line_of_an_even_frame(2);
  run_characters(63);
  TEST_EQUAL(crtc.c0, 63);
  write_register(8, 1);
  run_characters(1);
  TEST_EQUAL(crtc.c4, 0);
}

/* The disarm at C0=2 tests both of the things that can ask for an
   additional line: "the additional management state is deactivated if there
   was no line programmed (R5=0 or no 'Interlace Line'" (ch. 13.2.1, and
   again in 13.2.5 as "test of R5 and/or 'Interlace on even frame
   programmed'"). So a frame the interlace still asks a line of keeps its
   state whatever R5 has become. It must, too: the arm this would drop is
   not the one C0=R0 can put back, because that one reads the last line as
   it was settled while C0 was 0 and 1, and an R9 moved in between leaves
   the two disagreeing. A chip that dropped it would lose the adjustment
   with nothing to restore it and run C4 on past R4 for hundreds of lines. */
static void the_disarm_tests_the_interlace_line_as_well_as_r5(void) {
  program_standard();
  write_register(8, 1);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7); /* the frame's last row line */
  TEST_CHECK(crtc.parity_r6);
  write_register(9, 8); /* in force from C0=1, where no last line is seen */
  crtc_tick(&crtc);
  write_register(9, 7);
  write_register(5, 2); /* in force from C0=2 */
  crtc_tick(&crtc);
  write_register(5, 0); /* in force from C0=3, where the disarm looks */

  int characters = 0;
  for (int character = 0; character < 8 * SCANLINE; character++) {
    crtc_tick(&crtc);
    characters++;
    if (crtc.c0 == 0 && crtc.c4 == 0 && crtc.c9 == 0) {
      break;
    }
  }
  TEST_EQUAL(characters, 126);
}

/* C4 is seven bits wide, so an R4 of 127 puts the interlace line's own
   C4=R4+1 back at 0 — and with C9 zeroed for that line and C0 at its head,
   the line reads exactly like a frame's first character. A chip that took
   its frame's head from the counters rather than from the state machine
   would renew the line under the line it had just given, and never end a
   frame again (ch. 10.3.1.1, 12.1). */
static void an_r4_of_127_does_not_trap_the_frame(void) {
  program_standard();
  write_register(4, 127);
  write_register(9, 1);
  write_register(8, 1);
  int in_the_adjustment = 0;
  bool entered = false;
  for (long character = 0; character < 600L * SCANLINE; character++) {
    crtc_tick(&crtc);
    if (crtc.in_vertical_adjustment) {
      entered = true;
      in_the_adjustment++;
    } else if (entered) {
      break;
    }
  }
  /* The whole interlace line and the C0=R0 that held the frame open for
     it, which stands a character before the line begins. */
  TEST_EQUAL(in_the_adjustment, SCANLINE + 1);
}

/* ParityFrame is settled before the VSYNC is looked at, which only shows
   where R7 is 0 and the two fall on the same character: the frame that has
   just become even takes a MID-VSYNC in its own first line (ch. 19.7.2). */
static void the_parity_settles_before_an_r7_of_zero_is_read(void) {
  vsync_seen seen;

  program_standard();
  write_register(7, 0);
  write_register(8, 1);
  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(!seen.odd_frame);
  TEST_EQUAL(seen.character, 31);

  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(seen.odd_frame);
  TEST_EQUAL(seen.character, 0);
}

/* Ch. 19.8.1's counting tables for R9=6, transcribed. From the line after
   the one R8 is given 3 on, the raster address is C9 doubled with parity in
   bit 0, and the row ends where that address reaches R9 read up to the same
   parity — so a row is four lines where it was seven, and the two frames
   address alternate lines. */
static void the_video_mode_doubles_the_raster_address(void) {
  recorded_line got[MAX_RECORDED_LINES];
  static const r8_update on_the_first_line[] = {{0, 3}};

  static const uint8_t even_from_the_first_line[8][3] = {
      {0, 0, 0}, {0, 1, 2}, {0, 2, 4}, {0, 3, 6}, {1, 0, 0}, {1, 1, 2}, {1, 2, 4}, {1, 3, 6}};
  stand_on_an_even_frame(6);
  record_lines(got, 8, on_the_first_line, 1);
  check_lines("R8=3 on C9=0, even frame", got, even_from_the_first_line, 8);

  static const uint8_t odd_from_the_first_line[8][3] = {{0, 0, 0}, {0, 1, 3}, {0, 2, 5}, {0, 3, 7},
                                                        {1, 0, 1}, {1, 1, 3}, {1, 2, 5}, {1, 3, 7}};
  stand_on_an_odd_frame(6);
  record_lines(got, 8, on_the_first_line, 1);
  check_lines("R8=3 on C9=0, odd frame", got, odd_from_the_first_line, 8);

  /* The doubling waits for the head of the next line, so the line the mode
     is asked for on still addresses itself undoubled. */
  static const r8_update on_the_second_line[] = {{1, 3}};
  static const uint8_t even_from_the_second_line[8][3] = {
      {0, 0, 0}, {0, 1, 1}, {0, 2, 4}, {0, 3, 6}, {1, 0, 0}, {1, 1, 2}, {1, 2, 4}, {1, 3, 6}};
  stand_on_an_even_frame(6);
  record_lines(got, 8, on_the_second_line, 1);
  check_lines("R8=3 on C9=1, even frame", got, even_from_the_second_line, 8);

  static const uint8_t odd_from_the_second_line[8][3] = {
      {0, 0, 0}, {0, 1, 1}, {0, 2, 5}, {0, 3, 7}, {1, 0, 1}, {1, 1, 3}, {1, 2, 5}, {1, 3, 7}};
  stand_on_an_odd_frame(6);
  record_lines(got, 8, on_the_second_line, 1);
  check_lines("R8=3 on C9=1, odd frame", got, odd_from_the_second_line, 8);

  static const r8_update on_the_third_line[] = {{2, 3}};
  static const uint8_t even_from_the_third_line[8][3] = {
      {0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 6}, {1, 0, 0}, {1, 1, 2}, {1, 2, 4}, {1, 3, 6}};
  stand_on_an_even_frame(6);
  record_lines(got, 8, on_the_third_line, 1);
  check_lines("R8=3 on C9=2, even frame", got, even_from_the_third_line, 8);

  static const uint8_t odd_from_the_third_line[8][3] = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 7},
                                                        {1, 0, 1}, {1, 1, 3}, {1, 2, 5}, {1, 3, 7}};
  stand_on_an_odd_frame(6);
  record_lines(got, 8, on_the_third_line, 1);
  check_lines("R8=3 on C9=2, odd frame", got, odd_from_the_third_line, 8);
}

/* Parity joins the limit the moment R8 is written, a line before the
   doubling does, and a row that has already passed the line the new limit
   names cannot end on it: C9 climbs until the doubled address comes round
   through five bits. This is one of the two counting bugs ch. 19.5.2 offers
   a program as a way of reading the parity it is on. */
static void a_video_mode_entered_late_overflows_c9(void) {
  recorded_line got[MAX_RECORDED_LINES];

  static const r8_update on_the_fourth_line[] = {{3, 3}};
  static const uint8_t even_overflow[24][3] = {
      {0, 0, 0},   {0, 1, 1},   {0, 2, 2},   {0, 3, 3},   {0, 4, 8},   {0, 5, 10},
      {0, 6, 12},  {0, 7, 14},  {0, 8, 16},  {0, 9, 18},  {0, 10, 20}, {0, 11, 22},
      {0, 12, 24}, {0, 13, 26}, {0, 14, 28}, {0, 15, 30}, {0, 16, 0},  {0, 17, 2},
      {0, 18, 4},  {0, 19, 6},  {1, 0, 0},   {1, 1, 2},   {1, 2, 4},   {1, 3, 6}};
  stand_on_an_even_frame(6);
  record_lines(got, 24, on_the_fourth_line, 1);
  check_lines("R8=3 on C9=3, even frame", got, even_overflow, 24);

  static const uint8_t odd_overflow[24][3] = {
      {0, 0, 0},   {0, 1, 1},   {0, 2, 2},   {0, 3, 3},   {0, 4, 9},   {0, 5, 11},
      {0, 6, 13},  {0, 7, 15},  {0, 8, 17},  {0, 9, 19},  {0, 10, 21}, {0, 11, 23},
      {0, 12, 25}, {0, 13, 27}, {0, 14, 29}, {0, 15, 31}, {0, 16, 1},  {0, 17, 3},
      {0, 18, 5},  {0, 19, 7},  {1, 0, 1},   {1, 1, 3},   {1, 2, 5},   {1, 3, 7}};
  stand_on_an_odd_frame(6);
  record_lines(got, 24, on_the_fourth_line, 1);
  check_lines("R8=3 on C9=3, odd frame", got, odd_overflow, 24);

  /* Asked for on the row's last line, an even frame ends the row where it
     would have ended anyway — and an odd one finds the limit a line higher
     and overruns the whole way round.

     The even table prints C9 as 0, 2, 4, 6 for the row after, where the
     switching tables print 0, 1, 2, 3 for rows of the same shape and every
     exit table shows C9 carrying on by one. C9 is a counter: its column is
     corrected here, and the C9-VMA column, which is what the chip puts on
     RA, is copied as printed. */
  static const r8_update on_the_last_line[] = {{6, 3}};
  static const uint8_t even_on_r9[11][3] = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 3},
                                            {0, 4, 4}, {0, 5, 5}, {0, 6, 6}, {1, 0, 0},
                                            {1, 1, 2}, {1, 2, 4}, {1, 3, 6}};
  stand_on_an_even_frame(6);
  record_lines(got, 11, on_the_last_line, 1);
  check_lines("R8=3 on C9=R9, even frame", got, even_on_r9, 11);

  static const uint8_t odd_on_r9[24][3] = {
      {0, 0, 0},   {0, 1, 1},   {0, 2, 2},   {0, 3, 3},   {0, 4, 4},   {0, 5, 5},
      {0, 6, 6},   {0, 7, 15},  {0, 8, 17},  {0, 9, 19},  {0, 10, 21}, {0, 11, 23},
      {0, 12, 25}, {0, 13, 27}, {0, 14, 29}, {0, 15, 31}, {0, 16, 1},  {0, 17, 3},
      {0, 18, 5},  {0, 19, 7},  {1, 0, 1},   {1, 1, 3},   {1, 2, 5},   {1, 3, 7}};
  stand_on_an_odd_frame(6);
  record_lines(got, 24, on_the_last_line, 1);
  check_lines("R8=3 on C9=R9, odd frame", got, odd_on_r9, 24);
}

/* Leaving, the two switches part company the other way about: the limit
   loses its parity at once and the address keeps its doubling for one line
   more. So a row left on an address that has passed R9 runs on to R9
   undoubled, and one left on R9 itself ends there.

   The Compendium's exit tables give the line R8 is asked for on a doubled
   address, where its switching tables and its own prose give that line an
   undoubled one — "from the line C9 which follows that where R8 goes to 3".
   The switching tables are followed here. */
static void leaving_the_video_mode_drops_the_parity_from_the_limit(void) {
  recorded_line got[MAX_RECORDED_LINES];

  static const r8_update left_on_the_rows_first_line[] = {{0, 3}, {4, 0}};
  static const uint8_t even_left_early[12][3] = {{0, 0, 0}, {0, 1, 2}, {0, 2, 4}, {0, 3, 6},
                                                 {1, 0, 0}, {1, 1, 1}, {1, 2, 2}, {1, 3, 3},
                                                 {1, 4, 4}, {1, 5, 5}, {1, 6, 6}, {2, 0, 0}};
  stand_on_an_even_frame(6);
  record_lines(got, 12, left_on_the_rows_first_line, 2);
  check_lines("R8=0 on C4=1 C9=0, even frame", got, even_left_early, 12);

  static const r8_update left_on_the_rows_last_line[] = {{0, 3}, {7, 0}};
  static const uint8_t even_left_on_r9[12][3] = {{0, 0, 0}, {0, 1, 2}, {0, 2, 4}, {0, 3, 6},
                                                 {1, 0, 0}, {1, 1, 2}, {1, 2, 4}, {1, 3, 6},
                                                 {2, 0, 0}, {2, 1, 1}, {2, 2, 2}, {2, 3, 3}};
  stand_on_an_even_frame(6);
  record_lines(got, 12, left_on_the_rows_last_line, 2);
  check_lines("R8=0 on C4=1 C9=3, even frame", got, even_left_on_r9, 12);

  /* The other counting bug: on an odd frame that address is R9+1, which the
     limit no longer reaches, so the row runs on. */
  static const uint8_t odd_left_on_r9[12][3] = {{0, 0, 0}, {0, 1, 3}, {0, 2, 5}, {0, 3, 7},
                                                {1, 0, 1}, {1, 1, 3}, {1, 2, 5}, {1, 3, 7},
                                                {1, 4, 4}, {1, 5, 5}, {1, 6, 6}, {2, 0, 0}};
  stand_on_an_odd_frame(6);
  record_lines(got, 12, left_on_the_rows_last_line, 2);
  check_lines("R8=0 on C4=1 C9=3, odd frame", got, odd_left_on_r9, 12);
}

/* With an odd R9 the parity of a row's lines turns with C4 as well as with
   the frame, so the rows come out alternately five lines and four and a
   pair of them holds the nine an R9 of 7 asks for — the balance that lets
   the two frames carry the same number of lines (ch. 19.5.2). */
static void an_odd_r9_gives_the_rows_alternating_parities(void) {
  recorded_line got[MAX_RECORDED_LINES];
  static const r8_update on_the_first_line[] = {{0, 3}};

  static const uint8_t even_frame[18][3] = {{0, 0, 0}, {0, 1, 2}, {0, 2, 4}, {0, 3, 6}, {0, 4, 8},
                                            {1, 0, 1}, {1, 1, 3}, {1, 2, 5}, {1, 3, 7}, {2, 0, 0},
                                            {2, 1, 2}, {2, 2, 4}, {2, 3, 6}, {2, 4, 8}, {3, 0, 1},
                                            {3, 1, 3}, {3, 2, 5}, {3, 3, 7}};
  stand_on_an_even_frame(7);
  record_lines(got, 18, on_the_first_line, 1);
  check_lines("R9=7, even frame", got, even_frame, 18);

  static const uint8_t odd_frame[18][3] = {{0, 0, 0}, {0, 1, 3}, {0, 2, 5}, {0, 3, 7}, {1, 0, 0},
                                           {1, 1, 2}, {1, 2, 4}, {1, 3, 6}, {1, 4, 8}, {2, 0, 1},
                                           {2, 1, 3}, {2, 2, 5}, {2, 3, 7}, {3, 0, 0}, {3, 1, 2},
                                           {3, 2, 4}, {3, 3, 6}, {3, 4, 8}};
  stand_on_an_odd_frame(7);
  record_lines(got, 18, on_the_first_line, 1);
  check_lines("R9=7, odd frame", got, odd_frame, 18);
}

/* An odd R9 in the video mode gives a row five lines on one parity and four
   on the other, so an odd C4 of an odd frame begins where an even frame's
   begins one line earlier. Its VSYNC waits that line out, rising at
   C9.VMA=2 — which is what still leaves the two frames' syncs half a line
   apart (ch. 19.5.2, 19.7.1). */
static void an_odd_row_of_an_odd_frame_takes_its_vsync_late(void) {
  vsync_seen seen;

  stand_on_an_odd_frame(7);
  write_register(7, 1);
  write_register(8, 3);
  TEST_CHECK(next_vsync(&seen));
  TEST_EQUAL(seen.row, 1);
  TEST_EQUAL(seen.raster, 2);

  /* On the even frame that same row runs its odd lines and starts at 1, so
     the sync falls on the row's own first line. */
  stand_on_an_even_frame(7);
  write_register(7, 1);
  write_register(8, 3);
  TEST_CHECK(next_vsync(&seen));
  TEST_EQUAL(seen.row, 1);
  TEST_EQUAL(seen.raster, 1);

  /* An even C4 waits for nothing on either frame. */
  stand_on_an_odd_frame(7);
  write_register(7, 2);
  write_register(8, 3);
  TEST_CHECK(next_vsync(&seen));
  TEST_EQUAL(seen.row, 2);
  TEST_EQUAL(seen.raster, 1);

  /* Nor does an odd C4 whose rows are an even number of lines. */
  stand_on_an_odd_frame(6);
  write_register(7, 1);
  write_register(8, 3);
  TEST_CHECK(next_vsync(&seen));
  TEST_EQUAL(seen.row, 1);
  TEST_EQUAL(seen.raster, 1);
}

/* VMA' is captured where C0 reaches R1 on the row's last scanline, and in
   the video mode that is the last of the doubled address's rather than the
   last C9 would name — ch. 19.8.1 says the test "also takes place when
   C0=R1 ... and the assignment of VMA' with VMA". So the video pointer
   still steps R1 characters a row where C9 never reaches R9 at all. */
static void the_video_pointer_still_steps_a_row_at_a_time(void) {
  stand_on_an_even_frame(6);
  write_register(8, 3);

  TEST_CHECK(run_to_row(1));
  uint16_t first = crtc.vma;
  TEST_CHECK(run_to_row(2));
  uint16_t second = crtc.vma;
  TEST_CHECK(run_to_row(3));
  uint16_t third = crtc.vma;
  TEST_EQUAL(second - first, 40); /* R1 */
  TEST_EQUAL(third - second, 40);
}

/* The vertical adjustment is armed on the row's last scanline too, so a
   frame in the video mode still takes the lines R5 asks for. Five rows of
   four lines and eight adjustment lines, and the even frame's interlace
   line on top of them (ch. 11.2.2, 19.6.1, 19.8.1). */
static void the_video_mode_still_takes_the_adjustment_lines(void) {
  stand_on_an_even_frame(6);
  write_register(4, 4);
  write_register(5, 8);
  write_register(6, 2);
  write_register(8, 3);
  TEST_EQUAL(frame_scanlines(), 29);
  TEST_EQUAL(frame_scanlines(), 28);
  TEST_EQUAL(frame_scanlines(), 29);
}

/* Ch. 17.6.2's own example: R0 of 3 and R1 of 4, so C0 never reaches R1 and
   the line displays throughout — except that the chip runs ahead of the
   characters the Gate Array draws and sends its border on half a character
   early, putting one byte of it before C0 goes to 0. */
static void a_line_r1_never_ends_borders_its_last_byte(void) {
  program_standard();
  write_register(0, 3);
  write_register(1, 4);
  TEST_CHECK(run_to_row(1));
  /* Two whole lines of it: every character displays, and the one C0 names 3
     gives its second byte to the border. */
  for (int character = 0; character < 8; character++) {
    uint64_t pins = crtc_tick(&crtc);
    TEST_CHECK(pins & CRTC_DISPTMG);
    TEST_EQUAL((pins & CRTC_DISPTMG_SECOND_BYTE) != 0, crtc.c0 != 3);
  }

  /* "This behaviour remains true whatever the value of R0. If R0=0, then
     the display alternates between 1 DISP ON byte, and 1 DISP OFF byte." */
  write_register(0, 0);
  for (int character = 0; character < 4; character++) {
    uint64_t pins = crtc_tick(&crtc);
    TEST_CHECK(pins & CRTC_DISPTMG);
    TEST_CHECK(!(pins & CRTC_DISPTMG_SECOND_BYTE));
  }
}

/* Ch. 18.3.2: an R6 of 0 on a frame's first line is a conflict — C4 reaching
   R6 asks for the border and the new frame takes it away — and the two land
   a byte apart, so that line alternates displayed and bordered bytes where
   every other line of such a frame is border throughout. */
static void an_r6_of_zero_alternates_the_first_lines_bytes(void) {
  program_standard();
  write_register(6, 0);
  TEST_CHECK(run_to_row(0));
  for (int character = 0; character < 8; character++) {
    uint64_t pins = crtc_tick(&crtc);
    TEST_CHECK(pins & CRTC_DISPTMG);
    TEST_CHECK(!(pins & CRTC_DISPTMG_SECOND_BYTE));
  }
  /* The second line of the frame is not the first, so R6 shuts it whole. */
  run_scanlines(1);
  uint64_t after = crtc_tick(&crtc);
  TEST_CHECK(!(after & CRTC_DISPTMG));
  TEST_CHECK(!(after & CRTC_DISPTMG_SECOND_BYTE));

  /* And it is the conflict that parts the bytes, not the line: an R6 above
     0 leaves a frame's first line whole. */
  program_standard();
  TEST_CHECK(run_to_row(0));
  for (int character = 0; character < 8; character++) {
    uint64_t pins = crtc_tick(&crtc);
    TEST_CHECK(pins & CRTC_DISPTMG);
    TEST_CHECK(pins & CRTC_DISPTMG_SECOND_BYTE);
  }

  /* Nor is it any row's first line: with R9 of 0 every row is one line long,
     and C4 leaves R6 behind after the first of them, so the conflict is the
     frame's own and the rows after it are whole. */
  program_standard();
  write_register(6, 0);
  write_register(9, 0);
  TEST_CHECK(run_to_row(1));
  for (int character = 0; character < 8; character++) {
    uint64_t pins = crtc_tick(&crtc);
    TEST_CHECK(pins & CRTC_DISPTMG);
    TEST_EQUAL((pins & CRTC_DISPTMG_SECOND_BYTE) != 0, crtc.c0 != 63);
  }
}

/* R8's bits 5 and 4 are the SKEW-DISPTMG delays, which ch. 19.1's table
   gives this type and withholds from types 1 and 2: one character or two
   on the R1 border's two edges. Ch. 19.2.3 states each delay as its own
   pair of rules. Of one microsecond: "The BORDER is deactivated on the 2nd
   character after that or C0=R0 (C0=1)" and "The BORDER is activated on
   the 1st character after the one where C0=R1". Of two: the 3rd character
   (C0=2) and the 2nd. Both are read here from the display window's two
   ends, and the window is walked round C0=0 so that the opening is
   evidence and not only the closing. */
static void the_skew_delays_the_border_at_both_ends(void) {
  static const struct {
    uint8_t r8;
    int opens_at;  /* the first character of the line that displays */
    int closes_at; /* the first that does not */
  } functions[] = {{0x00, 0, 40}, {0x10, 1, 41}, {0x20, 2, 42}};

  for (size_t index = 0; index < sizeof functions / sizeof functions[0]; index++) {
    program_standard();
    write_register(8, functions[index].r8);
    TEST_CHECK(run_to_row(1));
    /* run_to_row leaves the character C0 names 0 already drawn, so the line
       is walked from the one after it and C0 read back off the chip. */
    for (int character = 1; character <= 64; character++) {
      uint64_t pins = crtc_tick(&crtc);
      bool shown = crtc.c0 >= functions[index].opens_at && crtc.c0 < functions[index].closes_at;
      TEST_EQUAL((pins & CRTC_DISPTMG) != 0, shown);
    }
  }
}

/* Ch. 19.2.3's note: "If R1=R0, then the BORDER is activated on C0=0" — the
   delay carries the closing round the line's end, so the border is the
   line's first character and every other one displays. */
static void a_skew_carries_the_border_round_the_lines_end(void) {
  for (uint8_t skew = 1; skew <= 2; skew++) {
    program_standard();
    write_register(1, 63);
    write_register(8, (uint8_t)(skew << 4));
    TEST_CHECK(run_to_row(1));
    for (int character = 1; character <= 127; character++) {
      uint64_t pins = crtc_tick(&crtc);
      TEST_EQUAL((pins & CRTC_DISPTMG) != 0, crtc.c0 != skew - 1);
    }
  }
}

/* And where R1 was never reached, a delay turns the half character the chip
   sends early into a whole one at the deferred place (ch. 19.2.4). */
static void a_skew_makes_the_early_border_a_whole_character(void) {
  for (uint8_t skew = 1; skew <= 2; skew++) {
    program_standard();
    write_register(0, 3);
    write_register(1, 4);
    write_register(8, (uint8_t)(skew << 4));
    TEST_CHECK(run_to_row(1));
    for (int character = 1; character <= 15; character++) {
      uint64_t pins = crtc_tick(&crtc);
      /* R0 stands in for R1, so the border is the character the delay
         carries it to — C0=0 for one, C0=1 for two — and every other
         character is displayed whole. */
      TEST_EQUAL((pins & CRTC_DISPTMG) != 0, crtc.c0 != skew - 1);
      TEST_EQUAL((pins & CRTC_DISPTMG_SECOND_BYTE) != 0, crtc.c0 != skew - 1);
    }
  }
}

/* Ch. 19.2.1: the BORDER ON function shuts the display where it stands, and
   the video pointer goes on counting under it. */
static void the_border_on_function_shuts_the_display(void) {
  program_standard();
  TEST_CHECK(run_to_row(1));
  run_characters(4);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
  uint16_t before = crtc.vma;
  write_register(8, 0x30);
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG_SECOND_BYTE));
  TEST_EQUAL(crtc.vma, before + 2);
  write_register(8, 0x00);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

/* Ch. 19.2.5.3's first row, which is what tells a held-back signal from a
   moved comparison: the delay is taken off after the character it deferred
   the border to has already passed. A chip that moved its comparisons would
   never reach the opening and would lose the rest of the line; one that
   holds the signal back reads out the latch as it stood and opens on time.
   R0 and R1 are both 63, as that section has them. */
static void a_skew_taken_off_late_does_not_cost_the_line(void) {
  program_standard();
  write_register(1, 63);
  TEST_CHECK(run_to_row(1));
  while (crtc.c0 != 60) {
    crtc_tick(&crtc);
  }
  write_register(8, 0x10);                        /* in force from C0=61 */
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);    /* 61 */
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);    /* 62 */
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);    /* 63, the border deferred */
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG)); /* 0, and here it is */
  write_register(8, 0x00);                        /* in force from C0=1 */
  for (int character = 1; character <= 20; character++) {
    TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
  }
}

/* And the BORDER ON function is not a count: it shuts the display where it
   stands and leaves the R1 border where the registers put it, so a line it
   is lifted from resumes as that border says, and not three characters late
   as it would if its value were read as a count. Ch. 19.2.1 says only that
   the function does not affect the R6 border; that it leaves the R1 one
   alone as well is read from ch. 19.1's table, where it is the fourth value
   of the skew field and named Non-output rather than a third delay. */
static void the_border_on_function_moves_no_comparison(void) {
  program_standard();
  TEST_CHECK(run_to_row(1));
  write_register(8, 0x30);
  while (crtc.c0 != 40) {
    TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  }
  write_register(8, 0x00); /* in force from C0=41, past R1 */
  for (int character = 41; character <= 63; character++) {
    TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  }
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG); /* C0=0 opens the next line */
}

/* Ch. 19.2 gives the BORDER ON function the whole register: "if the BORDER
   ON function is activated, the INTERLACE function on the 2 least
   significant bits is not considered", and adds that the point wants
   further investigation. Read literally it silences all three of the things
   those bits ask for, and all three are read here. */
static void the_border_on_function_silences_the_interlace_bits(void) {
  vsync_seen seen;

  /* No added line: an interlace mode lengthens an even frame by one, and
     under BORDER ON it does not (ch. 19.6.1). */
  program_standard();
  write_register(8, 0x01);
  TEST_CHECK(!crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 313);
  program_standard();
  write_register(8, 0x31);
  TEST_CHECK(!crtc.parity_frame);
  TEST_EQUAL(frame_scanlines(), 312);

  /* No MID-VSYNC: every frame's VSYNC begins at the head of its line and
     runs the eight lines R3 asks for, rather than eight and a part. */
  program_standard();
  write_register(8, 0x31);
  TEST_CHECK(next_vsync(&seen));
  TEST_EQUAL(seen.character, 0);
  TEST_EQUAL(seen.characters, 8 * SCANLINE);
  bool first_parity = seen.odd_frame;
  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(seen.odd_frame != first_parity); /* the frames alternate still */
  TEST_EQUAL(seen.character, 0);
  TEST_EQUAL(seen.characters, 8 * SCANLINE);

  /* No doubled raster address either: the counting is the plain one, an R9
     of 6 giving seven scanlines a row and RA following C9. */
  recorded_line got[MAX_RECORDED_LINES];
  static const r8_update border_on_with_the_video_mode[] = {{0, 0x33}};
  static const uint8_t undoubled[8][3] = {{0, 0, 0}, {0, 1, 1}, {0, 2, 2}, {0, 3, 3},
                                          {0, 4, 4}, {0, 5, 5}, {0, 6, 6}, {1, 0, 0}};
  stand_on_an_even_frame(6);
  record_lines(got, 8, border_on_with_the_video_mode, 1);
  check_lines("R8=#33 on C9=0, even frame", got, undoubled, 8);
  stand_on_an_odd_frame(6);
  record_lines(got, 8, border_on_with_the_video_mode, 1);
  check_lines("R8=#33 on C9=0, odd frame", got, undoubled, 8);
}

/* Ch. 19.2.3 exempts the video pointer from the delay outright: "The video
   pointer assignment when C9=R9 does not change regardless of the
   programmed delay: VMA is always assigned with VMA' when C0 reaches R1".
   So a row below a delayed line begins where an undelayed one would, and
   the extra characters a delay shows on the right are, in the chapter's
   words, "the characters whose address will be reloaded at the beginning of
   the line". */
static void a_skew_does_not_move_the_video_pointer(void) {
  uint16_t rows[3];
  for (int index = 0; index < 3; index++) {
    program_standard();
    write_register(8, (uint8_t)(index << 4));
    TEST_CHECK(run_to_row(2));
    rows[index] = crtc.vma;
  }
  TEST_EQUAL(rows[1], rows[0]);
  TEST_EQUAL(rows[2], rows[0]);
}

/* Ch. 19.2.4 keys the substitution on "the condition C0=R1 not being met
   during the line", and ch. 19.2.5 repeats it: "The condition C0=R1 is just
   replaced by the condition C0=R0 in this case." R1 standing beyond the
   line's end is the plainest way to miss the condition but not the only
   one — an R1 of 0 loses to the opening on the character they share (ch.
   18.3.1) — and a line that misses it any way earns the same border: half a
   character early with no delay, a whole one at the deferred place with
   one. */
static void every_way_of_missing_r1_borders_alike(void) {
  static const uint8_t missing_r1[] = {64, 0};

  for (size_t way = 0; way < sizeof missing_r1 / sizeof missing_r1[0]; way++) {
    for (uint8_t skew = 0; skew <= 2; skew++) {
      program_standard();
      write_register(1, missing_r1[way]);
      write_register(8, (uint8_t)(skew << 4));
      TEST_CHECK(run_to_row(1));
      for (int character = 1; character <= 64; character++) {
        uint64_t pins = crtc_tick(&crtc);
        bool border = skew != 0 && crtc.c0 == skew - 1;
        bool half = skew == 0 && crtc.c0 == 63;
        TEST_EQUAL((pins & CRTC_DISPTMG) != 0, !border);
        TEST_EQUAL((pins & CRTC_DISPTMG_SECOND_BYTE) != 0, !border && !half);
      }
    }
  }
}

/* An R1 moved behind C0 is missed for the rest of that line just as surely,
   and the chip has no memory of the value it held (ch. 19.2.5's "the last
   value programmed on the line is the one present when the 1st OUT is
   performed on the following line"). */
static void an_r1_moved_behind_c0_earns_the_missed_border(void) {
  program_standard();
  write_register(8, 0x10);
  TEST_CHECK(run_to_row(1));
  while (crtc.c0 != 30) {
    crtc_tick(&crtc);
  }
  write_register(1, 20); /* behind C0, and never met again this line */
  while (crtc.c0 != 63) {
    TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
  }
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG)); /* C0=0, the deferred byte */
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

/* Ch. 19.2.5.1, the row where the cancelling write lands latest: "SKEW
   DISP+1 is cancelled on C0<=63. BORDER management is handled with the 2nd
   OUT. The programming of the 1st OUT (R8=#10) is ignored." So a delay
   cancelled while C0=63 is drawn leaves the border on C0=63, where no delay
   would have put it. One microsecond later is 19.2.5.2 instead, and the
   byte disappears altogether — which is what that chapter is named for. */
static void a_skew_cancelled_in_time_leaves_the_border_where_it_was(void) {
  program_standard();
  write_register(1, 63);
  write_register(8, 0x10);
  TEST_CHECK(run_to_row(1));
  while (crtc.c0 != 62) {
    TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
  }
  write_register(8, 0x00);                        /* in force from C0=63 */
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG)); /* C0=63 borders */
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);    /* and C0=0 does not */
}

/* Ch. 19.2.1: BORDER ON "does not affect the BORDER R6 state (when C4=R6)
   and it is therefore possible to switch to one of the 3 other available
   states". So an R6 border thrown while the display was shut is still
   thrown when the function is lifted, and nothing but a new frame opens it
   (ch. 18.2.2). Ch. 19.2.2 asks the question the other way round — whether
   R8=0 can cancel a BORDER R6 — and marks it "To be tested"; here it
   cannot, which is our reading and not the documentation's answer. */
static void the_border_on_function_leaves_the_r6_border_standing(void) {
  program_standard();
  write_register(8, 0x30);
  TEST_CHECK(run_to_row(26)); /* past R6, where the R6 border is thrown */
  run_characters(4);
  write_register(8, 0x00);
  for (int character = 0; character < 8; character++) {
    TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG));
  }
  TEST_CHECK(run_to_row(0));
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
}

/* The substitution needs a character to put the border in, and only a delay
   provides one. Whether the BORDER ON function provides one too is asked
   nowhere: while it stands the display is shut, so on the line itself the
   two readings agree. They part on the line's last character, where a
   substitution would raise a latch that a delay written at C0=0 then hands
   out. We read the function as ch. 19.1's table names it — Non-output, the
   fourth value of the field rather than a third delay — so it earns no
   character, and a line it stood on opens like any other. */
static void the_border_on_function_earns_no_deferred_character(void) {
  for (uint8_t standing = 0x00; standing <= 0x30; standing = (uint8_t)(standing + 0x30)) {
    program_standard();
    write_register(1, 64); /* C0 never meets R1 */
    write_register(8, standing);
    TEST_CHECK(run_to_row(1));
    while (crtc.c0 != 63) {
      crtc_tick(&crtc);
    }
    write_register(8, 0x10);                     /* a delay, in force from C0=0 */
    TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG); /* C0=0 displays all the same */
  }
}

/* Ch. 19.2.5, where the delay is a rule read afresh at every character and
   not a countdown held: a border asked for and then taken back before its
   character arrives never comes, and one asked for too late to stop arrives
   twice. R0 and R1 are both 63, as those diagrams have them. */
static void a_skew_written_late_can_cancel_or_double_the_border(void) {
  /* 19.2.5.2: the delay defers the border from C0=63 to C0=0, and R8 put
     back to 0 on C0=0 defines the opening there instead — "it is considered
     immediately and cancels the C0=R1 condition". */
  program_standard();
  write_register(1, 63);
  write_register(8, 0x10);
  TEST_CHECK(run_to_row(1));
  while (crtc.c0 != 62) {
    crtc_tick(&crtc);
  }
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG); /* C0=63, deferred */
  write_register(8, 0x00);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG); /* C0=0, the opening wins */

  /* 19.2.5.4: with no delay the border comes on at C0=63, and a delay
     written there is "considered for the following character, which results
     in an additional BORDER byte". The section draws that write inside the
     character it names while 19.2.5.1 draws its writes at the head of the
     next one; the two cannot both be read literally, so the harness writes
     where every other test here writes — after the tick that named the
     character — and the sentence above is what grades it. */
  program_standard();
  write_register(1, 63);
  TEST_CHECK(run_to_row(1));
  while (crtc.c0 != 62) {
    crtc_tick(&crtc);
  }
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG)); /* C0=63, the border */
  write_register(8, 0x10);
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_DISPTMG)); /* C0=0, one more of it */
  TEST_CHECK(crtc_tick(&crtc) & CRTC_DISPTMG);
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
  /* Begun at C0=1 rather than at the head of the line, so the counter is
     initialized at the head of the next one and R3's eight lines are
     counted from there — the rest of this line runs on top of them (ch.
     16.4.1). */
  run_characters(8 * SCANLINE + (SCANLINE - 1) - 1);
  TEST_CHECK(crtc.vsync);
  run_characters(1);
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

static void a_sync_width_of_zero_makes_no_hsync(void) {
  /* Where types 2, 3 and 4 read 16, this one reads none — the difference a
     program tells them apart by (ch. 14.1, 14.5, 28.1.5). */
  program_standard();
  write_register(3, 0x80);
  int hsyncs = 0, vsyncs = 0;
  bool hsync_before = false, vsync_before = false;
  for (int tick = 0; tick < 2 * FRAME_TICKS; tick++) {
    uint64_t pins = crtc_tick(&crtc);
    if ((pins & CRTC_HSYNC) && !hsync_before) {
      hsyncs++;
    }
    if ((pins & CRTC_VSYNC) && !vsync_before) {
      vsyncs++;
    }
    hsync_before = (pins & CRTC_HSYNC) != 0;
    vsync_before = (pins & CRTC_VSYNC) != 0;
  }
  TEST_EQUAL(hsyncs, 0);
  TEST_EQUAL(vsyncs, 2); /* R3's high nibble is untouched by the rule */
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
  TEST_RUN(a_late_write_cannot_end_the_frame);
  TEST_RUN(an_r5_asked_for_in_time_adds_its_lines);
  TEST_RUN(a_cancelled_r5_leaves_the_last_line_standing);
  TEST_RUN(the_documented_sixteen_line_adjustment);
  TEST_RUN(r9_moved_at_c0_1_makes_the_line_an_adjustment);
  TEST_RUN(r4_moved_at_c0_1_makes_the_line_an_adjustment);
  TEST_RUN(a_late_write_cannot_begin_an_adjustment);
  TEST_RUN(an_r5_cancelled_in_time_adds_no_line);
  TEST_RUN(an_interlace_mode_adds_a_line_to_the_even_frames);
  TEST_RUN(an_interlace_mode_holds_the_even_frames_vsync_back);
  TEST_RUN(an_r6_of_zero_keeps_the_line_on_the_even_frame);
  TEST_RUN(a_disarmed_adjustment_cannot_take_a_second_interlace_line);
  TEST_RUN(the_interlace_line_is_asked_for_at_c0_r0);
  TEST_RUN(an_adjustment_line_still_decides_the_interlace_line);
  TEST_RUN(the_disarm_tests_the_interlace_line_as_well_as_r5);
  TEST_RUN(an_r4_of_127_does_not_trap_the_frame);
  TEST_RUN(the_parity_settles_before_an_r7_of_zero_is_read);
  TEST_RUN(the_video_mode_doubles_the_raster_address);
  TEST_RUN(a_video_mode_entered_late_overflows_c9);
  TEST_RUN(leaving_the_video_mode_drops_the_parity_from_the_limit);
  TEST_RUN(an_odd_r9_gives_the_rows_alternating_parities);
  TEST_RUN(an_odd_row_of_an_odd_frame_takes_its_vsync_late);
  TEST_RUN(the_video_pointer_still_steps_a_row_at_a_time);
  TEST_RUN(the_video_mode_still_takes_the_adjustment_lines);
  TEST_RUN(a_line_r1_never_ends_borders_its_last_byte);
  TEST_RUN(an_r6_of_zero_alternates_the_first_lines_bytes);
  TEST_RUN(the_skew_delays_the_border_at_both_ends);
  TEST_RUN(a_skew_carries_the_border_round_the_lines_end);
  TEST_RUN(a_skew_makes_the_early_border_a_whole_character);
  TEST_RUN(the_border_on_function_shuts_the_display);
  TEST_RUN(a_skew_taken_off_late_does_not_cost_the_line);
  TEST_RUN(the_border_on_function_moves_no_comparison);
  TEST_RUN(the_border_on_function_silences_the_interlace_bits);
  TEST_RUN(a_skew_does_not_move_the_video_pointer);
  TEST_RUN(every_way_of_missing_r1_borders_alike);
  TEST_RUN(an_r1_moved_behind_c0_earns_the_missed_border);
  TEST_RUN(a_skew_cancelled_in_time_leaves_the_border_where_it_was);
  TEST_RUN(the_border_on_function_leaves_the_r6_border_standing);
  TEST_RUN(the_border_on_function_earns_no_deferred_character);
  TEST_RUN(a_skew_written_late_can_cancel_or_double_the_border);
  TEST_RUN(the_sixty_hertz_table_makes_a_262_line_frame);
  TEST_RUN(one_vsync_per_equality_of_c4_and_r7);
  TEST_RUN(the_r1_border_holds_until_the_line_begins_again);
  TEST_RUN(the_r6_border_is_shut_for_the_whole_frame);
  TEST_RUN(an_r1_of_zero_leaves_the_line_displayed);
  TEST_RUN(a_c0_that_overflowed_does_not_open_the_display);
  TEST_RUN(a_sync_width_of_zero_makes_no_hsync);
  return TEST_REPORT("crtc");
}
