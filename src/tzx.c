/*
 * tzx.c — the blocks, and the pulses they come to.
 */
#include "tzx.h"

#include <string.h>

/* The clock every timing in the format is written in, whichever machine the
   tape is for. */
#define TICKS_PER_MILLISECOND 3500

/* The ROM's own timings, which a .tap is replayed at and a .tzx standard
   block asks for by name; the pause is the one block ID 10 defaults to. */
#define PILOT_TICKS 2168
#define FIRST_SYNC_TICKS 667
#define SECOND_SYNC_TICKS 735
#define ZERO_BIT_TICKS 855
#define ONE_BIT_TICKS 1710
#define HEADER_PILOT_PULSES 8063
#define DATA_PILOT_PULSES 3223
#define PAUSE_MILLISECONDS 1000

/* The leader tape that passes the head when a rewound cassette is played,
   before the recording can begin. GOST 20492-87 gives a cassette's leader as
   "(350±100) мм" and the NAB cassette standard (1976) as "12 to 15 inches";
   79mm of it has already passed the head, running from the take-up hub's
   clamp round the corner roller (ECMA-34, 1976, Figs. 3 and 4: hubs "42,5"
   apart, "27,9" behind the reference line, outer guides "66,6" apart; the
   roller, which no standard places, estimated from photographs of opened
   cassettes, among them
   https://commons.wikimedia.org/wiki/File:Compact_cassette_-_open.png).
   So (350 - 79)mm at 47.625mm a second, 5.7s, for the standards' nominal
   cassette. One sold without leader tape had none, and nothing here can
   tell which kind an image was taken from. */
#define LEADER_MILLISECONDS 5700

enum {
  PHASE_BETWEEN_BLOCKS = 0,
  PHASE_PILOT,
  PHASE_FIRST_SYNC,
  PHASE_SECOND_SYNC,
  PHASE_DATA,
  PHASE_TONE,
  PHASE_SEQUENCE,
  PHASE_PAUSE,
  PHASE_SILENCE,
  PHASE_STOPPED,
  PHASE_FINISHED,
};

/* "ZXTape!" and the end-of-text byte, then two version bytes. */
static const uint8_t signature[] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A};
#define HEADER_SIZE 10

/* Every .tap block opens with its length in two bytes, which are not part
   of it. */
#define LENGTH_BYTES 2

static uint32_t two_bytes_at(const uint8_t *image, uint32_t at) {
  return (uint32_t)(image[at] | (image[at + 1] << 8));
}

static uint32_t three_bytes_at(const uint8_t *image, uint32_t at) {
  return (uint32_t)(image[at] | (image[at + 1] << 8) | ((uint32_t)image[at + 2] << 16));
}

static uint32_t four_bytes_at(const uint8_t *image, uint32_t at) {
  return three_bytes_at(image, at) | ((uint32_t)image[at + 3] << 24);
}

/* A block this reader knows, whose length the file ends before giving. It is
   longer than any file can be, so the check for a block running off the end
   is what catches it. */
#define BLOCK_CUT_SHORT UINT32_MAX

/* How many bytes a .tzx block occupies, its identifying byte included; zero
   for a block this reader does not know, and BLOCK_CUT_SHORT for one it does.
   The lengths are the specification's own table, which is the only place a
   block's length is given — so an unknown block cannot be stepped over, and
   everything after it would be read as the wrong thing. */
