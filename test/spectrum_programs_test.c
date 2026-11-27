/*
 * spectrum_programs_test — the 48K against programs written to measure one.
 *
 * Each program here was written by someone else to test a Spectrum's
 * timing, and each is judged by what was seen on a real machine:
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
/* The firmware scans the keyboard off the 50Hz interrupt and debounces, so
   a key must be held for several scans and released for several more. */
#define FRAMES_PER_KEY 6
/* Long enough for the longest of the programs to come off the tape. */
#define MOST_FRAMES_TO_LOAD 6000
/* How often a program's screen is looked at while it works. */
#define FRAMES_BETWEEN_LOOKS 50

static const char *rom_directory = "roms";
static const char *tape_directory = "test/data/tapes";
static uint8_t rom[SPECTRUM_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_48K];
static uint8_t framebuffer[SPECTRUM_FRAMEBUFFER_WIDTH * SPECTRUM_FRAMEBUFFER_HEIGHT];
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

static uint8_t colour_at(int x, int y) {
  return framebuffer[y * SPECTRUM_FRAMEBUFFER_WIDTH + x] & 0x0F;
}

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

static bool power_on(void) {
  size_t length = 0;
  if (!read_file(rom_directory, "spectrum48.rom", image, sizeof image, &length, "make roms")) {
    return false;
  }
  if (length != sizeof rom) {
    TEST_FAIL("spectrum48.rom is %zu bytes, expected %zu", length, sizeof rom);
    return false;
  }
  memcpy(rom, image, sizeof rom);
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

/* The glyph in the ROM's own table, drawn either way round, or '?' for a
   shape it does not draw. */
static char character_for(const uint8_t glyph[8]) {
  for (int code = FIRST_CODE; code < 128; code++) {
    const uint8_t *drawn = rom + FONT_IN_ROM + (size_t)(code - FIRST_CODE) * 8;
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

static void read_screen(char text[ULA_ROWS][ULA_COLUMNS + 1]) {
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

/* LOAD "" typed at the prompt and the tape played, as a person would, until
   the program is on the screen with these words. */
static bool load_program(const char *name, const char *showing) {
  if (!power_on()) {
    return false;
  }
  size_t length = 0;
  if (!read_file(tape_directory, name, image, sizeof image, &length, "make tapes")) {
    return false;
  }
  const char *problem = NULL;
  if (!tzx_open(&reader, image, (uint32_t)length, SPECTRUM_TICKS_PER_MILLISECOND, TZX_SPECTRUM,
                &problem)) {
    TEST_FAIL("%s was refused: %s", name, problem);
    return false;
  }
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);

  run_frames(FRAMES_TO_PROMPT);
  if (!type_text("j\"\"\n")) { /* J at the K cursor, which is LOAD */
    return false;
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
  if (!load_program("floatspy.tap", "ULA TYPE:")) {
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
  if (!load_program("floatspy.tap", "ULA TYPE:")) {
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
  if (!load_program("butler48.tap", "choose test")) {
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
  for (int x = 0; x < SPECTRUM_FRAMEBUFFER_WIDTH; x++) {
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
  if (!load_program("btime.tap", "border test")) {
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
  if (!load_program("stime.tap", "screen test")) {
    return;
  }
  write_lands(14335, 2);
  write_lands(14336, 6);
}

/* atime writes the first character red paper and then white. Rak's table:
   at 14335 the red is gone, and at 14336 it shows. */
static void atime_sees_a_write_at_14335_and_not_at_14336(void) {
  if (!load_program("atime.tap", "screen test")) {
    return;
  }
  write_lands(14335, 7);
  write_lands(14336, 2);
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
  return TEST_REPORT("spectrum programs");
}
