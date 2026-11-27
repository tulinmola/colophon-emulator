/*
 * spectrum.h — the ZX Spectrum, wired: Sinclair's 48K and Amstrad's grey +2.
 *
 * This is the machine file: the one place that knows the chips are soldered
 * into a Spectrum. It owns the memory map, the I/O decode, and the tick that
 * runs the processor and the ULA together; the chips it wires know nothing
 * about it.
 *
 * There is less board here than a CPC has. The ULA is the whole video
 * department, and the keyboard hangs off the address lines with nothing in
 * between — so where a CPC's wiring routes a keypress through an 8255 and a
 * sound chip, this one reads the matrix directly.
 *
 * The two models are one design at two sizes. The +2 is the Spectrum 128
 * remade by Amstrad: its ULA, a 7K010, draws a longer line and one line
 * fewer; its RAM is eight banks of 16K where a 48K's is one run; its one ROM
 * chip holds two ROMs, Amstrad's revision of the 128's and the 48K's,
 * amended; and an AY-3-8912 sits on the bus beside the ULA. A register at port 0x7FFD, the
 * bank register, chooses which bank answers at 0xC000, which of two screens
 * the ULA draws, and which ROM answers at 0x0000, and one bit of it locks
 * the rest until reset, which is how a +2 runs 48K software as a 48K would.
 * Nothing on a 48K moves its memory map.
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
 * address. The ULA wants 0x4000 to 0x7FFF on both models, and on a +2
 * 0xC000 to 0xFFFF as well whenever an odd bank is paged there. Only the
 * top two address lines reach the ULA: the processor's own on a 48K, and on
 * a +2 the paging logic's, which show it an odd bank at 0xC000 as if it lay
 * at 0x4000 and tell no memory cycle from any other. So the rule covers
 * every kind of cycle, ports included, and the lock does not change it.
 * Rak measured all of this on a 128; a +2 carries the same paging logic.
 *
 * A port nothing answers reads the floating bus: the data bus is in two
 * halves joined by resistors, and the read finds whatever the ULA has on
 * its half at that moment — a byte of the screen it is fetching, or the
 * board's 0xFF when it fetches nothing — which is how a program with no
 * other clock finds the beam. The processor takes the byte on the read's
 * last T-state, so that is where a port is answered, after any T-states the
 * ULA held the clock for since the request.
 *
 * The bank register is decoded from two address lines, A15 and A1, both
 * low, on a read as readily as on a write, and it takes its byte as the
 * access ends: on the access's last T-state, after any T-states held before
 * it. A read therefore pages the machine with whatever byte the processor
 * takes, which on most of the frame is the floating bus's 0xFF: bank 7,
 * the second screen and the 48K ROM, all locked, which is the crash owners
 * of +2s report on reading port 0x7FFD. The sound chip is
 * decoded from the same two lines with A15 high, and A14 chooses between its
 * two ports: on a write, 0xFFFD selects a register and 0xBFFD writes it; on
 * a read, 0xFFFD reads the register and 0xBFFD selects nothing, so it reads
 * the floating bus.
 *
 * Not modelled on the +2: its two joystick ports, which read as if nothing
 * were plugged in; the keypad and the RS232 socket, which share the sound
 * chip's port A and are taken to read as empty; what a write to port 0xFE
 * comes back as on bit 6, which the 48K here leaves out as well, so that
 * the EAR socket alone answers there on both; the crash a 128 is reported
 * to suffer soon after the I register points at memory the ULA wants,
 * where the snow ula.h leaves out begins; and the paging logic one report
 * has some +2s carry, which ignores a read of port 0x7FFD.
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
 *   bit 4 rises. The +2's MIC pin, which nothing measures, is taken to
 *   stand where that pin does.
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
 * - Spectrum 128 Service Manual (Bravemay for Sinclair Research, X-1223),
 *   https://archive.org/details/zx-spectrum-128-service-manual — §4: bits 2
 *   to 0 of the bank register choose "the page occupying the top 16K", bit 3
 *   the screen in page 5 or 7, bit 4 the ROM and bit 5 locks the rest, and
 *   "On power up, or after reset the bank register is cleared"; §4.12.2,
 *   "ROM 1 is the old 48K Spectrum ROM (slightly modified) and is selected
 *   when bank register bit 4 sets address A14"; §4.12.11, the register
 *   "latches D5-D0 off the data bus on the negative (trailing) edge of the
 *   BANK output from the PAL", which is the access's end; and §5.6.2, the
 *   sound chip's BDIR and BC1 decoded from the paging logic's PSG, A14 and
 *   the direction, in a table with rows for writing 0xFFFD and 0xBFFD and
 *   for reading 0xFFFD, and none for reading 0xBFFD, which its pattern leaves
 *   no function at all. The parts list gives the crystal as 17.734475MHz,
 *   and the fault table the processor's clock pin as "3.54689 MHz".
 * - Ian Worsley's redraw of the grey +2's board, Z70500 issue 3,
 *   https://retroisle.com/sinclair/zxspectrum/Technical/Hardware/ZXSpectrum%2B2Schematics.png
 *   — one ROM chip, IC8; the bank register an LS174, six flip-flops,
 *   clocked through an inverter from the paging logic's BANK output, so it
 *   latches as BANK falls at the end of the access, and held off through a
 *   diode from its own bit 5; the sound chip's data lines on the
 *   processor's side of the resistors that split the data bus, its A8 and
 *   BC2 tied high and its clock pin wired to the ULA's pin C; its three
 *   channels each through 1K8 into the base of TR6, a BC239 with its emitter
 *   grounded, 330R from its collector to +5V, 560R from collector to base and
 *   180R from base to ground; that collector, GI SOUND, through 18K to the
 *   television modulator's audio input, and the ULA's MIC and EAR pins
 *   through 68K each, every one behind 1uF; GI SOUND, the MIC line and the
 *   tape's EAR line again through 10K and 100nF each to a socket, SK1 AUDIO
 *   OUT; 390R from the EAR pin to ground and 680R from it to the tape's
 *   line; and a crystal marked "35.4690 MHz", halved by a 74F74 into the
 *   ULA.
 * - JL's reading of the 128's paging logic, a HAL10H8 whose fuse was blown,
 *   by trying every input, https://trastero.speccy.org/cosas/JL/Pal/PAL10H8.html
 *   — "BANK = /A15Z * /A1Z * /IORQ * /RDL + /A15Z * /A1Z * /IORQ * /WRL",
 *   PSG the same with A15 high, and the two address lines it hands the ULA,
 *   "ULA15 = A15Z * /A14Z + A15Z * A14Z * /B0" and "ULA14 = /A15Z * A14Z +
 *   A15Z * A14Z * B0", with no MREQ or RFSH among its inputs.
 * - Patrik Rak on "a genuine 128k Spectrum", World of Spectrum forum, 2007,
 *   https://worldofspectrum.org/forums/discussion/comment/215386/#Comment_215386
 *   — "the memory in range #4000-#7FFF is contended as is any odd page paged
 *   in #C000-#FFFF range", with "the addition of contending also ports in
 *   #C000-#FFFF range in case a contended page is paged in", and "Disabling
 *   of paging doesn't disable the contention of the high range, port #7FFD
 *   is contended as any other odd port in this range (C:1x4)".
 * - Brendan Alford's run of Rak's ptime on a toastrack 128 and a grey +2,
 *   World of Spectrum forum, 2014,
 *   https://worldofspectrum.org/forums/discussion/comment/759591/#Comment_759591
 *   — the first line of the picture still drawn from the screen shown
 *   before the OUT for none of its width when the OUT to 0x7FFD begins at
 *   14358, sixteen pixels from 14359 to 14360, thirty-two from 14361 to
 *   14368, and so on in steps of eight T-states, "identical" on the +2.
 *   That the OUT begins where ptime says is Rak's word: he had made the test
 *   so "it always starts the OUT cycle exactly at the advertised T cycle
 *   even on 128k, despite the contention",
 *   https://worldofspectrum.org/forums/discussion/comment/757618/#Comment_757618.
 *   Of Alford's first run, the day before, Rak wrote that "this already shows
 *   that the OUT really happens during T3, not earlier", the datasheet's T3,
 *   which is the access's last,
 *   https://worldofspectrum.org/forums/discussion/comment/759529/#Comment_759529;
 *   and his table of 2019 gives the row "(early timings, add +1 for late)",
 *   https://worldofspectrum.org/forums/discussion/comment/958908/#Comment_958908.
 * - "Please test your ZX128+2 (grey model)", World of Spectrum forum,
 *   2011-2016,
 * https://worldofspectrum.org/forums/discussion/35523/please-test-your-zx128-2-grey-model — +2
 * owners reading port 0x7FFD and crashing, among them Peter Mulholland on a Z70500 issue 3 board:
 * "It causes a crash when port 0x7FFD is read"; and Velesoft's report that a late board "can
 * contain same chip HAL10H8 with fixed firmware (reading from port 32765 are ignored, ZX no
 * crash)", which no owner in the thread confirmed.
 * - Scott-Falk Hühn, "ZX Spectrum +2 - Improve sound quality",
 *   http://s-huehn.de/spectrum/en/hardware1.htm#sp2-2b — of the +2's sound
 *   output, SK1, "the 3 sources EAR, MIC/beeper and sound chip are mixed",
 *   which puts the beeper on the MIC pin; and there, as made, "the sound
 *   chip is much too quiet, the beeper too loud", which the board's values
 *   give at that socket, where each source comes in through 10K.
 * - Adam Wilson's restoration of a +2 on a Z70500 issue 3 board, 2021,
 *   https://retrorepairsandrefurbs.com/2021/07/07/sinclair-zx-spectrum-128k2-grey-restoration/
 *   — "design errors in the audio circuitry (specifically the balancing
 *   stage) cause the 3-channel audio from the AY-3-8912 chip to be
 *   significantly quieter than the beeper audio from the ULA", for which he
 *   lowered the resistors that carry it to the socket and to the
 *   television, R37 and R38; and, there, his "10k on schematic" for both,
 *   and a reader's board with R37 at 2.2K and R38 at 18K.
 * - Couriersud's measurement of an AY-3-8910's output at each level into
 *   loads from 983 ohms to an open circuit, 2014, as MAME's ay8910.cpp quotes
 *   it, https://github.com/mamedev/mame/blob/dd06c01/src/devices/sound/ay8910.cpp
 *   — from which each level's current into the +2's mixing stage is read.
 * - ON Semiconductor, BC237 series datasheet, 2006,
 *   https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/2465/BC237%28B%2C%20C%29%2C%20239C.pdf
 *   — the BC239's base-emitter voltage, 0.62V typical at 2mA and 25°C; its
 *   gain, listed for the BC239C alone, 380 to 800 and 500 typical at 2mA;
 *   and the family's gains, 120 to 800.
 * - comp.sys.sinclair FAQ, 128K reference, cited in ula.h — "The ULA bug
 *   which causes snow when I is set to point to contended memory still
 *   occurs, and also appears to crash the machine shortly after I is set to
 *   point to contended memory".
 * - General Instrument, "AY-3-8910/8912 Programmable Sound Generator Data
 *   Manual", February 1979, p. 10, cited in ay38910.h — the I/O ports "are
 *   provided with internal pull-ups on each pin".
 */
