/*
 * resampler_test — the filter graded as a filter: what it passes, what it
 * lets fold back, and where it puts a step.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "resampler.h"
#include "test.h"

#define PI 3.14159265358979323846

static resampler_t resampler;
static float samples[200000];

/* A square wave between 0 and `level`, toggling every `half` input
   clocks, run for `clocks`; returns the samples written. */
static size_t square(uint32_t half, int32_t level, long clocks) {
  size_t written = 0;
  bool high = false;
  for (long clock = 0; clock < clocks; clock += half) {
    high = !high;
    resampler_change(&resampler, 0, high ? level : 0);
    written += resampler_run(&resampler, half, samples + written,
                             sizeof samples / sizeof samples[0] - written);
  }
  return written;
}

/* |X(k)| of `count` samples from `from`, for a whole number of periods. */
static double magnitude(const float *from, size_t count, size_t bin) {
  double real = 0.0, imaginary = 0.0;
  for (size_t index = 0; index < count; index++) {
    double angle = 2.0 * PI * (double)bin * (double)index / (double)count;
    real += from[index] * cos(angle);
    imaginary -= from[index] * sin(angle);
  }
  return sqrt(real * real + imaginary * imaginary);
}

static void a_level_held_comes_out_exactly(void) {
  resampler_init(&resampler, 1, 48000.0 / 125000.0);
  resampler_change(&resampler, 0, 1000);
  size_t written = resampler_run(&resampler, 125000, samples, 100000);
  TEST_CHECK(written > 47000);
  for (size_t index = (size_t)2 * RESAMPLER_LATENCY; index < written; index++) {
    if (fabsf(samples[index] - 1000.0f) > 1e-3f) {
      TEST_FAIL("sample %zu is %f", index, (double)samples[index]);
      break;
    }
  }
  /* Half a window before the change, the band-limited step has not begun;
     nearer, it rings, as a step through a low-pass filter does. */
  TEST_CHECK(samples[0] == 0.0f);
}

/* A band-limited step is odd about its instant: what it lacks before the
   instant it has to spare after, so a step at a half sample reads the same
   either side of the middle. */
static void a_step_is_centred_on_its_instant(void) {
  resampler_init(&resampler, 1, 1.0 / 16.0);
  size_t written = resampler_run(&resampler, 8, samples, 10); /* half a sample in */
  resampler_change(&resampler, 0, 1000);
  written += resampler_run(&resampler, 16 * 64, samples + written, 100);
  /* The instant is half a sample past the start of sample LATENCY. */
  for (int away = 0; away < RESAMPLER_HALF_WIDTH - 1; away++) {
    float before = samples[RESAMPLER_LATENCY - away];
    float after = samples[RESAMPLER_LATENCY + 1 + away];
    TEST_CHECK(fabsf(before + after - 1000.0f) < 0.05f);
  }
  TEST_CHECK(written > (size_t)2 * RESAMPLER_HALF_WIDTH);
}

/* A square wave holds odd harmonics alone, at 2/(πn) of its swing; the
   rest of what comes out below `band` Hz is what folded back. The wave's
   period in output samples is whole across `count`, so every component
   falls on a bin. A wave above half the output rate has no fundamental in
   the band, and everything there is folded. */
static void grade(double output_rate, uint32_t input_rate, uint32_t half, size_t count, double band,
                  double *fundamental_error_db, double *worst_fold_db) {
  resampler_init(&resampler, 1, output_rate / input_rate);
  size_t written = square(half, 10000, (long)input_rate);
  TEST_CHECK(written >= count + 4096);
  const float *from = samples + 4096;
  double period = 2.0 * half * output_rate / input_rate;
  size_t fundamental = (size_t)floor((double)count / period + 0.5);
  bool in_band = 2.0 * half * output_rate / input_rate > 2.0;
  double expected = 2.0 / PI * 10000.0 * (double)count / 2.0;
  *fundamental_error_db =
      in_band ? 20.0 * log10(magnitude(from, count, fundamental) / expected) : 0.0;
  double worst = 0.0;
  size_t top = (size_t)floor(band * (double)count / output_rate);
  for (size_t bin = 1; bin <= top; bin++) {
    bool harmonic = in_band && bin % fundamental == 0 && (bin / fundamental) % 2 == 1;
    if (!harmonic) {
      double level = magnitude(from, count, bin);
      if (level > worst) {
        worst = level;
      }
    }
  }
  *worst_fold_db = 20.0 * log10(worst / expected + 1e-30);
}

/* A CPC's channel at period 9, 6944Hz, its fifth harmonic at 34.7kHz and
   up from there folding back into the band at 48kHz. */
static void at_48khz_nothing_folds_back_within_80_db(void) {
  double fundamental, fold;
  grade(48000.0, 125000, 9, 8640, 20000.0, &fundamental, &fold);
  printf("    48kHz, 6944Hz: fundamental %+.4f dB, worst fold %.1f dB\n", fundamental, fold);
  TEST_CHECK(fabs(fundamental) < 0.01);
  TEST_CHECK(fold < -80.0);
}

/* The same at 44.1kHz, where the band ends lower, at five twelfths of the
   rate, 18375Hz: above it, and below 20kHz, what folds back from between
   24.1 and 25.7kHz is not yet 80dB down. */
