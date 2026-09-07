/*
 * tzx.h — a tape image, read into pulses.
 *
 * One reader, two formats, two machines. A .tap is bare blocks of bytes,
 * replayed at the ROM's own timings, so it carries only what the ROM loader
 * can read, and only a Spectrum's. A .tzx records the timings themselves,
 * block by block, which is what a tape that brought a loader of its own
 * needs — and it is the CPC's .cdt under another name, so one reader serves
 * both machines. They are told apart by the signature a .tzx opens with and
 * a .tap has not got.
 *
 * What comes out is pulses, which is all a deck wants. Nothing here turns
 * the reel or knows what a level means.
 *
 * Every timing in the format is a Spectrum's T-state, whatever machine the
 * tape was made for, so a CPC's are scaled on the way out: its clock is 4MHz
 * where the format's is 3.5. Two blocks also mean different things on the
 * two machines, which is why the reader is told which one it is feeding: a
 * pause asking for none stops a Spectrum where a CPC reads it as no pause
 * at all, and the block that stops a 48K is one a CPC must ignore.
 *
 * Not every block can be played. The ones that record a waveform sample by
 * sample or build their data from a table of symbols, and the ones that send
 * the tape backwards — jumps, loops, calls and menus — are refused when the
 * image is opened rather than played wrongly, because a tape whose blocks are
 * visited out of order is a tape this reader would silently get wrong. So is
 * a block whose identifying byte this reader does not know. Blocks added
 * after version 1.10 carry their own length in the four bytes behind the ID,
 * which is what lets an older reader step over a newer block — but the older
 * blocks have their lengths in the specification's table and nowhere else, and
 * an unknown byte says nothing about which kind it is. So it is refused, and
 * everything behind it is not read as something else. The blocks that only describe a tape — its
 * title, who archived it, what hardware it wants — are stepped over by the length the specification
 * gives them, and say nothing to the deck.
 *
 * A tape can also stop before it ends. A Spectrum's multiload marks the point
 * between its parts, and there the reader says there is no next pulse; played
 * again, it carries on from the block behind the mark, which is where a real
 * deck's head would be standing.
 *
 * Sources:
 * - "Tape-Image (.CDT) file format" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/cdtcpc.html — the CPC's half of the
 *   format: "All timings are in Spectrum T-States"; a pause block asking for
 *   none "SHOULD NOT 'Stop the Tape'" but is "treated as 'no pause'"; and
 *   the block that stops a 48K Spectrum, which "Amstrad emulator's MUST
 *   ignore ... and it MUST NOT have any effect".
 * - "TZX format" v1.13 (Tomaz Kac, maintained by Martijn van der Heide),
 *   https://worldofspectrum.net/TZXformat.html — the signature and version
 *   bytes, every block's length so an unplayable one can be stepped over,
 *   the standard ROM timings in block ID 11's curly brackets, and that the
 *   level starts low and each pulse turns it over.
 * - "Emulator file formats" (World of Spectrum FAQ),
 *   https://worldofspectrum.org/faq/reference/formats.htm — a .tap is
 *   blocks, each opening with "two bytes specifying how many bytes will
 *   follow", then "raw tape data ... including the flag and checksum bytes".
 */
#ifndef COLOPHON_TZX_H
#define COLOPHON_TZX_H

#include <stdbool.h>
#include <stdint.h>

#include "tape.h"

/* Which machine's rules the image is read under. The format is one, and two
   of its blocks are not. */
typedef enum {
  TZX_SPECTRUM,
  TZX_AMSTRAD,
} tzx_machine;

typedef struct {
  const uint8_t *image; /* the host's bytes, which must outlive the reader */
  uint32_t image_length;
  uint32_t ticks_per_millisecond;
  tzx_machine machine;
  bool has_block_ids; /* a .tzx names every block; a .tap is bare bytes */

  uint32_t block_at;
  uint8_t phase;
  bool level;

  /* what the block in hand asks for */
  uint32_t pilot_ticks;
  uint32_t first_sync_ticks;
  uint32_t second_sync_ticks;
  uint32_t zero_bit_ticks;
  uint32_t one_bit_ticks;
  uint32_t pilot_pulses_left;
  uint32_t pause_milliseconds;
  uint32_t data_at;
  uint32_t data_end;
  uint8_t bits_in_the_last_byte;
  uint8_t bit_number;
  bool in_second_pulse; /* a bit is two pulses of one length */

  uint32_t tone_ticks;
  uint32_t tone_pulses_left;
  uint32_t sequence_at;
  uint32_t sequence_pulses_left;
} tzx_t;

/* Take up an image. Returns false having pointed `problem` at a sentence
 * saying what is wrong with the bytes or which block cannot be played. */
bool tzx_open(tzx_t *tzx, const uint8_t *image, uint32_t length, uint32_t ticks_per_millisecond,
              tzx_machine machine, const char **problem);

/* A tape_source: the next pulse, or false at the end of the tape and where a
 * block marks it to stop. Playing on past a stop carries on from the next
 * block; past the end there is nothing to carry on to. */
bool tzx_next_pulse(void *reader, tape_pulse_t *pulse);

#endif
