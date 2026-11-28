/*
 * spectrum_programs_test — the 48K and the +2 against programs written to
 * measure them.
 *
 * Each program here was written by someone else to test a Spectrum's
 * timing, and each is judged by what was seen on a real machine. On the
 * 48K:
 *
 * - Floating Spy (Ramsoft, 2002) reads an unattached port at every T-state
 *   of the picture's first eight lines and holds each byte to Ramsoft's own
 *   measurement of a real 48K, printing "Floating bus OK" if all agree.
 *   https://zxe.io/depot/software/ZX%20Spectrum/Floating%20Spy%20v0.33%20%282002-04%29%28Ramsoft%29%5B%21%5D.zip
 * - Richard and Tim Butler's 48K timing tests (2010) run groups of
 *   instructions until the interrupt stops them and hold where it caught
 *   them to values stored from real machines, early and late. Some of them
 *   need the floating bus: the suite's packagers name tests 35 to 37
 *   (MrKWatkins' EmulatorTestSuites, docs/content/ZXSpectrum/Timing.md),
 *   and without one the first to fail here is the contended test 36.
 *   https://www.zxspectrum4.net/op_timing.php
 * - Jan Bobrowski's btime and stime, and atime, Patrik Rak's slight
 *   modification of stime (ZXTests v3p, 2014), draw a border change, a
 *   write to the screen and a write to the attributes at a T-state the user
 *   chooses. Rak's table of 2019 records the T-states at which a real 48K
 *   shows each,
 *   https://worldofspectrum.org/forums/discussion/comment/958908/#Comment_958908,
 *   and the T-state a program prints is its own, so nothing here converts.
 *
 * On the +2, each loaded through its Tape Loader:
 *
 * - Floating Spy again, which finds a 128 for itself and holds it to
 *   Ramsoft's measurement of one.
 * - Bobrowski's minfo and Rak's Timing Test, held to Víctor Iborra's
 *   photographs of them on a grey +2.
 * - Rak's ptime, held to Brendan Alford's run of it.
 *
 * Each test below cites its record.
 *
 * Needs the firmware and the programs: run `make roms tapes`.
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

#define FRAMES_TO_PROMPT 100
/* A +2 tests its eight banks before it shows the menu. */
#define FRAMES_TO_MENU 300
/* The firmware scans the keyboard off the 50Hz interrupt and debounces, so
   a key must be held for several scans and released for several more. */
#define FRAMES_PER_KEY 6
/* Long enough for the longest of the programs to come off the tape. */
#define MOST_FRAMES_TO_LOAD 6000
/* How often a program's screen is looked at while it works. */
#define FRAMES_BETWEEN_LOOKS 50

static const char *rom_directory = "roms";
static const char *tape_directory = "test/data/tapes";
static uint8_t rom[SPECTRUM_PLUS2_ROM_SIZE]; /* room for either model's */
static uint8_t ram[SPECTRUM_RAM_128K];
static uint8_t framebuffer[SPECTRUM_LARGEST_FRAMEBUFFER];
static spectrum_t spectrum;

static uint8_t image[64 * 1024];
static tape_t tape;
static tzx_t reader;

/* The display's top-left sample in the framebuffer: a retrace and a left
   border across, and as far down as the frame sync left the picture. */
static int display_left(void) {
  return (spectrum.ula.frame.retrace_ticks + ULA_LEFT_BORDER_TICKS) * ULA_SAMPLES_PER_TICK;
}

static int display_top(void) {
  return spectrum.ula.frame.first_display_line - SPECTRUM_PICTURE_SHIFT;
}

static int raster_width(void) { return spectrum.ula.frame.ticks_per_line * ULA_SAMPLES_PER_TICK; }

static uint8_t colour_at(int x, int y) { return framebuffer[y * raster_width() + x] & 0x0F; }

/* The file into the buffer, which must have room to spare: one that fills
   it is taken to be larger than it. */
static bool read_file(const char *directory, const char *name, uint8_t *buffer, size_t room,
                      size_t *length, const char *remedy) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", directory, name);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    TEST_FAIL("cannot open %s — run '%s'", path, remedy);
    return false;
  }
  *length = fread(buffer, 1, room, file);
  fclose(file);
  if (*length == room) {
    TEST_FAIL("%s is larger than the %zu bytes kept for it", path, room);
    return false;
  }
  return true;
}

