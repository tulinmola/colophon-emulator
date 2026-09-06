/*
 * tape_test — the deck alone, timed against the published numbers.
 *
 * Every length here is written out as the specification writes it, not taken
 * from the deck's own constants: a test that reads tape.h grades tape.c
 * against its own arithmetic and would agree with any mistake in it.
 *
 * Sources:
 * - "TZX format" v1.13 (Tomaz Kac, maintained by Martijn van der Heide),
 *   https://worldofspectrum.net/TZXformat.html — block ID 11 tabulates the
 *   standard ROM timings a .tap is replayed at: pilot pulses of 2168
 *   T-states, 8063 of them before a header and 3223 before data, syncs of
 *   667 and 735, bits of 855 and 1710, and a second of pause after a block.
 */
#include "tape.h"
#include "test.h"

/* A Spectrum's, so the pause between blocks is a Spectrum's. */
#define TICKS_PER_MILLISECOND 3500

/* Bounds, so a deck that never leaves a pulse fails the test rather than
   hanging the tier. The longest pulse is the pause, at a second. */
#define MOST_TICKS_IN_A_PULSE (2 * 1000 * TICKS_PER_MILLISECOND)
#define MOST_PILOT_PULSES 20000
#define MOST_TICKS_IN_A_TAPE 40000000L

static tape_t tape;

/* Two blocks of two bytes: flag &00 marks a header and &FF data, and the
   second byte of each is there to be watched going out. */
static const uint8_t two_blocks[] = {0x02, 0x00, 0x00, 0xAA, 0x02, 0x00, 0xFF, 0x55};

static void insert_and_play(const uint8_t *image, uint32_t length) {
  const char *problem = NULL;
  tape_init(&tape, TICKS_PER_MILLISECOND);
  TEST_CHECK(tape_insert(&tape, image, length, &problem));
  tape_play(&tape);
}

/* The T-states the head holds its level for, up to the next edge. */
static uint32_t pulse(void) {
  const bool level = tape_level(&tape);
  uint32_t ticks = 0;
  while (tape_level(&tape) == level && ticks < MOST_TICKS_IN_A_PULSE) {
    tape_tick(&tape);
    ticks++;
  }
  return ticks;
}

/* One whole block: its pilot, its two syncs, and both bytes bit by bit, most
   significant first. Leaves the head on the pause that follows. */
static void the_block_plays(int want_pilot, uint8_t flag, uint8_t body) {
  int pilot = 0;
  uint32_t length = pulse();
  while (length == 2168 && pilot < MOST_PILOT_PULSES) {
    pilot++;
    length = pulse();
  }
  TEST_EQUAL(pilot, want_pilot);
  TEST_EQUAL(length, 667); /* the pulse that ended the pilot is the first sync */
  TEST_EQUAL(pulse(), 735);

  const uint8_t bytes[] = {flag, body};
  for (size_t index = 0; index < sizeof bytes; index++) {
    for (int bit = 7; bit >= 0; bit--) {
      uint32_t want = (bytes[index] >> bit) & 1 ? 1710 : 855;
      TEST_EQUAL(pulse(), want);
      TEST_EQUAL(pulse(), want);
    }
  }
}

/* The flag byte decides which pilot a block gets, and the threshold is 128
   rather than the &00 and &FF a header and a data block happen to use. */
static void the_pilot_turns_on_the_flag_bytes_top_bit(void) {
  const uint8_t just_under[] = {0x02, 0x00, 0x7F, 0x00};
  insert_and_play(just_under, sizeof just_under);
  TEST_EQUAL(pulse(), 1);
  int pilot = 0;
  while (pulse() == 2168 && pilot < MOST_PILOT_PULSES) {
    pilot++;
  }
  TEST_EQUAL(pilot, 8063);

  const uint8_t just_over[] = {0x02, 0x00, 0x80, 0x00};
  insert_and_play(just_over, sizeof just_over);
  TEST_EQUAL(pulse(), 1);
  pilot = 0;
  while (pulse() == 2168 && pilot < MOST_PILOT_PULSES) {
    pilot++;
  }
  TEST_EQUAL(pilot, 3223);
}

