/*
 * keyboard.c — the matrix, and the way letters reach it.
 */
#include <stddef.h>

#include "keyboard.h"

void keyboard_init(keyboard_t *keyboard) { keyboard_release_all(keyboard); }

void keyboard_press(keyboard_t *keyboard, keyboard_key key) {
  if (key / 8 < KEYBOARD_LINES) {
    keyboard->lines[key / 8] &= (uint8_t)~(1u << (key % 8));
  }
}

void keyboard_release(keyboard_t *keyboard, keyboard_key key) {
  if (key / 8 < KEYBOARD_LINES) {
    keyboard->lines[key / 8] |= (uint8_t)(1u << (key % 8));
  }
}

void keyboard_release_all(keyboard_t *keyboard) {
  for (int line = 0; line < KEYBOARD_LINES; line++) {
    keyboard->lines[line] = 0xFF;
  }
}

uint8_t keyboard_line(const keyboard_t *keyboard, uint8_t line) {
  return line < KEYBOARD_LINES ? keyboard->lines[line] : 0xFF;
}

/* The UK CPC keyboard, by matrix position: the character a key gives alone,
   and the one it gives with shift. A key that repeats a character in both
   columns has no shifted legend of its own.

   Positions run line by line from the table in "Reading the keyboard and
   Joysticks"; the keys with no character — cursors, function keys, Copy,
   Caps Lock and the joystick lines — are absent, and a character not found
   here is one this keyboard cannot type. */
typedef struct {
  keyboard_key key;
  char plain;
  char shifted;
} legend;

static const legend legends[] = {
    {KEYBOARD_KEY(2, 1), '[', '{'},
    {KEYBOARD_KEY(2, 3), ']', '}'},
    {KEYBOARD_KEY(2, 6), '\\', '`'},
    {KEYBOARD_KEY(3, 0), '^', '\0'}, /* shift gives the pound sign, not ASCII */
    {KEYBOARD_KEY(3, 1), '-', '='},
    {KEYBOARD_KEY(3, 2), '@', '|'},
    {KEYBOARD_KEY(3, 3), 'p', 'P'},
    {KEYBOARD_KEY(3, 4), ';', '+'},
    {KEYBOARD_KEY(3, 5), ':', '*'},
    {KEYBOARD_KEY(3, 6), '/', '?'},
    {KEYBOARD_KEY(3, 7), ',', '<'},
    {KEYBOARD_KEY(4, 0), '0', '_'},
    {KEYBOARD_KEY(4, 1), '9', ')'},
    {KEYBOARD_KEY(4, 2), 'o', 'O'},
    {KEYBOARD_KEY(4, 3), 'i', 'I'},
    {KEYBOARD_KEY(4, 4), 'l', 'L'},
    {KEYBOARD_KEY(4, 5), 'k', 'K'},
    {KEYBOARD_KEY(4, 6), 'm', 'M'},
    {KEYBOARD_KEY(4, 7), '.', '>'},
    {KEYBOARD_KEY(5, 0), '8', '('},
    {KEYBOARD_KEY(5, 1), '7', '\''},
    {KEYBOARD_KEY(5, 2), 'u', 'U'},
    {KEYBOARD_KEY(5, 3), 'y', 'Y'},
    {KEYBOARD_KEY(5, 4), 'h', 'H'},
    {KEYBOARD_KEY(5, 5), 'j', 'J'},
    {KEYBOARD_KEY(5, 6), 'n', 'N'},
    {KEYBOARD_KEY(5, 7), ' ', ' '},
    {KEYBOARD_KEY(6, 0), '6', '&'},
    {KEYBOARD_KEY(6, 1), '5', '%'},
    {KEYBOARD_KEY(6, 2), 'r', 'R'},
    {KEYBOARD_KEY(6, 3), 't', 'T'},
    {KEYBOARD_KEY(6, 4), 'g', 'G'},
    {KEYBOARD_KEY(6, 5), 'f', 'F'},
    {KEYBOARD_KEY(6, 6), 'b', 'B'},
    {KEYBOARD_KEY(6, 7), 'v', 'V'},
    {KEYBOARD_KEY(7, 0), '4', '$'},
    {KEYBOARD_KEY(7, 1), '3', '#'},
    {KEYBOARD_KEY(7, 2), 'e', 'E'},
    {KEYBOARD_KEY(7, 3), 'w', 'W'},
    {KEYBOARD_KEY(7, 4), 's', 'S'},
    {KEYBOARD_KEY(7, 5), 'd', 'D'},
    {KEYBOARD_KEY(7, 6), 'c', 'C'},
    {KEYBOARD_KEY(7, 7), 'x', 'X'},
    {KEYBOARD_KEY(8, 0), '1', '!'},
    {KEYBOARD_KEY(8, 1), '2', '"'},
    {KEYBOARD_KEY(8, 3), 'q', 'Q'},
    {KEYBOARD_KEY(8, 5), 'a', 'A'},
    {KEYBOARD_KEY(8, 7), 'z', 'Z'},
};

keyboard_key keyboard_key_for_character(char character, bool *shifted) {
  for (size_t index = 0; index < sizeof legends / sizeof legends[0]; index++) {
    if (legends[index].plain == character) {
      *shifted = false;
      return legends[index].key;
    }
  }
  for (size_t index = 0; index < sizeof legends / sizeof legends[0]; index++) {
    if (legends[index].shifted == character && character != '\0') {
      *shifted = true;
      return legends[index].key;
    }
  }
  return KEYBOARD_NO_KEY;
}