/* A ROM image into the ROM buffer at an offset, which it must fill exactly. */
static bool read_rom(const char *name, size_t at, size_t size) {
  size_t length = 0;
  if (!read_file(rom_directory, name, image, sizeof image, &length, "make roms")) {
    return false;
  }
  if (length != size) {
    TEST_FAIL("%s is %zu bytes, expected %zu", name, length, size);
    return false;
  }
  memcpy(rom + at, image, size);
  return true;
}

static bool power_on(spectrum_model model) {
  uint32_t ram_size = SPECTRUM_RAM_48K;
  if (model == SPECTRUM_48K) {
    if (!read_rom("spectrum48.rom", 0, SPECTRUM_48K_ROM_SIZE)) {
      return false;
    }
  } else {
    if (!read_rom("spectrumplus2-0.rom", 0, SPECTRUM_PLUS2_ROM_SIZE / 2) ||
        !read_rom("spectrumplus2-1.rom", SPECTRUM_PLUS2_ROM_SIZE / 2,
                  SPECTRUM_PLUS2_ROM_SIZE / 2)) {
      return false;
    }
    ram_size = SPECTRUM_RAM_128K;
  }
  memset(ram, 0x00, sizeof ram);
  memset(framebuffer, 0x00, sizeof framebuffer);
  spectrum_init(&spectrum, model, ram, ram_size, rom);
  spectrum_connect_monitor(&spectrum, framebuffer);
  return true;
}

static void run_frames(int frames) {
  for (int frame = 0; frame < frames; frame++) {
    for (uint32_t tick = 0; tick < spectrum.ula.frame.ticks_per_frame; tick++) {
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

/* What is typed is keys, and what they mean is the firmware's: a letter at
   the K cursor is the keyword printed on its key. */
static bool type_text(const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    if (*at == '\n') {
      type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
      continue;
    }
    spectrum_shift shift = SPECTRUM_NO_SHIFT;
    keyboard_key key = spectrum_key_for_character(*at, &shift);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("no key carries '%c'", *at);
      return false;
    }
    keyboard_key held = KEYBOARD_NO_KEY;
    if (shift == SPECTRUM_WITH_CAPS_SHIFT) {
      held = SPECTRUM_CAPS_SHIFT;
    } else if (shift == SPECTRUM_WITH_SYMBOL_SHIFT) {
      held = SPECTRUM_SYMBOL_SHIFT;
    }
    type_key(key, held);
  }
  return true;
}

/* The glyph in the table of the ROM that holds 48 BASIC, a +2's second,
   drawn either way round, or '?' for a shape it does not draw. */
static char character_for(const uint8_t glyph[8]) {
  const uint8_t *font =
      rom + FONT_IN_ROM + (spectrum.model == SPECTRUM_PLUS2 ? SPECTRUM_PLUS2_ROM_SIZE / 2 : 0);
  for (int code = FIRST_CODE; code < 128; code++) {
    const uint8_t *drawn = font + (size_t)(code - FIRST_CODE) * 8;
    bool same = true;
    bool inverse = true;
    for (int line = 0; line < 8; line++) {
      same = same && glyph[line] == drawn[line];
      inverse = inverse && glyph[line] == (uint8_t)~drawn[line];
    }
    if (same || inverse) {
      return (char)code;
    }
  }
  return '?';
}

/* The text the firmware prints, which it prints to the screen at 0x4000 on
   either model: on a +2 the screen in bank 5, whichever is being shown. */
static void read_screen(char text[ULA_ROWS][ULA_COLUMNS + 1]) {
  for (int row = 0; row < ULA_ROWS; row++) {
    for (int column = 0; column < ULA_COLUMNS; column++) {
      uint8_t glyph[8];
      for (int line = 0; line < 8; line++) {
        glyph[line] = spectrum_peek(&spectrum, ula_display_address(row * 8 + line, column));
      }
      text[row][column] = character_for(glyph);
    }
    text[row][ULA_COLUMNS] = '\0';
  }
}

/* The row the screen shows these words on, or -1. */
static int row_showing(const char *words) {
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  read_screen(text);
  for (int row = 0; row < ULA_ROWS; row++) {
    if (strstr(text[row], words) != NULL) {
      return row;
    }
  }
  return -1;
}

/* Whether a row of the screen reads these words and nothing after them. */
static bool row_reads(const char *words) {
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  read_screen(text);
  size_t length = strlen(words);
  for (int row = 0; row < ULA_ROWS; row++) {
    if (strncmp(text[row], words, length) == 0 &&
        strspn(text[row] + length, " ") == strlen(text[row] + length)) {
      return true;
    }
  }
  return false;
}

static void report_screen(void) {
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  read_screen(text);
  for (int row = 0; row < ULA_ROWS; row++) {
    fprintf(stderr, "    |%s|\n", text[row]);
  }
}

/* Run until the screen shows any of the words given, and say which, or -1
   if none has by the frame limit. */
static int run_until_showing(const char *const *words, int choices, int most_frames) {
  for (int frames = 0; frames < most_frames; frames += FRAMES_BETWEEN_LOOKS) {
    run_frames(FRAMES_BETWEEN_LOOKS);
    for (int choice = 0; choice < choices; choice++) {
      if (row_showing(words[choice]) >= 0) {
        return choice;
      }
    }
  }
  return -1;
}

static bool run_until(const char *words, int most_frames) {
  if (run_until_showing(&words, 1, most_frames) == 0) {
    return true;
  }
  TEST_FAIL("the screen never showed \"%s\"; it reads:", words);
  report_screen();
  return false;
}

/* LOAD "" typed at a 48K's prompt, or a +2's Tape Loader chosen from its
   menu, and the tape played, as a person would, until the program is on the
   screen with these words. */
static bool load_program(spectrum_model model, const char *name, const char *showing) {
  if (!power_on(model)) {
    return false;
  }
  size_t length = 0;
  if (!read_file(tape_directory, name, image, sizeof image, &length, "make tapes")) {
    return false;
  }
  const char *problem = NULL;
  if (!tzx_open(
          &reader, image, (uint32_t)length,
          (tzx_machine){.rules = TZX_SPECTRUM, .clock_hertz = spectrum_clock_hertz(&spectrum)},
          &problem)) {
    TEST_FAIL("%s was refused: %s", name, problem);
    return false;
  }
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);

  if (model == SPECTRUM_48K) {
    run_frames(FRAMES_TO_PROMPT);
    if (!type_text("j\"\"\n")) { /* J at the K cursor, which is LOAD */
      return false;
    }
  } else {
    if (!run_until("Tape Loader", FRAMES_TO_MENU)) {
      return false;
    }
    type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY); /* the menu's first choice */
  }
  tape_play(&tape);
  return run_until(showing, MOST_FRAMES_TO_LOAD);
}