static void a_deck_with_no_tape_presents_a_low_level(void) {
  tape_init(&tape, TICKS_PER_MILLISECOND);
  TEST_CHECK(!tape_loaded(&tape));
  tape_play(&tape);
  TEST_CHECK(!tape_playing(&tape));
  for (int tick = 0; tick < 10000; tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(!tape_level(&tape));
}

static void bytes_that_are_not_blocks_are_refused(void) {
  const char *problem = NULL;
  tape_init(&tape, TICKS_PER_MILLISECOND);

  const uint8_t truncated[] = {0x08, 0x00, 0x00, 0xAA};
  TEST_CHECK(!tape_insert(&tape, truncated, sizeof truncated, &problem));
  const uint8_t empty_block[] = {0x00, 0x00};
  TEST_CHECK(!tape_insert(&tape, empty_block, sizeof empty_block, &problem));
  /* One byte too many is one byte the blocks do not account for. */
  const uint8_t trailing[] = {0x02, 0x00, 0xFF, 0x55, 0x00};
  TEST_CHECK(!tape_insert(&tape, trailing, sizeof trailing, &problem));
  TEST_CHECK(!tape_insert(&tape, two_blocks, 0, &problem));
  TEST_CHECK(!tape_loaded(&tape));
}

/* Refusing a tape is not the same as ejecting the one already in. */
static void a_refused_tape_leaves_the_one_in_the_deck(void) {
  insert_and_play(two_blocks, sizeof two_blocks);
  pulse();
  const char *problem = NULL;
  const uint8_t truncated[] = {0x08, 0x00, 0x00, 0xAA};
  TEST_CHECK(!tape_insert(&tape, truncated, sizeof truncated, &problem));
  TEST_CHECK(tape_loaded(&tape));
  TEST_CHECK(tape_playing(&tape));
  TEST_EQUAL(pulse(), 2168);
}

static void a_stopped_deck_stands_where_it_was(void) {
  insert_and_play(two_blocks, sizeof two_blocks);
  pulse();
  const bool level = tape_level(&tape);
  const uint32_t left = tape.pulse_ticks_left;
  tape_stop(&tape);
  TEST_CHECK(!tape_playing(&tape));
  /* An odd number, so a deck that kept running could not come back to the
     level it stopped on and call it standing still. */
  for (int tick = 0; tick < 101897; tick++) {
    tape_tick(&tape);
  }
  TEST_EQUAL(tape_level(&tape), level);
  TEST_EQUAL(tape.pulse_ticks_left, left);

  /* And it takes up where it left off rather than starting the pulse again. */
  tape_play(&tape);
  TEST_EQUAL(pulse(), left);
}

static void the_whole_tape_plays_as_the_published_pattern(void) {
  insert_and_play(two_blocks, sizeof two_blocks);
  TEST_EQUAL(pulse(), 1); /* the level starts low and the first tick turns it over */

  the_block_plays(8063, 0x00, 0xAA); /* a header earns the longer pilot */
  TEST_CHECK(!tape_level(&tape));
  TEST_EQUAL(pulse(), 1000 * TICKS_PER_MILLISECOND);

  the_block_plays(3223, 0xFF, 0x55); /* and the data block behind it */
  TEST_CHECK(!tape_level(&tape));

  /* The last pause has no edge to end it, so it is counted out rather than
     measured: the tape runs out on its last tick and the deck stops. */
  for (uint32_t tick = 1; tick < 1000 * TICKS_PER_MILLISECOND; tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(tape_playing(&tape));
  tape_tick(&tape);
  TEST_CHECK(!tape_playing(&tape));
  TEST_CHECK(!tape_level(&tape));
}

static void a_tape_played_out_does_not_start_again(void) {
  insert_and_play(two_blocks, sizeof two_blocks);
  for (long tick = 0; tick < MOST_TICKS_IN_A_TAPE && tape_playing(&tape); tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(!tape_playing(&tape));
  tape_play(&tape);
  TEST_CHECK(!tape_playing(&tape));
  for (int tick = 0; tick < 10000; tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(!tape_level(&tape));
}

int main(void) {
  TEST_RUN(a_deck_with_no_tape_presents_a_low_level);
  TEST_RUN(bytes_that_are_not_blocks_are_refused);
  TEST_RUN(a_refused_tape_leaves_the_one_in_the_deck);
  TEST_RUN(a_stopped_deck_stands_where_it_was);
  TEST_RUN(the_whole_tape_plays_as_the_published_pattern);
  TEST_RUN(the_pilot_turns_on_the_flag_bytes_top_bit);
  TEST_RUN(a_tape_played_out_does_not_start_again);
  return TEST_REPORT("tape");
}
