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
  crtc_init(&crtc, 0);
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
  crtc_init(&crtc, 0);
  TEST_EQUAL(crtc.c0, 0);
  TEST_EQUAL(crtc.c9, 0);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.address_register, 0);
  TEST_CHECK(!crtc.hsync);
  TEST_CHECK(!crtc.vsync);
}

static void select_wears_five_bits(void) {
  crtc_init(&crtc, 0);
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 0xEC));
  TEST_EQUAL(crtc.address_register, 0x0C);
}

static void writes_wear_the_documented_widths(void) {
  crtc_init(&crtc, 0);
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

/* Ch. 21.2.2: types 1 and 2 read the cursor and the light pen and nothing
   besides — "an attempt to read another register (0 to 255) returns the
   value 0" — where type 0 reads the display start as well (ch. 21.2.1).
   Reading R12 is therefore what parts a type 0 from the rest, and register
   31 what parts a type 1 from a type 2: on a type 1 it "returns a non-zero
   value (I got 127 or 255)", a register UMC defined and this model never
   used. Both are how a program names the chip in front of it (ch. 28.1.9). */
static void each_type_answers_the_read_port_its_own_way(void) {
  static const struct {
    uint8_t type;
    uint8_t at_r12; /* the display start, which only a type 0 gives back */
    uint8_t at_r31; /* and the register that parts a type 1 from a type 2 */
  } cases[] = {{0, 0x30, 0x00}, {1, 0x00, 0xFF}, {2, 0x00, 0x00}};
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    static const uint8_t values[14] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0x18};
    crtc_init(&crtc, cases[index].type);
    for (int reg = 0; reg < 14; reg++) {
      write_register(reg, values[reg]);
    }
    write_register(14, 0x2A);
    write_register(15, 0x55);
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 12));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), cases[index].at_r12);
    /* Each half of a pointer is its own register, so a type that answers
       one answers the other. */
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 13));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)),
               cases[index].at_r12 == 0 ? 0x00 : 0x18);
    /* The cursor registers read back on every type; no board here wires the
       pin that would fill the light pen's pair, so those stay empty. */
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 14));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0x2A);
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 15));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0x55);
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 16));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0);
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 17));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0);
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 4));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0);
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 31));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), cases[index].at_r31);
    /* Only the low five bits of a number are read, so 108 names R12 and 124
       names R31 (ch. 21.2.1). */
    crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 108));
    TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), cases[index].at_r12);
  }
}

/* Ch. 21.3.2: types 0 and 2 "do not have a status register", so &BE00 is a
   bus nobody drives and the machine reads back whatever it put there. Type
   1 has one, and its unused bits — 0 to 4 and 7 — read 0 (ch. 21.3.3). */
static void only_type_1_drives_the_status_port(void) {
  for (uint8_t type = 0; type < 3; type++) {
    crtc_init(&crtc, type);
    uint64_t floating = crtc_set_data(0, 0x77) | CRTC_CS | CRTC_RW;
    TEST_EQUAL(crtc_data(crtc_access(&crtc, floating)), type == 1 ? 0x00 : 0x77);
  }
}

/* And its bit 5, which ch. 21.3.3 gives twice over, in prose and in two
   diagrams: the BORDER R6 condition on the two heads of line it names,
   "False: C4=C9=C0=0 / True: C4=R6 & C9=C0=0". Its diagrams read the port
   four times across each turn and the byte changes on the fourth, where C0
   comes round to 0 — so the bit speaks for the line being drawn, and a
   program that reads it during the line before is told the old answer.
   With rows of eight scanlines and R6 at 25, it rises at the head of the
   row R6 names and falls at the head of the frame. */
static void the_status_border_bit_turns_over_at_a_line_head(void) {
  static const uint8_t values[14] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0};
  crtc_init(&crtc, 1);
  for (int reg = 0; reg < 14; reg++) {
    write_register(reg, values[reg]);
  }
  int rose[3] = {-1, -1, -1}; /* c4, c9 and c0 where the bit went up */
  int fell[3] = {-1, -1, -1};
  bool standing = false;
  /* A frame to settle on, then the frame that is read. */
  for (long character = 0; character < 2L * 64 * 312; character++) {
    crtc_tick(&crtc);
    bool bit = (crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RW)) & 0x20) != 0;
    if (bit != standing && character >= 64L * 312) {
      int *where = bit ? rose : fell;
      if (where[0] < 0) {
        where[0] = crtc.c4;
        where[1] = crtc.c9;
        where[2] = crtc.c0;
      }
    }
    standing = bit;
  }
  TEST_EQUAL(rose[0], 25); /* the row R6 names */
  TEST_EQUAL(rose[1], 0);  /* on its first scanline */
  TEST_EQUAL(rose[2], 0);  /* and its first character */
  TEST_EQUAL(fell[0], 0);  /* and the frame's own head */
  TEST_EQUAL(fell[1], 0);
  TEST_EQUAL(fell[2], 0);
}

/* Ch. 16: the VSYNC's length "can be programmed on CRTC's 0, 3 and 4 (via
   register R3h). It is fixed at 16 for CRTC's 1 and 2", so the same R3h
   that shortens a type 0's pulse is not read at all by those two. And a
   pulse a program triggers, by writing R7 to the row C4 already stands on,
   is counted by those two "as if the VSYNC had started when C0=0"
   (ch. 16.4.2, 16.4.3), so it spends a line fewer than a type 0's, which
   keeps the rest of the line it began in. A pulse C4 simply walks into
   keeps all sixteen on every type (ch. 28.1.4), which is why the two
   columns below are set up differently: the first leaves R7 where C4 will
   meet it, the second writes R7 mid-line to trigger one.

   Counted as the line heads the pulse stands through, which is the measure
   the chip's own counter takes, C3h advancing at each C0=0. Nothing outside
   this repository grades the triggered rule: no group on the disc moves
   when it is taken away. */
static void each_type_keeps_its_own_vsync_length(void) {
  static const struct {
    uint8_t type;
    uint8_t r3;             /* R3h the length asked for, R3l a HSYNC of 14 */
    int walked_into;        /* line heads a pulse C4 meets stands through */
    int triggered_mid_line; /* and one an R7 written mid-line begins */
  } cases[] = {
      {0, 0x4E, 4, 4},   /* four lines asked for and four given */
      {1, 0x4E, 16, 15}, /* R3h unread, and a line fewer for being triggered */
      {2, 0x4E, 16, 15}, /* which ch. 16.4.3 gives this type in the same words */
      {0, 0x0E, 16, 16}, /* the R3h of 0 that makes a type 0 count 16 too */
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    for (int triggered = 0; triggered < 2; triggered++) {
      crtc_init(&crtc, cases[index].type);
      write_register(0, 63);
      write_register(1, 40);
      write_register(2, 46);
      write_register(3, cases[index].r3);
      write_register(4, 38);
      write_register(9, 7);
      write_register(6, 25);
      /* Left where C4 walks into it, or out of C4's reach until a write
         puts it under the row the chip already stands on. */
      write_register(7, triggered ? 127 : 5);
      int lines = 0;
      bool standing = false;
      for (long character = 0; character < 4L * 64 * 312; character++) {
        bool vsync = (crtc_tick(&crtc) & CRTC_VSYNC) != 0;
        if (vsync && crtc.c0 == 0) {
          lines++;
        }
        if (standing && !vsync) {
          break;
        }
        standing = vsync;
        if (triggered && !standing && crtc.c9 == 3 && crtc.c0 == 20) {
          write_register(7, crtc.c4);
        }
      }
      TEST_EQUAL(lines, triggered ? cases[index].triggered_mid_line : cases[index].walked_into);
    }
  }

  /* And the pulse an even interlaced frame raises on the half line of its
     own, which R7 was set for in advance rather than written to trigger: it
     "starts when R7 was programmed before C4=R7" and so "lasts 16 lines"
     (ch. 28.1.4), where one a write triggers on the same type gives a line
     up. */
  crtc_init(&crtc, 1);
  write_register(0, 63);
  write_register(1, 40);
  write_register(2, 46);
  write_register(3, 0x4E);
  write_register(4, 38);
  write_register(9, 7);
  write_register(6, 25);
  write_register(7, 5);
  write_register(8, 1); /* interlace, which raises an even frame's VSYNC late */
  int lines = 0;
  bool standing = false;
  bool began_mid_line = false;
  for (long character = 0; character < 6L * 64 * 312; character++) {
    bool vsync = (crtc_tick(&crtc) & CRTC_VSYNC) != 0;
    if (vsync && !standing) {
      began_mid_line = crtc.c0 != 0;
      lines = 0;
    }
    if (vsync && crtc.c0 == 0) {
      lines++;
    }
    if (standing && !vsync && began_mid_line) {
      break;
    }
    standing = vsync;
  }
  TEST_CHECK(began_mid_line);
  TEST_EQUAL(lines, 16);
}

/* Every type waits the half line an even frame's VSYNC is held to, and
   three of the five take a whole line besides: "there is also an exception
   on CRTC's 0, 3 and 4 when the line count of a C4 character is odd on an
   odd frame and an odd C4" (ch. 19.7.1). Of a type 1 ch. 19.5.3 says the
   opposite outright — "the VSYNC is not delayed from a line on odd C4s when
   R9 is even" — so where an odd frame finds C4 odd, a type 0 raises its
   VSYNC on the row's second scanline and a type 1 and a type 2 on its
   first. No line on the disc moves when the type test is taken away. */
static void types_1_and_2_delay_no_vsync_by_a_whole_line(void) {
  static const struct {
    uint8_t type;
    uint8_t scanline_on_an_odd_frame;
  } cases[] = {{0, 1}, {1, 0}, {2, 0}, {3, 1}, {4, 1}};
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, cases[index].type);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 38);
    write_register(9, 7); /* odd, which is what the whole-line delay asks for */
    write_register(6, 25);
    write_register(7, 5); /* odd, so C4 is odd where it meets R7 */
    write_register(8, 3); /* the interlace video mode */
    int half_line = -1;
    int odd_frame_scanline = -1;
    bool standing = false;
    for (long character = 0; character < 8L * 64 * 320; character++) {
      bool vsync = (crtc_tick(&crtc) & CRTC_VSYNC) != 0;
      if (vsync && !standing) {
        if (!crtc.parity_frame) {
          half_line = crtc.c0;
        } else if (odd_frame_scanline < 0) {
          odd_frame_scanline = crtc.c9;
        }
      }
      standing = vsync;
      if (half_line >= 0 && odd_frame_scanline >= 0) {
        break;
      }
    }
    /* The half line is every type's, and it is half of R0 (ch. 16.5). */
    TEST_EQUAL(half_line, 31);
    TEST_EQUAL(odd_frame_scanline, cases[index].scanline_on_an_odd_frame);
  }
}

