/*
 * floppy_test — disc images built byte by byte, then read back.
 *
 * Every image here is assembled in this file, so the tests are hermetic and
 * so a malformed one can be built deliberately. That second part is the
 * point: an image is somebody else's file, and the reader has to survive
 * one that lies about its own shape. The tests that matter most are the
 * ones where the bytes are wrong.
 */
#include <string.h>

#include "dsk.h"
#include "test.h"

static uint8_t image[64 * 1024];
static size_t image_length;
static floppy_t floppy;
static const char *problem;

/* An image under construction: the disc header, then tracks appended one at
   a time, each with its sectors. Lengths are filled in as they are known. */
static size_t track_at; /* where the track being built begins */
static size_t track_count;

/* An image's signature is a fixed-width field rather than a string: the
   bytes go in without the terminator the literal carries. */
static void write_signature(const char *signature, size_t length) {
  memcpy(image, signature, length);
}

static void begin_image(bool extended, uint8_t cylinders, uint8_t sides) {
  memset(image, 0, sizeof image);
  write_signature(extended ? "EXTENDED CPC DSK File\r\nDisk-Info\r\n"
                           : "MV - CPCEMU Disk-File\r\nDisk-Info\r\n",
                  34);
  image[0x30] = cylinders;
  image[0x31] = sides;
  image_length = 256;
  track_at = 0;
  track_count = 0;
}

/* The original format's one length for every track. */
static void set_uniform_track_length(size_t length) {
  image[0x32] = (uint8_t)(length & 0xFF);
  image[0x33] = (uint8_t)(length >> 8);
}

static void begin_track(uint8_t cylinder, uint8_t side, uint8_t size_code) {
  track_at = image_length;
  memcpy(image + track_at, "Track-Info\r\n", 12);
  image[track_at + 0x10] = cylinder;
  image[track_at + 0x11] = side;
  image[track_at + 0x14] = size_code;
  image[track_at + 0x15] = 0;
  image_length = track_at + 256;
}

/* Append a sector: its identity in the track header, its bytes after it.
   `stored` is how much room it takes, which is what lets a test build a
   sector shorter or longer than the size code claims. */
static void add_sector(uint8_t c, uint8_t h, uint8_t r, uint8_t n, uint8_t status1, uint8_t status2,
                       size_t stored, uint8_t fill) {
  uint8_t index = image[track_at + 0x15]++;
  uint8_t *entry = image + track_at + 0x18 + (size_t)index * 8;
  entry[0] = c;
  entry[1] = h;
  entry[2] = r;
  entry[3] = n;
  entry[4] = status1;
  entry[5] = status2;
  entry[6] = (uint8_t)(stored & 0xFF);
  entry[7] = (uint8_t)(stored >> 8);
  memset(image + image_length, fill, stored);
  image_length += stored;
}

/* Close the track off, recording its length where the format keeps it. An
   extended image stores only the high byte of that length, so a track is
   always a whole number of 256-byte units and the tail is padding. */
static void end_track(bool extended) {
  size_t length = ((image_length - track_at) + 255) / 256 * 256;
  image_length = track_at + length;
  if (extended) {
    image[0x34 + track_count] = (uint8_t)(length / 256);
  }
  track_count++;
}

static bool read_it(void) { return dsk_read(&floppy, image, image_length, &problem); }

/* Append the block that says where the sectors lay, as SAMdisk writes it:
   for every cylinder and side a track length and then each sector's
   identity mark, counted in bytes from the index. */
static void add_offsets(const uint16_t *words, size_t count) {
  memcpy(image + image_length, "Offset-Info\r\n", 13);
  image[image_length + 13] = 0;
  image[image_length + 14] = 0;
  image_length += 15;
  for (size_t index = 0; index < count; index++) {
    image[image_length++] = (uint8_t)(words[index] & 0xFF);
    image[image_length++] = (uint8_t)(words[index] >> 8);
  }
}

/* A plain 2-track, 2-sector-a-track extended image, the shape of the tests
   that are not about malformed bytes. */
static void build_plain_extended(void) {
  begin_image(true, 2, 1);
  for (uint8_t cylinder = 0; cylinder < 2; cylinder++) {
    begin_track(cylinder, 0, 2);
    add_sector(cylinder, 0, 0xC1, 2, 0, 0, 512, (uint8_t)(0x10 + cylinder));
    add_sector(cylinder, 0, 0xC2, 2, 0, 0, 512, (uint8_t)(0x20 + cylinder));
    end_track(true);
  }
}

static void an_extended_image_reads_back_its_shape(void) {
  build_plain_extended();
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy.cylinders, 2);
  TEST_EQUAL(floppy.sides, 1);
  TEST_CHECK(floppy_track_formatted(&floppy, 0, 0));
  TEST_CHECK(floppy_track_formatted(&floppy, 1, 0));
  TEST_CHECK(!floppy_track_formatted(&floppy, 2, 0));
  TEST_EQUAL(floppy_sector_count(&floppy, 1, 0), 2);

  const floppy_sector_t *sector = floppy_sector(&floppy, 1, 0, 1);
  TEST_CHECK(sector != NULL);
  TEST_EQUAL(sector->c, 1);
  TEST_EQUAL(sector->r, 0xC2);
  TEST_EQUAL(sector->n, 2);
  TEST_EQUAL(sector->announced, 512);
  TEST_EQUAL(sector->recorded, 512);
  TEST_EQUAL(sector->copies, 1);

  uint8_t bytes[512];
  TEST_EQUAL(floppy_read(&floppy, 1, 0, 1, 0, 0, bytes, sizeof bytes), 512);
  TEST_EQUAL(bytes[0], 0x21);
  TEST_EQUAL(bytes[511], 0x21);
  TEST_CHECK(floppy_sector(&floppy, 1, 0, 2) == NULL);
}

static void the_original_format_allots_every_sector_the_same_room(void) {
  begin_image(false, 1, 1);
  set_uniform_track_length(256 + (size_t)512 * 2);
  begin_track(0, 0, 2);
  /* The second sector's identity claims 256 bytes, but the original format
     still allots it the track's 512 and pads the rest. */
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0xAA);
  add_sector(0, 0, 0xC2, 1, 0, 0, 512, 0xBB);
  end_track(false);
  TEST_CHECK(read_it());

  const floppy_sector_t *first = floppy_sector(&floppy, 0, 0, 0);
  const floppy_sector_t *second = floppy_sector(&floppy, 0, 0, 1);
  TEST_EQUAL(first->recorded, 512);
  TEST_EQUAL(second->announced, 256);
  TEST_EQUAL(second->recorded, 256); /* not the 512 it was allotted */

  uint8_t bytes[512];
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 1, 0, 0, bytes, sizeof bytes), 256);
  TEST_EQUAL(bytes[0], 0xBB);
}

static void an_unformatted_track_takes_up_no_room(void) {
  begin_image(true, 3, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x11);
  end_track(true);
  image[0x34 + 1] = 0; /* cylinder 1 was never formatted */
  track_count++;
  begin_track(2, 0, 2);
  add_sector(2, 0, 0xC1, 2, 0, 0, 512, 0x33);
  end_track(true);

  TEST_CHECK(read_it());
  TEST_CHECK(floppy_track_formatted(&floppy, 0, 0));
  TEST_CHECK(!floppy_track_formatted(&floppy, 1, 0));
  TEST_CHECK(floppy_track_formatted(&floppy, 2, 0));
  TEST_EQUAL(floppy_sector_count(&floppy, 1, 0), 0);

  /* The track after the gap still lands on its own bytes. */
  uint8_t bytes[512];
  TEST_EQUAL(floppy_read(&floppy, 2, 0, 0, 0, 0, bytes, sizeof bytes), 512);
  TEST_EQUAL(bytes[0], 0x33);
}

/* A stored length that is an exact multiple of the announced one holds that
   many readings of a data field that read differently each time. */
static void an_unstable_sector_keeps_every_reading(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, (size_t)512 * 3, 0);
  end_track(true);
  for (int copy = 0; copy < 3; copy++) {
    memset(image + 256 + 256 + (size_t)copy * 512, (uint8_t)(0xE0 + copy), 512);
  }
  TEST_CHECK(read_it());

  const floppy_sector_t *sector = floppy_sector(&floppy, 0, 0, 0);
  TEST_EQUAL(sector->announced, 512);
  TEST_EQUAL(sector->recorded, 512);
  TEST_EQUAL(sector->copies, 3);

  uint8_t bytes[512];
  for (uint32_t revolution = 0; revolution < 7; revolution++) {
    TEST_EQUAL(floppy_read(&floppy, 0, 0, 0, revolution, 0, bytes, sizeof bytes), 512);
    TEST_EQUAL(bytes[0], 0xE0 + revolution % 3);
  }
}

