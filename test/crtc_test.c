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

/* The adjustment is one state held in two flags, and a line taken back
   drops both: a chip left thinking it had already spent its interlace line
   would withhold a later frame's without a word (ch. 13.2.1, 19.6.1). */
static void a_disarmed_adjustment_forgets_the_interlace_line(void) {
  program_standard();
  write_register(4, 0);
  write_register(5, 0);
  write_register(6, 0);
  write_register(9, 3);
  write_register(8, 1);
  crtc_tick(&crtc);

  run_scanlines(4);
  TEST_CHECK(crtc.in_vertical_adjustment);
  TEST_CHECK(crtc.interlace_line_given);
  TEST_EQUAL(crtc.c4, 1); /* R4+1, incremented once for the whole of it */

  /* R4 and R9 moved onto the counters, and the interlace asked for taken
     back, all in time for the character C0 names 3. */
  write_register(4, 1);
  write_register(9, 0);
  write_register(8, 0);
  run_characters(4);
  TEST_CHECK(!crtc.in_vertical_adjustment);
  TEST_CHECK(!crtc.interlace_line_given);
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
  TEST_RUN(a_disarmed_adjustment_forgets_the_interlace_line);
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
  TEST_RUN(the_sixty_hertz_table_makes_a_262_line_frame);
  TEST_RUN(one_vsync_per_equality_of_c4_and_r7);
  TEST_RUN(the_r1_border_holds_until_the_line_begins_again);
  TEST_RUN(the_r6_border_is_shut_for_the_whole_frame);
  TEST_RUN(an_r1_of_zero_leaves_the_line_displayed);
  TEST_RUN(a_c0_that_overflowed_does_not_open_the_display);
  TEST_RUN(a_sync_width_of_zero_makes_no_hsync);
  return TEST_REPORT("crtc");
}
