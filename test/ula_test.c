/*
 * ula_test — the chip alone, walked round its frame.
 *
 * The numbers checked here are the published ones, counted as ula.h counts,
 * from the T-state the chip raises the interrupt on. For the 5C and 6C: a
 * line of 224 T-states, a frame of 312 lines, the interrupt held 32, and the
 * first contended T-state Rak's 14336, the FAQ's 14335 by the FAQ's count.
 * For the 7K010 timed late: a line of 228 split as libspectrum splits it, a
 * retrace of 52; 311 lines and so 70908 T-states a frame; the interrupt held
 * 36, as libspectrum holds it and minfo reads it on +2s found running late;
 * 63 lines above the picture; and the first contended T-state 14363, Rak's
 * 14362 for an early chip and one later. On both, the first fetch is three
 * T-states after the first contended, as the Sinclair Wiki's floating bus
 * table stands to its contended memory table, and the first character is
 * drawn three after that, where Rak's btime row puts it given that the
 * processor here writes on an OUT's third T-state — the one number here
 * that rests on the rest of this emulator as well as on a source. The
 * 7K010's eight lines of frame sync, and the interrupt's line counted first
 * among the 63, are taken from the 5C as ula.h declares. Nothing in this
 * file is derived from the implementation.
 */
#include <string.h>

#include "test.h"
#include "ula.h"

static ula_t ula;

/* A type's frame as ula.h's sources give it. */
typedef struct {
  ula_type type;
  int ticks_per_line;
  int lines_per_frame;
  long ticks_per_frame;
  int retrace_ticks;
  int first_display_line;
  long first_contended_tick;
  long first_fetch_tick;
  long first_drawn_tick;
  int interrupt_ticks;
} published_frame;

static const published_frame five_c = {
    .type = ULA_5C_6C_EARLY,
    .ticks_per_line = 224,
    .lines_per_frame = 312,
    .ticks_per_frame = 69888,
    .retrace_ticks = 48,
    .first_display_line = 64,
    .first_contended_tick = 14336,
    .first_fetch_tick = 14339,
    .first_drawn_tick = 14342,
    .interrupt_ticks = 32,
};

static const published_frame seven_k = {
    .type = ULA_7K010_LATE,
    .ticks_per_line = 228,
    .lines_per_frame = 311,
    .ticks_per_frame = 70908,
    .retrace_ticks = 52,
    .first_display_line = 63,
    .first_contended_tick = 14363,
    .first_fetch_tick = 14366,
    .first_drawn_tick = 14369,
    .interrupt_ticks = 36,
};

static const published_frame *const both[] = {&five_c, &seven_k};
#define BOTH (sizeof both / sizeof both[0])

static int display_column(const published_frame *published) {
  return published->retrace_ticks + ULA_LEFT_BORDER_TICKS;
}

/* The T-state the beam stands on a raster line and column at, counting from
   where the sources put the first character drawn. */
static long tick_at(const published_frame *published, int line, int column) {
  long from_first = (long)(line - published->first_display_line) * published->ticks_per_line +
                    column - display_column(published);
  return (published->first_drawn_tick + from_first + published->ticks_per_frame) %
         published->ticks_per_frame;
}

/* The screen the chip reads, which the machine would hand it. */
static uint8_t screen[0x1B00];

/* Run the chip on for `count` T-states, handing it what it fetches and
   keeping what it draws, two samples a T-state. */
static void run_on(int count, uint8_t *samples) {
  for (int step = 0; step < count; step++) {
    uint16_t address = 0;
    if (ula_fetch(&ula, &address)) {
      ula_latch(&ula, screen[address - 0x4000]);
    }
    ula_video(&ula, &samples[(size_t)step * ULA_SAMPLES_PER_TICK]);
    ula_tick(&ula);
  }
}

static void run_from(long tick, int count, uint8_t *samples) {
  ula_seek(&ula, (uint32_t)tick);
  run_on(count, samples);
}

/* What the chip draws at a raster position, once it has run the slot
   before it. */
