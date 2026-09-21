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
#include "tape.h"
#include "test.h"
#include "tzx.h"

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

/* The ROM's own tape loader, which measures the time between edges on the
   EAR line and has no other way of knowing what it is being given. Entered
   with the flag byte it expects in A, the destination in IX, the length in
   DE and carry set to load rather than verify; it returns with carry set if
   the block arrived and its checksum agreed.

   Sources:
   - "The complete Spectrum ROM disassembly" (Ian Logan and Frank O'Hara),
     the LD-BYTES routine at &0556 and its entry conditions. */
#define LD_BYTES 0x0556

/* A header's pilot is 8063 pulses of 2168 T-states, which is five seconds,
   and a whole tape is that twice over with a second of pause between. */
#define MOST_FRAMES_TO_LOAD 700

static void a_block_loads_through_the_roms_own_loader(void) {
  if (!power_on()) {
    return;
  }
  /* One headerless block: the flag a data block carries, sixteen bytes, and
     the checksum the ROM will insist on. */
  static uint8_t image[] = {0x12, 0x00, 0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G',
                            'H',  'I',  'J',  'K', 'L', 'M', 'N', 'O', 'P', 0x00};
  uint8_t checksum = 0;
  for (size_t index = 2; index + 1 < sizeof image; index++) {
    checksum ^= image[index];
  }
  image[sizeof image - 1] = checksum;

  static tape_t tape;
  static tzx_t reader;
  const char *problem = NULL;
  if (!tzx_open(&reader, image, (uint32_t)sizeof image, SPECTRUM_TICKS_PER_MILLISECOND,
                TZX_SPECTRUM, &problem)) {
    TEST_FAIL("the tape was refused: %s", problem);
    return;
  }
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);
  tape_play(&tape);

  /* LD IX,&9000 : LD DE,16 : LD A,&FF : SCF : CALL LD-BYTES : HALT */
  static const uint8_t program[] = {0xDD,          0x21, 0x00, 0x90, 0x11, 0x10,
                                    0x00,          0x3E, 0xFF, 0x37, 0xCD, LD_BYTES & 0xFF,
                                    LD_BYTES >> 8, 0x76};
  for (size_t index = 0; index < sizeof program; index++) {
    spectrum_poke(&spectrum, (uint16_t)(0x8000 + index), program[index]);
  }
  spectrum.cpu.pc = 0x8000;
  spectrum.cpu.sp = 0x7FF0;

  for (long tick = 0; tick < MOST_FRAMES_TO_LOAD * (long)SPECTRUM_TICKS_PER_FRAME; tick++) {
    spectrum_tick(&spectrum);
    if (spectrum.cpu.halted) {
      break;
    }
  }
  if (!spectrum.cpu.halted) {
    TEST_FAIL("the loader never returned");
    return;
  }
  TEST_CHECK((spectrum.cpu.f & Z80_FLAG_C) != 0); /* the block arrived whole */
  for (int index = 0; index < 16; index++) {
    uint8_t want = image[3 + index];
    uint8_t got = spectrum_peek(&spectrum, (uint16_t)(0x9000 + index));
    if (got != want) {
      TEST_FAIL("byte %d of the block loaded as &%02X, not &%02X", index, got, want);
      return;
    }
  }
}

/* The whole path a person takes: type LOAD "" at the prompt, press PLAY,
   and let the firmware find a program on the tape and run it. Nothing here
   reaches into the machine — the header is read by the ROM, the block is
   loaded by the ROM, and BASIC runs what arrives. */
static void a_program_loads_off_a_tape_and_runs(void) {
  if (!power_on()) {
    return;
  }
  /* A header naming a BASIC program that autostarts at line 10, and the
     program: 10 PRINT "TAPE OK". Both blocks carry the flag and checksum a
     tape carries. */
  static uint8_t image[] = {
      0x13, 0x00, 0x00,                                            /* block, flag: a header */
      0x00,                                                        /* type 0: a program */
      't',  'a',  'p',  'e',  ' ', ' ', ' ', ' ', ' ', ' ',        /* its name */
      0x0F, 0x00,                                                  /* the program is 15 bytes */
      0x0A, 0x00,                                                  /* autostart at line 10 */
      0x0F, 0x00,                                                  /* and no variables after it */
      0x00,                                                        /* checksum, filled in below */
      0x11, 0x00, 0xFF,                                            /* block, flag: data */
      0x00, 0x0A, 0x0B, 0x00,                                      /* line 10, eleven bytes of it */
      0xF5, 0x22, 'T',  'A',  'P', 'E', ' ', 'O', 'K', 0x22, 0x0D, /* PRINT "TAPE OK" */
      0x00,                                                        /* checksum, filled in below */
  };
  /* A block's checksum is every byte of it from the flag onwards, exclusive
     or'd together, and it is the last byte of the block. */
  static const size_t header_at = 2, header_checksum_at = 20;
  static const size_t data_at = 23, data_checksum_at = 39;
  for (size_t index = header_at; index < header_checksum_at; index++) {
    image[header_checksum_at] ^= image[index];
  }
  for (size_t index = data_at; index < data_checksum_at; index++) {
    image[data_checksum_at] ^= image[index];
  }

  static tape_t tape;
  static tzx_t reader;
  const char *problem = NULL;
  if (!tzx_open(&reader, image, (uint32_t)sizeof image, SPECTRUM_TICKS_PER_MILLISECOND,
                TZX_SPECTRUM, &problem)) {
    TEST_FAIL("the tape was refused: %s", problem);
    return;
  }
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);

  run_frames(FRAMES_TO_PROMPT);
  type_key(SPECTRUM_KEY(6, 3), KEYBOARD_NO_KEY);       /* J, which is LOAD */
  type_key(SPECTRUM_KEY(5, 0), SPECTRUM_SYMBOL_SHIFT); /* " */
  type_key(SPECTRUM_KEY(5, 0), SPECTRUM_SYMBOL_SHIFT); /* " again, which the ROM */
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);           /*   only takes if it saw the */
  tape_play(&tape);                                    /*   first one let go */
  run_frames(MOST_FRAMES_TO_LOAD);

  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  /* The ROM prints what it found on the tape, and then the program it
     started prints for itself. */
  if (strncmp(text[1], "Program: tape", 13) != 0) {
    TEST_FAIL("the header line reads |%s|, expected what the ROM found", text[1]);
    return;
  }
  if (strncmp(text[2], "TAPE OK", 7) != 0) {
    TEST_FAIL("the line under it reads |%s|, expected what the program printed", text[2]);
    return;
  }
  if (strncmp(text[ULA_ROWS - 1], "0 OK, 10:1", 10) != 0) {
    TEST_FAIL("the report reads |%s|, expected BASIC's after line 10", text[ULA_ROWS - 1]);
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
  TEST_RUN(a_block_loads_through_the_roms_own_loader);
  TEST_RUN(a_program_loads_off_a_tape_and_runs);
  return TEST_REPORT("spectrum firmware");
}
