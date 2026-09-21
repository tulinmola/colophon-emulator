/*
 * gate_array_test — the chip alone, fed synthetic syncs.
 *
 * The interrupt generator is exercised pulse by pulse: R52 counts ends of
 * HSYNC, so each pulse here is one scanline's worth of edge.
 */
#include "gate_array.h"
#include "test.h"

static gate_array_t gate_array;

/* One of the CPU's cycles, as cpc_tick runs it: the character clock falls on
   quarter 0. */
static void advance(bool hsync, bool vsync) {
  gate_array_advance_phase(&gate_array);
  if (gate_array_character_clock(&gate_array)) {
    gate_array_tick(&gate_array, hsync, vsync);
  }
}

/* On to the next character clock and through it to quarter 1, where the
   interrupt generator counts. */
static void character(bool hsync, bool vsync) {
  do {
    advance(hsync, vsync);
  } while (!gate_array_character_clock(&gate_array));
  advance(hsync, vsync);
}

/* One HSYNC: assert, end it, and give the line one character more. The
   interrupt generator acts on the end a character after it (ch. 27.6.1),
   so a pulse that stopped at the end would leave every test here reading
   the moment before. */
static void pulse_hsync(void) {
  character(true, false);
  character(false, false);
  character(false, false);
}

static void pulse_hsyncs(int count) {
  for (int pulse = 0; pulse < count; pulse++) {
    pulse_hsync();
  }
}

/* A character whose two bytes carry the same display enable, which is what
   the chip reports unless one of the rules that part them is in play. */
static void draw_character(bool display, uint8_t byte0, uint8_t byte1,
                           uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER]) {
  gate_array_video(&gate_array, display, display, byte0, byte1, samples);
}

static void reset_state(void) {
  gate_array_init(&gate_array);
  TEST_CHECK(gate_array.lower_rom_enabled);
  TEST_CHECK(gate_array.upper_rom_enabled);
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.mode, 0);
  TEST_EQUAL(gate_array.pen, 0);
}

static void pen_selects_and_ink_paints(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x05); /* PENR: pen 5 */
  gate_array_write(&gate_array, 0x54); /* INKR: colour &14 */
  TEST_EQUAL(gate_array.inks[5], 0x14);
  gate_array_write(&gate_array, 0x10); /* PENR: the border */
  gate_array_write(&gate_array, 0x4B); /* INKR: colour &0B */
  TEST_EQUAL(gate_array.inks[16], 0x0B);
  TEST_EQUAL(gate_array.inks[5], 0x14);
}

static void rmr_owns_roms_and_mode(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x8D); /* RMR: ROMs off, mode 1 */
  TEST_CHECK(!gate_array.lower_rom_enabled);
  TEST_CHECK(!gate_array.upper_rom_enabled);
  TEST_EQUAL(gate_array.mode_pending, 1);
  TEST_EQUAL(gate_array.mode, 0); /* not yet: a mode waits for the HSYNC */
  pulse_hsync();
  TEST_EQUAL(gate_array.mode, 1);
}

/* The end of an HSYNC is counted a character later, and the interrupt it
   asks for rises then: "an interrupt always starts 1 µsec after the end of
   the HSYNC regardless of the CRTC", and the diagrams below that put it
   R3+1 microseconds after C0 reaches R2 — 15 for an R3 of 14, 9 for 8, 2
   for 1 (ch. 27.6.1, 27.6.2). Within that character it rises on quarter 1,
   a NOP's T3 here, where "the INT signal of the Z80A is positioned at T3
   level" (ch. 27.7.2). */
static void the_request_rises_a_character_after_the_hsync(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(51);
  TEST_CHECK(!gate_array.interrupt_request);

  character(true, false);
  character(false, false); /* the end R52 counts */
  TEST_EQUAL(gate_array.r52, 51);
  TEST_CHECK(!gate_array.interrupt_request);
  do {
    advance(false, false);
  } while (!gate_array_character_clock(&gate_array));
  TEST_CHECK(!gate_array.interrupt_request); /* the character, on quarter 0 */
  advance(false, false);
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(gate_array.interrupt_request); /* and on quarter 1 */
}

