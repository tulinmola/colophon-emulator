/*
 * tape_test — the deck alone.
 *
 * The pulses come from a list written here rather than from any image, so
 * what is graded is the timing and nothing else: the deck holds a level for
 * as long as it is told and then asks for the next.
 */
#include <string.h>

#include "tape.h"
#include "test.h"

/* Longer than any pulse the tests below hand out, so a deck that never
   leaves one fails the test rather than hanging the tier. */
#define MOST_TICKS_IN_A_PULSE 100000

static tape_t tape;

/* A source handing out pulses written down in the test. */
static tape_pulse_t script[8];
static size_t script_length;
static size_t script_at;

static bool from_script(void *reader, tape_pulse_t *pulse) {
  (void)reader;
  if (script_at == script_length) {
    return false;
  }
  *pulse = script[script_at++];
  return true;
}

static void play(const tape_pulse_t *pulses, size_t count) {
  memcpy(script, pulses, count * sizeof *pulses);
  script_length = count;
  script_at = 0;
  tape_init(&tape);
  tape_insert(&tape, from_script, NULL);
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

static void a_deck_with_no_tape_presents_a_low_level(void) {
  tape_init(&tape);
  TEST_CHECK(!tape_loaded(&tape));
  tape_play(&tape);
  TEST_CHECK(!tape_playing(&tape));
  for (int tick = 0; tick < 1000; tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(!tape_level(&tape));
}

static void a_pulse_is_held_for_as_long_as_it_says(void) {
  const tape_pulse_t pulses[] = {{100, true}, {250, false}, {7, true}};
  play(pulses, 3);
  TEST_EQUAL(pulse(), 1); /* the level leaves low on the first tick */
  TEST_EQUAL(pulse(), 100);
  TEST_EQUAL(pulse(), 250);
}

/* The deck does not turn the level over; it presents what it is handed, so
   a run of pulses at one level is a silence and not an edge. */
static void the_deck_presents_the_level_it_is_handed(void) {
  const tape_pulse_t pulses[] = {{50, true}, {60, true}, {70, false}};
  play(pulses, 3);
  tape_tick(&tape);
  TEST_CHECK(tape_level(&tape));
  for (int tick = 0; tick < 50 + 60 - 1; tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(tape_level(&tape)); /* two pulses, one unbroken level */
  tape_tick(&tape);
  TEST_CHECK(!tape_level(&tape));
}

static void a_stopped_deck_stands_where_it_was(void) {
  const tape_pulse_t pulses[] = {{500, true}, {40, false}};
  play(pulses, 2);
  tape_tick(&tape); /* into the first pulse */
  const uint32_t left = tape.ticks_left;
  tape_stop(&tape);
  TEST_CHECK(!tape_playing(&tape));
  for (int tick = 0; tick < 9999; tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(tape_level(&tape));
  TEST_EQUAL(tape.ticks_left, left);

  tape_play(&tape);
  TEST_EQUAL(pulse(), left); /* and takes up the rest of the pulse it was on */
}

static void the_deck_stops_when_the_source_runs_out(void) {
  const tape_pulse_t pulses[] = {{30, true}};
  play(pulses, 1);
  for (int tick = 0; tick < 100 && tape_playing(&tape); tick++) {
    tape_tick(&tape);
  }
  TEST_CHECK(!tape_playing(&tape));
  TEST_CHECK(!tape_level(&tape));
}

/* A pulse of no T-states would never end, so the deck treats it as the end
   of the tape rather than standing on it forever. */
static void a_pulse_of_no_ticks_ends_the_tape(void) {
  const tape_pulse_t pulses[] = {{20, true}, {0, true}, {40, false}};
  play(pulses, 3);
  int ticks = 0;
  while (ticks < 100 && tape_playing(&tape)) {
    tape_tick(&tape);
    ticks++;
  }
  /* One tick takes up the first pulse and twenty hold it, so the deck reaches
     the pulse of no length on the twenty-first and stops there — not at the
     end of the script, which is a pulse further on. */
  TEST_EQUAL(ticks, 21);
  TEST_CHECK(!tape_playing(&tape));
  TEST_CHECK(!tape_level(&tape)); /* and the pulse behind it never reached the head */
}

/* A tape put in over a running one is a new tape: the deck stops, and neither
   the level nor the T-states left of the old one carry over. */
static void a_tape_put_in_over_another_starts_it_stopped(void) {
  const tape_pulse_t first[] = {{20, true}, {40, false}};
  play(first, 2);
  tape_tick(&tape);
  TEST_CHECK(tape_playing(&tape));
  TEST_CHECK(tape_level(&tape));

  const tape_pulse_t second[] = {{30, true}, {40, false}};
  memcpy(script, second, sizeof second);
  script_length = 2;
  script_at = 0;
  tape_insert(&tape, from_script, NULL);
  TEST_CHECK(!tape_playing(&tape));
  TEST_CHECK(!tape_level(&tape));

  tape_play(&tape);
  TEST_EQUAL(pulse(), 1);  /* low again, as an empty deck is */
  TEST_EQUAL(pulse(), 30); /* and the new tape's first pulse, whole */
}

int main(void) {
  TEST_RUN(a_deck_with_no_tape_presents_a_low_level);
  TEST_RUN(a_pulse_is_held_for_as_long_as_it_says);
  TEST_RUN(the_deck_presents_the_level_it_is_handed);
  TEST_RUN(a_stopped_deck_stands_where_it_was);
  TEST_RUN(the_deck_stops_when_the_source_runs_out);
  TEST_RUN(a_pulse_of_no_ticks_ends_the_tape);
  TEST_RUN(a_tape_put_in_over_another_starts_it_stopped);
  return TEST_REPORT("tape");
}
