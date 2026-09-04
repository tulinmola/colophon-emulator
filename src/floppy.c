/*
 * floppy.c — the medium's own bookkeeping, and the track as a place.
 */
#include "floppy.h"

/* What the layout puts between the sectors and around them. */
#define SYNC_BYTE 0x00
#define MARK_BYTE 0xA1
#define INDEX_MARK_BYTE 0xC2
#define INDEX_MARK 0xFC
#define IDENTITY_MARK 0xFE

#define PREAMBLE_GAP 80
#define SYNC_LENGTH 12
#define MARK_LENGTH 3

uint32_t floppy_sector_length(uint8_t size_code) {
  return size_code >= 8 ? 32768u : 128u << size_code;
}

void floppy_init(floppy_t *floppy) { *floppy = (floppy_t){0}; }

void floppy_mount(floppy_t *floppy, uint8_t *image, size_t length) {
  floppy_init(floppy);
  floppy->image = image;
  floppy->image_length = length;
  floppy->image_capacity = length;
}

void floppy_give_room(floppy_t *floppy, size_t capacity) {
  if (capacity > floppy->image_length) {
    floppy->image_capacity = capacity;
  }
}

static floppy_track_t *track_at(floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  if (cylinder >= FLOPPY_MAX_CYLINDERS || side >= FLOPPY_MAX_SIDES) {
    return NULL;
  }
  return &floppy->tracks[cylinder][side];
}

bool floppy_add_track(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t image_offset,
                      uint32_t room, uint8_t gap, uint8_t filler) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (track == NULL) {
    return false;
  }
  if (image_offset > floppy->image_length || floppy->image_length - image_offset < room) {
    return false;
  }
  track->formatted = true;
  track->unreadable = false;
  track->sector_count = 0;
  track->gap = gap;
  track->filler = filler;
  track->length = FLOPPY_BYTES_PER_REVOLUTION;
  track->image_offset = image_offset;
  track->room = room;
  if (cylinder >= floppy->cylinders) {
    floppy->cylinders = (uint8_t)(cylinder + 1);
  }
  if (side >= floppy->sides) {
    floppy->sides = (uint8_t)(side + 1);
  }
  return true;
}

bool floppy_add_sector(floppy_t *floppy, uint8_t cylinder, uint8_t side,
                       const floppy_sector_t *sector) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (track == NULL || !track->formatted || track->sector_count >= FLOPPY_MAX_SECTORS) {
    return false;
  }
  /* The whole of every reading has to lie inside the image, or a later read
     would run off the end of borrowed memory. Phrased as subtraction from a
     length already known good, so that no sum can wrap past it. */
  uint64_t span = (uint64_t)sector->recorded * (sector->copies > 0 ? sector->copies : 1);
  if (sector->recorded > 0) {
    if (sector->image_offset > floppy->image_length) {
      return false;
    }
    if ((uint64_t)(floppy->image_length - sector->image_offset) < span) {
      return false;
    }
  }
  floppy_sector_t *added = &track->sectors[track->sector_count++];
  *added = *sector;
  /* A reading shorter than the identity announces is one the dumper
     clipped where the next sector began; a longer one carries the check
     and gap behind the field, which lie past the field rather than in it. */
  if (added->extent == 0 && added->recorded > 0 && added->copies > 0 && !added->no_data_field) {
    added->extent = added->recorded < added->announced ? added->recorded : added->announced;
  }
  return true;
}

static uint32_t data_extent(const floppy_sector_t *sector) {
  if (sector->no_data_field || sector->recorded == 0 || sector->copies == 0) {
    return 0;
  }
  return sector->extent;
}

void floppy_layout_track(floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (track == NULL || !track->formatted) {
    return;
  }
  uint32_t without_gaps = FLOPPY_TRACK_PREAMBLE;
  for (uint8_t index = 0; index < track->sector_count; index++) {
    without_gaps += FLOPPY_SECTOR_OVERHEAD + data_extent(&track->sectors[index]);
  }
  /* The last sector's gap runs to the index and counts for nothing. */
  uint32_t gaps = track->sector_count > 1 ? (uint32_t)track->sector_count - 1 : 0;
  uint32_t gap = track->gap;
  uint32_t length = FLOPPY_BYTES_PER_REVOLUTION;
  if (without_gaps > length) {
    gap = 0;
    length = without_gaps;
  } else if (gaps > 0 && without_gaps + gaps * gap > length) {
    gap = (length - without_gaps) / gaps;
  }
  uint32_t position = FLOPPY_TRACK_PREAMBLE;
  for (uint8_t index = 0; index < track->sector_count; index++) {
    track->sectors[index].position = position;
    position += FLOPPY_SECTOR_OVERHEAD + data_extent(&track->sectors[index]) + gap;
  }
  track->length = length;
}

void floppy_set_track_length(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t length) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (track != NULL && track->formatted && length > 0) {
    track->length = length;
  }
}