/* Stored longer than announced but not a multiple: one reading that runs on
   past its own end, which is gap data and not another copy. */
static void a_sector_running_into_the_gap_is_one_reading(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, (size_t)512 + 88, 0x5A);
  end_track(true);
  TEST_CHECK(read_it());

  const floppy_sector_t *sector = floppy_sector(&floppy, 0, 0, 0);
  TEST_EQUAL(sector->copies, 1);
  TEST_EQUAL(sector->recorded, 512 + 88);
  TEST_EQUAL(sector->announced, 512);
}

/* Shorter than announced is normal, not corrupt: it is how a protection
   hides data from anyone reading the size code and believing it. */
static void a_short_sector_reads_short(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 6);
  add_sector(0, 0, 0xC1, 6, 0, 0, 0x1800, 0x77); /* 8K claimed, 6K recorded */
  end_track(true);
  TEST_CHECK(read_it());

  const floppy_sector_t *sector = floppy_sector(&floppy, 0, 0, 0);
  TEST_EQUAL(sector->announced, 8192);
  TEST_EQUAL(sector->recorded, 0x1800);
  TEST_EQUAL(sector->copies, 1);

  static uint8_t bytes[8192];
  memset(bytes, 0xEE, sizeof bytes);
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 0, 0, 0, bytes, sizeof bytes), 0x1800);
  TEST_EQUAL(bytes[0x17FF], 0x77);
  TEST_EQUAL(bytes[0x1800], 0xEE); /* past the recording, left untouched */
}

static void a_track_may_announce_one_number_twice(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x01);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x02);
  end_track(true);
  TEST_CHECK(read_it());

  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), 2);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->r, 0xC1);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->r, 0xC1);
  uint8_t bytes[512];
  floppy_read(&floppy, 0, 0, 0, 0, 0, bytes, sizeof bytes);
  TEST_EQUAL(bytes[0], 0x01);
  floppy_read(&floppy, 0, 0, 1, 0, 0, bytes, sizeof bytes);
  TEST_EQUAL(bytes[0], 0x02);
}

/* An identity may disagree with the track it lies on, and the disagreement
   is the record: it is kept, not corrected. */
static void an_identity_that_lies_is_kept(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0x41, 1, 0xC1, 2, 0, 0, 512, 0x01);
  end_track(true);
  TEST_CHECK(read_it());

  const floppy_sector_t *sector = floppy_sector(&floppy, 0, 0, 0);
  TEST_EQUAL(sector->c, 0x41); /* the track is cylinder 0 */
  TEST_EQUAL(sector->h, 1);    /* on side 0 */
}

static void recorded_status_becomes_what_was_found(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0x00, 0x40, 512, 0); /* control mark: deleted */
  add_sector(0, 0, 0xC2, 2, 0x20, 0x20, 512, 0); /* data error, in the data field */
  add_sector(0, 0, 0xC3, 2, 0x20, 0x00, 512, 0); /* a check failed elsewhere: the identity */
  add_sector(0, 0, 0xC4, 2, 0x04, 0x01, 512, 0); /* no data, and no mark for it */
  end_track(true);
  TEST_CHECK(read_it());

  const floppy_sector_t *deleted = floppy_sector(&floppy, 0, 0, 0);
  TEST_CHECK(deleted->deleted);
  TEST_CHECK(!deleted->data_crc_error);

  const floppy_sector_t *bad_data = floppy_sector(&floppy, 0, 0, 1);
  TEST_CHECK(bad_data->data_crc_error);
  TEST_CHECK(!bad_data->identity_crc_error);

  const floppy_sector_t *bad_id = floppy_sector(&floppy, 0, 0, 2);
  TEST_CHECK(bad_id->identity_crc_error);
  TEST_CHECK(!bad_id->data_crc_error);

  TEST_CHECK(floppy_sector(&floppy, 0, 0, 3)->no_data_field);
}

/* Eight and above mean 32K, which is how the chip counts (Owen's extensions,
   and Arnold's table measured on one); the image definition's older rule
   that only three bits counted would read the first as 128 bytes. */
static void a_size_code_of_eight_or_more_means_32k(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 8, 0, 0, 128, 0x01);
  add_sector(0, 0, 0xC2, 0xFF, 0, 0, 128, 0x02);
  add_sector(0, 0, 0xC3, 7, 0, 0, 128, 0x03);
  end_track(true);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->announced, 32768);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->announced, 32768);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 2)->announced, 16384);
  TEST_EQUAL(floppy_sector_length(0), 128);
  TEST_EQUAL(floppy_sector_length(6), 8192);
}

static void trailing_bytes_after_the_last_track_are_allowed(void) {
  build_plain_extended();
  image_length += 999;
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy.cylinders, 2);
}

static void an_image_that_is_not_one_is_refused(void) {
  begin_image(true, 1, 1);
  TEST_CHECK(!dsk_read(&floppy, image, 255, &problem));
  TEST_CHECK(problem != NULL);

  /* Eight bytes decide it, so a writer may sign the rest of the line how it
     likes and still be read; a difference inside those eight is another
     file entirely. */
  build_plain_extended();
  write_signature("MV - CPCEMU Disk-Wossname\r\n", 27);
  TEST_CHECK(dsk_identify(image, image_length));
  write_signature("MV - CPD", 8);
  TEST_CHECK(!dsk_identify(image, image_length));
  TEST_CHECK(!dsk_read(&floppy, image, image_length, &problem));
  write_signature("EXTENDEX", 8);
  TEST_CHECK(!dsk_identify(image, image_length));

  build_plain_extended();
  TEST_CHECK(dsk_identify(image, image_length));
}

static void a_geometry_no_disc_has_is_refused(void) {
  build_plain_extended();
  image[0x31] = 0;
  TEST_CHECK(!read_it());
  build_plain_extended();
  image[0x31] = 3;
  TEST_CHECK(!read_it());
  build_plain_extended();
  image[0x30] = 0;
  TEST_CHECK(!read_it());
  build_plain_extended();
  image[0x30] = 0xFF;
  TEST_CHECK(!read_it());
}

static void an_image_that_ends_early_is_refused(void) {
  /* One byte short of what the last track declares. */
  build_plain_extended();
  TEST_CHECK(!dsk_read(&floppy, image, image_length - 1, &problem));
  TEST_CHECK(problem != NULL);
  /* And short by most of a track. */
  build_plain_extended();
  TEST_CHECK(!dsk_read(&floppy, image, 256 + 1024, &problem));

  /* Every track unformatted is a legal image and an empty disc. */
  build_plain_extended();
  image[0x34] = 0;
  image[0x35] = 0;
  TEST_CHECK(read_it());
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 0));

  /* Only the original format can state a length too small to hold a track
     header; the extended one records a high byte, whose every non-zero
     value is at least 256. */
  begin_image(false, 1, 1);
  set_uniform_track_length(128);
  begin_track(0, 0, 2);
  end_track(false);
  TEST_CHECK(!read_it());
  TEST_CHECK(problem != NULL);
}

static void a_track_that_does_not_begin_where_it_should_is_refused(void) {
  build_plain_extended();
  image[256] = 'X';
  TEST_CHECK(!read_it());
  TEST_CHECK(problem != NULL);
}

static void more_sectors_than_a_header_holds_is_refused(void) {
  build_plain_extended();
  image[256 + 0x15] = FLOPPY_MAX_SECTORS + 1;
  TEST_CHECK(!read_it());
  TEST_CHECK(problem != NULL);
}

/* A sector claiming more bytes than its track has left cannot be allowed to
   name an offset outside the image, whatever the header says. */
static void a_sector_reaching_past_its_track_is_refused(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x01);
  end_track(true);
  image[256 + 0x18 + 6] = 0x00;
  image[256 + 0x18 + 7] = 0x40; /* 16K inside a 768-byte track */
  TEST_CHECK(!read_it());
  TEST_CHECK(problem != NULL);
}

static void reading_outside_the_disc_answers_nothing(void) {
  build_plain_extended();
  TEST_CHECK(read_it());
  uint8_t bytes[512];
  TEST_EQUAL(floppy_read(&floppy, 40, 0, 0, 0, 0, bytes, sizeof bytes), 0);
  TEST_EQUAL(floppy_read(&floppy, 0, 1, 0, 0, 0, bytes, sizeof bytes), 0);
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 9, 0, 0, bytes, sizeof bytes), 0);
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 0, 0, 512, bytes, sizeof bytes), 0);
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 0, 0, 500, bytes, sizeof bytes), 12);
  TEST_CHECK(floppy_sector(&floppy, 200, 0, 0) == NULL);
}