/* Change one of a BASIC program's variables and carry on from a line: BREAK
   into it, which CAPS SHIFT and SPACE together are, and type the LET and the
   GO TO, which the K cursor takes as keywords from L and G. */
static bool set_and_go_to(const char *variable, long value, int line) {
  type_key(SPECTRUM_SPACE, SPECTRUM_CAPS_SHIFT);
  if (!run_until("BREAK into program", 100)) {
    return false;
  }
  char command[64];
  snprintf(command, sizeof command, "l%s=%ld:g%d\n", variable, value, line);
  return type_text(command);
}

/* Ramsoft's self test reads the picture's first eight lines a T-state at a
   time and prints its verdict when they are done. */
static void floating_spy_finds_the_bus_where_ramsofts_48k_has_it(void) {
  if (!load_program(SPECTRUM_48K, "floatspy.tap", "ULA TYPE:")) {
    return;
  }
  if (row_showing("ULA TYPE: 48K") < 0) {
    TEST_FAIL("Floating Spy did not take the machine for a 48K:");
    report_screen();
    return;
  }
  type_key(SPECTRUM_KEY(2, 4), KEYBOARD_NO_KEY); /* T, the self test */
  if (!run_until("Test completed.", 20000)) {
    return;
  }
  /* The OK stands in a box of inverse video a space wider than the word. */
  if (row_showing("Floating bus  OK  for ULA 48K") < 0) {
    TEST_FAIL("Floating Spy's verdict:");
    report_screen();
  }
}

/* The number a row of the screen gives after a label it starts with. */
static bool number_after(const char *row, const char *label, long *number) {
  size_t length = strlen(label);
  if (strncmp(row, label, length) != 0) {
    return false;
  }
  char *end = NULL;
  *number = strtol(row + length, &end, 10);
  return end != row + length;
}

/* Floating Spy's own reading: the T-state it reads at, by its count, and
   the byte it read there. */
