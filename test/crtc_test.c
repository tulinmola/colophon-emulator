/*
 * crtc_test — the chip alone, held against the Compendium's frame.
 *
 * The registers carry the values the CPC firmware programs — the numbers
 * the Compendium builds its standard frame from (ch. 6.1.5) — and the
 * outputs are recorded for two whole frames: 312 scanlines of 64 characters,
 * 19968 characters each frame, sync for sync.
 */
#include <string.h>

#include "crtc.h"
#include "test.h"

static crtc_t crtc;

/* The standard 50Hz values (Compendium ch. 6.1.5): R0-R9, R12-R13. */
#define FRAME_TICKS 19968
#define SCANLINE 64

static void write_register(int reg, uint8_t value) {
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, (uint8_t)reg));
  crtc_access(&crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, value));
}

static void program_standard(void) {
  static const uint8_t values[14] = {63, 40, 46, 0x8E, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0};
  crtc_init(&crtc);
  for (int reg = 0; reg < 14; reg++) {
    write_register(reg, values[reg]);
  }
}

static uint64_t recorded[2 * FRAME_TICKS];

static void record_two_frames(void) {
  program_standard();
  for (int tick = 0; tick < 2 * FRAME_TICKS; tick++) {
    recorded[tick] = crtc_tick(&crtc);
  }
}

static uint64_t at(int scanline, int character) {
  return recorded[(size_t)scanline * SCANLINE + (size_t)character];
}

static void reset_state(void) {
  crtc_init(&crtc);
  TEST_EQUAL(crtc.c0, 0);
  TEST_EQUAL(crtc.c9, 0);
  TEST_EQUAL(crtc.c4, 0);
  TEST_EQUAL(crtc.address_register, 0);
  TEST_CHECK(!crtc.hsync);
  TEST_CHECK(!crtc.vsync);
  TEST_EQUAL(crtc.type, 0);
}

static void select_wears_five_bits(void) {
  crtc_init(&crtc);
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 0xEC));
  TEST_EQUAL(crtc.address_register, 0x0C);
}

static void writes_wear_the_documented_widths(void) {
  crtc_init(&crtc);
  write_register(4, 0xFF);
  TEST_EQUAL(crtc.registers[4], 0x7F);
  write_register(12, 0xFF);
  TEST_EQUAL(crtc.registers[12], 0x3F);
  write_register(9, 0xFF);
  TEST_EQUAL(crtc.registers[9], 0x1F);
  write_register(16, 0x55); /* lightpen latch: writes fall on deaf ears */
  TEST_EQUAL(crtc.registers[16], 0);
  write_register(20, 0x55); /* no such register */
}

static void type0_reads_r12_to_r17_and_nothing_else(void) {
  program_standard();
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 12));
  TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0x30);
  crtc_access(&crtc, CRTC_CS | crtc_set_data(0, 4));
  TEST_EQUAL(crtc_data(crtc_access(&crtc, CRTC_CS | CRTC_RS | CRTC_RW)), 0);
  /* A status read: type 0 has no status register, the bus stays floating —
     whatever the machine drove passes through. */
  uint64_t floating = crtc_set_data(0, 0x77) | CRTC_CS | CRTC_RW;
  TEST_EQUAL(crtc_data(crtc_access(&crtc, floating)), 0x77);
}

static void unselected_chip_ignores_the_bus(void) {
  crtc_init(&crtc);
  crtc_access(&crtc, crtc_set_data(0, 7)); /* no CS */
  TEST_EQUAL(crtc.address_register, 0);
}

static void hsync_falls_where_r2_and_r3_put_it(void) {
  record_two_frames();
  int pulses = 0;
  for (int tick = 1; tick < 2 * FRAME_TICKS; tick++) {
    if ((recorded[tick] & CRTC_HSYNC) && !(recorded[tick - 1] & CRTC_HSYNC)) {
      pulses++;
    }
  }
  TEST_EQUAL(pulses, 2 * 312);
  /* Width 14 (R3 low nibble), starting at character 46 (R2). */
  TEST_CHECK(!(recorded[45] & CRTC_HSYNC));
  TEST_CHECK(recorded[46] & CRTC_HSYNC);
  TEST_CHECK(recorded[59] & CRTC_HSYNC);
  TEST_CHECK(!(recorded[60] & CRTC_HSYNC));
}