#ifndef COLOPHON_SPECTRUM_H
#define COLOPHON_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "ay38910.h"
#include "keyboard.h"
#include "monitor.h"
#include "tape.h"
#include "ula.h"
#include "z80.h"

typedef enum {
  /* Sinclair's, with a 5C or 6C ULA running early, and sixteen or forty-eight
     kilobytes of RAM: the firmware works out which by writing to the top of
     the address space and reading back. */
  SPECTRUM_48K,
  /* Amstrad's grey +2, with a 7K010 running late and 128K of RAM. */
  SPECTRUM_PLUS2,
} spectrum_model;

/* The whole raster each model's ULA covers, at two samples to a T-state:
   312 lines of 224 on the 48K, 311 of 228 on the +2. Unlike a CPC's, these
   are invariants and not conventions: a Spectrum has no programmable video
   counter, so every frame a ULA draws is as long as every other. A host
   that sets its framebuffer aside before it knows the model sets aside the
   largest. */
#define SPECTRUM_48K_RASTER_WIDTH (224 * ULA_SAMPLES_PER_TICK)
#define SPECTRUM_48K_RASTER_HEIGHT 312
#define SPECTRUM_PLUS2_RASTER_WIDTH (228 * ULA_SAMPLES_PER_TICK)
#define SPECTRUM_PLUS2_RASTER_HEIGHT 311
#define SPECTRUM_LARGEST_FRAMEBUFFER (SPECTRUM_PLUS2_RASTER_WIDTH * SPECTRUM_48K_RASTER_HEIGHT)

