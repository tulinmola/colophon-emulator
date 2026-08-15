/*
 * gate_array.h — the Amstrad 40010 Gate Array: colour registers, ROM
 * enables, video mode, and the interrupt generator.
 *
 * The chip watches the CRTC's syncs — one gate_array_tick() per character
 * clock — and holds the machine's maskable interrupt line. Register writes
 * arrive through gate_array_write(), dispatched on the data byte the way
 * the silicon dispatches them. The machine wiring decodes the port and
 * carries the INT line to the CPU; this file does neither.
 *
 * Implemented: the registers and the interrupt generator. Not yet: the
 * pixel serialiser and RGB output, and the READY signal that stretches the
 * CPU's cycles — they arrive with the video and timing rungs.
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 *
 * Sources:
 * - "The Amstrad CPC CRTC Compendium" v1.10 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.10-EN.pdf ch. 27 — the interrupt
 *   generator measured on hardware: the R52 counter and its name, the INT
 *   line maintained until acknowledged, bit 5 killed at acknowledge, and
 *   the rule two HSYNCs after VSYNC: an interrupt only if bit 5 is set.
 *   Where "The Gate Array" states that rule inverted, the Compendium is
 *   the one whose reading matches the mechanism's purpose, and the one
 *   tested on silicon.
 * - "The Gate Array" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/gatearray —
 *   the command dispatch on data bits 7-6, the PENR/INKR/RMR layouts, mode
 *   changes taking effect after the next HSYNC.
 * - "Interrupts on the CPC/CPC+ and KC Compact" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/ints.html — RMR bit 4 clears the
 *   pending request along with the counter.
 */
#ifndef COLOPHON_GATE_ARRAY_H
#define COLOPHON_GATE_ARRAY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint8_t pen;      /* the selected colour register: pens 0-15, 16 the border */
  uint8_t inks[17]; /* 5-bit hardware colour codes; [16] is the border */

  uint8_t mode;           /* the video mode in force */
  uint8_t mode_pending;   /* RMR bits 1-0 as last written; a mode change takes
                             effect after the next HSYNC ("The Gate Array") */
  bool lower_rom_enabled; /* RMR bits 2 and 3: a cleared bit enables; these
                             store the enabled state directly */
  bool upper_rom_enabled;

  /* The interrupt generator. R52 is the Compendium's name for the 6-bit
     counter of HSYNC ends (ch. 27.1). The request line, once raised, is
     maintained until acknowledged (ch. 27.3.1). */
  uint8_t r52;
  bool interrupt_request;
  uint8_t hsyncs_until_vsync_check; /* the two-HSYNC delay after a VSYNC
                                       starts (ch. 27.3.2); 0 = not armed */
  bool hsync_previous;
  bool vsync_previous;
} gate_array_t;

/* Power-on. Both ROM enables come up enabled — the reset vector is fetched
 * through the lower ROM, so the silicon can reset no other way; the rest is
 * zeroed by convention. */
void gate_array_init(gate_array_t *gate_array);

/* One command byte, as written to the chip's port. Dispatch is on bits 7-6;
 * bit 5 has no effect on this chip (it selects RMR2 on the Plus ASIC). The
 * 11 pattern is the PAL's MMR, not ours, and is ignored. */
void gate_array_write(gate_array_t *gate_array, uint8_t data);

/* One character clock: watch the syncs, run the interrupt counter. Returns
 * the state of the INT line. */
bool gate_array_tick(gate_array_t *gate_array, bool hsync, bool vsync);

/* The CPU has acknowledged the interrupt: the request drops and bit 5 of
 * R52 dies, so the next interrupt comes no closer than 32 lines — or 20,
 * if the counter had already passed 32 (ch. 27.7.1). */
void gate_array_interrupt_acknowledged(gate_array_t *gate_array);

#endif