/* Which R9 gives a row of a given height is not the same on a type 0 and a
   type 1. Ch. 28.1.7 sets them side by side: "on CRTC's 0 and 1, with R9
   programmed respectively with 6 and 7 without having modified R7, the
   VSYNC occurs 2 times faster, since C4=R7 with characters of 4 lines
   instead of 8", and ch. 19.4.1 and 19.4.2 say it from the programmer's
   side — a character of N lines wants "value N-2" from a type 0 and "the
   value N-1" from a type 1. The reason is in ch. 19.5.3: where a type 0
   makes its odd-lined rows out of an odd R9, a type 1 makes them out of an
   even one, and it reads the limit down to that parity where a type 0 reads
   it up (ch. 19.8.2). The pairs below are ch. 19.5.2's table and ch.
   19.5.3's diagram, both of which draw the two fields, so both fields are
   read here: a pair of rows holds the same total on either frame, and it is
   which of them is the longer that turns over. */
static void types_0_and_1_want_different_r9s_for_a_row(void) {
  static const struct {
    uint8_t type;
    uint8_t r9;
    int even_field[2]; /* scanlines in the frame's first two rows */
    int odd_field[2];  /* and in the first two of the frame after it */
  } cases[] = {
      {0, 6, {4, 4}, {4, 4}}, /* ch. 28.1.7's pair: one height, two R9s */
      {1, 7, {4, 4}, {4, 4}},
      {0, 7, {5, 4}, {4, 5}}, /* and the R9 each of them makes a pair of */
      {1, 8, {5, 4}, {4, 5}},
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, cases[index].type);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 38);
    write_register(9, cases[index].r9);
    write_register(6, 25);
    write_register(7, 35);
    write_register(8, 3); /* the interlace video mode */
    int rows[2][2] = {{0, 0}, {0, 0}};
    int counted[2] = {0, 0};
    int scanlines = 0;
    int row = -1;
    bool parity = false;
    for (long character = 0; character < 8L * 64 * 320; character++) {
      crtc_tick(&crtc);
      if (crtc.c0 != 0) {
        continue;
      }
      if (crtc.c4 != row) {
        int field = parity ? 1 : 0;
        if (row >= 0 && scanlines > 0 && counted[field] < 2) {
          rows[field][counted[field]++] = scanlines;
        }
        scanlines = 0;
        row = crtc.c4;
        parity = crtc.parity_frame;
      }
      scanlines++;
    }
    TEST_EQUAL(rows[0][0], cases[index].even_field[0]);
    TEST_EQUAL(rows[0][1], cases[index].even_field[1]);
    TEST_EQUAL(rows[1][0], cases[index].odd_field[0]);
    TEST_EQUAL(rows[1][1], cases[index].odd_field[1]);
  }
}

/* Types 0 and 2 settle the coming frame's parity on the row R6 names, so
   where C4 can never reach R6 "this state is no longer updated and
   ParityFrame remains frozen" (ch. 19.5.2, 19.5.4) — the frames stop
   alternating and the two fields stop differing. The other three anticipate
   nothing: "ParityFrame switch between each frame when C4 = C9 = C0 = 0"
   and does so "whatever the value of R8" (ch. 19.5.3, 19.5.5), so no R6 can
   freeze them. The extra line follows whichever parity its type keeps — the
   anticipated one on a type 0 and a type 2 (ch. 19.6.1, 19.6.3), the
   frame's own on the other three, where it "does not depend on the C4=R6
   equivalence" (ch. 19.6.2, 19.6.4) — so a frame carrying it is a line
   longer than the frame beside it, and the lengths turn over wherever the
   parity does. */
/* Ch. 19.5.3's diagrams for a pulse of the interlace video mode on a type
   1, read off the page as drawn: sixteen of them, one for each way the
   frame's parity, C4's, R9's and C9's can stand when the mode is asked for
   and given up again. Each names ParityC9 and ParityFrame after the OUT
   R8,3 and after the OUT R8,0 that follows it, and those are what this
   holds the chip to. Where the chapter's prose gives the rules as three
   lines of arithmetic, the diagrams give their answers case by case, and a
   slip in reading either shows up as a disagreement with the other. One
   rule of the three is not falsifiable here: in all sixteen the frame takes
   a parity on leaving the mode that it already held, so what the leaving
   does is invisible to them. */
static void an_r8_pulse_leaves_the_parities_the_diagrams_draw(void) {
  static const struct {
    bool frame_parity_odd; /* the parity the pulse finds */
    bool c4_odd;
    bool r9_odd;
    bool c9_odd;
    bool parity_c9_after_on;
    bool frame_parity_after_on;
    bool parity_c9_after_off;
    bool frame_parity_after_off;
  } cases[] = {
      /* Initial parity EVEN (page 211) */
      {false, false, false, false, false, false, false, false},
      {false, false, true, false, false, false, false, false},
      {false, false, false, true, false, false, false, false},
      {false, false, true, true, false, false, false, false},
      {false, true, false, false, true, false, false, false},
      {false, true, true, false, false, false, false, false},
      {false, true, false, true, true, false, false, false},
      {false, true, true, true, false, false, false, false},
      /* Initial parity ODD (page 212) */
      {true, false, false, false, false, false, false, false},
      {true, false, true, false, false, false, false, false},
      {true, false, false, true, true, true, true, true},
      {true, false, true, true, true, true, true, true},
      {true, true, false, false, true, false, false, false},
      {true, true, true, false, false, false, false, false},
      {true, true, false, true, false, true, true, true},
      {true, true, true, true, true, true, true, true},
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, 1);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 38);
    write_register(6, 25);
    write_register(7, 35);
    write_register(9, cases[index].r9_odd ? 7 : 6);
    /* The chip is walked to the standing the diagram names rather than set
       to it: the counters and the frame's parity are the chip's own, and a
       state written into them by hand is one it need never have reached. */
    bool found = false;
    for (long character = 0; character < 8L * 64 * 400 && !found; character++) {
      crtc_tick(&crtc);
      found = crtc.c0 == 20 && ((crtc.c4 & 1) != 0) == cases[index].c4_odd &&
              ((crtc.c9 & 1) != 0) == cases[index].c9_odd &&
              crtc.parity_frame == cases[index].frame_parity_odd;
    }
    TEST_CHECK(found);
    if (!found) {
      continue;
    }
    write_register(8, 3); /* OUT R8,3: the mode is asked for */
    TEST_EQUAL(crtc.parity_c9_held, cases[index].parity_c9_after_on);
    TEST_EQUAL(crtc.parity_frame, cases[index].frame_parity_after_on);
    write_register(8, 0); /* and given up again, on the same line */
    TEST_EQUAL(crtc.parity_c9_held, cases[index].parity_c9_after_off);
    TEST_EQUAL(crtc.parity_frame, cases[index].frame_parity_after_off);
  }
}

static void types_0_and_2_alone_can_freeze_their_frame_parity(void) {
  static const struct {
    uint8_t type;
    uint8_t r6;      /* the row a type 0 takes its anticipation on */
    bool alternates; /* whether the parity, and the extra line with it, turns */
  } cases[] = {
      {0, 5, true},   {1, 5, true},  {2, 5, true},   {3, 5, true},  {4, 5, true},
      {0, 25, false}, {1, 25, true}, {2, 25, false}, {3, 25, true}, {4, 25, true},
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, cases[index].type);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 6);
    write_register(9, 7);
    write_register(6, cases[index].r6);
    write_register(7, 3);
    write_register(8, 3);
    bool parity[4] = {false, false, false, false};
    int length[4] = {0, 0, 0, 0};
    int frames = 0;
    int lines = 0;
    bool head = false;
    for (long character = 0; character < 16L * 64 * 80 && frames < 4; character++) {
      crtc_tick(&crtc);
      if (crtc.c0 == 0) {
        lines++;
      }
      bool on_the_head = crtc.c0 == 0 && crtc.c4 == 0 && crtc.c9 == 0;
      if (on_the_head && !head) {
        if (frames > 0) {
          length[frames - 1] = lines - 1;
        }
        parity[frames++] = crtc.parity_frame;
        lines = 0;
      }
      head = on_the_head;
    }
    /* Four frames seen, so a chip that stopped drawing cannot pass by
       never contradicting anything. */
    TEST_EQUAL(frames, 4);
    for (int frame = 1; frame < 4; frame++) {
      TEST_EQUAL(parity[frame] != parity[frame - 1], cases[index].alternates);
    }
    /* And the extra line turns over with it, or with nothing. */
    TEST_EQUAL(length[1] != length[0], cases[index].alternates);
    TEST_EQUAL(length[2] != length[1], cases[index].alternates);
  }
}

/* A type 1 is the one type a program can tell which field to start on, and
   an interlace pulse is how: "if IVM mode is toggled on and off on an even
   C9 line, regardless of the value of R9, the parity is set to EVEN. It is
   thus possible to fix the parity quite easily on this CRTC" (ch. 19.5.3).
   The rules behind it are that chapter's own, taken on the third and fourth
   microseconds of the write. No other type has them, and on those the pulse
   leaves the parity where it found it. */
