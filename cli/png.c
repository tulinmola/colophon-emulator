/*
 * png.c — chunks, two checksums, and deflate's laziest block.
 */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32_of(const uint8_t *data, size_t length, uint32_t crc) {
  for (size_t index = 0; index < length; index++) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; bit++) {
      /* The polynomial the PNG specification prints, reflected. */
      crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
  }
  return crc;
}

static uint32_t adler32_of(const uint8_t *data, size_t length) {
  uint32_t low = 1;
  uint32_t high = 0;
  for (size_t index = 0; index < length; index++) {
    low = (low + data[index]) % 65521;
    high = (high + low) % 65521;
  }
  return (high << 16) | low;
}

static void put_be32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

static bool write_chunk(FILE *file, const char *type, const uint8_t *data, size_t length) {
  uint8_t header[8];
  put_be32(header, (uint32_t)length);
  memcpy(header + 4, type, 4);
  uint32_t crc = crc32_of(header + 4, 4, 0xFFFFFFFFu);
  crc = crc32_of(data, length, crc) ^ 0xFFFFFFFFu;
  uint8_t trailer[4];
  put_be32(trailer, crc);
  return fwrite(header, 1, 8, file) == 8 && fwrite(data, 1, length, file) == length &&
         fwrite(trailer, 1, 4, file) == 4;
}

bool png_write(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height) {
  /* Each row is prefixed with its filter type, which is always none. */
  size_t row_size = (size_t)width * 3 + 1;
  size_t raw_size = row_size * height;
  uint8_t *raw = malloc(raw_size);
  if (raw == NULL) {
    fprintf(stderr, "cannot hold %zu bytes of image\n", raw_size);
    return false;
  }
  for (uint32_t row = 0; row < height; row++) {
    raw[row * row_size] = 0;
    memcpy(raw + row * row_size + 1, pixels + (size_t)row * width * 3, (size_t)width * 3);
  }

  /* Deflate, stored: a two-byte zlib header, then blocks of at most 65535
     bytes each announcing their length and its complement, then Adler-32. */
  size_t block_count = raw_size / 0xFFFF + 1;
  size_t stream_size = 2 + raw_size + block_count * 5 + 4;
  uint8_t *stream = malloc(stream_size);
  if (stream == NULL) {
    free(raw);
    fprintf(stderr, "cannot hold %zu bytes of stream\n", stream_size);
    return false;
  }
  size_t written = 0;
  stream[written++] = 0x78; /* deflate, 32K window */
  stream[written++] = 0x01; /* no preset dictionary, check bits to suit */
  size_t remaining = raw_size;
  const uint8_t *source = raw;
  do {
    uint16_t block = remaining > 0xFFFF ? 0xFFFF : (uint16_t)remaining;
    remaining -= block;
    stream[written++] = remaining == 0 ? 1 : 0; /* the last block says so */
    stream[written++] = (uint8_t)block;
    stream[written++] = (uint8_t)(block >> 8);
    stream[written++] = (uint8_t)~block;
    stream[written++] = (uint8_t)(~block >> 8);
    memcpy(stream + written, source, block);
    written += block;
    source += block;
  } while (remaining > 0);
  put_be32(stream + written, adler32_of(raw, raw_size));
  written += 4;
  free(raw);

  uint8_t header[13];
  put_be32(header, width);
  put_be32(header + 4, height);
  header[8] = 8;  /* bits per sample */
  header[9] = 2;  /* colour type: truecolour */
  header[10] = 0; /* compression: deflate */
  header[11] = 0; /* filtering: the standard set */
  header[12] = 0; /* interlacing: none */

  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    free(stream);
    fprintf(stderr, "cannot open %s for writing\n", path);
    return false;
  }
  bool ok = fwrite(signature, 1, 8, file) == 8 && write_chunk(file, "IHDR", header, 13) &&
            write_chunk(file, "IDAT", stream, written) && write_chunk(file, "IEND", NULL, 0);
  free(stream);
  if (fclose(file) != 0) {
    ok = false;
  }
  if (!ok) {
    fprintf(stderr, "cannot write %s\n", path);
  }
  return ok;
}
