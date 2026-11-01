/*
 * gate_array.h — the Amstrad 40010 Gate Array: colour registers, ROM
 * enables, video mode, and the interrupt generator.
 *
 * The chip watches the CRTC's syncs — one gate_array_tick() per character
 * clock — holds the machine's maskable interrupt line, and turns the two
 * bytes the machine fetches for it into sixteen colour samples. Register
 * writes arrive through gate_array_write(), dispatched on the data byte the
 * way the silicon dispatches them. The machine wiring decodes the port,
 * carries the INT line to the CPU and hands over the fetched bytes; this
 * file does none of that.
 *
 * It is also the machine's clock: from 16MHz it makes the CPU's 4MHz and
 * the character clock's 1MHz, and it holds the CPU off the RAM for three
 * cycles in four, leaving it the fourth, on which the video fetch follows
 * its access. That hold is the READY signal, wired to the Z80's WAIT.
 *
 * Implemented: the registers, the interrupt generator, the byte-to-pixel
 * serialiser, the composite sync and READY. Not yet: the 40010's habit of
 * starting mode 2 one pixel early, which waits for Shaker and which the type 4
 * ASIC shares (ch. 9.1), a colour written in that mode changing a pixel sooner
 * with it on that chip (ch. 9.2.2); and where inside the character a new mode
 * begins, a few pixels in (ch. 9.3.4.1), a mode being taken here a whole
 * character at a time.
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 *
 * Sources:
 * - "The Amstrad CPC CRTC Compendium" v1.11 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.11-EN.pdf ch. 27 — the interrupt
 *   generator measured on hardware: the R52 counter and its name, the INT
 *   line maintained until acknowledged, bit 5 killed at acknowledge, and
 *   the rule two HSYNCs after VSYNC: an interrupt only if bit 5 is set,
 *   and the microsecond an interrupt waits after the end of the HSYNC
 *   that asked for it (ch. 27.6.1; ch. 27.6.2's diagrams put it R3+1
 *   after C0 reaches R2 for every width they draw, and ch. 27.6.5's a
 *   microsecond later where the sync reaches the chip a character late).
 *   Where "The Gate Array" states that rule inverted, the Compendium is
 *   the one whose reading matches the mechanism's purpose, and the one
 *   tested on silicon.
 * - "The Gate Array" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/gatearray —
 *   the command dispatch on data bits 7-6, the PENR/INKR/RMR layouts, the
 *   byte-to-pixel tables for the four modes, and the palette as measured on
 *   the outputs of a real 40010.
 * - The Compendium ch. 16.2.2 and 16.2.3 for the composite sync: the H06
 *   and V26 counters this file names, the four-microsecond C-HSYNC that
 *   begins two characters after the CRTC's, the four-line C-VSYNC that
 *   begins two HSYNCs after the CRTC's, the twenty-six lines of blanking,
 *   and the XNOR that inverts the line pulses during a frame sync.
 * - The Compendium ch. 7.1 — the Gate Array displays a character one
 *   microsecond after the CRTC hands over its address.
 * - The Compendium ch. 9.3 — where a mode write is taken: ch. 9.3.1's
 *   HSYNC-GA period, capped at six microseconds and needing two, and ch.
 *   9.3.2 and 9.3.3, which draw an OUT against R2=46, R3=14 a microsecond
 *   at a time.
 * - "Interrupts on the CPC/CPC+ and KC Compact" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/ints.html — RMR bit 4 clears the
 *   pending request along with the counter.
 */
#ifndef COLOPHON_GATE_ARRAY_H
#define COLOPHON_GATE_ARRAY_H

#include <stdbool.h>
#include <stdint.h>

/* Two bytes are fetched per character and always become sixteen pixel
   clocks: the Gate Array widens each pixel to suit the mode, so the byte
   rate holds while the resolution changes ("The Gate Array", video mode). */
#define GATE_ARRAY_SAMPLES_PER_CHARACTER 16

/* Hardware colour code 20 (the INKR command &54) is black — the level the
   Gate Array drives on R, G and B while the beam is blanked. */
#define GATE_ARRAY_BLACK 20

