/*
 * keyboard.c — the matrix.
 */
#include "keyboard.h"

void keyboard_init(keyboard_t *keyboard) { keyboard_release_all(keyboard); }

void keyboard_press(keyboard_t *keyboard, keyboard_key key) {
  if (key / 8 < KEYBOARD_MAX_LINES) {
    keyboard->lines[key / 8] &= (uint8_t)~(1u << (key % 8));
  }
}

void keyboard_release(keyboard_t *keyboard, keyboard_key key) {
  if (key / 8 < KEYBOARD_MAX_LINES) {
    keyboard->lines[key / 8] |= (uint8_t)(1u << (key % 8));
  }
}

void keyboard_release_all(keyboard_t *keyboard) {
  for (int line = 0; line < KEYBOARD_MAX_LINES; line++) {
    keyboard->lines[line] = 0xFF;
  }
}

uint8_t keyboard_line(const keyboard_t *keyboard, uint8_t line) {
  return line < KEYBOARD_MAX_LINES ? keyboard->lines[line] : 0xFF;
}