static void an_interlace_pulse_fixes_a_type_1_on_an_even_field(void) {
  for (uint8_t type = 0; type < 5; type++) {
    for (uint8_t r9 = 6; r9 <= 7; r9++) {
      for (int from_odd = 0; from_odd < 2; from_odd++) {
        crtc_init(&crtc, type);
        write_register(0, 63);
        write_register(1, 40);
        write_register(2, 46);
        write_register(3, 0x8E);
        write_register(4, 38);
        write_register(9, r9);
        write_register(6, 25);
        write_register(7, 35);
        /* Run to a frame whose parity is the one being started from, then
           stand on an even scanline of a row and pulse the mode. */
        bool standing = false;
        for (long character = 0; character < 8L * 64 * 320; character++) {
          crtc_tick(&crtc);
          bool head = crtc.c0 == 0 && crtc.c4 == 0 && crtc.c9 == 0;
          if (head && !standing && crtc.parity_frame == (from_odd != 0)) {
            break;
          }
          standing = head;
        }
        while (!(crtc.c9 % 2 == 0 && crtc.c0 == 10)) {
          crtc_tick(&crtc);
        }
        bool before = crtc.parity_frame;
        write_register(8, 3);
        crtc_tick(&crtc);
        write_register(8, 0);
        crtc_tick(&crtc);
        if (type == 1) {
          TEST_CHECK(!crtc.parity_frame); /* even, whichever it began on */
        } else {
          TEST_EQUAL(crtc.parity_frame, before);
        }
      }
    }
  }

  /* Turning it on and leaving it on is the half the pulse hides, and it is
     where the chapter's exception lives: the frame's parity survives only
     "for cases where ParityFrame and ParityC9 were odd before the request
     for IVM", and with an odd R9 it is the scanline alone that decides
     ParityC9. So entering on an even scanline settles the parity even
     whichever it was, and entering on an odd one only spares a frame that
     was odd already. */
  for (int from_odd = 0; from_odd < 2; from_odd++) {
    for (int on_an_odd_scanline = 0; on_an_odd_scanline < 2; on_an_odd_scanline++) {
      crtc_init(&crtc, 1);
      write_register(0, 63);
      write_register(1, 40);
      write_register(2, 46);
      write_register(3, 0x8E);
      write_register(4, 38);
      write_register(9, 7); /* odd, so C4 has no say in ParityC9 */
      write_register(6, 25);
      write_register(7, 35);
      bool standing = false;
      for (long character = 0; character < 8L * 64 * 320; character++) {
        crtc_tick(&crtc);
        bool head = crtc.c0 == 0 && crtc.c4 == 0 && crtc.c9 == 0;
        if (head && !standing && crtc.parity_frame == (from_odd != 0)) {
          break;
        }
        standing = head;
      }
      while (!(crtc.c9 % 2 == (on_an_odd_scanline ? 1 : 0) && crtc.c0 == 10)) {
        crtc_tick(&crtc);
      }
      write_register(8, 3);
      crtc_tick(&crtc);
      TEST_EQUAL(crtc.parity_frame, from_odd && on_an_odd_scanline);
    }
  }
}

/* Ch. 19.5.3 does not only state the rules an R8 write follows on a type 1,
   it draws them: printed pages 211 and 212 work fifteen scenarios, one for
   each way the frame's parity, C9, C4 and R9 can stand when the mode is
   pulsed on and off, and each is labelled with the Shaker test that
   exercises it. The table below is those pages, and the values are theirs.
   It is the only outside evidence for three of the rules — the correction
   C4 makes where R9 is even, the reset of ParityC9 where the frame was
   even, and the parity the frame takes back when the mode is left — none of
   which any line the scoreboard scores can reach. */
static void an_r8_write_answers_the_scenarios_the_chapter_draws(void) {
  static const struct {
    const char *drawn_as; /* the Shaker tests the page names it by */
    bool frame_odd;       /* how the chip stands when the pulse comes */
    bool c4_odd;
    bool r9_odd;
    bool c9_odd;
    bool held_on_entering; /* and the four values the page annotates */
    bool frame_on_entering;
    bool held_on_leaving;
    bool frame_on_leaving;
  } scenarios[] = {
      {"S/W", false, false, false, false, false, false, false, false},
      {"T/X", false, false, true, false, false, false, false, false},
      {"Q/U/Y1", false, false, false, true, false, false, false, false},
      {"R/V/Z1", false, false, true, true, false, false, false, false},
      {"D/H", false, true, false, false, true, false, false, false},
      {"E/I", false, true, true, false, false, false, false, false},
      {"B/F", false, true, false, true, true, false, false, false},
      {"C/G", false, true, true, true, false, false, false, false},
      {"ZA/ZC", true, false, false, false, false, false, false, false},
      {"ZB/ZD", true, false, true, false, false, false, false, false},
      {"P/Y2", true, false, false, true, true, true, true, true},
      {"Z", true, false, true, true, true, true, true, true},
      {"L/N", true, true, false, false, true, false, false, false},
      {"A/J2", true, true, false, true, false, true, true, true},
      {"K2", true, true, true, true, true, true, true, true},
  };
  for (unsigned index = 0; index < sizeof scenarios / sizeof *scenarios; index++) {
    crtc_init(&crtc, 1);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 38);
    write_register(9, scenarios[index].r9_odd ? 7 : 6);
    write_register(6, 25);
    write_register(7, 35);
    write_register(8, 0);
    /* Stand where the page stands: a row and a scanline of the parities it
       names, away from either end of the line. */
    bool stood = false;
    for (long character = 0; character < 8L * 64 * 320; character++) {
      crtc_tick(&crtc);
      if (((crtc.c4 & 1) != 0) == scenarios[index].c4_odd &&
          ((crtc.c9 & 1) != 0) == scenarios[index].c9_odd && crtc.c0 == 20) {
        stood = true;
        break;
      }
    }
    TEST_CHECK(stood);
    crtc.parity_frame = scenarios[index].frame_odd;
    write_register(8, 3);
    TEST_EQUAL(crtc.parity_c9_held, scenarios[index].held_on_entering);
    TEST_EQUAL(crtc.parity_frame, scenarios[index].frame_on_entering);
    crtc_tick(&crtc);
    write_register(8, 0);
    TEST_EQUAL(crtc.parity_c9_held, scenarios[index].held_on_leaving);
    TEST_EQUAL(crtc.parity_frame, scenarios[index].frame_on_leaving);
  }
}

/* Ch. 11.2.2 and 11.2.3 each draw the frame's additional lines, one table
   for each way a chip counts them, and the two are the test below. A type 0
   holds the row still and spends C9 on them, so C4 stays where the last row
   left it and C9 climbs; the second table is headed for a type 1 and a type
   2, which keep them on a counter of their own — "on these circuits, the C5
   and C9 counters are dissociated" — so the row goes on being counted and
   C4 on advancing while C5 counts the lines beside them. The chapter
   initializes R4=10, R5=16, R9=3, R1=40 and R0=63 (ch. 11.2.1), so the
   lines run from C4=11 with R9 written to 10 on the fifth of them, which
   its tables annotate; the rest are a standard frame's. */
static void each_type_counts_the_adjustment_lines_its_own_way(void) {
  static const struct {
    uint8_t type;
    const char *drawn; /* the row and scanline of each line, as the page has it */
  } cases[] = {
      {0, "11/0 11/1 11/2 11/3 11/4 11/5 11/6 11/7 11/8 11/9 11/10 11/11 11/12 11/13 11/14 11/15"},
      {1, "11/0 11/1 11/2 11/3 12/0 12/1 12/2 12/3 12/4 12/5 12/6 12/7 12/8 12/9 12/10 13/0"},
      {2, "11/0 11/1 11/2 11/3 12/0 12/1 12/2 12/3 12/4 12/5 12/6 12/7 12/8 12/9 12/10 13/0"},
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, cases[index].type);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 10);
    write_register(9, 3);
    write_register(5, 16);
    write_register(6, 25);
    write_register(7, 60);
    char drawn[128] = "";
    int length = 0;
    int counted = 0;
    bool standing = false;
    bool written = false;
    for (long character = 0; character < 40L * 64 * 400 && counted < 16; character++) {
      crtc_tick(&crtc);
      if (crtc.c0 != 0) {
        continue;
      }
      bool running = crtc.vertical_adjustment_in_progress;
      if (running && !standing) {
        length = 0;
        counted = 0;
      }
      standing = running;
      if (!running) {
        continue;
      }
      length += snprintf(drawn + length, sizeof drawn - (size_t)length,
                         counted ? " %d/%d" : "%d/%d", crtc.c4, crtc.c9);
      counted++;
      /* The page writes R9 on the line after the one it marks, so the row
         that moved did so on the value it was counting against. */
      if (counted == 5 && !written) {
        write_register(9, 10);
        written = true;
      }
    }
    TEST_EQUAL(counted, 16);
    if (strcmp(drawn, cases[index].drawn) != 0) {
      TEST_FAIL("type %u drew %s, where the table says %s", cases[index].type, drawn,
                cases[index].drawn);
    }
  }

  /* And however they are counted, R5 says how many there are: the run ends
     "when the number of the next additional line reaches R5" on either
     counter (ch. 11.3.1, 11.3.2). */
  for (uint8_t type = 0; type < 3; type++) {
    for (uint8_t r5 = 1; r5 <= 6; r5++) {
      crtc_init(&crtc, type);
      write_register(0, 63);
      write_register(1, 40);
      write_register(2, 46);
      write_register(3, 0x8E);
      write_register(4, 10);
      write_register(9, 3);
      write_register(5, r5);
      write_register(6, 25);
      write_register(7, 60);
      int lines = 0;
      bool standing = false;
      bool seen = false;
      for (long character = 0; character < 40L * 64 * 400; character++) {
        crtc_tick(&crtc);
        if (crtc.c0 != 0) {
          continue;
        }
        bool running = crtc.vertical_adjustment_in_progress;
        if (running && !standing) {
          lines = 0;
        }
        if (running) {
          lines++;
        }
        if (standing && !running && seen) {
          break;
        }
        seen = seen || running;
        standing = running;
      }
      TEST_EQUAL(lines, r5);
    }
  }
}

/* A type 1 latches a state when it opens a run of additional lines, "if
   R5>0 when C4 should return to 0 at the end of the frame", and taking R5
   back to 0 does not clear it: "the state is not deactivated, C4 does not
   return to 0 and C5 loops". Only a later R5 closes it — "if C5+1 reaches
   an R5>0, then the additional management changes C4 to 0 before
   deactivating its state" (ch. 11.3.2). The chapter offers that to a
   program as a way of holding a frame open — "it is possible to change C4
   and C9 to 0 on any line with this method" — so the test is the offer
   taken up: open a run, cancel R5, and name the line to end on — named on
   the tenth line the run spends with R5 at 0, which is its eleventh and its
   last, so the run is eleven lines long.

   A program that names none does not hold the frame for ever, because "C4,
   however, continues to be compared to R4 to process the change from C4 to
   0". The run opens with C4 one past R4, and C4 comes back round to R4 after
   the 128 rows a seven-bit counter has, each of the R9+1 = 4 lines the setup
   below gives a row. The management is not deactivated there — "the
   additional management, however, remains activated" — so C5, which never
   stopped counting, spends the rest of its own round of thirty-two before it
   reaches the 0 R5 was cancelled to: 128 x 4 + 32 below.

   Types 0 and 2 latch no such state. An R5 cancelled under those is only
   the overflow ch. 11.3.1 gives its two — "if R5 is modified with a value
   less than C5+1/C9+1, then the counter overflows and continues to count up
   to 0 to reach the new R5 value" — which is the thirty-two a five-bit
   counter has before it comes round to the 0 it was cancelled to. That
   chapter is headed "CRTC's 0, 2" and the deadlock's is headed "CRTC 1",
   which is the whole of why the type column below names three types. */
