/*
 * keyboard.h — the key matrix.
 *
 * Lines of eight switches. A machine selects one line and reads a byte in
 * which a zero means pressed: the switches pull their bit down, so an
 * untouched keyboard reads &FF, and so does a line that does not exist.
 *
 * What is wired to each line, what is printed on each key, and how many
 * lines a machine fits belong to that machine and are declared with it.
 * This file reserves the room and holds none of the rest: it is the grid of
 * switches and nothing else.
 *
 * Not modelled: keyboard clash. On real hardware three keys held at the
 * corners of a rectangle in the matrix conjure the fourth, because the
 * switches are a grid of wires with nothing to stop a current going the
 * long way round. The rule is exactly stated in the source below, and
 * nothing we can run yet would notice its absence.
 *
 * Sources:
 * - "Reading the keyboard and Joysticks" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/keyboard.html — the active-low sense,
 *   that a line that does not exist reads &FF, and the clash rule.
 */
#ifndef COLOPHON_KEYBOARD_H
#define COLOPHON_KEYBOARD_H

#include <stdint.h>

/* The widest matrix fitted, which is the room a keyboard reserves: ten lines
   on a CPC, eight on a Spectrum. How many a machine actually has is declared
   with that machine. */
#define KEYBOARD_MAX_LINES 10

/* A position in the matrix, as line * 8 + bit. */
typedef uint8_t keyboard_key;

#define KEYBOARD_KEY(line, bit) ((keyboard_key)((line) * 8 + (bit)))
#define KEYBOARD_NO_KEY ((keyboard_key)0xFF)

typedef struct {
  /* One byte per line, a set bit meaning released. */
  uint8_t lines[KEYBOARD_MAX_LINES];
} keyboard_t;

void keyboard_init(keyboard_t *keyboard);

void keyboard_press(keyboard_t *keyboard, keyboard_key key);
void keyboard_release(keyboard_t *keyboard, keyboard_key key);
void keyboard_release_all(keyboard_t *keyboard);

/* The byte a selected line presents. Past the widest matrix a line reads
 * &FF, as an unwired one does on hardware. A machine that fits fewer lines
 * than the matrix reserves reads only as far as its own. */
uint8_t keyboard_line(const keyboard_t *keyboard, uint8_t line);

#endif
