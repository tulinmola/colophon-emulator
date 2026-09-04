/*
 * drive_test — the mechanism: a motor, a head, and a disc going round.
 */
#include <string.h>

#include "drive.h"
#include "test.h"

static uint8_t image[4096];
static floppy_t floppy;
static drive_t drive;

static void a_disc_in_a_turning_drive(uint8_t heads) {
  floppy_mount(&floppy, image, sizeof image);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, sizeof image, 0x52, 0xE5));
  floppy_layout_track(&floppy, 0, 0);
  drive_init(&drive, heads);
  drive_insert(&drive, &floppy);
  drive_set_motor(&drive, true);
}

static void tick(uint32_t microseconds) {
  while (microseconds-- > 0) {
    drive_tick(&drive);
  }
}

/* READY needs a disc, a motor, and the spin-up over. */
static void ready_needs_a_disc_and_a_motor(void) {
  drive_init(&drive, 1);
  TEST_CHECK(!drive_ready(&drive));
  drive_set_motor(&drive, true);
  TEST_CHECK(!drive_ready(&drive));
  floppy_mount(&floppy, image, sizeof image);
  drive_insert(&drive, &floppy);
  TEST_CHECK(drive_ready(&drive));
  drive_set_motor(&drive, false);
  TEST_CHECK(!drive_ready(&drive));
  drive_insert(&drive, NULL);
  drive_set_motor(&drive, true);
  TEST_CHECK(!drive_ready(&drive));
}

/* A spin-up, when the host gives one, counts from MOTOR ON; the disc is
   turning the while. */
static void a_spin_up_delays_ready(void) {
  drive_init(&drive, 1);
  floppy_mount(&floppy, image, sizeof image);
  drive_insert(&drive, &floppy);
  drive.spin_up = 1000;
  drive_set_motor(&drive, true);
  TEST_CHECK(!drive_ready(&drive));
  tick(999);
  TEST_CHECK(!drive_ready(&drive));
  tick(1);
  TEST_CHECK(drive_ready(&drive));
  drive_set_motor(&drive, true); /* already on: no new spin-up */
  TEST_CHECK(drive_ready(&drive));
  drive_set_motor(&drive, false);
  drive_set_motor(&drive, true);
  TEST_CHECK(!drive_ready(&drive));
}

/* One byte every 32µs, 6250 to a revolution, the index for one microsecond
   as it comes round, and a count of revolutions. */
static void the_disc_turns_at_300_rpm(void) {
  a_disc_in_a_turning_drive(1);
  TEST_EQUAL(drive.position, 0);
  tick(31);
  TEST_EQUAL(drive.position, 0);
  TEST_CHECK(!drive.byte_passed);
  tick(1);
  TEST_EQUAL(drive.position, 1);
  TEST_CHECK(drive.byte_passed);
  TEST_CHECK(!drive_index(&drive));
  tick(1);
  TEST_CHECK(!drive.byte_passed);
  tick(6249 * 32 - 2);
  TEST_EQUAL(drive.position, 6249);
  TEST_EQUAL(drive.revolutions, 0);
  tick(1);
  TEST_EQUAL(drive.position, 0);
  TEST_EQUAL(drive.revolutions, 1);
  TEST_CHECK(drive_index(&drive));
  tick(1);
  TEST_CHECK(!drive_index(&drive));
  /* 200ms a turn. */
  tick(200000 - 2);
  TEST_EQUAL(drive.revolutions, 1);
  tick(1);
  TEST_EQUAL(drive.revolutions, 2);
  /* A track laid out longer takes longer to come round. */
  floppy_set_track_length(&floppy, 0, 0, 7000);
  tick(7000 * 32);
  TEST_EQUAL(drive.revolutions, 3);
  TEST_EQUAL(drive.position, 0);
}

/* A head stepped onto a track laid out shorter than where it stood has
   not passed the index; it is the same way round the disc, in fewer
   bytes. */
static void a_step_onto_a_shorter_track_passes_no_index(void) {
  a_disc_in_a_turning_drive(1);
  TEST_CHECK(floppy_add_track(&floppy, 1, 0, 0, sizeof image, 0x52, 0xE5));
  floppy_layout_track(&floppy, 1, 0);
  floppy_set_track_length(&floppy, 0, 0, 7000);
  tick(6500 * 32);
  TEST_EQUAL(drive.position, 6500);
  drive_step(&drive, true);
  TEST_EQUAL(drive.cylinder, 1);
  TEST_EQUAL(drive.position, 6500 * 6250 / 7000);
  TEST_EQUAL(drive.revolutions, 0);
  tick(32);
  TEST_EQUAL(drive.position, 6500 * 6250 / 7000 + 1);
  TEST_EQUAL(drive.revolutions, 0);
}

static void a_stopped_motor_stops_the_disc(void) {
  a_disc_in_a_turning_drive(1);
  tick(100 * 32);
  TEST_EQUAL(drive.position, 100);
  drive_set_motor(&drive, false);
  tick(100 * 32);
  TEST_EQUAL(drive.position, 100);
  TEST_CHECK(!drive.byte_passed);
}

/* The head steps between zero and the medium's width, and no further. */
static void the_head_steps_within_the_medium(void) {
  drive_init(&drive, 1);
  TEST_CHECK(drive_track_zero(&drive));
  drive_step(&drive, false);
  TEST_EQUAL(drive.cylinder, 0);
  drive_step(&drive, true);
  TEST_EQUAL(drive.cylinder, 1);
  TEST_CHECK(!drive_track_zero(&drive));
  for (int step = 0; step < 200; step++) {
    drive_step(&drive, true);
  }
  TEST_EQUAL(drive.cylinder, FLOPPY_MAX_CYLINDERS - 1);
  drive_step(&drive, false);
  TEST_EQUAL(drive.cylinder, FLOPPY_MAX_CYLINDERS - 2);
}

/* SIDE SELECT means nothing to a drive with one head. */
static void side_select_needs_a_second_head(void) {
  drive_init(&drive, 1);
  drive_select_side(&drive, true);
  TEST_CHECK(!drive.side);
  TEST_CHECK(!drive_two_sided(&drive));
  drive_init(&drive, 2);
  drive_select_side(&drive, true);
  TEST_CHECK(drive.side);
  TEST_CHECK(drive_two_sided(&drive));
  drive_select_side(&drive, false);
  TEST_CHECK(!drive.side);
}

static void write_protect_is_the_discs(void) {
  drive_init(&drive, 1);
  TEST_CHECK(!drive_write_protected(&drive));
  floppy_mount(&floppy, image, sizeof image);
  drive_insert(&drive, &floppy);
  TEST_CHECK(!drive_write_protected(&drive));
  floppy.write_protected = true;
  TEST_CHECK(drive_write_protected(&drive));
}

int main(void) {
  TEST_RUN(ready_needs_a_disc_and_a_motor);
  TEST_RUN(a_spin_up_delays_ready);
  TEST_RUN(the_disc_turns_at_300_rpm);
  TEST_RUN(a_step_onto_a_shorter_track_passes_no_index);
  TEST_RUN(a_stopped_motor_stops_the_disc);
  TEST_RUN(the_head_steps_within_the_medium);
  TEST_RUN(side_select_needs_a_second_head);
  TEST_RUN(write_protect_is_the_discs);
  return TEST_REPORT("drive");
}