/* And an interrupt asked for by an HSYNC that has just ended is cancelled
   with the rest by RMR's bit 4, rather than arriving a character after the
   program disowned it. */
static void rmr_bit_4_reaches_a_request_not_yet_risen(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(51);
  character(true, false);
  character(false, false);
  gate_array_write(&gate_array, 0x90); /* RMR with bit 4 */
  character(false, false);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);
}

/* The one step of R52 an acknowledge can arrive in front of. "If R52 is 31,
   and the GATE ARRAY receives an end of HSYNC from the CRTC to increment
   R52, but at the same time it receives the order to eliminate bit 5, what
   happens?" — either "R52 goes from 31 to 32, then its bit 5 is eliminated,
   and R52 goes to 0", leaving the next interrupt 52 lines off, or "bit 5 of
   R52=31 is eliminated (which has no effect) and R52 changes to 32", leaving
   it 20 (ch. 27.7.1). The chip answers by order alone: an acknowledge heard
   before the count leaves 32, and one heard after it 0. Both are heard on
   quarter 1, where the machine hears them, a character apart. Where the
   processor's acknowledge ends is cpc_timing_test's to pin, and which order
   an instruction's length leads to is graded only by Shaker's B (R). */
static void an_acknowledge_races_r52_past_31(void) {
  reset_state();
  pulse_hsyncs(31);
  TEST_EQUAL(gate_array.r52, 31);
  character(true, false);
  character(false, false);                        /* the end that will take R52 to 32 */
  gate_array_interrupt_acknowledged(&gate_array); /* heard first */
  character(false, false);
  TEST_EQUAL(gate_array.r52, 32);

  reset_state();
  pulse_hsyncs(31);
  character(true, false);
  character(false, false);
  character(false, false); /* counted: 32 */
  TEST_EQUAL(gate_array.r52, 32);
  gate_array_interrupt_acknowledged(&gate_array); /* heard after */
  TEST_EQUAL(gate_array.r52, 0);
}

/* Nor is a step that finds bit 5 already set a race: there is nothing new to
   take away, and the acknowledge takes it either side of the count. */
static void only_the_step_past_31_races(void) {
  for (int heard_after = 0; heard_after <= 1; heard_after++) {
    reset_state();
    pulse_hsyncs(40);
    TEST_EQUAL(gate_array.r52, 40);
    character(true, false);
    character(false, false);
    if (!heard_after) {
      gate_array_interrupt_acknowledged(&gate_array);
    }
    character(false, false);
    if (heard_after) {
      gate_array_interrupt_acknowledged(&gate_array);
    }
    TEST_EQUAL(gate_array.r52, 9); /* 41 with bit 5 taken away */
  }
}

/* Whether an HSYNC end is the frame's check is settled when it ends, though
   what it does waits: one that ended the character before a VSYNC began is
   not among the two HSYNCs after it (ch. 27.3.2). */
static void an_hsync_ending_before_the_vsync_is_not_counted_after_it(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(40);
  character(true, false);
  character(false, false); /* the end, not yet acted on */
  character(false, true);  /* VSYNC begins, and that end is acted on */
  TEST_EQUAL(gate_array.r52, 41);
  pulse_hsync(); /* the first after the VSYNC */
  TEST_EQUAL(gate_array.r52, 42);
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync(); /* the second: the check, with bit 5 of 42 set */
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(gate_array.interrupt_request);
}

static void r52_loops_at_52_and_holds_the_request(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(51);
  TEST_EQUAL(gate_array.r52, 51);
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync();
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(gate_array.interrupt_request);
  pulse_hsyncs(10); /* nobody acknowledges: the line is maintained */
  TEST_CHECK(gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 10);
}

static void acknowledge_kills_bit_5(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(52); /* request raised, R52 back at 0 */
  pulse_hsyncs(40); /* still unacknowledged; R52 evolves */
  gate_array_interrupt_acknowledged(&gate_array);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 8); /* 40 with bit 5 dead */
  pulse_hsyncs(43);
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync(); /* 8 + 44 = 52 */
  TEST_CHECK(gate_array.interrupt_request);
}