static void a_type_1_holds_a_frame_open_where_r5_is_cancelled(void) {
  /* NEVER_ASKED stands in the column a line would be named in, for the
     program that names none. WATCHED is twice the longest run asked for
     below, and bounds a run that will not end, so that a chip holding the
     frame for ever fails here rather than spinning until the character
     count runs out. */
  enum { NEVER_ASKED = -1, WATCHED = 2 * (128 * 4 + 32) };
  static const struct {
    uint8_t type;
    int end_it_after; /* lines to run with R5 at 0 before naming the end */
    int lines;        /* and how long the run comes out */
  } cases[] = {
      {1, 10, 11},                    /* ended where the program asks, and not before */
      {1, 20, 21},                    /* and again ten lines later, to show it is the asking */
      {1, NEVER_ASKED, 128 * 4 + 32}, /* or C4's round, and then C5's own */
      {2, NEVER_ASKED, 32},           /* while these two merely count five bits */
      {0, NEVER_ASKED, 32},           /* the one of them on C5, the other on C9 */
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, cases[index].type);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 10);
    write_register(9, 3);
    write_register(5, 2);
    write_register(6, 25);
    write_register(7, 60);
    int lines = 0;
    int held = 0;
    bool standing = false;
    bool seen = false;
    bool cancelled = false;
    bool ended = false;
    for (long character = 0; character < 80L * 64 * 400; character++) {
      crtc_tick(&crtc);
      if (crtc.c0 != 0) {
        continue;
      }
      bool running = crtc.vertical_adjustment_in_progress;
      if (running && !standing) {
        lines = 0;
        held = 0;
      }
      if (running) {
        lines++;
        if (!cancelled) {
          write_register(5, 0);
          cancelled = true;
        } else if (++held == cases[index].end_it_after) {
          write_register(5, (uint8_t)((crtc.c5 + 1) & 0x1F));
        }
        if (lines > WATCHED) {
          break;
        }
      }
      if (standing && !running && seen) {
        ended = true;
        break;
      }
      seen = seen || running;
      standing = running;
    }
    TEST_CHECK(ended);
    TEST_EQUAL(lines, cases[index].lines);
  }
}

/* The state is taken where a run opens, and a frame is never held by the R5
   of the frame before it. The path that tells a taken state from a carried
   one is a last line unmade by an R9 write at C0=1, where "the current line
   becomes the 'first' adjustment line" (ch. 10.3.1.2): the run that opens
   there opens with "C4 should return to 0 at the end of the frame (C4=R4,
   C9=R9)" false, which is the one comparison ch. 11.3.2 puts its state
   behind. R5 stands above 0 over that opening and is cancelled inside the
   run, so a chip that took no state counts the plain five-bit overflow,
   where one that took the state here — or carried it from the two frames
   before, which did open with the comparison true — would hold the frame
   past that count, to the row C4 comes round to R4 on — R9 is moved and R4
   is not, so C4 must climb all 128 rows to get there, and the difference is
   a whole frame rather than a line of it. */
static void a_type_1_opens_each_run_with_the_r5_it_has(void) {
  enum { GIVE_UP_AFTER = 4096 };
  crtc_init(&crtc, 1);
  write_register(0, 63);
  write_register(1, 40);
  write_register(2, 46);
  write_register(3, 0x8E);
  write_register(4, 10);
  write_register(9, 3);
  write_register(5, 5);
  write_register(6, 25);
  write_register(7, 60);
  int spent_runs = 0;
  int lines = 0;
  bool standing = false;
  bool unmade = false;
  bool opened_mid_line = false;
  bool cancelled = false;
  bool ended = false;
  for (long character = 0; character < 400L * 64 * 400 && !ended; character++) {
    /* The write that unmakes the last line is made during the character C0
       names 1, the only one that can leave the state true and the
       comparison false: one made at C0=0 would have been seen by the line's
       own second look, and one made later leaves the state standing
       (ch. 12.2, 10.3.1.2, both headed for a type 0 and read here whatever
       the type). */
    if (spent_runs == 2 && !unmade && crtc.c0 == 1 && crtc.c4 == 10 && crtc.c9 == 3) {
      write_register(9, 4);
      unmade = true;
      /* That line is the run's first and is already past its own C0=0,
         which is where the lines below are counted, so it is counted
         here instead. */
      opened_mid_line = true;
    }
    crtc_tick(&crtc);
    if (crtc.c0 != 0) {
      continue;
    }
    bool running = crtc.vertical_adjustment_in_progress;
    if (running && !standing) {
      lines = opened_mid_line ? 1 : 0;
      opened_mid_line = false;
    }
    if (running) {
      lines++;
      /* The two frames before opened their runs with the comparison true
         and R5 above 0, so a state carried rather than taken would stand
         latched going in. This run's own opening had it false. */
      /* Cancelled on the run's third line, by which point the row has
         reached R9 once and carried C4 a step past R4: a state taken here
         would have to climb the whole counter to find R4 again, where one
         cancelled while C4 still stood on R4 would be released on the very
         next line and tell us nothing. */
      if (unmade && !cancelled && lines == 3) {
        write_register(5, 0);
        cancelled = true;
      }
      if (lines > GIVE_UP_AFTER) {
        break;
      }
    }
    if (standing && !running) {
      spent_runs++;
      /* Two runs spent with R5 above 0, and the state stands latched. */
      if (unmade) {
        ended = true;
      }
    }
    standing = running;
  }
  TEST_CHECK(ended);
  TEST_EQUAL(lines, 32);
}

/* The other half of that parenthesis. A line whose C4 has already gone past
   R4 can still arm a run, because R5's own window asks only that the row be
   on its last scanline (ch. 11.2.2, 12.2, 13.2.1) — so a program that moves
   R4 down under its own row counter opens a run on a line that was never
   going to end the frame. "C4 should return to 0 at the end of the frame
   (C4=R4, C9=R9)" is false there as surely as it is on a last line unmade,
   and no state is taken: an R5 cancelled inside such a run is ch. 11.3.1's
   plain overflow, the thirty-two a five-bit counter has. A chip that read
   only the C9 half of the parenthesis would hold the frame instead, until C4
   had climbed its whole counter to find R4 again. */
static void a_type_1_takes_no_state_where_c4_is_past_r4(void) {
  enum { GIVE_UP_AFTER = 4096 };
  crtc_init(&crtc, 1);
  write_register(0, 63);
  write_register(1, 40);
  write_register(2, 46);
  write_register(3, 0x8E);
  write_register(4, 8);
  write_register(9, 3);
  write_register(5, 2);
  write_register(6, 25);
  write_register(7, 60);
  int lines = 0;
  bool standing = false;
  bool moved = false;
  bool cancelled = false;
  bool ended = false;
  for (long character = 0; character < 400L * 64 * 400 && !ended; character++) {
    /* R4 taken below C4 on a row's last scanline, which leaves the row
       armed for a run it cannot end a frame with. */
    if (!moved && crtc.c0 == 0 && crtc.c4 == 4 && crtc.c9 == 3) {
      write_register(4, 2);
      moved = true;
    }
    crtc_tick(&crtc);
    if (crtc.c0 != 0) {
      continue;
    }
    bool running = crtc.vertical_adjustment_in_progress;
    if (running && !standing) {
      lines = 0;
    }
    if (running) {
      lines++;
      if (moved && !cancelled && lines == 2) {
        write_register(5, 0);
        cancelled = true;
      }
      if (lines > GIVE_UP_AFTER) {
        break;
      }
    }
    if (standing && !running && cancelled) {
      ended = true;
    }
    standing = running;
  }
  TEST_CHECK(ended);
  TEST_EQUAL(lines, 32);
}

/* A last line can be made as well as unmade. "It is therefore not necessary
   to anticipate the programming of R4 (or R9) on the current line for the
   last line condition to be true on the following line. It is possible to
   modify R4 or R9 on the current line as long as C0<2 to validate the 'Last
   Line' state (and thus validate the reset of C4 on the following line)"
   (ch. 12.2). So a row that was going to be an ordinary one ends the frame,
   and C4 comes back to 0 under it.

   The chapter's window is C0<2 and it counts where a write is made, which
   settles the first two cases below. The third is outside it: this type "no
   longer repeats this test on the other values of C0>1". The write at C0=1
   is the one this file reads a character later, where it is in force — the
   same place the chapter's converse is read, a write there unmaking a last
   line and spending it on an adjustment instead. */
static void a_write_can_make_a_last_line_as_well_as_unmake_one(void) {
  static const struct {
    uint8_t written_at; /* the character the write is made on */
    bool ends_the_frame;
  } cases[] = {
      {0, true},  /* inside the chapter's window */
      {1, true},  /* its last character */
      {2, false}, /* outside it, and no test is repeated there */
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, 0);
    write_register(0, 63);
    write_register(1, 40);
    write_register(2, 46);
    write_register(3, 0x8E);
    write_register(4, 10);
    write_register(9, 3);
    write_register(5, 0);
    write_register(6, 25);
    write_register(7, 60);
    bool written = false;
    bool ended = false;
    /* Row 4 is an ordinary one until R4 is written to name it. */
    for (long character = 0; character < 40L * 64 * 20; character++) {
      if (!written && crtc.c4 == 4 && crtc.c9 == 3 && crtc.c0 == cases[index].written_at) {
        write_register(4, 4);
        written = true;
      }
      crtc_tick(&crtc);
      if (written && crtc.c0 == 0 && crtc.c9 == 0) {
        ended = crtc.c4 == 0;
        break;
      }
    }
    TEST_CHECK(written);
    TEST_EQUAL(ended, cases[index].ends_the_frame);
  }
}

static void unselected_chip_ignores_the_bus(void) {
  crtc_init(&crtc, 0);
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
   renewed it when the adjustment ended would let one frame take two lines; one that
   never renewed it would withhold a later frame's without a word (ch. 11.9,
   13.2.1, 19.6.1). */
