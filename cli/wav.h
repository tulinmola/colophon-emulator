/*
 * wav.h — a WAV writer, the little of one we need.
 *
 * Sixteen-bit PCM, any number of channels interleaved, at any whole rate.
 * The samples are written as they come, and the two sizes the header
 * carries are filled in when the file is closed.
 *
 * Sources:
 * - Peter Kabal, "Audio File Format Specifications: WAVE",
 *   https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html —
 *   the RIFF chunk, the fmt chunk for WAVE_FORMAT_PCM and the data chunk,
 *   little-endian throughout, after Microsoft's "Multimedia Programming
 *   Interface and Data Specifications 1.0" of 1991.
 */
#ifndef COLOPHON_WAV_H
#define COLOPHON_WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  FILE *file;
  const char *path;
  uint32_t rate;
  uint16_t channels;
  uint32_t frames;
  /* An error writing, and frames refused for the 4GiB the sizes can hold;
     the file is closed either way as the frames it does hold. */
  bool failed;
  bool full;
} wav_t;

/* Begin a file of `channels` interleaved channels at `rate` frames a
 * second. Returns false if it could not be created, having said so. */
bool wav_open(wav_t *wav, const char *path, uint32_t rate, uint16_t channels);

/* Append `frames` frames, each `channels` samples. */
void wav_write(wav_t *wav, const int16_t *samples, size_t frames);

/* Fill in the sizes and close. Returns false if anything along the way
 * failed or was refused, having said what. */
bool wav_close(wav_t *wav);

#endif
