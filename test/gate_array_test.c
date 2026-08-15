/*
 * gate_array_test — the chip alone, fed synthetic syncs.
 *
 * The interrupt generator is exercised pulse by pulse: R52 counts ends of
 * HSYNC, so each pulse here is one scanline's worth of edge.
 */
#include "gate_array.h"
#include "test.h"

static gate_array_t gate_array;

/* One HSYNC: assert, then end it — R52 counts the end. */
static void pulse_hsync(void) {
  gate_array_tick(&gate_array, true, false);
  gate_array_tick(&gate_array, false, false);
}

static void pulse_hsyncs(int count) {
  for (int pulse = 0; pulse < count; pulse++) {
    pulse_hsync();
  }
}

static void reset_state(void) {
  gate_array_init(&gate_array);
  TEST_CHECK(gate_array.lower_rom_enabled);
  TEST_CHECK(gate_array.upper_rom_enabled);
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.mode, 0);
  TEST_EQUAL(gate_array.pen, 0);
}

static void pen_selects_and_ink_paints(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x05); /* PENR: pen 5 */
  gate_array_write(&gate_array, 0x54); /* INKR: colour &14 */
  TEST_EQUAL(gate_array.inks[5], 0x14);
  gate_array_write(&gate_array, 0x10); /* PENR: the border */
  gate_array_write(&gate_array, 0x4B); /* INKR: colour &0B */
  TEST_EQUAL(gate_array.inks[16], 0x0B);
  TEST_EQUAL(gate_array.inks[5], 0x14);
}

static void rmr_owns_roms_and_mode(void) {
  gate_array_init(&gate_array);
  gate_array_write(&gate_array, 0x8D); /* RMR: ROMs off, mode 1 */
  TEST_CHECK(!gate_array.lower_rom_enabled);
  TEST_CHECK(!gate_array.upper_rom_enabled);
  TEST_EQUAL(gate_array.mode_pending, 1);
  TEST_EQUAL(gate_array.mode, 0); /* not yet: a mode waits for the HSYNC */
  pulse_hsync();
  TEST_EQUAL(gate_array.mode, 1);
}

static void r52_loops_at_52_and_holds_the_request(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(51);
  TEST_EQUAL(gate_array.r52, 51);
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync();
  TEST_EQUAL(gate_array.r52, 0);
  TEST_CHECK(gate_array.interrupt_request);
  pulse_hsyncs(10); /* nobody acknowledges: the line is maintained */
  TEST_CHECK(gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 10);
}

static void acknowledge_kills_bit_5(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(52); /* request raised, R52 back at 0 */
  pulse_hsyncs(40); /* still unacknowledged; R52 evolves */
  gate_array_interrupt_acknowledged(&gate_array);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 8); /* 40 with bit 5 dead */
  pulse_hsyncs(43);
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync(); /* 8 + 44 = 52 */
  TEST_CHECK(gate_array.interrupt_request);
}

static void rmr_bit_4_clears_counter_and_request(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(30);
  gate_array_write(&gate_array, 0x90); /* RMR with bit 4 */
  TEST_EQUAL(gate_array.r52, 0);
  pulse_hsyncs(52);
  TEST_CHECK(gate_array.interrupt_request);
  gate_array_write(&gate_array, 0x90);
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);
}

static void vsync_check_interrupts_only_from_afar(void) {
  /* Two HSYNCs after a VSYNC begins: interrupt only if bit 5 of R52 is
     set, and R52 returns to 0 either way (Compendium ch. 27.3.2). */
  gate_array_init(&gate_array);
  pulse_hsyncs(40);
  gate_array_tick(&gate_array, false, true); /* VSYNC begins */
  pulse_hsync();                             /* counts: R52 = 41 */
  TEST_CHECK(!gate_array.interrupt_request);
  pulse_hsync(); /* the check: bit 5 of 41 is set */
  TEST_CHECK(gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);

  gate_array_init(&gate_array);
  pulse_hsyncs(20);
  gate_array_tick(&gate_array, false, true);
  pulse_hsyncs(2); /* bit 5 of 21 is clear: too close, no interrupt */
  TEST_CHECK(!gate_array.interrupt_request);
  TEST_EQUAL(gate_array.r52, 0);
}

static void suppression_does_not_revoke_a_held_request(void) {
  gate_array_init(&gate_array);
  pulse_hsyncs(52); /* raised, nobody listening */
  pulse_hsyncs(20);
  gate_array_tick(&gate_array, false, true);
  pulse_hsyncs(2); /* the check suppresses a new request only */
  TEST_CHECK(gate_array.interrupt_request);
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(pen_selects_and_ink_paints);
  TEST_RUN(rmr_owns_roms_and_mode);
  TEST_RUN(r52_loops_at_52_and_holds_the_request);
  TEST_RUN(acknowledge_kills_bit_5);
  TEST_RUN(rmr_bit_4_clears_counter_and_request);
  TEST_RUN(vsync_check_interrupts_only_from_afar);
  TEST_RUN(suppression_does_not_revoke_a_held_request);
  return TEST_REPORT("gate_array");
}