static void an_adjustment_in_progress_keeps_its_interlace_line(void) {
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
  TEST_CHECK(crtc.vertical_adjustment_armed);
  TEST_CHECK(crtc.interlace_line_given);
  TEST_EQUAL(crtc.c4, 1); /* R4+1, incremented once for the whole of it */

  /* R4 and R9 moved onto the counters in time for the character C0 names 3,
     R4 and R9 are moved back onto the counters and R8 with them, so that
     every condition the disarm tests is in place and only the adjustment
     already begun stands in its way. */
  write_register(4, 1);
  write_register(9, 0);
  write_register(8, 0);
  run_characters(4);
  characters += 4;
  /* Past cancelling: "the additional management being in progress, it can
     no longer be cancelled on C0=2" (ch. 13.2.6). A line earlier the same
     writes would have taken the arming back; here nothing does. */
  TEST_CHECK(crtc.vertical_adjustment_armed);
  TEST_CHECK(crtc.interlace_line_given);

  /* So the frame ends with the one line it was owed — its own four and the
     interlace line — where a chip that renewed the flag when the adjustment ended would
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
  int armed_characters = 0;
  bool entered = false;
  for (long character = 0; character < 600L * SCANLINE; character++) {
    crtc_tick(&crtc);
    if (crtc.vertical_adjustment_armed) {
      entered = true;
      armed_characters++;
    } else if (entered) {
      break;
    }
  }
  /* The interlace line and the last line before it: the chip assesses the
     last line at C0=0 and C0=1 and "arms an internal flag by default" there
     (ch. 12.1, 13.2.5), so the flag stands for the whole of the last line
     and not only from the C0=R0 that asks for the interlace line. */
  TEST_EQUAL(armed_characters, 2 * SCANLINE);
}

/* Ch. 13.2.2 puts a state between the C4/R7 equality and the VSYNC it
   raises: "Each time C0=2, a state validates the update of C4=R7 on the
   next C0=0. This state is cancelled when C0=0." A line that never reaches
   C0=2 therefore leaves the next line's equality unread, and R0 below 2 is
   how a program arranges it — "if R0 is modified with a value lower than 2
   on the line preceding the equivalence C4=R7 (On the line where C9=R9 and
   C4=R7-1), the VSYNC will not be authorized on C0=0". Shaker calls this
   one LOST VSYNC ON R0=0 and LOST VSYNC ON R0=1, asks it of both, and reads
   the sync back off PPI port B on each of the twelve lines it prints: bit 0
   clear, where before this state was here it stood. */
static void a_line_too_short_to_reach_c0_2_costs_the_next_its_vsync(void) {
  for (uint8_t narrow_r0 = 0; narrow_r0 <= 1; narrow_r0++) {
    program_standard();
    TEST_CHECK(run_to_row(29));
    run_scanlines(7); /* the line before C4 reaches R7, C9 already at R9 */
    TEST_EQUAL(crtc.c4, 29);
    TEST_EQUAL(crtc.c9, 7);
    write_register(0, narrow_r0);
    /* 256 characters is a hundred and twenty-eight of these short lines,
       well past where C4 walks onto R7 and off it again. */
    for (int character = 0; character < 256; character++) {
      TEST_CHECK(!(crtc_tick(&crtc) & CRTC_VSYNC));
    }
  }
}

/* And the state is a state, not a rule read afresh: R0 put back before the
   line has passed C0=2 arms it after all, and the VSYNC comes as it would
   have. */
static void an_r0_restored_before_c0_2_keeps_the_vsync(void) {
  program_standard();
  TEST_CHECK(run_to_row(29));
  run_scanlines(7);
  write_register(0, 1);
  crtc_tick(&crtc);      /* draws C0=1, the last character an R0 of 1 allows */
  write_register(0, 63); /* in force from C0=2, which therefore arrives */
  int vsync_characters = 0;
  for (int character = 0; character < 12 * SCANLINE; character++) {
    if (crtc_tick(&crtc) & CRTC_VSYNC) {
      vsync_characters++;
    }
  }
  TEST_EQUAL(vsync_characters, 8 * SCANLINE);
}

/* The equality is not merely missed but spent: "This will also cause the
   VSYNC to block for the value C4=R7", so a program that widens R0 again
   on the same line does not get the VSYNC back, and only the liftings of
   ch. 16.3 — a C4 that moves, or an R7 written — bring it round again. The
   widening here comes two short lines later, on a later line of the same
   row, which is as long as C4 stands still. */
static void a_vsync_lost_to_a_short_line_stays_lost_for_that_row(void) {
  program_standard();
  TEST_CHECK(run_to_row(29));
  run_scanlines(7);
  write_register(0, 1);
  run_characters(2 * 2); /* two of the two-character lines */
  write_register(0, 63);
  for (int character = 0; character < 4 * SCANLINE; character++) {
    TEST_CHECK(!(crtc_tick(&crtc) & CRTC_VSYNC));
  }
  /* R7 written lifts the block, and the equality serves again (ch. 16.3).
     Begun away from the head of a line, the pulse runs R3's eight lines and
     the rest of the line it began in besides (ch. 16.4.1). */
  while (crtc.c0 != 2) {
    crtc_tick(&crtc);
  }
  write_register(7, crtc.c4);
  int vsync_characters = 0;
  for (int character = 0; character < 12 * SCANLINE; character++) {
    if (crtc_tick(&crtc) & CRTC_VSYNC) {
      vsync_characters++;
    }
  }
  TEST_EQUAL(vsync_characters, 8 * SCANLINE + (63 - 2));
}

/* "A value lower than 2" is the whole of the condition, and 2 itself is on
   the safe side of it: a line of three characters reaches C0=2 and arms, a
   line of two does not (ch. 13.2.2). */
static void a_line_of_three_characters_still_arms_the_vsync(void) {
  program_standard();
  TEST_CHECK(run_to_row(29));
  run_scanlines(7);
  write_register(0, 2);
  int vsync_characters = 0;
  for (int character = 0; character < 30 * 3; character++) { /* thirty lines */
    if (crtc_tick(&crtc) & CRTC_VSYNC) {
      vsync_characters++;
    }
  }
  TEST_EQUAL(vsync_characters, 8 * 3); /* R3's eight lines, three characters each */
}

/* Ch. 16.3's own degenerate case: "if R7=0, then a VSYNC occurs when C4=0.
   If R4 is 0, then C4 remains at 0 ... C4 is therefore 0 during the VSYNC
   but also after the end of the VSYNC. In this context, there is no more
   VSYNC." Exactly one, then — which is also what says the chip wakes with
   ch. 13.2.2's state standing, since C4 never moves to lift a block, and a
   chip that woke without it would give none at all. */
static void an_r7_and_r4_of_zero_give_one_vsync_and_no_more(void) {
  crtc_init(&crtc, 0);
  write_register(0, 63);
  write_register(1, 40);
  write_register(3, 0x8E);
  write_register(4, 0);
  write_register(9, 3);
  /* R7 is left at the zero power-on gives it, so the equality is never made
     by hand — and C4, which an R4 of 0 never moves, never lifts a block. */
  int vsync_characters = 0;
  for (long character = 0; character < 200L * SCANLINE; character++) {
    if (crtc_tick(&crtc) & CRTC_VSYNC) {
      vsync_characters++;
    }
  }
  TEST_EQUAL(vsync_characters, 8 * SCANLINE);
}

/* Ch. 16.4.1.1 gives the equality made by hand a rule of its own, which is
   not the arming of ch. 13.2.2: "the VSYNC is triggered immediately if it
   was not already in progress, except if this modification occurs when
   C0vs=0 or C0vs=1. If the modification of R7 with the value of C4 took
   place when C0vs<2, we are in a BLOCKED VSYNC." The chapter reads it off
   the PPI six microseconds after the write, and finds the sync inactive. */
static void an_r7_written_at_a_lines_head_blocks_instead_of_triggering(void) {
  for (uint8_t at = 0; at <= 2; at++) {
    program_standard();
    write_register(7, 100); /* beyond C4's reach, so nothing walks into it */
    TEST_CHECK(run_to_row(10));
    while (crtc.c0 != at) {
      crtc_tick(&crtc);
    }
    write_register(7, crtc.c4);
    bool sync = false;
    for (int character = 0; character < 6; character++) {
      sync = sync || (crtc_tick(&crtc) & CRTC_VSYNC) != 0;
    }
    TEST_EQUAL(sync, at == 2);
  }
}

/* And that is the whole of the condition on it: a line that never armed
   still takes the VSYNC an R7 write makes on it, because the arming governs
   the equality C4 walks into, while "when R7=C4 with C0vs>1, the VSYNC is
   'triggered' during the line" carries no such clause (ch. 16.4.1.1). */
static void an_r7_written_on_an_unarmed_line_still_triggers(void) {
  program_standard();
  write_register(7, 100);
  TEST_CHECK(run_to_row(10));
  write_register(0, 1);
  run_characters(2); /* a line of two characters, which never reaches C0=2 */
  write_register(0, 63);
  while (crtc.c0 != 10) {
    crtc_tick(&crtc);
  }
  write_register(7, crtc.c4);
  TEST_CHECK(crtc_tick(&crtc) & CRTC_VSYNC);
}

/* The management has no more defined a start than the VSYNC's permission
   does, and this chip wakes holding it — a chip that has been running has
   passed C0=1 more times than anyone can count. It shows only where R0 is
   0 from the first character, which is what power-on leaves it: one
   boundary lands, and then the freeze. */
static void the_chip_wakes_with_its_counters_managed(void) {
  crtc_init(&crtc, 0);
  write_register(9, 7); /* a row of eight lines, so C9 is what the boundary moves */
  for (int character = 0; character < 16; character++) { /* a dozen and more */
    crtc_tick(&crtc);
  }
  TEST_EQUAL(crtc.c9, 1);
  TEST_EQUAL(crtc.c4, 0);
}

/* Every last line is armed for an adjustment, whether or not anything wants
   one — "the CRTC assesses whether it is on the last line, and if so, arms
   an internal flag by default", C0=2 being left to "assess the conditions
   for disarming ... in particular by testing the value of R5" (ch. 12.1,
   13.2.5). The same assessment takes it back where the last line is unmade
   under it: ch. 12.2 has that state "overridden if R4 or R9 is modified to
   C0==0 so that C4 becomes different from R4", and that the arming goes with
   it is our reading of that rather than its own words. */
static void every_last_line_is_armed_and_an_unmade_one_disarmed(void) {
  program_standard();
  write_register(5, 0);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);                           /* the last line, C0=0 drawn */
  TEST_CHECK(crtc.vertical_adjustment_armed); /* armed, though R5 asks nothing */
  run_characters(3);                          /* past the C0=3 the disarm reads */
  TEST_CHECK(!crtc.vertical_adjustment_armed);

  program_standard();
  write_register(5, 0);
  TEST_CHECK(run_to_row(38));
  run_scanlines(7);
  write_register(4, 39); /* in force from C0=1, unmaking the last line */
  crtc_tick(&crtc);
  TEST_CHECK(!crtc.vertical_adjustment_armed);
}

