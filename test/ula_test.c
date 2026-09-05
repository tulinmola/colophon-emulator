/*
 * ula_test — the chip alone, walked round its frame.
 *
 * The numbers checked here are the published ones: a line of 224 T-states,
 * a frame of 312 lines, the interrupt held 32, the first displayed byte
 * 14336 after it, and the first contended T-state 14335. Nothing in this
 * file is derived from the implementation.
 */
#include "test.h"
#include "ula.h"

static ula_t ula;

/* Stand the beam on a raster line and column. A line begins at its retrace
   and the frame at the interrupt, which falls a retrace and a left border
   into line zero. */
static void put_beam(int line, int column) {
  int origin = ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS;
  ula_seek(&ula, (uint32_t)((line * ULA_TICKS_PER_LINE + column - origin + ULA_TICKS_PER_FRAME) %
                            ULA_TICKS_PER_FRAME));
}

static void samples_at(int line, int column, uint8_t display, uint8_t attribute, uint8_t out[2]) {
  put_beam(line, column);
  ula_video(&ula, display, attribute, out);
}

static void the_frame_is_312_lines_of_224(void) {
  ula_init(&ula);
  TEST_EQUAL(ULA_TICKS_PER_LINE, 224);
  TEST_EQUAL(ULA_LINES_PER_FRAME, 312);
  TEST_EQUAL(ULA_TICKS_PER_FRAME, 69888);
  TEST_EQUAL(ULA_LEFT_BORDER_TICKS, 24);
  TEST_EQUAL(ULA_DISPLAY_TICKS, 128);
  TEST_EQUAL(ULA_RIGHT_BORDER_TICKS, 24);
  TEST_EQUAL(ULA_RETRACE_TICKS, 48);
  for (long tick = 0; tick < ULA_TICKS_PER_FRAME - 1; tick++) {
    ula_tick(&ula);
  }
  TEST_EQUAL(ula.frame_count, 0);
  ula_tick(&ula);
  TEST_EQUAL(ula.frame_count, 1);
  TEST_EQUAL(ula.frame_tick, 0);
}

/* line and column are derived from frame_tick, so ticking and seeking must
   never disagree about where the beam is. */
static void ticking_and_seeking_agree_all_the_way_round_a_frame(void) {
  ula_t ticked;
  ula_init(&ticked);
  for (uint32_t tick = 0; tick < ULA_TICKS_PER_FRAME; tick++) {
    ula_seek(&ula, tick);
    if (ticked.frame_tick != ula.frame_tick || ticked.line != ula.line ||
        ticked.column != ula.column) {
      TEST_FAIL("at T-state %u ticking gives %u/%u/%u, seeking %u/%u/%u", tick, ticked.frame_tick,
                ticked.line, ticked.column, ula.frame_tick, ula.line, ula.column);
      return;
    }
    ula_tick(&ticked);
  }
  TEST_EQUAL(ticked.frame_tick, 0);
  TEST_EQUAL(ticked.line, 0);
  TEST_EQUAL(ticked.frame_count, 1);
}

static void the_interrupt_opens_the_frame_and_is_held_32_tstates(void) {
  ula_init(&ula);
  TEST_CHECK(ula_interrupt(&ula));
  ula_seek(&ula, 31);
  TEST_CHECK(ula_interrupt(&ula));
  ula_seek(&ula, 32);
  TEST_CHECK(!ula_interrupt(&ula));
  ula_seek(&ula, ULA_TICKS_PER_FRAME - 1);
  TEST_CHECK(!ula_interrupt(&ula));
}

/* "the interrupt must be generated not at the start of a line, but at the
   same offset into a line as the first display byte and 64 scanlines
   earlier" — which puts T-state 0 inside the vertical sync, directly above
   the first pixel the chip ever paints. */
static void the_interrupt_stands_directly_above_the_first_pixel(void) {
  ula_init(&ula);
  TEST_EQUAL(ula.line, 0);
  TEST_CHECK(ula_csync(&ula)); /* line 0 is vertical sync */
  uint16_t column_of_interrupt = ula.column;
  ula_seek(&ula, 14336);
  TEST_EQUAL(ula.line, 64);
  TEST_EQUAL(ula.column, column_of_interrupt);
  TEST_CHECK(!ula_csync(&ula));
}

static void the_first_displayed_byte_is_14336_tstates_after_the_interrupt(void) {
  ula_init(&ula);
  uint16_t display = 0, attribute = 0;
  long first = -1;
  for (long tick = 0; tick < ULA_TICKS_PER_FRAME; tick++) {
    ula_seek(&ula, (uint32_t)tick);
    if (ula_fetch(&ula, &display, &attribute)) {
      first = tick;
      break;
    }
  }
  TEST_EQUAL(first, 14336);
  TEST_EQUAL(display, 0x4000);
  TEST_EQUAL(attribute, 0x5800);
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
  ula_init(&ula);
  int line = ULA_FIRST_DISPLAY_LINE;
  int column = ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS;
  uint8_t got[8];
  for (int tick = 0; tick < 4; tick++) {
    samples_at(line, column + tick, 0xA0, 0x07, &got[tick * 2]);
  }
  /* 0xA0 is 1010 0000: ink, paper, ink, paper, then four of paper. */
  const uint8_t want[8] = {7, 0, 7, 0, 0, 0, 0, 0};
  for (int pixel = 0; pixel < 8; pixel++) {
    TEST_EQUAL(got[pixel], want[pixel]);
  }
}

static void bright_lifts_both_ink_and_paper(void) {
  ula_init(&ula);
  uint8_t got[2];
  samples_at(ULA_FIRST_DISPLAY_LINE, ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS, 0x80, 0x41, got);
  TEST_EQUAL(got[0], 0x09); /* ink 1, bright */
  TEST_EQUAL(got[1], 0x08); /* paper 0, bright */
}

