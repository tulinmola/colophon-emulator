/*
 * dsk.h — reading a disc image onto a floppy, and writing one back.
 *
 * Two formats, one reader. The original stores every track at one length
 * and every sector at one allocation; the extended one gives each track a
 * length of its own and each sector the length it really occupies, which is
 * what lets it describe a disc whose sectors lie about their size or are
 * recorded more than once. Which is in hand is settled by the first eight
 * bytes and never guessed at from the geometry.
 *
 * The reader translates: the status bytes an image records are the result
 * of whatever controller dumped the disc, and they stop here. What crosses
 * into the medium are the findings behind them — a deleted mark, a failed
 * check, an identity with nothing behind it — so that nothing downstream
 * has to hold a datasheet to read a disc.
 *
 * An image is somebody else's file and is trusted for nothing. Every offset
 * is proved to lie inside it before it is read, and a defect that makes
 * further parsing unsafe refuses the whole image rather than returning half
 * a disc that looks whole. A track declared at a rate or in a mode these
 * controllers cannot decode holds nothing their heads can find, and is
 * kept as it was for the writer.
 *
 * The writer knows one format, the extended one, since it is the only one
 * that can say everything the medium holds; and it always records where
 * the sectors lay, so that an image written here and read again is the same
 * disc to the byte.
 *
 * No file handling here: bytes in, bytes out, like everything else in this
 * directory.
 *
 * Sources:
 * - "Disk image file format" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/dsk.html — the original layout: the
 *   256-byte disc and track headers, the single track length that applies
 *   to all of them, the uniform sector allocation, and the status bits an
 *   entry records.
 * - "Extended DiSK image definition" (Kevin Thacker, with extensions by
 *   John Elliott and Simon Owen),
 *   https://cpctech.cpcwiki.de/docs/extdsk.html — the per-track length
 *   table whose zero means a track that was never formatted, the per-sector
 *   stored length, the rule that a stored length which is an exact multiple
 *   of the announced one holds that many readings of an unstable sector,
 *   and the data rate and recording mode a track may declare.
 * - "Further EDSK extensions" (Simon Owen),
 *   https://simonowen.com/misc/extextdsk.txt — the Offset-Info block that
 *   records each sector's distance from the index, and the correction of
 *   the size code to what the chip counts.
 */
#ifndef COLOPHON_DSK_H
#define COLOPHON_DSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "floppy.h"

/* True if these bytes begin either kind of image. */
bool dsk_identify(const uint8_t *image, size_t length);

/* Lay an image over a floppy. Returns false having pointed `problem` at a
 * sentence saying what is wrong with the bytes, leaving the floppy empty.
 * The image is borrowed, not copied: it must outlive the floppy, and it is
 * where the floppy's writes land. */
bool dsk_read(floppy_t *floppy, uint8_t *image, size_t length, const char **problem);

/* Write a floppy out as an extended image. Returns the size the image
 * needs, and fills `out` only when `capacity` holds it, so that a first
 * call with no room measures and a second one writes. Zero means the
 * medium holds a track the format cannot describe. */
size_t dsk_write(const floppy_t *floppy, uint8_t *out, size_t capacity);

#endif
