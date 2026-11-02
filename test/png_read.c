#include "png_read.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t png_read_crc32(const uint8_t *data, size_t length, uint32_t crc) {
  uint32_t table[256];
  for (uint32_t index = 0; index < 256; index++) {
    uint32_t value = index;
    for (int bit = 0; bit < 8; bit++) {
      value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  for (size_t index = 0; index < length; index++) {
    crc = table[(crc ^ data[index]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

uint32_t png_read_adler32(const uint8_t *data, size_t length) {
  uint32_t low = 1;
  uint32_t high = 0;
  for (size_t index = 0; index < length; index++) {
    low = (low + data[index]) % 65521;
    high = (high + low) % 65521;
  }
  return (high << 16) | low;
}

static uint32_t be32_at(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static void describe(char *problem, size_t problem_size, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(problem, problem_size, format, arguments);
  va_end(arguments);
}

static uint8_t *read_whole_file(const char *path, size_t *size_out, char *problem,
                                size_t problem_size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    describe(problem, problem_size, "cannot open %s", path);
    return NULL;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  uint8_t *contents = size > 0 ? malloc((size_t)size) : NULL;
  if (contents == NULL || fread(contents, 1, (size_t)size, file) != (size_t)size) {
    describe(problem, problem_size, "cannot read %s", path);
    free(contents);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size_out = (size_t)size;
  return contents;
}

/* The picture's size, refusing an IHDR the writer would not write, and
   IDAT's contents gathered into one zlib stream; false having described the
   first thing wrong. */
static bool read_chunks(const uint8_t *contents, size_t size, uint32_t *width_out,
                        uint32_t *height_out, uint8_t **stream_out, size_t *stream_size_out,
                        char *problem, size_t problem_size) {
  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (size < 8 || memcmp(contents, signature, 8) != 0) {
    describe(problem, problem_size, "the signature is not a PNG's");
    return false;
  }
  uint8_t *stream = NULL;
  size_t stream_size = 0;
  bool ended = false;
  for (size_t at = 8; at + 12 <= size;) {
    uint32_t length = be32_at(contents + at);
    const uint8_t *type = contents + at + 4;
    const uint8_t *data = contents + at + 8;
    if (length > size - at - 12) {
      describe(problem, problem_size, "a chunk runs past the end of the file");
      free(stream);
      return false;
    }
    uint32_t crc = png_read_crc32(type, 4, 0xFFFFFFFFu);
    crc = png_read_crc32(data, length, crc) ^ 0xFFFFFFFFu;
    if (crc != be32_at(data + length)) {
      describe(problem, problem_size, "the %.4s chunk fails its CRC", (const char *)type);
      free(stream);
      return false;
    }
    if (memcmp(type, "IHDR", 4) == 0) {
      if (length != 13) {
        describe(problem, problem_size, "IHDR holds %u bytes, not 13", length);
        free(stream);
        return false;
      }
      *width_out = be32_at(data);
      *height_out = be32_at(data + 4);
      /* Eight bits a sample, truecolour, deflate, standard filtering and no
         interlace. */
      static const uint8_t written[5] = {8, 2, 0, 0, 0};
      if (memcmp(data + 8, written, sizeof written) != 0) {
        describe(problem, problem_size, "IHDR asks for a picture the writer never writes");
        free(stream);
        return false;
      }
    } else if (memcmp(type, "IDAT", 4) == 0 && length > 0) {
      uint8_t *grown = realloc(stream, stream_size + length);
      if (grown == NULL) {
        describe(problem, problem_size, "cannot hold %u more bytes of IDAT", length);
        free(stream);
        return false;
      }
      stream = grown;
      memcpy(stream + stream_size, data, length);
      stream_size += length;
    } else if (memcmp(type, "IEND", 4) == 0) {
      ended = at + 12 + length == size;
      if (!ended) {
        describe(problem, problem_size, "something follows IEND");
        free(stream);
        return false;
      }
    }
    at += 12 + (size_t)length;
  }
  if (!ended || stream == NULL) {
    describe(problem, problem_size, "the file has no IEND or no IDAT");
    free(stream);
    return false;
  }
  if (*width_out == 0 || *height_out == 0) {
    describe(problem, problem_size, "the file declares a %ux%u picture", *width_out, *height_out);
    free(stream);
    return false;
  }
  *stream_out = stream;
  *stream_size_out = stream_size;
  return true;
}

/* Inflate, for the one block type the writer emits, into `raw_size` bytes;
   false having described the first thing wrong. */
static bool inflate_stored(const uint8_t *stream, size_t stream_size, uint8_t *raw, size_t raw_size,
                           char *problem, size_t problem_size) {
  /* The zlib header: deflate, a window of at most 32K, no preset dictionary,
     and a check that makes the two bytes a multiple of 31 (RFC 1950, 2.2). */
  if (stream_size < 2 || (stream[0] & 0x0F) != 8 || (stream[0] >> 4) > 7 ||
      (stream[1] & 0x20) != 0 || ((stream[0] << 8) | stream[1]) % 31 != 0) {
    describe(problem, problem_size, "the zlib header is not the writer's kind");
    return false;
  }
  size_t raw_at = 0;
  size_t stream_at = 2;
  bool last = false;
  while (!last) {
    if (stream_at + 5 > stream_size) {
      describe(problem, problem_size, "the stream ends before its last block");
      return false;
    }
    last = (stream[stream_at] & 1) != 0;
    if ((stream[stream_at] & 6) != 0) {
      describe(problem, problem_size, "a block is compressed; only stored blocks are written");
      return false;
    }
    uint16_t block = (uint16_t)(stream[stream_at + 1] | (stream[stream_at + 2] << 8));
    uint16_t complement = (uint16_t)(stream[stream_at + 3] | (stream[stream_at + 4] << 8));
    if ((uint16_t)~block != complement) {
      describe(problem, problem_size, "a block's length and its complement disagree");
      return false;
    }
    if (block > raw_size - raw_at || block > stream_size - stream_at - 5) {
      describe(problem, problem_size, "a block holds more than the image or the stream");
      return false;
    }
    memcpy(raw + raw_at, stream + stream_at + 5, block);
    raw_at += block;
    stream_at += 5 + (size_t)block;
  }
  if (raw_at != raw_size) {
    describe(problem, problem_size, "the blocks hold %zu bytes where the image needs %zu", raw_at,
             raw_size);
    return false;
  }
  if (stream_at + 4 > stream_size ||
      png_read_adler32(raw, raw_size) != be32_at(stream + stream_at)) {
    describe(problem, problem_size, "the stream fails its Adler-32");
    return false;
  }
  return true;
}

uint8_t *png_read(const char *path, uint32_t *width_out, uint32_t *height_out, char *problem,
                  size_t problem_size) {
  size_t size = 0;
  uint8_t *contents = read_whole_file(path, &size, problem, problem_size);
  if (contents == NULL) {
    return NULL;
  }
  uint32_t declared_width = 0;
  uint32_t declared_height = 0;
  uint8_t *stream = NULL;
  size_t stream_size = 0;
  bool read = read_chunks(contents, size, &declared_width, &declared_height, &stream, &stream_size,
                          problem, problem_size);
  free(contents);
  if (!read) {
    return NULL;
  }

  /* A bound on each side keeps the sizes below from wrapping, even where
     size_t is 32 bits; nothing here draws a picture a tenth as wide. */
  if (declared_width > 16384 || declared_height > 16384) {
    describe(problem, problem_size, "a %ux%u picture is larger than any written here",
             declared_width, declared_height);
    free(stream);
    return NULL;
  }
  size_t row_size = (size_t)declared_width * 3 + 1;
  size_t raw_size = row_size * declared_height;
  uint8_t *raw = malloc(raw_size);
  uint8_t *pixels = malloc((size_t)declared_width * declared_height * 3);
  bool ok = raw != NULL && pixels != NULL;
  if (!ok) {
    describe(problem, problem_size, "cannot hold a %ux%u picture", declared_width, declared_height);
  }
  ok = ok && inflate_stored(stream, stream_size, raw, raw_size, problem, problem_size);
  free(stream);

  /* Unfilter, for the one filter the writer uses. */
  for (uint32_t row = 0; ok && row < declared_height; row++) {
    if (raw[row * row_size] != 0) {
      describe(problem, problem_size, "row %u is filtered", row);
      ok = false;
      break;
    }
    memcpy(pixels + (size_t)row * declared_width * 3, raw + row * row_size + 1,
           (size_t)declared_width * 3);
  }
  free(raw);
  if (!ok) {
    free(pixels);
    return NULL;
  }
  *width_out = declared_width;
  *height_out = declared_height;
  return pixels;
}
