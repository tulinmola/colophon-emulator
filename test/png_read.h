/*
 * png_read.h — a PNG reader for the tests, for as little of the format as
 * png.h writes.
 *
 * It is the tests' own, deliberately: its CRC is spelled apart from the
 * writer's, and both its checksums are held to their published check values
 * (png_test.c), so a writer that is consistently wrong cannot agree with it.
 * It understands only what the writer emits — truecolour, no filtering,
 * deflate's stored blocks — and refuses any picture made otherwise; chunks
 * it does not know it passes over. The sources are png.h's.
 */
#ifndef COLOPHON_PNG_READ_H
#define COLOPHON_PNG_READ_H

#include <stddef.h>
#include <stdint.h>

/* The two checksums. The CRC is worked from a table, as the sample code in
 * the PNG specification works it, where the writer works it a bit at a time:
 * two spellings of one algorithm can disagree, and one spelling checking
 * itself cannot. Adler-32 is spelled as the writer and RFC 1950 spell it.
 * The CRC is continued from `crc`; start it at &FFFFFFFF and invert what it
 * returns last. */
uint32_t png_read_crc32(const uint8_t *data, size_t length, uint32_t crc);
uint32_t png_read_adler32(const uint8_t *data, size_t length);

/* Read a file into freshly allocated pixels, three bytes each in red, green,
 * blue order, row by row from the top; or return NULL having written the
 * first thing wrong with it into `problem`. The caller frees the pixels. */
uint8_t *png_read(const char *path, uint32_t *width_out, uint32_t *height_out, char *problem,
                  size_t problem_size);

#endif