static void vsync_holds_eight_scanlines_from_row_30(void) {
  record_two_frames();
  int rises = 0;
  for (int tick = 1; tick < 2 * FRAME_TICKS; tick++) {
    if ((recorded[tick] & CRTC_VSYNC) && !(recorded[tick - 1] & CRTC_VSYNC)) {
      rises++;
    }
  }
  TEST_EQUAL(rises, 2);
  /* Row 30 (R7) begins at scanline 240; width 8 (R3 high nibble). */
  TEST_CHECK(!(at(239, 63) & CRTC_VSYNC));
  TEST_CHECK(at(240, 0) & CRTC_VSYNC);
  TEST_CHECK(at(247, 63) & CRTC_VSYNC);
  TEST_CHECK(!(at(248, 0) & CRTC_VSYNC));
}

static void display_covers_40_by_200(void) {
  record_two_frames();
  TEST_CHECK(recorded[0] & CRTC_DISPTMG);
  TEST_CHECK(recorded[39] & CRTC_DISPTMG);
  TEST_CHECK(!(recorded[40] & CRTC_DISPTMG));
  TEST_CHECK(at(199, 0) & CRTC_DISPTMG);
  TEST_CHECK(!(at(200, 0) & CRTC_DISPTMG));
  long displayed = 0;
  for (int tick = 0; tick < FRAME_TICKS; tick++) {
    if (recorded[tick] & CRTC_DISPTMG) {
      displayed++;
    }
  }
  TEST_EQUAL(displayed, 40L * 200L);
}

static void the_video_pointer_walks_the_documented_rows(void) {
  record_two_frames();
  /* Frame start: MA = R12/R13, RA = 0. */
  TEST_EQUAL(crtc_ma(recorded[0]), 0x3000);
  TEST_EQUAL(crtc_ra(recorded[0]), 0);
  /* Every scanline of a row replays the row's addresses; RA counts the
     scanlines. */
  TEST_EQUAL(crtc_ma(at(7, 0)), 0x3000);
  TEST_EQUAL(crtc_ra(at(7, 0)), 7);
  /* Rows advance by R1 characters. */
  TEST_EQUAL(crtc_ma(at(8, 0)), 0x3000 + 40);
  TEST_EQUAL(crtc_ra(at(8, 0)), 0);
  TEST_EQUAL(crtc_ma(at(24 * 8, 0)), 0x3000 + 24 * 40);
  /* MA keeps counting past the displayed area. */
  TEST_EQUAL(crtc_ma(recorded[63]), 0x3000 + 63);
}

static void the_frame_locks_at_19968(void) {
  record_two_frames();
  for (int tick = 0; tick < FRAME_TICKS; tick++) {
    TEST_CHECK(recorded[tick] == recorded[tick + FRAME_TICKS]);
    if (recorded[tick] != recorded[tick + FRAME_TICKS]) {
      return; /* one report says it all */
    }
  }
  TEST_EQUAL(crtc_ma(recorded[FRAME_TICKS]), 0x3000);
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(select_wears_five_bits);
  TEST_RUN(writes_wear_the_documented_widths);
  TEST_RUN(type0_reads_r12_to_r17_and_nothing_else);
  TEST_RUN(unselected_chip_ignores_the_bus);
  TEST_RUN(hsync_falls_where_r2_and_r3_put_it);
  TEST_RUN(vsync_holds_eight_scanlines_from_row_30);
  TEST_RUN(display_covers_40_by_200);
  TEST_RUN(the_video_pointer_walks_the_documented_rows);
  TEST_RUN(the_frame_locks_at_19968);
  return TEST_REPORT("crtc");
}