static void rmr_bit_4_clears_counter_and_request(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(30);
  gate_array_write(&gate_array, 0x90); /* RMR with bit 4 */
  TEST_EQUAL(gate_array.r52, 0);
  pulse_hsyncs(52);
  TEST_CHECK(gate_array.interrupt_request);
  gate_array_write(&gate_array, 0x90);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);
}

/* The interrupt the frame's own check raises waits the same character as
   the one R52 counts out (ch. 27.6.1, 27.3.2). */
static void the_vsync_checks_request_waits_the_same_character(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(40);
  character(false, true); /* VSYNC begins */
  pulse_hsync();          /* counts: R52 = 41 */

  character(true, false);
  character(false, false); /* the end the check falls on */
  TEST_EQUAL(gate_array.r52, 41);
  TEST_CHECK(!gate_array.interrupt_request);
  character(false, false);
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(gate_array.interrupt_request);
}

/* And the acknowledge does not reach a request raised on the cycle it is
   heard on, where RMR's bit 4 does: it answers the interrupt the processor
   was offered, and the counter has since asked for another. */
static void an_acknowledge_leaves_a_request_just_risen(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(51);
  character(true, false);
  character(false, false);
  character(false, false); /* the count that asks, this very cycle */
  TEST_CHECK(gate_array.interrupt_request);
  gate_array_interrupt_acknowledged(&gate_array);
  TEST_CHECK(gate_array.interrupt_request);
  advance(false, false);
  gate_array_interrupt_acknowledged(&gate_array); /* a cycle on, it does */
  TEST_CHECK(!gate_array.interrupt_request);
}

static void vsync_check_interrupts_only_from_afar(void) {
  /* Two HSYNCs after a VSYNC begins: interrupt only if bit 5 of R52 is
     set, and R52 returns to 0 either way (Compendium ch. 27.3.2). */
  gate_array_init(&gate_array);
  pulse_hsyncs(40);
  character(false, true); /* VSYNC begins */
  pulse_hsync();          /* counts: R52 = 41 */
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync(); /* the check: bit 5 of 41 is set */
  TEST_CHECK(gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);

  gate_array_init(&gate_array);
  pulse_hsyncs(20);
  character(false, true);
  pulse_hsyncs(2); /* bit 5 of 21 is clear: too close, no interrupt */
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);
}

/* Give every pen the ink of its own number, so a sample reads back as the
   pen that produced it. */
static void inks_name_their_pens(void) {
  for (uint8_t pen = 0; pen < 16; pen++) {
    gate_array_write(&gate_array, pen);
    gate_array_write(&gate_array, (uint8_t)(0x40 | pen));
  }
}

/* Serialise one character, discarding the pipeline's first output. */
static void serialise(uint8_t byte0, uint8_t byte1, uint8_t samples[16]) {
  draw_character(true, byte0, byte1, samples);
  draw_character(true, 0, 0, samples);
}

static void mode_0_paints_two_fat_pixels_a_byte(void) {
  gate_array_init(&gate_array);
  inks_name_their_pens();
  gate_array_write(&gate_array, 0x8C); /* RMR: mode 0 */
  pulse_hsync();
  uint8_t samples[16];
  /* Bit 7 carries pen bit 0, bit 3 pen bit 1, bit 5 pen bit 2, bit 1 pen
     bit 3; the odd bits carry the second pixel. &88 is pen 3, then 0. */
  serialise(0x88, 0x00, samples);
  for (int sample = 0; sample < 4; sample++) {
    TEST_EQUAL(samples[sample], 3);
  }
  for (int sample = 4; sample < 16; sample++) {
    TEST_EQUAL(samples[sample], 0);
  }
}

