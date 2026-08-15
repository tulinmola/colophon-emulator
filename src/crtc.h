/*
 * crtc.h — the 6845 CRTC, stepped at its character clock.
 *
 * One crtc_tick() call advances the chip by one character (one CCLK cycle)
 * and returns its output pins: the memory address, the raster address, the
 * syncs and the display enable. The machine wiring decides what those pins
 * mean; this file knows nothing about any machine. Register access arrives
 * asynchronously through crtc_access(), the way the E strobe reaches the
 * chip regardless of CCLK.
 *
 * Implemented: the ideal frame construction of Compendium ch. 6, worn by
 * type 0 (HD6845S/UM6845): its register widths, its readable set, its
 * VMA/VMA' reload rules. Not yet: interlace and skew (R8 is stored, unread),
 * cursor, lightpen, and the per-type divergences — each arrives when Shaker
 * can judge it.
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 *
 * Sources:
 * - "The Amstrad CPC CRTC Compendium" v1.10 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.10-EN.pdf — the counter names we
 *   adopt as it asks (ch. 3.1), the frame construction (ch. 6), the type-0
 *   register file and access ports (ch. 4.3), VMA/VMA' and their reload
 *   rules (ch. 20).
 * - "The CRTC" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/crtc — the
 *   register overview and the five types.
 */
#ifndef COLOPHON_CRTC_H
#define COLOPHON_CRTC_H

#include <stdbool.h>
#include <stdint.h>

/* Bus pin layout in the 64-bit pin mask:
 * bits 0..13  MA0..MA13 (memory address, a character/word address)
 * bits 16..23 D0..D7    (data bus, the same lanes z80.h uses)
 * bits 24..28 RA0..RA4  (raster address)
 * bits 29..   control pins, names as the datasheets print them */
#define CRTC_DISPTMG (1ULL << 29) /* display enable */
#define CRTC_HSYNC (1ULL << 30)
#define CRTC_VSYNC (1ULL << 31)
#define CRTC_CS (1ULL << 32) /* input: chip select */
#define CRTC_RS (1ULL << 33) /* input: register select (0 address, 1 data) */
#define CRTC_RW (1ULL << 34) /* input: direction; the datasheet's R/W, 1 = read */

static inline uint16_t crtc_ma(uint64_t pins) { return (uint16_t)(pins & 0x3FFF); }
static inline uint8_t crtc_ra(uint64_t pins) { return (uint8_t)((pins >> 24) & 0x1F); }
static inline uint8_t crtc_data(uint64_t pins) { return (uint8_t)((pins >> 16) & 0xFF); }
static inline uint64_t crtc_set_data(uint64_t pins, uint8_t data) {
  return (pins & ~0xFF0000ULL) | ((uint64_t)data << 16);
}

typedef struct {
  /* R0-R17, selected through the address register. Writes are masked to the
     documented type-0 widths (Compendium ch. 4.3); R16/R17 ignore writes. */
  uint8_t registers[18];
  uint8_t address_register; /* AR, 5 bits: the register number a select names */
  uint8_t type;             /* Longshot's numbering 0-4; only type 0 exists here yet */

  /* Counters, named as the Compendium names them (ch. 3.1). */
  uint8_t c0;  /* horizontal character counter */
  uint8_t c9;  /* scanline within the character row; drives RA */
  uint8_t c4;  /* character row counter */
  uint8_t c5;  /* vertical-adjustment scanline counter */
  uint8_t c3l; /* HSYNC width counter, 4 bits: R3 low nibble, 0 counts 16 */
  uint8_t c3h; /* VSYNC scanline counter, 4 bits: R3 high nibble, 0 counts 16 */
  bool in_vertical_adjustment;

  /* VMA and VMA', the two internal pointers (ch. 20): VMA runs, one
     character per tick; VMA' is the transient row latch that captures VMA
     at C0=R1 on a row's last scanline. The underscore renders the prime
     mark. */
  uint16_t vma;
  uint16_t vma_;

  bool hsync;
  bool vsync;
} crtc_t;

/* Power-on. Real silicon leaves the register file undefined; zeroes here,
 * for determinism. */
void crtc_init(crtc_t *crtc);

/* Advance one character clock. Returns the output pins. */
uint64_t crtc_tick(crtc_t *crtc);

/* One bus transaction: CS, RS, RW and the data lanes in; the data lanes out
 * when the chip drives them. Where it does not — a read this type never
 * answers — the data passes through untouched, the bus left floating for
 * the machine to interpret. */
uint64_t crtc_access(crtc_t *crtc, uint64_t pins);

#endif
