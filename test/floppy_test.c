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

static void begin_image(bool extended, uint8_t cylinders, uint8_t sides) {
  memset(image, 0, sizeof image);
  memcpy(image,
         extended ? "EXTENDED CPC DSK File\r\nDisk-Info\r\n"
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

static void a_size_code_counts_three_bits(void) {
  begin_image(true, 1, 1);
  begin_track(0, 0, 2);
  add_sector(0, 0, 0xC1, 8, 0, 0, 128, 0x01);
  add_sector(0, 0, 0xC2, 0xFF, 0, 0, (size_t)128 * 128, 0x02);
  end_track(true);
  TEST_CHECK(read_it());
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 0)->announced, 128);   /* 8 reads as 0 */
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 1)->announced, 16384); /* &FF reads as 7 */
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
  memcpy(image, "MV - CPCEMU Disk-Wossname\r\n", 27);
  TEST_CHECK(dsk_identify(image, image_length));
  memcpy(image, "MV - CPD", 8);
  TEST_CHECK(!dsk_identify(image, image_length));
  TEST_CHECK(!dsk_read(&floppy, image, image_length, &problem));
  memcpy(image, "EXTENDEX", 8);
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

  TEST_CHECK(!floppy_add_track(&floppy, FLOPPY_MAX_CYLINDERS, 0));
  TEST_CHECK(!floppy_add_track(&floppy, 0, FLOPPY_MAX_SIDES));
  TEST_CHECK(floppy_add_track(&floppy, 0, 0));

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
  TEST_CHECK(floppy_add_track(&floppy, 0, 0));
  TEST_CHECK(!floppy_add_sector(&floppy, 0, 0, &sector));
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
  TEST_RUN(a_size_code_counts_three_bits);
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
  return TEST_REPORT("floppy");
}
