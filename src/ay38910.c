/*
 * ay38910.c — the register file, the chip select, the port the keyboard
 * arrives on, and the generators.
 */
#include <stddef.h>

#include "ay38910.h"

/* Not every register is eight bits wide: the tone periods carry four bits
   in their high halves, the noise period five, the envelope shape four, so
   a register reads back only what the chip kept. */
static const uint8_t writable_bits[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

/* Register 7's enables, a set bit disabling (GI's data manual, §3.3), and
   the envelope shape's four bits in register 13 (§3.5.2). */
#define TONE_DISABLED(channel) (1u << (channel))
#define NOISE_DISABLED(channel) (8u << (channel))
#define AMPLITUDE_FROM_ENVELOPE 0x10
#define SHAPE_HOLD 0x01
#define SHAPE_ALTERNATE 0x02
#define SHAPE_ATTACK 0x04
#define SHAPE_CONTINUE 0x08

#define NOISE_REGISTER_BITS 0x1FFFFu

static uint16_t tone_period(const ay38910_t *psg, int channel) {
  const uint8_t *pair = &psg->registers[(size_t)channel * 2];
  return (uint16_t)(pair[0] | (pair[1] & 0x0F) << 8);
}

static uint16_t envelope_period(const ay38910_t *psg) {
  return (uint16_t)(psg->registers[11] | psg->registers[12] << 8);
}

/* The die's noise output is the register's last stage inverted. */
static bool noise(const ay38910_t *psg) { return (psg->noise_register & 0x10000) == 0; }

static uint8_t envelope_level(const ay38910_t *psg) {
  if (psg->envelope_ended) {
    return 0;
  }
  bool rising = ((psg->registers[13] & SHAPE_ATTACK) != 0) != psg->envelope_turned;
  uint8_t step = psg->envelope_step & 0x0F;
  return rising ? step : (uint8_t)(15 - step);
}

/* What reaches each output: the amplitude, gated by whichever of tone and
   noise the channel has enabled, as the die's channel gates it. With both
   disabled the amplitude passes steadily: "Disabling noise and tone does
   not turn off a channel" (GI's data manual, §3.3). */
static void update_outputs(ay38910_t *psg) {
  uint8_t enables = psg->registers[AY38910_MIXER];
  bool noise_level = noise(psg);
  uint8_t envelope = envelope_level(psg);
  for (int channel = 0; channel < AY38910_CHANNELS; channel++) {
    bool sounding = (psg->tone[channel] || (enables & TONE_DISABLED(channel))) &&
                    (noise_level || (enables & NOISE_DISABLED(channel)));
    uint8_t amplitude = psg->registers[8 + channel];
    uint8_t level = (amplitude & AMPLITUDE_FROM_ENVELOPE) ? envelope : (amplitude & 0x0F);
    psg->output[channel] = sounding ? level : 0;
  }
}

/* The manual does not say what a write to register 13 does to a running
   envelope. On the die every such write, of the same shape too, restarts
   the step count and clears the period's count and its flip-flop (env_rst,
   ep_rst). While f1 is high the count is held at 0 until f1 falls. In the
   comparing clock it stands at 1 here, the count the die makes as f1 falls
   made already. While f1 is low the count's first stage keeps the charge it
   took while f1 was high, before the count it has made since: from a count
   that was even then it reads 1 at once, and from one that was odd 0,
   which it does not hold, so that until the next count it reaches a period
   of 0 as a counter reaches its period. Here the field has counted since,
   and its parity is the other way about. */
static void restart_envelope(ay38910_t *psg) {
  bool odd = (psg->envelope_counter & 1) != 0;
  bool low = psg->divider > AY38910_PHASE_COMPARE;
  psg->envelope_counter = psg->divider == AY38910_PHASE_COMPARE || (low && odd) ? 1 : 0;
  psg->envelope_zero_unheld = low && !odd;
  psg->envelope_half = false;
  psg->envelope_step = 0;
  psg->envelope_turned = false;
  psg->envelope_holding = false;
  psg->envelope_ended = false;
}

/* One step of the envelope's level, sixteen to a cycle. At a cycle's end
   a shape without Continue "will reset to 0000 after one cycle and hold at
   that count"; Alternate "reverses count direction (up-down) after each
   cycle"; Hold limits it "to one cycle, holding the last count", and with
   Alternate as well holds the turned one (GI's data manual, §3.5.2 and
   Fig. 7). */
static void advance_envelope(ay38910_t *psg) {
  if (psg->envelope_holding || psg->envelope_ended) {
    return;
  }
  if (psg->envelope_step < 15) {
    psg->envelope_step++;
    return;
  }
  uint8_t shape = psg->registers[13];
  if (!(shape & SHAPE_CONTINUE)) {
    psg->envelope_ended = true;
    return;
  }
  if (shape & SHAPE_ALTERNATE) {
    psg->envelope_turned = !psg->envelope_turned;
  }
  if (shape & SHAPE_HOLD) {
    psg->envelope_holding = true;
    return;
  }
  psg->envelope_step = 0;
}

/* Whether a counter has reached its period: "carry_out=1 when
   counter>=period" in the die's model, with a period of 0 reached as 1 is.
   A counter at 0 is one held since its last carry, its last restart or
   reset, and reaches nothing. */
static bool reached(uint16_t counter, uint16_t period) { return counter >= (period ? period : 1); }

/* Each carry says whether it changed anything a channel's output reads,
   and leaves the counter as the comparison found it or, carrying, at 0. */
static bool carry_tone(ay38910_t *psg, int channel) {
  if (!reached(psg->tone_counter[channel], tone_period(psg, channel))) {
    return false;
  }
  psg->tone_counter[channel] = 0;
  psg->tone[channel] = !psg->tone[channel];
  return true;
}

static bool carry_noise(ay38910_t *psg) {
  if (!reached(psg->noise_counter, psg->registers[6] & 0x1F)) {
    return false;
  }
  psg->noise_counter = 0;
  psg->noise_half = !psg->noise_half;
  if (psg->noise_half) {
    /* Taps at stages 17 and 14, and a 1 shifted in while the register is
       all zeroes, which is how it leaves the state reset puts it in. */
    uint32_t bits = psg->noise_register;
    uint32_t in = (((bits >> 16) ^ (bits >> 13)) & 1) | (bits == 0);
    psg->noise_register = ((bits << 1) | in) & NOISE_REGISTER_BITS;
    return true;
  }
  return false;
}

static bool carry_envelope(ay38910_t *psg) {
  bool reaches = psg->envelope_zero_unheld ? envelope_period(psg) == 0
                                           : reached(psg->envelope_counter, envelope_period(psg));
  if (!reaches) {
    return false;
  }
  psg->envelope_zero_unheld = false;
  psg->envelope_counter = 0;
  psg->envelope_half = !psg->envelope_half;
  if (psg->envelope_half) {
    return false;
  }
  advance_envelope(psg);
  return true;
}

/* The outputs are worked out again only when a carry changed something a
   channel is taking: its tone or noise while enabled, or the envelope
   while some amplitude follows it. */
void ay38910_step(ay38910_t *psg) {
  uint8_t enables = psg->registers[AY38910_MIXER];
  bool changed = false;
  for (int channel = 0; channel < AY38910_CHANNELS; channel++) {
    changed |= carry_tone(psg, channel) && !(enables & TONE_DISABLED(channel));
    psg->tone_counter[channel]++;
  }
  changed |= carry_noise(psg) && (enables & 0x38) != 0x38;
  psg->noise_counter++;
  bool enveloped =
      (psg->registers[8] | psg->registers[9] | psg->registers[10]) & AMPLITUDE_FROM_ENVELOPE;
  changed |= carry_envelope(psg) && enveloped;
  psg->envelope_counter++;
  if (changed) {
    update_outputs(psg);
  }
}

/* The comparisons are wired, not clocked, and the clock that compares
   holds the strobe through any write in it: a period written then is
   compared at once, by a counter that has not already carried. On the die
   that counter has not yet counted for this step, so it is compared here
   without the count made already, and keeps it. */
static void compare_after_writing(ay38910_t *psg, uint8_t number) {
  if (psg->divider != AY38910_PHASE_COMPARE) {
    return;
  }
  if (number <= 5 && psg->tone_counter[number / 2] > 0) {
    psg->tone_counter[number / 2]--;
    carry_tone(psg, number / 2);
    psg->tone_counter[number / 2]++;
  } else if (number == 6 && psg->noise_counter > 0) {
    psg->noise_counter--;
    carry_noise(psg);
    psg->noise_counter++;
  } else if ((number == 11 || number == 12) && psg->envelope_counter > 0) {
    psg->envelope_counter--;
    carry_envelope(psg);
    psg->envelope_counter++;
  }
}

/* Reset leaves every tone flip-flop and the noise's set, and the
   envelope's clear (the die's model, traced after reset). */
void ay38910_init(ay38910_t *psg) {
  *psg = (ay38910_t){0};
  psg->port_a_input = 0xFF;
  for (int channel = 0; channel < AY38910_CHANNELS; channel++) {
    psg->tone[channel] = true;
  }
  psg->noise_half = true;
}

uint8_t ay38910_access(ay38910_t *psg, ay38910_function function, uint8_t data) {
  switch (function) {
    case AY38910_SELECT:
      psg->chip_selected = (data & 0xF0) == 0;
      if (psg->chip_selected) {
        psg->selected = data & 0x0F;
      }
      break;
    case AY38910_WRITE:
      if (psg->chip_selected) {
        psg->registers[psg->selected] = data & writable_bits[psg->selected];
        if (psg->selected == 13) {
          restart_envelope(psg);
        }
        compare_after_writing(psg, psg->selected);
        update_outputs(psg);
      }
      break;
    case AY38910_READ:
      /* An unselected chip drives nothing: its "bidirectional buffers are
         forced to a high impedance state" (GI's data manual, §2.1.1), and
         the die gates every read by PSG_SEL. The bus keeps what was put on
         it. */
      if (!psg->chip_selected) {
        break;
      }
      /* Port A gives the pins while it is an input and its own latch once it
         is an output; every other register simply reads back. */
      if (psg->selected == AY38910_PORT_A && ay38910_port_a_is_input(psg)) {
        return psg->port_a_input;
      }
      return psg->registers[psg->selected];
    case AY38910_INACTIVE:
      break;
  }
  return data;
}

void ay38910_present_port_a(ay38910_t *psg, uint8_t levels) { psg->port_a_input = levels; }

void ay38910_settle_envelope(ay38910_t *psg) {
  uint8_t shape = psg->registers[13];
  if (!(shape & SHAPE_CONTINUE)) {
    psg->envelope_ended = true;
  } else if (shape & SHAPE_HOLD) {
    psg->envelope_step = 15;
    psg->envelope_turned = (shape & SHAPE_ALTERNATE) != 0;
    psg->envelope_holding = true;
  }
  update_outputs(psg);
}
