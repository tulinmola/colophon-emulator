/*
 * png.h — a PNG writer, the little of one we need.
 *
 * Eight-bit RGB, no interlacing, no filtering, and deflate that never
 * compresses: the format allows stored blocks, so the whole encoder is two
 * checksums and some framing. The file is larger than it would be from a
 * real compressor and opens in everything.
 *
 * Sources:
 * - "Portable Network Graphics (PNG) Specification (Third Edition)",
 *   https://www.w3.org/TR/png-3/ — the signature, the chunk layout, the
 *   CRC-32 the chunks carry, and IHDR/IDAT/IEND.
 * - RFC 1950, https://www.rfc-editor.org/rfc/rfc1950 — the zlib wrapper
 *   IDAT holds, and its Adler-32.
 * - RFC 1951, https://www.rfc-editor.org/rfc/rfc1951 — deflate, of which we
 *   use only the stored block: a type of 00, a length, and its complement.
 */
#ifndef COLOPHON_PNG_H
#define COLOPHON_PNG_H

#include <stdbool.h>
#include <stdint.h>

/* Write `width` by `height` pixels, three bytes each in red, green, blue
 * order, row by row from the top. Returns false if the file could not be
 * written, having reported why. */
bool png_write(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height);

#endif
