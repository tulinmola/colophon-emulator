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
 * A track is also a place, and every byte on it has a position. An image
 * records sectors but seldom where they lay, so the medium lays them out
 * the way a formatter would have and lets a head find what lies between —
 * gaps, marks, checks — because a controller told to read past the end of a
 * sector does exactly that, and some discs are protected by what it finds.
 * The medium is sized to the image it borrows, which is where its two
 * capacities come from.
 *
 * Nothing is copied. A floppy borrows the image it was built from and holds
 * offsets into it, so those bytes must outlive it and must not move — the
 * same contract the core already makes for a ROM. Writes land in that
 * image, in place; a track formatted larger than it was takes fresh room
 * past the image's end, as far as the host said the buffer reaches.
 *
 * Sources:
 * - µPD765A/µPD765B datasheet (NEC), mirrored at
 *   https://cpctech.cpcwiki.de/docs/upd765a/necfdc.htm — the four identity
 *   bytes C, H, R and N a sector announces and the controller matches
 *   against, N counting 128 << N bytes, and the result-phase status
 *   registers whose recorded bits are translated before they reach here, so
 *   that nothing in this file is phrased in a controller's vocabulary.
 * - "Further EDSK extensions" (Simon Owen),
 *   https://simonowen.com/misc/extextdsk.txt — the size code as the chip
 *   really counts it: eight and above mean 32K, correcting the earlier rule
 *   that only three bits counted. Arnold's fdc.c records the same table
 *   from a chip it was measured on. Also the sector offsets an image may
 *   carry, and the gap bytes some protections hide data in.
 * - "Extended DiSK image definition" (Kevin Thacker et al.),
 *   https://cpctech.cpcwiki.de/docs/extdsk.html — the data rate: MFM at
 *   the 4MHz clock these controllers run at is a 2µs cell, sixteen to a
 *   byte; and the limits of what an image can list.
 * - MAME, src/lib/formats/flopimg.cpp, build_pc_track_mfm, and
 *   dsk_dsk.cpp — the System 34 layout a formatter writes, byte counts and
 *   fill values, which is how a track is reconstructed here from the
 *   sectors alone; and the revolution of 100000 cells at 300 rpm.
 */
#ifndef COLOPHON_FLOPPY_H
#define COLOPHON_FLOPPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Fixed room, since the core allocates nothing: as much as an image can
   describe, which is 204 tracks over two sides and 29 sectors to a track. */
#define FLOPPY_MAX_SECTORS 29
#define FLOPPY_MAX_CYLINDERS 102
#define FLOPPY_MAX_SIDES 2

/* MFM puts two 2µs cells under each data bit, so a byte takes 32µs; a
   revolution at 300 rpm is 200ms, which is 6250 of them. */
#define FLOPPY_MICROSECONDS_PER_BYTE 32
#define FLOPPY_BYTES_PER_REVOLUTION 6250

/* The System 34 layout, in bytes. A track opens with 80 of gap, 12 of sync,
   the index mark and 50 more of gap before the first sector. Each sector is
   12 of sync, the identity mark and its four bytes, a two-byte check, 22 of
   gap, 12 of sync, the data mark, the data, its check, and the gap the
   formatter was told to leave. */
#define FLOPPY_TRACK_PREAMBLE 146
#define FLOPPY_MARK_BYTES 4       /* three of sync mark and the address mark */
#define FLOPPY_IDENTITY_BYTES 4   /* C, H, R and N */
#define FLOPPY_CHECK_BYTES 2      /* the CRC after a field */
#define FLOPPY_ID_FIELD 16        /* from the sector's start to its C byte */
#define FLOPPY_ID_KNOWN 22        /* the identity and its check have passed */
#define FLOPPY_DATA_FIELD 60      /* from the sector's start to its first byte */
#define FLOPPY_SECTOR_OVERHEAD 62 /* everything but the data and the gap after */
#define FLOPPY_GAP_BYTE 0x4E
#define FLOPPY_DATA_MARK 0xFB
#define FLOPPY_DELETED_DATA_MARK 0xF8

/* The check the controller family computes over a field: CRC-CCITT, run
   from the three mark bytes through the field and its two check bytes,
   which leaves zero when the field is sound. */
#define FLOPPY_CRC_INITIAL 0xFFFF
uint16_t floppy_crc(uint16_t crc, uint8_t byte);

/* One sector, as the disc announces and records it.
 *
 * A sector's identity is what it claims, not what is true: C and H may
 * disagree with the track the sector lies on, R is unique only by
 * convention, and the data field behind the identity may be missing or the
 * wrong length. Protected discs are built out of exactly those
 * disagreements, so none of them is corrected here. */
typedef struct {
  uint8_t c; /* the cylinder this sector claims to sit on */
  uint8_t h; /* the head it claims to be under */
  uint8_t r; /* its own number */
  uint8_t n; /* its size code */

  /* What reading the disc found, in the disc's terms rather than in the
     status bits some controller once reported it through. An image cannot
     say that both checks failed, and a controller would never see it: an
     identity that fails its check is never read past. */
  bool deleted;            /* a deleted data address mark, not a normal one */
  bool identity_crc_error; /* the identity field failed its own check */
  bool data_crc_error;     /* the data field failed its check */
  bool no_data_field;      /* an identity with nothing recorded behind it */

  uint32_t announced;    /* what N counts */
  uint32_t recorded;     /* what one reading of it actually holds */
  uint32_t extent;       /* what the data field occupies on the track: the
                            length it was written with, which is announced
                            unless a reading was clipped short of it or a
                            formatter wrote it another length */
  uint32_t copies;       /* readings stored back to back; above one where the
                            data field was found to be unstable */
  uint32_t image_offset; /* where the first reading begins in the image */
  uint32_t position;     /* where its sync begins, in bytes from the index */
} floppy_sector_t;

