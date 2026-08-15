/*
 * monitor.h — a cathode ray tube.
 *
 * The monitor receives what the video cable carries: a stream of colour
 * samples and one composite sync line. It does not know what a sample
 * means — it paints the value where the beam is standing and lets whoever
 * reads the framebuffer decide what colour that was. It separates line
 * retrace from frame retrace the way the tube's own circuitry does, by how
 * long the sync is held.
 *
 * Nothing here is particular to any computer. A machine that emits
 * composite sync can drive it by saying how many samples make a line, how
 * many lines make a frame, and how long a sync must be held to mean the
 * frame is over.
 *
 * Not modelled: the flywheel a real tube runs its horizontal oscillator on.
 * Every sync edge retraces here, and a frame that outruns the tube clamps
 * at the bottom where a monitor would lose vertical hold and roll.
 *
 * Sources:
 * - "The Amstrad CPC CRTC Compendium" v1.10 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.10-EN.pdf ch. 16.2 — the composite
 *   sync a CTM monitor is given, and ch. 16.2.4 for the measured threshold
 *   below which a monitor can no longer anchor the image vertically.
 * - "The CRTC" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/crtc — the
 *   monitor holds its beam in the top-left corner through the frame
 *   flyback, which is where a frame retrace leaves it.
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 */
#ifndef COLOPHON_MONITOR_H
#define COLOPHON_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  /* Host-provided, width * height samples; the core allocates nothing. The
     beam paints only where it passes, so whatever the host puts here shows
     through wherever the beam never goes. */
  uint8_t *framebuffer;
  uint16_t width;  /* samples across one line */
  uint16_t height; /* lines down one frame */
  /* A sync held at least this many samples means a frame retrace. Anything
     shorter is a line. */
  uint16_t frame_sync_samples;

  uint16_t beam_x;
  uint16_t beam_y;
  uint16_t sync_held;  /* samples the sync has been asserted */
  bool sync;           /* the sync line as of the last run received */
  bool frame_retraced; /* one retrace per sync block; a short pulse re-arms */
} monitor_t;

void monitor_init(monitor_t *monitor, uint8_t *framebuffer, uint16_t width, uint16_t height,
                  uint16_t frame_sync_samples);

/* Receive a run of samples during which the sync line held one level. The
 * bool models assertion, not voltage: composite sync is active low on the
 * cable, and true here means active. */
void monitor_receive(monitor_t *monitor, const uint8_t *samples, uint8_t count, bool sync);

#endif