/* What a sample is drawn from when the beam is blanked: no colour register,
   so no colour written to one reaches it. */
#define GATE_ARRAY_UNLIT 17

/* What an HSYNC end is to the interrupt generator: one step of R52, or the
   check the frame makes two HSYNCs after its VSYNC began (ch. 27.3.2). */
typedef enum {
  GATE_ARRAY_NO_HSYNC_END = 0,
  GATE_ARRAY_R52_STEP,
  GATE_ARRAY_FRAME_CHECK,
} gate_array_hsync_end_t;

typedef struct {
  uint8_t pen;      /* the selected colour register: pens 0-15, 16 the border */
  uint8_t inks[17]; /* 5-bit hardware colour codes; [16] is the border */

  uint8_t mode;           /* the video mode in force, 0 to 3 */
  uint8_t mode_pending;   /* RMR bits 1-0 as last written, taken where the
                             HSYNC-GA period of the next line sync of two
                             characters or more ends (gate_array.c) */
  bool lower_rom_enabled; /* RMR bits 2 and 3: a cleared bit enables; these
                             store the enabled state directly */
  bool upper_rom_enabled;

  /* The interrupt generator. R52 is the Compendium's name for the 6-bit
     counter of HSYNC ends (ch. 27.1). The request line, once raised, is
     maintained until acknowledged (ch. 27.3.1). */
  uint8_t r52;
  bool interrupt_request;
  /* What an HSYNC end does to the interrupt generator is decided when it
     ends and done a character and a quarter later: "an interrupt always
     starts 1 µsec after the end of the HSYNC regardless of the CRTC"
     (Compendium ch. 27.6.1). */
  gate_array_hsync_end_t hsync_end_last_character; /* carried to the next */
  gate_array_hsync_end_t hsync_end_to_act_on;      /* done on its quarter 1 */
  bool interrupt_raised_this_microsecond;          /* by that, until the next character clock */
  /* Whether an end is acted on at the character clock that hands it over
     instead, a quarter sooner; set by a board for the chip it is (cpc.c). */
  bool acts_on_a_sync_end_at_the_clock;
  uint8_t hsyncs_until_vsync_check; /* the two-HSYNC delay after a VSYNC
                                       starts (ch. 27.3.2); 0 = not armed */
  bool hsync_previous;
  bool vsync_previous;

  /* Composite sync. The Gate Array does not pass the CRTC's syncs through:
     it counts them out again on its own two counters, which the Compendium
     names for the values they stop at (ch. 16.2.2). */
  uint8_t h06;   /* characters since the CRTC's HSYNC began */
  uint8_t v26;   /* CRTC HSYNC ends since its VSYNC began */
  bool vsync_ga; /* the Gate Array's own VSYNC handling is under way */
  bool sig_hsync;
  bool sig_vsync;
  bool black_hsync; /* the beam is blanked through the CRTC's HSYNC */
  bool black_vsync; /* and through 26 lines from the start of its VSYNC */

  /* The Gate Array displays a character one microsecond after the CRTC
     hands over its address (ch. 7.1), so what it draws now is what was
     fetched last time. */
  uint8_t latched_bytes[2];
  bool latched_display[2];
  /* A colour written in the microsecond a character is displayed reaches it
     part of the way in (Compendium ch. 9.2.2): what each sample of the last
     character drawn was drawn from, a pen, 16 for the border or
     GATE_ARRAY_UNLIT for the blanked beam. */
  uint8_t drawn_from[GATE_ARRAY_SAMPLES_PER_CHARACTER];

  /* Where the machine stands in the four CPU cycles that make a character.
     The real chip runs a sequencer over sixteen 16MHz ticks; counting the
     CPU's four is the same thing seen from further away. */
  uint8_t cpu_phase;
} gate_array_t;

/* Power-on. Both ROM enables come up enabled — the reset vector is fetched
 * through the lower ROM, so the silicon can reset no other way; the rest is
 * zeroed by convention. It also builds the byte-to-pens table the serialiser
 * paints from, which no chip has of its own, so one that has not been
 * through here paints pen 0 and no other. */
void gate_array_init(gate_array_t *gate_array);

