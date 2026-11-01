/*
 * resampler.c — band-limited steps, laid down where the levels change.
 */
#include <math.h>
#include <stdbool.h>

#include "resampler.h"

#define FRACTION_BITS 32
#define RING_MASK (RESAMPLER_RING - 1)

/* For 80 dB, β = 0.1102 (A - 8.7); and over a transition of a sixth of the
   sample rate, from 5/12 to 7/12, an order of (A - 8) / (2.285 Δω), 30.1,
   so 31 and 32 taps, an even number, centring the kernel between two. */
#define KAISER_BETA 7.857

/* Integration points to a phase: the step is the running sum of the
   impulse, taken eight times finer than the phases it is kept at. */
#define SUBDIVISIONS 8

#define PI 3.14159265358979323846

static double bessel_i0(double x) {
  double sum = 1.0, term = 1.0, half = x / 2.0;
  for (int k = 1; k < 64; k++) {
    term *= (half / k) * (half / k);
    sum += term;
    if (term < sum * 1e-17) {
      break;
    }
  }
  return sum;
}

/* The impulse of a low-pass at half the sample rate, windowed, at `t`
   samples from its centre. */
static double windowed_sinc(double t) {
  double edge = t / RESAMPLER_HALF_WIDTH;
  if (edge <= -1.0 || edge >= 1.0) {
    return 0.0;
  }
  double sinc = t == 0.0 ? 1.0 : sin(PI * t) / (PI * t);
  return sinc * bessel_i0(KAISER_BETA * sqrt(1.0 - edge * edge)) / bessel_i0(KAISER_BETA);
}

/* The impulse summed by the trapezium rule, eight points to a 256th of a
   sample. Given the whole sum as `total`, each 256th's share of it, less
   the bare step, goes into the kernel. Returns the whole sum. */
static double integrate(resampler_t *resampler, double total) {
  enum { STEPS_PER_SAMPLE = RESAMPLER_PHASES * SUBDIVISIONS };
  enum { POINTS = 2 * RESAMPLER_HALF_WIDTH * STEPS_PER_SAMPLE };
  double sum = 0.0;
  double previous = windowed_sinc(-RESAMPLER_HALF_WIDTH);
  for (int point = 1; point <= POINTS; point++) {
    double t = -RESAMPLER_HALF_WIDTH + (double)point / STEPS_PER_SAMPLE;
    double value = windowed_sinc(t);
    sum += (previous + value) / (2.0 * STEPS_PER_SAMPLE);
    previous = value;
    if (total > 0.0 && point % SUBDIVISIONS == 0) {
      /* The 256th this is, from the window's start; tap j of phase p
         stands (j + 1) * 256 - p of them in, and the edges between taps
         are reached twice, as phase 0 of one and phase 256 of the next. */
      int index = point / SUBDIVISIONS;
      double step = sum / total;
      for (int tap = 0; tap < 2 * RESAMPLER_HALF_WIDTH; tap++) {
        int phase = (tap + 1) * RESAMPLER_PHASES - index;
        if (phase >= 0 && phase <= RESAMPLER_PHASES) {
          /* The bare step lands on the first sample after the instant,
             which is tap HALF_WIDTH, so each tap keeps to one side of it
             at every phase and the kernel has no jump to interpolate
             across. */
          double bare = tap >= RESAMPLER_HALF_WIDTH ? 1.0 : 0.0;
          resampler->residual_kernel[phase][tap] = (float)(step - bare);
        }
      }
    }
  }
  return sum;
}

/* The residual kernel: the band-limited step, scaled to end at 1, less the
   bare step, at every tap and phase. Tap j of phase p is the sample
   (j - HALF_WIDTH + 1) after the whole sample the step falls in, the step
   p 256ths of the way past it. The first pass finds the sum to scale by. */
