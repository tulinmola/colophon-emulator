/*
 * ula.h — the Ferranti uncommitted logic array, stepped at the CPU's clock.
 *
 * Where a CPC divides the work between a 6845 and a Gate Array, a Spectrum
 * has one chip. It counts out the frame, reads the screen, turns bytes into
 * pixels, raises the interrupt, sends the monitor its composite sync, holds
 * the border colour and drives the speaker. It does not read memory: the
 * machine fetches the pair of bytes ula_fetch names and hands them over,
 * as it does for a Gate Array.
 *
 * Everything here describes the T-state the chip is standing on. ula_tick()
 * ends that T-state and begins the next, so a caller reads first and ticks
 * last.
 *
 * The frame is counted from the interrupt, which is where every published
 * timing for this machine is measured from.
 *
 * A sample is four bits — brightness, then green, red and blue — which is
 * the attribute byte's own colour encoding widened by one bit.
 *
 * Implemented: the frame, the interrupt, the screen fetch and its address
 * scramble, the serialiser, FLASH, the border, composite sync, and the
 * contention slot. Not modelled: the chip fetches ahead of the beam through
 * a pipeline this file collapses into one T-state, which is the pipeline a
 * floating-bus read observes — so the value an unattached port returns
 * cannot be answered from here yet.
 *
 * Sources:
 * - Sinclair ZX Spectrum Service Manual §5.1 — the ULA derives the CPU
 *   clock from an external 14MHz crystal; the dot clock is half of it, so
 *   two pixels leave the chip for every T-state.
 * - libspectrum's timings.c (Philip Kendall, Fuse),
 *   https://sourceforge.net/p/fuse-emulator/libspectrum/ci/master/tree/timings.c
 *   — the 5C/6C frame: 24 + 128 + 24 + 48 T-states to a line, 312 lines to
 *   a frame, the interrupt held 32 T-states, and the first displayed byte
 *   14336 T-states after it.
 * - "ZX Spectrum Compatible Interrupts" (Chris Smith),
 *   http://www.zxdesign.info/interrupts.shtml — those 64 lines are 8 of
 *   vertical sync and 56 of border, and, because 14336 divides exactly by a
 *   line, "the interrupt must be generated not at the start of a line, but
 *   at the same offset into a line as the first display byte and 64
 *   scanlines earlier". That offset is what places the frame's first
 *   T-state where it is. The page is served over plain http and only under
 *   the www name; https and the bare domain both refuse.
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
 *   and that the first contended T-state of a frame is 14335.
 * - comp.sys.sinclair FAQ, 16K/48K reference,
 *   https://worldofspectrum.org/faq/reference/48kreference.htm — FLASH
 *   swaps ink and paper every 16 frames, and the write to port 0xFE.
 */
#ifndef COLOPHON_ULA_H
#define COLOPHON_ULA_H

#include <stdbool.h>
#include <stdint.h>

/* The dot clock is twice the CPU's, so a T-state is two pixels wide. */
#define ULA_SAMPLES_PER_TICK 2

/* A line, in the order the beam walks it: the retrace first, so that a
   monitor timing its lines from the sync lands on the left border. */
#define ULA_RETRACE_TICKS 48
#define ULA_LEFT_BORDER_TICKS 24
#define ULA_DISPLAY_TICKS 128
#define ULA_RIGHT_BORDER_TICKS 24
#define ULA_TICKS_PER_LINE 224

#define ULA_VSYNC_LINES 8
#define ULA_FIRST_DISPLAY_LINE 64
#define ULA_DISPLAY_LINES 192
#define ULA_LINES_PER_FRAME 312
#define ULA_TICKS_PER_FRAME (ULA_TICKS_PER_LINE * ULA_LINES_PER_FRAME)

/* The interrupt opens the frame and is held this long. A CPU that has not
   finished an instruction by then misses it altogether. */
#define ULA_INTERRUPT_TICKS 32

/* The screen is 32 characters by 24, eight pixel rows to a character. */
#define ULA_COLUMNS 32
#define ULA_ROWS 24

typedef struct {
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

  uint8_t border;  /* port 0xFE bits 2-0 */
  bool speaker;    /* bit 4, which also drives the EAR socket */
  bool microphone; /* bit 3 */
} ula_t;

void ula_init(ula_t *ula);

/* Stand the chip at a given T-state of the frame. Anything that places it
 * from outside — a test, a snapshot — goes through here, because the beam
 * counters above are derived and would otherwise disagree with it. */
void ula_seek(ula_t *ula, uint32_t frame_tick);

/* Where the chip reads for the character it is painting. False outside the
 * picture, when it reads nothing and the border shows. A character is eight
 * pixels and so four T-states wide, so the same pair is named four times
 * over before it moves on. */
bool ula_fetch(const ula_t *ula, uint16_t *display, uint16_t *attribute);

/* Paint this T-state: two samples from the pair of bytes ula_fetch named,
 * or the border, or black where the beam is blanked. */
void ula_video(const ula_t *ula, uint8_t display, uint8_t attribute,
               uint8_t samples[ULA_SAMPLES_PER_TICK]);

/* The composite sync on its way to the monitor, asserted when active: the
 * line's retrace, and eight whole lines at the top of the frame. Unlike a
 * Gate Array's, the frame pulse is not serrated — the line pulses inside it
 * are simply absent, which is what a monitor timing itself from pulse
 * lengths has to be told to expect. */
bool ula_csync(const ula_t *ula);

/* The interrupt line, held for the first ULA_INTERRUPT_TICKS of a frame. */
bool ula_interrupt(const ula_t *ula);

/* T-states the chip would keep the bus from a contended memory access
 * beginning now; zero when it does not want the bus. The machine decides
 * what to do with them: this chip takes the bus by holding the CPU's clock,
 * which is not something a wait line can express.
 *
 * A port access is contended by a different rule, which turns on the port's
 * low bit and on whether its high byte looks like contended memory. That
 * rule is not here. */
uint8_t ula_contention(const ula_t *ula);

/* One command byte, as written to any port with A0 low. */
void ula_write(ula_t *ula, uint8_t data);

/* End this T-state and begin the next. */
void ula_tick(ula_t *ula);

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