static bool floating_spy_reading(long *tstate, int *byte) {
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  read_screen(text);
  bool have_time = false;
  bool have_byte = false;
  long read = 0;
  for (int row = 0; row < ULA_ROWS; row++) {
    have_time = have_time || number_after(text[row], "IN() TIME: ", tstate);
    if (number_after(text[row], "IN() BYTE: ", &read)) {
      have_byte = true;
      *byte = (int)read;
    }
  }
  return have_time && have_byte;
}

/* Press a key Floating Spy steps its time with until the time moves. */
static bool step_floating_spy(const char *key, long *tstate, int *byte) {
  long before = *tstate;
  for (int presses = 0; presses < 4; presses++) {
    if (!type_text(key)) {
      return false;
    }
    run_frames(FRAMES_PER_KEY);
    if (!floating_spy_reading(tstate, byte)) {
      TEST_FAIL("Floating Spy's reading is not on the screen:");
      report_screen();
      return false;
    }
    if (*tstate != before) {
      return true;
    }
  }
  TEST_FAIL("Floating Spy's time stayed at %ld", before);
  return false;
}

/* Chris Smith read port 16639, whose high byte the ULA charges at every
   T-state of the access, with Floating Spy on a real 48K: "the Spectrum
   begins at 14344" with "offsets 0, 2, 8, 8, 8, 8", and away from the first
   of them "the usual 67, 255 x 7, 69, 255 x 7, 71 ... sequence" ("Shocking
   Test Results" and "Testing The New Contention Model", 2007),
   https://web.archive.org/web/20141225102821/http://www.zxdesign.info/softwareTest.shtml
   https://web.archive.org/web/20141207122034/http://www.zxdesign.info/contentionTest2.shtml.
   Floating Spy fills the picture's first eight lines' attributes with 64
   upwards, so 67, 69 and 71 are the attributes of its fourth, sixth and
   eighth characters. */
static void floating_spy_reads_a_contended_port_where_smiths_48k_did(void) {
  if (!load_program(SPECTRUM_48K, "floatspy.tap", "ULA TYPE:")) {
    return;
  }
  if (!type_text("p16639\n")) {
    return;
  }
  run_frames(FRAMES_PER_KEY);
  long tstate = 0;
  int byte = 0;
  if (row_showing("I/O PORT: 16639") < 0 || !floating_spy_reading(&tstate, &byte)) {
    TEST_FAIL("Floating Spy was not set to port 16639:");
    report_screen();
    return;
  }
  while (tstate > 14340) {
    if (!step_floating_spy("a", &tstate, &byte)) {
      return;
    }
  }
  static const struct {
    long tstate;
    int byte; /* -1 for a byte Smith does not give */
  } bytes[] = {{14344, -1}, {14346, 67}, {14354, 69}, {14362, 71}, {14370, -1}, {14378, -1}};
  size_t next = 0;
  while (tstate <= 14380) {
    bool expected = next < sizeof bytes / sizeof bytes[0] && bytes[next].tstate == tstate;
    if (expected != (byte != 0xFF)) {
      TEST_FAIL("at %ld Floating Spy read %d, where Smith's 48K read %s", tstate, byte,
                expected ? "a byte of the screen" : "255");
    } else if (expected && bytes[next].byte >= 0 && byte != bytes[next].byte) {
      TEST_FAIL("at %ld Floating Spy read %d, where Smith's 48K read %d", tstate, byte,
                bytes[next].byte);
    }
    next += expected ? 1 : 0;
    if (!step_floating_spy("q", &tstate, &byte)) {
      return;
    }
  }
}

/* The tests decide first whether the machine runs early or late, and then
   hold it to the values stored for that kind. This one is early. A test
   that fails stops to show what it expected. */
static void the_butlers_tests_find_an_early_48k_and_pass_it(void) {
  if (!load_program(SPECTRUM_48K, "butler48.tap", "choose test")) {
    return;
  }
  if (row_showing("TYPE1 (Early) timings detected.") < 0) {
    TEST_FAIL("the tests did not find an early machine:");
    report_screen();
    return;
  }
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY); /* every test */
  static const char *const endings[] = {"All Tests Complete", "Press any key for next test"};
  int ending = run_until_showing(endings, 2, 20000);
  if (ending != 0 || row_showing("All Tests Complete 100% Pass") < 0) {
    TEST_FAIL("the tests stopped here:");
    report_screen();
  }
}

/* btime's red stripe is drawn on the border of the line above the picture,
   as far across as the OUT that starts it lands: its first sample, or -1. */
