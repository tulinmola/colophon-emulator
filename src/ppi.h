/*
 * ppi.h — the Intel 8255 programmable peripheral interface.
 *
 * Three eight-bit ports and a control register. Ports A and B are whole;
 * port C splits into two nibbles that take their direction separately, and
 * its bits can be set and cleared one at a time through the control
 * register. Only mode 0, plain input and output, is implemented: it is the
 * only mode the machines that use this chip ever select.
 *
 * A port set to input presents &FF to whatever is wired to it, because its
 * outputs go to high impedance and the pull-ups win. That is not a detail:
 * it is what a device on the other side reads while the CPU is reading, and
 * getting it wrong is invisible until something depends on it.
 *
 * The chip knows nothing of keyboards or sound. What the ports are wired to
 * is the machine's business: it presents input levels before a read and
 * collects output levels after a write.
 *
 * Sources:
 * - "8255 PPI" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/8255cpc.html — the port functions, the
 *   high-impedance rule above, and the observation that modes 1 and 2 go
 *   unused by any program.
 * - Intel 8255A datasheet, as reproduced at
 *   https://cpctech.cpcwiki.de/docs/datasheet/msm82c55a.pdf — the control
 *   word's layout and the bit set/reset command.
 */
#ifndef COLOPHON_PPI_H
#define COLOPHON_PPI_H

#include <stdbool.h>
#include <stdint.h>

/* The four things the chip's two address lines select. */
typedef enum {
  PPI_PORT_A = 0,
  PPI_PORT_B = 1,
  PPI_PORT_C = 2,
  PPI_CONTROL = 3,
} ppi_selection;

typedef struct {
  /* What the CPU last wrote. A port reads its latch back when it is an
     output; an input port reads the levels the machine presents. */
  uint8_t output[3];
  /* The levels wired to each port from outside, as the machine last set
     them. */
  uint8_t input[3];
  uint8_t control;
  /* Direction per port, port C by nibble. True is input, following the
     control word's own sense. */
  bool port_a_input;
  bool port_b_input;
  bool port_c_lower_input;
  bool port_c_upper_input;
} ppi_t;

/* Power-on. The control word is zero, which by the datasheet's own encoding
 * would be mode 0 with every port an output; real silicon resets all ports
 * to input, and does so with the output latches cleared. */
void ppi_init(ppi_t *ppi);

/* The CPU reads a port. Control is write-only and reads as the last control
 * word, which is what the chip's bus leaves behind. */
uint8_t ppi_read(const ppi_t *ppi, ppi_selection selection);

/* The CPU writes a port or the control register. A control word with bit 7
 * set configures the ports and clears the output latches; without it, the
 * word sets or clears a single bit of port C. */
void ppi_write(ppi_t *ppi, ppi_selection selection, uint8_t data);

/* What a device wired to a port sees: the latch when the port is an output,
 * &FF when it is an input and the pins have gone high. */
uint8_t ppi_output_of(const ppi_t *ppi, ppi_selection selection);

/* The machine presenting levels to an input port. */
void ppi_present(ppi_t *ppi, ppi_selection selection, uint8_t levels);

#endif
