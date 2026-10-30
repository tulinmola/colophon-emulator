/*
 * ay38910.c — the register file, the chip select, and the port the keyboard
 * arrives on.
 */
#include "ay38910.h"

/* Not every register is eight bits wide: the tone periods carry four bits
   in their high halves, the noise period five, the envelope shape four, so
   a register reads back only what the chip kept. */
static const uint8_t writable_bits[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

void ay38910_init(ay38910_t *psg) {
  *psg = (ay38910_t){0};
  psg->port_a_input = 0xFF;
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
      }
      break;
    case AY38910_READ:
      /* An unselected chip drives nothing: its "bidirectional buffers are
         forced to a high impedance state" (GI's data manual, §2.1.1), and
         the die gates every read by PSG_SEL. The bus keeps what the machine
         had on it. */
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