/* Tracks are stored cylinder by cylinder with the sides interleaved, and
   the length table is in that same order. */
static void a_two_sided_image_interleaves_its_tracks(void) {
  begin_image(true, 2, 2);
  for (uint8_t cylinder = 0; cylinder < 2; cylinder++) {
    for (uint8_t side = 0; side < 2; side++) {
      begin_track(cylinder, side, 2);
      add_sector(cylinder, side, 0xC1, 2, 0, 0, 512, (uint8_t)(0xA0 + cylinder * 2 + side));
      end_track(true);
    }
  }
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy.cylinders, 2);
  TEST_EQUAL(floppy.sides, 2);

  uint8_t bytes[512];
  for (uint8_t cylinder = 0; cylinder < 2; cylinder++) {
    for (uint8_t side = 0; side < 2; side++) {
      TEST_CHECK(floppy_track_formatted(&floppy, cylinder, side));
      TEST_EQUAL(floppy_read(&floppy, cylinder, side, 0, 0, 0, bytes, sizeof bytes), 512);
      TEST_EQUAL(bytes[0], 0xA0 + cylinder * 2 + side);
    }
  }
}

/* A second side that was never formatted still costs its table entry. */
static void one_side_of_a_two_sided_image_may_be_blank(void) {
  begin_image(true, 2, 2);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x11);
  end_track(true);
  image[0x34 + 1] = 0;
  track_count++;
  begin_track(1, 0, 2);
  add_sector(1, 0, 0xC1, 2, 0, 0, 512, 0x22);
  end_track(true);
  image[0x34 + 3] = 0;
  track_count++;

  TEST_CHECK(read_it());
  TEST_CHECK(floppy_track_formatted(&floppy, 0, 0));
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 1));
  TEST_CHECK(floppy_track_formatted(&floppy, 1, 0));
  TEST_CHECK(!floppy_track_formatted(&floppy, 1, 1));
  uint8_t bytes[512];
  TEST_EQUAL(floppy_read(&floppy, 1, 0, 0, 0, 0, bytes, sizeof bytes), 512);
  TEST_EQUAL(bytes[0], 0x22);
}

/* An identity with nothing recorded behind it: no reading to choose from
   and nothing to hand back. */
static void an_identity_with_no_data_behind_it_reads_nothing(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0x04, 0x01, 0, 0);
  add_sector(0, 0, 0xC2, 2, 0, 0, 512, 0x99);
  end_track(true);
  TEST_CHECK(read_it());

  const floppy_sector_t *empty = floppy_sector(&floppy, 0, 0, 0);
  TEST_CHECK(empty->no_data_field);
  TEST_EQUAL(empty->recorded, 0);
  TEST_EQUAL(empty->copies, 0);
  uint8_t bytes[512];
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 0, 0, 0, bytes, sizeof bytes), 0);
  /* The sector behind it still lands on its own bytes. */
  TEST_EQUAL(floppy_read(&floppy, 0, 0, 1, 0, 0, bytes, sizeof bytes), 512);
  TEST_EQUAL(bytes[0], 0x99);
}

static void identifying_needs_a_whole_header(void) {
  build_plain_extended();
  TEST_CHECK(!dsk_identify(image, 255));
  TEST_CHECK(dsk_identify(image, 256));
}

/* A refused image must leave nothing behind: reading stops at the first
   defect, and the tracks read before it describe a disc that never was. */
static void a_refused_image_leaves_nothing_mounted(void) {
  build_plain_extended();
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy.cylinders, 2);

  build_plain_extended();
  image[256 + 1280] = 'X'; /* the second track's signature */
  TEST_CHECK(!read_it());
  TEST_EQUAL(floppy.cylinders, 0);
  TEST_EQUAL(floppy.sides, 0);
  TEST_CHECK(floppy.image == NULL);
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 0));
  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), 0);
}

/* The medium's own refusals, which no image needs to be built to reach. */
static void the_medium_refuses_what_it_cannot_hold(void) {
  static uint8_t small[1024];
  floppy_init(&floppy);
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 0));
  floppy_mount(&floppy, small, sizeof small);

  TEST_CHECK(!floppy_add_track(&floppy, FLOPPY_MAX_CYLINDERS, 0, 0, 0, 0x52, 0xE5));
  TEST_CHECK(!floppy_add_track(&floppy, 0, FLOPPY_MAX_SIDES, 0, 0, 0x52, 0xE5));
  TEST_CHECK(
      !floppy_add_track(&floppy, 0, 0, 0, sizeof small + 1, 0x52, 0xE5)); /* room past the image */
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, sizeof small, 0x52, 0xE5));

  floppy_sector_t sector = {.r = 0xC1, .n = 2, .announced = 512, .recorded = 512, .copies = 1};
  TEST_CHECK(!floppy_add_sector(&floppy, 1, 0, &sector)); /* no track there */

  /* Readings that reach past the borrowed image are refused whatever the
     header claimed, because the medium is the last thing between a lie and
     a read outside memory nobody owns. */
  sector.image_offset = (uint32_t)sizeof small - 511;
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
  sector.image_offset = (uint32_t)sizeof small + 1;
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
  sector.image_offset = 0;
  sector.copies = 2; /* two readings fit the buffer exactly */
  TEST_CHECK(floppy_add_sector(&floppy, 0, 0, &sector));
  sector.copies = 3; /* a third does not */
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));

  sector.copies = 1;
  sector.image_offset = 0;
  for (int index = 1; index < FLOPPY_MAX_SECTORS; index++) {
    TEST_CHECK(floppy_add_sector(&floppy, 0, 0, &sector));
  }
  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), FLOPPY_MAX_SECTORS);
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));

  /* Nothing with data in it can be added to an unmounted medium, which is
     what keeps a sector and a missing image from ever meeting. */
  floppy_init(&floppy);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, 0, 0x52, 0xE5));
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
}

/* A field runs no further than a reading: its check is reckoned over the
   whole field, and the last reading ends where the image does. An identity
   with nothing behind it is held to the same, since a write takes that
   away and leaves the field it was given. */
static void a_field_runs_no_further_than_its_reading(void) {
  static uint8_t small[1024];
  floppy_mount(&floppy, small, sizeof small);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, sizeof small, 0x52, 0xE5));
  floppy_sector_t sector = {
      .r = 0xC1, .n = 2, .announced = 512, .recorded = 512, .extent = 513, .copies = 2};
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
  sector.no_data_field = true;
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
  sector.no_data_field = false;
  sector.copies = 1;
  sector.image_offset = sizeof small - 512;
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
  sector.recorded = 256; /* clipped short of what it announces */
  sector.extent = 512;
  sector.image_offset = sizeof small - 256;
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
  sector.recorded = 512;
  sector.image_offset = sizeof small - 512;
  TEST_CHECK(floppy_add_sector(&floppy, 0, 0, &sector));
}

/* Where the sectors lie once laid out as a formatter would have placed
   them: the preamble, then each sector after the one before it and its
   gap; a revolution of 6250 bytes. */
static void sectors_are_laid_out_as_a_formatter_would(void) {
  build_plain_extended();
  image[0x100 + 0x16] = 0x52; /* the gap the DATA format leaves */
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->position, FLOPPY_TRACK_PREAMBLE);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position,
             FLOPPY_TRACK_PREAMBLE + FLOPPY_SECTOR_OVERHEAD + 512 + 0x52);
  TEST_EQUAL(floppy_track_length(&floppy, 0, 0), FLOPPY_BYTES_PER_REVOLUTION);
  TEST_EQUAL(floppy_sector_beginning_at(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE), 0);
  TEST_EQUAL(floppy_sector_beginning_at(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE + 1), -1);
  /* An unformatted track still has a length: the disc turns regardless. */
  TEST_EQUAL(floppy_track_length(&floppy, 5, 0), FLOPPY_BYTES_PER_REVOLUTION);
}

/* Gaps the revolution cannot fit are shortened evenly, and sectors it
   cannot fit at all stretch it. */
