/*
 * ula.h — the Ferranti uncommitted logic array, stepped at the CPU's clock.
 *
 * Where a CPC divides the work between a 6845 and a Gate Array, a Spectrum
 * has one chip. It counts out the frame, reads the screen, turns bytes into
 * pixels, raises the interrupt, sends the monitor its composite sync, holds
 * the border colour and drives the sound. It does not read memory: the
 * machine fetches the byte ula_fetch names and hands it over with
 * ula_latch, as it does for a Gate Array.
 *
 * Everything here describes the T-state the chip is standing on. ula_tick()
 * ends that T-state and begins the next, so a caller reads first and ticks
 * last.
 *
 * The frame is counted from the T-state the chip raises the interrupt on,
 * and the processor here sees the line on that T-state and begins its
 * answer on the next. That is the count Jan Bobrowski's test programs
 * print, and Patrik Rak's with them, so their numbers are this file's; Rak
 * says Chris Smith counts so too. Fuse lets its processor begin the answer
 * on its T-state 0, and the T-states the FAQ and the Sinclair Wiki count
 * from the interrupt agree with Fuse's, so each of those is one more here —
 * though the wiki says it counts from the same T-state as this file, with
 * the processor answering no sooner than the next. Taken as this file
 * counts, those numbers would bring the screen a T-state sooner after the
 * interrupt than on any machine measured.
 *
 * The chip comes in types, as the 6845 does, and two are built. What sets
 * them apart here is the frame alone — how long a line is, how many lines
 * there are, where the picture starts and how long the interrupt is held —
 * and the picture, the serialiser and the slot the chip keeps are the same
 * on both.
 *
 * A type here is a chip as one machine times it, because the same chip can
 * run early or late: with everything one T-state later after the interrupt
 * than another of its family. One account has the ULA drifting from one to
 * the other as it warms, the other the processor catching the interrupt's
 * leading edge a T-state sooner; neither ties it to a part number, so an
 * early 7K010 would be a type of its own, as would a late 6C.
 *
 * The picture is fetched in slots of eight T-states that begin where
 * contention does. In each, the chip reads a bitmap byte, its attribute,
 * the next bitmap byte and its attribute on the slot's fourth to seventh
 * T-states, and leaves its side of the bus idle for the other four. What it
 * reads goes into latches, and every fourth T-state it takes the next byte
 * and its attribute from them and draws from those for four. Outside the
 * picture it takes the border colour at the same steps, so a new border
 * shows only from the next of them, eight pixels at a time.
 *
 * A sample is four bits — brightness, then green, red and blue — which is
 * the attribute byte's own colour encoding widened by one bit.
 *
 * Implemented: the frame, the interrupt, the screen fetch and its address
 * scramble, the latches and the drawing from them, FLASH, the border,
 * composite sync, and the contention slot. Not modelled: snow, the fetches
 * the processor's refresh corrupts when its I register points into
 * 0x4000-0x7FFF.
 *
 * Sources:
 * - Sinclair ZX Spectrum Service Manual §5.1 — the 48K's ULA derives the
 *   CPU clock from an external 14MHz crystal; the dot clock is half of it,
 *   so two pixels leave the chip for every T-state.
 * - libspectrum's timings.c (Philip Kendall, Fuse),
 *   https://sourceforge.net/p/fuse-emulator/libspectrum/ci/master/tree/timings.c
 *   — the 5C/6C frame: 24 + 128 + 24 + 48 T-states to a line, 312 lines to
 *   a frame, and the interrupt held 32 T-states. And its 7C frame: 24 +
 *   128 + 24 + 52 to a line, and the interrupt held 36. The split of that
 *   line is libspectrum's alone: nothing found measures it, any more than
 *   where the 7K010's frame sync lies. Fuse, which reads these tables,
 *   takes an interrupt between instructions while "tstates <
 *   machine_current->timings.interrupt_length" (its z80/z80.c,
 *   z80_interrupt), tstates reduced by a frame at the frame's end (its
 *   spectrum.c), so its processor can begin the answer on T-state 0.
 * - Patrik Rak on a 128 he had just tested, World of Spectrum forum, 2007,
 *   https://worldofspectrum.org/forums/discussion/comment/215386/#Comment_215386
 *   — "Everything starts at 14362 instead of 14336, the line is 228 instead
 *   of 224", and, of the FAQ below, "Barring the same off-by-one difference,
 *   the FAQ is correct". He counts as this file does, so his 14336 is the
 *   5C's first contended T-state here, and his 14362 an early 7K010's.
 *   And on how they count, in the same thread,
 *   https://worldofspectrum.org/forums/discussion/comment/213404/#Comment_213404
 *   — Bobrowski is
 *   "measuring time from when ULA pulled INTREQ low (IM2 handler reached at
 *   20T after that time, 0T annotated as start of interrupt signal). He
 *   counts everything relative to ULA, same as Chris does, and so do I",
 *   where Fuse's author and others "count things from when the interrupt
 *   happens", which "makes the 1T difference".
 * - Brendan Alford's measurements on a toastrack 128 and a grey +2, World
 *   of Spectrum forum, 2014, with Jan Bobrowski's test programs and Rak's
 *   ptime: the same results on both but for the interrupt, which the
 *   toastrack's Zilog read as 35 and the +2's own NEC D780C-2 as 34, and
 *   "Frame time: 70908" on both,
 *   https://worldofspectrum.org/forums/discussion/comment/759432/#Comment_759432;
 *   and the +2 with other processors, another NEC reading 34 and three
 *   Zilogs 36, a Z8400APS 8622 among them; and a later timing test with a
 *   Zilog in it giving the toastrack's results again,
 *   https://worldofspectrum.org/forums/discussion/comment/759591/#Comment_759591.
 *   Rak's reading of them: that the processor "sometimes samples the /INT
 *   correctly during the first cycle and sometimes it misses that", that
 *   "the similar thing happens during the trailing rising edge", and that
 *   the miss "is the source of the early/late timing differences",
 *   https://worldofspectrum.org/forums/discussion/comment/759662/#Comment_759662;
 *   his wondering "if 34 is the intended duration which the 128K ULA
 *   generates",
 *   https://worldofspectrum.org/forums/discussion/comment/759528/#Comment_759528;
 *   and his table of 2019 marking the 48K's figures and the 128's "(early
 *   timings, add +1 for late)",
 *   https://worldofspectrum.org/forums/discussion/comment/958908/#Comment_958908
 *   — "All this is measured relative to when ULA starts asserting INTREQ,
 *   that's start of cycle 0. If the CPU senses the line during this cycle,
 *   it starts processing the interrupt at start of cycle 1 (which results
 *   in so called late timing ...). Some CPUs miss this cycle ... thus
 *   starting processing interrupt at start of cycle 2 (so called early
 *   timing ...)". The processor here starts at cycle 1, as Bobrowski's
 *   programs reckon it (an IM2 handler reached at 20T, Comment_213404
 *   above), and the early row's numbers are this file's; that in his
 *   account an early machine's processor and chip both stand a T-state
 *   later is nothing a program can see. Its 48K row gives contention
 *   starting at 14336; Bobrowski's btime drawing the border change at the
 *   picture's left edge for an OUT whose machine cycle begins from 14112 to
 *   14115 and the same picture for all four, the 128's at 14134 to 14137;
 *   and his stime seeing a write to the screen whose cycle begins at 14335
 *   and missing one at 14336.
 * - Víctor Iborra's photographs of test programs on a Spanish grey +2, in
 *   the zxe.io depot: in 2023, Rak's Timing Test 0.3, in which the first
 *   NOP charged for contention starts at 14363, a T-state after the 14362
 *   at which Rak's table starts an early 128's, both in Rak's count,
 *   https://zxe.io/depot/testing/ZX%20Spectrum/Timing%20Test%20v0.3%20%282013-12-09%29%28Rak,%20Patrik%29%20-%20Testing%20%7B2023-07-05%7D%7BIborra,%20Vi%CC%81ctor%7D%7BZX%20Spectrum%20+2%20ES%7D%20128K%20mode%20-%200%20-%20Contended%20NOP%20%5Bsmall%5D.jpg;
 *   and in 2024, Bobrowski's minfo reading "INT time: 36",
 *   https://zxe.io/depot/testing/ZX%20Spectrum/Minfo%20%282011-08-20%29%28Bobrowski,%20Jan%29%20-%20Testing%20%7B2024-03-27%7D%7BIborra,%20Vi%CC%81ctor%7D%7BZX%20Spectrum%20+2%20ES%7D.jpg.
 * - Richard and Tim Butler's 128K timing tests, as recorded on
 *   https://github.com/redcode/ZXSpectrum/wiki/ZX-Spectrum-Timing-Tests-128K
 *   — three grey +2s printing "Timing Tests Failed (48K Mode): None", one
 *   of them tested by Alford with a Zilog Z8400APS 8622 in it, where three
 *   toastracks fail the same five; the page's explanation, that the test
 *   "was likely written on a ZX Spectrum +2, a model that ... typically has
 *   late timings".
 * - Paul Farrow's disassembly of the 128's ROMs,
 *   http://www.fruitcake.plus.com/Sinclair/Spectrum128/ROMDisassembly/Spectrum128ROMDisassembly.htm
 *   — the notes on timing in its ROM 0 listing, "TV scan lines = 311 total,
 *   63 above picture (48K Spectrum had 312 total, 64 above picture)".
 * - "ZX Spectrum Compatible Interrupts" (Chris Smith),
 *   https://web.archive.org/web/20141214071206/http://www.zxdesign.info/interrupts.shtml
 *   — those 64 lines are 8 of vertical sync and 56 of border, and, because
 *   14336 divides exactly by a line, "the interrupt must be generated not
 *   at the start of a line, but at the same offset into a line as the first
 *   display byte and 64 scanlines earlier": an offset on the chip's own
 *   counter, which its fetching and drawing follow by a few T-states. His
 *   site no longer answers, and the Internet Archive keeps the page.
 *   Its measured line divides the two borders 16 and 32 where libspectrum
 *   divides them 24 and 24. Both agree on 128 displayed and 48 blanked, and
 *   nothing here turns on which is right: the two timings that matter are
 *   absolute T-states from the interrupt, and the split moves only where
 *   the picture sits across the raster.
 * - "Spectrum Video Modes" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Spectrum%20Video%20Modes — the
 *   display file's address scramble, the attribute area, and the attribute
 *   byte's fields.
 * - "Contended memory" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended_memory — the delay a
 *   contended access owes by its position in the chip's eight-T-state slot,
 *   that the first contended T-state of a frame is 14335 (14336 here), and
 *   its account of early and late, that "as the ULA heats up, it drifts from
 *   "early timing" to "late timing"".
 * - "Floating bus" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Floating_bus — the slot's
 *   fetches: "two sets of display and attribute bytes (order: bitmap,
 *   attribute, bitmap+1, attribute+1) being fetched during the first 4
 *   cycles followed by 4 idle cycles", the first at 14338 on the 48K and
 *   14364 on the 128 by the wiki's count, three after its first contended
 *   T-states, and "one T-state later on "late timing" machines".
 * - "Memory Contention Revised and Snow" (Chris Smith, 2007),
 *   https://web.archive.org/web/20150326163820/http://www.zxdesign.info/memContRevision.shtml
 *   — the same from the chip's side: the slot is eight cycles from the
 *   first contended, the video bytes are fetched on its fourth to seventh,
 *   and "the ULA releases the memory at cycle 8, followed by cycles 1, 2
 *   and 3 of the next pass".
 * - comp.sys.sinclair FAQ, 16K/48K reference,
 *   https://worldofspectrum.org/faq/reference/48kreference.htm — FLASH
 *   swaps ink and paper every 16 frames, the write to port 0xFE, "At cycle
 *   14335 (just one cycle before the top left corner is reached) the delay
 *   is 6 cycles" (14336 here), and "Some machines use the timings given in
 *   this FAQ, while others are one T state later for all timings". And its
 *   128K reference,
 *   https://worldofspectrum.org/faq/reference/128kreference.htm — the
 *   6,5,4,3,2,1,0,0 pattern "starts at 14361 T states after the interrupt
 *   as opposed to 14335, and repeats every 228 T states" (14362 here),
 *   "There are 63 scanlines before the television picture, as opposed to
 *   64", and "on some machines all timings (including contended memory
 *   timings) are one T state later".
 * - Richard and Tim Butler's 48K timing tests, https://www.zxspectrum4.net/op_timing.php
 *   — programs that grade a machine against values stored in them from
 *   real 48Ks, run here by test/spectrum_programs_test.c. They pass with
 *   the 5C's first contended T-state at 14336 and fail at 14335.
 */
