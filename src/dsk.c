/*
 * dsk.c — the two disc image layouts, read onto a medium and written back.
 */
#include <string.h>

#include "dsk.h"

#define HEADER_SIZE 256
#define AT_CREATOR 0x22
#define AT_CYLINDERS 0x30
#define AT_SIDES 0x31
#define AT_TRACK_LENGTH 0x32 /* original only: the length every track shares */
#define AT_LENGTH_TABLE 0x34 /* extended only: one high byte per track */

#define TRACK_HEADER_SIZE 256
#define AT_TRACK_CYLINDER 0x10
#define AT_TRACK_SIDE 0x11
#define AT_TRACK_RATE 0x12 /* John Elliott's extension: zero is unknown */
#define AT_TRACK_MODE 0x13
#define AT_TRACK_SIZE_CODE 0x14
#define AT_TRACK_SECTORS 0x15
#define AT_TRACK_GAP 0x16
#define AT_TRACK_FILLER 0x17
#define AT_SECTOR_LIST 0x18
#define SECTOR_ENTRY_SIZE 8

/* The only rate and recording these controllers read. A track declared at
   any other is one their heads find nothing on. */
#define RATE_UNKNOWN 0
#define RATE_DOUBLE_DENSITY 1
#define MODE_UNKNOWN 0
#define MODE_MFM 2

/* Simon Owen's block after the last track: fifteen bytes of tag, then for
   every track a length and a position per sector. */
#define OFFSET_TAG_SIZE 15
static const uint8_t offset_tag[OFFSET_TAG_SIZE] = "Offset-Info\r\n";

/* The track's own size code means nothing in the extended layout, where
   every sector carries its length; on a track with no sectors it is the
   standard formats' 512 bytes. */
#define EMPTY_TRACK_SIZE_CODE 2

static const char extended_tag[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";
static const char track_tag[] = "Track-Info\r\n";
static const char creator[] = "Colophon";

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

/* The track blocks in the order the image holds them, which is the order
   the offsets block describes them in: every block counts, readable or
   not, with the sector count its own header gives. */
typedef struct {
  uint8_t cylinder;
  uint8_t side;
  uint8_t sectors;
} block_order_t;

static bool fail(const char **problem, const char *sentence) {
  *problem = sentence;
  return false;
}

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
  sector->announced = floppy_sector_length(entry[3]);

  uint8_t status1 = entry[4];
  uint8_t status2 = entry[5];
  sector->deleted = (status2 & ST2_CONTROL_MARK) != 0;
  sector->data_crc_error = (status2 & ST2_DATA_FIELD_ERROR) != 0;
  /* A failed check the data field does not own belongs to the identity
     field, which is the only other thing on a track carrying one. */
  sector->identity_crc_error = (status1 & ST1_DATA_ERROR) != 0 && !sector->data_crc_error;
  sector->no_data_field = (status1 & ST1_NO_DATA) != 0 || (status2 & ST2_MISSING_DATA_MARK) != 0;
}

/* The reverse translation, for writing: what a controller would have
   reported having found this. */