static void a_gap_the_revolution_cannot_hold_is_shortened(void) {
  begin_image(true, 2, 1);
  begin_track(0, 0, 2);
  image[track_at + 0x16] = 0x52;
  for (int index = 0; index < 10; index++) {
    add_sector(0, 0, (uint8_t)(0xC1 + index), 2, 0, 0, 512, 0x00);
  }
  end_track(true);
  begin_track(1, 0, 2);
  image[track_at + 0x16] = 0x52;
  for (int index = 0; index < 12; index++) {
    add_sector(1, 0, (uint8_t)(0xC1 + index), 2, 0, 0, 512, 0x00);
  }
  end_track(true);
  TEST_CHECK(read_it());
  /* Ten sectors leave 6250 - 146 - 10 * 574 = 364 bytes for nine gaps. */
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position, 146 + 574 + 40);
  TEST_EQUAL(floppy_track_length(&floppy, 0, 0), 6250);
  /* Twelve do not fit even touching: the track is as long as they are. */
  TEST_EQUAL(floppy_sector(&floppy, 1, 0, 1)->position, 146 + 574);
  TEST_EQUAL(floppy_track_length(&floppy, 1, 0), 146 + 12 * 574);
}

/* Every byte of a track answers: the preamble, the sync and marks around
   each field, the identities, the checks the controller family computes,
   the data, and the gap. The check values were computed outside this
   code, by a Python CRC-16 with initial value &FFFF and polynomial &1021
   run over the three mark bytes, the address mark and the field, and that
   routine gives &29B1 for "123456789", the standard's own test vector. */
static void the_track_reads_as_a_place(void) {
  build_plain_extended();
  image[0x100 + 0x16] = 0x52;
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 0, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 80, 0), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 92, 0), 0xC2);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 95, 0), 0xFC);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 145, 0), 0x4E);
  uint32_t at = FLOPPY_TRACK_PREAMBLE;
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at, 0), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 12, 0), 0xA1);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 15, 0), 0xFE);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_ID_FIELD, 0), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_ID_FIELD + 2, 0), 0xC1);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_ID_FIELD + 3, 0), 0x02);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 20, 0), 0xDC); /* CRC of A1 A1 A1 FE 00 00 C1 02 */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 21, 0), 0x3B);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_ID_KNOWN, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 44, 0), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 56, 0), 0xA1);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + 59, 0), 0xFB);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_DATA_FIELD, 0), 0x10);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_DATA_FIELD + 511, 0), 0x10);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_DATA_FIELD + 512, 0),
             0x01); /* CRC of the field */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_DATA_FIELD + 513, 0), 0x40);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, at + FLOPPY_DATA_FIELD + 514, 0), 0x4E);
  /* The second sector, then the gap to the index, then round again. */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 802 + FLOPPY_DATA_FIELD, 0), 0x20);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 6249, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 6250, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 6250 + 92, 0), 0xC2);
  /* An unformatted track is gap from end to end. */
  TEST_EQUAL(floppy_byte(&floppy, 7, 0, 92, 0), 0x4E);
}

/* A failed check answers with a check that fails; a deleted mark is the
   deleted one; an unstable field reads differently each revolution. */
static void what_was_found_is_what_the_track_answers(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0x20, 0x00, 512, 0x10);  /* identity check failed */
  add_sector(0, 0, 0xC2, 2, 0x00, 0x40, 512, 0x10);  /* deleted */
  add_sector(0, 0, 0xC3, 2, 0x20, 0x20, 512, 0x10);  /* data check failed */
  add_sector(0, 0, 0xC4, 2, 0x00, 0x00, 1024, 0x01); /* two readings */
  memset(image + image_length - 512, 0x02, 512);
  end_track(true);
  TEST_CHECK(read_it());
  const floppy_sector_t *bad_identity = floppy_sector(&floppy, 0, 0, 0);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, bad_identity->position + 20, 0), (uint8_t)~0xDC);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, bad_identity->position + 21, 0), (uint8_t)~0x3B);
  const floppy_sector_t *deleted = floppy_sector(&floppy, 0, 0, 1);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, deleted->position + 59, 0), 0xF8);
  const floppy_sector_t *bad_data = floppy_sector(&floppy, 0, 0, 2);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, bad_data->position + FLOPPY_DATA_FIELD + 512, 0),
             (uint8_t)~0x01);
  const floppy_sector_t *unstable = floppy_sector(&floppy, 0, 0, 3);
  TEST_EQUAL(unstable->copies, 2);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, unstable->position + FLOPPY_DATA_FIELD, 0), 0x01);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, unstable->position + FLOPPY_DATA_FIELD, 1), 0x02);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, unstable->position + FLOPPY_DATA_FIELD, 2), 0x01);
}

/* A field shorter than announced ends where the dumper clipped it, and a
   head reading on finds its check, the gap, and the next sector's sync —
   which is what a controller told to read the announced length gets. */
static void reading_past_a_short_field_finds_the_next_sector(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  image[track_at + 0x16] = 0x52;
  add_sector(0, 0, 0xC1, 2, 0, 0, 256, 0x10);
  add_sector(0, 0, 0xC2, 2, 0, 0, 512, 0x20);
  end_track(true);
  TEST_CHECK(read_it());
  uint32_t data = FLOPPY_TRACK_PREAMBLE + FLOPPY_DATA_FIELD;
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 255, 0), 0x10);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 256, 0), 0x33); /* CRC over 256 bytes */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 257, 0), 0xCF);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 258, 0), 0x4E);
  uint32_t next = FLOPPY_TRACK_PREAMBLE + FLOPPY_SECTOR_OVERHEAD + 256 + 0x52;
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position, next);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, next - 1, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, next, 0), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, next + FLOPPY_ID_FIELD + 2, 0), 0xC2);
}

static void writes_land_in_the_data_field_and_nowhere_else(void) {
  build_plain_extended();
  TEST_CHECK(read_it());
  TEST_CHECK(!floppy.modified);
  uint32_t data = FLOPPY_TRACK_PREAMBLE + FLOPPY_DATA_FIELD;
  TEST_CHECK(floppy_write_byte(&floppy, 0, 0, data + 3, 0xAA));
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 3, 0), 0xAA);
  TEST_EQUAL(image[0x200 + 3], 0xAA); /* in the image itself, in place */
  TEST_CHECK(floppy.modified);
  TEST_CHECK(!floppy_write_byte(&floppy, 0, 0, data - 1, 0xBB));   /* the data mark */
  TEST_CHECK(!floppy_write_byte(&floppy, 0, 0, data + 512, 0xBB)); /* the check */
  TEST_CHECK(!floppy_write_byte(&floppy, 0, 0, 10, 0xBB));         /* the preamble */
  TEST_CHECK(!floppy_write_byte(&floppy, 3, 0, data, 0xBB));       /* no track */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data - 1, 0), 0xFB);
  floppy.write_protected = true;
  TEST_CHECK(!floppy_write_byte(&floppy, 0, 0, data + 4, 0xCC));
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 4, 0), 0x10);
}

/* A data field written whole is sound: it reads the same every revolution,
   its check passes, and it carries the mark it was written with. */
static void a_data_field_written_whole_is_sound(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0x20, 0x20, 1024, 0x01); /* two readings, check failed */
  end_track(true);
  TEST_CHECK(read_it());
  const floppy_sector_t *sector = floppy_sector(&floppy, 0, 0, 0);
  TEST_EQUAL(sector->copies, 2);
  TEST_CHECK(sector->data_crc_error);
  floppy_data_written(&floppy, 0, 0, 0, true);
  TEST_EQUAL(sector->copies, 1);
  TEST_CHECK(!sector->data_crc_error);
  TEST_CHECK(sector->deleted);
  TEST_CHECK(floppy.modified);
}

/* A track formats into the room its image gives it and no further. */
static void a_track_formats_into_the_room_it_has(void) {
  build_plain_extended();
  TEST_CHECK(read_it());
  static const uint8_t identities[3][4] = {{0, 0, 0x41, 2}, {0, 0, 0x42, 2}, {0, 0, 0x43, 2}};
  TEST_CHECK(floppy_format_track(&floppy, 0, 0, identities, 2, 2, 0x4E, 0xE5));
  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), 2);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->r, 0x41);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->r, 0x42);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position,
             FLOPPY_TRACK_PREAMBLE + FLOPPY_SECTOR_OVERHEAD + 512 + 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE + FLOPPY_DATA_FIELD, 0), 0xE5);
  TEST_EQUAL(image[0x200], 0xE5);
  TEST_EQUAL(image[0x200 + 1023], 0xE5);
  TEST_CHECK(floppy.modified);
  /* Three of them need 1536 bytes of a 1024-byte track: refused, unchanged. */
  TEST_CHECK(!floppy_format_track(&floppy, 0, 0, identities, 3, 2, 0x4E, 0xE5));
  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), 2);
  TEST_CHECK(!floppy_format_track(&floppy, 0, 0, identities, 2, 3, 0x4E, 0xE5));
  TEST_CHECK(
      !floppy_format_track(&floppy, 5, 0, identities, 1, 2, 0x4E, 0xE5)); /* no room at all */
  /* An empty track is a track. */
  TEST_CHECK(floppy_format_track(&floppy, 0, 0, identities, 0, 2, 0x4E, 0xE5));
  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), 0);
  floppy.write_protected = true;
  TEST_CHECK(!floppy_format_track(&floppy, 0, 0, identities, 1, 2, 0x4E, 0xE5));
}

