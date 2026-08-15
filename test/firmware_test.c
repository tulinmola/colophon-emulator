/*
 * firmware_test — boot the real firmware and read the screen back.
 *
 * The acceptance tier for the machine. Everything here is judged by
 * Locomotive Software and Amstrad rather than by us: the boot screen is
 * theirs, the arithmetic is BASIC's, and the letters are identified by
 * looking each glyph up in the character table the ROM itself carries at
 * &3800 — eight bytes per code, which is how the firmware draws them in the
 * first place. A test that recognised letters by our own table would only
 * prove we agree with ourselves.
 *
 * Needs the firmware images: run `make roms` first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpc.h"
#include "test.h"

/* The display, in characters: 40 columns of 25 rows, each glyph 8 pixels
   wide — 16 samples in mode 1 — and 8 lines tall, starting at the raster
   position the syncs put it. */
#define COLUMNS 40
#define ROWS 25
#define DISPLAY_LEFT 272
#define DISPLAY_TOP 70
#define FONT_IN_ROM 0x3800

/* The boot screen stops changing at frame 42; this waits well past it. */
#define FRAMES_TO_PROMPT 78
#define FRAMES_PER_KEY 3

static uint8_t ram[0x20000];
static uint8_t rom[0x8000];
static uint8_t framebuffer[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT];
static cpc_t cpc;
static char screen[ROWS][COLUMNS + 1];
static const char *rom_directory = "roms";

static bool load_rom(const char *file) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", rom_directory, file);
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    TEST_FAIL("cannot open %s — run 'make roms'", path);
    return false;
  }
  size_t read = fread(rom, 1, sizeof rom, handle);
  fclose(handle);
  if (read != sizeof rom) {
    TEST_FAIL("%s holds %zu bytes, expected %zu", path, read, sizeof rom);
    return false;
  }
  return true;
}

static void run_frames(long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    cpc_tick(&cpc);
  }
}

static bool power_on(const char *file, uint32_t ram_size, bool fifty_hz) {
  memset(ram, 0, sizeof ram);
  memset(framebuffer, 0, sizeof framebuffer);
  if (!load_rom(file)) {
    return false;
  }
  cpc_init(&cpc, ram, ram_size, rom);
  cpc_set_upper_rom(&cpc, 0, rom + 0x4000);
  cpc_connect_monitor(&cpc, framebuffer);
  cpc_set_links(&cpc, fifty_hz, CPC_MANUFACTURER_AMSTRAD);
  return true;
}

/* Read the display back as text, naming each cell by the ROM's own font. */
static void read_screen(void) {
  uint8_t paper = cpc.gate_array.inks[0];
  for (int row = 0; row < ROWS; row++) {
    for (int column = 0; column < COLUMNS; column++) {
      uint8_t glyph[8];
      for (int line = 0; line < 8; line++) {
        uint8_t bits = 0;
        for (int pixel = 0; pixel < 8; pixel++) {
          size_t x = (size_t)DISPLAY_LEFT + (size_t)column * 16 + (size_t)pixel * 2;
          size_t y = (size_t)DISPLAY_TOP + (size_t)row * 8 + (size_t)line;
          if (framebuffer[y * CPC_FRAMEBUFFER_WIDTH + x] != paper) {
            bits |= (uint8_t)(0x80u >> pixel);
          }
        }
        glyph[line] = bits;
      }
      char found = '?';
      for (int code = 32; code < 127; code++) {
        if (memcmp(rom + FONT_IN_ROM + (size_t)code * 8, glyph, 8) == 0) {
          found = (char)code;
          break;
        }
      }
      /* The cursor is a solid block, which is no character at all. */
      if (found == '?' && glyph[0] == 0xFF) {
        found = ' ';
      }
      screen[row][column] = found;
    }
    screen[row][COLUMNS] = '\0';
  }
}

static void print_screen(void) {
  for (int row = 0; row < ROWS; row++) {
    printf("    %2d |%s|\n", row, screen[row]);
  }
}

static void expect_row_contains(int row, const char *text) {
  if (strstr(screen[row], text) == NULL) {
    TEST_FAIL("row %d reads \"%s\", expected it to contain \"%s\"", row, screen[row], text);
    print_screen();
  }
}

static void type_key(keyboard_key key, bool shifted) {
  if (shifted) {
    keyboard_press(&cpc.keyboard, KEYBOARD_SHIFT);
  }
  keyboard_press(&cpc.keyboard, key);
  run_frames(FRAMES_PER_KEY);
  keyboard_release_all(&cpc.keyboard);
  run_frames(FRAMES_PER_KEY);
}