static const floppy_track_t *readable_track(const floppy_t *floppy, uint8_t cylinder,
                                            uint8_t side) {
  if (cylinder >= FLOPPY_MAX_CYLINDERS || side >= FLOPPY_MAX_SIDES) {
    return NULL;
  }
  const floppy_track_t *track = &floppy->tracks[cylinder][side];
  return track->formatted && !track->unreadable ? track : NULL;
}

bool floppy_track_formatted(const floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  return readable_track(floppy, cylinder, side) != NULL;
}

uint8_t floppy_sector_count(const floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  return track == NULL ? 0 : track->sector_count;
}

uint32_t floppy_track_length(const floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  return track == NULL ? FLOPPY_BYTES_PER_REVOLUTION : track->length;
}

const floppy_sector_t *floppy_sector(const floppy_t *floppy, uint8_t cylinder, uint8_t side,
                                     uint8_t index) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  if (track == NULL || index >= track->sector_count) {
    return NULL;
  }
  return &track->sectors[index];
}

int floppy_sector_beginning_at(const floppy_t *floppy, uint8_t cylinder, uint8_t side,
                               uint32_t position) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  if (track == NULL) {
    return -1;
  }
  for (uint8_t index = 0; index < track->sector_count; index++) {
    if (track->sectors[index].position == position) {
      return index;
    }
  }
  return -1;
}

/* The sector that `position` falls in: the nearest one to begin at or
   before it. Before the first there is only the preamble. */
static int sector_holding(const floppy_track_t *track, uint32_t position) {
  int found = -1;
  for (uint8_t index = 0; index < track->sector_count; index++) {
    uint32_t begins = track->sectors[index].position;
    if (begins <= position && (found < 0 || begins >= track->sectors[found].position)) {
      found = index;
    }
  }
  return found;
}

uint16_t floppy_crc(uint16_t crc, uint8_t byte) {
  crc ^= (uint16_t)(byte << 8);
  for (int bit = 0; bit < 8; bit++) {
    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

/* A check that failed on the disc is given every bit inverted, as MAME's
   track builder does: a check that fails, whatever the field. */
static uint16_t failing(uint16_t crc) { return (uint16_t)~crc; }

static uint16_t marks_crc(uint8_t address_mark) {
  uint16_t crc = FLOPPY_CRC_INITIAL;
  for (int mark = 0; mark < MARK_LENGTH; mark++) {
    crc = floppy_crc(crc, MARK_BYTE);
  }
  return floppy_crc(crc, address_mark);
}

static uint16_t identity_crc(const floppy_sector_t *sector) {
  uint16_t crc = marks_crc(IDENTITY_MARK);
  crc = floppy_crc(crc, sector->c);
  crc = floppy_crc(crc, sector->h);
  crc = floppy_crc(crc, sector->r);
  crc = floppy_crc(crc, sector->n);
  return sector->identity_crc_error ? failing(crc) : crc;
}

static const uint8_t *reading(const floppy_t *floppy, const floppy_sector_t *sector,
                              uint32_t revolution) {
  return floppy->image + sector->image_offset +
         (size_t)(revolution % sector->copies) * sector->recorded;
}

static uint16_t data_crc(const floppy_t *floppy, const floppy_sector_t *sector,
                         uint32_t revolution) {
  const uint8_t *data = reading(floppy, sector, revolution);
  uint16_t crc = marks_crc(sector->deleted ? FLOPPY_DELETED_DATA_MARK : FLOPPY_DATA_MARK);
  for (uint32_t index = 0; index < data_extent(sector); index++) {
    crc = floppy_crc(crc, data[index]);
  }
  return sector->data_crc_error ? failing(crc) : crc;
}

static uint8_t preamble_byte(uint32_t position) {
  if (position < PREAMBLE_GAP) {
    return FLOPPY_GAP_BYTE;
  }
  position -= PREAMBLE_GAP;
  if (position < SYNC_LENGTH) {
    return SYNC_BYTE;
  }
  position -= SYNC_LENGTH;
  if (position < MARK_LENGTH) {
    return INDEX_MARK_BYTE;
  }
  if (position == MARK_LENGTH) {
    return INDEX_MARK;
  }
  return FLOPPY_GAP_BYTE;
}

static uint8_t sector_byte(const floppy_t *floppy, const floppy_sector_t *sector, uint32_t offset,
                           uint32_t revolution) {
  if (offset < SYNC_LENGTH) {
    return SYNC_BYTE;
  }
  if (offset < FLOPPY_ID_FIELD - 1) {
    return MARK_BYTE;
  }
  if (offset == FLOPPY_ID_FIELD - 1) {
    return IDENTITY_MARK;
  }
  if (offset < FLOPPY_ID_FIELD + 4) {
    const uint8_t identity[FLOPPY_IDENTITY_BYTES] = {sector->c, sector->h, sector->r, sector->n};
    return identity[offset - FLOPPY_ID_FIELD];
  }
  if (offset < FLOPPY_ID_KNOWN) {
    uint16_t crc = identity_crc(sector);
    return offset == FLOPPY_ID_KNOWN - 2 ? (uint8_t)(crc >> 8) : (uint8_t)crc;
  }
  uint32_t extent = data_extent(sector);
  if (extent == 0 || offset < FLOPPY_DATA_FIELD - SYNC_LENGTH - MARK_LENGTH - 1) {
    return FLOPPY_GAP_BYTE;
  }
  if (offset < FLOPPY_DATA_FIELD - MARK_LENGTH - 1) {
    return SYNC_BYTE;
  }
  if (offset < FLOPPY_DATA_FIELD - 1) {
    return MARK_BYTE;
  }
  if (offset == FLOPPY_DATA_FIELD - 1) {
    return sector->deleted ? FLOPPY_DELETED_DATA_MARK : FLOPPY_DATA_MARK;
  }
  offset -= FLOPPY_DATA_FIELD;
  if (offset < sector->recorded) {
    return reading(floppy, sector, revolution)[offset];
  }
  if (offset < extent + FLOPPY_CHECK_BYTES) {
    uint16_t crc = data_crc(floppy, sector, revolution);
    return offset == extent ? (uint8_t)(crc >> 8) : (uint8_t)crc;
  }
  return FLOPPY_GAP_BYTE;
}

uint8_t floppy_byte(const floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t position,
                    uint32_t revolution) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  if (track == NULL) {
    return FLOPPY_GAP_BYTE;
  }
  position %= track->length;
  int index = sector_holding(track, position);
  if (index < 0) {
    return preamble_byte(position);
  }
  const floppy_sector_t *sector = &track->sectors[index];
  return sector_byte(floppy, sector, position - sector->position, revolution);
}

uint32_t floppy_read(const floppy_t *floppy, uint8_t cylinder, uint8_t side, uint8_t index,
                     uint32_t copy, uint32_t offset, uint8_t *out, uint32_t count) {
  const floppy_sector_t *sector = floppy_sector(floppy, cylinder, side, index);
  if (sector == NULL || sector->copies == 0 || sector->recorded == 0) {
    return 0;
  }
  if (offset >= sector->recorded) {
    return 0;
  }
  uint32_t available = sector->recorded - offset;
  if (count > available) {
    count = available;
  }
  const uint8_t *from = reading(floppy, sector, copy) + offset;
  for (uint32_t index_in_read = 0; index_in_read < count; index_in_read++) {
    out[index_in_read] = from[index_in_read];
  }
  return count;
}

bool floppy_write_byte(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t position,
                       uint8_t byte) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (floppy->write_protected || track == NULL || !track->formatted) {
    return false;
  }
  position %= track->length;
  int index = sector_holding(track, position);
  if (index < 0) {
    return false;
  }
  floppy_sector_t *sector = &track->sectors[index];
  uint32_t offset = position - sector->position;
  if (offset < FLOPPY_DATA_FIELD || sector->copies == 0) {
    return false;
  }
  offset -= FLOPPY_DATA_FIELD;
  if (offset >= sector->recorded) {
    return false;
  }
  floppy->image[sector->image_offset + offset] = byte;
  floppy->modified = true;
  return true;
}