#ifndef COLOPHON_ULA_H
#define COLOPHON_ULA_H

#include <stdbool.h>
#include <stdint.h>

/* The dot clock is twice the CPU's, so a T-state is two pixels wide: on
   either type a line's 256 pixels of picture take its 128 T-states. */
#define ULA_SAMPLES_PER_TICK 2

/* A line, in the order the beam walks it: the retrace first, so that a
   monitor timing its lines from the sync lands on the left border. The
   retrace is the type's; what follows it is the same on both. */
#define ULA_LEFT_BORDER_TICKS 24
#define ULA_DISPLAY_TICKS 128
#define ULA_RIGHT_BORDER_TICKS 24

/* The frame sync, eight whole lines from the line the interrupt falls in,
   as Chris Smith gives the 48K's. Nothing found measures where the 7K010's
   lies, so it is taken to be the same. */
#define ULA_VSYNC_LINES 8
#define ULA_DISPLAY_LINES 192

/* The screen is 32 characters by 24, eight pixel rows to a character. */
#define ULA_COLUMNS 32
#define ULA_ROWS 24

typedef enum {
  /* Ferranti's 5C and 6C, early: 224 T-states a line, 312 lines, the
     interrupt held 32 and the first contended T-state 14336 after it. */
  ULA_5C_6C_EARLY,
  /* Ferranti's 7K010, marked Amstrad 40056 on a grey +2, late: 228
     T-states a line, 311 lines, the interrupt held 36 and the first
     contended T-state 14363 after it. Late because grey +2s pass Butler's
     tests, which the redcode wiki reckons were written on a +2, a model
     that typically runs late. How long the interrupt is held, as a processor
     reads it, depends on the processor as well: Alford's +2 read 34 with
     its own NEC, running early, and 36 with three Zilogs. Of +2s found
     running late, Iborra's reads 36, its processor unrecorded, and a +2
     Alford tested passed Butler's tests with a Zilog marked as one of the
     three, very likely the same machine. Neither reading was taken on the
     day its machine was found late. */
  ULA_7K010_LATE,
} ula_type;