static void write_status(const floppy_sector_t *sector, uint8_t *status1, uint8_t *status2) {
  *status1 = 0;
  *status2 = 0;
  if (sector->identity_crc_error || sector->data_crc_error) {
    *status1 |= ST1_DATA_ERROR;
  }
  if (sector->data_crc_error) {
    *status2 |= ST2_DATA_FIELD_ERROR;
  }
  if (sector->deleted) {
    *status2 |= ST2_CONTROL_MARK;
  }
  if (sector->no_data_field) {
    *status1 |= ST1_NO_DATA;
  }
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

static bool readable_recording(const uint8_t *header) {
  uint8_t rate = header[AT_TRACK_RATE];
  uint8_t mode = header[AT_TRACK_MODE];
  return (rate == RATE_UNKNOWN || rate == RATE_DOUBLE_DENSITY) &&
         (mode == MODE_UNKNOWN || mode == MODE_MFM);
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
  uint32_t allotment = floppy_sector_length(header[AT_TRACK_SIZE_CODE]);

  /* A track recorded at a rate or in a mode these controllers cannot
     decode holds nothing their heads can find; it keeps its whole block as
     its room, so that a write of the image keeps it too. */
  if (!readable_recording(header)) {
    if (!floppy_add_track(floppy, cylinder, side, (uint32_t)at, (uint32_t)track_length,
                          header[AT_TRACK_GAP], header[AT_TRACK_FILLER])) {
      return fail(problem, "the image has more tracks than the medium holds");
    }
    floppy->tracks[cylinder][side].unreadable = true;
    return true;
  }
  if (!floppy_add_track(floppy, cylinder, side, (uint32_t)(at + TRACK_HEADER_SIZE), (uint32_t)room,
                        header[AT_TRACK_GAP], header[AT_TRACK_FILLER])) {
    return fail(problem, "the image has more tracks than the medium holds");
  }
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
  floppy_layout_track(floppy, cylinder, side);
  return true;
}

static uint16_t read_word(const uint8_t *at) { return (uint16_t)(at[0] | (at[1] << 8)); }

/* Where the sectors really lay, if the image says. The block is optional
   and may be short; a track it does not reach, or describes impossibly,
   keeps the layout the formatter would have given it. Which byte of the
   identity field the offset measures to is not written down anywhere, so
   it is taken as the first of its sync, and is off by at most the sixteen
   bytes to the mark if it was that. */
static void read_offsets(floppy_t *floppy, const uint8_t *image, size_t length, size_t at,
                         const block_order_t *order, size_t blocks) {
  if (at > length || length - at < sizeof offset_tag ||
      memcmp(image + at, offset_tag, sizeof offset_tag) != 0) {
    return;
  }
  at += sizeof offset_tag;
  for (size_t block = 0; block < blocks; block++) {
    floppy_track_t *track = &floppy->tracks[order[block].cylinder][order[block].side];
    size_t entry = 2 + (size_t)order[block].sectors * 2;
    if (length - at < entry) {
      return;
    }
    if (!track->unreadable) {
      /* Sectors pass in the order they are listed, so their offsets must
         climb; a block that says otherwise is not believed. */
      uint32_t track_length = read_word(image + at);
      bool plausible = track_length > 0;
      uint32_t previous = 0;
      for (uint8_t index = 0; index < track->sector_count && plausible; index++) {
        uint32_t offset = read_word(image + at + 2 + (size_t)index * 2);
        plausible = offset < track_length && (index == 0 || offset > previous);
        previous = offset;
      }
      if (plausible) {
        for (uint8_t index = 0; index < track->sector_count; index++) {
          track->sectors[index].position = read_word(image + at + 2 + (size_t)index * 2);
        }
        floppy_set_track_length(floppy, order[block].cylinder, order[block].side, track_length);
      }
    }
    at += entry;
  }
}

static bool read_image(floppy_t *floppy, uint8_t *image, size_t length, const char **problem) {
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

  block_order_t order[FLOPPY_MAX_CYLINDERS * FLOPPY_MAX_SIDES];
  size_t blocks = 0;
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
      if (!read_track(floppy, image, at, track_length, extended, cylinder, side, problem)) {
        return false;
      }
      order[blocks].cylinder = cylinder;
      order[blocks].side = side;
      order[blocks].sectors = image[at + AT_TRACK_SECTORS];
      blocks++;
      at += track_length;
    }
  }
  if (extended) {
    read_offsets(floppy, image, length, at, order, blocks);
  }
  return true;
}