static uint32_t block_span(const uint8_t *image, uint32_t length, uint32_t at) {
  const uint32_t left = length - at - 1;
  const uint8_t id = image[at];
  const uint8_t *body = image + at + 1;
  switch (id) {
    case 0x10:
      return left < 4 ? BLOCK_CUT_SHORT : 1 + 4 + two_bytes_at(body, 2);
    case 0x11:
      return left < 0x12 ? BLOCK_CUT_SHORT : 1 + 0x12 + three_bytes_at(body, 0x0F);
    case 0x12:
      return left < 4 ? BLOCK_CUT_SHORT : 1 + 4;
    case 0x13:
      return left < 1 ? BLOCK_CUT_SHORT : 1 + 1 + 2u * body[0];
    case 0x14:
      return left < 0x0A ? BLOCK_CUT_SHORT : 1 + 0x0A + three_bytes_at(body, 7);
    case 0x20:
      return left < 2 ? BLOCK_CUT_SHORT : 1 + 2;
    case 0x21:
      return left < 1 ? BLOCK_CUT_SHORT : 1u + 1 + body[0];
    case 0x22:
      return 1;
    case 0x24:
      return left < 2 ? BLOCK_CUT_SHORT : 1 + 2;
    case 0x25:
      return 1;
    case 0x2A:
    case 0x2B: {
      /* Added after v1.10, so each carries its own: "ALL custom blocks that
         will be added after version 1.10 will have the length of the block in
         first 4 bytes (long word) after the ID". */
      if (left < 4) {
        return BLOCK_CUT_SHORT;
      }
      const uint32_t said = four_bytes_at(body, 0);
      return said > left - 4 ? BLOCK_CUT_SHORT : 1 + 4 + said;
    }
    case 0x30:
      return left < 1 ? BLOCK_CUT_SHORT : 1u + 1 + body[0];
    case 0x31:
      return left < 2 ? BLOCK_CUT_SHORT : 1u + 2 + body[1];
    case 0x32:
      return left < 2 ? BLOCK_CUT_SHORT : 1 + 2 + two_bytes_at(body, 0);
    case 0x33:
      return left < 1 ? BLOCK_CUT_SHORT : 1 + 1 + 3u * body[0];
    case 0x35: {
      if (left < 0x14) {
        return BLOCK_CUT_SHORT;
      }
      const uint32_t said = four_bytes_at(body, 0x10);
      return said > left - 0x14 ? BLOCK_CUT_SHORT : 1 + 0x14 + said;
    }
    case 0x34: /* emulation info: Spectrum-specific, and an Amstrad ignores it */
      return 1 + 8;
    case 0x40: /* a snapshot, which an Amstrad ignores likewise */
      return left < 4 ? BLOCK_CUT_SHORT : 1 + 4 + three_bytes_at(body, 1);
    case 0x5A:
      return 1 + 9;
    default:
      return 0;
  }
}

/* The blocks this reader plays or steps over; every other one it refuses,
   with a reason a reader of the message can act on. */
static const char *refusal_for_block(uint8_t id) {
  switch (id) {
    case 0x15:
    case 0x18:
      return "records its waveform sample by sample, which this reader does not play";
    case 0x19:
      return "records its data as a table of symbols, which this reader does not build";
    case 0x23:
    case 0x26:
    case 0x27:
    case 0x28:
      return "sends the tape elsewhere or asks a question, and its blocks would be "
             "played in the wrong order";
    default:
      return "is a block this reader does not know, and its length with it";
  }
}

/* A data block's last byte may carry fewer than eight bits, but not more and
   not none. A block with no data has no last byte, and its field no meaning. */
static bool last_byte_carries_whole_bits(const uint8_t *body, uint8_t id) {
  uint32_t data_length;
  uint8_t bits;
  if (id == 0x11) {
    data_length = three_bytes_at(body, 0x0F);
    bits = body[0x0C];
  } else if (id == 0x14) {
    data_length = three_bytes_at(body, 7);
    bits = body[4];
  } else {
    return true;
  }
  return data_length == 0 || (bits > 0 && bits <= 8);
}

/* The format asks for more than one pass, but the rule that plays every other
   count plays one once, with nothing left to guess. */
static const char *refusal_for_loop(uint8_t id, const uint8_t *body, bool *in_loop) {
  if (id == 0x24) {
    if (*in_loop) {
      return "begins a loop inside another, which the format forbids";
    }
    if (two_bytes_at(body, 0) == 0) {
      return "has a loop that asks for no passes";
    }
    *in_loop = true;
  } else if (id == 0x25) {
    if (!*in_loop) {
      return "ends a loop it never began";
    }
    *in_loop = false;
  }
  return NULL;
}