/* One type's frame, fixed for the life of the chip. */
typedef struct {
  uint16_t ticks_per_line;
  uint16_t lines_per_frame;
  uint32_t ticks_per_frame;
  uint16_t retrace_ticks;
  /* The raster line the picture begins on, the line the interrupt falls in
     being line 0, and the first T-state after the interrupt the chip wants
     the bus on, which begins the first slot of the picture. */
  uint16_t first_display_line;
  uint32_t first_contended_tick;
  /* The interrupt opens the frame and is held this long. A CPU that has not
     finished an instruction by then misses it altogether. */
  uint8_t interrupt_ticks;
} ula_frame;

typedef struct {
  ula_frame frame;

  /* T-states since the interrupt, which is the unit every published timing
     for this machine is given in. */
  uint32_t frame_tick;
  /* Frames since reset. FLASH swaps ink and paper on bit 4 of it. */
  uint32_t frame_count;

  /* Where the beam stands, which is what the chip's own two counters hold.
     Derived from frame_tick by ula_tick and ula_seek; cache, never the
     state. Deriving them on demand costs a division a caller would pay
     several times a T-state. */
  uint16_t line;
  uint16_t column;

  /* What the chip has read for the slot under way: each of its two
     characters' bitmap byte and attribute, the even character's first. */
  uint8_t fetched_bitmap[2];
  uint8_t fetched_attribute[2];
  /* What it is drawing from, taken every fourth T-state: a character's
     bitmap byte and attribute, or for the border no pixels on a paper of
     the border colour. */
  uint8_t drawn_bitmap;
  uint8_t drawn_attribute;

  uint8_t border;  /* port 0xFE bits 2-0 */
  bool speaker;    /* bit 4: the sound, on a 48K's loudspeaker or not */
  bool microphone; /* bit 3 */
} ula_t;

