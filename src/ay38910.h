/*
 * ay38910.h — General Instrument's AY-3-8910 programmable sound generator,
 * as the AY-3-8912: its registers, its one I/O port and its sound.
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
 * The sound is made at an eighth of the clock pin's rate, one step a
 * stride of eight clocks. Each of the three tone channels counts steps up
 * to its period and toggles its output on reaching it, so a whole wave
 * takes 16·TP clocks; the noise counts the same way, and a 17-bit shift
 * register moves once every two of its periods, 16·NP clocks; the envelope
 * counts up to its period, and its level moves one step every two periods,
 * sixteen steps to a cycle of 256·EP clocks. A channel's tone and noise,
 * each unless disabled, gate its amplitude, the envelope's level or its
 * own, onto its output, which is a level from 0 to 15 and not yet a
 * voltage: what that level makes depends on what the pin is loaded with,
 * and that belongs to the board. A channel with both disabled gives its
 * amplitude steadily.
 *
 * What no measurement settles follows the die and is said where it is
 * done: a period of 0 behaves as 1, a period written below the count is
 * reached at the next step, any write to register 13 restarts the envelope
 * and its period count, and reset clears the divider's phase. The timing
 * to the clock follows the die too — which clock compares and which
 * counts, when after reset the first edge comes, what a write does in the
 * clock it lands in — and the tests hold every clock of 47 scenarios to
 * the die's model run as a simulation.
 *
 * Implemented: the register file, the function decoding, port A, the
 * generators and the mixer. Port B, which only the AY-3-8910 has, is not.
 *
 * Sources:
 * - General Instrument, "AY-3-8910/8912 Programmable Sound Generator Data
 *   Manual", February 1979,
 *   https://github.com/floooh/emu-info/blob/93d132f/gi/GI_AY-3-8910_Feb-1979.pdf
 *   — the tone, noise and envelope as divisions of the clock (§3.1, §3.2,
 *   §3.5), the mixer (§3.3), the envelope's shapes and its four bits
 *   (§3.5.2 and Fig. 7); and §2.1.1, p. 7: the high address bits "function
 *   as "chip selects"", A8 fixed to recognise 1, DA7–DA4 "programmed to
 *   recognize only a 0000 code", and "A valid high order address latches
 *   the register address".
 * - deathsoft and lvd, the AY-3-8910 reverse-engineered from its die,
 *   https://github.com/lvd2/ay-3-8910_reverse_engineered/blob/cf8d14d/rtl/ay_model.v
 *   — what the manual leaves out: counters that count up and
 *   compare, the noise register's taps and output, what a write to
 *   register 13 restarts, and the state reset leaves; an address that
 *   fails the match latches nothing and sets a flip-flop that takes PSG_SEL
 *   low, and reset sets it too; while PSG_SEL is low the chip neither
 *   writes nor drives a read. Run under Icarus Verilog 12.0, with its
 *   trireg nets kept as wires that hold their last value, it gave the
 *   levels the tests compare clock by clock (test/ay38910_model/).
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

#define AY38910_CHANNELS 3

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

  /* The phase of the clock divider, 0 to 7, counted from reset: the die's
     three /2 stages, which count down where this counts up. The die's
     clock phase f1 is high at 0 to 3 and low at 4 to 7. */
  uint8_t divider;
  /* Each channel's count of steps toward its period, and its output
     flip-flop: the die's channel_counter and tone_output. */
  uint16_t tone_counter[AY38910_CHANNELS];
  bool tone[AY38910_CHANNELS];
  /* The noise period's count, the flip-flop its periods toggle, and the
     17-bit register that moves as it rises: the die's noise_counter,
     pre_f4 and noise_reg. */
  uint8_t noise_counter;
  bool noise_half;
  uint32_t noise_register;
  /* The envelope period's count and the flip-flop its periods toggle, on
     whose fall the level steps (the die's env_counter and f_env); the step
     within the cycle, 0 to 15 (emx); whether alternation has turned the
     direction (env_decay_attack); whether the shape holds (env_hold), and
     whether a shape that does not continue has ended, which sends its
     level to 0 (env_cont). */
  uint16_t envelope_counter;
  bool envelope_half;
  uint8_t envelope_step;
  bool envelope_turned;
  bool envelope_holding;
  bool envelope_ended;
  /* A 0 the envelope's count was left at by a restart while f1 was low,
     which the die's first stage does not hold: until the count, it reaches
     a period of 0. */
  bool envelope_zero_unheld;

  /* Each channel's output, a level from 0 to 15: the die's ch_a, ch_b and
     ch_c. */
  uint8_t output[AY38910_CHANNELS];
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

/* An envelope found in its registers with no record of how far it has run,
 * as a snapshot leaves one: a shape that ends or holds is put where it
 * rests, as one started long before would be, and one that repeats goes on
 * from where a write to register 13 started it. */
void ay38910_settle_envelope(ay38910_t *psg);

/* The divider's phase at which the generators move. On the die each
 * counter is compared with its period at this phase (cnt_clk) and counts a
 * clock later (f1 falling); here it counts at once after the comparison,
 * which nothing outside the chip can tell apart but a write landing in the
 * clock between, and the writes allow for that. */
#define AY38910_PHASE_COMPARE 3

/* What ay38910_tick does at that phase. */
void ay38910_step(ay38910_t *psg);

/* One clock on the chip's clock pin. */
static inline void ay38910_tick(ay38910_t *psg) {
  psg->divider = (uint8_t)((psg->divider + 1) & 7);
  if (psg->divider == AY38910_PHASE_COMPARE) {
    ay38910_step(psg);
  } else if (psg->divider == AY38910_PHASE_COMPARE + 1) {
    psg->envelope_zero_unheld = false;
  }
}

#endif
