/*
 * ppi.c — ports, directions, and the control word.
 */
#include "ppi.h"

/* Control word, mode set (bit 7 = 1):
 *   bit 6-5  group A mode        bit 4  port A direction
 *   bit 3    port C upper        bit 2  group B mode
 *   bit 1    port B direction    bit 0  port C lower
 * A direction bit set means input. */
static void configure(ppi_t *ppi, uint8_t word) {
  ppi->control = word;
  ppi->port_a_input = (word & 0x10) != 0;
  ppi->port_c_upper_input = (word & 0x08) != 0;
  ppi->port_b_input = (word & 0x02) != 0;
  ppi->port_c_lower_input = (word & 0x01) != 0;
  /* A mode-set clears the output latches. */
  ppi->output[PPI_PORT_A] = 0;
  ppi->output[PPI_PORT_B] = 0;
  ppi->output[PPI_PORT_C] = 0;
}

void ppi_init(ppi_t *ppi) {
  *ppi = (ppi_t){0};
  ppi->input[PPI_PORT_A] = 0xFF;
  ppi->input[PPI_PORT_B] = 0xFF;
  ppi->input[PPI_PORT_C] = 0xFF;
  /* Reset leaves every port an input, which is the mode-set word &9B. */
  configure(ppi, 0x9B);
}

uint8_t ppi_read(const ppi_t *ppi, ppi_selection selection) {
  switch (selection) {
    case PPI_PORT_A:
      return ppi->port_a_input ? ppi->input[PPI_PORT_A] : ppi->output[PPI_PORT_A];
    case PPI_PORT_B:
      return ppi->port_b_input ? ppi->input[PPI_PORT_B] : ppi->output[PPI_PORT_B];
    case PPI_PORT_C: {
      /* Each nibble answers from wherever its own direction points. */
      uint8_t lower = ppi->port_c_lower_input ? ppi->input[PPI_PORT_C] : ppi->output[PPI_PORT_C];
      uint8_t upper = ppi->port_c_upper_input ? ppi->input[PPI_PORT_C] : ppi->output[PPI_PORT_C];
      return (uint8_t)((upper & 0xF0) | (lower & 0x0F));
    }
    default:
      return ppi->control;
  }
}

void ppi_write(ppi_t *ppi, ppi_selection selection, uint8_t data) {
  if (selection == PPI_CONTROL) {
    if (data & 0x80) {
      configure(ppi, data);
    } else {
      /* Bit set/reset: bits 3-1 name a bit of port C, bit 0 is the value. */
      uint8_t bit = (uint8_t)(1u << ((data >> 1) & 0x07));
      if (data & 0x01) {
        ppi->output[PPI_PORT_C] |= bit;
      } else {
        ppi->output[PPI_PORT_C] &= (uint8_t)~bit;
      }
    }
    return;
  }
  /* Writing an input port loads the latch without reaching the pins, which
     is why a port turned back to output resumes what was written to it. */
  ppi->output[selection] = data;
}

uint8_t ppi_output_of(const ppi_t *ppi, ppi_selection selection) {
  switch (selection) {
    case PPI_PORT_A:
      return ppi->port_a_input ? 0xFF : ppi->output[PPI_PORT_A];
    case PPI_PORT_B:
      return ppi->port_b_input ? 0xFF : ppi->output[PPI_PORT_B];
    case PPI_PORT_C: {
      uint8_t lower = ppi->port_c_lower_input ? 0x0F : (ppi->output[PPI_PORT_C] & 0x0F);
      uint8_t upper = ppi->port_c_upper_input ? 0xF0 : (ppi->output[PPI_PORT_C] & 0xF0);
      return (uint8_t)(upper | lower);
    }
    default:
      return 0xFF;
  }
}

void ppi_present(ppi_t *ppi, ppi_selection selection, uint8_t levels) {
  if (selection != PPI_CONTROL) {
    ppi->input[selection] = levels;
  }
}