/* An image may say where its sectors lay, and says it by their identity
   marks, which SAMdisk's decoder measures after the three bytes of sync
   mark (src/BitstreamDecoder.cpp, scan_bitstream_mfm_fm); one that says
   the impossible is laid out as usual. */
static void an_image_may_record_where_the_sectors_lay(void) {
  build_plain_extended();
  static const uint16_t offsets[] = {
      6300, 200, 900,   /* cylinder 0: sectors from 185 and 885 */
      6300, 200, 36768, /* cylinder 1: the second past the end */
  };
  add_offsets(offsets, 6);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_track_length(&floppy, 0, 0), 6300);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->position, 185);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position, 885);
  TEST_EQUAL(floppy_track_length(&floppy, 1, 0), FLOPPY_BYTES_PER_REVOLUTION);
  TEST_EQUAL(floppy_sector(&floppy, 1, 0, 0)->position, FLOPPY_TRACK_PREAMBLE);
  /* The bytes lie where the image says. */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 199, 0), 0xA1);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 200, 0), 0xFE);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 900, 0), 0xFE);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 885 + FLOPPY_DATA_FIELD, 0), 0x20);
}

/* A track declared at a rate or in a mode this family cannot decode is,
   to its heads, unformatted. */
static void a_track_recorded_another_way_is_unformatted(void) {
  build_plain_extended();
  image[0x100 + 0x12] = 2; /* high density */
  TEST_CHECK(read_it());
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 0));
  TEST_CHECK(floppy_track_formatted(&floppy, 1, 0));
  build_plain_extended();
  image[0x100 + 0x12] = 1;
  image[0x100 + 0x13] = 2; /* double density, MFM: the only thing it reads */
  TEST_CHECK(read_it());
  TEST_CHECK(floppy_track_formatted(&floppy, 0, 0));
  build_plain_extended();
  image[0x100 + 0x13] = 1; /* FM */
  TEST_CHECK(read_it());
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 0));
}

static bool same_sector(const floppy_sector_t *a, const floppy_sector_t *b) {
  return a->c == b->c && a->h == b->h && a->r == b->r && a->n == b->n && a->deleted == b->deleted &&
         a->identity_crc_error == b->identity_crc_error && a->data_crc_error == b->data_crc_error &&
         a->no_data_field == b->no_data_field && a->announced == b->announced &&
         a->recorded == b->recorded && a->copies == b->copies && a->position == b->position;
}

/* An image written back and read again is the same disc: every track,
   every sector, every reading, and where each lay. And written a second
   time it is the same bytes, so the writer has one answer. */
static void an_image_written_back_reads_as_the_same_disc(void) {
  static uint8_t written[64 * 1024];
  static uint8_t again[64 * 1024];
  static floppy_t second;
  begin_image(true, 3, 1);
  begin_track(0, 0, 2);
  image[track_at + 0x16] = 0x52;
  image[track_at + 0x17] = 0xE5;
  add_sector(0, 0, 0xC1, 2, 0x00, 0x00, 512, 0x10);
  add_sector(0, 0, 0xC2, 2, 0x00, 0x40, 512, 0x11);    /* deleted */
  add_sector(0, 0, 0xC3, 2, 0x20, 0x20, 1536, 0x12);   /* three readings, check failed */
  add_sector(0, 0, 0xC4, 2, 0x20, 0x00, 512, 0x13);    /* identity check failed */
  add_sector(0, 0, 0xC5, 2, 0x04, 0x00, 0, 0x00);      /* nothing behind it */
  add_sector(0, 0, 0xC6, 6, 0x00, 0x00, 0x1800, 0x14); /* an 8K sector, clipped */
  end_track(true);
  /* Cylinder 1 unformatted. */
  begin_track(2, 0, 1);
  add_sector(2, 0, 0x01, 1, 0x00, 0x00, 256, 0x15);
  end_track(true);
  TEST_CHECK(read_it());

  size_t needed = dsk_write(&floppy, NULL, 0);
  TEST_CHECK(needed > 0 && needed <= sizeof written);
  TEST_EQUAL(dsk_write(&floppy, written, needed - 1), needed); /* measured, not written */
  TEST_EQUAL(dsk_write(&floppy, written, sizeof written), needed);
  TEST_CHECK(memcmp(written, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34) == 0);

  const char *second_problem = NULL;
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_EQUAL(second.cylinders, floppy.cylinders);
  TEST_EQUAL(second.sides, floppy.sides);
  for (uint8_t cylinder = 0; cylinder < 3; cylinder++) {
    TEST_EQUAL(floppy_track_formatted(&second, cylinder, 0),
               floppy_track_formatted(&floppy, cylinder, 0));
    TEST_EQUAL(floppy_track_length(&second, cylinder, 0),
               floppy_track_length(&floppy, cylinder, 0));
    TEST_EQUAL(floppy_sector_count(&second, cylinder, 0),
               floppy_sector_count(&floppy, cylinder, 0));
    for (uint8_t index = 0; index < floppy_sector_count(&floppy, cylinder, 0); index++) {
      const floppy_sector_t *was = floppy_sector(&floppy, cylinder, 0, index);
      const floppy_sector_t *is = floppy_sector(&second, cylinder, 0, index);
      TEST_CHECK(same_sector(was, is));
      for (uint32_t copy = 0; copy < was->copies; copy++) {
        uint8_t theirs[0x1800];
        uint8_t ours[0x1800];
        uint32_t got = floppy_read(&floppy, cylinder, 0, index, copy, 0, ours, sizeof ours);
        TEST_EQUAL(floppy_read(&second, cylinder, 0, index, copy, 0, theirs, sizeof theirs), got);
        TEST_CHECK(memcmp(ours, theirs, got) == 0);
      }
    }
  }
  TEST_EQUAL(second.tracks[0][0].gap, 0x52);
  TEST_EQUAL(second.tracks[0][0].filler, 0xE5);

  TEST_EQUAL(dsk_write(&second, again, sizeof again), needed);
  TEST_CHECK(memcmp(written, again, needed) == 0);
}

/* Two sectors on each of `cylinders` tracks, the second cylinder never
   formatted when `blank_second` says so. */
static void build_cylinders(uint8_t cylinders, bool blank_second) {
  begin_image(true, cylinders, 1);
  for (uint8_t cylinder = 0; cylinder < cylinders; cylinder++) {
    if (blank_second && cylinder == 1) {
      track_count++; /* its length stays zero in the table */
      continue;
    }
    begin_track(cylinder, 0, 2);
    add_sector(cylinder, 0, 0xC1, 2, 0, 0, 512, 0x10);
    add_sector(cylinder, 0, 0xC2, 2, 0, 0, 512, 0x20);
    end_track(true);
  }
}

/* The offsets block has an entry for every cylinder and side, a track
   never formatted giving its length alone and one no head can read giving
   its whole entry; a block that stops short leaves the rest laid out as
   usual. The definition gives an entry to each track block instead, but
   SAMdisk, which writes these blocks, reads and writes them this way
   (src/types/dsk.cpp, ReadDSK and WriteDSK), and the image it wrote of
   Chicago 30s (UK, 1988), SHA-256
   065dea6a3a21c993865395289d25d4e25f936abd8e5940209f19be1c9a7bc529, whose
   last cylinder is blank, ends its block with that cylinder's zero. */
static void offsets_name_every_cylinder_and_side(void) {
  build_cylinders(4, true);
  image[0x100 + 0x12] = 2; /* the first track is high density: unreadable */
  static const uint16_t offsets[] = {
      6300, 200, 900,  /* the unreadable one, with two sectors */
      0,               /* cylinder 1, never formatted */
      6300, 315, 1015, /* cylinder 2: sectors from 300 and 1000 */
      6300,            /* cylinder 3's entry, cut short */
  };
  add_offsets(offsets, 8);
  TEST_CHECK(read_it());
  TEST_CHECK(!floppy_track_formatted(&floppy, 0, 0));
  TEST_CHECK(!floppy_track_formatted(&floppy, 1, 0));
  TEST_EQUAL(floppy_sector(&floppy, 2, 0, 0)->position, 300);
  TEST_EQUAL(floppy_sector(&floppy, 2, 0, 1)->position, 1000);
  TEST_EQUAL(floppy_track_length(&floppy, 2, 0), 6300);
  TEST_EQUAL(floppy_sector(&floppy, 3, 0, 0)->position, FLOPPY_TRACK_PREAMBLE);
  TEST_EQUAL(floppy_track_length(&floppy, 3, 0), FLOPPY_BYTES_PER_REVOLUTION);
  /* The track no head can read is written back with the length its entry
     gave, the first word of the twenty bytes of entries that end the image:
     six for it, two for the blank one and six for each of the others. */
  static uint8_t written[64 * 1024];
  size_t needed = dsk_write(&floppy, written, sizeof written);
  TEST_CHECK(needed > 20);
  TEST_EQUAL(written[needed - 20] | (written[needed - 19] << 8), 6300);
}