static void drawn_at(const published_frame *published, int line, int column, uint8_t out[2]) {
  enum { RUN_IN = 16 };
  uint8_t samples[(RUN_IN + 1) * ULA_SAMPLES_PER_TICK];
  run_from(tick_at(published, line, column) - RUN_IN, RUN_IN + 1, samples);
  out[0] = samples[(size_t)RUN_IN * ULA_SAMPLES_PER_TICK];
  out[1] = samples[(size_t)RUN_IN * ULA_SAMPLES_PER_TICK + 1];
}

static void frame_is_as_published(const published_frame *published) {
  ula_init(&ula, published->type);
  TEST_EQUAL(ula.frame.ticks_per_line, published->ticks_per_line);
  TEST_EQUAL(ula.frame.lines_per_frame, published->lines_per_frame);
  TEST_EQUAL(ula.frame.ticks_per_frame, published->ticks_per_frame);
  TEST_EQUAL(ula.frame.retrace_ticks, published->retrace_ticks);
  TEST_EQUAL(ula.frame.retrace_ticks + ULA_LEFT_BORDER_TICKS + ULA_DISPLAY_TICKS +
                 ULA_RIGHT_BORDER_TICKS,
             published->ticks_per_line);
  for (long tick = 0; tick < published->ticks_per_frame - 1; tick++) {
    ula_tick(&ula);
  }
  TEST_EQUAL(ula.frame_count, 0);
  ula_tick(&ula);
  TEST_EQUAL(ula.frame_count, 1);
  TEST_EQUAL(ula.frame_tick, 0);
}

static void the_frame_is_312_lines_of_224(void) {
  TEST_EQUAL(ULA_LEFT_BORDER_TICKS, 24);
  TEST_EQUAL(ULA_DISPLAY_TICKS, 128);
  TEST_EQUAL(ULA_RIGHT_BORDER_TICKS, 24);
  frame_is_as_published(&five_c);
}

static void a_7k010s_frame_is_311_lines_of_228(void) { frame_is_as_published(&seven_k); }

/* line and column are derived from frame_tick, so ticking and seeking must
   never disagree about where the beam is. */
static void ticking_and_seeking_agree_all_the_way_round_a_frame(void) {
  for (size_t index = 0; index < BOTH; index++) {
    ula_t ticked;
    ula_init(&ticked, both[index]->type);
    ula_init(&ula, both[index]->type);
    for (uint32_t tick = 0; tick < ticked.frame.ticks_per_frame; tick++) {
      ula_seek(&ula, tick);
      if (ticked.frame_tick != ula.frame_tick || ticked.line != ula.line ||
          ticked.column != ula.column) {
        TEST_FAIL("type %zu, at T-state %u ticking gives %u/%u/%u, seeking %u/%u/%u", index, tick,
                  ticked.frame_tick, ticked.line, ticked.column, ula.frame_tick, ula.line,
                  ula.column);
        return;
      }
      ula_tick(&ticked);
    }
    TEST_EQUAL(ticked.frame_tick, 0);
    TEST_EQUAL(ticked.line, 0);
    TEST_EQUAL(ticked.frame_count, 1);

    /* A T-state given from outside can be anything, and is one of a frame. */
    ula_t wrapped;
    ula_init(&wrapped, both[index]->type);
    ula_seek(&wrapped, UINT32_MAX);
    ula_seek(&ula, (uint32_t)(UINT32_MAX % both[index]->ticks_per_frame));
    TEST_EQUAL(wrapped.frame_tick, ula.frame_tick);
    TEST_EQUAL(wrapped.line, ula.line);
    TEST_EQUAL(wrapped.column, ula.column);
  }
}

static void interrupt_is_held(const published_frame *published) {
  ula_init(&ula, published->type);
  TEST_CHECK(ula_interrupt(&ula));
  ula_seek(&ula, (uint32_t)published->interrupt_ticks - 1);
  TEST_CHECK(ula_interrupt(&ula));
  ula_seek(&ula, (uint32_t)published->interrupt_ticks);
  TEST_CHECK(!ula_interrupt(&ula));
  ula_seek(&ula, (uint32_t)published->ticks_per_frame - 1);
  TEST_CHECK(!ula_interrupt(&ula));
}

