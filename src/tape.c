/*
 * tape.c — the block, pulse by pulse.
 */
#include "tape.h"

#include <string.h>

/* Which part of a block is playing. NEXT_BLOCK is between blocks, where the
   one after this is taken up or the tape is found to have run out. */
enum {
  PHASE_NEXT_BLOCK = 0,
  PHASE_PILOT,
  PHASE_FIRST_SYNC,
  PHASE_SECOND_SYNC,
  PHASE_DATA,
  PHASE_PAUSE,
  PHASE_FINISHED,
};

/* Every block opens with its length in two bytes, which are not part of it. */
#define LENGTH_BYTES 2

static uint32_t block_length(const tape_t *tape, uint32_t at) {
  return (uint32_t)(tape->image[at] | (tape->image[at + 1] << 8));
}

void tape_init(tape_t *tape, uint32_t ticks_per_millisecond) {
  memset(tape, 0, sizeof *tape);
  tape->ticks_per_millisecond = ticks_per_millisecond;
  tape->phase = PHASE_FINISHED;
}

bool tape_insert(tape_t *tape, const uint8_t *image, uint32_t length, const char **problem) {
  /* The blocks have to walk to the end exactly. Nothing else identifies a
     .tap: it has no signature and no header of its own, so a file that does
     not walk is the only kind this can refuse. */
  uint32_t at = 0;
  uint32_t blocks = 0;
  while (at + LENGTH_BYTES <= length) {
    uint32_t block = (uint32_t)(image[at] | (image[at + 1] << 8));
    if (block == 0) {
      *problem = "has a block of no bytes, which no tape carries";
      return false;
    }
    at += LENGTH_BYTES + block;
    blocks++;
  }
  if (at != length || blocks == 0) {
    *problem = "does not divide into blocks, so it is not a tape";
    return false;
  }

  uint32_t ticks_per_millisecond = tape->ticks_per_millisecond;
  tape_init(tape, ticks_per_millisecond);
  tape->image = image;
  tape->image_length = length;
  tape->phase = PHASE_NEXT_BLOCK;
  return true;
}

bool tape_loaded(const tape_t *tape) { return tape->image != NULL; }

void tape_play(tape_t *tape) { tape->playing = tape_loaded(tape) && tape->phase != PHASE_FINISHED; }

void tape_stop(tape_t *tape) { tape->playing = false; }

bool tape_playing(const tape_t *tape) { return tape->playing; }

bool tape_level(const tape_t *tape) { return tape->level; }

static void start_block(tape_t *tape) {
  if (tape->block_at + LENGTH_BYTES > tape->image_length) {
    tape->phase = PHASE_FINISHED;
    tape->playing = false;
    tape->level = false;
    return;
  }
  tape->byte_at = tape->block_at + LENGTH_BYTES;
  tape->data_end = tape->byte_at + block_length(tape, tape->block_at);
  /* A header's flag byte is under 128 and earns the longer pilot. */
  tape->pilot_pulses_left =
      tape->image[tape->byte_at] < 128 ? TAPE_HEADER_PILOT_PULSES : TAPE_DATA_PILOT_PULSES;
  tape->bit_number = 7;
  tape->in_second_pulse = false;
  tape->phase = PHASE_PILOT;
}

/* The level turns over at every pulse, except through the pause, which is
   one long stretch of low. */
static void next_pulse(tape_t *tape) {
  if (tape->phase == PHASE_NEXT_BLOCK) {
    start_block(tape);
  }
  switch (tape->phase) {
    case PHASE_PILOT:
      tape->pilot_pulses_left--;
      tape->level = !tape->level;
      tape->pulse_ticks_left = TAPE_PILOT_TICKS;
      if (tape->pilot_pulses_left == 0) {
        tape->phase = PHASE_FIRST_SYNC;
      }
      break;

    case PHASE_FIRST_SYNC:
      tape->level = !tape->level;
      tape->pulse_ticks_left = TAPE_FIRST_SYNC_TICKS;
      tape->phase = PHASE_SECOND_SYNC;
      break;

    case PHASE_SECOND_SYNC:
      tape->level = !tape->level;
      tape->pulse_ticks_left = TAPE_SECOND_SYNC_TICKS;
      tape->phase = PHASE_DATA;
      break;

    case PHASE_DATA: {
      bool one = (tape->image[tape->byte_at] & (1u << tape->bit_number)) != 0;
      tape->level = !tape->level;
      tape->pulse_ticks_left = one ? TAPE_ONE_BIT_TICKS : TAPE_ZERO_BIT_TICKS;
      if (!tape->in_second_pulse) {
        tape->in_second_pulse = true;
        break;
      }
      tape->in_second_pulse = false;
      if (tape->bit_number > 0) {
        tape->bit_number--;
        break;
      }
      tape->bit_number = 7;
      if (++tape->byte_at == tape->data_end) {
        tape->phase = PHASE_PAUSE;
      }
      break;
    }

    case PHASE_PAUSE:
      tape->level = false;
      tape->pulse_ticks_left = TAPE_PAUSE_MILLISECONDS * tape->ticks_per_millisecond;
      tape->block_at = tape->data_end;
      tape->phase = PHASE_NEXT_BLOCK;
      break;

    case PHASE_FINISHED:
    default:
      tape->playing = false;
      tape->level = false;
      break;
  }
}

void tape_tick(tape_t *tape) {
  if (!tape->playing) {
    return;
  }
  if (tape->pulse_ticks_left > 0) {
    tape->pulse_ticks_left--;
  }
  if (tape->pulse_ticks_left == 0) {
    next_pulse(tape);
  }
}