/* Offsets that do not climb are not believed, nor a mark at or past the
   end of its track, nor a first mark too near the index for the sync and
   marks ahead of it, which would have to begin before it. */
static void offsets_that_say_the_impossible_are_not_believed(void) {
  build_cylinders(5, false);
  static const uint16_t offsets[] = {
      6300, 3000, 200,  /* falling */
      6300, 200,  200,  /* standing still */
      6300, 14,   900,  /* a sync beginning before the index */
      6300, 15,   900,  /* a sync beginning at it */
      6300, 200,  6300, /* a mark at the end of the track */
  };
  add_offsets(offsets, 15);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->position, FLOPPY_TRACK_PREAMBLE);
  TEST_EQUAL(floppy_track_length(&floppy, 0, 0), FLOPPY_BYTES_PER_REVOLUTION);
  TEST_EQUAL(floppy_sector(&floppy, 1, 0, 0)->position, FLOPPY_TRACK_PREAMBLE);
  TEST_EQUAL(floppy_sector(&floppy, 2, 0, 0)->position, FLOPPY_TRACK_PREAMBLE);
  TEST_EQUAL(floppy_sector(&floppy, 3, 0, 0)->position, 0);
  TEST_EQUAL(floppy_sector(&floppy, 3, 0, 1)->position, 885);
  TEST_EQUAL(floppy_sector(&floppy, 4, 0, 1)->position,
             FLOPPY_TRACK_PREAMBLE + FLOPPY_SECTOR_OVERHEAD + 512);
}

/* Most of Chicago 30s's tracks, as its SAMdisk image records them, are
   6273 bytes long, with the first sector's mark at 219 and the tenth's at
   5745, which puts the end of its data field and check 31 bytes past the
   index. Here the eight between them are left out, and the last sector
   holds one reading counting up, or `copies` of them each filled with its
   own number and recorded as failing its check, since SAMdisk takes
   several readings only of a field that failed (src/types/dsk.cpp,
   ReadDSK). A blank cylinder and a plain one follow, so that a writer must
   give the blank one's entry between two others. */
#define WRAPPING_TRACK_LENGTH 6273
#define WRAPPING_SECTOR_START (5745 - 15)
static const uint16_t wrapping_offsets[] = {WRAPPING_TRACK_LENGTH, 219, 5745, 0, 6250, 161, 775};

static void build_wrapping_track(size_t copies) {
  begin_image(true, 3, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x10);
  uint8_t failed = copies > 1 ? 0x20 : 0x00;
  add_sector(0, 0, 0xCA, 2, failed, failed, 512 * copies, 0x00);
  uint8_t *data = image + image_length - 512 * copies;
  for (size_t index = 0; index < 512 * copies; index++) {
    data[index] = copies == 1 ? (uint8_t)index : (uint8_t)(index / 512 + 1);
  }
  end_track(true);
  track_count++; /* cylinder 1 never formatted: its length stays zero */
  begin_track(2, 0, 2);
  add_sector(2, 0, 0xC1, 2, 0, 0, 512, 0x10);
  add_sector(2, 0, 0xC2, 2, 0, 0, 512, 0x20);
  end_track(true);
  add_offsets(wrapping_offsets, sizeof wrapping_offsets / sizeof wrapping_offsets[0]);
}

/* The check the controller family runs over the last sector's data field,
   from its marks through its check bytes: up to the index on revolution
   `revolution`, and past it on the next. */
static uint16_t check_across_the_index(uint32_t revolution) {
  uint16_t crc = FLOPPY_CRC_INITIAL;
  uint32_t first = WRAPPING_SECTOR_START + FLOPPY_DATA_FIELD - FLOPPY_MARK_BYTES;
  uint32_t end = WRAPPING_SECTOR_START + FLOPPY_DATA_FIELD + 512 + FLOPPY_CHECK_BYTES;
  for (uint32_t position = first; position < end; position++) {
    uint32_t passing = position >= WRAPPING_TRACK_LENGTH ? revolution + 1 : revolution;
    crc = floppy_crc(crc, floppy_byte(&floppy, 0, 0, position % WRAPPING_TRACK_LENGTH, passing));
  }
  return crc;
}

/* A sector written across the index runs on past it: its data and check
   pass on the next revolution, then the gap, and then the preamble the
   formatter wrote, index mark and all. */
static void a_sector_may_run_on_past_the_index(void) {
  build_wrapping_track(1);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_track_length(&floppy, 0, 0), WRAPPING_TRACK_LENGTH);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position, WRAPPING_SECTOR_START);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, WRAPPING_TRACK_LENGTH - 1, 0), 482 & 0xFF);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 0, 1), 483 & 0xFF);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 28, 1), 511 & 0xFF);
  TEST_EQUAL(check_across_the_index(0), 0);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 31, 1), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 80, 1), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 92, 1), 0xC2);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 95, 1), 0xFC);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 219, 1), 0xFE);
}

/* What passes after the index is the reading that began before it, so a
   field read across the index is one reading, with that reading's check.
   Three readings, so that the revolution before and the one after differ.
   Each is recorded as failing, and a check that fails by every bit
   inverted leaves &1D0F whatever the field, as the Python CRC-16 above
   gives over any field followed by its inverted check. */
static void a_field_across_the_index_is_one_reading(void) {
  build_wrapping_track(3);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->copies, 3);
  uint32_t data = WRAPPING_SECTOR_START + FLOPPY_DATA_FIELD;
  for (uint32_t revolution = 0; revolution < 3; revolution++) {
    TEST_EQUAL(floppy_byte(&floppy, 0, 0, data, revolution), revolution + 1);
    TEST_EQUAL(floppy_byte(&floppy, 0, 0, 10, revolution + 1), revolution + 1);
    TEST_EQUAL(check_across_the_index(revolution), 0x1D0F);
  }
  /* The first revolution's tail is the last reading's, the readings taken
     round as `floppy_read` takes them. */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 10, 0), 3);
}

/* A track laid out as a formatter would have, whose last sector was stored
   with more of the gap behind it than the revolution leaves room for, has
   that gap run on past the index as far as the first sector, as SAMdisk
   stores a last sector's gap (src/BitstreamDecoder.cpp,
   scan_bitstream_mfm_fm). */
static void a_last_sector_stored_long_runs_on_past_the_index(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  image[track_at + 0x16] = 0x4E;
  for (uint8_t index = 0; index < 8; index++) {
    add_sector(0, 0, (uint8_t)(0xC1 + index), 2, 0, 0, 512, 0xE5);
  }
  add_sector(0, 0, 0xC9, 2, 0, 0, 1000, 0x77);
  end_track(true);
  TEST_CHECK(read_it());
  uint32_t last = FLOPPY_TRACK_PREAMBLE + 8 * (FLOPPY_SECTOR_OVERHEAD + 512 + 0x4E);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 8)->position, last);
  TEST_EQUAL(floppy_track_length(&floppy, 0, 0), FLOPPY_BYTES_PER_REVOLUTION);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, FLOPPY_BYTES_PER_REVOLUTION - 1, 0), 0x77);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 0, 1), 0x77);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 92, 1), 0x77);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE - 1, 1), 0x77);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE, 1), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE + 15, 1), 0xFE);
}

/* A sector past the index covers the preamble only as far as it reaches: a
   reading that runs on past its field carries the gap it was read with,
   which here ends inside the preamble's sync, and an identity with nothing
   recorded behind it ends at its check, whatever bytes the image kept for
   it. */
