/*
 * tape.h — a cassette deck.
 *
 * The deck presents one bit: the level at the play head. It advances a
 * T-state at a time and knows nothing of what that level means, which is the
 * machine's business — a Spectrum reads it on bit 6 of port &FE, a CPC on
 * bit 7 of the 8255's port B.
 *
 * A tape is played, not read. The image holds bytes; what reaches the head
 * are the edges those bytes were recorded as, and a loader measures the time
 * between edges rather than being handed a byte. Nothing here decodes one.
 *
 * Where a disc is three files — the image, the medium, and the drive that
 * turns it — a tape is one. The blocks are read here, the pulses are made
 * here, and the reel turns here, because a .tap has no geometry to lay out
 * and there is nothing between the reel and the socket to model.
 *
 * Only .tap is understood, which is bytes at the ROM's own timings and so
 * loads only what the ROM loader loads. A tape that brought a loader of its
 * own wants a .tzx, which records the timings themselves; that format will
 * want the reading split off from the deck.
 *
 * The pulse timings below are in T-states of the machine that recorded the
 * tape, so the deck plays the numbers it is given and never converts. Only
 * the pause between blocks is a duration, which is why the deck is told the
 * machine's clock and nothing else about it.
 *
 * Sources:
 * - "Emulator file formats" (World of Spectrum FAQ),
 *   https://worldofspectrum.org/faq/reference/formats.htm — a .tap is
 *   blocks, each opening with "two bytes specifying how many bytes will
 *   follow", then "raw tape data ... including the flag and checksum bytes".
 * - "TZX format" v1.13 (Tomaz Kac, maintained by Martijn van der Heide),
 *   https://worldofspectrum.net/TZXformat.html — the standard ROM timings a
 *   .tap is replayed at, tabulated in block ID 11; that a header's pilot is
 *   8063 pulses and a data block's 3223, told apart by the flag byte; that
 *   the level starts low and every pulse turns it over; and that the format
 *   also serves "the Amstrad CPC", which is where .cdt comes from.
 */
#ifndef COLOPHON_TAPE_H
#define COLOPHON_TAPE_H

#include <stdbool.h>
#include <stdint.h>

#define TAPE_PILOT_TICKS 2168
#define TAPE_FIRST_SYNC_TICKS 667
#define TAPE_SECOND_SYNC_TICKS 735
#define TAPE_ZERO_BIT_TICKS 855
#define TAPE_ONE_BIT_TICKS 1710
#define TAPE_HEADER_PILOT_PULSES 8063
#define TAPE_DATA_PILOT_PULSES 3223
#define TAPE_PAUSE_MILLISECONDS 1000

typedef struct {
  const uint8_t *image; /* the host's bytes, which must outlive the deck */
  uint32_t image_length;
  uint32_t ticks_per_millisecond;

  uint32_t block_at;
  uint32_t byte_at;
  uint32_t data_end; /* one past the playing block's last byte */
  uint32_t pilot_pulses_left;
  uint32_t pulse_ticks_left;
  uint8_t phase; /* which part of a block is playing */
  uint8_t bit_number;
  bool in_second_pulse; /* a bit is two pulses of one length */
  bool level;
  bool playing;
} tape_t;

/* An empty deck, told how many T-states its machine spends in a
 * millisecond, which is all it ever learns about one. */
void tape_init(tape_t *tape, uint32_t ticks_per_millisecond);

/* Put a tape in, rewound and stopped. Returns false having pointed `problem`
 * at a sentence saying what is wrong with the bytes, and leaves whatever was
 * in the deck where it was. */
bool tape_insert(tape_t *tape, const uint8_t *image, uint32_t length, const char **problem);

bool tape_loaded(const tape_t *tape);

void tape_play(tape_t *tape);
void tape_stop(tape_t *tape);
bool tape_playing(const tape_t *tape);

void tape_tick(tape_t *tape);

/* The level at the play head, low when there is no tape or it has run out. */
bool tape_level(const tape_t *tape);

#endif