void ula_init(ula_t *ula, ula_type type);

/* Stand the chip at a given T-state of the frame. Anything that places it
 * from outside — a test, a snapshot — goes through here, because the beam
 * counters above are derived and would otherwise disagree with it. What the
 * chip has fetched and is drawing from stays as it was, so up to two
 * characters after a seek are drawn from it before the slots catch up. */
void ula_seek(ula_t *ula, uint32_t frame_tick);

/* The screen address the chip reads on this T-state, if it reads one: a
 * bitmap byte or an attribute, on four T-states of every eight across a
 * line of the picture. False when its side of the bus is idle. */
bool ula_fetch(const ula_t *ula, uint16_t *address);

/* The byte read for ula_fetch's address, handed to the chip on the same
 * T-state. */
void ula_latch(ula_t *ula, uint8_t data);

/* Draw this T-state: two samples from what the chip is drawing from,
 * taking the next character or the border colour first on every fourth,
 * or black where the beam is blanked. */
void ula_video(ula_t *ula, uint8_t samples[ULA_SAMPLES_PER_TICK]);

/* The composite sync on its way to the monitor, asserted when active: the
 * line's retrace, and eight whole lines at the top of the frame. Unlike a
 * Gate Array's, the frame pulse is not serrated — the line pulses inside it
 * are simply absent, which is what a monitor timing itself from pulse
 * lengths has to be told to expect. */