/* One command byte, as written to the chip's port. Dispatch is on bits 7-6;
 * bit 5 has no effect on this chip (it selects RMR2 on the Plus ASIC). The
 * 11 pattern is the PAL's MMR, not ours, and is ignored. */
void gate_array_write(gate_array_t *gate_array, uint8_t data);

/* One character clock: watch the syncs, and act on the end of one handed
 * over here where the chip is set to act at the clock. */
void gate_array_tick(gate_array_t *gate_array, bool hsync, bool vsync);

/* The INT line, held from the moment the counter raises it until the CPU
 * acknowledges. Sampled every CPU cycle, where the character clock this
 * chip is ticked on comes round only every fourth. */
static inline bool gate_array_interrupt(const gate_array_t *gate_array) {
  return gate_array->interrupt_request;
}

/* The CPU has acknowledged the interrupt: the request drops and bit 5 of
 * R52 dies, so the next interrupt comes no closer than 32 lines — or 20,
 * if the counter had already passed 32 (ch. 27.7.1). Called on the cycle
 * the acknowledge's M1 ends, after that cycle's gate_array_advance_phase;
 * a request raised in that same microsecond is left standing. */
void gate_array_interrupt_acknowledged(gate_array_t *gate_array);

/* The composite sync on its way to the monitor, asserted when active. It is
 * the two internal sync states XNORed, which leaves the line pulses intact
 * during a frame sync but inverted — a broad pulse with serrations, which
 * is what lets a monitor hold both locks off one wire (ch. 16.2.2). */
static inline bool gate_array_csync(const gate_array_t *gate_array) {
  return gate_array->sig_hsync != gate_array->sig_vsync;
}

/* Move on by one of the CPU's four cycles, counting an HSYNC end for the
 * interrupt generator on the second of them, where the chip is not set to
 * count it at the clock. */
void gate_array_advance_phase(gate_array_t *gate_array);

/* Whether a character clock falls on the cycle just reached — the moment
 * the CRTC is read and two bytes are fetched. */
static inline bool gate_array_character_clock(const gate_array_t *gate_array) {
  return gate_array->cpu_phase == 0;
}

/* READY, held on three cycles in four so that a CPU access can only finish
 * on the fourth. This is what rounds every machine cycle up to a whole
 * microsecond and costs the CPU a quarter of its nominal speed.
 *
 * The chip knows nothing about which cycle the CPU is in: its designers'
 * trick "has been to continually generate 3 Tw followed by a 'no Tw'
 * cycle" (Compendium ch. 4.4.4), and the CPU meets that pattern wherever
 * its own sampling happens to fall. An
 * instruction whose T-states do not divide by four leaves the next one to
 * be stretched at its opcode fetch, which is how everything ends up
 * "linearized" onto the microsecond. */
static inline bool gate_array_ready(const gate_array_t *gate_array) {
  return gate_array->cpu_phase != 0;
}

/* Serialise one character. The two flags are the CRTC's display enable as
 * each of the character's two bytes finds it; flags and bytes are held a
 * microsecond before they reach the screen, so this writes out the set
 * handed over last time and keeps the flags. The bytes follow in
 * gate_array_fetch. */
void gate_array_video(gate_array_t *gate_array, bool display_first_byte, bool display_second_byte,
                      uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER]);

/* The display enable fell after the character's flags were handed over, a
 * write having shut it inside the character (crtc.h): both of its bytes are
 * drawn as border. */
void gate_array_shut_the_display(gate_array_t *gate_array);

/* The samples of the character drawn last, from the one named onwards, drawn
 * again in the colours in force now: the board names the sample a colour
 * written since the character was drawn reaches it from (cpc.c). */
void gate_array_redraw_with_colours_since(const gate_array_t *gate_array, uint8_t from,
                                          uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER]);

/* The two bytes read at the address the CRTC handed over with the flags,
 * held with them for the next gate_array_video. */
void gate_array_fetch(gate_array_t *gate_array, uint8_t byte0, uint8_t byte1);

/* What the Gate Array's three-state RGB logic puts on the cable for a
 * hardware colour code, as 0xRRGGBB. */
uint32_t gate_array_rgb(uint8_t colour_code);

#endif
