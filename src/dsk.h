/*
 * dsk.h — reading a disc image onto a floppy.
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
 * a disc that looks whole.
 *
 * No file handling here: bytes in, like everything else in this directory.
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
 *   and the three-bit reading of the size code.
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
 * The image is borrowed, not copied: it must outlive the floppy. */
bool dsk_read(floppy_t *floppy, const uint8_t *image, size_t length, const char **problem);

#endif