/* A line of three characters still reaches its disarm, though the character
   this file reads it at does not exist there: ch. 11.2.2 and ch. 12.2 part
   the narrow lines at "R0 < 2", so an R0 of 2 is on the wide side and the
   frame ends where it would have ended. */
static void a_line_of_three_characters_still_reaches_its_disarm(void) {
  program_standard();
  write_register(4, 1);
  write_register(5, 0);
  write_register(9, 1);
  TEST_CHECK(run_to_row(1));
  run_scanlines(1); /* C4=1=R4, C9=1=R9: the frame's last line */
  write_register(0, 2);
  run_characters(2);
  write_register(4, 0); /* so the counters cannot end the frame on their own */
  int lines = 0;
  for (int character = 0; character < 200; character++) {
    crtc_tick(&crtc);
    if (crtc.c0 == 0) {
      lines++;
      if (crtc.c4 == 0 && crtc.c9 == 0) {
        break;
      }
    }
  }
  TEST_EQUAL(lines, 1); /* the next line is the frame's first */
}

/* Ch. 13.2.6's worked case. "If C9=R9 and C4=R4 (when R0 goes to 0), then
   ... an additional management is activated, and which will remain so when
   C0 can once again exceed 1. It is then R5 which controls the end of the
   additional management. To stop this management, program R5 with C9+1."
   The chapter's own summary is shorter: "if C9=R9 and C4=R4 then C4=R4+1.
   When R0>0, C4 is managed by C9/R5." So the frozen row advance is not a
   row advance at all but the adjustment beginning, and the line it lands on
   is one the assessment at C0=0 can no longer take back — which it would,
   C4 having just moved off R4 and the last line with it. */
static void a_freeze_on_a_last_line_begins_an_adjustment(void) {
  program_standard();
  write_register(5, 0);
  for (long character = 0; character < 400L * SCANLINE; character++) {
    crtc_tick(&crtc);
    if (crtc.c4 == 38 && crtc.c9 == 7 && crtc.c0 == 0) {
      break;
    }
  }
  write_register(0, 0); /* C0 already stands at 0, so it stays there */
  for (int character = 0; character < 8; character++) {
    crtc_tick(&crtc);
  }
  TEST_EQUAL(crtc.c4, 39); /* R4+1 */
  TEST_CHECK(crtc.vertical_adjustment_in_progress);

  /* Widened again: "C9+1 being different from R5, then C9 is incremented"
     (ch. 13.2.6), so C9 climbs from the 7 the freeze left it at while C4
     holds where it landed. */
  write_register(0, 63);
  run_scanlines(4);
  TEST_EQUAL(crtc.c4, 39);
  TEST_EQUAL(crtc.c9, 11);

  /* "To stop this management, program R5 with C9+1." */
  write_register(5, 12);
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.c9, 0);
}

/* Ch. 11.2.2 lists the ways an adjustment comes about with R5 at 0: an R4 or
   R9 moved at C0=1 of a last line, "or if C0 can never reach 2 because R0 <
   2". Ch. 13.2.1 and ch. 13.2.5 say what that second one draws, both of them
   inside their own R0=1 case: it lasts "1 line of 2 usec before ceasing
   (C4+1, C9=0)", and only "on the next line" does "the end of additional
   management reset C4 and C9 to 0". Ch. 13.2.5 draws the picture — "when
   R4=R9=0, each 'line' of 2 usec is therefore immediately followed by a
   'line' of 2 usec for which C9=0 and C4=1" — so the lines alternate. A wider
   line measures before it gives, which is ch. 13.2.4's reminder and what
   Shaker's graded E (1) holds this chip to. */
static void a_narrow_line_draws_the_adjustment_it_cannot_disarm(void) {
  crtc_init(&crtc, 0);
  write_register(0, 1);
  write_register(1, 40);
  write_register(3, 0x8E);
  write_register(4, 0);
  write_register(5, 0);
  write_register(6, 25);
  write_register(7, 30);
  write_register(9, 0);
  crtc_tick(&crtc); /* the priming tick draws no character */

  /* Every line is a last line here, so every one is armed and none is
     disarmed, and they come out as the chapter's picture has them. */
  static const uint8_t alternating[6][2] = {{1, 0}, {0, 0}, {1, 0}, {0, 0}, {1, 0}, {0, 0}};
  for (int line = 0; line < 6; line++) {
    run_characters(2); /* a line of two characters */
    TEST_EQUAL(crtc.c4, alternating[line][0]);
    TEST_EQUAL(crtc.c9, alternating[line][1]);
  }

  /* And a row of four scanlines puts that line after the last of them:
     "this 'line' of 2 usec (for which C4=1) occurs after the last value of
     C9" (ch. 13.2.5). */
  crtc_init(&crtc, 0);
  write_register(0, 1);
  write_register(3, 0x8E);
  write_register(4, 0);
  write_register(5, 0);
  write_register(6, 25);
  write_register(7, 30);
  write_register(9, 3);
  crtc_tick(&crtc);
  static const uint8_t after_the_row[5][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 0}, {0, 0}};
  for (int line = 0; line < 5; line++) {
    run_characters(2);
    TEST_EQUAL(crtc.c4, after_the_row[line][0]);
    TEST_EQUAL(crtc.c9, after_the_row[line][1]);
  }
}

/* And the ceasing after one line belongs to the R0=1 case alone. Ch. 13.2.6
   gives a line of one character the other ending: the run "will remain so
   when C0 can once again exceed 1. It is then R5 which controls the end of
   the additional management. To stop this management, program R5 with
   C9+1." Its table is walked here row by row — the frozen line, three
   widened ones with C4 held and C9 climbing, and the ending the chapter
   prescribes. */
static void a_run_begun_under_a_stopped_picture_ends_on_r5(void) {
  program_standard();
  write_register(4, 0);
  write_register(5, 0);
  write_register(9, 0);
  for (long character = 0; character < 400L * SCANLINE; character++) {
    crtc_tick(&crtc);
    if (crtc.c4 == 0 && crtc.c9 == 0 && crtc.c0 == 20) {
      break;
    }
  }
  write_register(0, 0); /* narrowed away from the line's own head */
  for (int character = 0; character < 400 && crtc.c0 != 0; character++) {
    crtc_tick(&crtc); /* C0 runs to its top and comes back the long way */
  }
  TEST_CHECK(crtc.c0 == 0);
  run_characters(4);
  TEST_EQUAL(crtc.c4, 1); /* R4+1, and the run begun */
  TEST_EQUAL(crtc.c9, 0);

  write_register(0, 63);
  for (uint8_t line = 1; line <= 3; line++) {
    run_scanlines(1);
    TEST_EQUAL(crtc.c4, 1); /* held where the stopping left it */
    TEST_EQUAL(crtc.c9, line);
  }
  write_register(5, (uint8_t)(crtc.c9 + 1));
  run_scanlines(1);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.c9, 0);
}

/* Ch. 15.3.1: "On CRTC 0, two HSYNC's cannot be contiguous if position
   C0=R2 is encountered when C3l reaches R3l, and R3l has not been modified
   on this position." It is what keeps a line shorter than its own sync out
   of the endless HSYNC the other types fall into — ch. 15.3.2 sets R0=0,
   R2=0 and R3l=1 and says "on a CRTC 0, the HSYNC will not take place. It
   will occur on the 3rd C0=0", so the syncs alternate with the characters
   between them. */
static void two_hsyncs_cannot_be_contiguous(void) {
  crtc_init(&crtc, 0);
  write_register(0, 0);
  write_register(2, 0);
  write_register(3, 0x01);
  write_register(4, 4);
  write_register(9, 1);
  for (int character = 0; character < 16; character++) {
    TEST_EQUAL((crtc_tick(&crtc) & CRTC_HSYNC) != 0, character % 2 == 0);
  }

  /* A sync four characters wide on a line of four ends where it began, so
     the same block holds it to every other line. */
  crtc_init(&crtc, 0);
  write_register(0, 3);
  write_register(2, 0);
  write_register(3, 0x04);
  write_register(4, 4);
  write_register(9, 1);
  for (int character = 0; character < 16; character++) {
    TEST_EQUAL((crtc_tick(&crtc) & CRTC_HSYNC) != 0, (character / 4) % 2 == 0);
  }
}

/* And the exception, which is where a R2.JIT HSYNC begins: "on this
   position, if C0 is again equal to R2 but R3l is modified, then a new
   HSYNC-CRTC begins without C3l being zeroed" (ch. 15.3.3). The write is
   what is read here, not the value it carries — the same reading ch. 16.3
   states outright for R7, "whatever the value written" — so a rewrite of
   the value R3l already holds reaches it, and the sync it lets through runs
   until C3l comes round the four bits to R3l again. Nothing outside this
   repository grades that reading: a write that changes R3l cannot be told
   apart here, the comparison that ends the sync reading the new value on
   the same character. */
static void an_r3_written_in_time_carries_the_hsync_on(void) {
  crtc_init(&crtc, 0);
  write_register(0, 0);
  write_register(2, 0);
  write_register(3, 0x04);
  write_register(4, 4);
  write_register(9, 1);
  for (int character = 0; character < 4; character++) {
    TEST_CHECK(crtc_tick(&crtc) & CRTC_HSYNC); /* C3l counting towards R3l */
  }
  write_register(3, 0x04); /* in force on the character C3l reaches it */
  for (int character = 0; character < 16; character++) {
    TEST_CHECK(crtc_tick(&crtc) & CRTC_HSYNC); /* let through, and counting on */
  }
  TEST_CHECK(!(crtc_tick(&crtc) & CRTC_HSYNC));
}

/* Ch. 14.5: "it is possible to change the value of R3l when C3l counts,
   which can affect the length of the HSYNC. If R3l is changed with a value
   less than C3l, then C3l is overflowing". Ch. 14.5.4 gives the other half,
   the one a program aims at: "if R3l is modified ... with the value of C3l
   when C0 is at position which corresponds to C3l while R3l was greater
   than this value, then the HSYNC stops on CRTC's 0, 1 and 2. This
   technique is called R3.JIT". Ch. 14.5.1 draws both for this type at R2=11
   and R3l=10, and the widths below are read off that diagram: written with
   0, C3l runs 0 to 15 and round to 0 again; written with 1, one character
   further. */