static void a_sector_past_the_index_covers_only_what_it_reaches(void) {
  begin_image(true, 2, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x10);
  add_sector(0, 0, 0xC2, 2, 0, 0, 600, 0x20);
  memset(image + image_length - 88, 0x55, 88); /* the check and gap behind the field */
  end_track(true);
  begin_track(1, 0, 2);
  add_sector(1, 0, 0xC1, 2, 0, 0, 512, 0x10);
  add_sector(1, 0, 0xC2, 2, 0x04, 0x01, 512, 0x20); /* no data field */
  end_track(true);
  /* On the first track the reading's 600 bytes end at 86; on the second the
     identity's last check byte passes at 3. */
  static const uint16_t offsets[] = {
      WRAPPING_TRACK_LENGTH, 219, WRAPPING_TRACK_LENGTH + 86 - 660 + 15,
      WRAPPING_TRACK_LENGTH, 219, WRAPPING_TRACK_LENGTH - 3,
  };
  add_offsets(offsets, 6);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 0, 1), 0x55);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 85, 1), 0x55);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 86, 1), 0x00);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 92, 1), 0xC2);
  TEST_EQUAL(floppy_byte(&floppy, 1, 0, 0, 1), 0xC2); /* its R */
  TEST_EQUAL(floppy_byte(&floppy, 1, 0, 3, 1), 0xDC); /* CRC of A1 A1 A1 FE 01 00 C2 02 */
  TEST_EQUAL(floppy_byte(&floppy, 1, 0, 4, 1), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 1, 0, 92, 1), 0xC2);
}

/* A write past the index lands in the field it belongs to; the check and
   the gap beyond it take nothing. */
static void a_write_past_the_index_lands_in_its_field(void) {
  build_wrapping_track(1);
  TEST_CHECK(read_it());
  const floppy_sector_t *sector = floppy_sector(&floppy, 0, 0, 1);
  TEST_CHECK(floppy_write_byte(&floppy, 0, 0, 10, 0xAA));
  TEST_EQUAL(image[sector->image_offset + 493], 0xAA);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 10, 1), 0xAA);
  TEST_CHECK(!floppy_write_byte(&floppy, 0, 0, 29, 0xBB)); /* the check */
  TEST_CHECK(!floppy_write_byte(&floppy, 0, 0, 31, 0xBB)); /* the gap */
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, 31, 1), 0x4E);
}

/* An image written back gives back the offsets block it was read with,
   word for word, the blank cylinder's entry among them, and reads back as
   the same track. */
static void offsets_are_written_back_as_they_were_read(void) {
  static uint8_t written[64 * 1024];
  static floppy_t second;
  build_wrapping_track(1);
  TEST_CHECK(read_it());
  size_t needed = dsk_write(&floppy, written, sizeof written);
  TEST_CHECK(needed > 0);
  size_t block = sizeof wrapping_offsets + 15;
  TEST_CHECK(needed >= block);
  TEST_CHECK(memcmp(written + needed - block, image + image_length - block, block) == 0);
  const char *second_problem = NULL;
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_EQUAL(floppy_track_length(&second, 0, 0), WRAPPING_TRACK_LENGTH);
  TEST_EQUAL(floppy_sector(&second, 0, 0, 1)->position, WRAPPING_SECTOR_START);
  TEST_EQUAL(floppy_byte(&second, 0, 0, 28, 1), 511 & 0xFF);
}

/* An unreadable track keeps its block through a write of the image, and
   an original-layout image, a track with no sectors and a disc with no
   track at all come back as they went. */
static void what_a_writer_cannot_read_it_still_keeps(void) {
  static uint8_t written[64 * 1024];
  static floppy_t second;
  const char *second_problem = NULL;

  build_plain_extended();
  image[0x100 + 0x12] = 2;
  TEST_CHECK(read_it());
  size_t needed = dsk_write(&floppy, written, sizeof written);
  TEST_CHECK(needed > 0);
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_EQUAL(second.cylinders, 2);
  TEST_CHECK(!floppy_track_formatted(&second, 0, 0));
  TEST_CHECK(second.tracks[0][0].unreadable);
  TEST_EQUAL(floppy_sector(&second, 1, 0, 1)->r, 0xC2);
  /* The block itself came through: its own header still says HD. */
  TEST_EQUAL(written[0x100 + 0x12], 2);

  begin_image(false, 1, 1);
  set_uniform_track_length(256 + 1024);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0, 0, 512, 0x10);
  add_sector(0, 0, 0xC2, 2, 0, 0, 512, 0x20);
  end_track(false);
  TEST_CHECK(read_it());
  needed = dsk_write(&floppy, written, sizeof written);
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_EQUAL(floppy_sector_count(&second, 0, 0), 2);
  TEST_EQUAL(floppy_sector(&second, 0, 0, 1)->recorded, 512);

  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  end_track(true);
  TEST_CHECK(read_it());
  needed = dsk_write(&floppy, written, sizeof written);
  TEST_EQUAL(needed, 256 + 256 + 15 + 2);
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_CHECK(floppy_track_formatted(&second, 0, 0));
  TEST_EQUAL(floppy_sector_count(&second, 0, 0), 0);

  begin_image(true, 2, 1);
  TEST_CHECK(read_it()); /* every track unformatted */
  needed = dsk_write(&floppy, written, sizeof written);
  TEST_EQUAL(needed, 256 + 15 + 2); /* the one track's entry: a length of zero */
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_CHECK(!floppy_track_formatted(&second, 0, 0));
}

/* A track the table's byte cannot measure, or the offsets block cannot
   place, is not written. */
static void a_track_too_big_to_describe_is_not_written(void) {
  static uint8_t huge[3 * 32768 + 256];
  floppy_mount(&floppy, huge, sizeof huge);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, sizeof huge, 0x52, 0xE5));
  floppy_sector_t sector = {.r = 1, .n = 8, .announced = 32768, .recorded = 32768, .copies = 1};
  for (int index = 0; index < 3; index++) {
    sector.image_offset = (uint32_t)index * 32768;
    TEST_CHECK(floppy_add_sector(&floppy, 0, 0, &sector));
  }
  floppy_layout_track(&floppy, 0, 0);
  TEST_EQUAL(dsk_write(&floppy, NULL, 0), 0);

  /* A sector is written by its identity mark, which the block can only
     place before the index. */
  floppy_mount(&floppy, huge, sizeof huge);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, sizeof huge, 0x52, 0xE5));
  sector = (floppy_sector_t){.r = 1, .n = 2, .announced = 512, .recorded = 512, .copies = 1};
  TEST_CHECK(floppy_add_sector(&floppy, 0, 0, &sector));
  floppy_set_track_length(&floppy, 0, 0, 6300);
  floppy.tracks[0][0].sectors[0].position = 6300 - 16;
  TEST_CHECK(dsk_write(&floppy, NULL, 0) > 0);
  floppy.tracks[0][0].sectors[0].position = 6300 - 15;
  TEST_EQUAL(dsk_write(&floppy, NULL, 0), 0);

  /* A track the head cannot decode is held to the same: its block, padded
     out to whole units, and its length. */
  floppy_mount(&floppy, huge, sizeof huge);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, 255 * 256, 0x52, 0xE5));
  floppy.tracks[0][0].unreadable = true;
  TEST_CHECK(dsk_write(&floppy, NULL, 0) > 0);
  floppy_set_track_length(&floppy, 0, 0, 0x10000);
  TEST_EQUAL(dsk_write(&floppy, NULL, 0), 0);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, 255 * 256 - 255, 0x52, 0xE5));
  floppy.tracks[0][0].unreadable = true;
  TEST_CHECK(dsk_write(&floppy, NULL, 0) > 0);
  TEST_CHECK(floppy_add_track(&floppy, 0, 0, 0, 255 * 256 + 1, 0x52, 0xE5));
  floppy.tracks[0][0].unreadable = true;
  TEST_EQUAL(dsk_write(&floppy, NULL, 0), 0);
}

/* The original layout gives every track one length, which need not be a
   whole number of the 256-byte units the extended layout counts in. A
   track the head cannot decode keeps its block, padded out to the next
   unit with zeros as every block is, and the tracks after it stay where
   the table says they are. */
