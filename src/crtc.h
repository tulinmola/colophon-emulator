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
 * Implemented: the frame construction of Compendium ch. 6 as type 0
 * (HD6845S/UM6845) performs it — its register widths, its readable set, its
 * VMA/VMA' reload rules, the counter widths a program can overrun, the last
 * line decided while C0 is 0 or 1, the vertical adjustment spent on C9 —
 * asked for by R5, taken back where R5 is cancelled in time, and opened by
 * an R4 or R9 moved under a standing last line — and the block that stops
 * one VSYNC condition serving twice. Of R8 everything but the cursor's skew
 * is read: the frame parity this chip keeps in two states rather than one,
 * the line either interlace mode adds to the end of an even frame, the
 * MID-VSYNC that holds an even frame's VSYNC back to the middle of its line
 * — which, beginning away from a line's head, then runs longer than R3 asks
 * for — and the counting of the interlace video mode, where the raster
 * address becomes C9 doubled with a parity filling the bit it leaves and R9
 * is read up to that same parity — the doubling taken up a line after the
 * write that asks for it, the parity in the limit at once, which is what
 * leaves a row entered or left off its parity counting the long way round,
 * and what a program reads its own frame parity from (ch. 19.8.1). Above
 * those sit the SKEW-DISPTMG functions, which ch. 19.1 gives this type and
 * withholds from types 1 and 2: a delay of one character or two on both
 * edges of the R1 border, or the display shut outright, which takes the
 * interlace bits with it (ch. 19.2). Nothing outside this repository grades
 * those: what stands behind them is a reading of ch. 19.2's diagrams and no
 * evidence we did not write. The interlace line is asked for on the
 * deadline of its own that ch. 11.9 gives it, later than R5's three
 * characters and read on the last line a frame has — which may be one of
 * R5's own, so a program may add the line or take it back from inside an
 * adjustment, and a frame R5 asked nothing of is held open for it. Nothing
 * outside this repository grades that deadline either. C0 names the
 * character being drawn and holds it for that whole microsecond, which is
 * what a positional register write needs; the last line and the vertical
 * adjustment already read it, and both are settled at the characters ch.
 * 13.2.1 settles them at rather than wherever they next stand. Not yet: the
 * cursor, its own skew and the lightpen, no host here wiring those pins;
 * the per-type divergences; the rest of what ch. 13.2.1 gives a line's
 * first three microseconds — the counter updates those characters schedule
 * for a later one — and the VSYNC arming of ch. 13.2.2. Every other
 * comparison is made where it stands. Two of the border's rules move
 * DISPLAY ENABLE inside a character — the byte of border at C0=R0 on a line
 * where R1 was never reached and no skew stands ready to defer it to a
 * whole character of its own (ch. 17.6.2, 19.2.4), and the byte-by-byte
 * alternation an R6 of 0 makes on a frame's first line (ch. 18.3.2) — and
 * both are here, which is why a tick reports that pin for each of the two
 * bytes a character is drawn from rather than once.
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 *
 * Sources:
 * - "The Amstrad CPC CRTC Compendium" v1.11 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.11-EN.pdf — the counter names we
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
 * bits 29..   control pins, names as the datasheets print them, and one
 *             this chip has no pin for (see DISPLAY ENABLE below) */
/* DISPLAY ENABLE. The chip has one such pin, and this reports it twice: two
 * of type 0's rules move it half a character — one of them only while no
 * skew stands ready to defer that border to a whole character of its own —
 * and the machine fetches two bytes for every character the chip names, so
 * what a tick owes the machine is the pin as each byte finds it (ch.
 * 17.6.2, 18.3.2, 19.2.4). The datasheet's
 * name is kept for the first, which is where the pin stands when the
 * character begins; the second wears a name of its own because the
 * datasheet has none for it. */