static void flash_swaps_ink_and_paper_every_sixteen_frames(void) {
  ula_init(&ula);
  uint8_t got[2];
  int line = ULA_FIRST_DISPLAY_LINE;
  int column = ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS;

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
  ula_init(&ula);
  ula_write(&ula, 0x05);
  uint8_t got[2];
  int line = ULA_FIRST_DISPLAY_LINE;
  samples_at(line, ULA_RETRACE_TICKS, 0xFF, 0xFF, got);
  TEST_EQUAL(got[0], 5); /* the left border */
  samples_at(line, ULA_TICKS_PER_LINE - 1, 0xFF, 0xFF, got);
  TEST_EQUAL(got[0], 5); /* and the right */
  samples_at(ULA_FIRST_DISPLAY_LINE - 1, ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS, 0xFF, 0xFF,
             got);
  TEST_EQUAL(got[0], 5); /* the line above the picture is all border */
}

static void the_beam_is_blanked_through_a_sync(void) {
  ula_init(&ula);
  ula_write(&ula, 0x05);
  uint8_t got[2];
  samples_at(ULA_FIRST_DISPLAY_LINE, 0, 0xFF, 0xFF, got);
  TEST_EQUAL(got[0], 0);
  TEST_EQUAL(got[1], 0);
}

static void the_frame_sync_holds_across_eight_whole_lines(void) {
  ula_init(&ula);
  TEST_EQUAL(ULA_VSYNC_LINES, 8);
  TEST_EQUAL(ULA_FIRST_DISPLAY_LINE, 64);
  TEST_EQUAL(ULA_DISPLAY_LINES, 192);
  for (int line = 0; line < 8; line++) {
    put_beam(line, 0);
    TEST_CHECK(ula_csync(&ula));
    put_beam(line, ULA_TICKS_PER_LINE - 1);
    TEST_CHECK(ula_csync(&ula));
  }
  /* The line after it syncs only through its retrace, like any other. */
  put_beam(8, ULA_RETRACE_TICKS - 1);
  TEST_CHECK(ula_csync(&ula));
  put_beam(8, ULA_RETRACE_TICKS);
  TEST_CHECK(!ula_csync(&ula));
}

static void the_first_contended_tstate_of_a_frame_is_14335(void) {
  ula_init(&ula);
  long first = -1;
  for (long tick = 0; tick < ULA_TICKS_PER_FRAME; tick++) {
    ula_seek(&ula, (uint32_t)tick);
    if (ula_contention(&ula) != 0) {
      first = tick;
      break;
    }
  }
  TEST_EQUAL(first, 14335);
}

static void the_slot_owes_six_falling_to_none(void) {
  ula_init(&ula);
  const uint8_t want[8] = {6, 5, 4, 3, 2, 1, 0, 0};
  for (int slot = 0; slot < 8; slot++) {
    ula_seek(&ula, (uint32_t)(14335 + slot));
    TEST_EQUAL(ula_contention(&ula), want[slot]);
  }
  /* and it repeats for the whole of the screen the chip is reading */
  ula_seek(&ula, 14335 + 8);
  TEST_EQUAL(ula_contention(&ula), 6);
  ula_seek(&ula, 14335 + 127);
  TEST_EQUAL(ula_contention(&ula), 0);
}

static void nothing_is_owed_off_the_picture(void) {
  ula_init(&ula);
  ula_seek(&ula, 14334); /* one before the first */
  TEST_EQUAL(ula_contention(&ula), 0);
  ula_seek(&ula, 14335 + 128); /* one past the last */
  TEST_EQUAL(ula_contention(&ula), 0);
  ula_seek(&ula, 0); /* the vertical sync */
  TEST_EQUAL(ula_contention(&ula), 0);
  put_beam(ULA_FIRST_DISPLAY_LINE - 1, ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS);
  TEST_EQUAL(ula_contention(&ula), 0); /* the border above the picture */
  put_beam(ULA_FIRST_DISPLAY_LINE + ULA_DISPLAY_LINES, ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS);
  TEST_EQUAL(ula_contention(&ula), 0); /* and below it */
}

static void port_fe_holds_the_border_the_microphone_and_the_speaker(void) {
  ula_init(&ula);
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
  TEST_RUN(ticking_and_seeking_agree_all_the_way_round_a_frame);
  TEST_RUN(the_interrupt_opens_the_frame_and_is_held_32_tstates);
  TEST_RUN(the_interrupt_stands_directly_above_the_first_pixel);
  TEST_RUN(the_first_displayed_byte_is_14336_tstates_after_the_interrupt);
  TEST_RUN(the_display_file_scatters_a_row_across_three_thirds);
  TEST_RUN(attributes_are_one_byte_to_a_character);
  TEST_RUN(a_byte_paints_eight_pixels_most_significant_first);
  TEST_RUN(bright_lifts_both_ink_and_paper);
  TEST_RUN(flash_swaps_ink_and_paper_every_sixteen_frames);
  TEST_RUN(the_border_fills_the_line_either_side_of_the_picture);
  TEST_RUN(the_beam_is_blanked_through_a_sync);
  TEST_RUN(the_frame_sync_holds_across_eight_whole_lines);
  TEST_RUN(the_first_contended_tstate_of_a_frame_is_14335);
  TEST_RUN(the_slot_owes_six_falling_to_none);
  TEST_RUN(nothing_is_owed_off_the_picture);
  TEST_RUN(port_fe_holds_the_border_the_microphone_and_the_speaker);
  return TEST_REPORT("ula");
}