static void mode_1_paints_four_pixels_a_byte(void) {
  gate_array_init(&gate_array);
  inks_name_their_pens();
  gate_array_write(&gate_array, 0x8D); /* RMR: mode 1 */
  pulse_hsync();
  uint8_t samples[16];
  /* &80 sets bit 7: pen bit 0 of the leftmost pixel. */
  serialise(0x80, 0x08, samples);
  TEST_EQUAL(samples[0], 1);
  TEST_EQUAL(samples[1], 1);
  TEST_EQUAL(samples[2], 0);
  /* &08 sets bit 3: pen bit 1 of the second byte's leftmost pixel. */
  TEST_EQUAL(samples[8], 2);
  TEST_EQUAL(samples[9], 2);
  TEST_EQUAL(samples[10], 0);
}

static void mode_2_paints_a_pixel_a_bit(void) {
  gate_array_init(&gate_array);
  inks_name_their_pens();
  gate_array_write(&gate_array, 0x8E); /* RMR: mode 2 */
  pulse_hsync();
  uint8_t samples[16];
  serialise(0xA0, 0x01, samples);
  TEST_EQUAL(samples[0], 1);
  TEST_EQUAL(samples[1], 0);
  TEST_EQUAL(samples[2], 1);
  TEST_EQUAL(samples[3], 0);
  TEST_EQUAL(samples[15], 1);
  TEST_EQUAL(samples[14], 0);
}

static void mode_3_ignores_four_bits_a_byte(void) {
  gate_array_init(&gate_array);
  inks_name_their_pens();
  gate_array_write(&gate_array, 0x8F); /* RMR: mode 3 */
  pulse_hsync();
  uint8_t samples[16];
  /* Mode 0's widths, mode 1's two leftmost pixels: &88 is pen 3, and the
     bits mode 0 would have read as pen bits 2 and 3 do nothing. */
  serialise(0x88, 0x30, samples);
  for (int sample = 0; sample < 4; sample++) {
    TEST_EQUAL(samples[sample], 3);
  }
  for (int sample = 8; sample < 16; sample++) {
    TEST_EQUAL(samples[sample], 0);
  }
}

static void the_border_fills_a_character_that_is_not_displayed(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x10); /* PENR: the border */
  gate_array_write(&gate_array, 0x49); /* INKR: colour 9 */
  uint8_t samples[16];
  draw_character(false, 0xFF, 0xFF, samples);
  draw_character(false, 0xFF, 0xFF, samples);
  for (int sample = 0; sample < 16; sample++) {
    TEST_EQUAL(samples[sample], 9);
  }
}

/* The display enable is read once for each of a character's two bytes, so
   the border can take one and not the other — which is what two of the
   CRTC's own rules do with it (ch. 17.6.2, 18.3.2). */
static void the_border_can_take_one_byte_of_a_character(void) {
  gate_array_init(&gate_array);
  inks_name_their_pens();
  gate_array_write(&gate_array, 0x8E); /* RMR: mode 2, a pixel a bit */
  pulse_hsync();                       /* which a mode change waits for */
  gate_array_write(&gate_array, 0x10); /* PENR: the border */
  gate_array_write(&gate_array, 0x49); /* INKR: colour 9 */
  uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER];

  /* Displayed first, border second: the bytes are all ones, so a displayed
     byte reads as pen 1 and a bordered one as the border's own colour. */
  gate_array_video(&gate_array, true, false, 0xFF, 0xFF, samples);
  gate_array_video(&gate_array, true, false, 0xFF, 0xFF, samples);
  for (int sample = 0; sample < 8; sample++) {
    TEST_EQUAL(samples[sample], 1);
  }
  for (int sample = 8; sample < GATE_ARRAY_SAMPLES_PER_CHARACTER; sample++) {
    TEST_EQUAL(samples[sample], 9);
  }

  /* And the other way about. */
  gate_array_video(&gate_array, false, true, 0xFF, 0xFF, samples);
  gate_array_video(&gate_array, false, true, 0xFF, 0xFF, samples);
  for (int sample = 0; sample < 8; sample++) {
    TEST_EQUAL(samples[sample], 9);
  }
  for (int sample = 8; sample < GATE_ARRAY_SAMPLES_PER_CHARACTER; sample++) {
    TEST_EQUAL(samples[sample], 1);
  }
}

