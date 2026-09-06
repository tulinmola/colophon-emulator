/*
 * keyboard_test — the matrix alone.
 *
 * Nothing here knows what is printed on a key or how many lines a machine
 * fits: those belong to the machines, and each grades its own.
 */
#include <string.h>

#include "keyboard.h"
#include "test.h"

/* The keyboard with ground either side of it, so a key the matrix has no
   room for is caught writing where it should not rather than passing
   unnoticed into whatever lies next in memory. */
static struct {
  uint8_t before[32];
  keyboard_t keyboard;
  uint8_t after[32];
} matrix;

static void an_untouched_keyboard_reads_high(void) {
  keyboard_init(&matrix.keyboard);
  for (uint8_t line = 0; line < KEYBOARD_MAX_LINES; line++) {
    TEST_EQUAL(keyboard_line(&matrix.keyboard, line), 0xFF);
  }
}

static void a_pressed_key_pulls_its_bit_down(void) {
  keyboard_init(&matrix.keyboard);
  keyboard_press(&matrix.keyboard, KEYBOARD_KEY(2, 2));
  TEST_EQUAL(keyboard_line(&matrix.keyboard, 2), 0xFB);
  TEST_EQUAL(keyboard_line(&matrix.keyboard, 1), 0xFF);
  keyboard_release(&matrix.keyboard, KEYBOARD_KEY(2, 2));
  TEST_EQUAL(keyboard_line(&matrix.keyboard, 2), 0xFF);
}

static void several_keys_share_a_line(void) {
  keyboard_init(&matrix.keyboard);
  keyboard_press(&matrix.keyboard, KEYBOARD_KEY(2, 5));
  keyboard_press(&matrix.keyboard, KEYBOARD_KEY(2, 7));
  TEST_EQUAL(keyboard_line(&matrix.keyboard, 2), 0x5F);
  keyboard_release_all(&matrix.keyboard);
  TEST_EQUAL(keyboard_line(&matrix.keyboard, 2), 0xFF);
}

static void a_line_past_the_matrix_reads_high(void) {
  keyboard_init(&matrix.keyboard);
  keyboard_press(&matrix.keyboard, KEYBOARD_KEY(0, 0));
  for (uint8_t line = KEYBOARD_MAX_LINES; line < 16; line++) {
    TEST_EQUAL(keyboard_line(&matrix.keyboard, line), 0xFF);
  }
}

/* A key past the last line has nowhere to go. Pressing clears bits and
   releasing sets them, so the ground is laid twice: once all ones for a
   press to pull down, once all zeroes for a release to push up. */
static void a_key_past_the_matrix_is_dropped(void) {
  const keyboard_key past_the_last = KEYBOARD_KEY(KEYBOARD_MAX_LINES, 0);
  keyboard_init(&matrix.keyboard);

  memset(matrix.before, 0xFF, sizeof matrix.before);
  memset(matrix.after, 0xFF, sizeof matrix.after);
  keyboard_press(&matrix.keyboard, past_the_last);
  keyboard_press(&matrix.keyboard, KEYBOARD_NO_KEY);
  for (size_t index = 0; index < sizeof matrix.before; index++) {
    TEST_EQUAL(matrix.before[index], 0xFF);
    TEST_EQUAL(matrix.after[index], 0xFF);
  }

  memset(matrix.before, 0x00, sizeof matrix.before);
  memset(matrix.after, 0x00, sizeof matrix.after);
  keyboard_release(&matrix.keyboard, past_the_last);
  keyboard_release(&matrix.keyboard, KEYBOARD_NO_KEY);
  for (size_t index = 0; index < sizeof matrix.before; index++) {
    TEST_EQUAL(matrix.before[index], 0x00);
    TEST_EQUAL(matrix.after[index], 0x00);
  }

  for (uint8_t line = 0; line < KEYBOARD_MAX_LINES; line++) {
    TEST_EQUAL(keyboard_line(&matrix.keyboard, line), 0xFF);
  }
}

int main(void) {
  TEST_RUN(an_untouched_keyboard_reads_high);
  TEST_RUN(a_pressed_key_pulls_its_bit_down);
  TEST_RUN(several_keys_share_a_line);
  TEST_RUN(a_line_past_the_matrix_reads_high);
  TEST_RUN(a_key_past_the_matrix_is_dropped);
  return TEST_REPORT("keyboard");
}