static void the_interrupt_opens_the_frame_and_is_held_32_tstates(void) {
  interrupt_is_held(&five_c);
}

static void a_late_7k010s_interrupt_is_held_36_tstates(void) { interrupt_is_held(&seven_k); }

/* "the interrupt must be generated not at the start of a line, but at the
   same offset into a line as the first display byte and 64 scanlines
   earlier" — on the chip's counter, whose first slot begins at the first
   contended T-state. T-state 0 is inside the vertical sync. */
static void the_interrupt_stands_64_lines_above_the_first_slot(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  TEST_EQUAL(ula.line, 0);
  TEST_CHECK(ula_csync(&ula));
  uint16_t column_of_interrupt = ula.column;
  ula_seek(&ula, 14336);
  TEST_EQUAL(ula.line, 64);
  TEST_EQUAL(ula.column, column_of_interrupt);
  TEST_CHECK(!ula_csync(&ula));
}

/* Farrow's "63 above picture", the interrupt's own line taken as the first
   of them, as on a 5C. 63 lines of 228 are 14364, so the first slot opens a
   T-state short of where the interrupt stands. */
static void a_late_7k010s_first_slot_is_63_lines_down(void) {
  ula_init(&ula, ULA_7K010_LATE);
  TEST_EQUAL(ula.line, 0);
  TEST_CHECK(ula_csync(&ula));
  uint16_t column_of_interrupt = ula.column;
  ula_seek(&ula, 14363);
  TEST_EQUAL(ula.line, 63);
  TEST_EQUAL(ula.column + 1, column_of_interrupt);
}

/* The address fetched on a T-state, or 0 for none. */
static uint16_t fetched_at(long tick) {
  ula_seek(&ula, (uint32_t)tick);
  uint16_t address = 0;
  return ula_fetch(&ula, &address) ? address : 0;
}

/* Bitmap, attribute, bitmap, attribute on four T-states of each eight, then
   four idle, across 128 of a line and none of the rest. */
static void fetches_follow_the_slots(const published_frame *published) {
  ula_init(&ula, published->type);
  long first = published->first_fetch_tick;
  const uint16_t want[16] = {0x4000, 0x5800, 0x4001, 0x5801, 0, 0, 0, 0,
                             0x4002, 0x5802, 0x4003, 0x5803, 0, 0, 0, 0};
  for (int step = 0; step < 16; step++) {
    TEST_EQUAL(fetched_at(first + step), want[step]);
  }
  TEST_EQUAL(fetched_at(first - 1), 0);
  TEST_EQUAL(fetched_at(first + 120), 0x401E); /* the line's last slot */
  TEST_EQUAL(fetched_at(first + 123), 0x581F);
  TEST_EQUAL(fetched_at(first + 124), 0);
  TEST_EQUAL(fetched_at(first + published->ticks_per_line), 0x4100); /* the next pixel row */
  long last_line = first + 191L * published->ticks_per_line;
  TEST_EQUAL(fetched_at(last_line + 122), 0x57FF); /* the last byte of the screen */
  TEST_EQUAL(fetched_at(last_line + 123), 0x5AFF);
  TEST_EQUAL(fetched_at(last_line + published->ticks_per_line), 0);
}

static void a_5c_fetches_from_14339_two_characters_a_slot(void) {
  fetches_follow_the_slots(&five_c);
}

static void a_late_7k010_fetches_from_14366(void) { fetches_follow_the_slots(&seven_k); }

/* Every byte of the display file once a frame, and an attribute once for
   each of its character's eight rows. */