static void the_beam_is_blanked_through_the_syncs(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x10);
  gate_array_write(&gate_array, 0x49); /* a border that is not black */
  uint8_t samples[16];
  gate_array_tick(&gate_array, true, false); /* the CRTC's HSYNC begins */
  draw_character(false, 0, 0, samples);
  draw_character(false, 0, 0, samples);
  for (int sample = 0; sample < 16; sample++) {
    TEST_EQUAL(samples[sample], GATE_ARRAY_BLACK);
  }
  gate_array_tick(&gate_array, false, false); /* and ends */
  draw_character(false, 0, 0, samples);
  TEST_EQUAL(samples[0], 9);
}

static void a_character_reaches_the_screen_a_microsecond_late(void) {
  gate_array_init(&gate_array);
  inks_name_their_pens();
  gate_array_write(&gate_array, 0x8E); /* mode 2 */
  pulse_hsync();
  uint8_t samples[16];
  draw_character(true, 0xFF, 0xFF, samples);
  TEST_EQUAL(samples[0], 0); /* what was in the pipeline before */
  draw_character(true, 0x00, 0x00, samples);
  TEST_EQUAL(samples[0], 1); /* the &FF handed over last time */
  draw_character(true, 0x00, 0x00, samples);
  TEST_EQUAL(samples[0], 0);
}

static void csync_follows_the_hsync_two_characters_behind(void) {
  gate_array_init(&gate_array);
  TEST_CHECK(!gate_array_csync(&gate_array));
  gate_array_tick(&gate_array, true, false); /* HSYNC begins */
  TEST_CHECK(!gate_array_csync(&gate_array));
  gate_array_tick(&gate_array, true, false);
  TEST_CHECK(!gate_array_csync(&gate_array));
  gate_array_tick(&gate_array, true, false); /* H06 reaches 2 */
  TEST_CHECK(gate_array_csync(&gate_array));
  for (int character = 0; character < 3; character++) {
    gate_array_tick(&gate_array, true, false);
    TEST_CHECK(gate_array_csync(&gate_array));
  }
  gate_array_tick(&gate_array, true, false); /* H06 reaches 6: four wide */
  TEST_CHECK(!gate_array_csync(&gate_array));
}

static void a_short_hsync_cuts_the_pulse_short(void) {
  gate_array_init(&gate_array);
  gate_array_tick(&gate_array, true, false);
  gate_array_tick(&gate_array, true, false);
  gate_array_tick(&gate_array, true, false); /* asserted at H06 = 2 */
  TEST_CHECK(gate_array_csync(&gate_array));
  gate_array_tick(&gate_array, false, false); /* the CRTC's HSYNC ends */
  TEST_CHECK(!gate_array_csync(&gate_array));
}

/* A line of the standard frame: 64 characters, HSYNC 14 wide from 46. */
static void run_standard_line(bool vsync) {
  for (int character = 0; character < 64; character++) {
    bool hsync = character >= 46 && character < 60;
    gate_array_tick(&gate_array, hsync, vsync);
  }
}

static void the_frame_sync_inverts_the_line_pulses(void) {
  gate_array_init(&gate_array);
  run_standard_line(false);
  run_standard_line(true); /* the CRTC's VSYNC: V26 counts to 1 */
  run_standard_line(true); /* V26 reaches 2 at this line's HSYNC end, where
                              the Gate Array's own VSYNC begins */
  int asserted = 0;
  for (int character = 0; character < 64; character++) {
    bool hsync = character >= 46 && character < 60;
    gate_array_tick(&gate_array, hsync, true);
    if (gate_array_csync(&gate_array)) {
      asserted++;
    }
  }
  /* Inverted: asserted almost the whole line, with a four-character
     serration where the line pulse would have been. */
  TEST_EQUAL(asserted, 60);
  TEST_CHECK(gate_array.sig_vsync);
}

static void the_frame_sync_lasts_four_lines(void) {
  gate_array_init(&gate_array);
  run_standard_line(false);
  run_standard_line(true); /* V26 = 1 at this line's HSYNC end */
  int vsync_lines = 0;
  for (int line = 0; line < 10; line++) {
    run_standard_line(true);
    if (gate_array.sig_vsync) {
      vsync_lines++;
    }
  }
  TEST_EQUAL(vsync_lines, 4);
}

