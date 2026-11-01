/*
 * resampler.h — a machine's levels made into samples at a host's rate.
 *
 * A machine's sound leaves it as levels that hold between instants of its
 * own clock, a staircase whose steps come faster than a sound card is
 * usually run. Sampled as it stands, each step moves to the sample after
 * it, and everything the staircase holds above half the sample rate folds
 * back below it. Here each step is laid down instead as the step a low-pass
 * filter at half the output rate makes of it, at its own instant, from a
 * kernel kept at 256ths of a sample and interpolated between them, so the
 * samples are those of the staircase heard through that filter. The work
 * is done once a step, not once an input clock.
 *
 * The filter is a windowed sinc, 32 taps under a Kaiser window, -6 dB at
 * half the output rate: flat to five twelfths of the output rate and 80 dB
 * down from seven twelfths, so at 48kHz whatever folds back below 20kHz
 * comes from above 28kHz and is that far down. The samples come
 * RESAMPLER_LATENCY behind the levels.
 *
 * Sources:
 * - Eli Brandt, "Hard Sync Without Aliasing", Carnegie Mellon, 2001,
 *   https://www.cs.cmu.edu/~eli/papers/icmc01-hardsync.pdf — "pre-integrate
 *   the windowed sinc to get what we might call a BLEP (a band-limited step
 *   function)", and "instead of placing impulses, place BLEPs". Here a BLEP
 *   is kept as the bare step and what it adds beside it, so that a held
 *   level is carried exactly and not summed out of the impulses.
 * - Julius O. Smith III, "Spectral Audio Signal Processing", 2011, "Under
 *   the Hood of kaiserord",
 *   https://ccrma.stanford.edu/~jos/sasp/Hood_kaiserord.html — Kaiser's
 *   formulas for the window's β, 0.1102 (A - 8.7) for an attenuation A over
 *   50dB, and its order, (A - 8) / (2.285 Δω) for a transition Δω.
 */
#ifndef COLOPHON_RESAMPLER_H
#define COLOPHON_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

#define RESAMPLER_CHANNELS 2
#define RESAMPLER_HALF_WIDTH 16
#define RESAMPLER_PHASES 256
#define RESAMPLER_RING 64

/* Output samples between a level and the samples that carry it. */
#define RESAMPLER_LATENCY RESAMPLER_HALF_WIDTH

typedef struct {
  /* What a band-limited step adds beside the bare one, at each tap and at
     each 256th of a sample the step may fall at. */
  float residual_kernel[RESAMPLER_PHASES + 1][2 * RESAMPLER_HALF_WIDTH];
  int channels;
  /* Output samples to an input clock, and the time now in output samples,
     both in 32.32 fixed point. */
  uint64_t step;
  uint64_t position;
  /* The next sample to give out. */
  uint64_t emitted;
  /* Samples given out to nobody, the caller having lent no room for them. */
  uint64_t dropped;
  int64_t level[RESAMPLER_CHANNELS];
  /* The level at the next sample to give out, the steps that change it at
     the samples to come, and the residuals laid down there. */
  int64_t running[RESAMPLER_CHANNELS];
  int64_t steps[RESAMPLER_CHANNELS][RESAMPLER_RING];
  float residuals[RESAMPLER_CHANNELS][RESAMPLER_RING];
} resampler_t;

/* `ratio` is output samples per input clock: 48000 / 4000000 for a CPC's
 * ticks heard at 48kHz, 48000 / 3500000 for a Spectrum's. `channels` is 1
 * or 2, and anything else is taken as the nearer. Every level starts at 0
 * unless resampler_start_at says otherwise. */
void resampler_init(resampler_t *resampler, int channels, double ratio);

/* A channel's level as it stood before the first sample, so that a machine
 * picked up already sounding does not begin with a step up from 0. Only
 * before resampler_change and resampler_run. */
void resampler_start_at(resampler_t *resampler, int channel, int32_t level);

/* A new ratio from now on, for a host keeping its sound card fed. */
void resampler_set_ratio(resampler_t *resampler, double ratio);

/* A channel's level changes now. */
void resampler_change(resampler_t *resampler, int channel, int32_t level);

/* Time moves on by `clocks`, and the samples it finishes are written to
 * `samples`, channels interleaved, up to `capacity` of them; any more are
 * counted in `dropped`. Returns how many were written. A capacity of
 * clocks × ratio + 2 is always enough. */
size_t resampler_run(resampler_t *resampler, uint32_t clocks, float *samples, size_t capacity);

#endif
