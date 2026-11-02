/*
 * png_test — write a PNG, then read it back and insist it says the same.
 *
 * The reader is the tests' own, png_read.h, which says why it cannot agree
 * with a writer that is consistently wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
#include "png_read.h"
#include "test.h"

#define OUTPUT_PATH "build/png_test_output.png"

/* The check values both algorithms publish for the string "123456789". If
   these fail, the reader is not evidence of anything. */
static void the_readers_checksums_match_their_published_values(void) {
  const uint8_t digits[] = "123456789";
  TEST_EQUAL(png_read_crc32(digits, 9, 0xFFFFFFFFu) ^ 0xFFFFFFFFu, 0xCBF43926u);
  TEST_EQUAL(png_read_adler32(digits, 9), 0x091E01DEu);
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
  char problem[256];
  uint8_t *read_back = png_read(OUTPUT_PATH, &read_width, &read_height, problem, sizeof problem);
  if (read_back == NULL) {
    TEST_FAIL("%s", problem);
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
