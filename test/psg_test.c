/*
 * psg_test — selecting, writing, reading, and the port the keyboard uses.
 */
#include "psg.h"
#include "test.h"

static psg_t psg;

static void select_register(uint8_t number) { psg_access(&psg, PSG_SELECT, number); }

static void reset_state(void) {
  psg_init(&psg);
  TEST_EQUAL(psg.selected, 0);
  TEST_EQUAL(psg.registers[0], 0);
  TEST_CHECK(psg_port_a_is_input(&psg));
}

static void a_register_holds_what_it_is_given(void) {
  psg_init(&psg);
  select_register(8);
  psg_access(&psg, PSG_WRITE, 0x0D);
  TEST_EQUAL(psg.registers[8], 0x0D);
  TEST_EQUAL(psg_access(&psg, PSG_READ, 0), 0x0D);
}

static void a_register_keeps_only_the_bits_it_has(void) {
  psg_init(&psg);
  select_register(1); /* the high half of a tone period: four bits */
  psg_access(&psg, PSG_WRITE, 0xFF);
  TEST_EQUAL(psg_access(&psg, PSG_READ, 0), 0x0F);
  select_register(6); /* the noise period: five bits */
  psg_access(&psg, PSG_WRITE, 0xFF);
  TEST_EQUAL(psg_access(&psg, PSG_READ, 0), 0x1F);
  select_register(0); /* the low half: all eight */
  psg_access(&psg, PSG_WRITE, 0xFF);
  TEST_EQUAL(psg_access(&psg, PSG_READ, 0), 0xFF);
}

static void the_register_number_wears_four_bits(void) {
  psg_init(&psg);
  select_register(0xF3);
  TEST_EQUAL(psg.selected, 3);
}

static void a_selection_lasts_until_the_next_one(void) {
  psg_init(&psg);
  select_register(4);
  psg_access(&psg, PSG_WRITE, 0x11);
  psg_access(&psg, PSG_WRITE, 0x22);
  TEST_EQUAL(psg.registers[4], 0x22);
  TEST_EQUAL(psg.registers[5], 0);
}

static void the_inactive_function_touches_nothing(void) {
  psg_init(&psg);
  select_register(2);
  psg_access(&psg, PSG_INACTIVE, 0x99);
  TEST_EQUAL(psg.selected, 2);
  TEST_EQUAL(psg.registers[2], 0);
  /* Inactive leaves the bus as it found it. */
  TEST_EQUAL(psg_access(&psg, PSG_INACTIVE, 0x99), 0x99);
}

static void port_a_reads_the_matrix_while_it_is_an_input(void) {
  psg_init(&psg);
  psg_present_port_a(&psg, 0xBE);
  select_register(PSG_PORT_A);
  TEST_EQUAL(psg_access(&psg, PSG_READ, 0), 0xBE);
}

static void port_a_reads_its_own_latch_once_it_is_an_output(void) {
  psg_init(&psg);
  psg_present_port_a(&psg, 0xBE);
  select_register(PSG_MIXER);
  psg_access(&psg, PSG_WRITE, 0x40); /* bit 6: port A becomes an output */
  TEST_CHECK(!psg_port_a_is_input(&psg));
  select_register(PSG_PORT_A);
  psg_access(&psg, PSG_WRITE, 0x17);
  TEST_EQUAL(psg_access(&psg, PSG_READ, 0), 0x17);
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(a_register_holds_what_it_is_given);
  TEST_RUN(a_register_keeps_only_the_bits_it_has);
  TEST_RUN(the_register_number_wears_four_bits);
  TEST_RUN(a_selection_lasts_until_the_next_one);
  TEST_RUN(the_inactive_function_touches_nothing);
  TEST_RUN(port_a_reads_the_matrix_while_it_is_an_input);
  TEST_RUN(port_a_reads_its_own_latch_once_it_is_an_output);
  return TEST_REPORT("psg");
}
