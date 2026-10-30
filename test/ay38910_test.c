/*
 * ay38910_test — selecting, writing, reading, the chip select, and the port the
 * keyboard uses.
 */
#include "ay38910.h"
#include "test.h"

static ay38910_t psg;

static void select_register(uint8_t number) { ay38910_access(&psg, AY38910_SELECT, number); }

static void reset_state(void) {
  ay38910_init(&psg);
  TEST_EQUAL(psg.selected, 0);
  TEST_CHECK(!psg.chip_selected);
  TEST_EQUAL(psg.registers[0], 0);
  TEST_CHECK(ay38910_port_a_is_input(&psg));
}

/* Reset sets the flip-flop that leaves the chip unselected (deathsoft and
   lvd's model, PSG_SEL), so a write needs an address before it. */
static void a_reset_chip_takes_no_write_until_it_is_selected(void) {
  ay38910_init(&psg);
  ay38910_access(&psg, AY38910_WRITE, 0x55);
  TEST_EQUAL(psg.registers[0], 0);
  select_register(0);
  ay38910_access(&psg, AY38910_WRITE, 0x55);
  TEST_EQUAL(psg.registers[0], 0x55);
}

static void a_register_holds_what_it_is_given(void) {
  ay38910_init(&psg);
  select_register(8);
  ay38910_access(&psg, AY38910_WRITE, 0x0D);
  TEST_EQUAL(psg.registers[8], 0x0D);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x0D);
}

static void a_register_keeps_only_the_bits_it_has(void) {
  ay38910_init(&psg);
  select_register(1); /* the high half of a tone period: four bits */
  ay38910_access(&psg, AY38910_WRITE, 0xFF);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x0F);
  select_register(6); /* the noise period: five bits */
  ay38910_access(&psg, AY38910_WRITE, 0xFF);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x1F);
  select_register(0); /* the low half: all eight */
  ay38910_access(&psg, AY38910_WRITE, 0xFF);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0xFF);
}

/* "Unless otherwise specified, address bits DA7--DA4 are programmed to
   recognize only a 0000 code" (GI's data manual, §2.1.1). */
static void only_an_address_below_sixteen_selects_the_chip(void) {
  for (int address = 0; address < 256; address++) {
    ay38910_init(&psg);
    select_register(0);
    select_register((uint8_t)address);
    TEST_CHECK(psg.chip_selected == (address < 16));
  }
}

/* Kevin Thacker's ACID "invalid register write test", written for real
   CPCs and Spectrums (psg.asm, invalid_reg_write_test,
   https://github.com/floooh/emu-info/tree/master/tests/cpc/acid/psg): with
   every register filled from its reg_data, a write of &33 after each
   address from 16 to 255 changes none of them. The note "failed, got 1,
   expected 0 on cpc6128 with type 0" stands between this test and the
   next, which is disabled; both report in that form, and the file's notes
   mostly sit above the test they describe. */
static void no_address_above_fifteen_writes_a_register(void) {
  static const uint8_t reg_data[16] = {1, 2, 3, 4, 5, 6, 7, 0xC8, 9, 10, 11, 12, 13, 14, 15, 16};
  ay38910_init(&psg);
  for (int index = 0; index < 16; index++) {
    select_register((uint8_t)index);
    ay38910_access(&psg, AY38910_WRITE, reg_data[index]);
  }
  for (int address = 16; address < 256; address++) {
    select_register((uint8_t)address);
    ay38910_access(&psg, AY38910_WRITE, 0x33);
    for (int index = 0; index < 16; index++) {
      TEST_EQUAL(psg.registers[index], reg_data[index]);
    }
  }
}

/* "A valid high order address latches the register address" (§2.1.1), and
   one that is not valid latches nothing, so the number selected before
   stays; on the die it also takes PSG_SEL low (ay38910.h), so the write goes
   nowhere. */
static void an_address_above_fifteen_latches_nothing(void) {
  ay38910_init(&psg);
  select_register(2);
  ay38910_access(&psg, AY38910_WRITE, 0x11);
  select_register(0x13);
  TEST_EQUAL(psg.selected, 2);
  ay38910_access(&psg, AY38910_WRITE, 0x22);
  for (int index = 0; index < 16; index++) {
    TEST_EQUAL(psg.registers[index], index == 2 ? 0x11 : 0);
  }
}

/* An unselected chip's "bidirectional buffers are forced to a high
   impedance state" (§2.1.1). */
static void an_unselected_chip_leaves_the_bus_as_it_found_it(void) {
  ay38910_init(&psg);
  select_register(8);
  ay38910_access(&psg, AY38910_WRITE, 0x0D);
  select_register(0xF8);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0xA5), 0xA5);
}

static void an_address_that_matches_selects_the_chip_again(void) {
  ay38910_init(&psg);
  select_register(0x40);
  select_register(4);
  TEST_CHECK(psg.chip_selected);
  ay38910_access(&psg, AY38910_WRITE, 0x33);
  TEST_EQUAL(psg.registers[4], 0x33);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x33);
}

static void a_selection_lasts_until_the_next_one(void) {
  ay38910_init(&psg);
  select_register(4);
  ay38910_access(&psg, AY38910_WRITE, 0x11);
  ay38910_access(&psg, AY38910_WRITE, 0x22);
  TEST_EQUAL(psg.registers[4], 0x22);
  TEST_EQUAL(psg.registers[5], 0);
}

static void the_inactive_function_touches_nothing(void) {
  ay38910_init(&psg);
  select_register(2);
  ay38910_access(&psg, AY38910_INACTIVE, 0x99);
  TEST_EQUAL(psg.selected, 2);
  TEST_EQUAL(psg.registers[2], 0);
  /* Inactive leaves the bus as it found it. */
  TEST_EQUAL(ay38910_access(&psg, AY38910_INACTIVE, 0x99), 0x99);
}

static void port_a_reads_the_matrix_while_it_is_an_input(void) {
  ay38910_init(&psg);
  ay38910_present_port_a(&psg, 0xBE);
  select_register(AY38910_PORT_A);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0xBE);
}

static void port_a_reads_its_own_latch_once_it_is_an_output(void) {
  ay38910_init(&psg);
  ay38910_present_port_a(&psg, 0xBE);
  select_register(AY38910_MIXER);
  ay38910_access(&psg, AY38910_WRITE, 0x40); /* bit 6: port A becomes an output */
  TEST_CHECK(!ay38910_port_a_is_input(&psg));
  select_register(AY38910_PORT_A);
  ay38910_access(&psg, AY38910_WRITE, 0x17);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x17);
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(a_register_holds_what_it_is_given);
  TEST_RUN(a_register_keeps_only_the_bits_it_has);
  TEST_RUN(a_reset_chip_takes_no_write_until_it_is_selected);
  TEST_RUN(only_an_address_below_sixteen_selects_the_chip);
  TEST_RUN(no_address_above_fifteen_writes_a_register);
  TEST_RUN(an_address_above_fifteen_latches_nothing);
  TEST_RUN(an_unselected_chip_leaves_the_bus_as_it_found_it);
  TEST_RUN(an_address_that_matches_selects_the_chip_again);
  TEST_RUN(a_selection_lasts_until_the_next_one);
  TEST_RUN(the_inactive_function_touches_nothing);
  TEST_RUN(port_a_reads_the_matrix_while_it_is_an_input);
  TEST_RUN(port_a_reads_its_own_latch_once_it_is_an_output);
  return TEST_REPORT("ay38910");
}
