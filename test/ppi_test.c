/*
 * ppi_test — directions, latches, and what the other side sees.
 */
#include "ppi.h"
#include "test.h"

static ppi_t ppi;

/* Mode 0 throughout: port A output, port B input, port C output — the
   configuration the CPC's firmware keeps, and the one its own documented
   routines restore. */
#define CONTROL_PORT_A_OUTPUT 0x82
#define CONTROL_PORT_A_INPUT 0x92

static void reset_leaves_every_port_an_input(void) {
  ppi_init(&ppi);
  TEST_CHECK(ppi.port_a_input);
  TEST_CHECK(ppi.port_b_input);
  TEST_CHECK(ppi.port_c_lower_input);
  TEST_CHECK(ppi.port_c_upper_input);
  TEST_EQUAL(ppi_output_of(&ppi, PPI_PORT_A), 0xFF);
}

static void an_output_port_reads_its_own_latch(void) {
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  ppi_present(&ppi, PPI_PORT_A, 0x11); /* whatever is wired up is ignored */
  ppi_write(&ppi, PPI_PORT_A, 0x5A);
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_A), 0x5A);
  TEST_EQUAL(ppi_output_of(&ppi, PPI_PORT_A), 0x5A);
}

static void an_input_port_reads_what_is_wired_to_it(void) {
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_INPUT);
  ppi_present(&ppi, PPI_PORT_A, 0x3C);
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_A), 0x3C);
}

static void an_input_port_presents_high_to_the_device(void) {
  /* The rule that decides whether a PSG sees stale data or an idle bus. */
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  ppi_write(&ppi, PPI_PORT_A, 0x0E);
  TEST_EQUAL(ppi_output_of(&ppi, PPI_PORT_A), 0x0E);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_INPUT);
  TEST_EQUAL(ppi_output_of(&ppi, PPI_PORT_A), 0xFF);
}

static void configuring_the_chip_clears_the_latches(void) {
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  ppi_write(&ppi, PPI_PORT_A, 0x77);
  ppi_write(&ppi, PPI_PORT_C, 0x77);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_A), 0);
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_C), 0);
}

static void port_c_takes_its_two_halves_apart(void) {
  /* Upper nibble input, lower output: %10001000. */
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, 0x88);
  ppi_write(&ppi, PPI_PORT_C, 0xAB);
  ppi_present(&ppi, PPI_PORT_C, 0x5F);
  TEST_CHECK(ppi.port_c_upper_input);
  TEST_CHECK(!ppi.port_c_lower_input);
  /* The upper nibble comes from the pins, the lower from the latch. */
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_C), 0x5B);
  TEST_EQUAL(ppi_output_of(&ppi, PPI_PORT_C), 0xFB);
}

static void a_single_bit_of_port_c_can_be_set_and_cleared(void) {
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  ppi_write(&ppi, PPI_CONTROL, 0x0B); /* bit 5 set: %0000 101 1 */
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_C), 0x20);
  ppi_write(&ppi, PPI_CONTROL, 0x0F); /* bit 7 set */
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_C), 0xA0);
  ppi_write(&ppi, PPI_CONTROL, 0x0A); /* bit 5 cleared */
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_C), 0x80);
}

static void a_latch_survives_a_turn_around_the_houses(void) {
  /* Writing a port while it is an input loads the latch without reaching
     the pins, so turning it back to output resumes what was written. */
  ppi_init(&ppi);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  ppi_write(&ppi, PPI_PORT_A, 0x0E);
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_INPUT);
  ppi_present(&ppi, PPI_PORT_A, 0x3C);
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_A), 0x3C);
  /* Reconfiguring clears the latches, so this is the documented cost of
     the round trip rather than a memory of &0E. */
  ppi_write(&ppi, PPI_CONTROL, CONTROL_PORT_A_OUTPUT);
  TEST_EQUAL(ppi_read(&ppi, PPI_PORT_A), 0);
}

int main(void) {
  TEST_RUN(reset_leaves_every_port_an_input);
  TEST_RUN(an_output_port_reads_its_own_latch);
  TEST_RUN(an_input_port_reads_what_is_wired_to_it);
  TEST_RUN(an_input_port_presents_high_to_the_device);
  TEST_RUN(configuring_the_chip_clears_the_latches);
  TEST_RUN(port_c_takes_its_two_halves_apart);
  TEST_RUN(a_single_bit_of_port_c_can_be_set_and_cleared);
  TEST_RUN(a_latch_survives_a_turn_around_the_houses);
  return TEST_REPORT("ppi");
}
