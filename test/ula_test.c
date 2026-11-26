/*
 * ula_test — the chip alone, walked round its frame.
 *
 * The numbers checked here are the published ones. For the 5C and 6C: a
 * line of 224 T-states, a frame of 312 lines, the interrupt held 32, the
 * first displayed byte 14336 after it, and the first contended T-state
 * 14335. For the 7K010 timed late: a line of 228 split as libspectrum
 * splits it, a retrace of 52; 311 lines and so 70908 T-states a frame; the
 * interrupt held 36, as libspectrum holds it and minfo reads it on +2s
 * found running late; 63 lines above the picture; and the FAQ's 14361 for
 * the first contended T-state and libspectrum's 14362 for the first
 * displayed byte, each one later. Its eight lines of frame sync, and the
 * interrupt's line counted first among the 63, are taken from the 5C as
 * ula.h declares. Nothing in this file is derived from the implementation.
 */
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
  long first_display_tick;
  long first_contended_tick;
  int interrupt_ticks;
} published_frame;

static const published_frame five_c = {
    .type = ULA_5C_6C_EARLY,
    .ticks_per_line = 224,
    .lines_per_frame = 312,
    .ticks_per_frame = 69888,
    .retrace_ticks = 48,
    .first_display_line = 64,
    .first_display_tick = 14336,
    .first_contended_tick = 14335,
    .interrupt_ticks = 32,
};

static const published_frame seven_k = {
    .type = ULA_7K010_LATE,
    .ticks_per_line = 228,
    .lines_per_frame = 311,
    .ticks_per_frame = 70908,
    .retrace_ticks = 52,
    .first_display_line = 63,
    .first_display_tick = 14363,
    .first_contended_tick = 14362,
    .interrupt_ticks = 36,
};

static const published_frame *const both[] = {&five_c, &seven_k};
#define BOTH (sizeof both / sizeof both[0])

static int display_column(const published_frame *published) {
  return published->retrace_ticks + ULA_LEFT_BORDER_TICKS;
}

/* Stand the beam on a raster line and column, counting from where the
   sources put the picture's first byte. */
static void put_beam_on(const published_frame *published, int line, int column) {
  long from_first = (long)(line - published->first_display_line) * published->ticks_per_line +
                    column - display_column(published);
  long tick = (published->first_display_tick + from_first + published->ticks_per_frame) %
              published->ticks_per_frame;
  ula_seek(&ula, (uint32_t)tick);
}

static void put_beam(int line, int column) { put_beam_on(&five_c, line, column); }

static void samples_at(int line, int column, uint8_t display, uint8_t attribute, uint8_t out[2]) {
  put_beam(line, column);
  ula_video(&ula, display, attribute, out);
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
   earlier" — which puts T-state 0 inside the vertical sync, directly above
   the first pixel the chip ever paints. */
static void the_interrupt_stands_directly_above_the_first_pixel(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  TEST_EQUAL(ula.line, 0);
  TEST_CHECK(ula_csync(&ula)); /* line 0 is vertical sync */
  uint16_t column_of_interrupt = ula.column;
  ula_seek(&ula, 14336);
  TEST_EQUAL(ula.line, 64);
  TEST_EQUAL(ula.column, column_of_interrupt);
  TEST_CHECK(!ula_csync(&ula));
}

static void first_displayed_byte_is_as_published(const published_frame *published) {
  ula_init(&ula, published->type);
  uint16_t display = 0, attribute = 0;
  long first = -1;
  for (long tick = 0; tick < published->ticks_per_frame; tick++) {
    ula_seek(&ula, (uint32_t)tick);
    if (ula_fetch(&ula, &display, &attribute)) {
      first = tick;
      break;
    }
  }
  TEST_EQUAL(first, published->first_display_tick);
  TEST_EQUAL(ula.line, published->first_display_line);
  TEST_EQUAL(ula.column, display_column(published));
  TEST_EQUAL(display, 0x4000);
  TEST_EQUAL(attribute, 0x5800);
}

static void the_first_displayed_byte_is_14336_tstates_after_the_interrupt(void) {
  first_displayed_byte_is_as_published(&five_c);
}

/* Farrow's "63 above picture", the interrupt's own line taken as the first
   of them, as on a 5C. */
static void a_late_7k010_displays_its_first_byte_63_lines_down_at_14363(void) {
  first_displayed_byte_is_as_published(&seven_k);
  ula_init(&ula, ULA_7K010_LATE);
  TEST_EQUAL(ula.line, 0);
  TEST_CHECK(ula_csync(&ula));
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

static void a_byte_paints_eight_pixels_most_significant_first(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    uint8_t got[8];
    for (int tick = 0; tick < 4; tick++) {
      put_beam_on(published, published->first_display_line, display_column(published) + tick);
      ula_video(&ula, 0xA0, 0x07, &got[(size_t)tick * 2]);
    }
    /* 0xA0 is 1010 0000: ink, paper, ink, paper, then four of paper. */
    const uint8_t want[8] = {7, 0, 7, 0, 0, 0, 0, 0};
    for (int pixel = 0; pixel < 8; pixel++) {
      TEST_EQUAL(got[pixel], want[pixel]);
    }
  }
}

static void bright_lifts_both_ink_and_paper(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  uint8_t got[2];
  samples_at(64, 72, 0x80, 0x41, got);
  TEST_EQUAL(got[0], 0x09); /* ink 1, bright */
  TEST_EQUAL(got[1], 0x08); /* paper 0, bright */
}

static void flash_swaps_ink_and_paper_every_sixteen_frames(void) {
  ula_init(&ula, ULA_5C_6C_EARLY);
  uint8_t got[2];
  int line = 64;
  int column = 72;

  samples_at(line, column, 0x80, 0x87, got); /* flashing, ink 7 on paper 0 */
  TEST_EQUAL(got[0], 7);
  ula.frame_count = 15;
  samples_at(line, column, 0x80, 0x87, got);
  TEST_EQUAL(got[0], 7);
  ula.frame_count = 16;
  samples_at(line, column, 0x80, 0x87, got);
  TEST_EQUAL(got[0], 0); /* swapped */
  ula.frame_count = 32;
  samples_at(line, column, 0x80, 0x87, got);
  TEST_EQUAL(got[0], 7); /* and back, a 32-frame cycle */
}

static void the_border_fills_the_line_either_side_of_the_picture(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    ula_write(&ula, 0x05);
    uint8_t got[2];
    int line = published->first_display_line;
    put_beam_on(published, line, published->retrace_ticks);
    ula_video(&ula, 0xFF, 0xFF, got);
    TEST_EQUAL(got[0], 5); /* the left border */
    put_beam_on(published, line, published->ticks_per_line - 1);
    ula_video(&ula, 0xFF, 0xFF, got);
    TEST_EQUAL(got[0], 5); /* and the right */
    put_beam_on(published, line - 1, display_column(published));
    ula_video(&ula, 0xFF, 0xFF, got);
    TEST_EQUAL(got[0], 5); /* the line above the picture is all border */

    /* The picture's last column and line are picture, ink 6 here, and the
       next of each is border. */
    int last_column = display_column(published) + ULA_DISPLAY_TICKS - 1;
    int last_line = published->first_display_line + ULA_DISPLAY_LINES - 1;
    put_beam_on(published, line, last_column);
    ula_video(&ula, 0xFF, 0x06, got);
    TEST_EQUAL(got[1], 6);
    put_beam_on(published, line, last_column + 1);
    ula_video(&ula, 0xFF, 0x06, got);
    TEST_EQUAL(got[0], 5);
    put_beam_on(published, last_line, display_column(published));
    ula_video(&ula, 0xFF, 0x06, got);
    TEST_EQUAL(got[0], 6);
    put_beam_on(published, last_line + 1, display_column(published));
    ula_video(&ula, 0xFF, 0x06, got);
    TEST_EQUAL(got[0], 5);
  }
}

