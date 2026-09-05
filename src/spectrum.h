/*
 * spectrum.h — the ZX Spectrum 48K, wired.
 *
 * This is the machine file: the one place that knows the chips are soldered
 * into a Spectrum. It owns the memory map, the I/O decode, and the tick that
 * runs the processor and the ULA together; the chips it wires know nothing
 * about it.
 *
 * There is less board here than a CPC has. The ULA is the whole video
 * department, the memory map never moves, and the keyboard hangs off the
 * address lines with nothing in between — so where a CPC's wiring routes a
 * keypress through an 8255 and a sound chip, this one reads the matrix
 * directly.
 *
 * One difference from cpc.c is worth stating because it removes a hazard
 * rather than adding one. A Gate Array holds the processor off the bus with
 * a wait line, which leaves the request pins asserted for as long as the
 * cycle is held, so that machine must take care to dispatch each I/O cycle
 * once. A ULA takes the bus by stopping the processor's clock instead: a
 * held cycle is a tick the processor never runs, so no request is ever
 * presented twice and no such guard is needed.
 *
 * Not wired yet: the ULA reports what a contended access owes, and nothing
 * here acts on it. Every access runs at full speed, so a program timed
 * against the screen — which on this machine is most of them — runs faster
 * than it did.
 *
 * Sources:
 * - "Contended I/O" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended%20I/O — the ULA "pauses
 *   the processor by stopping its clock", which is what the paragraph above
 *   turns on.
 * - "ZX Spectrum ULA" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/ZX%20Spectrum%20ULA — the chip
 *   answers any even port, so the decode is the one address line A0; a read
 *   returns the keyboard on bits 0-4 and the EAR socket on bit 6.
 * - "Keyboard" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Keyboard, and "The Keyboard"
 *   (Chris Smith), http://www.zxdesign.info/keyboard.shtml — the forty keys
 *   as eight half-rows of five, each half-row selected by one of the upper
 *   address lines A8 to A15 held low, and read together when more than one
 *   is.
 */
#ifndef COLOPHON_SPECTRUM_H
#define COLOPHON_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "keyboard.h"
#include "monitor.h"
#include "ula.h"
#include "z80.h"

/* The whole raster the beam covers, at two samples to a T-state. Unlike a
   CPC's, this is an invariant and not a convention: a Spectrum has no
   programmable video counter, so every frame is this long. */
#define SPECTRUM_FRAMEBUFFER_WIDTH (ULA_TICKS_PER_LINE * ULA_SAMPLES_PER_TICK)
#define SPECTRUM_FRAMEBUFFER_HEIGHT ULA_LINES_PER_FRAME
#define SPECTRUM_TICKS_PER_FRAME ULA_TICKS_PER_FRAME

/* Half of the line's own sync, which is where the middle of that pulse lands
   once the beam is timed from it. */
#define SPECTRUM_LINE_SYNC_CENTRE (ULA_RETRACE_TICKS * ULA_SAMPLES_PER_TICK / 2)
/* A sync held this long means the frame is over. No measurement of what a
   television needs was in hand, so this is chosen rather than found: one
   line's worth of samples, comfortably longer than a line's own sync and far
   shorter than the eight lines the frame's runs to. */
#define SPECTRUM_FRAME_SYNC_SAMPLES SPECTRUM_FRAMEBUFFER_WIDTH

/* The memory the board fits: 16K of ROM low, then RAM. Sixteen kilobytes of
   it makes a 16K machine and forty-eight a 48K, which the firmware works out
   for itself by writing to the top of the address space and reading back. */
#define SPECTRUM_ROM_SIZE 0x4000
#define SPECTRUM_RAM_BASE 0x4000
#define SPECTRUM_RAM_16K 0x4000
#define SPECTRUM_RAM_48K 0xC000

/* A key, as the half-row that selects it and the bit that reads it. The
   half-rows are numbered by the address line that selects each: 0 is A8. */
#define SPECTRUM_KEY(half_row, bit) KEYBOARD_KEY(half_row, bit)
#define SPECTRUM_HALF_ROWS 8

/* The four keys that carry no character and so cannot be looked up by one. */
#define SPECTRUM_CAPS_SHIFT SPECTRUM_KEY(0, 0)
#define SPECTRUM_SYMBOL_SHIFT SPECTRUM_KEY(7, 1)
#define SPECTRUM_ENTER SPECTRUM_KEY(6, 0)
#define SPECTRUM_SPACE SPECTRUM_KEY(7, 0)

typedef struct {
  z80_t cpu;
  uint64_t pins; /* the bus between ticks */

  ula_t ula;
  monitor_t monitor;
  keyboard_t keyboard;

  /* What the EAR socket presents on bit 6 of a read. Nothing drives it
     here; on hardware it also hears bit 4 of the last write through the
     board's own resistors, by a route that differs between issues and is
     not modelled. */
  bool ear;

  /* Host-provided storage; the core allocates nothing. */
  const uint8_t *rom; /* SPECTRUM_ROM_SIZE, answering from 0x0000 */
  uint8_t *ram;       /* ram_size bytes, answering from SPECTRUM_RAM_BASE */
  uint32_t ram_size;
} spectrum_t;

/* Power-on. The processor fetches its first instruction from the ROM at
 * 0x0000, which is the only thing that answers there. */
void spectrum_init(spectrum_t *spectrum, uint8_t *ram, uint32_t ram_size, const uint8_t *rom);

/* Plug in a monitor: SPECTRUM_FRAMEBUFFER_WIDTH * SPECTRUM_FRAMEBUFFER_HEIGHT
 * bytes of colour codes, host-owned. Unplugged, the machine runs on and
 * draws into the void, as it would with the aerial out.
 *
 * The picture does not land where the beam drew it. A Spectrum's frame sync
 * is a plain pulse held across eight whole lines, with none of the short
 * pulses a Gate Array leaves inside its own, and a monitor that tells the
 * two syncs apart by length has nothing to re-arm on until the sync ends —
 * so the raster arrives eight lines higher than it was drawn, and the first
 * eight lines of the framebuffer carry the blanking. */
void spectrum_connect_monitor(spectrum_t *spectrum, uint8_t *framebuffer);
#define SPECTRUM_PICTURE_SHIFT ULA_VSYNC_LINES

/* Advance the machine one T-state: the processor and the ULA both run every
 * time. Returns the bus for the host to watch. */
uint64_t spectrum_tick(spectrum_t *spectrum);

/* Tick until the processor is between instructions. A snapshot has nowhere
 * to record a half-executed one, so anything about to take one owes the
 * machine this call first. */
void spectrum_finish_instruction(spectrum_t *spectrum);

/* The processor's view without the processor: a peek under the ROM sees the
 * ROM, and a poke there lands nowhere, as a write from the processor would. */
uint8_t spectrum_peek(const spectrum_t *spectrum, uint16_t address);
void spectrum_poke(spectrum_t *spectrum, uint16_t address, uint8_t value);

#endif
