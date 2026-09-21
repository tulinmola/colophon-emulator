/*
 * tape.c — the reel, turning.
 */
#include "tape.h"

#include <string.h>

void tape_init(tape_t *tape) { memset(tape, 0, sizeof *tape); }

void tape_insert(tape_t *tape, tape_source source, void *reader) {
  tape_init(tape);
  tape->source = source;
  tape->reader = reader;
}

bool tape_loaded(const tape_t *tape) { return tape->source != NULL; }

void tape_play(tape_t *tape) { tape->playing = tape_loaded(tape); }

void tape_stop(tape_t *tape) { tape->playing = false; }

bool tape_playing(const tape_t *tape) { return tape->playing; }

bool tape_level(const tape_t *tape) { return tape->level; }

void tape_tick(tape_t *tape) {
  if (!tape->playing) {
    return;
  }
  if (tape->ticks_left > 0) {
    tape->ticks_left--;
  }
  if (tape->ticks_left > 0) {
    return;
  }
  tape_pulse_t pulse = {0, false};
  if (!tape->source(tape->reader, &pulse) || pulse.ticks == 0) {
    tape->playing = false;
    tape->level = false;
    return;
  }
  tape->level = pulse.level;
  tape->ticks_left = pulse.ticks;
}