static void every_byte_of_the_screen_is_fetched_once_a_frame(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    static uint8_t times[0x1B00];
    memset(times, 0, sizeof times);
    long first = -1;
    for (long tick = 0; tick < published->ticks_per_frame; tick++) {
      uint16_t address = fetched_at(tick);
      if (address != 0) {
        times[address - 0x4000]++;
        first = first < 0 ? tick : first;
      }
    }
    TEST_EQUAL(first, published->first_fetch_tick);
    for (size_t offset = 0; offset < sizeof times; offset++) {
      if (times[offset] != (offset < 0x1800 ? 1 : 8)) {
        TEST_FAIL("type %zu fetched &%04zX %d times a frame", index, 0x4000 + offset,
                  times[offset]);
        return;
      }
    }
  }
}

static void the_display_file_scatters_a_row_across_three_thirds(void) {
  TEST_EQUAL(ula_display_address(0, 0), 0x4000);
  TEST_EQUAL(ula_display_address(1, 0), 0x4100);  /* the next pixel row */
  TEST_EQUAL(ula_display_address(8, 0), 0x4020);  /* the next character row */
  TEST_EQUAL(ula_display_address(64, 0), 0x4800); /* the second third */
  TEST_EQUAL(ula_display_address(128, 0), 0x5000);
  TEST_EQUAL(ula_display_address(0, 31), 0x401F);
  TEST_EQUAL(ula_display_address(191, 31), 0x57FF); /* the last byte of it */
}

static void attributes_are_one_byte_to_a_character(void) {
  TEST_EQUAL(ula_attribute_address(0, 0), 0x5800);
  TEST_EQUAL(ula_attribute_address(7, 0), 0x5800); /* all eight pixel rows share it */
  TEST_EQUAL(ula_attribute_address(8, 0), 0x5820);
  TEST_EQUAL(ula_attribute_address(191, 31), 0x5AFF);
}

/* The first character is drawn from the first T-state at the picture's left
   edge, a byte most significant pixel first, and the second four T-states
   on, each in its own attribute. */
static void characters_are_drawn_from_the_slots_seventh_tstate(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    ula_write(&ula, 0x03);
    memset(screen, 0, sizeof screen);
    screen[0x0000] = 0xA0; /* 1010 0000: ink, paper, ink, paper, then paper */
    screen[0x1800] = 0x07; /* ink 7 on paper 0 */
    screen[0x0001] = 0x0F;
    screen[0x1801] = 0x0A; /* ink 2 on paper 1 */
    /* From the character's worth of border before the first, which begins
       ahead of the first fetch. */
    uint8_t got[12 * ULA_SAMPLES_PER_TICK];
    run_from(published->first_drawn_tick - 4, 12, got);
    const uint8_t want[24] = {3, 3, 3, 3, 3, 3, 3, 3, 7, 0, 7, 0,
                              0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2};
    for (int sample = 0; sample < 24; sample++) {
      TEST_EQUAL(got[sample], want[sample]);
    }
    ula_seek(&ula, (uint32_t)published->first_drawn_tick);
    TEST_EQUAL(ula.line, published->first_display_line);
    TEST_EQUAL(ula.column, display_column(published));
  }
}

/* What is drawn is what was fetched: a write to the screen after the chip
   has read a byte reaches the beam a frame later. */
static void a_byte_is_drawn_as_it_was_fetched(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    memset(screen, 0, sizeof screen);
    screen[0x0001] = 0xFF;
    screen[0x1801] = 0x07;
    uint8_t got[8 * ULA_SAMPLES_PER_TICK];
    /* Through the fetch of the second character's attribute, which ends
       its slot's fetches. */
    run_from(published->first_fetch_tick, 4, got);
    screen[0x0001] = 0x00;
    screen[0x1801] = 0x10; /* paper 2, which would draw as 2 */
    run_on(8, got);
    /* The second character is drawn on the fourth T-state of these and the
       three after it. */
    for (int sample = 6; sample < 14; sample++) {
      TEST_EQUAL(got[sample], 7);
    }
  }
}

static void bright_lifts_both_ink_and_paper(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  memset(screen, 0, sizeof screen);
  screen[0x0000] = 0x80;
  screen[0x1800] = 0x41;
  uint8_t got[2];
  drawn_at(&five_c, 64, 72, got);
  TEST_EQUAL(got[0], 0x09); /* ink 1, bright */
  TEST_EQUAL(got[1], 0x08); /* paper 0, bright */
}