static void an_r3l_written_during_a_hsync_stops_it_or_overflows(void) {
  static const struct {
    uint8_t written; /* the R3l in force on the sync's sixth character */
    int characters;  /* and the width the sync comes out */
  } cases[] = {
      {10, 10}, /* the value it already holds, which changes nothing */
      {5, 5},   /* R3.JIT: the value C3l stands at, and the sync stops */
      {0, 16},  /* under it, so C3l overflows and runs round to it */
      {1, 17},
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    crtc_init(&crtc, 0);
    write_register(0, 63);
    write_register(2, 11);
    write_register(3, 0x8A);
    write_register(4, 4);
    write_register(9, 1);
    int characters = 0;
    bool written = false;
    for (int character = 0; character < 40; character++) {
      if (crtc_tick(&crtc) & CRTC_HSYNC) {
        characters++;
      } else if (characters > 0) {
        break;
      }
      if (characters == 5 && !written) {
        write_register(3, (uint8_t)(0x80 | cases[index].written));
        written = true;
      }
    }
    TEST_EQUAL(characters, cases[index].characters);
  }
}

/* A line of one character never reaches C0=1, so "C9 processing management"
   is never enabled again and "all of the CRTC counters are frozen as long as
   R0=0" (ch. 13.2.1, 13.2.4). What the last managed line had already decided
   still lands, and only one part of it can: "if C9 had reached R9 on the
   first C0=0, then the reset of C9 had been armed as well as the increment
   to C4. With C9 being frozen, only C4 will increment", and once only,
   "because it has taken place". */
static void a_line_of_one_character_freezes_the_counters(void) {
  static const struct {
    uint8_t from_c9; /* the row's scanline when R0 is taken to 0 */
    uint8_t at_c0;   /* and the character the write is made on */
    uint8_t settles_c4;
    uint8_t settles_c9;
  } cases[] = {
      /* Past C0=1, so the management still stands and one boundary runs
         managed before the freeze takes. */
      {3, 5, 10, 4},
      {6, 5, 11, 7},
      /* And on C0=0 itself, where no boundary runs managed at all and the
         armed increment is the whole of what happens. */
      {7, 0, 11, 7},
  };

  for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
    program_standard();
    TEST_CHECK(run_to_row(10));
    run_scanlines(cases[index].from_c9);
    run_characters(cases[index].at_c0);
    write_register(0, 0);
    if (cases[index].at_c0 != 0) {
      /* C0 stood past 0 when R0 became 0, so it climbs to its own top and
         comes back the long way; that wrap is the first C0=0 with R0=0. */
      for (int character = 0; character < 400 && crtc.c0 != 0; character++) {
        crtc_tick(&crtc);
      }
      TEST_CHECK(crtc.c0 == 0);
    }
    for (int character = 0; character < 8; character++) { /* well past a second */
      crtc_tick(&crtc);
    }
    TEST_EQUAL(crtc.c4, cases[index].settles_c4);
    TEST_EQUAL(crtc.c9, cases[index].settles_c9);
  }
}

/* The VSYNC's line counter is frozen with the rest of them, which is what
   leaves a pulse begun on such a line running for ever: "the VSYNC line
   counter C3h is frozen, and the VSYNC is not deactivated if R3h was worth
   1 (because C3h can no longer reach R3h)". A line of two characters keeps
   its management, so there C3h counts and the pulse "will have lasted 2
   µsec in total" (ch. 16.4.1.2). */
static void a_line_of_one_character_leaves_a_vsync_running_where_two_do_not(void) {
  for (uint8_t narrow_r0 = 0; narrow_r0 <= 1; narrow_r0++) {
    program_standard();
    /* R3h of 1: one line of VSYNC, over the HSYNC width R3l already held. */
    write_register(3, 0x1E);
    uint64_t pins;
    do {
      pins = crtc_tick(&crtc);
    } while (!(pins & CRTC_VSYNC) || crtc.c0 != 0);
    write_register(0, narrow_r0);
    if (narrow_r0 == 0) {
      /* Eight times over the one line R3h asked for, and still running. */
      for (int character = 0; character < 8 * SCANLINE; character++) {
        TEST_CHECK(crtc_tick(&crtc) & CRTC_VSYNC);
      }
    } else {
      TEST_CHECK(crtc_tick(&crtc) & CRTC_VSYNC); /* the second of its two */
      TEST_CHECK(!(crtc_tick(&crtc) & CRTC_VSYNC));
    }
  }
}

/* The freeze shuts out the registers that feed the counters — "updates to
   registers R4, R5 and R9 are no longer considered as long as R0=0" — but
   not R8: "on the other hand, R8 continues to be considered each time
   C0=0" (ch. 13.2.1). So the interlace video mode's doubling is taken up
   under a frozen chip, and the raster address moves though no counter
   does. */
static void a_frozen_chip_still_reads_r8(void) {
  program_standard();
  TEST_CHECK(run_to_row(10));
  run_scanlines(3);
  run_characters(5);
  write_register(0, 0);
  for (int character = 0; character < 400 && crtc.c0 != 0; character++) {
    crtc_tick(&crtc);
  }
  TEST_CHECK(crtc.c0 == 0);
  TEST_EQUAL(crtc_ra(crtc_tick(&crtc)), 4); /* C9, frozen where it landed */
  write_register(8, 3);
  uint8_t raster = 0;
  for (int character = 0; character < 8; character++) {
    raster = crtc_ra(crtc_tick(&crtc));
  }
  TEST_EQUAL(raster, 8); /* the same C9, doubled */
}

/* An adjustment moves C4 once for all its lines together, "and in
   additional management one only once if C4 was worth R4" (ch. 13.2.1), so
   what the freeze keeps against the next boundary has to ask the same
   question the boundary itself asks. A chip that armed on the row's last
   scanline alone would move C4 a second time under a still picture. */
static void a_frozen_adjustment_moves_c4_no_further(void) {
  program_standard();
  write_register(5, 20);
  for (long character = 0; character < 400L * SCANLINE; character++) {
    crtc_tick(&crtc);
    if (crtc.vertical_adjustment_armed && crtc.c9 == 7 && crtc.c4 == 39 && crtc.c0 == 0) {
      break;
    }
  }
  TEST_EQUAL(crtc.c4, 39); /* R4+1, and there it stays for the whole of it */
  write_register(0, 0);
  for (int character = 0; character < 8; character++) {
    crtc_tick(&crtc);
  }
  TEST_EQUAL(crtc.c4, 39);
}

/* ParityFrame turns as a frame's first character is entered (ch. 19.5.2),
   which is an event and not a comparison standing: a chip frozen on that
   character enters nothing more, and its parity — which the raster address
   and the VSYNC's placement both read — must hold with the counters. */
static void a_chip_frozen_on_a_frame_head_keeps_its_parity(void) {
  program_standard();
  write_register(6, 0);
  write_register(8, 3);
  crtc_tick(&crtc); /* the priming tick draws no character */
  for (long character = 0; character < 400L * SCANLINE; character++) {
    crtc_tick(&crtc);
    if (crtc.c4 == 0 && crtc.c9 == 0 && crtc.c0 == 0) {
      break;
    }
  }
  TEST_EQUAL(crtc.c0, 0);
  write_register(0, 0);
  bool parity = crtc.parity_frame;
  for (int character = 0; character < 20; character++) {
    crtc_tick(&crtc);
    TEST_EQUAL(crtc.parity_frame, parity);
  }
}

/* ParityFrame is settled before the VSYNC is looked at, which only shows
   where R7 is 0 and the two fall on the same character: the frame that has
   just become even takes a MID-VSYNC in its own first line (ch. 19.7.2). */
static void the_parity_settles_before_an_r7_of_zero_is_read(void) {
  vsync_seen seen;

  program_standard();
  /* R7 is written where C4 is not 0, so the equality is left for the frame
     to walk into rather than made by hand (ch. 16.4.1.1). */
  TEST_CHECK(run_to_row(1));
  run_characters(2);
  write_register(7, 0);
  write_register(8, 1);
  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(seen.odd_frame);
  TEST_EQUAL(seen.character, 0);

  TEST_CHECK(next_vsync(&seen));
  TEST_CHECK(!seen.odd_frame);
  TEST_EQUAL(seen.character, 31);
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
     frame's own and the rows after it are whole. R1 is put where C0 cannot
     reach it, which is the chapter's own way of holding the conflict open:
     an R1 met while R6 still stands at 0 makes the border definitive and
     there would be no row left to look at (ch. 18.3.2). */
  program_standard();
  write_register(6, 0);
  write_register(9, 0);
  write_register(1, 64); /* R0 + 1 */
  TEST_CHECK(run_to_row(1));
  for (int character = 0; character < 8; character++) {
    uint64_t pins = crtc_tick(&crtc);
    TEST_CHECK(pins & CRTC_DISPTMG);
    TEST_EQUAL((pins & CRTC_DISPTMG_SECOND_BYTE) != 0, crtc.c0 != 63);
  }
}

/* The frame's first line is where an R6 of 0 can still be taken back, and
   the character C0 meets R1 on is the deadline: "in this situation however,
   if R6 is 0 when C0=R1, the BORDER becomes definitive", where an R1 put out
   of C0's reach leaves it cancellable — "if we prevent C0=R1 on the line
   C4=C9=0 (for example R1=R0+1), and R6 is no longer equal to 0, then the
   BORDER is deactivated on the following line" (ch. 18.3.2). Ch. 18.3.3
   settles the same standing on a type 1 the other way, and ch. 18.3.4 says
   types 3 and 4 never meet it. */