static void an_unreadable_block_is_padded_to_whole_units(void) {
  static uint8_t written[64 * 1024];
  static uint8_t again[64 * 1024];
  static floppy_t second;
  const char *second_problem = NULL;

  begin_image(false, 2, 1);
  set_uniform_track_length(256 + 512 + 128);
  for (uint8_t cylinder = 0; cylinder < 2; cylinder++) {
    begin_track(cylinder, 0, 2);
    /* The second announces 128 bytes, half a unit, of its allotment. */
    add_sector(cylinder, 0, 0xC1, cylinder == 0 ? 2 : 0, 0, 0, 512, (uint8_t)(0x10 + cylinder));
    memset(image + image_length, 0x77, 128);
    image_length += 128;
  }
  image[0x100 + 0x12] = 2; /* the first track is high density: unreadable */
  TEST_CHECK(read_it());

  memset(written, 0xEE, sizeof written);
  size_t needed = dsk_write(&floppy, written, sizeof written);
  TEST_EQUAL(needed, 256 + 1024 + 512 + 15 + 4 + 4);
  TEST_EQUAL(written[0x34], 4);
  TEST_EQUAL(written[0x35], 2);
  TEST_CHECK(memcmp(written + 0x100, image + 0x100, 896) == 0);
  static const uint8_t padding[128];
  TEST_CHECK(memcmp(written + 0x100 + 896, padding, sizeof padding) == 0);
  TEST_CHECK(memcmp(written + 0x100 + 1024 + 256 + 128, padding, sizeof padding) == 0);
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_CHECK(second.tracks[0][0].unreadable);
  uint8_t byte = 0;
  TEST_EQUAL(floppy_read(&second, 1, 0, 0, 0, 0, &byte, 1), 1);
  TEST_EQUAL(byte, 0x11);
  TEST_EQUAL(dsk_write(&second, again, sizeof again), needed);
  TEST_CHECK(memcmp(again, written, needed) == 0);
}

/* A formatter's length is the field's, whatever the identity announces,
   and the layout follows the field. */
static void a_format_lays_out_the_length_it_wrote(void) {
  build_plain_extended();
  TEST_CHECK(read_it());
  static const uint8_t identities[2][4] = {{0, 0, 0x41, 1}, {0, 0, 0x42, 3}};
  TEST_CHECK(floppy_format_track(&floppy, 0, 0, identities, 2, 2, 0x52, 0xE5));
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->announced, 256);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->extent, 512);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->announced, 1024);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->extent, 512);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->position,
             FLOPPY_TRACK_PREAMBLE + FLOPPY_SECTOR_OVERHEAD + 512 + 0x52);
  /* The check lies after the whole field, and covers it. */
  uint32_t data = FLOPPY_TRACK_PREAMBLE + FLOPPY_DATA_FIELD;
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 511, 0), 0xE5);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, data + 514, 0), 0x4E);
}

/* An identity with nothing behind it is gap from its check on, and an
   unstable field written whole reads the same every revolution. */
static void nothing_behind_an_identity_reads_as_gap(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 2, 0x04, 0x00, 0, 0x00);    /* nothing behind it */
  add_sector(0, 0, 0xC2, 2, 0x00, 0x00, 1024, 0x01); /* two readings */
  memset(image + image_length - 512, 0x02, 512);
  end_track(true);
  TEST_CHECK(read_it());
  uint32_t first = floppy_sector(&floppy, 0, 0, 0)->position;
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, first + FLOPPY_ID_KNOWN, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, first + FLOPPY_DATA_FIELD - 1, 0), 0x4E);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, first + FLOPPY_DATA_FIELD, 0), 0x4E);
  uint32_t second = floppy_sector(&floppy, 0, 0, 1)->position;
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, second + FLOPPY_DATA_FIELD, 1), 0x02);
  TEST_CHECK(floppy_write_byte(&floppy, 0, 0, second + FLOPPY_DATA_FIELD, 0x77));
  floppy_data_written(&floppy, 0, 0, 1, false);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, second + FLOPPY_DATA_FIELD, 0), 0x77);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, second + FLOPPY_DATA_FIELD, 1), 0x77);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->copies, 1);
}

/* A track formatted larger than the room it had takes fresh room past the
   image, when the host gave any, and comes through a write of the image. */
static void a_format_may_take_room_past_the_image(void) {
  static uint8_t written[64 * 1024];
  static floppy_t second;
  build_plain_extended();
  TEST_CHECK(read_it());
  static const uint8_t identities[3][4] = {{0, 0, 0x41, 3}, {0, 0, 0x42, 3}, {0, 0, 0x43, 3}};
  TEST_CHECK(!floppy_format_track(&floppy, 0, 0, identities, 3, 3, 0x4E, 0xE5));
  size_t before = floppy.image_length;
  floppy_give_room(&floppy, before + 3072);
  TEST_CHECK(floppy_format_track(&floppy, 0, 0, identities, 3, 3, 0x4E, 0xE5));
  TEST_EQUAL(floppy.image_length, before + 3072);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->image_offset, before);
  TEST_EQUAL(floppy_byte(&floppy, 0, 0, FLOPPY_TRACK_PREAMBLE + FLOPPY_DATA_FIELD + 1023, 0), 0xE5);
  /* The room is spent: a fourth format of the size finds none. */
  TEST_CHECK(!floppy_format_track(&floppy, 1, 0, identities, 3, 3, 0x4E, 0xE5));
  size_t needed = dsk_write(&floppy, written, sizeof written);
  const char *second_problem = NULL;
  TEST_CHECK(dsk_read(&second, written, needed, &second_problem));
  TEST_EQUAL(floppy_sector_count(&second, 0, 0), 3);
  TEST_EQUAL(floppy_sector(&second, 0, 0, 2)->recorded, 1024);
  TEST_EQUAL(floppy_sector_count(&second, 1, 0), 2);
}

int main(void) {
  TEST_RUN(an_extended_image_reads_back_its_shape);
  TEST_RUN(the_original_format_allots_every_sector_the_same_room);
  TEST_RUN(an_unformatted_track_takes_up_no_room);
  TEST_RUN(an_unstable_sector_keeps_every_reading);
  TEST_RUN(a_sector_running_into_the_gap_is_one_reading);
  TEST_RUN(a_short_sector_reads_short);
  TEST_RUN(a_track_may_announce_one_number_twice);
  TEST_RUN(an_identity_that_lies_is_kept);
  TEST_RUN(recorded_status_becomes_what_was_found);
  TEST_RUN(a_size_code_of_eight_or_more_means_32k);
  TEST_RUN(trailing_bytes_after_the_last_track_are_allowed);
  TEST_RUN(an_image_that_is_not_one_is_refused);
  TEST_RUN(a_geometry_no_disc_has_is_refused);
  TEST_RUN(an_image_that_ends_early_is_refused);
  TEST_RUN(a_track_that_does_not_begin_where_it_should_is_refused);
  TEST_RUN(more_sectors_than_a_header_holds_is_refused);
  TEST_RUN(a_sector_reaching_past_its_track_is_refused);
  TEST_RUN(reading_outside_the_disc_answers_nothing);
  TEST_RUN(a_two_sided_image_interleaves_its_tracks);
  TEST_RUN(one_side_of_a_two_sided_image_may_be_blank);
  TEST_RUN(an_identity_with_no_data_behind_it_reads_nothing);
  TEST_RUN(identifying_needs_a_whole_header);
  TEST_RUN(a_refused_image_leaves_nothing_mounted);
  TEST_RUN(the_medium_refuses_what_it_cannot_hold);
  TEST_RUN(a_field_runs_no_further_than_its_reading);
  TEST_RUN(sectors_are_laid_out_as_a_formatter_would);
  TEST_RUN(a_gap_the_revolution_cannot_hold_is_shortened);
  TEST_RUN(the_track_reads_as_a_place);
  TEST_RUN(what_was_found_is_what_the_track_answers);
  TEST_RUN(reading_past_a_short_field_finds_the_next_sector);
  TEST_RUN(writes_land_in_the_data_field_and_nowhere_else);
  TEST_RUN(a_data_field_written_whole_is_sound);
  TEST_RUN(a_track_formats_into_the_room_it_has);
  TEST_RUN(an_image_may_record_where_the_sectors_lay);
  TEST_RUN(a_track_recorded_another_way_is_unformatted);
  TEST_RUN(an_image_written_back_reads_as_the_same_disc);
  TEST_RUN(offsets_name_every_cylinder_and_side);
  TEST_RUN(offsets_that_say_the_impossible_are_not_believed);
  TEST_RUN(a_sector_may_run_on_past_the_index);
  TEST_RUN(a_field_across_the_index_is_one_reading);
  TEST_RUN(a_sector_past_the_index_covers_only_what_it_reaches);
  TEST_RUN(a_last_sector_stored_long_runs_on_past_the_index);
  TEST_RUN(a_write_past_the_index_lands_in_its_field);
  TEST_RUN(offsets_are_written_back_as_they_were_read);
  TEST_RUN(what_a_writer_cannot_read_it_still_keeps);
  TEST_RUN(a_track_too_big_to_describe_is_not_written);
  TEST_RUN(an_unreadable_block_is_padded_to_whole_units);
  TEST_RUN(a_format_lays_out_the_length_it_wrote);
  TEST_RUN(nothing_behind_an_identity_reads_as_gap);
  TEST_RUN(a_format_may_take_room_past_the_image);
  return TEST_REPORT("floppy");
}
