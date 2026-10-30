/*
 * ay38910.h — General Instrument's AY-3-8910 programmable sound generator,
 * as far as the AY-3-8912's registers and its one I/O port.
 *
 * The chip has sixteen registers and, as the AY-3-8912, a single eight-bit
 * port. It is addressed through three function lines — BDIR, BC1 and BC2 —
 * which between them say: do nothing, latch a register number, write the
 * latched register, or read it. The CPC ties BC2 high, leaving two lines
 * and four functions.
 *
 * An address on the data bus is eight bits and a register number four. The
 * high four are a chip select, matched against a code fixed in the chip,
 * 0000 unless a customer ordered another: a matching address latches the
 * low four, and any other latches nothing and leaves the chip unselected,
 * deaf to writes and silent on reads until an address matches again. A8
 * must be high as well, and the CPC ties it high too; the AY-3-8912 has no
 * A9. Reset leaves the chip unselected.
 *
 * Implemented: the register file, the function decoding, and port A. The
 * three tone channels, the noise generator, the mixer and the envelope are
 * stored and not sounded — nothing here makes audio yet, and a register
 * this file remembers but does not act on is a fact a caller should know
 * rather than discover.
 *
 * Sources:
 * - General Instrument, "AY-3-8910/8912 Programmable Sound Generator Data
 *   Manual", February 1979,
 *   https://raw.githubusercontent.com/floooh/emu-info/master/gi/GI_AY-3-8910_Feb-1979.pdf
 *   — §2.1.1, p. 7: the high address bits "function as "chip selects"",
 *   A8 fixed to recognise 1, DA7–DA4 "programmed to recognize only a 0000
 *   code", and "A valid high order address latches the register address".
 * - deathsoft and lvd, the AY-3-8910 reverse-engineered from its die,
 *   https://github.com/lvd2/ay-3-8910_reverse_engineered (rtl/ay_model.v
 *   at cf8d14d) — what the manual leaves out: an address that fails the
 *   match latches nothing and sets a flip-flop that takes PSG_SEL low, and
 *   reset sets it too; while PSG_SEL is low the chip neither writes nor
 *   drives a read.
 * - Amstrad, CPC6128 service manual, 1985,
 *   https://archive.org/details/Amstrad_CPC6128_Service_Manual_1985_Amstrad_Consumer_Electronics_a
 *   — the CPU circuit diagram (leaf 16), where the AY's A8 (pin 17) and BC2
 *   (pin 19) go to +5V.
 * - "AY-3-8912 PSG" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/psg.html — the function encoding, the
 *   sequences for selecting, reading and writing a register, and the rule
 *   that the inactive function must come between two others.
 * - "Reading the keyboard and Joysticks" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/keyboard.html — that the matrix arrives
 *   at port A, which is register 14.
 */
#ifndef COLOPHON_AY38910_H
#define COLOPHON_AY38910_H

#include <stdbool.h>
#include <stdint.h>

/* Register 7 is the mixer, and its two top bits are the directions of the
   I/O ports rather than anything to do with sound. A set bit is an output,
   which is the opposite sense to the 8255's. */
#define AY38910_MIXER 7
#define AY38910_PORT_A 14

/* The functions BDIR and BC1 select, with BC2 high. */
typedef enum {
  AY38910_INACTIVE = 0,
  AY38910_READ = 1,
  AY38910_WRITE = 2,
  AY38910_SELECT = 3,
} ay38910_function;

typedef struct {
  uint8_t registers[16];
  /* The register address latch: the low four bits of the last address
     that selected the chip. */
  uint8_t selected;
  /* The die's PSG_SEL outside a select: false from reset, and from an
     address whose high four bits are not 0000, until one whose are. */
  bool chip_selected;
  /* What the machine has wired to port A. Read only while the port is an
     input; when it is an output the register reads its own latch back. */
  uint8_t port_a_input;
} ay38910_t;

void ay38910_init(ay38910_t *psg);

/* True when register 7 leaves port A an input, which is how a machine that
 * reads something through it must keep the chip. */
static inline bool ay38910_port_a_is_input(const ay38910_t *psg) {
  return (psg->registers[AY38910_MIXER] & 0x40) == 0;
}

/* One bus transaction. `data` is what the machine has on the chip's data
 * bus; the return is what the chip puts back, which is `data` untouched for
 * every function but a read of a selected chip. */
uint8_t ay38910_access(ay38910_t *psg, ay38910_function function, uint8_t data);

/* The machine presenting levels to port A. */
void ay38910_present_port_a(ay38910_t *psg, uint8_t levels);

#endif