static void an_r6_of_zero_is_taken_back_only_before_r1(void) {
  static const struct {
    const char *what;
    uint8_t r1;         /* 40 is met on the line; 64 is not */
    bool taken_back;    /* whether R6 is written above 0 on the first line */
    bool borders_after; /* what the frame's second row comes out as */
  } cases[] = {
      {"taken back after C0 met R1", 40, true, true},
      {"taken back with R1 out of reach", 64, true, false},
      {"never taken back", 40, false, true},
  };
  for (unsigned index = 0; index < sizeof cases / sizeof *cases; index++) {
    program_standard();
    write_register(9, 0); /* a row to a line, so the second row is the second line */
    write_register(1, cases[index].r1);
    write_register(6, 0);
    TEST_CHECK(run_to_row(0));
    /* Along the first line, past R1 wherever it stands. */
    for (int character = 0; character < 50; character++) {
      crtc_tick(&crtc);
    }
    if (cases[index].taken_back) {
      write_register(6, 25);
    }
    TEST_CHECK(run_to_row(1));
    uint64_t pins = crtc_tick(&crtc);
    TEST_EQUAL((pins & CRTC_DISPTMG) == 0, cases[index].borders_after);
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
   line's end is the plainest way to miss the condition, and a line that
   misses it earns the same border whatever the delay: half a character
   early with none, a whole one at the deferred place with one. */
static void a_line_that_misses_r1_borders_at_r0(void) {
  for (uint8_t skew = 0; skew <= 2; skew++) {
    program_standard();
    write_register(1, 64);
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
     stands, so long as the line is past its second character — the head of
     a line has a rule of its own (ch. 16.4.1.1). The same equality cannot
     start a second: C4 must move, or R7 must be written again (ch. 16.3,
     16.4.1). */
  program_standard();
  write_register(9, 31); /* rows long enough to hold a whole VSYNC */
  TEST_CHECK(run_to_row(1));
  TEST_CHECK(!crtc.vsync);

  run_characters(2); /* past C0=1, where a hand-made equality is blocked */
  write_register(7, 1);
  crtc_tick(&crtc);
  TEST_CHECK(crtc.vsync);
  /* Begun away from the head of the line, so the counter is initialized at
     the head of the next one and R3's eight lines are counted from there —
     the rest of this line runs on top of them. "The total duration of the
     VSYNC is increased by the number of µsec corresponding to the
     calculation R0 - C0vs", which is 63 less the 2 the write landed on
     (ch. 16.4.1, 16.4.1.1). */
  run_characters(8 * SCANLINE + (63 - 2) - 1);
  TEST_CHECK(crtc.vsync);
  run_characters(1);
  TEST_CHECK(!crtc.vsync);
  run_scanlines(10); /* still row 1, and still no second VSYNC */
  TEST_CHECK(!crtc.vsync);

  run_characters(2);
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

/* "If R1 is zeroed then no more characters are displayed, regardless of the
   CRTC of a CPC" (ch. 17.1): the opening and C0=R1 land on the same
   character, and the border has it — whatever the delay, and on the frame's
   first line too, where an R6 of 0 has rules of its own (ch. 18.3.2). */
static void an_r1_of_zero_borders_every_line(void) {
  static const uint8_t r6s[] = {25, 0};
  for (size_t index = 0; index < sizeof r6s / sizeof r6s[0]; index++) {
    for (uint8_t skew = 0; skew <= 2; skew++) {
      program_standard();
      write_register(1, 0);
      write_register(6, r6s[index]);
      write_register(8, (uint8_t)(skew << 4));
      TEST_CHECK(run_to_row(1));
      uint64_t pins = 0;
      do {
        pins = crtc_tick(&crtc);
      } while (crtc.c4 != 0 || crtc.c9 != 0 || crtc.c0 != 0);
      for (int character = 0; character < 64 * 16; character++) {
        if (pins & CRTC_DISPTMG) {
          TEST_FAIL("R6=%u skew %u: C4=%u C9=%u C0=%u displayed with R1 at 0", r6s[index], skew,
                    crtc.c4, crtc.c9, crtc.c0);
          return;
        }
        pins = crtc_tick(&crtc);
      }
    }
  }
}

/* Ch. 17.5.1's four drawings for types 0, 1 and 2: an OUT R1,0 whose write
   reaches the chip before the line's C0=0, or on it, is "just in time" and
   the line is BORDER; one reaching it on C0=1 is "too late" and the line is
   displayed, to be bordered from the next. Written where this harness
   writes everything, after the tick that named the character; the
   character a write lands on keeps the pins it was given (see crtc.c). */
static void r1_of_zero_is_just_in_time_up_to_the_lines_c0_of_0(void) {
  static const struct {
    uint8_t lands_on; /* C0 when the write reaches the chip */
    bool line_displayed;
  } cases[] = {{0x3E, false}, {0x3F, false}, {0x00, false}, {0x01, true}};
  for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
    program_standard();
    TEST_CHECK(run_to_row(1));
    while (crtc.c0 != cases[index].lands_on) {
      crtc_tick(&crtc);
    }
    write_register(1, 0);
    /* The rest of the line the write lands on, or the whole of the next one
       where it lands before C0=0: C0 2 to 63 carry the answer either way,
       and C0=1 too wherever the write came before it. */
    while (crtc.c0 != 1) {
      uint64_t pins = crtc_tick(&crtc);
      if (crtc.c0 == 1) {
        TEST_CHECK(!(pins & CRTC_DISPTMG));
      }
    }
    int displayed = 0;
    for (int character = 2; character <= 63; character++) {
      displayed += (crtc_tick(&crtc) & CRTC_DISPTMG) != 0;
    }
    TEST_EQUAL(displayed, cases[index].line_displayed ? 62 : 0);
    /* And whichever it was, the line after it is border. */
    while (crtc.c0 != 63) {
      crtc_tick(&crtc);
    }
    displayed = 0;
    for (int character = 0; character <= 63; character++) {
      displayed += (crtc_tick(&crtc) & CRTC_DISPTMG) != 0;
    }
    TEST_EQUAL(displayed, 0);
  }
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
  TEST_RUN(each_type_answers_the_read_port_its_own_way);
  TEST_RUN(each_type_keeps_its_own_vsync_length);
  TEST_RUN(types_1_and_2_delay_no_vsync_by_a_whole_line);
  TEST_RUN(types_0_and_1_want_different_r9s_for_a_row);
  TEST_RUN(an_r8_pulse_leaves_the_parities_the_diagrams_draw);
  TEST_RUN(types_0_and_2_alone_can_freeze_their_frame_parity);
  TEST_RUN(each_type_counts_the_adjustment_lines_its_own_way);
  TEST_RUN(a_type_1_holds_a_frame_open_where_r5_is_cancelled);
  TEST_RUN(a_type_1_opens_each_run_with_the_r5_it_has);
  TEST_RUN(a_type_1_takes_no_state_where_c4_is_past_r4);
  TEST_RUN(an_interlace_pulse_fixes_a_type_1_on_an_even_field);
  TEST_RUN(an_r8_write_answers_the_scenarios_the_chapter_draws);
  TEST_RUN(only_type_1_drives_the_status_port);
  TEST_RUN(the_status_border_bit_turns_over_at_a_line_head);
  TEST_RUN(a_write_can_make_a_last_line_as_well_as_unmake_one);
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
  TEST_RUN(an_adjustment_in_progress_keeps_its_interlace_line);
  TEST_RUN(the_interlace_line_is_asked_for_at_c0_r0);
  TEST_RUN(an_adjustment_line_still_decides_the_interlace_line);
  TEST_RUN(the_disarm_tests_the_interlace_line_as_well_as_r5);
  TEST_RUN(an_r4_of_127_does_not_trap_the_frame);
  TEST_RUN(a_line_too_short_to_reach_c0_2_costs_the_next_its_vsync);
  TEST_RUN(an_r0_restored_before_c0_2_keeps_the_vsync);
  TEST_RUN(a_vsync_lost_to_a_short_line_stays_lost_for_that_row);
  TEST_RUN(a_line_of_three_characters_still_arms_the_vsync);
  TEST_RUN(the_chip_wakes_with_its_counters_managed);
  TEST_RUN(every_last_line_is_armed_and_an_unmade_one_disarmed);
  TEST_RUN(a_line_of_three_characters_still_reaches_its_disarm);
  TEST_RUN(a_freeze_on_a_last_line_begins_an_adjustment);
  TEST_RUN(a_narrow_line_draws_the_adjustment_it_cannot_disarm);
  TEST_RUN(a_run_begun_under_a_stopped_picture_ends_on_r5);
  TEST_RUN(two_hsyncs_cannot_be_contiguous);
  TEST_RUN(an_r3_written_in_time_carries_the_hsync_on);
  TEST_RUN(an_r3l_written_during_a_hsync_stops_it_or_overflows);
  TEST_RUN(a_line_of_one_character_freezes_the_counters);
  TEST_RUN(a_line_of_one_character_leaves_a_vsync_running_where_two_do_not);
  TEST_RUN(a_frozen_chip_still_reads_r8);
  TEST_RUN(a_frozen_adjustment_moves_c4_no_further);
  TEST_RUN(a_chip_frozen_on_a_frame_head_keeps_its_parity);
  TEST_RUN(an_r7_and_r4_of_zero_give_one_vsync_and_no_more);
  TEST_RUN(an_r7_written_at_a_lines_head_blocks_instead_of_triggering);
  TEST_RUN(an_r7_written_on_an_unarmed_line_still_triggers);
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
  TEST_RUN(an_r6_of_zero_is_taken_back_only_before_r1);
  TEST_RUN(the_skew_delays_the_border_at_both_ends);
  TEST_RUN(a_skew_carries_the_border_round_the_lines_end);
  TEST_RUN(a_skew_makes_the_early_border_a_whole_character);
  TEST_RUN(the_border_on_function_shuts_the_display);
  TEST_RUN(a_skew_taken_off_late_does_not_cost_the_line);
  TEST_RUN(the_border_on_function_moves_no_comparison);
  TEST_RUN(the_border_on_function_silences_the_interlace_bits);
  TEST_RUN(a_skew_does_not_move_the_video_pointer);
  TEST_RUN(a_line_that_misses_r1_borders_at_r0);
  TEST_RUN(an_r1_moved_behind_c0_earns_the_missed_border);
  TEST_RUN(a_skew_cancelled_in_time_leaves_the_border_where_it_was);
  TEST_RUN(the_border_on_function_leaves_the_r6_border_standing);
  TEST_RUN(the_border_on_function_earns_no_deferred_character);
  TEST_RUN(a_skew_written_late_can_cancel_or_double_the_border);
  TEST_RUN(the_sixty_hertz_table_makes_a_262_line_frame);
  TEST_RUN(one_vsync_per_equality_of_c4_and_r7);
  TEST_RUN(the_r1_border_holds_until_the_line_begins_again);
  TEST_RUN(the_r6_border_is_shut_for_the_whole_frame);
  TEST_RUN(an_r1_of_zero_borders_every_line);
  TEST_RUN(r1_of_zero_is_just_in_time_up_to_the_lines_c0_of_0);
  TEST_RUN(a_c0_that_overflowed_does_not_open_the_display);
  TEST_RUN(a_sync_width_of_zero_makes_no_hsync);
  return TEST_REPORT("crtc");
}
