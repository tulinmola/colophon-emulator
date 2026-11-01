/*
 * wav_test — write a WAV, then read its bytes back against the layout.
 *
 * The reader is the test's own and knows only the canonical 44-byte header
 * of PCM: every field is checked at the offset the specification gives it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "wav.h"

#define OUTPUT_PATH "build/wav_test_output.wav"

static uint32_t get32(const uint8_t *at) {
  return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static uint16_t get16(const uint8_t *at) { return (uint16_t)(at[0] | at[1] << 8); }

static void a_written_file_reads_back_as_its_layout_says(void) {
  /* More than the writer's chunk of 512 samples, so the chunks join. */
  static int16_t samples[2 * 700];
  for (int index = 0; index < 2 * 700; index++) {
    samples[index] = (int16_t)(index * 37 - 20000);
  }
  wav_t wav;
  TEST_CHECK(wav_open(&wav, OUTPUT_PATH, 125000, 2));
  wav_write(&wav, samples, 300);
  wav_write(&wav, samples + 600, 400);
  TEST_CHECK(wav_close(&wav));

  FILE *file = fopen(OUTPUT_PATH, "rb");
  TEST_CHECK(file != NULL);
  if (file == NULL) {
    return;
  }
  static uint8_t bytes[44 + 4 * 700 + 16];
  size_t size = fread(bytes, 1, sizeof bytes, file);
  fclose(file);
  TEST_EQUAL(size, 44 + 4 * 700);
  TEST_CHECK(memcmp(bytes, "RIFF", 4) == 0);
  TEST_EQUAL(get32(bytes + 4), 36 + 4 * 700);
  TEST_CHECK(memcmp(bytes + 8, "WAVE", 4) == 0);
  TEST_CHECK(memcmp(bytes + 12, "fmt ", 4) == 0);
  TEST_EQUAL(get32(bytes + 16), 16);     /* the fmt chunk's size */
  TEST_EQUAL(get16(bytes + 20), 1);      /* WAVE_FORMAT_PCM */
  TEST_EQUAL(get16(bytes + 22), 2);      /* channels */
  TEST_EQUAL(get32(bytes + 24), 125000); /* frames a second */
  TEST_EQUAL(get32(bytes + 28), 500000); /* bytes a second */
  TEST_EQUAL(get16(bytes + 32), 4);      /* bytes a frame */
  TEST_EQUAL(get16(bytes + 34), 16);     /* bits a sample */
  TEST_CHECK(memcmp(bytes + 36, "data", 4) == 0);
  TEST_EQUAL(get32(bytes + 40), 4 * 700);
  for (int index = 0; index < 2 * 700; index++) {
    TEST_EQUAL((int16_t)get16(bytes + 44 + (size_t)index * 2), samples[index]);
  }
}

/* The header's sizes are 32 bits: frames past them are refused, and the
   file still closes as the frames it holds. */
static void a_file_stops_at_the_4gib_its_sizes_can_hold(void) {
  wav_t wav;
  TEST_CHECK(wav_open(&wav, OUTPUT_PATH, 48000, 2));
  int16_t frame[2] = {1, 2};
  wav_write(&wav, frame, 1);
  /* As though the file already held its last whole frame but one. */
  wav.frames = (UINT32_MAX - 36) / 4 - 1;
  wav_write(&wav, frame, 1);
  TEST_CHECK(!wav.full);
  wav_write(&wav, frame, 1);
  TEST_CHECK(wav.full);
  TEST_CHECK(!wav_close(&wav));
  FILE *file = fopen(OUTPUT_PATH, "rb");
  uint8_t header[44] = {0};
  bool read = file != NULL && fread(header, 1, sizeof header, file) == sizeof header;
  if (file != NULL) {
    fclose(file);
  }
  TEST_CHECK(read);
  if (read) {
    TEST_EQUAL(get32(header + 40), 4 * ((UINT32_MAX - 36) / 4));
  }
}

int main(void) {
  TEST_RUN(a_written_file_reads_back_as_its_layout_says);
  TEST_RUN(a_file_stops_at_the_4gib_its_sizes_can_hold);
  return TEST_REPORT("wav");
}