void floppy_data_written(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint8_t index,
                         bool deleted) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (floppy->write_protected || track == NULL || !track->formatted ||
      index >= track->sector_count) {
    return;
  }
  floppy_sector_t *sector = &track->sectors[index];
  if (sector->copies > 1) {
    sector->copies = 1;
  }
  if (sector->recorded > 0) {
    sector->no_data_field = false;
  }
  sector->data_crc_error = false;
  sector->deleted = deleted;
  floppy->modified = true;
}

bool floppy_format_track(floppy_t *floppy, uint8_t cylinder, uint8_t side,
                         const uint8_t (*identities)[FLOPPY_IDENTITY_BYTES], uint8_t count,
                         uint8_t size_code, uint8_t gap, uint8_t filler) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (floppy->write_protected || track == NULL || !track->formatted || count > FLOPPY_MAX_SECTORS) {
    return false;
  }
  uint32_t field_length = floppy_sector_length(size_code);
  uint64_t needed = (uint64_t)field_length * count;
  if (needed > track->room) {
    /* Fresh room past the image, if the host gave any; the old room is
       left behind. */
    if (needed > floppy->image_capacity - floppy->image_length) {
      return false;
    }
    track->image_offset = (uint32_t)floppy->image_length;
    track->room = (uint32_t)needed;
    floppy->image_length += (size_t)needed;
  }
  track->unreadable = false;
  track->sector_count = 0;
  track->gap = gap;
  track->filler = filler;
  uint32_t offset = track->image_offset;
  for (uint8_t index = 0; index < count; index++) {
    floppy_sector_t sector = {0};
    sector.c = identities[index][0];
    sector.h = identities[index][1];
    sector.r = identities[index][2];
    sector.n = identities[index][3];
    sector.announced = floppy_sector_length(sector.n);
    sector.recorded = field_length;
    sector.extent = field_length;
    sector.copies = 1;
    sector.image_offset = offset;
    for (uint32_t byte = 0; byte < field_length; byte++) {
      floppy->image[offset + byte] = filler;
    }
    offset += field_length;
    track->sectors[track->sector_count++] = sector;
  }
  floppy_layout_track(floppy, cylinder, side);
  floppy->modified = true;
  return true;
}
