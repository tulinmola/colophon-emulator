/*
 * spectrum_firmware_test — boot the real firmware and read the screen back.
 *
 * The acceptance tier for the machine. Everything here is judged by Sinclair
 * rather than by us: the boot message is theirs, the arithmetic is BASIC's,
 * and the letters are identified by looking each glyph up in the character
 * table the ROM itself carries at &3D00 — eight bytes per code from 32 up,
 * which is how the firmware draws them in the first place. A test that
 * recognised letters by our own table would only prove we agree with
 * ourselves.
 *
 * The screen is read twice by two routes: out of the display file, and out
 * of the framebuffer the monitor painted. Only the second goes through the
 * serialiser, the sync separator and the beam, so agreeing means the whole
 * video path holds and not merely the memory under it.
 *
 * Needs the firmware image: run `make roms`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "test.h"

#define FONT_IN_ROM 0x3D00
#define FIRST_CODE 32

/* The display's top-left sample in the framebuffer: a retrace and a left
   border across, and as far down as the frame sync left the picture. */
#define DISPLAY_LEFT ((ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS) * ULA_SAMPLES_PER_TICK)
#define DISPLAY_TOP (ULA_FIRST_DISPLAY_LINE - SPECTRUM_PICTURE_SHIFT)

/* The boot message is on screen well before this; the ROM spends most of it
   testing the RAM it has just found. */
#define FRAMES_TO_PROMPT 100
/* The firmware scans the keyboard off the 50Hz interrupt and debounces, so
   a key must be held for several scans and released for several more. */
#define FRAMES_PER_KEY 6

static const char *rom_directory = "roms";
static uint8_t rom[SPECTRUM_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_48K];
static uint8_t framebuffer[SPECTRUM_FRAMEBUFFER_WIDTH * SPECTRUM_FRAMEBUFFER_HEIGHT];
static spectrum_t spectrum;

static bool load_rom(void) {
  char path[512];
  snprintf(path, sizeof path, "%s/spectrum48.rom", rom_directory);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    TEST_FAIL("cannot open %s — run 'make roms'", path);
    return false;
  }
  size_t read = fread(rom, 1, sizeof rom, file);
  fclose(file);
  if (read != sizeof rom) {
    TEST_FAIL("%s is %zu bytes, expected %zu", path, read, sizeof rom);
    return false;
  }
  return true;
}

static bool power_on(void) {
  if (!load_rom()) {
    return false;
  }
  memset(ram, 0x00, sizeof ram);
  memset(framebuffer, 0x00, sizeof framebuffer);
  spectrum_init(&spectrum, ram, sizeof ram, rom);
  spectrum_connect_monitor(&spectrum, framebuffer);
  return true;
}

static void run_frames(int frames) {
  for (int frame = 0; frame < frames; frame++) {
    for (int tick = 0; tick < SPECTRUM_TICKS_PER_FRAME; tick++) {
      spectrum_tick(&spectrum);
    }
  }
}

static void type_key(keyboard_key key, keyboard_key held_with) {
  if (held_with != KEYBOARD_NO_KEY) {
    keyboard_press(&spectrum.keyboard, held_with);
  }
  keyboard_press(&spectrum.keyboard, key);
  run_frames(FRAMES_PER_KEY);
  keyboard_release_all(&spectrum.keyboard);
  run_frames(FRAMES_PER_KEY);
}

/* The glyph in the ROM's own table, or '?' for a shape it does not draw. */
static char character_for(const uint8_t glyph[8]) {
  for (int code = FIRST_CODE; code < 128; code++) {
    if (memcmp(rom + FONT_IN_ROM + (size_t)(code - FIRST_CODE) * 8, glyph, 8) == 0) {
      return (char)code;
    }
  }
  return '?';
}

static void screen_from_memory(char text[ULA_ROWS][ULA_COLUMNS + 1]) {
  for (int row = 0; row < ULA_ROWS; row++) {
    for (int column = 0; column < ULA_COLUMNS; column++) {
      uint8_t glyph[8];
      for (int line = 0; line < 8; line++) {
        glyph[line] = ram[ula_display_address(row * 8 + line, column) - SPECTRUM_RAM_BASE];
      }
      text[row][column] = character_for(glyph);
    }
    text[row][ULA_COLUMNS] = '\0';
  }
}

/* The same reading, taken off the framebuffer. Whichever colour fills most
   of a cell is its paper; the rest is ink. */