static void flash_swaps_ink_and_paper_every_sixteen_frames(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  memset(screen, 0, sizeof screen);
  screen[0x0000] = 0x80;
  screen[0x1800] = 0x87; /* flashing, ink 7 on paper 0 */
  uint8_t got[2];
  drawn_at(&five_c, 64, 72, got);
  TEST_EQUAL(got[0], 7);
  ula.frame_count = 15;
  drawn_at(&five_c, 64, 72, got);
  TEST_EQUAL(got[0], 7);
  ula.frame_count = 16;
  drawn_at(&five_c, 64, 72, got);
  TEST_EQUAL(got[0], 0); /* swapped */
  ula.frame_count = 32;
  drawn_at(&five_c, 64, 72, got);
  TEST_EQUAL(got[0], 7); /* and back, a 32-frame cycle */
}

static void the_border_fills_the_line_either_side_of_the_picture(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    ula_write(&ula, 0x05);
    memset(screen, 0xFF, sizeof screen);
    for (size_t offset = 0x1800; offset < sizeof screen; offset++) {
      screen[offset] = 0x06; /* ink 6 */
    }
    uint8_t got[2];
    int line = published->first_display_line;
    int left = display_column(published);
    drawn_at(published, line, published->retrace_ticks, got);
    TEST_EQUAL(got[0], 5); /* the left border */
    drawn_at(published, line, published->ticks_per_line - 1, got);
    TEST_EQUAL(got[0], 5); /* and the right */
    drawn_at(published, line - 1, left, got);
    TEST_EQUAL(got[0], 5); /* the line above the picture is all border */

    /* The picture's first and last columns and lines are picture, and the
       next of each is border. */
    int last_column = left + ULA_DISPLAY_TICKS - 1;
    int last_line = line + ULA_DISPLAY_LINES - 1;
    drawn_at(published, line, left - 1, got);
    TEST_EQUAL(got[1], 5);
    drawn_at(published, line, left, got);
    TEST_EQUAL(got[0], 6);
    drawn_at(published, line, last_column, got);
    TEST_EQUAL(got[1], 6);
    drawn_at(published, line, last_column + 1, got);
    TEST_EQUAL(got[0], 5);
    drawn_at(published, last_line, left, got);
    TEST_EQUAL(got[0], 6);
    drawn_at(published, last_line + 1, left, got);
    TEST_EQUAL(got[0], 5);
  }
}

/* The border colour is taken every fourth T-state, in step with the
   characters, so a new one shows only from the next of those. */
static void a_new_border_shows_from_the_next_character(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    ula_write(&ula, 0x01);
    long above = tick_at(published, published->first_display_line - 1, display_column(published));
    uint8_t got[8 * ULA_SAMPLES_PER_TICK];
    run_from(above - 8, 5, got); /* into the character before the edge */
    ula_write(&ula, 0x02);
    for (int step = 0; step < 7; step++) {
      ula_video(&ula, &got[(size_t)step * ULA_SAMPLES_PER_TICK]);
      ula_tick(&ula);
    }
    /* Three more T-states of the old colour, and the new from the edge. */
    const uint8_t want[14] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2};
    for (int sample = 0; sample < 14; sample++) {
      TEST_EQUAL(got[sample], want[sample]);
    }
  }
}

static void the_beam_is_blanked_through_a_sync(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    ula_write(&ula, 0x05);
    uint8_t got[2];
    drawn_at(published, published->first_display_line, published->retrace_ticks - 1, got);
    TEST_EQUAL(got[0], 0);
    TEST_EQUAL(got[1], 0);
  }
}

