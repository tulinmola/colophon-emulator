/*
 * psg.h — the General Instrument AY-3-8912 sound generator, as far as its
 * registers and its one I/O port.
 *
 * The chip has sixteen registers and, on this variant, a single eight-bit
 * port. It is addressed through three function lines — BDIR, BC1 and BC2 —
 * which between them say: do nothing, latch a register number, write the
 * latched register, or read it. BC2 is tied high wherever this chip is
 * used, leaving two lines and four functions.
 *
 * Implemented: the register file, the function decoding, and port A. The
 * three tone channels, the noise generator, the mixer and the envelope are
 * stored and not sounded — nothing here makes audio yet, and a register
 * this file remembers but does not act on is a fact a caller should know
 * rather than discover.
 *
 * Sources:
 * - "AY-3-8912 PSG" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/psg.html — the function encoding, the
 *   sequences for selecting, reading and writing a register, and the rule
 *   that the inactive function must come between two others.
 * - "Reading the keyboard and Joysticks" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/keyboard.html — that the matrix arrives
 *   at port A, which is register 14.
 */
#ifndef COLOPHON_PSG_H
#define COLOPHON_PSG_H

#include <stdbool.h>
#include <stdint.h>

/* Register 7 is the mixer, and its two top bits are the directions of the
   I/O ports rather than anything to do with sound. A set bit is an output,
   which is the opposite sense to the 8255's. */
#define PSG_MIXER 7
#define PSG_PORT_A 14

/* The functions BDIR and BC1 select, with BC2 high. */
typedef enum {
  PSG_INACTIVE = 0,
  PSG_READ = 1,
  PSG_WRITE = 2,
  PSG_SELECT = 3,
} psg_function;

typedef struct {
  uint8_t registers[16];
  uint8_t selected;
  /* What the machine has wired to port A. Read only while the port is an
     input; when it is an output the register reads its own latch back. */
  uint8_t port_a_input;
} psg_t;

void psg_init(psg_t *psg);

/* True when register 7 leaves port A an input, which is how a machine that
 * reads something through it must keep the chip. */
static inline bool psg_port_a_is_input(const psg_t *psg) {
  return (psg->registers[PSG_MIXER] & 0x40) == 0;
}

/* One bus transaction. `data` is what the machine has on the chip's data
 * bus; the return is what the chip puts back, which is `data` untouched for
 * every function but a read. */
uint8_t psg_access(psg_t *psg, psg_function function, uint8_t data);

/* The machine presenting levels to port A. */
void psg_present_port_a(psg_t *psg, uint8_t levels);

#endif
