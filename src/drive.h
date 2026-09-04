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
 * and one byte passes the head every 32µs. A tick is one microsecond. A
 * spin-up, where the host gives one, delays READY and not the turning.
 *
 * What is not modelled: how long a motor takes to reach speed. No source
 * measures it for these drives — the operating system waits a full second
 * and the interface documentation says only that "there is no defined
 * minimum or maximum time" — so it is a number the host may set and is
 * zero until someone measures one. Nor the mechanical stop a head hits
 * past its last cylinder: the head steps as far as the medium is wide, and
 * finds unformatted tracks there, which is what a head past the disc's
 * last cylinder finds.
 *
 * Sources:
 * - "Floppy disc controller and Floppy disc drives" (Kevin Thacker's
 *   cpctech), https://cpctech.cpcwiki.de/docs/fdc.html — the Shugart lines
 *   a drive must answer, the motor driven from the machine's own port, and
 *   the spin-up time nobody specifies.
 * - µPD765A/µPD765B datasheet (NEC), mirrored at
 *   https://cpctech.cpcwiki.de/docs/upd765a/necfdc.htm — the drive-side
 *   signals the controller reads and what it makes of them.
 */
#ifndef COLOPHON_DRIVE_H
#define COLOPHON_DRIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "floppy.h"

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

  uint32_t spin_up;     /* microseconds from MOTOR ON to speed; unmeasured */
  uint32_t spinning_up; /* microseconds still to go */
} drive_t;

/* A drive with the given number of heads and nothing in it. */
void drive_init(drive_t *drive, uint8_t heads);

/* Put a disc in, or take it out with NULL. The disc is borrowed. */
void drive_insert(drive_t *drive, floppy_t *floppy);

/* MOTOR ON, from the machine. Turning it on starts the spin-up. */
void drive_set_motor(drive_t *drive, bool on);

/* SIDE SELECT, from the controller. A one-headed drive ignores it. */
void drive_select_side(drive_t *drive, bool side);

/* One STEP pulse, inward toward the higher cylinders or outward toward
 * zero. The head cannot step past zero or past the medium's width. */
void drive_step(drive_t *drive, bool inward);

/* One microsecond of the disc turning. */
void drive_tick(drive_t *drive);

/* The lines a controller reads. READY needs a disc, the motor on, and the
 * spin-up over. INDEX is true for the microsecond the index passes. */
bool drive_ready(const drive_t *drive);
bool drive_track_zero(const drive_t *drive);
bool drive_write_protected(const drive_t *drive);
bool drive_two_sided(const drive_t *drive);
bool drive_index(const drive_t *drive);

#endif