static void screen_from_framebuffer(char text[ULA_ROWS][ULA_COLUMNS + 1]) {
  for (int row = 0; row < ULA_ROWS; row++) {
    for (int column = 0; column < ULA_COLUMNS; column++) {
      int counts[16] = {0};
      for (int line = 0; line < 8; line++) {
        for (int pixel = 0; pixel < 8; pixel++) {
          int y = DISPLAY_TOP + row * 8 + line;
          int x = DISPLAY_LEFT + column * 8 + pixel;
          counts[framebuffer[y * SPECTRUM_FRAMEBUFFER_WIDTH + x] & 0x0F]++;
        }
      }
      int paper = 0;
      for (int colour = 1; colour < 16; colour++) {
        if (counts[colour] > counts[paper]) {
          paper = colour;
        }
      }
      uint8_t glyph[8];
      for (int line = 0; line < 8; line++) {
        uint8_t bits = 0;
        for (int pixel = 0; pixel < 8; pixel++) {
          int y = DISPLAY_TOP + row * 8 + line;
          int x = DISPLAY_LEFT + column * 8 + pixel;
          if ((framebuffer[y * SPECTRUM_FRAMEBUFFER_WIDTH + x] & 0x0F) != paper) {
            bits |= (uint8_t)(0x80 >> pixel);
          }
        }
        glyph[line] = bits;
      }
      text[row][column] = character_for(glyph);
    }
    text[row][ULA_COLUMNS] = '\0';
  }
}

/* The message is Sinclair's, and its first character is the copyright sign,
   which the ROM's table draws at code 127. */
static void the_48k_boots_to_its_copyright_message(void) {
  if (!power_on()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  char expected[ULA_COLUMNS + 1];
  snprintf(expected, sizeof expected, "%c 1982 Sinclair Research Ltd", 127);
  if (strncmp(text[ULA_ROWS - 1], expected, strlen(expected)) != 0) {
    TEST_FAIL("bottom line reads |%s|", text[ULA_ROWS - 1]);
  }
}

static void the_screen_reads_the_same_through_the_beam(void) {
  if (!power_on()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  char from_memory[ULA_ROWS][ULA_COLUMNS + 1];
  char from_beam[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(from_memory);
  screen_from_framebuffer(from_beam);
  /* Read off the beam alone first, so that two blank screens agreeing
     cannot pass for a working video path. */
  char expected[ULA_COLUMNS + 1];
  snprintf(expected, sizeof expected, "%c 1982 Sinclair Research Ltd", 127);
  if (strncmp(from_beam[ULA_ROWS - 1], expected, strlen(expected)) != 0) {
    TEST_FAIL("the beam's bottom line reads |%s|", from_beam[ULA_ROWS - 1]);
  }
  for (int row = 0; row < ULA_ROWS; row++) {
    if (strcmp(from_memory[row], from_beam[row]) != 0) {
      TEST_FAIL("row %d: display file |%s| but beam |%s|", row, from_memory[row], from_beam[row]);
      return;
    }
  }
}

static void basic_does_arithmetic_it_is_typed(void) {
  if (!power_on()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  /* At the K cursor, P is the PRINT keyword; + is symbol shift with K. */
  type_key(SPECTRUM_KEY(5, 0), KEYBOARD_NO_KEY);       /* P */
  type_key(SPECTRUM_KEY(3, 1), KEYBOARD_NO_KEY);       /* 2 */
  type_key(SPECTRUM_KEY(6, 2), SPECTRUM_SYMBOL_SHIFT); /* + */
  type_key(SPECTRUM_KEY(3, 1), KEYBOARD_NO_KEY);       /* 2 */
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);

  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  TEST_EQUAL(text[0][0], '4');
  if (strncmp(text[ULA_ROWS - 1], "0 OK", 4) != 0) {
    TEST_FAIL("bottom line reads |%s|, expected BASIC's OK report", text[ULA_ROWS - 1]);
  }
}

/* The table of key legends, judged by the firmware rather than by itself:
   the characters are looked up, the keys held down, and what BASIC makes of
   them read back off the screen. `p` at the K cursor is the PRINT keyword,
   the quotes are symbol-shifted, and the letters inside them arrive as
   letters because by then the cursor is in L mode. */
static void the_keyboard_types_what_is_printed_on_it(void) {
  if (!power_on()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  const char *line = "p\"a1z.\"";
  for (const char *at = line; *at != '\0'; at++) {
    spectrum_shift shift = SPECTRUM_NO_SHIFT;
    keyboard_key key = spectrum_key_for_character(*at, &shift);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("no key carries '%c'", *at);
      return;
    }
    keyboard_key held = KEYBOARD_NO_KEY;
    if (shift == SPECTRUM_WITH_CAPS_SHIFT) {
      held = SPECTRUM_CAPS_SHIFT;
    } else if (shift == SPECTRUM_WITH_SYMBOL_SHIFT) {
      held = SPECTRUM_SYMBOL_SHIFT;
    }
    type_key(key, held);
  }
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);

  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  if (strncmp(text[0], "a1z.", 4) != 0) {
    TEST_FAIL("the top line reads |%s|, expected what was typed", text[0]);
  }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    rom_directory = argv[1];
  }
  TEST_RUN(the_48k_boots_to_its_copyright_message);
  TEST_RUN(the_screen_reads_the_same_through_the_beam);
  TEST_RUN(basic_does_arithmetic_it_is_typed);
  TEST_RUN(the_keyboard_types_what_is_printed_on_it);
  return TEST_REPORT("spectrum firmware");
}
