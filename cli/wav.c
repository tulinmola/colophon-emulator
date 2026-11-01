/*
 * wav.c — RIFF, fmt and data, written as they are laid out.
 */
#include <string.h>

#include "wav.h"

#define HEADER_BYTES 44
#define WAVE_FORMAT_PCM 1
#define BITS_PER_SAMPLE 16

static void put16(uint8_t *at, uint16_t value) {
  at[0] = (uint8_t)value;
  at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *at, uint32_t value) {
  put16(at, (uint16_t)value);
  put16(at + 2, (uint16_t)(value >> 16));
}

/* The RIFF chunk's size counts everything after its own eight bytes; the
   data chunk's counts the samples' bytes. */
static void fill_header(uint8_t *header, uint32_t rate, uint16_t channels, uint32_t data_bytes) {
  uint16_t block_align = (uint16_t)(channels * BITS_PER_SAMPLE / 8);
  memcpy(header, "RIFF", 4);
  put32(header + 4, HEADER_BYTES - 8 + data_bytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  put32(header + 16, 16);
  put16(header + 20, WAVE_FORMAT_PCM);
  put16(header + 22, channels);
  put32(header + 24, rate);
  put32(header + 28, rate * block_align);
  put16(header + 32, block_align);
  put16(header + 34, BITS_PER_SAMPLE);
  memcpy(header + 36, "data", 4);
  put32(header + 40, data_bytes);
}

bool wav_open(wav_t *wav, const char *path, uint32_t rate, uint16_t channels) {
  *wav = (wav_t){.path = path, .rate = rate, .channels = channels};
  wav->file = fopen(path, "wb");
  if (wav->file == NULL) {
    fprintf(stderr, "cannot create %s\n", path);
    return false;
  }
  uint8_t header[HEADER_BYTES];
  fill_header(header, rate, channels, 0);
  wav->failed = fwrite(header, 1, sizeof header, wav->file) != sizeof header;
  return true;
}

void wav_write(wav_t *wav, const int16_t *samples, size_t frames) {
  /* The sizes the header holds are 32 bits. */
  uint64_t data_bytes = ((uint64_t)wav->frames + frames) * wav->channels * (BITS_PER_SAMPLE / 8);
  if (data_bytes > UINT32_MAX - (HEADER_BYTES - 8)) {
    if (!wav->full) {
      fprintf(stderr, "%s stops at the 4GiB a WAV can hold\n", wav->path);
    }
    wav->full = true;
    return;
  }
  uint8_t bytes[2 * 512];
  size_t count = frames * wav->channels;
  for (size_t from = 0; from < count && !wav->failed; from += 512) {
    size_t chunk = count - from < 512 ? count - from : 512;
    for (size_t index = 0; index < chunk; index++) {
      put16(bytes + 2 * index, (uint16_t)samples[from + index]);
    }
    wav->failed = fwrite(bytes, 2, chunk, wav->file) != chunk;
  }
  wav->frames += (uint32_t)frames;
}

bool wav_close(wav_t *wav) {
  if (wav->file == NULL) {
    return false;
  }
  uint8_t header[HEADER_BYTES];
  fill_header(header, wav->rate, wav->channels,
              wav->frames * wav->channels * (BITS_PER_SAMPLE / 8));
  bool ok = !wav->failed && fseek(wav->file, 0, SEEK_SET) == 0 &&
            fwrite(header, 1, sizeof header, wav->file) == sizeof header;
  ok = fclose(wav->file) == 0 && ok;
  wav->file = NULL;
  if (!ok) {
    fprintf(stderr, "could not write %s\n", wav->path);
  }
  return ok && !wav->full;
}