static int stripe_starts_at(void) {
  int y = display_top() - 1;
  for (int x = 0; x < raster_width(); x++) {
    if (colour_at(x, y) == 2) {
      return x;
    }
  }
  return -1;
}

/* Each program measures the frame before it draws anything, which takes it
   a few dozen. */
#define FRAMES_TO_SETTLE 50

static void border_change_lands(long tstate, int characters_right_of_the_picture) {
  if (tstate != 14112 && !set_and_go_to("t", tstate, 120)) {
    return;
  }
  run_frames(FRAMES_TO_SETTLE);
  char words[32];
  snprintf(words, sizeof words, "%ld + 224 = %ld", tstate, tstate + 224);
  if (row_showing(words) < 0) {
    TEST_FAIL("btime was not set to %ld:", tstate);
    report_screen();
    return;
  }
  int expected = display_left() + characters_right_of_the_picture * 8;
  int got = stripe_starts_at();
  if (got != expected) {
    TEST_FAIL("an OUT at %ld starts the stripe at sample %d, expected %d", tstate, got, expected);
  }
}

/* Rak's table: the change meets the picture's left edge for an OUT whose
   machine cycle begins from 14112 to 14115, and those four draw the same
   picture. That a T-state either side moves it a character is not in the
   table: it is what a colour taken every four T-states makes of it. */
static void btime_meets_the_picture_from_14112_to_14115(void) {
  if (!load_program(SPECTRUM_48K, "btime.tap", "border test")) {
    return;
  }
  border_change_lands(14112, 0);
  border_change_lands(14113, 0);
  border_change_lands(14114, 0);
  border_change_lands(14115, 0);
  border_change_lands(14116, 1);
  border_change_lands(14111, -1);
}

/* The first character of the screen, as the beam drew it: the colour of its
   first eight samples if they are one colour, or -1. */
static int first_character_colour(void) {
  int colour = colour_at(display_left(), display_top());
  for (int x = display_left(); x < display_left() + 8; x++) {
    if (colour_at(x, display_top()) != colour) {
      return -1;
    }
  }
  return colour;
}

static void write_lands(long tstate, int colour) {
  if (tstate != 14335 && !set_and_go_to("t", tstate, 120)) {
    return;
  }
  run_frames(FRAMES_TO_SETTLE);
  char words[16];
  snprintf(words, sizeof words, "T:%ld", tstate);
  if (row_showing(words) < 0) {
    TEST_FAIL("the test was not set to %ld:", tstate);
    report_screen();
    return;
  }
  int got = first_character_colour();
  if (got != colour) {
    TEST_FAIL("a write at %ld leaves the first character colour %d, expected %d", tstate, got,
              colour);
  }
}

/* stime clears and then fills the first byte of the screen, the top row of
   the first character, in red ink on yellow paper. Rak's table: a write
   whose machine cycle begins at 14335 is seen, and one at 14336 is not. */
static void stime_sees_a_write_at_14335_and_not_at_14336(void) {
  if (!load_program(SPECTRUM_48K, "stime.tap", "screen test")) {
    return;
  }
  write_lands(14335, 2);
  write_lands(14336, 6);
}

/* atime writes the first character red paper and then white. Rak's table:
   at 14335 the red is gone, and at 14336 it shows. */
static void atime_sees_a_write_at_14335_and_not_at_14336(void) {
  if (!load_program(SPECTRUM_48K, "atime.tap", "screen test")) {
    return;
  }
  write_lands(14335, 7);
  write_lands(14336, 2);
}

/* Floating Spy finds a 128-family machine for itself, and reads the bus
   against Ramsoft's measurement of one: "It works on
   48K/128K/+2 Spectrums and the specific ULA model is autodetected"
   ("The floating bus explained!", Ramsoft, 2002,
   https://k1.spdns.de/Develop/Projects/zxsp/Info/Floating%20bus%20technical%20guide.html). */
static void floating_spy_finds_the_bus_where_ramsofts_128_has_it(void) {
  if (!load_program(SPECTRUM_PLUS2, "floatspy.tap", "ULA TYPE:")) {
    return;
  }
  if (row_showing("ULA TYPE: 128K") < 0) {
    TEST_FAIL("Floating Spy did not take the machine for a 128K:");
    report_screen();
    return;
  }
  type_key(SPECTRUM_KEY(2, 4), KEYBOARD_NO_KEY); /* T, the self test */
  if (!run_until("Test completed.", 20000)) {
    return;
  }
  if (row_showing("Floating bus  OK  for ULA 128K") < 0) {
    TEST_FAIL("Floating Spy's verdict:");
    report_screen();
  }
}

