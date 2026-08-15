/*
 * dsk.c — the two disc image layouts, read onto a medium.
 */
#include <string.h>

#include "dsk.h"

#define HEADER_SIZE 256
#define AT_CYLINDERS 0x30
#define AT_SIDES 0x31
#define AT_TRACK_LENGTH 0x32 /* original only: the length every track shares */
#define AT_LENGTH_TABLE 0x34 /* extended only: one high byte per track */

#define TRACK_HEADER_SIZE 256
#define AT_TRACK_SIZE_CODE 0x14
#define AT_TRACK_SECTORS 0x15
#define AT_SECTOR_LIST 0x18
#define SECTOR_ENTRY_SIZE 8

/* Status registers 1 and 2 as the µPD765 returns them and as an image
   records them per sector. The two sources disagree and the datasheet wins:
   "Disk image file format" prints the control mark at bit 5, where it also
   prints the data-field error, while the µPD765A datasheet (both linked
   from dsk.h) puts the control mark at bit 6 and the data-field error at
   bit 5. A chip's own datasheet describes what a controller recorded; the
   image definition only describes where it was written down. */
#define ST1_NO_DATA 0x04
#define ST1_DATA_ERROR 0x20
#define ST2_MISSING_DATA_MARK 0x01
#define ST2_DATA_FIELD_ERROR 0x20
#define ST2_CONTROL_MARK 0x40

static bool fail(const char **problem, const char *sentence) {
  *problem = sentence;
  return false;
}

/* A size code is three bits wide, so 8 counts as 0, and no byte from the
   file can ask for a shift the arithmetic will not hold (extended
   definition, extension 2.1). */
static uint32_t announced_length(uint8_t size_code) { return 128u << (size_code & 0x07); }

static bool has_tag(const uint8_t *image, size_t length, const char *tag) {
  size_t tag_length = strlen(tag);
  return length >= tag_length && memcmp(image, tag, tag_length) == 0;
}

/* The extended tag exists so a reader of the original format cannot misread
   the file. The two are disjoint: neither falls back on the other. */
static bool is_extended(const uint8_t *image, size_t length) {
  return has_tag(image, length, "EXTENDED");
}

bool dsk_identify(const uint8_t *image, size_t length) {
  if (length < HEADER_SIZE) {
    return false;
  }
  /* Eight bytes settle it. The original's definition says "MV - CPC" is
     enough on its own, and the rest of that line varies by writer. */
  return is_extended(image, length) || has_tag(image, length, "MV - CPC");
}

static void read_identity(const uint8_t *entry, floppy_sector_t *sector) {
  sector->c = entry[0];
  sector->h = entry[1];
  sector->r = entry[2];
  sector->n = entry[3];
  sector->announced = announced_length(entry[3]);

  uint8_t status1 = entry[4];
  uint8_t status2 = entry[5];
  sector->deleted = (status2 & ST2_CONTROL_MARK) != 0;
  sector->data_crc_error = (status2 & ST2_DATA_FIELD_ERROR) != 0;
  /* A failed check the data field does not own belongs to the identity
     field, which is the only other thing on a track carrying one. */
  sector->identity_crc_error = (status1 & ST1_DATA_ERROR) != 0 && !sector->data_crc_error;
  sector->no_data_field = (status1 & ST1_NO_DATA) != 0 || (status2 & ST2_MISSING_DATA_MARK) != 0;
}

/* How many readings of a sector its stored length holds. An exact multiple
   of the announced length is that many recordings of a data field that read
   differently each time; anything else is one reading, whether it falls
   short of what the identity claims or runs past it into the gap behind. */
static void split_into_copies(floppy_sector_t *sector, uint32_t stored) {
  if (stored == 0) {
    sector->recorded = 0;
    sector->copies = 0;
  } else if (sector->announced > 0 && stored > sector->announced &&
             stored % sector->announced == 0) {
    sector->recorded = sector->announced;
    sector->copies = stored / sector->announced;
  } else {
    sector->recorded = stored;
    sector->copies = 1;
  }
}

