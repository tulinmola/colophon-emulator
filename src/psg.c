/*
 * psg.c — the register file, and the port the keyboard arrives on.
 */
#include "psg.h"

/* Not every register is eight bits wide: the tone periods carry four bits
   in their high halves, the noise period five, the envelope shape four, so
   a register reads back only what the chip kept. */
static const uint8_t writable_bits[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

void psg_init(psg_t *psg) {
  *psg = (psg_t){0};
  psg->port_a_input = 0xFF;
}

uint8_t psg_access(psg_t *psg, psg_function function, uint8_t data) {
  switch (function) {
    case PSG_SELECT:
      psg->selected = data & 0x0F;
      break;
    case PSG_WRITE:
      psg->registers[psg->selected] = data & writable_bits[psg->selected];
      break;
    case PSG_READ:
      /* Port A gives the pins while it is an input and its own latch once it
         is an output; every other register simply reads back. */
      if (psg->selected == PSG_PORT_A && psg_port_a_is_input(psg)) {
        return psg->port_a_input;
      }
      return psg->registers[psg->selected];
    case PSG_INACTIVE:
      break;
  }
  return data;
}

void psg_present_port_a(psg_t *psg, uint8_t levels) { psg->port_a_input = levels; }
