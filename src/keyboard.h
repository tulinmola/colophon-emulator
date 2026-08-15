/*
 * keyboard.h — the key matrix.
 *
 * Ten lines of eight switches. A machine selects one line and reads a byte
 * in which a zero means pressed: the switches pull their bit down, so an
 * untouched keyboard reads &FF, and so does a line that does not exist.
 *
 * Joystick 0 has line 9 to itself, all but its top bit, which is DEL.
 * Joystick 1 shares line 6 with the letters, which is why its directions
 * can be played from the keyboard and why two-player games pick their keys
 * carefully.
 *
 * Not modelled: keyboard clash. On real hardware three keys held at the
 * corners of a rectangle in the matrix conjure the fourth, because the
 * switches are a grid of wires with nothing to stop a current going the
 * long way round. The rule is exactly stated in the source below, and
 * nothing we can run yet would notice its absence.
 *
 * Sources:
 * - "Reading the keyboard and Joysticks" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/keyboard.html — the matrix table, the
 *   active-low sense, that lines past the tenth always read &FF, and the
 *   clash rule.
 */
#ifndef COLOPHON_KEYBOARD_H
#define COLOPHON_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#define KEYBOARD_LINES 10

/* A position in the matrix, as line * 8 + bit — the numbering the CPC's own
   documentation uses for its key codes. */
typedef uint8_t keyboard_key;

#define KEYBOARD_KEY(line, bit) ((keyboard_key)((line) * 8 + (bit)))
#define KEYBOARD_NO_KEY ((keyboard_key)0xFF)

/* The keys a text-typing caller needs by name; the rest it finds through
   keyboard_key_for_character. */
#define KEYBOARD_RETURN KEYBOARD_KEY(2, 2)
#define KEYBOARD_SHIFT KEYBOARD_KEY(2, 5)
#define KEYBOARD_CONTROL KEYBOARD_KEY(2, 7)
#define KEYBOARD_SPACE KEYBOARD_KEY(5, 7)
#define KEYBOARD_TAB KEYBOARD_KEY(8, 4)
#define KEYBOARD_ESCAPE KEYBOARD_KEY(8, 2)
#define KEYBOARD_DELETE KEYBOARD_KEY(9, 7)

typedef struct {
  /* One byte per line, a set bit meaning released. */
  uint8_t lines[KEYBOARD_LINES];
} keyboard_t;

void keyboard_init(keyboard_t *keyboard);

void keyboard_press(keyboard_t *keyboard, keyboard_key key);
void keyboard_release(keyboard_t *keyboard, keyboard_key key);
void keyboard_release_all(keyboard_t *keyboard);

/* The byte a selected line presents. Lines the machine does not have read
 * &FF, as they do on hardware. */
uint8_t keyboard_line(const keyboard_t *keyboard, uint8_t line);

/* Where a character lives on a UK CPC keyboard, and whether shift is held
 * to reach it. Returns KEYBOARD_NO_KEY for a character the keyboard cannot
 * produce. */
keyboard_key keyboard_key_for_character(char character, bool *shifted);

#endif
