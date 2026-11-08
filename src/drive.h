/*
 * drive.h — a floppy disc drive: the mechanism between a controller and a
 * disc.
 *
 * The drive owns what neither the medium nor the controller does: whether
 * the motor turns, which cylinder the head stands over, which side it
 * reads, and where on the track the disc has turned to. It answers the
 * Shugart lines a controller reads — READY, TRACK 0, WRITE PROTECT, INDEX,
 * TWO SIDE — and takes the ones a controller and a machine drive: STEP and
 * DIRECTION, SIDE SELECT, MOTOR ON. On the boards that fit this family the
 * motor line comes from the machine and not from the controller, which is
 * why the drive is not inside it.
 *
 * Time here is the disc's: it turns at 300 rpm whenever the motor is on,
 * and one byte passes the head every 32µs. A tick is one microsecond.
 *
 * READY waits for the disc to turn. Amstrad's service manual says of the
 * 6128's drive that "It is normally not ready until 2 revolutions are made
 * after the disk insertion". Two revolutions are taken here as three index
 * pulses, the first and the two that close a revolution each, and the rule
 * as holding after MOTOR ON as after insertion: READY rises at the third
 * index pulse after either — 400 to 600ms on, as the disc stands, and
 * always 600 after a disc goes in, since one goes in at the index — and
 * falls the moment the motor stops. Kevin Thacker's drive test, timed from
 * MOTOR ON on a 6128's own 3" drive, rules out the second pulse: its longest
 * wait was 2564 polls of Sense Drive Status, and a poll there is no shorter
 * than the roughly 213µs it takes here, where the chip answers each command
 * byte at once (upd765.h), so that wait was at least 0.55s, past the 0.40
 * the second pulse can reach. Its whole range, 1735 to 2564 polls, falls
 * within the third pulse's 0.40 to 0.60s if a poll there took 230 to 234µs.
 * It found READY gone at the first poll after the motor stopped. The disc
 * turns at full speed from the moment the motor starts: how it comes up to
 * speed is not modelled, and READY is all a controller sees of it.
 *
 * What is not modelled besides: a drive that holds READY after its motor
 * stops, which no measurement shows; and the mechanical stop a head hits
 * past its last cylinder, so the head steps as far as the medium is wide
 * and finds unformatted tracks there, which is what a head past the disc's
 * last cylinder finds.
 *
 * Sources:
 * - "Floppy disc controller and Floppy disc drives" (Kevin Thacker's
 *   cpctech), https://cpctech.cpcwiki.de/docs/fdc.html — the Shugart lines
 *   a drive must answer, and the motor driven from the machine's own port.
 * - The CPC6128 service manual (Amstrad, 1985),
 *   https://archive.org/details/Amstrad_CPC6128_Service_Manual_1985_Amstrad_Consumer_Electronics_a
 *   — the drive's READY after two revolutions, and an "Index period
 *   200±5msec".
 * - Kevin Thacker's drive test, from Arnold's test programs (mirrored by
 *   Andre Weissflog),
 *   https://github.com/floooh/emu-info/blob/93d132f/tests/cpc/acid/disc/drivetest/drivetest.asm
 *   — its notes for a 3" drive on a type 1 6128, "on: 6c7, a04,93f" and
 *   "off: 0,0,0": the least, most and average polls of Sense Drive Status
 *   before READY rose after MOTOR ON, and before it fell after MOTOR OFF.
 *   The 6128 Plus's drive beside it, "3f5, 74e, 4a7", rises sooner, nearer
 *   the second pulse than the third, so drives differ. The poll's length
 *   here is from the same program: a READY held back a fixed 300ms gives
 *   1408 polls. Run here by hand, in a run no tier repeats, it counts 2048
 *   to 2305, 2116 on average.
 * - µPD765A/µPD765B datasheet (NEC), mirrored at
 *   https://cpctech.cpcwiki.de/docs/upd765a/necfdc.htm — the drive-side
 *   signals the controller reads and what it makes of them.
 */
#ifndef COLOPHON_DRIVE_H
#define COLOPHON_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "floppy.h"

/* Two revolutions, from the first index pulse to the third (above). */
#define DRIVE_INDEX_PULSES_TO_READY 3

typedef struct {
  floppy_t *floppy; /* the disc in the drive, or NULL for none */
  uint8_t heads;    /* one or two; SIDE SELECT means nothing to a one */

  bool motor;       /* MOTOR ON, as the machine drives it */
  bool side;        /* SIDE SELECT, as the controller drives it */
  uint8_t cylinder; /* where the head stands; TRACK 0 when it is zero */

  uint32_t position;    /* the byte under the head, counted from the index */
  uint32_t revolutions; /* index pulses since the disc went in */
  uint32_t microseconds_into_byte;
  bool byte_passed; /* a byte went by this microsecond */

  uint8_t index_pulses_seen; /* since MOTOR ON or the disc went in, up to READY */
} drive_t;

/* A drive with the given number of heads and nothing in it. */
void drive_init(drive_t *drive, uint8_t heads);

/* Put a disc in, or take it out with NULL. The disc is borrowed. */
void drive_insert(drive_t *drive, floppy_t *floppy);

/* MOTOR ON, from the machine. Turning it on starts the count to READY
 * again. */
void drive_set_motor(drive_t *drive, bool on);

/* SIDE SELECT, from the controller. A one-headed drive ignores it. */
void drive_select_side(drive_t *drive, bool side);

/* One STEP pulse, inward toward the higher cylinders or outward toward
 * zero. The head cannot step past zero or past the medium's width. */
void drive_step(drive_t *drive, bool inward);

/* One microsecond of the disc turning. */
void drive_tick(drive_t *drive);

/* The lines a controller reads. READY needs a disc, the motor on, and the
 * third index pulse since either. INDEX is true for the microsecond the
 * index passes. */
bool drive_ready(const drive_t *drive);
bool drive_track_zero(const drive_t *drive);
bool drive_write_protected(const drive_t *drive);
bool drive_two_sided(const drive_t *drive);
bool drive_index(const drive_t *drive);

#endif
