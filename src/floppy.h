/*
 * floppy.h — a sectored floppy disc, as a controller finds it.
 *
 * This is the medium: not a file format, and not a drive. Whether a motor
 * turns, which cylinder the head is over, and what belongs in a status
 * register are all somebody else's.
 *
 * The shape is the one an IBM System 34 double-density disc presents to the
 * controller reading it: sectors announcing themselves with four identity
 * bytes, each followed by a data field that may be shorter, longer, or less
 * certain than the identity claims. That shape belongs to the controller
 * family and not to any machine that fitted one, so no machine appears here.
 *
 * Nothing is copied. A floppy borrows the image it was built from and holds
 * offsets into it, so those bytes must outlive it and must not move — the
 * same contract the core already makes for a ROM.
 *
 * Sources:
 * - µPD765A/µPD765B datasheet (NEC), mirrored at
 *   https://cpctech.cpcwiki.de/docs/upd765a/necfdc.htm — the four identity
 *   bytes C, H, R and N a sector announces and the controller matches
 *   against, N counting 128 << N bytes, and the result-phase status
 *   registers whose recorded bits are translated before they reach here, so
 *   that nothing in this file is phrased in a controller's vocabulary.
 */
#ifndef COLOPHON_FLOPPY_H
#define COLOPHON_FLOPPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A track header describes its sectors in the bytes between its own start
   and the 256 where its data begins, eight bytes each from offset 24: 29
   descriptions fit and a thirtieth would overwrite the first sector. */
#define FLOPPY_MAX_SECTORS 29
/* An extended image lists one track length per byte of the 204 that follow
   its header fields, over at most two sides. */
#define FLOPPY_MAX_CYLINDERS 102
#define FLOPPY_MAX_SIDES 2

/* One sector, as the disc announces and records it.
 *
 * A sector's identity is what it claims, not what is true: C and H may
 * disagree with the track the sector lies on, R is unique only by
 * convention, and the data field behind the identity may be missing or the
 * wrong length. Protected discs are built out of exactly those
 * disagreements, so none of them is corrected here.
 *
 * N is three bits wide, so a size code of 8 means the same as 0 (extended
 * image definition, extension 2.1). */
typedef struct {
  uint8_t c; /* the cylinder this sector claims to sit on */
  uint8_t h; /* the head it claims to be under */
  uint8_t r; /* its own number */
  uint8_t n; /* its size code */

  /* What reading the disc found, in the disc's terms rather than in the
     status bits some controller once reported it through. */
  bool deleted;            /* a deleted data address mark, not a normal one */
  bool identity_crc_error; /* the identity field failed its own check */
  bool data_crc_error;     /* the data field failed its check */
  bool no_data_field;      /* an identity with nothing recorded behind it */

  uint32_t announced;    /* 128 << n */
  uint32_t recorded;     /* what one reading of it actually holds */
  uint32_t copies;       /* readings stored back to back; above one where the
                            data field was found to be unstable */
  uint32_t image_offset; /* where the first reading begins in the image */
} floppy_sector_t;

typedef struct {
  bool formatted;
  uint8_t sector_count;
  floppy_sector_t sectors[FLOPPY_MAX_SECTORS];
} floppy_track_t;

typedef struct {
  const uint8_t *image; /* borrowed; must outlive the floppy and not move */
  size_t image_length;
  uint8_t cylinders;
  uint8_t sides;
  floppy_track_t tracks[FLOPPY_MAX_CYLINDERS][FLOPPY_MAX_SIDES];
} floppy_t;

/* An empty disc. */
void floppy_init(floppy_t *floppy);

/* Lay the medium over an image, which every offset added afterwards is
 * checked against. */
void floppy_mount(floppy_t *floppy, const uint8_t *image, size_t length);

/* A reader adds a formatted track and then its sectors in the order they
 * pass under the head. A track never added is unformatted. Both refuse
 * rather than exceed a fixed capacity or accept a sector whose readings
 * fall outside the image. */
bool floppy_add_track(floppy_t *floppy, uint8_t cylinder, uint8_t side);
bool floppy_add_sector(floppy_t *floppy, uint8_t cylinder, uint8_t side,
                       const floppy_sector_t *sector);

/* A cylinder or side past what the disc has answers as unformatted rather
 * than refusing, because that is what a head finds there. */
bool floppy_track_formatted(const floppy_t *floppy, uint8_t cylinder, uint8_t side);
uint8_t floppy_sector_count(const floppy_t *floppy, uint8_t cylinder, uint8_t side);

/* The sectors in the order they pass under the head, which is the only
 * order there is: R does not identify a sector, since a track may announce
 * the same number twice. NULL past the end. */
const floppy_sector_t *floppy_sector(const floppy_t *floppy, uint8_t cylinder, uint8_t side,
                                     uint8_t index);

/* Read from one sector's data field into `out`, starting `offset` bytes in.
 *
 * `copy` chooses between the readings of an unstable sector, taken modulo
 * their number so that any revolution counter will do. Returns how many
 * bytes came from recorded data, which is fewer than `count` when the read
 * runs past what the disc holds; the rest of `out` is left untouched,
 * because the disc has no answer there and inventing one would record
 * something that was never on it. */
uint32_t floppy_read(const floppy_t *floppy, uint8_t cylinder, uint8_t side, uint8_t index,
                     uint32_t copy, uint32_t offset, uint8_t *out, uint32_t count);

#endif