static void the_beam_is_blanked_for_twenty_six_lines(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x10);
  gate_array_write(&gate_array, 0x49);
  run_standard_line(true); /* the CRTC's VSYNC begins */
  int blanked = 0;
  for (int line = 0; line < 40; line++) {
    run_standard_line(false);
    if (gate_array.black_vsync) {
      blanked++;
    }
  }
  TEST_EQUAL(blanked, 24); /* plus the line that began it and the one that
                              ends it partway: 26 in all */
  TEST_EQUAL(gate_array.v26, 26);
}

static void the_palette_is_the_one_measured_on_silicon(void) {
  TEST_EQUAL(gate_array_rgb(GATE_ARRAY_BLACK), 0x000201);
  TEST_EQUAL(gate_array_rgb(11), 0xFFF3F9); /* bright white */
  TEST_EQUAL(gate_array_rgb(4), 0x00026B);  /* blue */
  TEST_EQUAL(gate_array_rgb(12), 0xF30506); /* red */
  TEST_EQUAL(gate_array_rgb(10), 0xF3F30D); /* bright yellow */
  /* Hardware 0 and 1 are Amstrad's two "white"s: a mid grey, and its ghost
     a shade away. */
  TEST_EQUAL(gate_array_rgb(0), 0x6E7D6B);
  TEST_EQUAL(gate_array_rgb(1), 0x6E7B6D);
  TEST_EQUAL(gate_array_rgb(0x54), gate_array_rgb(20)); /* five bits kept */
}

static void suppression_does_not_revoke_a_held_request(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(52); /* raised, nobody listening */
  pulse_hsyncs(20);
  character(false, true);
  pulse_hsyncs(2); /* the check suppresses a new request only */
  TEST_CHECK(gate_array.interrupt_request);
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(pen_selects_and_ink_paints);
  TEST_RUN(rmr_owns_roms_and_mode);
  TEST_RUN(an_acknowledge_races_r52_past_31);
  TEST_RUN(only_the_step_past_31_races);
  TEST_RUN(an_hsync_ending_before_the_vsync_is_not_counted_after_it);
  TEST_RUN(r52_loops_at_52_and_holds_the_request);
  TEST_RUN(acknowledge_kills_bit_5);
  TEST_RUN(rmr_bit_4_clears_counter_and_request);
  TEST_RUN(the_request_rises_a_character_after_the_hsync);
  TEST_RUN(rmr_bit_4_reaches_a_request_not_yet_risen);
  TEST_RUN(the_vsync_checks_request_waits_the_same_character);
  TEST_RUN(an_acknowledge_leaves_a_request_just_risen);
  TEST_RUN(vsync_check_interrupts_only_from_afar);
  TEST_RUN(suppression_does_not_revoke_a_held_request);
  TEST_RUN(mode_0_paints_two_fat_pixels_a_byte);
  TEST_RUN(mode_1_paints_four_pixels_a_byte);
  TEST_RUN(mode_2_paints_a_pixel_a_bit);
  TEST_RUN(mode_3_ignores_four_bits_a_byte);
  TEST_RUN(the_border_fills_a_character_that_is_not_displayed);
  TEST_RUN(the_border_can_take_one_byte_of_a_character);
  TEST_RUN(the_beam_is_blanked_through_the_syncs);
  TEST_RUN(a_character_reaches_the_screen_a_microsecond_late);
  TEST_RUN(csync_follows_the_hsync_two_characters_behind);
  TEST_RUN(a_short_hsync_cuts_the_pulse_short);
  TEST_RUN(the_frame_sync_inverts_the_line_pulses);
  TEST_RUN(the_frame_sync_lasts_four_lines);
  TEST_RUN(the_beam_is_blanked_for_twenty_six_lines);
  TEST_RUN(the_palette_is_the_one_measured_on_silicon);
  return TEST_REPORT("gate_array");
}
