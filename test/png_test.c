/*
 * png_test — write a PNG, then read it back and insist it says the same.
 *
 * The reader here is the test's own, deliberately: it recomputes both
 * checksums from published check values rather than from the writer's
 * tables, so a writer that is consistently wrong cannot agree with it. It
 * understands only what we emit — truecolour, no filtering, deflate's
 * stored blocks — and complains about anything else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
#include "test.h"

#define OUTPUT_PATH "build/png_test_output.png"

static uint32_t crc32_check(const uint8_t *data, size_t length, uint32_t crc) {
  for (size_t index = 0; index < length; index++) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
  }
  return crc;
}

static uint32_t adler32_check(const uint8_t *data, size_t length) {
  uint32_t low = 1;
  uint32_t high = 0;
  for (size_t index = 0; index < length; index++) {
    low = (low + data[index]) % 65521;
    high = (high + low) % 65521;
  }
  return (high << 16) | low;
}

/* The check values both algorithms publish for the string "123456789". If
   these fail, the reader below is not evidence of anything. */
static void the_readers_checksums_match_their_published_values(void) {
  const uint8_t digits[] = "123456789";
  TEST_EQUAL(crc32_check(digits, 9, 0xFFFFFFFFu) ^ 0xFFFFFFFFu, 0xCBF43926u);
  TEST_EQUAL(adler32_check(digits, 9), 0x091E01DEu);
}

static uint32_t be32_at(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

/* Read the file back into freshly allocated pixels, or return NULL having
   reported the first thing wrong with it. */
static uint8_t *read_png(const char *path, uint32_t *width_out, uint32_t *height_out) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    TEST_FAIL("cannot open %s", path);
    return NULL;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  uint8_t *contents = malloc((size_t)size);
  size_t read = fread(contents, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    TEST_FAIL("short read of %s", path);
    free(contents);
    return NULL;
  }

  static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (size < 8 || memcmp(contents, signature, 8) != 0) {
    TEST_FAIL("signature is not a PNG's");
    free(contents);
    return NULL;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t *stream = NULL;
  size_t stream_size = 0;
  bool ended = false;
  size_t at = 8;
  while (at + 12 <= (size_t)size) {
    uint32_t length = be32_at(contents + at);
    const uint8_t *type = contents + at + 4;
    const uint8_t *data = contents + at + 8;
    if (at + 12 + length > (size_t)size) {
      TEST_FAIL("chunk runs past the end of the file");
      break;
    }
    uint32_t crc = crc32_check(type, 4, 0xFFFFFFFFu);
    crc = crc32_check(data, length, crc) ^ 0xFFFFFFFFu;
    if (crc != be32_at(data + length)) {
      TEST_FAIL("%.4s chunk fails its CRC", (const char *)type);
      break;
    }
    if (memcmp(type, "IHDR", 4) == 0) {
      width = be32_at(data);
      height = be32_at(data + 4);
      TEST_EQUAL(data[8], 8);  /* bits per sample */
      TEST_EQUAL(data[9], 2);  /* truecolour */
      TEST_EQUAL(data[10], 0); /* deflate */
      TEST_EQUAL(data[11], 0); /* standard filtering */
      TEST_EQUAL(data[12], 0); /* not interlaced */
    } else if (memcmp(type, "IDAT", 4) == 0) {
      uint8_t *grown = realloc(stream, stream_size + length);
      if (grown == NULL) {
        TEST_FAIL("cannot hold %u more bytes of IDAT", length);
        break;
      }
      stream = grown;
      memcpy(stream + stream_size, data, length);
      stream_size += length;
    } else if (memcmp(type, "IEND", 4) == 0) {
      ended = true;
    }
    at += 12 + length;
  }
  if (!ended || stream == NULL) {
    TEST_FAIL("the file has no IEND or no IDAT");
    free(contents);
    free(stream);
    return NULL;
  }
  if (width == 0 || height == 0) {
    TEST_FAIL("the file declares a %ux%u picture", width, height);
    free(contents);
    free(stream);
    return NULL;
  }

  /* Inflate, for the one block type we emit. */
  size_t row_size = (size_t)width * 3 + 1;
  size_t raw_size = row_size * height;
  uint8_t *raw = malloc(raw_size);
  size_t raw_at = 0;
  size_t stream_at = 2; /* past the zlib header */
  bool last = false;
  while (!last && stream_at + 4 <= stream_size) {
    last = (stream[stream_at] & 1) != 0;
    if ((stream[stream_at] & 6) != 0) {
      TEST_FAIL("a block is compressed; only stored blocks are written");
      break;
    }
    uint16_t block = (uint16_t)(stream[stream_at + 1] | (stream[stream_at + 2] << 8));
    uint16_t complement = (uint16_t)(stream[stream_at + 3] | (stream[stream_at + 4] << 8));
    if ((uint16_t)~block != complement) {
      TEST_FAIL("a block's length and its complement disagree");
      break;
    }
    if (raw_at + block > raw_size) {
      TEST_FAIL("the blocks hold more than the image needs");
      break;
    }
    memcpy(raw + raw_at, stream + stream_at + 5, block);
    raw_at += block;
    stream_at += 5 + block;
  }
  TEST_EQUAL(raw_at, raw_size);
  TEST_EQUAL(adler32_check(raw, raw_size), be32_at(stream + stream_at));
  free(stream);
  free(contents);

  /* Unfilter, for the one filter we use. */
  uint8_t *pixels = malloc((size_t)width * height * 3);
  for (uint32_t row = 0; row < height; row++) {
    TEST_EQUAL(raw[row * row_size], 0);
    memcpy(pixels + (size_t)row * width * 3, raw + row * row_size + 1, (size_t)width * 3);
  }
  free(raw);
  *width_out = width;
  *height_out = height;
  return pixels;
}

/* Write a gradient, read it back, insist every byte survived. */
static void round_trip(uint32_t width, uint32_t height) {
  uint8_t *pixels = malloc((size_t)width * height * 3);
  for (size_t index = 0; index < (size_t)width * height * 3; index++) {
    pixels[index] = (uint8_t)(index * 7 + index / 13);
  }
  TEST_CHECK(png_write(OUTPUT_PATH, pixels, width, height));
  uint32_t read_width = 0;
  uint32_t read_height = 0;
  uint8_t *read_back = read_png(OUTPUT_PATH, &read_width, &read_height);
  if (read_back == NULL) {
    free(pixels);
    return;
  }
  TEST_EQUAL(read_width, width);
  TEST_EQUAL(read_height, height);
  if (memcmp(pixels, read_back, (size_t)width * height * 3) != 0) {
    TEST_FAIL("the pixels read back differ from the ones written");
  }
  free(read_back);
  free(pixels);
  remove(OUTPUT_PATH);
}

static void a_single_pixel_survives(void) { round_trip(1, 1); }

static void a_small_image_survives(void) { round_trip(16, 9); }

/* Raw bytes past 65535 need more than one stored block, so this exercises
   the block loop and the last-block flag. */
static void an_image_of_several_blocks_survives(void) { round_trip(200, 200); }

static void a_screenshot_sized_image_survives(void) { round_trip(768, 544); }

int main(void) {
  TEST_RUN(the_readers_checksums_match_their_published_values);
  TEST_RUN(a_single_pixel_survives);
  TEST_RUN(a_small_image_survives);
  TEST_RUN(an_image_of_several_blocks_survives);
  TEST_RUN(a_screenshot_sized_image_survives);
  return TEST_REPORT("png");
}