typedef struct {
  bool formatted;
  bool unreadable; /* recorded at a rate or in a mode the head cannot decode:
                      its room is the whole of what the image held for it,
                      and a head finds nothing on it */
  uint8_t sector_count;
  uint8_t gap;           /* the gap the formatter left after each data field */
  uint8_t filler;        /* what it filled the data fields with */
  uint32_t length;       /* bytes in one revolution of this track */
  uint32_t image_offset; /* where the track's data begins in the image */
  uint32_t room;         /* how many bytes of image the track may use */
  floppy_sector_t sectors[FLOPPY_MAX_SECTORS];
} floppy_track_t;

typedef struct {
  uint8_t *image; /* borrowed; must outlive the floppy and not move */
  size_t image_length;
  size_t image_capacity; /* how far the buffer reaches past the image */
  uint8_t cylinders;
  uint8_t sides;
  bool write_protected; /* the tab on the disc */
  bool modified;        /* the image no longer matches what was mounted */
  floppy_track_t tracks[FLOPPY_MAX_CYLINDERS][FLOPPY_MAX_SIDES];
} floppy_t;

/* How many bytes a size code announces: 128 << N up to 7, and 32K from 8
 * on, which is how the chip itself counts. */
uint32_t floppy_sector_length(uint8_t size_code);

/* An empty disc. */
void floppy_init(floppy_t *floppy);

/* Lay the medium over an image, which every offset added afterwards is
 * checked against. */
void floppy_mount(floppy_t *floppy, uint8_t *image, size_t length);

/* Say how far the buffer holding the image reaches beyond it. A track
 * formatted larger than the room it had is laid out there. */
void floppy_give_room(floppy_t *floppy, size_t capacity);

/* A reader adds a formatted track and then its sectors in the order they
 * pass under the head, then has the track laid out. A track never added is
 * unformatted. `image_offset` and `room` say where in the image the track's
 * data lives and how much of it a later format may use. All refuse rather
 * than exceed a fixed capacity or accept a sector whose readings fall
 * outside the image. A sector added with no extent is given the one its
 * reading implies. */
bool floppy_add_track(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t image_offset,
                      uint32_t room, uint8_t gap, uint8_t filler);
bool floppy_add_sector(floppy_t *floppy, uint8_t cylinder, uint8_t side,
                       const floppy_sector_t *sector);

/* Give every sector on a track a position, as the formatter would have
 * placed it: the preamble, then each sector after the one before it and its
 * gap. A gap the revolution has no room for is shortened, and sectors the
 * revolution cannot hold at all lengthen it. A reader that knows where the
 * sectors really lay sets their positions itself and calls this with the
 * measured length instead. */
void floppy_layout_track(floppy_t *floppy, uint8_t cylinder, uint8_t side);
void floppy_set_track_length(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t length);

/* A cylinder or side past what the disc has answers as unformatted rather
 * than refusing, because that is what a head finds there; so does a track
 * the head cannot decode. An unformatted track still turns: it has a
 * length and every byte on it is gap. */
bool floppy_track_formatted(const floppy_t *floppy, uint8_t cylinder, uint8_t side);
uint8_t floppy_sector_count(const floppy_t *floppy, uint8_t cylinder, uint8_t side);
uint32_t floppy_track_length(const floppy_t *floppy, uint8_t cylinder, uint8_t side);

/* The sectors in the order they pass under the head, which is the only
 * order there is: R does not identify a sector, since a track may announce
 * the same number twice. NULL past the end. */
const floppy_sector_t *floppy_sector(const floppy_t *floppy, uint8_t cylinder, uint8_t side,
                                     uint8_t index);

/* The sector whose sync begins at `position`, or -1 when nothing does. */
int floppy_sector_beginning_at(const floppy_t *floppy, uint8_t cylinder, uint8_t side,
                               uint32_t position);

/* The byte under the head at `position` on the given revolution: recorded
 * data where the disc has some, and otherwise what the layout puts there —
 * sync, marks, identities, checks and gap. An unstable sector answers with
 * a different reading each revolution, and a check that failed answers with
 * a check that fails. Positions wrap, because the track does. */
uint8_t floppy_byte(const floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t position,
                    uint32_t revolution);

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

/* Put a byte on the disc at `position`. It lands only inside a sector's
 * recorded data, which is the only place the image has room for it;
 * anywhere else it is lost, and the call says so. A write-protected disc
 * takes nothing. */
bool floppy_write_byte(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint32_t position,
                       uint8_t byte);

/* A data field written whole is sound again: its check passes, it reads
 * the same every revolution, and it carries the mark it was written with.
 * Nothing changes on a write-protected disc. */
void floppy_data_written(floppy_t *floppy, uint8_t cylinder, uint8_t side, uint8_t index,
                         bool deleted);

/* Format a track: `count` sectors with the identities given, their data
 * fields `size_code` long whatever N each identity announces, filled with
 * `filler`, `gap` bytes apart. Refuses a write-protected disc, more sectors
 * than a track holds, or a track that fits neither the room it had nor the
 * room past the image, and then changes nothing. */
bool floppy_format_track(floppy_t *floppy, uint8_t cylinder, uint8_t side,
                         const uint8_t (*identities)[FLOPPY_IDENTITY_BYTES], uint8_t count,
                         uint8_t size_code, uint8_t gap, uint8_t filler);

#endif
