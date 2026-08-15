/*
 * keyboard_test — the matrix, and the layout printed on the keycaps.
 */
#include "keyboard.h"
#include "test.h"

static keyboard_t keyboard;

static void an_untouched_keyboard_reads_high(void) {
  keyboard_init(&keyboard);
  for (uint8_t line = 0; line < KEYBOARD_LINES; line++) {
    TEST_EQUAL(keyboard_line(&keyboard, line), 0xFF);
  }
}

static void a_pressed_key_pulls_its_bit_down(void) {
  keyboard_init(&keyboard);
  keyboard_press(&keyboard, KEYBOARD_RETURN); /* line 2, bit 2 */
  TEST_EQUAL(keyboard_line(&keyboard, 2), 0xFB);
  TEST_EQUAL(keyboard_line(&keyboard, 1), 0xFF);
  keyboard_release(&keyboard, KEYBOARD_RETURN);
  TEST_EQUAL(keyboard_line(&keyboard, 2), 0xFF);
}

static void several_keys_share_a_line(void) {
  keyboard_init(&keyboard);
  keyboard_press(&keyboard, KEYBOARD_SHIFT);   /* line 2, bit 5 */
  keyboard_press(&keyboard, KEYBOARD_CONTROL); /* line 2, bit 7 */
  TEST_EQUAL(keyboard_line(&keyboard, 2), 0x5F);
  keyboard_release_all(&keyboard);
  TEST_EQUAL(keyboard_line(&keyboard, 2), 0xFF);
}

static void a_line_the_machine_lacks_reads_high(void) {
  keyboard_init(&keyboard);
  keyboard_press(&keyboard, KEYBOARD_KEY(0, 0));
  for (uint8_t line = KEYBOARD_LINES; line < 16; line++) {
    TEST_EQUAL(keyboard_line(&keyboard, line), 0xFF);
  }
}

/* Spot checks against the matrix table, one per corner and a few in the
   middle, so a transcription slip shows up here rather than as a wrong
   letter three rungs later. */
static void the_matrix_is_where_the_table_says(void) {
  bool shifted = false;
  TEST_EQUAL(keyboard_key_for_character('1', &shifted), KEYBOARD_KEY(8, 0));
  TEST_CHECK(!shifted);
  TEST_EQUAL(keyboard_key_for_character('!', &shifted), KEYBOARD_KEY(8, 0));
  TEST_CHECK(shifted);
  TEST_EQUAL(keyboard_key_for_character('"', &shifted), KEYBOARD_KEY(8, 1));
  TEST_CHECK(shifted);
  TEST_EQUAL(keyboard_key_for_character('#', &shifted), KEYBOARD_KEY(7, 1));
  TEST_CHECK(shifted);
  TEST_EQUAL(keyboard_key_for_character('p', &shifted), KEYBOARD_KEY(3, 3));
  TEST_EQUAL(keyboard_key_for_character('P', &shifted), KEYBOARD_KEY(3, 3));
  TEST_CHECK(shifted);
  TEST_EQUAL(keyboard_key_for_character('z', &shifted), KEYBOARD_KEY(8, 7));
  TEST_EQUAL(keyboard_key_for_character(' ', &shifted), KEYBOARD_SPACE);
  TEST_CHECK(!shifted);
  TEST_EQUAL(keyboard_key_for_character('+', &shifted), KEYBOARD_KEY(3, 4));
  TEST_CHECK(shifted);
  TEST_EQUAL(keyboard_key_for_character('[', &shifted), KEYBOARD_KEY(2, 1));
  TEST_EQUAL(keyboard_key_for_character(']', &shifted), KEYBOARD_KEY(2, 3));
  TEST_EQUAL(keyboard_key_for_character('@', &shifted), KEYBOARD_KEY(3, 2));
  TEST_CHECK(!shifted);
}

static void a_character_the_keyboard_lacks_is_refused(void) {
  bool shifted = false;
  TEST_EQUAL(keyboard_key_for_character('~', &shifted), KEYBOARD_NO_KEY);
  TEST_EQUAL(keyboard_key_for_character('\n', &shifted), KEYBOARD_NO_KEY);
  TEST_EQUAL(keyboard_key_for_character('\0', &shifted), KEYBOARD_NO_KEY);
}

/* Every letter and digit must be reachable, and no two keys may claim the
   same character — the sort of thing a hand-copied table gets wrong once. */
static void the_layout_is_complete_and_unambiguous(void) {
  static const char *reachable = "abcdefghijklmnopqrstuvwxyz0123456789 "
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "!\"#$%&'()_-=[]{}@|;+:*/?,<.>^\\`";
  for (const char *character = reachable; *character != '\0'; character++) {
    bool shifted = false;
    if (keyboard_key_for_character(*character, &shifted) == KEYBOARD_NO_KEY) {
      TEST_FAIL("'%c' is on no key", *character);
    }
  }
  for (int first = 32; first < 127; first++) {
    bool shifted = false;
    keyboard_key key = keyboard_key_for_character((char)first, &shifted);
    if (key == KEYBOARD_NO_KEY) {
      continue;
    }
    for (int second = first + 1; second < 127; second++) {
      bool other_shifted = false;
      keyboard_key other = keyboard_key_for_character((char)second, &other_shifted);
      if (key == other && shifted == other_shifted && first != ' ') {
        TEST_FAIL("'%c' and '%c' claim the same key", first, second);
      }
    }
  }
}

int main(void) {
  TEST_RUN(an_untouched_keyboard_reads_high);
  TEST_RUN(a_pressed_key_pulls_its_bit_down);
  TEST_RUN(several_keys_share_a_line);
  TEST_RUN(a_line_the_machine_lacks_reads_high);
  TEST_RUN(the_matrix_is_where_the_table_says);
  TEST_RUN(a_character_the_keyboard_lacks_is_refused);
  TEST_RUN(the_layout_is_complete_and_unambiguous);
  return TEST_REPORT("keyboard");
}
