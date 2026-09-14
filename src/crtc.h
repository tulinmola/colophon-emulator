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
 * Type 0 is the type implemented, and four things are not its alone. The first
 * is what a machine can read of the chip: the registers each type hands back
 * on the read port, and the status register type 1 alone has, are answered for
 * types 0, 1 and 2 (ch. 21.2, 21.3), which is what a program names the chip by
 * (ch. 28.1.8, 28.1.9) — what a program probing for a type 2 would make of it
 * being a reading of those chapters and no evidence we did not write. The
 * second is the VSYNC. Types 1 and 2 cannot program its length, so it "is
 * fixed at 16" whatever R3h holds (ch. 16), and a pulse either of them is made
 * to begin by a write to R7 is counted "as if the VSYNC had started when C0=0"
 * and spends a line fewer than a type 0's (ch. 16.4.2, 16.4.3), where one
 * begun on a frame's own half line keeps all sixteen (ch. 28.1.4). Neither of
 * them takes the whole line below either, which ch. 19.7.1 gives "CRTC's 0, 3
 * and 4" alone. No line on the disc moves on the shortened count or the whole
 * line withheld: what stands behind those two is those chapters' sentences and
 * tests of our own. The third is how a type 1 reads R9 in the interlace video
 * mode, and it is the first of them the disc does grade: its odd-lined rows
 * come of an even R9 where a type 0's come of an odd one (ch. 19.5.3, 19.8.2),
 * and it reads the limit down to that parity where a type 0 reads it up — a
 * character of N lines wanting "the value N-1" of it where a type 0 asks
 * "value N-2" (ch. 19.4.1, 19.4.2), which is why a type 0 and a type 1 want R9
 * "programmed respectively with 6 and 7" for rows of the same four lines
 * (ch. 28.1.7). Entering that mode in the middle of a row is still a type 0's
 * rule here, and leaves a type 1 counting the long way round where ch. 19.8.2
 * sets its parity outright. A type 2 shares none of the third: ch. 19.4.3 and
 * 19.5.4 give it an interlace of its own, in which "parity is respected
 * whatever the values of R9 and C4", and it is answered here as a type 0 is.
 * The fourth is the frame parity itself. Types 1, 3 and 4 anticipate none of
 * it: ParityFrame "switch between each frame when C4 = C9 = C0 = 0" and does
 * so "whatever the value of R8" (ch. 19.5.3, 19.5.5), where a type 0 and a
 * type 2 take the parity R6 anticipated and hold it for ever once C4 can no
 * longer reach R6 (ch. 19.5.2, 19.5.4) — so those three cannot be frozen, and
 * cannot be made to add the interlace line to every frame. That line follows
 * each type's own parity (ch. 19.6.1 to 19.6.4). The disc grades this one and
 * is not pleased: Shaker's C (S) came right and fell silent, and of C (O)'s
 * twenty-four answers four fewer agree than before. Which way that reads
 * depends on the parity the chip wakes holding, because what a program uses to
 * set it — the R8 writes of ch. 19.5.3's own page — is not here. Every other
 * behaviour below is type 0's whatever the type is set to, and a number naming
 * none of the five is neither refused nor corrected.
 *
 * Implemented: the frame construction of Compendium ch. 6 as type 0
 * (HD6845S/UM6845) performs it — its register widths, its VMA/VMA' reload
 * rules, the counter widths a program can overrun, the last line decided while
 * C0 is 0 or 1, the vertical adjustment spent on C9 — armed on every last
 * line, taken back where R5 is cancelled in time or the last line itself is
 * unmade, opened by an R4 or R9 moved under a standing last line, and past
 * taking back once begun — and the block that stops one VSYNC condition
 * serving twice. Of R8 everything but the cursor's skew is read: the frame
 * parity this chip keeps in two states rather than one, the line either
 * interlace mode adds to the end of an even frame, the MID-VSYNC that holds an
 * even frame's VSYNC back to the middle of its line — which, beginning away
 * from a line's head, then runs longer than R3 asks for — the whole line an
 * odd frame's VSYNC waits where the video mode gives its rows an odd number of
 * lines (ch. 19.5.2, 19.7.1), and the counting of the interlace video mode,
 * where the raster address becomes C9 doubled with a parity filling the bit it
 * leaves and R9 is read up to that same parity — the doubling taken up a line
 * after the write that asks for it, the parity in the limit at once, which is
 * what leaves a row entered or left off its parity counting the long way
 * round, and what a program reads its own frame parity from (ch. 19.8.1).
 * Above those sit the SKEW-DISPTMG functions, which ch. 19.1 gives this type
 * and withholds from types 1 and 2: a delay of one character or two on both
 * edges of the R1 border, or the display shut outright, which takes the
 * interlace bits with it (ch. 19.2). Nothing outside this repository grades
 * those: what stands behind them is a reading of ch. 19.2's diagrams and no
 * evidence we did not write. The interlace line is asked for on the deadline
 * of its own that ch. 11.9 gives it, later than R5's three characters and read
 * on the last line a frame has — which may be one of R5's own, so a program
 * may add the line or take it back from inside an adjustment, and a frame R5
 * asked nothing of is held open for it. Nothing outside this repository grades
 * that deadline either. C0 names the character being drawn and holds it for
 * that whole microsecond, which is what a positional register write needs; the
 * last line and the vertical adjustment already read it, and both are settled
 * at the characters ch. 13.2.1 settles them at rather than wherever they next
 * stand. A VSYNC is authorized by the state ch. 13.2.2 has the chip raise at
 * C0=2 and cancel at the next C0=0, so a line too short to reach C0=2 costs
 * the next one its VSYNC and spends the equality besides; and an equality made
 * by hand at a line's head is a blocked VSYNC rather than a VSYNC, where past
 * C0=1 it triggers where it stands (ch. 16.4.1.1). A line of one character
 * never reaches C0=1, so "C9 processing management" is never enabled again and
 * C4, C9 and the VSYNC's own line count freeze where they stand, which is why
 * a pulse begun there never ends; the arming the last managed line made still
 * lands, and a C4 increment is all of it that can, once — and where that
 * increment falls on a last line it is ch. 13.2.6's worked table beginning
 * rather than a row ending, so the adjustment outlives the widening and C9 is
 * measured against R5 for the rest of the frame (ch. 13.2.1, 13.2.4, 13.2.6,
 * 16.4.1.2). Taking that up cost Shaker's C (P) its graded line, and the line
 * is worth less than it looks: read at three boot phases in forty on a chip
 * altered in no way at all, and at six in forty on this one, always with the
 * same right answer. The HSYNC's width is counted per character rather than
 * per line and goes on counting. R4 and R9 written under a frozen chip are
 * read here, where ch. 13.2.1 says they are no longer considered and
 * ch. 13.2.4 then wants them for the last line it assesses at C0=0; the
 * chapter is in two minds and this is our reading. The adjustment is armed as
 * the chip arms it: ch. 12.1 gives the window, "this management of additional
 * line(s) is managed when C0<2", ch. 13.2.5 has the chip "assess whether it is
 * on the last line, and if so, arm an internal flag by default" there, and
 * leaves C0=2 to "assess the conditions for disarming ... in particular by
 * testing the value of R5". That assessment takes an arm back on R5 alone, so
 * a last line unmade at C0=0 unmakes the arming with it (ch. 12.2) — the
 * interlace line is left out, its question being put "on the last line of a
 * frame" (ch. 11.9) and this one no longer being one, which is our reading and
 * ungraded; an R5 above 0 still admits a line whose C4 has gone past R4, which
 * the assessment cannot see; and an adjustment already begun is past both
 * (ch. 13.2.6), which is what keeps ch. 10.3.1.2's exception alive now the
 * disarm asks only what ch. 13.2.5 says it asks. A line too short to reach the
 * disarm keeps what it was armed with, as ch. 11.2.2 and ch. 12.2 have it at
 * "R0 < 2" — the disarm being read at C0=3, where a write made at C0=2 has
 * landed, and again at the head of the line after one of three characters,
 * which has no such character of its own. What such a line draws is here with
 * it. Ch. 11.2.2 lists the ways an adjustment comes about with R5 at 0 — an R4
 * or R9 moved at C0=1 of a last line, "or if C0 can never reach 2 because R0 <
 * 2" — and ch. 13.2.1 and ch. 13.2.5 say what that second one draws, both
 * inside their own R0=1 case: it lasts "1 line of 2 usec before ceasing (C4+1,
 * C9=0)", and only "on the next line" does "the end of additional management
 * reset C4 and C9 to 0". So a narrow line is entered before it is measured and
 * a wider one measured before it is entered, which is ch. 13.2.4's reminder
 * and what Shaker's graded E (1) holds this chip to: it times an R5 cancelled
 * on a 64-character last line, and a chip that gave a line there too would
 * answer four of its seven a line too long. Ch. 13.2.5's own account of that
 * ending — that it stops "when the calculated C9 becomes equal to R5" — cannot
 * produce the picture it draws two sentences later, C9 not being zeroed once
 * C4 has left R4; the picture is followed here and the mechanism is not, and
 * nothing outside this repository grades the narrow line either way. The
 * ceasing after one line is the R0=1 case's own; a line of one character keeps
 * its run instead, and "it is then R5 which controls the end ... To stop this
 * management, program R5 with C9+1" (ch. 13.2.6). Absent for want of a pin:
 * the cursor, its own skew and the light pen, which no host here wires. Two
 * HSYNCs cannot be contiguous on this type — "if position C0=R2 is encountered
 * when C3l reaches R3l, and R3l has not been modified on this position" —
 * which is what keeps a line shorter than its own sync out of the endless
 * HSYNC the other types fall into (ch. 15.3.1, 15.3.2). A write to R3 there is
 * the exception, taken as the modification whatever value it carries, and the
 * sync it lets through carries the old count on rather than starting from
 * nothing, which is where a R2.JIT HSYNC begins (ch. 15.3.3). That chapter's
 * own example moves R2 as well and comes out right without the exception being
 * reached at all, the new R3l carrying the old sync past its ending comparison
 * on its own. R3l updated during a HSYNC is here in both of its halves: the
 * write of the value C3l already holds, which stops the sync where it stands,
 * and the smaller value, which overflows C3l instead and runs the sync round
 * to it (ch. 14.5, 14.5.4). Not yet: the gap the R2.JIT leaves, the chip
 * beginning the second sync "around 3.5 Pixel-M2 after the one that has just
 * ended" where a pin reported once per character can only stay high, so the
 * Gate Array is given one sync here where the chip gives it two; a write that
 * changes R3l on that character, seen here by the comparison that ends the
 * sync as well, where the chip's is not; the R0 of ch. 13.7.2 enlarged on the
 * character C0 names 1, where the old value ends the line and the new one
 * counts C0 on; the per-type divergences of the timing; the rest of what
 * ch. 13.2.1 gives a line's first three microseconds — the counter updates
 * those characters schedule for a later one. Every other comparison is made
 * where it stands. Nor is the read table of ch. 21.2.3: it names a register by
 * three bits rather than five and mirrors itself onto the status port, where
 * types 3 and 4 are answered here as a type 2 is. Two of the border's rules
 * move DISPLAY ENABLE inside a character — the byte of border at C0=R0 on a
 * line where R1 was never reached and no skew stands ready to defer it to a
 * whole character of its own (ch. 17.6.2, 19.2.4), and the byte-by-byte
 * alternation an R6 of 0 makes on a frame's first line (ch. 18.3.2) — and both
 * are here, which is why a tick reports that pin for each of the two bytes a
 * character is drawn from rather than once.
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
  /* Which of the five types this is, numbered as ch. 4.2's table numbers
     them: 0 is Hitachi's HD6845S and UMC's UM6845, 1 UMC's UM6845R, 2
     Motorola's MC6845, and 3 and 4 the Amstrad ASICs that emulate one. That
     table splits type 1 into a 1-A and a 1-B, two behaviours observed of
     the one part, which nothing here tells apart. */
  uint8_t type;

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
  uint8_t c3h; /* VSYNC scanline counter, 4 bits: R3 high nibble, 0 counts 16,
                  and types 1 and 2 never read that nibble at all (ch. 16) */

  /* This line ends the frame. Decided while C0 is 0 or 1 and held for the
     rest of the line, so a register written afterwards cannot take it back
     (ch. 10.3.1.2). */
  bool last_line;
  bool vertical_adjustment_armed;
  /* Whether an adjustment has actually begun, as against being armed for
     one: "the additional management being in progress, it can no longer be
     cancelled on C0=2" (ch. 13.2.6), which is the character this file reads
     at C0=3, where a write made at C0=2 has landed, and again at the head of
     the line after one of three characters. */
  bool vertical_adjustment_in_progress;
  /* And whether the line it is giving is the last of them, which only a line
     too narrow to be disarmed can need: such an adjustment lasts "1 line of
     2 usec before ceasing (C4+1, C9=0)", and it is the line after that on
     which "the end of additional management reset C4 and C9 to 0" (ch.
     13.2.1). */
  bool adjustment_on_its_last_line;

  /* Whether the chip stood on a frame's first character last time it was
     asked. ParityFrame turns as that character is entered (ch. 19.5.2), and
     a chip frozen on it enters nothing: without this the parity would turn
     under a still picture every microsecond. */
  bool stood_on_the_frame_head;

  /* Frame parity, which the Compendium keeps in two states rather than one
     on a type 0 and a type 2 (ch. 19.5.2, 19.5.4). ParityFrame is this
     frame's, taken from ParityR6 at the frame's first character; ParityR6
     anticipates the next frame's where C4 stands on R6, and it does so
     whatever R8 holds. Where R6 stands above R4 C4 never reaches it, and
     both freeze — which is how a program stops those two alternating, and
     how it asks for the extra line on every frame rather than every other.
     Types 1, 3 and 4 keep the one state: ParityFrame turns over at each
     frame's own head, ParityR6 goes unread, and no R6 can stop them
     (ch. 19.5.3, 19.5.5). True is odd, and the power-on zeroes leave a type
     0 or a type 2 on an even first frame where the other three turn at that
     first head and so begin odd; real silicon wakes on whatever it wakes
     on, and every frame after inherits the phase. The document turns
     ParityR6 "when C4 reaches R6" and we read that comparison standing, as
     the same C4/R6 comparison is read for DISPLAY ENABLE (ch. 18.2.1); the
     two part company only for an R6 written mid-frame onto the row the chip
     already stands on, and nothing we can run grades that. */
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
  /* The state ch. 13.2.2 has the chip raise at C0=2 so that the next C0=0
     may read C4 against R7. No line has run at power-on and nothing has
     raised it, so this chip wakes holding it, which is the state a chip
     that has been running is in. */
  bool vsync_armed;
  /* "C9 processing management", which ch. 13.2.4 has the chip disable at
     C0=0 and enable again at C0=1, so that a line of one character leaves
     it disabled and C4, C9 and the VSYNC's line count frozen where they
     stand. It wakes standing for the same reason the one above does. */
  bool c9_processing_managed;
  /* And the one thing that still lands while it is disabled: "if C9 had
     reached R9 on the first C0=0, then the reset of C9 had been armed as
     well as the increment to C4. With C9 being frozen, only C4 will
     increment" (ch. 13.2.4). */
  bool c4_increment_armed;
  /* A VSYNC can begin anywhere in a line — where R7 is written the value C4
     already holds, and on every even frame of an interlace mode. C3h is
     then initialized at the next C0=0 instead of being advanced there, so
     the part line it began in is not one of R3's (ch. 16.4.1). */
  bool vsync_began_mid_line;
  /* And whether that late start was the half line an even interlaced frame
     raises its VSYNC on rather than an R7 written to meet C4, which is the
     only one of the two the shortened count belongs to (ch. 16.4.2). */
  bool vsync_began_on_its_half_line;

  /* DISPLAY ENABLE is two latches rather than two comparisons (ch. 6.1.3,
     18.2.1). The R1 one opens at the head of every line; the R6 one, once
     shut, is shut for the frame, and it outranks the other. */
  bool display_r1;
  /* What that latch was one character ago and two, because the SKEW-DISPTMG
     functions hold the display enable back by one or the other before it
     leaves the chip (ch. 19.2.3). */
  bool display_r1_earlier[2];
  bool display_r6;
  /* And the same condition as the type 1 status register reports it, which
     turns over a character earlier than the border itself is thrown: "bit 5
     of the Status register is updated when C0=R0" (ch. 21.3.3), where the
     border belongs to the line it is drawn on. The two are kept apart so
     that reading the port cannot move the picture. */
  bool status_border_r6;
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
  /* Whether the character being drawn is the one an HSYNC ended on: "two
     HSYNC's cannot be contiguous if position C0=R2 is encountered when C3l
     reaches R3l, and R3l has not been modified on this position"
     (ch. 15.3.1). */
  bool hsync_ended_here;
  /* Whether R3 was written in time to be read on that character, which is
     the modification that same sentence exempts. */
  bool r3_written_for_this_character;
  bool vsync;
} crtc_t;

/* Power-on as the given type. Real silicon leaves the register file
 * undefined; zeroes here, for determinism, which leaves the chip before a
 * line's first character — with the VSYNC's authorization and the
 * management of C9 the two states that wake standing. */
void crtc_init(crtc_t *crtc, uint8_t type);

/* Advance one character clock. Returns the output pins. */
uint64_t crtc_tick(crtc_t *crtc);

/* One bus transaction: CS, RS, RW and the data lanes in; the data lanes out
 * when the chip drives them. Where it does not — a read this type never
 * answers — the data passes through untouched, the bus left floating for
 * the machine to interpret. */
uint64_t crtc_access(crtc_t *crtc, uint64_t pins);

#endif
