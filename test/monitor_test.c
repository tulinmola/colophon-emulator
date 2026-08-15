/*
 * monitor_test — the beam and the sync separator, on a small tube.
 *
 * The monitor here is 32 samples across and 8 lines down, with a frame
 * sync of 12 samples, so a whole frame fits in one screenful of assertions.
 * Runs are 4 samples long, the way a machine would hand over a character.
 */
#include <string.h>

#include "monitor.h"
#include "test.h"

#define WIDTH 32
#define HEIGHT 8
#define FRAME_SYNC 12
#define RUN 4

static uint8_t framebuffer[WIDTH * HEIGHT];
static monitor_t monitor;

static void power_on(void) {
  memset(framebuffer, 0, sizeof framebuffer);
  monitor_init(&monitor, framebuffer, WIDTH, HEIGHT, FRAME_SYNC);
}

/* A run of `count` samples all of one value, with the sync line held. */
static void receive(uint8_t value, uint8_t count, bool sync) {
  uint8_t samples[16];
  for (uint8_t index = 0; index < count; index++) {
    samples[index] = value;
  }
  monitor_receive(&monitor, samples, count, sync);
}

static uint8_t at(int x, int y) { return framebuffer[(size_t)y * WIDTH + x]; }

static void the_beam_paints_where_it_stands(void) {
  power_on();
  receive(0xAA, RUN, false);
  receive(0xBB, RUN, false);
  TEST_EQUAL(monitor.beam_x, 8);
  TEST_EQUAL(monitor.beam_y, 0);
  TEST_EQUAL(at(0, 0), 0xAA);
  TEST_EQUAL(at(3, 0), 0xAA);
  TEST_EQUAL(at(4, 0), 0xBB);
  TEST_EQUAL(at(8, 0), 0); /* untouched: the beam paints only where it goes */
}

static void a_short_sync_starts_the_next_line(void) {
  power_on();
  receive(0x11, RUN, false);
  receive(0, RUN, true); /* 4 samples of sync: shorter than a frame */
  TEST_EQUAL(monitor.beam_y, 1);
  TEST_EQUAL(monitor.beam_x, 4);
  receive(0x22, RUN, false);
  TEST_EQUAL(at(4, 1), 0x22);
}

static void a_long_sync_starts_the_next_frame(void) {
  power_on();
  receive(0x11, RUN, false);
  receive(0, RUN, true);
  receive(0x22, RUN, false);
  receive(0, RUN, true);
  TEST_EQUAL(monitor.beam_y, 2);
  /* Now hold the sync past the frame threshold. */
  receive(0, RUN, false);
  receive(0, RUN, true); /* line retrace: beam_y 3, sync_held 4 */
  receive(0, RUN, true); /* 8 */
  receive(0, RUN, true); /* 12: the frame anchors */
  TEST_EQUAL(monitor.beam_y, 0);
  TEST_CHECK(monitor.frame_retraced);
}

static void one_frame_retrace_per_sync_block(void) {
  power_on();
  /* A frame sync as the Gate Array shapes it: long pulses split by short
     gaps, not short pulses split by long gaps. Only the first anchors. */
  for (int line = 0; line < 4; line++) {
    receive(0, RUN, true);
    receive(0, RUN, true);
    receive(0, RUN, true);
    receive(0, RUN, true); /* 16 samples asserted: past the threshold */
    receive(0, RUN, false);
    if (line == 0) {
      TEST_EQUAL(monitor.beam_y, 0);
    }
  }
  /* Four broad pulses, four lines: the last one left the beam three lines
     down, not back at the top. */
  TEST_EQUAL(monitor.beam_y, 3);
}

static void a_short_pulse_arms_the_next_frame(void) {
  power_on();
  receive(0, RUN * 4, true); /* a frame sync */
  receive(0, RUN, false);
  TEST_EQUAL(monitor.beam_y, 0);
  TEST_CHECK(monitor.frame_retraced);
  receive(0, RUN, true); /* a line's short pulse... */
  receive(0, RUN, false);
  TEST_CHECK(!monitor.frame_retraced); /* ...re-arms the anchor */
  receive(0, RUN * 4, true);
  TEST_EQUAL(monitor.beam_y, 0);
}

static void the_beam_clamps_at_the_bottom(void) {
  power_on();
  for (int line = 0; line < 20; line++) {
    receive(0x33, RUN, true);
    receive(0x33, RUN, false);
  }
  TEST_EQUAL(monitor.beam_y, HEIGHT - 1);
}

static void samples_past_the_edge_are_dropped(void) {
  power_on();
  for (int run = 0; run < 12; run++) {
    receive(0x44, RUN, false); /* 48 samples across a 32-sample line */
  }
  TEST_EQUAL(monitor.beam_x, 48);
  TEST_EQUAL(at(WIDTH - 1, 0), 0x44);
  TEST_EQUAL(at(0, 1), 0); /* it did not wrap onto the next line */
}

static void an_unplugged_monitor_ignores_the_cable(void) {
  monitor_init(&monitor, NULL, WIDTH, HEIGHT, FRAME_SYNC);
  receive(0x55, RUN, false);
  receive(0x55, RUN, true);
  TEST_EQUAL(monitor.beam_x, 0);
  TEST_EQUAL(monitor.beam_y, 0);
}

int main(void) {
  TEST_RUN(the_beam_paints_where_it_stands);
  TEST_RUN(a_short_sync_starts_the_next_line);
  TEST_RUN(a_long_sync_starts_the_next_frame);
  TEST_RUN(one_frame_retrace_per_sync_block);
  TEST_RUN(a_short_pulse_arms_the_next_frame);
  TEST_RUN(the_beam_clamps_at_the_bottom);
  TEST_RUN(samples_past_the_edge_are_dropped);
  TEST_RUN(an_unplugged_monitor_ignores_the_cable);
  return TEST_REPORT("monitor");
}