bool dsk_read(floppy_t *floppy, uint8_t *image, size_t length, const char **problem) {
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

/* What a track takes in the image: its header and its data, in whole
   256-byte units, which is the only size the table can express. A track
   too big for the table's byte cannot be written at all, nor one whose
   length or positions the offsets block's words cannot hold. A track the
   head cannot decode keeps the block it was read from. */
static size_t track_block_size(const floppy_track_t *track) {
  if (track->unreadable) {
    return track->room;
  }
  if (track->length > 0xFFFF) {
    return 0;
  }
  size_t data = 0;
  for (uint8_t index = 0; index < track->sector_count; index++) {
    const floppy_sector_t *sector = &track->sectors[index];
    if (sector->position > 0xFFFF) {
      return 0;
    }
    data += (size_t)sector->recorded * sector->copies;
  }
  size_t block = TRACK_HEADER_SIZE + ((data + 255) & ~(size_t)255);
  return block > (size_t)255 * 256 ? 0 : block;
}

static const uint8_t *unreadable_block(const floppy_t *floppy, const floppy_track_t *track) {
  return floppy->image + track->image_offset;
}

static uint8_t sectors_written(const floppy_t *floppy, const floppy_track_t *track) {
  return track->unreadable ? unreadable_block(floppy, track)[AT_TRACK_SECTORS]
                           : track->sector_count;
}

static void put_word(uint8_t *at, uint32_t value) {
  at[0] = (uint8_t)value;
  at[1] = (uint8_t)(value >> 8);
}

static void write_track(const floppy_t *floppy, const floppy_track_t *track, uint8_t cylinder,
                        uint8_t side, uint8_t *out) {
  memset(out, 0, TRACK_HEADER_SIZE);
  memcpy(out, track_tag, sizeof track_tag);
  out[AT_TRACK_CYLINDER] = cylinder;
  out[AT_TRACK_SIDE] = side;
  out[AT_TRACK_SIZE_CODE] = track->sector_count > 0 ? track->sectors[0].n : EMPTY_TRACK_SIZE_CODE;
  out[AT_TRACK_SECTORS] = track->sector_count;
  out[AT_TRACK_GAP] = track->gap;
  out[AT_TRACK_FILLER] = track->filler;
  uint8_t *data = out + TRACK_HEADER_SIZE;
  for (uint8_t index = 0; index < track->sector_count; index++) {
    const floppy_sector_t *sector = &track->sectors[index];
    uint8_t *entry = out + AT_SECTOR_LIST + (size_t)index * SECTOR_ENTRY_SIZE;
    entry[0] = sector->c;
    entry[1] = sector->h;
    entry[2] = sector->r;
    entry[3] = sector->n;
    write_status(sector, &entry[4], &entry[5]);
    size_t stored = (size_t)sector->recorded * sector->copies;
    put_word(entry + 6, (uint32_t)stored);
    if (stored > 0) {
      memcpy(data, floppy->image + sector->image_offset, stored);
    }
    data += stored;
  }
}

size_t dsk_write(const floppy_t *floppy, uint8_t *out, size_t capacity) {
  size_t total = HEADER_SIZE + sizeof offset_tag;
  for (uint8_t cylinder = 0; cylinder < floppy->cylinders; cylinder++) {
    for (uint8_t side = 0; side < floppy->sides; side++) {
      const floppy_track_t *track = &floppy->tracks[cylinder][side];
      if (!track->formatted) {
        continue;
      }
      size_t block = track_block_size(track);
      if (block == 0) {
        return 0;
      }
      total += block + 2 + (size_t)sectors_written(floppy, track) * 2;
    }
  }
  if (out == NULL || capacity < total) {
    return total;
  }

  memset(out, 0, HEADER_SIZE);
  memcpy(out, extended_tag, sizeof extended_tag - 1);
  memcpy(out + AT_CREATOR, creator, sizeof creator - 1);
  /* A disc with no track on it is still a disc with a side. */
  out[AT_CYLINDERS] = floppy->cylinders > 0 ? floppy->cylinders : 1;
  out[AT_SIDES] = floppy->sides > 0 ? floppy->sides : 1;
  size_t at = HEADER_SIZE;
  for (uint8_t cylinder = 0; cylinder < floppy->cylinders; cylinder++) {
    for (uint8_t side = 0; side < floppy->sides; side++) {
      const floppy_track_t *track = &floppy->tracks[cylinder][side];
      if (!track->formatted) {
        continue;
      }
      size_t block = track_block_size(track);
      out[AT_LENGTH_TABLE + (size_t)cylinder * floppy->sides + side] = (uint8_t)(block / 256);
      if (track->unreadable) {
        memcpy(out + at, unreadable_block(floppy, track), block);
      } else {
        memset(out + at, 0, block);
        write_track(floppy, track, cylinder, side, out + at);
      }
      at += block;
    }
  }
  memcpy(out + at, offset_tag, sizeof offset_tag);
  at += sizeof offset_tag;
  for (uint8_t cylinder = 0; cylinder < floppy->cylinders; cylinder++) {
    for (uint8_t side = 0; side < floppy->sides; side++) {
      const floppy_track_t *track = &floppy->tracks[cylinder][side];
      if (!track->formatted) {
        continue;
      }
      put_word(out + at, track->length);
      at += 2;
      for (uint8_t index = 0; index < sectors_written(floppy, track); index++) {
        put_word(out + at, track->unreadable ? 0 : track->sectors[index].position);
        at += 2;
      }
    }
  }
  return total;
}