static void the_frame_sync_holds_across_eight_whole_lines(void) {
  TEST_EQUAL(ULA_VSYNC_LINES, 8);
  TEST_EQUAL(ULA_DISPLAY_LINES, 192);
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    for (int line = 0; line < 8; line++) {
      ula_seek(&ula, (uint32_t)tick_at(published, line, 0));
      TEST_CHECK(ula_csync(&ula));
      ula_seek(&ula, (uint32_t)tick_at(published, line, published->ticks_per_line - 1));
      TEST_CHECK(ula_csync(&ula));
    }
    /* The line after it syncs only through its retrace, like any other. */
    ula_seek(&ula, (uint32_t)tick_at(published, 8, published->retrace_ticks - 1));
    TEST_CHECK(ula_csync(&ula));
    ula_seek(&ula, (uint32_t)tick_at(published, 8, published->retrace_ticks));
    TEST_CHECK(!ula_csync(&ula));
  }
}

static uint8_t contention_at(long tick) { return ula_contention(&ula, (uint32_t)tick); }

static void first_contended_tstate_is_as_published(const published_frame *published) {
  ula_init(&ula, published->type);
  long first = -1;
  for (long tick = 0; tick < published->ticks_per_frame; tick++) {
    if (contention_at(tick) != 0) {
      first = tick;
      break;
    }
  }
  TEST_EQUAL(first, published->first_contended_tick);
  TEST_EQUAL(contention_at(published->first_contended_tick - 1), 0);
}

static void the_first_contended_tstate_of_a_frame_is_14336(void) {
  first_contended_tstate_is_as_published(&five_c);
}

static void a_late_7k010s_first_contended_tstate_is_14363(void) {
  first_contended_tstate_is_as_published(&seven_k);
}

/* The published table, transcribed: two slots of it at the head of the
   frame, then the line's last and the next line's first. A 5C's line leaves
   96 T-states with no delay, a 7K010's 100. */
static void slot_owes_six_falling_to_none(const published_frame *published) {
  ula_init(&ula, published->type);
  long first = published->first_contended_tick;
  long line = published->ticks_per_line;
  const uint8_t want[16] = {6, 5, 4, 3, 2, 1, 0, 0, 6, 5, 4, 3, 2, 1, 0, 0};
  for (int tick = 0; tick < 16; tick++) {
    TEST_EQUAL(contention_at(first + tick), want[tick]);
  }
  TEST_EQUAL(contention_at(first + 127), 0); /* the line's last contended tstate */
  TEST_EQUAL(contention_at(first + 128), 0); /* then none owed to the line's end */
  TEST_EQUAL(contention_at(first + line - 1), 0);
  TEST_EQUAL(contention_at(first + line), 6); /* and the pattern starts again */
}

static void the_slot_owes_six_falling_to_none(void) {
  slot_owes_six_falling_to_none(&five_c);
  TEST_EQUAL(contention_at(14463), 0);
  TEST_EQUAL(contention_at(14464), 0);
  TEST_EQUAL(contention_at(14559), 0);
  TEST_EQUAL(contention_at(14560), 6);
}

static void a_7k010s_slot_repeats_every_228(void) {
  slot_owes_six_falling_to_none(&seven_k);
  TEST_EQUAL(contention_at(14590), 0);
  TEST_EQUAL(contention_at(14591), 6);
}

static void picture_is_the_only_part_that_owes(const published_frame *published) {
  ula_init(&ula, published->type);
  long owing = 0;
  for (long tick = 0; tick < published->ticks_per_frame; tick++) {
    if (contention_at(tick) != 0) {
      owing++;
    }
  }
  /* Six of every eight, for 128 tstates of each of 192 lines. */
  TEST_EQUAL(owing, 192 * 128 * 6 / 8);
  long last_line = published->first_contended_tick + 191L * published->ticks_per_line;
  TEST_EQUAL(contention_at(0), 0); /* the vertical sync */
  TEST_EQUAL(contention_at(published->first_contended_tick - published->ticks_per_line),
             0);                                 /* the border above the picture */
  TEST_EQUAL(contention_at(last_line), 6);       /* the last line that owes anything */
  TEST_EQUAL(contention_at(last_line + 128), 0); /* and nothing after it */
  TEST_EQUAL(contention_at(published->ticks_per_frame - 1), 0); /* to the frame's end */
}