static void make_kernel(resampler_t *resampler) {
  double total = integrate(resampler, 0.0);
  integrate(resampler, total);
  /* Tap 0 of phase 256 stands at the window's very start, where the step
     has not begun. */
  resampler->residual_kernel[RESAMPLER_PHASES][0] = 0.0f;
}

void resampler_init(resampler_t *resampler, int channels, double ratio) {
  *resampler = (resampler_t){0};
  resampler->channels = channels < 1                    ? 1
                        : channels > RESAMPLER_CHANNELS ? RESAMPLER_CHANNELS
                                                        : channels;
  make_kernel(resampler);
  resampler_set_ratio(resampler, ratio);
  /* Time starts half a window in, so that a level changed at once has
     samples before it to lay its residual on. */
  resampler->position = (uint64_t)RESAMPLER_HALF_WIDTH << FRACTION_BITS;
}

void resampler_set_ratio(resampler_t *resampler, double ratio) {
  resampler->step = (uint64_t)(ratio * 4294967296.0 + 0.5);
}

void resampler_start_at(resampler_t *resampler, int channel, int32_t level) {
  if (channel >= 0 && channel < resampler->channels) {
    resampler->level[channel] = level;
    resampler->running[channel] = level;
  }
}

void resampler_change(resampler_t *resampler, int channel, int32_t level) {
  if (channel < 0 || channel >= resampler->channels) {
    return;
  }
  int64_t delta = level - resampler->level[channel];
  if (delta == 0) {
    return;
  }
  resampler->level[channel] = level;
  uint64_t whole = resampler->position >> FRACTION_BITS;
  uint32_t fraction = (uint32_t)resampler->position;
  uint32_t phase = fraction >> (FRACTION_BITS - 8);
  float between = (float)((fraction >> 8) & 0xFFFF) / 65536.0f;
  const float *before = resampler->residual_kernel[phase];
  const float *after = resampler->residual_kernel[phase + 1];
  float *residuals = resampler->residuals[channel];
  uint64_t first = whole - RESAMPLER_HALF_WIDTH + 1;
  for (int tap = 0; tap < 2 * RESAMPLER_HALF_WIDTH; tap++) {
    float kernel = before[tap] + (after[tap] - before[tap]) * between;
    residuals[(first + (uint64_t)tap) & RING_MASK] += (float)delta * kernel;
  }
  /* The bare step lands on the first sample after the instant; at the
     instant itself the residual holds the band-limited half. */
  resampler->steps[channel][(whole + 1) & RING_MASK] += delta;
}

size_t resampler_run(resampler_t *resampler, uint32_t clocks, float *samples, size_t capacity) {
  resampler->position += (uint64_t)clocks * resampler->step;
  uint64_t now = resampler->position >> FRACTION_BITS;
  size_t written = 0;
  /* A sample is finished once no step yet to come can reach it, which is
     half a window behind now. */
  while (resampler->emitted + RESAMPLER_HALF_WIDTH <= now) {
    uint64_t slot = resampler->emitted & RING_MASK;
    bool room = written < capacity;
    for (int channel = 0; channel < resampler->channels; channel++) {
      resampler->running[channel] += resampler->steps[channel][slot];
      if (room) {
        samples[written * (size_t)resampler->channels + (size_t)channel] =
            (float)resampler->running[channel] + resampler->residuals[channel][slot];
      }
      resampler->steps[channel][slot] = 0;
      resampler->residuals[channel][slot] = 0.0f;
    }
    if (room) {
      written++;
    } else {
      resampler->dropped++;
    }
    resampler->emitted++;
  }
  /* Time is counted afresh, by whole turns of the ring so that every slot
     keeps its sample, long before the 32 bits of whole samples would run
     out, a day into a run at 48kHz. */
  if (resampler->emitted >= (uint64_t)1 << 30) {
    uint64_t turns = resampler->emitted & ~(uint64_t)RING_MASK;
    resampler->emitted -= turns;
    resampler->position -= turns << FRACTION_BITS;
  }
  return written;
}
