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
 * Contention is that same stopped clock, and it is wired here rather than in
 * the chip because it turns on the memory map. Before each tick the
 * processor is asked what it is about to begin; if the address it will
 * present is one the ULA wants, the clock is held for as many T-states as
 * the chip owes, and the beam runs on without it. A port is contended by a
 * rule of its own, which turns on the port's low bit as well as its
 * address.
 *
 * A port nothing answers reads the floating bus: the data bus is in two
 * halves joined by resistors, and the read finds whatever the ULA has on
 * its half at that moment — a byte of the screen it is fetching, or the
 * board's 0xFF when it fetches nothing — which is how a program with no
 * other clock finds the beam. The processor takes the byte on the read's
 * last T-state, so that is where a port is answered, after any T-states the
 * ULA held the clock for since the request.
 *
 * Sources:
 * - "Contended I/O" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended%20I/O — the ULA "pauses
 *   the processor by stopping its clock", which is what the paragraphs above
 *   turn on.
 * - "Contended memory" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended%20memory — 0x4000 to
 *   0x7FFF is the contended range, and the charge falls "on the first tstate
 *   (T1) of any instruction fetch, memory read or memory write operation",
 *   with the 16K and 48K ULAs applying it "under all circumstances" and so
 *   to the internal T-states that hold an address between accesses.
 * - "ZX Spectrum ULA" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/ZX%20Spectrum%20ULA — the chip
 *   answers any even port, so the decode is the one address line A0; a read
 *   returns the keyboard on bits 0-4 and the EAR socket on bit 6.
 * - "Keyboard" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Keyboard, and "The Keyboard"
 *   (Chris Smith),
 *   https://web.archive.org/web/20141021195037/http://www.zxdesign.info/keyboard.shtml
 *   — the forty keys
 *   as eight half-rows of five, each half-row selected by one of the upper
 *   address lines A8 to A15 held low, and read together when more than one
 *   is.
 * - "Spectrum Cassette, EAR and MIC port, Issue 2 and 3" (Pera Putnik),
 *   https://zx48.8bitchip.info/cassport.htm — the voltage on pin 28 after
 *   each setting of bits 4 and 3, measured on an Issue 3 "without input
 *   signal on EAR"; and, with bit 3 set, bit 6 still reading high for about
 *   180 to 2800 T-states after bit 4 falls, for "capacitors connected
 *   between pin 28 of ULA and EAR and MIC connectors", with "no delay" when
 *   bit 4 rises.
 * - "The floating bus explained!" (Ramsoft, technical report rev 1.06,
 *   2002), served at http://www.ramsoft.bbk.org/floatingbus.html and now
 *   mirrored at
 *   https://k1.spdns.de/Develop/Projects/zxsp/Info/Floating%20bus%20technical%20guide.html
 *   — "Due to the resistors which decouple the two dedicated buses inside
 *   the Spectrum, when the Z80 reads from an unattached port (such as 0xFF)
 *   it actually reads the data currently present on the ULA bus", idle
 *   0xFF, and "during a port read, the Z80 fetches the input byte from the
 *   bus in the last cycle of the I/O transaction (i.e. T4)".
 * - "Clock Alignment and Stability" (Chris Smith, 2007),
 *   https://web.archive.org/web/20150326163725/http://www.zxdesign.info/clockAlignment.shtml
 *   — on his clone, the processor samples a port "approximately 32ns after
 *   the falling edge of the CPU clock during T3 of an IO instruction": the
 *   datasheet's T3, which follows the wait state the processor inserts, and
 *   Ramsoft's T4.
 * - Sinclair ZX Spectrum Service Manual,
 *   https://spectrumcomputing.co.uk/pub/sinclair/technical-docs/ZXSpectrum48K_ServiceManual.pdf,
 *   and its Figure 1.5, the Issue 3 circuit diagram,
 *   https://spectrumforeveryone.com/wp-content/uploads/2017/08/ZXSpectrumIssue3-Schematics.gif
 *   — the speaker hung off pin 28 through D9 into TR7, a ZTX450 emitter
 *   follower, and the EAR socket reaching the pin through R36 and C32; and,
 *   in §5.4.3, the MIC output alone "barely sufficient to drive the
 *   loudspeaker".
 * - ZX Spectrum Introduction (Sinclair), chapter 6,
 *   https://worldofspectrum.org/ZXSpectrumIntroduction/chapter_six.html — of
 *   a tape played too loud, "you may be able to hear it coming through the
 *   computer's own loudspeaker".
 */
#ifndef COLOPHON_SPECTRUM_H
#define COLOPHON_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "keyboard.h"
#include "monitor.h"
#include "tape.h"
#include "ula.h"
#include "z80.h"

/* The whole raster the beam covers, at two samples to a T-state: the 48K's
   ULA, a 5C or 6C, draws 312 lines of 224. Unlike a CPC's, this is an
   invariant and not a convention: a Spectrum has no programmable video
   counter, so every frame this ULA draws is this long. */
#define SPECTRUM_FRAMEBUFFER_WIDTH (224 * ULA_SAMPLES_PER_TICK)
#define SPECTRUM_FRAMEBUFFER_HEIGHT 312
#define SPECTRUM_TICKS_PER_FRAME (224 * 312)