static inline bool ula_csync(const ula_t *ula) {
  return ula->line < ULA_VSYNC_LINES || ula->column < ula->frame.retrace_ticks;
}

/* The interrupt line, held for the frame's first interrupt_ticks. */
static inline bool ula_interrupt(const ula_t *ula) {
  return ula->frame_tick < ula->frame.interrupt_ticks;
}

/* T-states the chip would keep the bus from an access beginning at this
 * point in the frame; zero when it does not want the bus. Nothing else bears
 * on it: the screen is always in the same place and always read, so the
 * whole of contention is a function of where in the frame the access falls.
 * Ticks past the end of a frame wrap into the next.
 *
 * What to do with the answer is the machine's. This chip takes the bus by
 * holding the processor's clock, which is not something a wait line can
 * express; and it is the machine that knows which addresses are contended,
 * and that a port is contended by a rule of its own. */
uint8_t ula_contention(const ula_t *ula, uint32_t frame_tick);

/* One command byte, as written to any port with A0 low. */
void ula_write(ula_t *ula, uint8_t data);

/* End this T-state and begin the next. */
static inline void ula_tick(ula_t *ula) {
  if (++ula->frame_tick == ula->frame.ticks_per_frame) {
    ula->frame_tick = 0;
    ula->frame_count++;
  }
  if (++ula->column == ula->frame.ticks_per_line) {
    ula->column = 0;
    if (++ula->line == ula->frame.lines_per_frame) {
      ula->line = 0;
    }
  }
}

/* The display file's address scramble, and the attribute that colours it.
 * `row` is a pixel row, 0 to 191; `column` a character column, 0 to 31. */
uint16_t ula_display_address(int row, int column);
uint16_t ula_attribute_address(int row, int column);

/* What a sample is worth on the cable, as 0xRRGGBB.
 *
 * These levels are chosen and not measured, which is the one thing here a
 * reader should not take on trust. The chip drives one line per gun and one
 * more for brightness, so the shape of the answer is fixed — two levels a
 * gun, and eight colours twice over — but nothing found while this was
 * written gave the voltages those lines actually reach, neither the service
 * manual, whose analogue section is not reproduced, nor the reverse
 * engineering of the board. The pair below is the one the field has settled
 * on. A measurement off real hardware would replace it, and the picture
 * would shift a little when it did.
 *
 * Bright black is still black: codes 0 and 8 are the same colour, because
 * brightness lifts a gun that is already off nowhere. */
uint32_t ula_rgb(uint8_t sample);

#endif