/* T-states a second, which is what a tape's timings are read against. The
   48K's ULA makes it from a 14MHz crystal (ula.h). The +2's crystal is
   marked "35.4690 MHz", which to the figures marked is eight times PAL's
   colour subcarrier, 35.46895MHz; it is halved into the ULA, which divides
   by five, as the 128's divides its own 17.734475MHz. */
#define SPECTRUM_48K_CLOCK_HERTZ 3500000
#define SPECTRUM_PLUS2_CLOCK_HERTZ 3546895

/* The ROM each model fits, answering from 0x0000: the 48K's 16K, and the
   +2's one chip of 32K, which holds two ROMs, Amstrad's revision of the
   128's below the 48K's amended one. */
#define SPECTRUM_48K_ROM_SIZE 0x4000
#define SPECTRUM_PLUS2_ROM_SIZE 0x8000

/* The RAM a model fits, answering from SPECTRUM_RAM_BASE: 16K or 48K on the
   48K, and on the +2 eight banks of 16K. */
#define SPECTRUM_RAM_BASE 0x4000
#define SPECTRUM_RAM_16K 0x4000
#define SPECTRUM_RAM_48K 0xC000
#define SPECTRUM_RAM_128K 0x20000
#define SPECTRUM_BANK_SIZE 0x4000