static void the_beam_is_blanked_through_a_sync(void) {
  for (size_t index = 0; index < BOTH; index++) {
    const published_frame *published = both[index];
    ula_init(&ula, published->type);
    ula_write(&ula, 0x05);
    uint8_t got[2];
    put_beam_on(published, published->first_display_line, published->retrace_ticks - 1);
    ula_video(&ula, 0xFF, 0xFF, got);
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
      put_beam_on(published, line, 0);
      TEST_CHECK(ula_csync(&ula));
      put_beam_on(published, line, published->ticks_per_line - 1);
      TEST_CHECK(ula_csync(&ula));
    }
    /* The line after it syncs only through its retrace, like any other. */
    put_beam_on(published, 8, published->retrace_ticks - 1);
    TEST_CHECK(ula_csync(&ula));
    put_beam_on(published, 8, published->retrace_ticks);
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

static void the_first_contended_tstate_of_a_frame_is_14335(void) {
  first_contended_tstate_is_as_published(&five_c);
}

static void a_late_7k010s_first_contended_tstate_is_14362(void) {
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
  TEST_EQUAL(contention_at(14462), 0);
  TEST_EQUAL(contention_at(14463), 0);
  TEST_EQUAL(contention_at(14558), 0);
  TEST_EQUAL(contention_at(14559), 6);
}

static void a_7k010s_slot_repeats_every_228(void) {
  slot_owes_six_falling_to_none(&seven_k);
  TEST_EQUAL(contention_at(14589), 0);
  TEST_EQUAL(contention_at(14590), 6);
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
  TEST_EQUAL(contention_at(57119), 6);
  TEST_EQUAL(contention_at(57247), 0);
}

static void a_7k010s_picture_is_the_only_part_of_its_frame_that_owes(void) {
  picture_is_the_only_part_that_owes(&seven_k);
  TEST_EQUAL(contention_at(57910), 6);
  TEST_EQUAL(contention_at(58038), 0);
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
  TEST_RUN(the_interrupt_stands_directly_above_the_first_pixel);
  TEST_RUN(the_first_displayed_byte_is_14336_tstates_after_the_interrupt);
  TEST_RUN(a_late_7k010_displays_its_first_byte_63_lines_down_at_14363);
  TEST_RUN(the_display_file_scatters_a_row_across_three_thirds);
  TEST_RUN(attributes_are_one_byte_to_a_character);
  TEST_RUN(a_byte_paints_eight_pixels_most_significant_first);
  TEST_RUN(bright_lifts_both_ink_and_paper);
  TEST_RUN(flash_swaps_ink_and_paper_every_sixteen_frames);
  TEST_RUN(the_border_fills_the_line_either_side_of_the_picture);
  TEST_RUN(the_beam_is_blanked_through_a_sync);
  TEST_RUN(the_frame_sync_holds_across_eight_whole_lines);
  TEST_RUN(the_first_contended_tstate_of_a_frame_is_14335);
  TEST_RUN(a_late_7k010s_first_contended_tstate_is_14362);
  TEST_RUN(the_slot_owes_six_falling_to_none);
  TEST_RUN(a_7k010s_slot_repeats_every_228);
  TEST_RUN(the_picture_is_the_only_part_of_the_frame_that_owes);
  TEST_RUN(a_7k010s_picture_is_the_only_part_of_its_frame_that_owes);
  TEST_RUN(a_tstate_past_the_frame_is_one_in_the_next);
  TEST_RUN(port_fe_holds_the_border_the_microphone_and_the_speaker);
  return TEST_REPORT("ula");
}