static bool read_track(floppy_t *floppy, const uint8_t *image, size_t at, size_t track_length,
                       bool extended, uint8_t cylinder, uint8_t side, const char **problem) {
  const uint8_t *header = image + at;
  if (memcmp(header, "Track-Info", 10) != 0) {
    return fail(problem, "a track does not begin where the image says it does");
  }
  uint8_t sector_count = header[AT_TRACK_SECTORS];
  if (sector_count > FLOPPY_MAX_SECTORS) {
    return fail(problem, "a track lists more sectors than its header has room for");
  }

  /* The room a track's header leaves for data, and how much of it the
     sectors so far have taken. Every step is subtraction from what is left,
     so no running total can wrap past the end. */
  size_t room = track_length - TRACK_HEADER_SIZE;
  size_t taken = 0;
  uint32_t allotment = announced_length(header[AT_TRACK_SIZE_CODE]);

  for (uint8_t index = 0; index < sector_count; index++) {
    const uint8_t *entry = header + AT_SECTOR_LIST + (size_t)index * SECTOR_ENTRY_SIZE;
    floppy_sector_t sector = {0};
    read_identity(entry, &sector);

    /* The original format gives every sector on a track the same room and
       records no length of its own; the extended one stores the true one. */
    size_t occupies = extended ? (size_t)(entry[6] | (entry[7] << 8)) : allotment;
    if (occupies > room - taken) {
      return fail(problem, "a sector's data runs past the end of its track");
    }
    uint32_t stored = (uint32_t)occupies;
    if (!extended && stored > sector.announced) {
      stored = sector.announced; /* the rest of the allotment is padding */
    }
    split_into_copies(&sector, stored);
    sector.image_offset = (uint32_t)(at + TRACK_HEADER_SIZE + taken);
    if (!floppy_add_sector(floppy, cylinder, side, &sector)) {
      return fail(problem, "a sector's data runs past the end of the image");
    }
    taken += occupies;
  }
  return true;
}

static bool read_image(floppy_t *floppy, const uint8_t *image, size_t length,
                       const char **problem) {
  if (length < HEADER_SIZE) {
    return fail(problem, "the image is shorter than its own header");
  }
  if (!dsk_identify(image, length)) {
    return fail(problem, "the image does not begin like a disc image");
  }
  bool extended = is_extended(image, length);
  uint8_t cylinders = image[AT_CYLINDERS];
  uint8_t sides = image[AT_SIDES];
  if (sides < 1 || sides > FLOPPY_MAX_SIDES) {
    return fail(problem, "the image claims a number of sides no drive has");
  }
  if (cylinders < 1 || cylinders > FLOPPY_MAX_CYLINDERS) {
    return fail(problem, "the image claims more cylinders than the medium holds");
  }

  floppy_mount(floppy, image, length);

  size_t at = HEADER_SIZE;
  for (uint8_t cylinder = 0; cylinder < cylinders; cylinder++) {
    for (uint8_t side = 0; side < sides; side++) {
      size_t track_length;
      if (extended) {
        /* Zero is a track that was never formatted: no data and no header
           of its own anywhere in the file, so it takes up no room in it. */
        track_length = (size_t)image[AT_LENGTH_TABLE + (size_t)cylinder * sides + side] * 256;
        if (track_length == 0) {
          continue;
        }
      } else {
        track_length = (size_t)image[AT_TRACK_LENGTH] | ((size_t)image[AT_TRACK_LENGTH + 1] << 8);
      }
      if (track_length < TRACK_HEADER_SIZE) {
        return fail(problem, "a track is shorter than the header it must carry");
      }
      if (at > length || length - at < track_length) {
        return fail(problem, "the image ends in the middle of a track");
      }
      if (!floppy_add_track(floppy, cylinder, side)) {
        return fail(problem, "the image has more tracks than the medium holds");
      }
      if (!read_track(floppy, image, at, track_length, extended, cylinder, side, problem)) {
        return false;
      }
      at += track_length;
    }
  }
  return true;
}

bool dsk_read(floppy_t *floppy, const uint8_t *image, size_t length, const char **problem) {
  *problem = NULL;
  if (read_image(floppy, image, length, problem)) {
    return true;
  }
  /* A refused image leaves nothing mounted. Reading stops at the first
     defect, so the tracks read before it describe a disc that was never
     whole, and a caller that ignored the return would be handed one. */
  floppy_init(floppy);
  return false;
}