/* The +2's bank register, port 0x7FFD: the bank answering at 0xC000; the
   screen the ULA draws, bank 5's when clear and bank 7's when set; the ROM
   answering at 0x0000, the 128's when clear and the 48K's when set; and the
   lock, which leaves the register as it is until reset. Bank 5 always
   answers at 0x4000 and bank 2 at 0x8000. The register has six bits, and
   the top two of a byte written to it reach nothing. */
#define SPECTRUM_BANK_PAGE 0x07
#define SPECTRUM_BANK_SCREEN 0x08
#define SPECTRUM_BANK_ROM 0x10
#define SPECTRUM_BANK_LOCK 0x20

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
  spectrum_model model;

  z80_t cpu;
  uint64_t pins; /* the bus between ticks */

  ula_t ula;
  monitor_t monitor;
  keyboard_t keyboard;
  /* The +2's sound chip; a 48K has none, and its wiring never reaches this
     one. */
  ay38910_t psg;

  /* The +2's; always 0 on a 48K, which has none. */
  uint8_t bank_register;
  /* A port access the bank register answers, which it latches as the
     access ends, on the next T-state the processor runs. */
  bool latching_bank_register;

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
     boards and is not modelled. */
  bool ear;

  /* The deck, host-owned as a disc is, and NULL when there is none. Nothing
     on this board reaches it: a Spectrum has no motor line. */
  tape_t *tape;

  /* Host-provided storage; the core allocates nothing. The +2's banks lie
     in order, bank 0 first. */
  const uint8_t *rom; /* the model's ROM size */
  uint8_t *ram;       /* ram_size bytes */
  uint32_t ram_size;
} spectrum_t;

