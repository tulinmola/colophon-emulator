/*
 * floppy.c — the medium's own bookkeeping.
 */
#include "floppy.h"

void floppy_init(floppy_t *floppy) { *floppy = (floppy_t){0}; }

void floppy_mount(floppy_t *floppy, const uint8_t *image, size_t length) {
  floppy_init(floppy);
  floppy->image = image;
  floppy->image_length = length;
}

static floppy_track_t *track_at(floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  if (cylinder >= FLOPPY_MAX_CYLINDERS || side >= FLOPPY_MAX_SIDES) {
    return NULL;
  }
  return &floppy->tracks[cylinder][side];
}

bool floppy_add_track(floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  floppy_track_t *track = track_at(floppy, cylinder, side);
  if (track == NULL) {
    return false;
  }
  track->formatted = true;
  track->sector_count = 0;
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
  track->sectors[track->sector_count++] = *sector;
  return true;
}

static const floppy_track_t *readable_track(const floppy_t *floppy, uint8_t cylinder,
                                            uint8_t side) {
  if (cylinder >= FLOPPY_MAX_CYLINDERS || side >= FLOPPY_MAX_SIDES) {
    return NULL;
  }
  const floppy_track_t *track = &floppy->tracks[cylinder][side];
  return track->formatted ? track : NULL;
}

bool floppy_track_formatted(const floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  return readable_track(floppy, cylinder, side) != NULL;
}

uint8_t floppy_sector_count(const floppy_t *floppy, uint8_t cylinder, uint8_t side) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  return track == NULL ? 0 : track->sector_count;
}

const floppy_sector_t *floppy_sector(const floppy_t *floppy, uint8_t cylinder, uint8_t side,
                                     uint8_t index) {
  const floppy_track_t *track = readable_track(floppy, cylinder, side);
  if (track == NULL || index >= track->sector_count) {
    return NULL;
  }
  return &track->sectors[index];
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
  size_t from = sector->image_offset + (size_t)(copy % sector->copies) * sector->recorded + offset;
  for (uint32_t index_in_read = 0; index_in_read < count; index_in_read++) {
    out[index_in_read] = floppy->image[from + index_in_read];
  }
  return count;
}