#define CRTC_DISPTMG (1ULL << 29)
#define CRTC_DISPTMG_SECOND_BYTE (1ULL << 35)
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

  /* Counters, named as the Compendium names them (ch. 3.1). Each is
     narrower than the byte holding it, and a program can leave one above
     its limit; the widths are what bring it back (ch. 10.3.1.1, 12.1). */
  uint8_t c0;  /* horizontal character counter, 8 bits; after a tick it names
                  the character that tick drew */
  uint8_t c9;  /* scanline within the character row, 5 bits; drives RA. Type
                  0 has no C5 and spends C9 on the vertical adjustment too
                  (ch. 11.2.2) */
  uint8_t c4;  /* character row counter, 7 bits */
  uint8_t c3l; /* HSYNC width counter, 4 bits: R3 low nibble. A nibble of 0
                  is no HSYNC at all on this type, where types 2, 3 and 4
                  read it as 16 (ch. 14.1, 14.5) */
  uint8_t c3h; /* VSYNC scanline counter, 4 bits: R3 high nibble, 0 counts 16 */

  /* This line ends the frame. Decided while C0 is 0 or 1 and held for the
     rest of the line, so a register written afterwards cannot take it back
     (ch. 10.3.1.2). */
  bool last_line;
  bool in_vertical_adjustment;

  /* Frame parity, which the Compendium keeps in two states rather than one
     (ch. 19.5.2). ParityFrame is this frame's, taken from ParityR6 at the
     frame's first character; ParityR6 anticipates the next frame's where C4
     stands on R6, and it does so whatever R8 holds. Where R6 stands above
     R4 C4 never reaches it, and both freeze — which is how a program stops
     the frames alternating, and how it asks for the extra line on every
     frame rather than every other. True is odd, and the power-on zeroes
     leave the first frame even; real silicon wakes on whatever it wakes on,
     and every frame after inherits the phase. The document turns ParityR6
     "when C4 reaches R6" and we read that comparison standing, as the same
     C4/R6 comparison is read for DISPLAY ENABLE (ch. 18.2.1); the two part
     company only for an R6 written mid-frame onto the row the chip already
     stands on, and nothing we can run grades that. */
  bool parity_frame;
  bool parity_r6;
  /* What R8 answered at C0=R0, which is where ch. 11.9 asks it and a
     microsecond before the line it decides could begin. */
  bool interlace_line_owed;
  /* The one line interlace adds after the R5 lines, and one to a frame
     however the adjustment carrying it ends (ch. 11.9, 19.6.1). */
  bool interlace_line_given;
  /* The interlace video mode as the counters see it, which is not quite as
     R8 holds it: the mode a write asks for is taken up at the next C0=0,
     after that line's own R9 test (ch. 19.8.1). */
  bool interlace_video_mode;
  /* A chip that has drawn nothing has no character to leave behind, so the
     first tick draws one instead of advancing past one. Zero is the
     power-on state, which is what leaves the first tick drawing a line's
     first character rather than its second. */
  bool has_drawn_a_character;
  /* One C4/R7 equality raises one VSYNC: the comparison must change, by C4
     moving or R7 being written, before it raises another (ch. 16.3). */
  bool vsync_blocked;
  /* A VSYNC can begin anywhere in a line — where R7 is written the value C4
     already holds, and on every even frame of an interlace mode. C3h is
     then initialized at the next C0=0 instead of being advanced there, so
     the part line it began in is not one of R3's (ch. 16.4.1). */
  bool vsync_began_mid_line;

  /* DISPLAY ENABLE is two latches rather than two comparisons (ch. 6.1.3,
     18.2.1). The R1 one opens at the head of every line; the R6 one, once
     shut, is shut for the frame, and it outranks the other. */
  bool display_r1;
  /* What that latch was one character ago and two, because the SKEW-DISPTMG
     functions hold the display enable back by one or the other before it
     leaves the chip (ch. 19.2.3). */
  bool display_r1_earlier[2];
  bool display_r6;
  /* The line ran its length. Only the C0 that returns to 0 from R0 opens the
     display again — one that got there by overflowing 255 does not (ch.
     17.1). */
  bool c0_reached_r0;

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
 * for determinism, which leaves the chip before a line's first character. */
void crtc_init(crtc_t *crtc);

/* Advance one character clock. Returns the output pins. */
uint64_t crtc_tick(crtc_t *crtc);

/* One bus transaction: CS, RS, RW and the data lanes in; the data lanes out
 * when the chip drives them. Where it does not — a read this type never
 * answers — the data passes through untouched, the bus left floating for
 * the machine to interpret. */
uint64_t crtc_access(crtc_t *crtc, uint64_t pins);

#endif
