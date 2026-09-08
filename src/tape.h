/*
 * tape.h — a cassette deck.
 *
 * The deck turns the reel and nothing else. It presents one bit — the level
 * at the play head — and advances a T-state at a time; what that level means
 * is the machine's business, and what is recorded on the tape is a reader's.
 * A Spectrum reads the head on bit 6 of port &FE, a CPC on bit 7 of the
 * 8255's port B.
 *
 * A tape is played, not read. What reaches the head are the edges the bytes
 * were recorded as, and a loader measures the time between edges rather than
 * being handed a byte. Nothing here decodes one, and nothing here knows what
 * a block is: the deck asks for the next pulse and times it.
 *
 * This is the split a disc already has — the drive that turns it, and the
 * image that says what is on it. See tzx.h for the reader that answers.
 */
#ifndef COLOPHON_TAPE_H
#define COLOPHON_TAPE_H

#include <stdbool.h>
#include <stdint.h>

/* One pulse: the level the head holds, and how long it holds it. */
typedef struct {
  uint32_t ticks;
  bool level;
} tape_pulse_t;

/* Where the pulses come from. Returns false when there is no next pulse —
 * because the tape has run out, or because it is marked to stop where it
 * stands. The deck cannot tell those apart and does not try: it stops, and
 * playing again asks the reader what comes next. A pulse of no T-states is
 * the same as no pulse, since a deck that timed one would never reach the
 * one behind it. */
typedef bool (*tape_source)(void *reader, tape_pulse_t *pulse);

typedef struct {
  tape_source source;
  void *reader;
  uint32_t ticks_left;
  bool level;
  bool playing;
} tape_t;

/* An empty deck. */
void tape_init(tape_t *tape);

/* Put a tape in, stopped. The reader is the host's and must outlive it. */
void tape_insert(tape_t *tape, tape_source source, void *reader);

bool tape_loaded(const tape_t *tape);

void tape_play(tape_t *tape);
void tape_stop(tape_t *tape);
bool tape_playing(const tape_t *tape);

void tape_tick(tape_t *tape);

/* The level at the play head, low when there is no tape or it has run out. */
bool tape_level(const tape_t *tape);

#endif