/* minfo measures the frame, whether EI holds off an interrupt as a prefix
   does, and for how many T-states the interrupt can still be taken. Víctor
   Iborra's photograph of it on a Spanish grey +2, 2024-03-27, reads
   "Frame time: 70908", "EI is prefix: yes" and "INT time: 36",
   https://zxe.io/depot/testing/ZX%20Spectrum/Minfo%20%282011-08-20%29%28Bobrowski,%20Jan%29%20-%20Testing%20%7B2024-03-27%7D%7BIborra,%20Vi%CC%81ctor%7D%7BZX%20Spectrum%20+2%20ES%7D.jpg
 */
static void minfo_reads_the_plus2_as_iborras_did(void) {
  if (!load_program(SPECTRUM_PLUS2, "minfo.tap", "INT time")) {
    return;
  }
  static const char *const lines[] = {"Frame time: 70908", "EI is prefix: yes", "INT time: 36"};
  for (size_t index = 0; index < sizeof lines / sizeof lines[0]; index++) {
    if (!row_reads(lines[index])) {
      TEST_FAIL("minfo does not show \"%s\":", lines[index]);
      report_screen();
      return;
    }
  }
}

/* The Timing Test's table: 160 T-states from 14336, eight to a row. */
#define TIMED_IN_A_ROW 8
#define TIMED_ROWS 20
#define FIRST_TIMED 14336

typedef struct {
  long first;
  int durations[TIMED_IN_A_ROW];
} timed_row;

/* One of the photographed tables. Each row given holds until the next one
   given, as the photographs show. */
typedef struct {
  const char *choice; /* typed at its prompt */
  const char *title;  /* what it prints after "Test: " */
  const timed_row *rows;
  size_t row_count;
} timing_photograph;

#define FOUR {4, 4, 4, 4, 4, 4, 4, 4}

static const timed_row contended_nop[] = {
    {14336, FOUR},
    {14360, {4, 4, 4, 10, 9, 8, 7, 6}},
    {14368, {5, 4, 4, 10, 9, 8, 7, 6}},
    {14488, {5, 4, 4, 4, 4, 4, 4, 4}},
};
static const timed_row snow_effect_nop[] = {
    {14336, FOUR},
    {14344, {4, 4, 4, 10, 9, 8, 7, 6}},
    {14352, {5, 4, 4, 10, 9, 8, 7, 6}},
    {14472, {5, 4, 4, 4, 4, 4, 4, 4}},
    {14480, FOUR},
};
static const timed_row in_00fe[] = {
    {14336, FOUR},
    {14360, {4, 4, 10, 9, 8, 7, 6, 5}},
    {14488, FOUR},
};
static const timed_row uncontended[] = {
    {14336, FOUR},
};
static const timed_row in_7ffe[] = {
    {14336, FOUR},
    {14360, {4, 4, 10, 10, 9, 8, 7, 6}},
    {14368, {5, 4, 10, 10, 9, 8, 7, 6}},
    {14488, {5, 4, 4, 4, 4, 4, 4, 4}},
};
static const timed_row in_7fff[] = {
    {14336, FOUR},
    {14360, {10, 10, 16, 16, 15, 14, 13, 12}},
    {14368, {11, 10, 16, 16, 15, 14, 13, 12}},
    {14480, {11, 10, 10, 10, 9, 8, 7, 6}},
    {14488, {5, 4, 4, 4, 4, 4, 4, 4}},
};
/* The page at 0xC000 is BASIC's own, bank 0, which the ULA leaves alone. */
static const timed_row page_ret[] = {
    {14336, {0, 0, 0, 0, 0, 0, 0, 0}},
};

#define PHOTOGRAPH(choice, title, rows)                                                            \
  {(choice), (title), (rows), sizeof(rows) / sizeof((rows)[0])}

/* 0xFFFE answers as 0x00FE does, and 0xFFFF as 0x00FF, as the photographs
   show: bank 0, paged at 0xC000, is not one the ULA wants (spectrum.h). */