/* The board's clock is 3.5MHz, which is what a tape's timings are read
   against. Where it comes from is in ula.h. */
#define SPECTRUM_TICKS_PER_MILLISECOND 3500

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

/* As for a CPC's ten. */
typedef char spectrum_keyboard_fits_the_matrix[SPECTRUM_HALF_ROWS <= KEYBOARD_MAX_LINES ? 1 : -1];

/* The four keys that carry no character and so cannot be looked up by one. */
#define SPECTRUM_CAPS_SHIFT SPECTRUM_KEY(0, 0)
#define SPECTRUM_SYMBOL_SHIFT SPECTRUM_KEY(7, 1)
#define SPECTRUM_ENTER SPECTRUM_KEY(6, 0)
#define SPECTRUM_SPACE SPECTRUM_KEY(7, 0)

/* Which shift a character needs, if any. Forty keys carry upwards of two
   hundred meanings between them, and which one arrives depends on the mode
   the firmware is in as well as the shift held — so this names the key and
   the shift, and what the machine makes of the pair is the firmware's
   business. Most of all: at the K cursor a letter is a keyword, so `p`
   typed at the start of a line is PRINT and not a letter at all. */
typedef enum {
  SPECTRUM_NO_SHIFT = 0,
  SPECTRUM_WITH_CAPS_SHIFT,
  SPECTRUM_WITH_SYMBOL_SHIFT,
} spectrum_shift;

/* The key carrying a character, and the shift held to reach it. Returns
 * KEYBOARD_NO_KEY for a character no key carries — the pound sign has a key
 * and no ASCII, and the tokens printed on the letter keys (<=, THEN, AT)
 * are not characters at all. */
keyboard_key spectrum_key_for_character(char character, spectrum_shift *shift);

typedef struct {
  z80_t cpu;
  uint64_t pins; /* the bus between ticks */

  ula_t ula;
  monitor_t monitor;
  keyboard_t keyboard;

  /* T-states the ULA is holding the processor's clock for. The beam goes on
     through them; the processor does not. */
  uint8_t held_ticks;

  /* The T-states of a port access still to be weighed, one bit each. A port
     is charged at more than one of its own, so the answer is worked out when
     the access begins and spent as its T-states come. */
  uint8_t port_charges_left;

  /* A port read under way, its byte to be taken on the next T-state the
     processor runs. */
  bool reading_port;

  /* What the ULA's half of the data bus carries on this T-state: the byte
     it is fetching, or 0xFF. */
  uint8_t ula_bus;

  /* What the EAR socket hears with no tape in the deck; a loaded one drives
     the socket instead. On hardware it also hears bit 4 of the last write
     through the board's own resistors, by a route that differs between
     issues and is not modelled. */
  bool ear;

  /* The deck, host-owned as a disc is, and NULL when there is none. Nothing
     on this board reaches it: a Spectrum has no motor line. */
  tape_t *tape;

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

/* Put a tape in the deck, or take it out with NULL. */
void spectrum_insert_tape(spectrum_t *spectrum, tape_t *tape);

/* Advance the machine one T-state. The ULA runs every time; the processor
 * runs unless the ULA is holding its clock, and a held tick returns the
 * bus the last one left, address and all. Returns that bus for the host to
 * watch. A port read's byte is on it as it stands on each T-state from the
 * request on, and the byte the processor takes is the one on the tick after
 * the last that shows the request. */
uint64_t spectrum_tick(spectrum_t *spectrum);

/* Between instructions and owing the ULA nothing. A held T-state is one the
 * processor does not run, so an instruction whose last T-state was a charged
 * one leaves the clock stopped past its end: the processor has finished and
 * the machine has not. */
bool spectrum_instruction_complete(const spectrum_t *spectrum);

/* Tick until it is. A snapshot has nowhere to record a half-executed
 * instruction, nor a hold still to be served, so anything about to take one
 * owes the machine this call first. */
void spectrum_finish_instruction(spectrum_t *spectrum);

/* The processor's view without the processor: a peek under the ROM sees the
 * ROM, and a poke there lands nowhere, as a write from the processor would. */
uint8_t spectrum_peek(const spectrum_t *spectrum, uint16_t address);
void spectrum_poke(spectrum_t *spectrum, uint16_t address, uint8_t value);

/* The speaker is moved by a write to the ULA, which can land on any
   T-state. */
#define SPECTRUM_SOUND_STEPS_PER_SECOND (SPECTRUM_TICKS_PER_MILLISECOND * 1000)

/* The voltage across the loudspeaker, in tenths of a millivolt. The ULA
 * drives one pin for the speaker and both tape sockets, at a level set by
 * bits 4 and 3 of the last write to it, and the speaker stands two silicon
 * junctions below that pin, taken at 0.7V each: bit 4 sounds it, and bit 3
 * alone does not. The board is an Issue 3's.
 *
 * Left out, for want of a measurement: the little current the junctions
 * pass below their drop; the pin's fall, which the capacitors to the tape
 * sockets slow, taken here as instant; a playing tape, which reaches the
 * same pin through the EAR socket's capacitor and so the speaker, wherever
 * it lifts the pin past the junctions, by an amount the recorder's volume
 * and the ULA's output stage set; and what the speaker makes of its
 * voltage. */
uint16_t spectrum_sound_speaker(const spectrum_t *spectrum);

#endif