/* Power-on, as the given model. The processor fetches its first instruction
 * from the ROM at 0x0000, which is the only thing that answers there; on a
 * +2 that is the first of its two, since the bank register starts clear. */
void spectrum_init(spectrum_t *spectrum, spectrum_model model, uint8_t *ram, uint32_t ram_size,
                   const uint8_t *rom);

/* The model's clock, as SPECTRUM_48K_CLOCK_HERTZ or
 * SPECTRUM_PLUS2_CLOCK_HERTZ. */
uint32_t spectrum_clock_hertz(const spectrum_t *spectrum);

/* Plug in a monitor: the model's raster in bytes of colour codes,
 * host-owned. Unplugged, the machine runs on and draws into the void, as
 * it would with the aerial out.
 *
 * The picture does not land where the beam drew it. A 48K's frame sync is a
 * plain pulse held across eight whole lines, and a +2's is taken to be the
 * same (ula.h), with none of the short
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

/* The processor's view without the processor, through the banks paged in:
 * a peek under the ROM sees the ROM, and a poke there lands nowhere, as a
 * write from the processor would. */
uint8_t spectrum_peek(const spectrum_t *spectrum, uint16_t address);
void spectrum_poke(spectrum_t *spectrum, uint16_t address, uint8_t value);

/* The sound where the board sends it to be heard, in tenths of a
 * millivolt, which a write to the ULA or the sound chip can move on any
 * T-state, and the sound chip's own clock on every other. Each model sends
 * it somewhere different.
 *
 * A 48K's is the voltage across its loudspeaker. Its ULA drives one pin
 * for the speaker and both tape sockets, at a level set by bits 4 and 3 of
 * the last write to it, and the speaker stands two silicon junctions below
 * that pin, taken at 0.7V each: bit 4 sounds it, and bit 3 alone does not.
 * The board is an Issue 3's. Left out, for want of a measurement: the
 * little current the junctions pass below their drop; the pin's fall,
 * which the capacitors to the tape sockets slow, taken here as instant; a
 * playing tape, which reaches the same pin through the EAR socket's
 * capacitor and so the speaker, wherever it lifts the pin past the
 * junctions, by an amount the recorder's volume and the ULA's output stage
 * set; and what the speaker makes of its voltage.
 *
 * A +2 has no loudspeaker. Its sound goes to the television, and this is
 * the voltage at the audio input of the MC1376 that modulates it, pin 5,
 * into no current. Three things are mixed there through resistors. The
 * sound chip's three channels each push a current into one transistor's
 * base, where the currents add, and the transistor turns their sum into a
 * voltage that falls as it grows. The ULA's MIC pin carries bits 4 and 3
 * together, as a 48K's one pin does, and is taken to stand at the levels
 * that pin was measured at, no measurement of this ULA's having been found.
 * And the EAR pin, which is the tape's way in, stands at nothing with no
 * tape playing. Bit 4 moves the mix about three times as far as one channel
 * at its loudest, and as far as all three together, which owners bear out:
 * the sound chip "significantly quieter than the beeper audio from the
 * ULA" (Wilson, above).
 *
 * Left out on the +2: the capacitors each source passes through, which
 * keep only what changes faster than a few times a second; the modulator
 * and the television after this pin; SK1, the board's audio socket, which
 * mixes the same sources in other proportions; a playing tape, heard
 * through the EAR pin; the few per cent by which the transistor's output is
 * not a straight line in its input, and by which a loud channel lets the
 * others push less; and the boards whose mixing resistors are not the ones
 * drawn here, as owners report some are (Wilson, above). */
uint16_t spectrum_sound(const spectrum_t *spectrum);

#endif