static const timing_photograph timing_photographs[] = {
    PHOTOGRAPH("0", "contended NOP", contended_nop),
    PHOTOGRAPH("1", "snow effect NOP", snow_effect_nop),
    PHOTOGRAPH("2", "IN #00FE IO", in_00fe),
    PHOTOGRAPH("3", "IN #00FF IO", uncontended),
    PHOTOGRAPH("4", "IN #7FFE IO", in_7ffe),
    PHOTOGRAPH("5", "IN #7FFF IO", in_7fff),
    PHOTOGRAPH("6", "IN #FFFE IO", in_00fe),
    PHOTOGRAPH("7", "IN #FFFF IO", uncontended),
    PHOTOGRAPH("8", "128k page RET", page_ret),
};

/* The duration the photograph gives for an instruction started at a
   T-state. */
static int photographed(const timing_photograph *photograph, long tstate) {
  size_t index = 0;
  while (index + 1 < photograph->row_count && photograph->rows[index + 1].first <= tstate) {
    index++;
  }
  const timed_row *row = &photograph->rows[index];
  return row->durations[(tstate - FIRST_TIMED) % TIMED_IN_A_ROW];
}

/* A row of the table as printed: its first T-state, then eight durations. */
static bool timed_row_on_screen(const char *text, long *first, int durations[TIMED_IN_A_ROW]) {
  char *end = NULL;
  *first = strtol(text, &end, 10);
  if (end == text) {
    return false;
  }
  for (int index = 0; index < TIMED_IN_A_ROW; index++) {
    const char *from = end;
    durations[index] = (int)strtol(from, &end, 10);
    if (end == from) {
      return false;
    }
  }
  return true;
}

/* Rak's Timing Test (2013) times an instruction started at each of 160
   T-states and prints how long each took, less the RET that ends it. Iborra
   photographed every one of its tests on a Spanish grey +2 in 128K mode,
   2023-07-05: the depot's "Timing Test v0.3 (2013-12-09)(Rak, Patrik) -
   Testing {2023-07-05}{Iborra, Víctor}{ZX Spectrum +2 ES} 128K mode"
   series, listed at https://github.com/redcode/ZXSpectrum/wiki/Timing-Test,
   whose "Frame Duration" photograph shows the frame the program measures
   before its menu as 70908. */
static void the_timing_test_reads_as_on_iborras_plus2(void) {
  if (!load_program(SPECTRUM_PLUS2, "timing.tap", "Choose test")) {
    return;
  }
  if (row_showing("Frame duration: 70908") < 0) {
    TEST_FAIL("the frame was not measured at 70908:");
    report_screen();
    return;
  }
  for (size_t test = 0; test < sizeof timing_photographs / sizeof timing_photographs[0]; test++) {
    const timing_photograph *photograph = &timing_photographs[test];
    char typed[8];
    snprintf(typed, sizeof typed, "%s\n", photograph->choice);
    if (!type_text(typed) || !run_until("Press any key.", 20000)) {
      return;
    }
    char text[ULA_ROWS][ULA_COLUMNS + 1];
    read_screen(text);
    char title[40];
    snprintf(title, sizeof title, "Test: %s", photograph->title);
    if (strstr(text[0], title) == NULL) {
      TEST_FAIL("the table is not titled \"%s\":", title);
      report_screen();
      return;
    }
    for (int row = 0; row < TIMED_ROWS; row++) {
      long first = 0;
      int durations[TIMED_IN_A_ROW];
      if (!timed_row_on_screen(text[2 + row], &first, durations) ||
          first != FIRST_TIMED + (long)row * TIMED_IN_A_ROW) {
        TEST_FAIL("%s: row %d of the table is not one:", photograph->title, row);
        report_screen();
        return;
      }
      for (int index = 0; index < TIMED_IN_A_ROW; index++) {
        long tstate = first + index;
        int expected = photographed(photograph, tstate);
        if (durations[index] != expected) {
          TEST_FAIL("%s from %ld took %d T-states, where Iborra's +2 took %d", photograph->title,
                    tstate, durations[index], expected);
        }
      }
    }
    type_key(SPECTRUM_SPACE, KEYBOARD_NO_KEY);
    if (!run_until("Choose test", 500)) {
      return;
    }
  }
}

/* The run of the other screen's black at the left of the picture's first
   line, in pixels. */
static int ptime_bar(void) {
  int width = 0;
  while (width < 256) {
    uint8_t colour = colour_at(display_left() + width, display_top());
    if (colour != 0 && colour != 8) {
      break;
    }
    width++;
  }
  return width;
}