static bool walk_tzx(const uint8_t *image, uint32_t length, const char **problem) {
  uint32_t at = HEADER_SIZE;
  bool in_loop = false;
  while (at < length) {
    const uint8_t id = image[at];
    const uint32_t span = block_span(image, length, at);
    if (span == 0) {
      *problem = refusal_for_block(id);
      return false;
    }
    if (span > length - at) {
      *problem = "has a block that runs off the end of the file";
      return false;
    }
    const uint8_t *body = image + at + 1;
    if (!last_byte_carries_whole_bits(body, id)) {
      *problem = "has a data block whose last byte carries no whole number of bits";
      return false;
    }
    const char *loop_problem = refusal_for_loop(id, body, &in_loop);
    if (loop_problem != NULL) {
      *problem = loop_problem;
      return false;
    }
    at += span;
  }
  if (in_loop) {
    *problem = "begins a loop it never ends";
    return false;
  }
  return true;
}

static bool walk_tap(const uint8_t *image, uint32_t length, const char **problem) {
  uint32_t at = 0;
  uint32_t blocks = 0;
  while (at + LENGTH_BYTES <= length) {
    uint32_t block = two_bytes_at(image, at);
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
  return true;
}

static uint32_t leader_still_wanted(const tzx_t *tzx);

bool tzx_open(tzx_t *tzx, const uint8_t *image, uint32_t length, uint32_t ticks_per_millisecond,
              tzx_machine machine, const char **problem) {
  const bool has_block_ids =
      length >= HEADER_SIZE && memcmp(image, signature, sizeof signature) == 0;
  if (!has_block_ids && machine == TZX_AMSTRAD) {
    *problem = "carries no .cdt signature, and a bare .tap is a Spectrum's alone";
    return false;
  }
  if (has_block_ids ? !walk_tzx(image, length, problem) : !walk_tap(image, length, problem)) {
    return false;
  }
  memset(tzx, 0, sizeof *tzx);
  tzx->image = image;
  tzx->image_length = length;
  tzx->ticks_per_millisecond = ticks_per_millisecond;
  tzx->machine = machine;
  tzx->has_block_ids = has_block_ids;
  tzx->block_at = has_block_ids ? HEADER_SIZE : 0;
  tzx->phase = PHASE_BETWEEN_BLOCKS;
  tzx->leader_ticks_left = leader_still_wanted(tzx);
  return true;
}

/* Where a data block's bytes are, and how much of its last one goes out. */
static void take_up_data(tzx_t *tzx, uint32_t data_at, uint32_t data_length, uint8_t last_bits,
                         uint32_t pause) {
  tzx->data_at = data_at;
  tzx->data_end = data_at + data_length;
  tzx->bits_in_the_last_byte = last_bits;
  tzx->bit_number = 7;
  tzx->in_second_pulse = false;
  tzx->pause_milliseconds = pause;
}

static void take_up_block(tzx_t *tzx) {
  if (tzx->block_at >= tzx->image_length) {
    tzx->phase = PHASE_FINISHED;
    return;
  }
  if (!tzx->has_block_ids) {
    const uint32_t length = two_bytes_at(tzx->image, tzx->block_at);
    const uint32_t data_at = tzx->block_at + LENGTH_BYTES;
    tzx->pilot_ticks = PILOT_TICKS;
    tzx->first_sync_ticks = FIRST_SYNC_TICKS;
    tzx->second_sync_ticks = SECOND_SYNC_TICKS;
    tzx->zero_bit_ticks = ZERO_BIT_TICKS;
    tzx->one_bit_ticks = ONE_BIT_TICKS;
    tzx->pilot_pulses_left = tzx->image[data_at] < 128 ? HEADER_PILOT_PULSES : DATA_PILOT_PULSES;
    take_up_data(tzx, data_at, length, 8, PAUSE_MILLISECONDS);
    tzx->block_at = data_at + length;
    tzx->phase = PHASE_PILOT;
    return;
  }

  const uint32_t at = tzx->block_at;
  const uint8_t id = tzx->image[at];
  const uint8_t *body = tzx->image + at + 1;
  const uint32_t body_at = at + 1;
  tzx->block_at = at + block_span(tzx->image, tzx->image_length, at);

  switch (id) {
    case 0x10: { /* the ROM's own timings, and the flag byte picks the pilot */
      const uint32_t length = two_bytes_at(body, 2);
      tzx->pilot_ticks = PILOT_TICKS;
      tzx->first_sync_ticks = FIRST_SYNC_TICKS;
      tzx->second_sync_ticks = SECOND_SYNC_TICKS;
      tzx->zero_bit_ticks = ZERO_BIT_TICKS;
      tzx->one_bit_ticks = ONE_BIT_TICKS;
      /* A block may declare no data at all, and then there is no flag
         byte to read. */
      tzx->pilot_pulses_left =
          length > 0 && body[4] < 128 ? HEADER_PILOT_PULSES : DATA_PILOT_PULSES;
      take_up_data(tzx, body_at + 4, length, 8, two_bytes_at(body, 0));
      tzx->phase = PHASE_PILOT;
      break;
    }
    case 0x11: { /* the block says what every timing is */
      tzx->pilot_ticks = two_bytes_at(body, 0);
      tzx->first_sync_ticks = two_bytes_at(body, 2);
      tzx->second_sync_ticks = two_bytes_at(body, 4);
      tzx->zero_bit_ticks = two_bytes_at(body, 6);
      tzx->one_bit_ticks = two_bytes_at(body, 8);
      tzx->pilot_pulses_left = two_bytes_at(body, 0x0A);
      take_up_data(tzx, body_at + 0x12, three_bytes_at(body, 0x0F), body[0x0C],
                   two_bytes_at(body, 0x0D));
      tzx->phase = PHASE_PILOT;
      break;
    }
    case 0x12: /* a tone of one length */
      tzx->tone_ticks = two_bytes_at(body, 0);
      tzx->tone_pulses_left = two_bytes_at(body, 2);
      tzx->phase = PHASE_TONE;
      break;
    case 0x13: /* a run of pulses, each its own length */
      tzx->sequence_pulses_left = body[0];
      tzx->sequence_at = body_at + 1;
      tzx->phase = PHASE_SEQUENCE;
      break;
    case 0x14: /* data with no pilot and no sync in front of it */
      tzx->zero_bit_ticks = two_bytes_at(body, 0);
      tzx->one_bit_ticks = two_bytes_at(body, 2);
      take_up_data(tzx, body_at + 0x0A, three_bytes_at(body, 7), body[4], two_bytes_at(body, 5));
      tzx->phase = PHASE_DATA;
      break;
    case 0x20: /* silence; asking for none stops a Spectrum, and on an
                  Amstrad is no pause at all ("Tape-Image (.CDT)") */
      tzx->pause_milliseconds = two_bytes_at(body, 0);
      if (tzx->pause_milliseconds > 0) {
        tzx->phase = PHASE_PAUSE;
      } else {
        tzx->phase = tzx->machine == TZX_AMSTRAD ? PHASE_BETWEEN_BLOCKS : PHASE_STOPPED;
      }
      break;
    case 0x24: /* the head of a loop; its first pass starts with the block behind */
      tzx->loop_at = tzx->block_at;
      tzx->loop_passes_left = two_bytes_at(body, 0);
      tzx->loop_pass_reached_the_deck = false;
      tzx->phase = PHASE_BETWEEN_BLOCKS;
      break;
    case 0x25:
      /* A pass that gave the deck nothing holds no block that plays, so every
         pass after it would give nothing too, and they are not walked: a loop
         of 65535 passes over a block of text would otherwise spend more steps
         than the walk below allows. */
      tzx->loop_passes_left--;
      if (tzx->loop_passes_left > 0 && tzx->loop_pass_reached_the_deck) {
        tzx->block_at = tzx->loop_at;
        tzx->loop_pass_reached_the_deck = false;
      }
      tzx->phase = PHASE_BETWEEN_BLOCKS;
      break;
    case 0x2A: /* "stop the tape if in 48K mode", which an Amstrad ignores */
      tzx->phase = tzx->machine == TZX_AMSTRAD ? PHASE_BETWEEN_BLOCKS : PHASE_STOPPED;
      break;
    case 0x2B: /* the level, said outright rather than turned over */
      if (four_bytes_at(body, 0) >= 1) {
        tzx->level = body[4] != 0;
      }
      tzx->phase = PHASE_BETWEEN_BLOCKS;
      break;
    default: /* the blocks that describe rather than record */
      tzx->phase = PHASE_BETWEEN_BLOCKS;
      break;
  }
}

/* The last byte of a block may carry fewer than eight bits. */
static uint8_t bits_of_this_byte(const tzx_t *tzx) {
  return tzx->data_at + 1 == tzx->data_end ? tzx->bits_in_the_last_byte : 8;
}

/* Every timing in the format is a Spectrum's T-state, so a machine with
   another clock holds each pulse proportionally longer or shorter. */
static uint32_t in_machine_ticks(const tzx_t *tzx, uint32_t ticks) {
  if (tzx->ticks_per_millisecond == TICKS_PER_MILLISECOND) {
    return ticks;
  }
  return ticks * tzx->ticks_per_millisecond / TICKS_PER_MILLISECOND;
}

/* One pulse, holding the level the tape stands at and then turning it over.
   A pulse of no T-states is an element the block has not got — rippers leave
   them in the tail of a block and real tapes carry them — so it is passed
   over without a pulse and without an edge, which is what it sounds like. */
static bool hold(tzx_t *tzx, uint32_t ticks, tape_pulse_t *pulse) {
  const uint32_t held = in_machine_ticks(tzx, ticks);
  if (held == 0) {
    return false;
  }
  pulse->level = tzx->level;
  pulse->ticks = held;
  tzx->level = !tzx->level;
  return true;
}

static bool walk_to_next_pulse(tzx_t *tzx, tape_pulse_t *pulse) {
  /* Every pass that returns no pulse either moves to a phase that will or
     takes up the next block, and both are bounded by the image: a block costs
     at least a byte of it, and a byte of data at most sixteen passes, two for
     each of its bits. A pilot and a tone are counted in pulses rather than
     bytes and so are not bounded by either, which is why one of no length is
     passed over whole rather than pulse by pulse. A loop sends the walk back
     over its blocks only after a pass that gave the deck something, and every
     pass gives its pulses at the same blocks, so between two pulses the walk
     covers the end of one pass and the start of the next, which never
     overlap: no block twice.
     The count makes all of that a thing the code holds to rather than an
     argument about it. */
  const uint64_t most_steps = 16 * (uint64_t)tzx->image_length + 64;
  for (uint64_t step = 0; step < most_steps; step++) {
    switch (tzx->phase) {
      case PHASE_BETWEEN_BLOCKS:
        take_up_block(tzx);
        break;

      case PHASE_PILOT:
        /* A pilot of no pulses and a pilot whose pulses are no length are one
           absent pilot, and counting the second out pulse by pulse would spend
           the whole walk on a tone nobody hears. The length asked of it is the
           one the machine will hold, not the one the block declares: a slow
           enough clock scales a short pulse away to nothing. */
        if (tzx->pilot_pulses_left == 0 || in_machine_ticks(tzx, tzx->pilot_ticks) == 0) {
          tzx->phase = PHASE_FIRST_SYNC;
          break;
        }
        tzx->pilot_pulses_left--;
        if (hold(tzx, tzx->pilot_ticks, pulse)) {
          return true;
        }
        break;

      case PHASE_FIRST_SYNC:
        tzx->phase = PHASE_SECOND_SYNC;
        if (hold(tzx, tzx->first_sync_ticks, pulse)) {
          return true;
        }
        break;

      case PHASE_SECOND_SYNC:
        tzx->phase = PHASE_DATA;
        if (hold(tzx, tzx->second_sync_ticks, pulse)) {
          return true;
        }
        break;

      case PHASE_DATA: {
        if (tzx->data_at >= tzx->data_end) {
          tzx->phase = tzx->pause_milliseconds == 0 ? PHASE_BETWEEN_BLOCKS : PHASE_PAUSE;
          break;
        }
        const bool one = (tzx->image[tzx->data_at] & (1u << tzx->bit_number)) != 0;
        const uint32_t ticks = one ? tzx->one_bit_ticks : tzx->zero_bit_ticks;
        if (!tzx->in_second_pulse) {
          tzx->in_second_pulse = true;
        } else {
          tzx->in_second_pulse = false;
          const uint8_t bits = bits_of_this_byte(tzx);
          if (tzx->bit_number > 8 - bits) {
            tzx->bit_number--;
          } else {
            tzx->bit_number = 7;
            tzx->data_at++;
          }
        }
        if (hold(tzx, ticks, pulse)) {
          return true;
        }
        break;
      }

      case PHASE_TONE:
        if (tzx->tone_pulses_left == 0 || in_machine_ticks(tzx, tzx->tone_ticks) == 0) {
          tzx->phase = PHASE_BETWEEN_BLOCKS;
          break;
        }
        tzx->tone_pulses_left--;
        if (hold(tzx, tzx->tone_ticks, pulse)) {
          return true;
        }
        break;

      case PHASE_SEQUENCE: {
        if (tzx->sequence_pulses_left == 0) {
          tzx->phase = PHASE_BETWEEN_BLOCKS;
          break;
        }
        const uint32_t ticks = two_bytes_at(tzx->image, tzx->sequence_at);
        tzx->sequence_at += 2;
        tzx->sequence_pulses_left--;
        if (hold(tzx, ticks, pulse)) {
          return true;
        }
        break;
      }

      case PHASE_PAUSE:
        /* A pause is low, and the format asks for a millisecond of the
           opposite level in front of it so that the block's last edge is
           finished rather than swallowed. A line already low needs none: the
           silence carries the same level on and no edge is lost. */
        tzx->phase = PHASE_SILENCE;
        if (tzx->level) {
          tzx->pause_milliseconds--;
          pulse->level = true;
          pulse->ticks = tzx->ticks_per_millisecond;
          return true;
        }
        break;

      case PHASE_SILENCE:
        tzx->phase = PHASE_BETWEEN_BLOCKS;
        /* The level is left low, so the next pulse holds low and makes no
           edge — which is what the format says a pause leaves behind. */
        tzx->level = false;
        if (tzx->pause_milliseconds == 0) {
          break;
        }
        pulse->level = false;
        pulse->ticks = tzx->pause_milliseconds * tzx->ticks_per_millisecond;
        return true;

      case PHASE_STOPPED:
      case PHASE_FINISHED:
        /* The deck lets the line fall low when there is no next pulse
           (tape.h), which swallows the edge that ends a low last pulse, and a
           loader timing edges never reads its last bit. So the other level is
           held first, for the millisecond a pause begins with. */
        if (tzx->level) {
          tzx->level = false;
          pulse->level = true;
          pulse->ticks = tzx->ticks_per_millisecond;
          return true;
        }
        if (tzx->phase == PHASE_STOPPED) {
          /* Stopped, not spent. A multiload marks the point between its parts,
             and the tape stands there until it is played again — which carries
             on from the next block, where the head is, and from a low line,
             as the format asks of a tape played "from a certain position". */
          tzx->phase = PHASE_BETWEEN_BLOCKS;
          tzx->loop_pass_reached_the_deck = true;
        }
        return false;

      default:
        return false;
    }
  }
  return false;
}

/* A pulse of the image, and a mark that the loop pass it falls in reached the
   deck. */
static bool next_pulse_of_the_image(tzx_t *tzx, tape_pulse_t *pulse) {
  if (!walk_to_next_pulse(tzx, pulse)) {
    return false;
  }
  tzx->loop_pass_reached_the_deck = true;
  return true;
}

bool tzx_next_pulse(void *reader, tape_pulse_t *pulse) {
  tzx_t *tzx = reader;
  if (tzx->leader_ticks_left > 0) {
    pulse->level = false;
    pulse->ticks = tzx->leader_ticks_left;
    tzx->leader_ticks_left = 0;
    return true;
  }
  return next_pulse_of_the_image(tzx, pulse);
}

static uint32_t leader_still_wanted(const tzx_t *tzx) {
  const uint32_t leader = LEADER_MILLISECONDS * tzx->ticks_per_millisecond;
  tzx_t ahead = *tzx;
  uint32_t silent = 0;
  tape_pulse_t pulse = {0, false};
  while (silent < leader && next_pulse_of_the_image(&ahead, &pulse) && !pulse.level) {
    silent += pulse.ticks;
  }
  return silent < leader ? leader - silent : 0;
}
