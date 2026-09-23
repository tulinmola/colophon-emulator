/*
 * monitor.c — the beam, and the circuit that separates the two syncs.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "monitor.h"

void monitor_init(monitor_t *monitor, uint8_t *framebuffer, uint16_t width, uint16_t height,
                  uint16_t frame_sync_samples, uint16_t line_sync_centre) {
  *monitor = (monitor_t){0};
  monitor->framebuffer = framebuffer;
  monitor->width = width;
  monitor->height = height;
  monitor->frame_sync_samples = frame_sync_samples;
  monitor->line_sync_centre = line_sync_centre;
}

void monitor_receive(monitor_t *monitor, const uint8_t *samples, uint8_t count, bool sync) {
  if (monitor->framebuffer == NULL) {
    return;
  }

  if (sync && !monitor->sync) {
    /* The sync begins: the beam flies back and starts the next line. A
       frame that outruns the tube is clamped at the bottom; a real monitor
       would lose vertical hold and roll. */
    monitor->beam_x = 0;
    if (monitor->beam_y + 1 < monitor->height) {
      monitor->beam_y++;
    }
    monitor->sync_held = 0;
  } else if (!sync && monitor->sync && monitor->sync_held < monitor->frame_sync_samples) {
    /* A short pulse just ended, so the frame sync is over and the next long
       one may retrace again. During a frame sync every pulse is long — the
       serrations that keep the lines locked are gaps, not pulses — so this
       arms exactly once per frame. */
    monitor->frame_retraced = false;
  }

  if (!sync && monitor->sync && monitor->sync_held <= 2 * monitor->line_sync_centre) {
    /* A line's sync has ended, and only now is its width known. The beam is
       placed as if the line had been timed from the pulse's middle, which is
       what carries a shortened pulse into a picture shifted right by half of
       what was taken off (Compendium ch. 14.4). A pulse too long for a line
       is the frame's, and the beam is left where it stands. */
    monitor->beam_x = (uint16_t)(monitor->line_sync_centre + monitor->sync_held / 2);
  }
  monitor->sync = sync;

  /* A run outside a sync moves the beam along one row: nothing here moves
     it down, so the row is the same for every sample, and the samples that
     land on the raster are the ones before its width. The whole-run step
     stands for the walk while the sum stays inside the counter; a run that
     would carry it past its last value is left to the walk, which brings
     the beam round to the raster's left where this cannot. */
  if (!sync && (uint32_t)monitor->beam_x + count <= UINT16_MAX) {
    if (monitor->beam_y < monitor->height && monitor->beam_x < monitor->width) {
      uint16_t room = (uint16_t)(monitor->width - monitor->beam_x);
      uint8_t painted = count < room ? count : (uint8_t)room;
      memcpy(&monitor->framebuffer[(size_t)monitor->beam_y * monitor->width + monitor->beam_x],
             samples, painted);
    }
    monitor->beam_x = (uint16_t)(monitor->beam_x + count);
    return;
  }

  for (uint8_t index = 0; index < count; index++) {
    if (sync) {
      monitor->sync_held++;
      if (monitor->sync_held == monitor->frame_sync_samples && !monitor->frame_retraced) {
        /* The gun goes to the top-left corner and is held there ("The
           CRTC", Monitor frame-fly), so the frame begins at the origin
           rather than wherever the line had reached. */
        monitor->beam_x = 0;
        monitor->beam_y = 0;
        monitor->frame_retraced = true;
      }
    }
    if (monitor->beam_x < monitor->width && monitor->beam_y < monitor->height) {
      monitor->framebuffer[(size_t)monitor->beam_y * monitor->width + monitor->beam_x] =
          samples[index];
    }
    monitor->beam_x++;
  }
}