/* Step ptime with Q or A until it prints the T-state wanted. */
static bool ptime_set_to(long wanted) {
  for (int presses = 0; presses < 8; presses++) {
    char text[ULA_ROWS][ULA_COLUMNS + 1];
    read_screen(text);
    long shown = 0;
    if (!number_after(text[3], "", &shown)) {
      TEST_FAIL("ptime does not show its T-state:");
      report_screen();
      return false;
    }
    if (shown == wanted) {
      return true;
    }
    if (!type_text(shown < wanted ? "q" : "a")) {
      return false;
    }
    run_frames(FRAMES_PER_KEY);
  }
  TEST_FAIL("ptime never showed %ld", wanted);
  return false;
}

/* The bar is looked at once the loop has run this many frames. */
#define FRAMES_TO_STEADY 20

/* ptime switches the screen shown with an OUT to 0x7FFD meant to begin at
   the T-state it prints. Every byte the ULA fetched for the picture's first
   line before the switch comes from the screen shown before the OUT, which
   draws black from the left. Brendan Alford's run of the updated version on
   a toastrack read, a line to each, "14358 - no bar visible", "14359 - 16
   pixels wide", "14360 = 16", then "14361 = 32" and so on to "14368 = 32",
   "14369 = 48" and so on to "14376 = 48", and "14377 = 64"; and on his
   grey +2, with a Zilog in it, "results are again identical to the
   toastrack" (World of Spectrum forum, 2014),
   https://worldofspectrum.org/forums/discussion/comment/759591/#Comment_759591.
   The tape here, ZXTests v3p's, 1079 bytes and dated 2014-03-11, is taken
   to be that version, Rak's "should be 1709 bytes long" for it
   (https://worldofspectrum.org/forums/discussion/comment/759528/#Comment_759528)
   reading as the same digits transposed; that is an inference.
   It is stepped here from the 14361 ptime starts at down to 14358 and then
   up, the way its keys allow; how Alford stepped it is not recorded. On
   this machine its loop does not begin the OUT on the T-state it prints:
   the IN that ends the loop, which by our reading of its code is placed to
   bring the OUT's contention to a multiple of four before the loop halts
   for the next frame, lands where this machine's contention, which the
   Timing Test's photographs grade, does not, and the OUT begins up to three
   T-states after the one printed. The bars are read as the screen shows
   them, as Alford read his. A bank register latched a T-state sooner than
   the OUT's last, as the processor puts the byte out, misses six of the
   twenty. */
static void ptime_meets_the_first_line_as_on_alfords_plus2(void) {
  if (!load_program(SPECTRUM_PLUS2, "ptime.tap", "screen paging test")) {
    return;
  }
  static const struct {
    long tstate;
    int pixels;
  } bars[] = {{14358, 0},  {14359, 16}, {14360, 16}, {14361, 32},
              {14368, 32}, {14369, 48}, {14376, 48}, {14377, 64}};
  long tstate = 14358;
  size_t next = 0;
  int expected = 0;
  while (tstate <= 14377) {
    if (next < sizeof bars / sizeof bars[0] && bars[next].tstate == tstate) {
      expected = bars[next].pixels;
      next++;
    }
    if (!ptime_set_to(tstate)) {
      return;
    }
    run_frames(FRAMES_TO_STEADY);
    int got = ptime_bar();
    if (got != expected) {
      TEST_FAIL("an OUT at %ld shows %d pixels of the other screen, where Alford's showed %d",
                tstate, got, expected);
    }
    tstate++;
  }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    rom_directory = argv[1];
  }
  if (argc > 2) {
    tape_directory = argv[2];
  }
  TEST_RUN(floating_spy_finds_the_bus_where_ramsofts_48k_has_it);
  TEST_RUN(floating_spy_reads_a_contended_port_where_smiths_48k_did);
  TEST_RUN(the_butlers_tests_find_an_early_48k_and_pass_it);
  TEST_RUN(btime_meets_the_picture_from_14112_to_14115);
  TEST_RUN(stime_sees_a_write_at_14335_and_not_at_14336);
  TEST_RUN(atime_sees_a_write_at_14335_and_not_at_14336);
  TEST_RUN(floating_spy_finds_the_bus_where_ramsofts_128_has_it);
  TEST_RUN(minfo_reads_the_plus2_as_iborras_did);
  TEST_RUN(the_timing_test_reads_as_on_iborras_plus2);
  TEST_RUN(ptime_meets_the_first_line_as_on_alfords_plus2);
  return TEST_REPORT("spectrum programs");
}