static void the_picture_is_the_only_part_of_the_frame_that_owes(void) {
  picture_is_the_only_part_that_owes(&five_c);
  TEST_EQUAL(contention_at(57120), 6);
  TEST_EQUAL(contention_at(57248), 0);
}

static void a_7k010s_picture_is_the_only_part_of_its_frame_that_owes(void) {
  picture_is_the_only_part_that_owes(&seven_k);
  TEST_EQUAL(contention_at(57911), 6);
  TEST_EQUAL(contention_at(58039), 0);
}

/* A caller walking a port's four tstates adds delays to a position and can
   run off the end of the frame doing it. */
static void a_tstate_past_the_frame_is_one_in_the_next(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    TEST_EQUAL(contention_at(published->ticks_per_frame + published->first_contended_tick), 6);
    TEST_EQUAL(contention_at(published->ticks_per_frame + published->first_contended_tick + 1), 5);
    TEST_EQUAL(contention_at(published->ticks_per_frame), 0);
  }
}

static void port_fe_holds_the_border_the_microphone_and_the_speaker(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  ula_write(&ula, 0x00);
  TEST_EQUAL(ula.border, 0);
  TEST_CHECK(!ula.microphone);
  TEST_CHECK(!ula.speaker);
  ula_write(&ula, 0xFF);
  TEST_EQUAL(ula.border, 7); /* the top three bits reach nothing */
  TEST_CHECK(ula.microphone);
  TEST_CHECK(ula.speaker);
  ula_write(&ula, 0x12);
  TEST_EQUAL(ula.border, 2);
  TEST_CHECK(!ula.microphone);
  TEST_CHECK(ula.speaker);
}

int main(void) {
  TEST_RUN(the_frame_is_312_lines_of_224);
  TEST_RUN(a_7k010s_frame_is_311_lines_of_228);
  TEST_RUN(ticking_and_seeking_agree_all_the_way_round_a_frame);
  TEST_RUN(the_interrupt_opens_the_frame_and_is_held_32_tstates);
  TEST_RUN(a_late_7k010s_interrupt_is_held_36_tstates);
  TEST_RUN(the_interrupt_stands_64_lines_above_the_first_slot);
  TEST_RUN(a_late_7k010s_first_slot_is_63_lines_down);
  TEST_RUN(a_5c_fetches_from_14339_two_characters_a_slot);
  TEST_RUN(a_late_7k010_fetches_from_14366);
  TEST_RUN(every_byte_of_the_screen_is_fetched_once_a_frame);
  TEST_RUN(the_display_file_scatters_a_row_across_three_thirds);
  TEST_RUN(attributes_are_one_byte_to_a_character);
  TEST_RUN(characters_are_drawn_from_the_slots_seventh_tstate);
  TEST_RUN(a_byte_is_drawn_as_it_was_fetched);
  TEST_RUN(bright_lifts_both_ink_and_paper);
  TEST_RUN(flash_swaps_ink_and_paper_every_sixteen_frames);
  TEST_RUN(the_border_fills_the_line_either_side_of_the_picture);
  TEST_RUN(a_new_border_shows_from_the_next_character);
  TEST_RUN(the_beam_is_blanked_through_a_sync);
  TEST_RUN(the_frame_sync_holds_across_eight_whole_lines);
  TEST_RUN(the_first_contended_tstate_of_a_frame_is_14336);
  TEST_RUN(a_late_7k010s_first_contended_tstate_is_14363);
  TEST_RUN(the_slot_owes_six_falling_to_none);
  TEST_RUN(a_7k010s_slot_repeats_every_228);
  TEST_RUN(the_picture_is_the_only_part_of_the_frame_that_owes);
  TEST_RUN(a_7k010s_picture_is_the_only_part_of_its_frame_that_owes);
  TEST_RUN(a_tstate_past_the_frame_is_one_in_the_next);
  TEST_RUN(port_fe_holds_the_border_the_microphone_and_the_speaker);
  return TEST_REPORT("ula");
}
