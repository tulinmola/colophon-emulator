/*
 * drive.c — the mechanism: motor, head and the turning disc.
 */
#include "drive.h"

void drive_init(drive_t *drive, uint8_t heads) {
  *drive = (drive_t){0};
  drive->heads = heads;
}

void drive_insert(drive_t *drive, floppy_t *floppy) {
  drive->floppy = floppy;
  drive->position = 0;
  drive->revolutions = 0;
  drive->microseconds_into_byte = 0;
  drive->byte_passed = false;
}

void drive_set_motor(drive_t *drive, bool on) {
  if (on && !drive->motor) {
    drive->spinning_up = drive->spin_up;
  }
  if (!on) {
    drive->byte_passed = false;
  }
  drive->motor = on;
}

void drive_select_side(drive_t *drive, bool side) { drive->side = drive->heads > 1 && side; }

/* The position is an angle: the same fraction of a turn on the new track,
   which may be laid out in more bytes or fewer. */
void drive_step(drive_t *drive, bool inward) {
  uint8_t from = drive->cylinder;
  if (inward) {
    if (drive->cylinder < FLOPPY_MAX_CYLINDERS - 1) {
      drive->cylinder++;
    }
  } else if (drive->cylinder > 0) {
    drive->cylinder--;
  }
  if (drive->floppy != NULL && drive->cylinder != from) {
    uint64_t was = floppy_track_length(drive->floppy, from, drive->side);
    uint64_t now = floppy_track_length(drive->floppy, drive->cylinder, drive->side);
    drive->position = (uint32_t)((uint64_t)drive->position * now / was);
  }
}

void drive_tick(drive_t *drive) {
  drive->byte_passed = false;
  if (!drive->motor || drive->floppy == NULL) {
    return;
  }
  if (drive->spinning_up > 0) {
    drive->spinning_up--;
  }
  if (++drive->microseconds_into_byte < FLOPPY_MICROSECONDS_PER_BYTE) {
    return;
  }
  drive->microseconds_into_byte = 0;
  drive->byte_passed = true;
  drive->position++;
  if (drive->position >= floppy_track_length(drive->floppy, drive->cylinder, drive->side)) {
    drive->position = 0;
    drive->revolutions++;
  }
}

bool drive_ready(const drive_t *drive) {
  return drive->floppy != NULL && drive->motor && drive->spinning_up == 0;
}

bool drive_track_zero(const drive_t *drive) { return drive->cylinder == 0; }

bool drive_write_protected(const drive_t *drive) {
  return drive->floppy != NULL && drive->floppy->write_protected;
}

bool drive_two_sided(const drive_t *drive) { return drive->heads > 1; }

bool drive_index(const drive_t *drive) { return drive->byte_passed && drive->position == 0; }