static void type_text(const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    bool shifted = false;
    keyboard_key key = *at == '\n' ? KEYBOARD_RETURN : keyboard_key_for_character(*at, &shifted);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("this keyboard has no '%c'", *at);
      return;
    }
    type_key(key, shifted);
  }
}

static void the_6128_boots_to_its_prompt(void) {
  if (!power_on("cpc6128.rom", 0x20000, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  read_screen();
  expect_row_contains(1, "Amstrad 128K Microcomputer");
  expect_row_contains(1, "(v3)");
  expect_row_contains(3, "1985 Amstrad Consumer Electronics plc");
  expect_row_contains(4, "and Locomotive Software Ltd.");
  expect_row_contains(6, "BASIC 1.1");
  expect_row_contains(8, "Ready");
}

static void the_664_boots_to_its_prompt(void) {
  if (!power_on("cpc664.rom", 0x10000, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  read_screen();
  expect_row_contains(1, "Amstrad 64K Microcomputer");
  expect_row_contains(1, "(v2)");
  /* The 664 introduced BASIC 1.1, which the 6128 inherited; only the 464
     shipped 1.0. */
  expect_row_contains(6, "BASIC 1.1");
  expect_row_contains(8, "Ready");
}

static void the_464_boots_to_its_prompt(void) {
  if (!power_on("cpc464.rom", 0x10000, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  read_screen();
  expect_row_contains(1, "Amstrad 64K Microcomputer");
  expect_row_contains(1, "(v1)");
  expect_row_contains(6, "BASIC 1.0");
  expect_row_contains(8, "Ready");
}

/* The whole machine, judged by a third party: the keyboard matrix, the
   PPI's direction flipping, the PSG, the 50Hz scan and the interrupt that
   drives it all have to be right for BASIC to answer at all. */
static void basic_does_arithmetic_it_is_typed(void) {
  if (!power_on("cpc6128.rom", 0x20000, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_text("PRINT 2+2\n");
  run_frames(10);
  read_screen();
  expect_row_contains(9, "PRINT 2+2");
  expect_row_contains(10, "4");
  expect_row_contains(11, "Ready");
}

/* Every character the keyboard claims to have, typed and read back off the
   screen. The firmware's own key table is the judge, which is how the comma
   and the full stop were caught sitting on each other's keys. */
static void every_character_types_as_itself(void) {
  static const char *punctuation = "abz019 ,.;:/?-=[]{}@#$%&*()_+<>";
  if (!power_on("cpc6128.rom", 0x20000, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_text(punctuation);
  run_frames(4);
  read_screen();
  expect_row_contains(9, punctuation);
}

/* BASIC counts colours its own way and the firmware translates: what BASIC
   calls 26 reaches the Gate Array as hardware code 11, and what it calls 1
   arrives as 4. The two numberings sit side by side in Grimware's INKR
   table, and only the hardware one means anything to the chip. */
static void a_typed_line_can_change_the_border(void) {
  if (!power_on("cpc6128.rom", 0x20000, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  TEST_EQUAL(cpc.gate_array.inks[16], 4); /* BASIC 1: blue */
  type_text("BORDER 26\n");
  run_frames(10);
  TEST_EQUAL(cpc.gate_array.inks[16], 11); /* BASIC 26: bright white */
  type_text("BORDER 0\n");
  run_frames(10);
  TEST_EQUAL(cpc.gate_array.inks[16], 20); /* BASIC 0: black */
}

/* The 6128's ROM holds two CRTC tables and picks between them on the link
   the PPI reports: 312 lines at 50Hz, 262 at 60Hz. */
static void the_refresh_link_chooses_the_crtc_table(void) {
  if (!power_on("cpc6128.rom", 0x20000, true)) {
    return;
  }
  run_frames(4);
  TEST_EQUAL(cpc.crtc.registers[4], 38);
  TEST_EQUAL(cpc.crtc.registers[5], 0);
  TEST_EQUAL(cpc.crtc.registers[7], 30);

  if (!power_on("cpc6128.rom", 0x20000, false)) {
    return;
  }
  run_frames(4);
  TEST_EQUAL(cpc.crtc.registers[4], 31);
  TEST_EQUAL(cpc.crtc.registers[5], 6);
  TEST_EQUAL(cpc.crtc.registers[7], 27);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    rom_directory = argv[1];
  }
  TEST_RUN(the_6128_boots_to_its_prompt);
  TEST_RUN(the_664_boots_to_its_prompt);
  TEST_RUN(the_464_boots_to_its_prompt);
  TEST_RUN(basic_does_arithmetic_it_is_typed);
  TEST_RUN(every_character_types_as_itself);
  TEST_RUN(a_typed_line_can_change_the_border);
  TEST_RUN(the_refresh_link_chooses_the_crtc_table);
  return TEST_REPORT("firmware");
}