static void at_44100hz_nothing_folds_back_within_80_db(void) {
  double fundamental, fold;
  /* 125000 / 18 against 44100: 147 periods in 1058.4 samples is not
     whole; period 25 makes 2500Hz, 441 samples to 25 periods. */
  grade(44100.0, 125000, 25, 4410, 18375.0, &fundamental, &fold);
  printf("    44.1kHz, 2500Hz: fundamental %+.4f dB, worst fold %.1f dB\n", fundamental, fold);
  TEST_CHECK(fabs(fundamental) < 0.01);
  TEST_CHECK(fold < -80.0);
}

/* High in the band the filter must still pass: 15625Hz, period 4. */
static void the_band_is_flat_to_15khz(void) {
  double fundamental, fold;
  grade(48000.0, 125000, 4, 7680, 20000.0, &fundamental, &fold);
  printf("    48kHz, 15625Hz: fundamental %+.4f dB, worst fold %.1f dB\n", fundamental, fold);
  TEST_CHECK(fabs(fundamental) < 0.01);
  TEST_CHECK(fold < -80.0);
}

/* A tone in the stopband itself, period 2, 31250Hz: at 48kHz it folds to
   16750Hz, 7/12 of the rate being where the filter is first 80dB down, and
   at 44.1kHz to 12850Hz. */
static void a_tone_in_the_stopband_folds_back_80_db_down(void) {
  double fundamental, fold;
  grade(48000.0, 125000, 2, 1920, 20000.0, &fundamental, &fold);
  printf("    48kHz, 31250Hz: worst fold %.1f dB\n", fold);
  TEST_CHECK(fold < -80.0);
  grade(44100.0, 125000, 2, 4410, 18375.0, &fundamental, &fold);
  printf("    44.1kHz, 31250Hz: worst fold %.1f dB\n", fold);
  TEST_CHECK(fold < -80.0);
}

/* A machine picked up already sounding begins at its level, with no step
   up from 0 to ring. */
static void a_channel_started_at_a_level_holds_it_from_the_first_sample(void) {
  resampler_init(&resampler, 1, 0.5);
  resampler_start_at(&resampler, 0, 6359);
  size_t written = resampler_run(&resampler, 400, samples, 1000);
  TEST_CHECK(written > 100);
  for (size_t index = 0; index < written; index++) {
    TEST_CHECK(fabsf(samples[index] - 6359.0f) < 1e-3f);
  }
}

/* A day and more at 48kHz passes 2^32 samples, which time counts afresh
   before then. A resampler started just short of that, and stepped across
   it every few clocks, gives what one started at 0 gives. */
static resampler_t fresh;
static float fresh_samples[4000];

static void time_runs_on_past_a_day(void) {
  resampler_init(&fresh, 1, 0.5);
  resampler_init(&resampler, 1, 0.5);
  resampler.emitted = ((uint64_t)1 << 30) - 40;
  resampler.position = (resampler.emitted + RESAMPLER_HALF_WIDTH) << 32;
  size_t written = 0, written_fresh = 0;
  bool high = false;
  for (int run = 0; run < 400; run++) {
    high = !high;
    resampler_change(&resampler, 0, high ? 1000 : 0);
    resampler_change(&fresh, 0, high ? 1000 : 0);
    written += resampler_run(&resampler, 7, samples + written, 10);
    written_fresh += resampler_run(&fresh, 7, fresh_samples + written_fresh, 10);
  }
  TEST_EQUAL(written, written_fresh);
  TEST_CHECK(resampler.emitted < 64 + written);
  TEST_CHECK(memcmp(samples, fresh_samples, written * sizeof samples[0]) == 0);
}

/* Room for fewer samples than are due: the rest are counted, never
   written past the end. */
static void samples_with_no_room_are_counted_not_written(void) {
  resampler_init(&resampler, 2, 0.5);
  resampler_change(&resampler, 0, 7);
  resampler_change(&resampler, 1, -7);
  float few[10 * 2 + 1];
  few[20] = 12345.0f;
  size_t written = resampler_run(&resampler, 200, few, 10);
  TEST_EQUAL(written, 10);
  /* Time starts half a window in, 16 samples, and 200 clocks at a half take
     it 100 further: samples 0 to 100 are finished. */
  TEST_EQUAL(resampler.dropped, 101 - 10);
  TEST_CHECK(few[20] == 12345.0f);
}

/* A host keeping its sound card fed nudges the ratio; the level carries on
   unbroken across the change. */
static void a_new_ratio_carries_the_level_on(void) {
  resampler_init(&resampler, 1, 0.01);
  resampler_change(&resampler, 0, 500);
  size_t written = resampler_run(&resampler, 10000, samples, 1000);
  resampler_set_ratio(&resampler, 0.01002);
  written += resampler_run(&resampler, 10000, samples + written, 1000);
  for (size_t index = (size_t)2 * RESAMPLER_LATENCY; index < written; index++) {
    TEST_CHECK(fabsf(samples[index] - 500.0f) < 1e-3f);
  }
}

int main(void) {
  TEST_RUN(a_level_held_comes_out_exactly);
  TEST_RUN(a_step_is_centred_on_its_instant);
  TEST_RUN(at_48khz_nothing_folds_back_within_80_db);
  TEST_RUN(at_44100hz_nothing_folds_back_within_80_db);
  TEST_RUN(the_band_is_flat_to_15khz);
  TEST_RUN(a_tone_in_the_stopband_folds_back_80_db_down);
  TEST_RUN(a_channel_started_at_a_level_holds_it_from_the_first_sample);
  TEST_RUN(time_runs_on_past_a_day);
  TEST_RUN(samples_with_no_room_are_counted_not_written);
  TEST_RUN(a_new_ratio_carries_the_level_on);
  return TEST_REPORT("resampler");
}
