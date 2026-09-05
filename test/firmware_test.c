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
 * The disc is judged the same way. AMSDOS catalogues a disc somebody else
 * wrote, and the names and sizes it prints are set against what a reader
 * written here, knowing nothing of the controller, finds in the same
 * image; then a file it loads is compared byte for byte with that reader's
 * copy. Two routes to the same bytes, only one of them through the chip.
 *
 * Needs the firmware and disc images: run `make roms` and `make discs`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpc.h"
#include "dsk.h"
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
static uint8_t amsdos[0x4000];
static uint8_t framebuffer[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT];
static cpc_t cpc;
static char screen[ROWS][COLUMNS + 1];
static const char *rom_directory = "roms";
static const char *disc_directory = "test/data/discs";

/* The disc: Shaker's, as fetched. A DATA-format disc holds 180K, so this
   is room enough for it and its headers. */
static uint8_t disc_image[256 * 1024];
static size_t disc_length;
static floppy_t disc;

static bool load_file(const char *directory, const char *file, uint8_t *into, size_t capacity,
                      size_t *length, const char *remedy) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", directory, file);
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    TEST_FAIL("cannot open %s — run '%s'", path, remedy);
    return false;
  }
  size_t read = fread(into, 1, capacity, handle);
  fclose(handle);
  if (length != NULL) {
    *length = read;
  } else if (read != capacity) {
    TEST_FAIL("%s holds %zu bytes, expected %zu", path, read, capacity);
    return false;
  }
  return true;
}

static bool load_rom(const char *file) {
  return load_file(rom_directory, file, rom, sizeof rom, NULL, "make roms");
}

static void run_frames(long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    cpc_tick(&cpc);
  }
}

/* The 664 and 6128 have the disc interface and its ROM built in; the 464
   has neither. */
static bool power_on(const char *file, uint32_t ram_size, bool fifty_hz, bool disc_interface) {
  memset(ram, 0, sizeof ram);
  memset(framebuffer, 0, sizeof framebuffer);
  if (!load_rom(file)) {
    return false;
  }
  if (disc_interface &&
      !load_file(rom_directory, "amsdos.rom", amsdos, sizeof amsdos, NULL, "make roms")) {
    return false;
  }
  cpc_init(&cpc, ram, ram_size, rom);
  cpc_set_upper_rom(&cpc, 0, rom + 0x4000);
  if (disc_interface) {
    cpc_fit_disc_interface(&cpc, true);
    cpc_set_upper_rom(&cpc, 7, amsdos);
  }
  cpc_connect_monitor(&cpc, framebuffer);
  cpc_set_links(&cpc, fifty_hz, CPC_MANUFACTURER_AMSTRAD);
  return true;
}

static bool insert_shaker(void) {
  if (!load_file(disc_directory, "shaker27.dsk", disc_image, sizeof disc_image, &disc_length,
                 "make discs")) {
    return false;
  }
  const char *problem = NULL;
  if (!dsk_read(&disc, disc_image, disc_length, &problem)) {
    TEST_FAIL("shaker27.dsk: %s", problem);
    return false;
  }
  cpc_insert_disc(&cpc, 0, &disc);
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

static void expect_screen_contains(const char *text) {
  for (int row = 0; row < ROWS; row++) {
    if (strstr(screen[row], text) != NULL) {
      return;
    }
  }
  TEST_FAIL("no row contains \"%s\"", text);
  print_screen();
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
  if (!power_on("cpc6128.rom", 0x20000, true, true)) {
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
  if (!power_on("cpc664.rom", 0x10000, true, true)) {
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
  if (!power_on("cpc464.rom", 0x10000, true, false)) {
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
  if (!power_on("cpc6128.rom", 0x20000, true, true)) {
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
  if (!power_on("cpc6128.rom", 0x20000, true, true)) {
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
  if (!power_on("cpc6128.rom", 0x20000, true, true)) {
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
  if (!power_on("cpc6128.rom", 0x20000, true, true)) {
    return;
  }
  run_frames(4);
  TEST_EQUAL(cpc.crtc.registers[4], 38);
  TEST_EQUAL(cpc.crtc.registers[5], 0);
  TEST_EQUAL(cpc.crtc.registers[7], 30);

  if (!power_on("cpc6128.rom", 0x20000, false, true)) {
    return;
  }
  run_frames(4);
  TEST_EQUAL(cpc.crtc.registers[4], 31);
  TEST_EQUAL(cpc.crtc.registers[5], 6);
  TEST_EQUAL(cpc.crtc.registers[7], 27);
}

/* The disc image read on its own terms — the extended layout's track table
   and sector lists, and AMSDOS's directory on track 0 — by a reader that
   knows nothing of floppies or controllers. What it finds is what the
   machine must find by the other route. */
static const uint8_t *image_sector(uint8_t track, uint8_t r) {
  size_t at = 0x100;
  for (uint8_t before = 0; before < track; before++) {
    at += (size_t)disc_image[0x34 + before] * 256;
  }
  const uint8_t *header = disc_image + at;
  size_t data = at + 0x100;
  for (uint8_t index = 0; index < header[0x15]; index++) {
    const uint8_t *entry = header + 0x18 + (size_t)index * 8;
    size_t stored = (size_t)(entry[6] | (entry[7] << 8));
    if (entry[2] == r) {
      return disc_image + data;
    }
    data += stored;
  }
  return NULL;
}

/* A DATA-format disc: nine 512-byte sectors a track numbered from &C1,
   1K blocks two sectors each, the directory in the first two blocks. */
static const uint8_t *image_block_half(uint8_t block, int half) {
  int logical = block * 2 + half;
  return image_sector((uint8_t)(logical / 9), (uint8_t)(0xC1 + logical % 9));
}

typedef struct {
  char name[13]; /* NAME.EXT as CAT prints it */
  uint16_t blocks;
  uint16_t load;
  uint16_t length;
  uint8_t contents[32 * 1024];
} catalogue_entry_t;

/* The directory: 64 entries of 32 bytes over four sectors, each an extent
   of a file naming up to sixteen blocks. Entries of one file are gathered
   by extent number; &E5 marks one never used. */
static size_t read_catalogue(catalogue_entry_t *entries, size_t capacity) {
  size_t count = 0;
  for (int extent = 0; extent < 64; extent++) {
    for (int slot = 0; slot < 64; slot++) {
      const uint8_t *entry =
          image_block_half((uint8_t)(slot / 32), (slot / 16) % 2) + (size_t)(slot % 16) * 32;
      if (entry[0] == 0xE5 || entry[12] != extent) {
        continue;
      }
      char name[13];
      int length = 0;
      for (int index = 1; index <= 8 && entry[index] != ' '; index++) {
        name[length++] = (char)(entry[index] & 0x7F);
      }
      name[length++] = '.';
      for (int index = 9; index <= 11 && (entry[index] & 0x7F) != ' '; index++) {
        name[length++] = (char)(entry[index] & 0x7F);
      }
      name[length] = '\0';
      catalogue_entry_t *file = NULL;
      for (size_t index = 0; index < count; index++) {
        if (strcmp(entries[index].name, name) == 0) {
          file = &entries[index];
        }
      }
      if (file == NULL) {
        if (count == capacity) {
          return count;
        }
        file = &entries[count++];
        memset(file, 0, sizeof *file);
        memcpy(file->name, name, sizeof file->name);
      }
      for (int index = 16; index < 32 && entry[index] != 0; index++) {
        size_t at = (size_t)file->blocks * 1024;
        if (at + 1024 <= sizeof file->contents) {
          memcpy(file->contents + at, image_block_half(entry[index], 0), 512);
          memcpy(file->contents + at + 512, image_block_half(entry[index], 1), 512);
        }
        file->blocks++;
      }
    }
  }
  /* The AMSDOS header, 128 bytes before the file itself: the load address
     at &15 and the length at &18. */
  for (size_t index = 0; index < count; index++) {
    const uint8_t *header = entries[index].contents;
    entries[index].load = (uint16_t)(header[0x15] | (header[0x16] << 8));
    entries[index].length = (uint16_t)(header[0x18] | (header[0x19] << 8));
  }
  return count;
}

/* AMSDOS catalogues the disc; the names and sizes it prints are the ones
   an independent reading of the image predicts, to the letter. */
static void amsdos_catalogues_the_disc(void) {
  if (!power_on("cpc6128.rom", 0x20000, true, true) || !insert_shaker()) {
    return;
  }
  static catalogue_entry_t entries[16];
  size_t count = read_catalogue(entries, 16);
  TEST_EQUAL(count, 5);
  run_frames(FRAMES_TO_PROMPT);
  type_text("CAT\n");
  run_frames(200);
  read_screen();
  expect_screen_contains("Drive A: user  0");
  uint16_t used = 0;
  for (size_t index = 0; index < count; index++) {
    char line[32];
    snprintf(line, sizeof line, "%-12.12s %3uK", entries[index].name, entries[index].blocks);
    expect_screen_contains(line);
    used = (uint16_t)(used + entries[index].blocks);
  }
  char free_line[32];
  snprintf(free_line, sizeof free_line, "%3uK free", 178 - used); /* 180K less the directory */
  expect_screen_contains(free_line);
  expect_screen_contains("Ready");
}

/* A file loaded through the controller, the ROM and BASIC is the same
   bytes the independent reading gives, at the address its header names. */
static void amsdos_loads_a_file_off_the_disc(void) {
  if (!power_on("cpc6128.rom", 0x20000, true, true) || !insert_shaker()) {
    return;
  }
  static catalogue_entry_t entries[16];
  size_t count = read_catalogue(entries, 16);
  const catalogue_entry_t *file = NULL;
  for (size_t index = 0; index < count; index++) {
    if (strcmp(entries[index].name, "SHAKE27A.BIN") == 0) {
      file = &entries[index];
    }
  }
  if (file == NULL) {
    TEST_FAIL("the image has no SHAKE27A.BIN");
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_text("MEMORY &3CFF\nLOAD\"SHAKE27A.BIN\"\n");
  run_frames(400);
  read_screen();
  expect_screen_contains("Ready");
  /* In the RAM itself: at the prompt the lower ROM lies over &0000-&3FFF
     and a peek through the map would read the firmware's font. */
  for (uint16_t offset = 0; offset < file->length; offset++) {
    if (ram[file->load + offset] != file->contents[128 + offset]) {
      TEST_FAIL("byte %u of the file differs at &%04X", offset, file->load + offset);
      print_screen();
      return;
    }
  }
  TEST_CHECK(!disc.modified);
}

/* A module run off the disc takes the machine over. Shaker's menu is drawn
   in the ROM's own characters, but in mode 2, where a character is eight
   samples across rather than mode 1's sixteen — so the reader here, which
   counts in sixteens, cannot spell it. The mode and the program counter say
   whose code is running; `shaker_test` reads the menu itself. */
static void a_shaker_module_runs_off_the_disc(void) {
  if (!power_on("cpc6128.rom", 0x20000, true, true) || !insert_shaker()) {
    return;
  }
  static catalogue_entry_t entries[16];
  size_t count = read_catalogue(entries, 16);
  const catalogue_entry_t *file = NULL;
  for (size_t index = 0; index < count; index++) {
    if (strcmp(entries[index].name, "SHAKE27A.BIN") == 0) {
      file = &entries[index];
    }
  }
  if (file == NULL) {
    TEST_FAIL("the image has no SHAKE27A.BIN");
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_text("RUN\"SHAKE27A\n");
  run_frames(400);
  TEST_EQUAL(cpc.gate_array.mode, 2);
  TEST_CHECK(cpc.cpu.pc >= file->load && cpc.cpu.pc < file->load + file->length);
}

/* A program saved through the ROM lands on the disc: the independent
   reading of the same bytes finds the file in the directory afterwards,
   and the ROM lists it. */
static void amsdos_saves_a_file_onto_the_disc(void) {
  if (!power_on("cpc6128.rom", 0x20000, true, true) || !insert_shaker()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_text("10 PRINT 1\nSAVE\"PROBE\"\n");
  run_frames(250);
  TEST_CHECK(disc.modified);
  static catalogue_entry_t entries[16];
  size_t count = read_catalogue(entries, 16);
  TEST_EQUAL(count, 6);
  bool found = false;
  for (size_t index = 0; index < count; index++) {
    if (strcmp(entries[index].name, "PROBE.BAS") == 0) {
      found = true;
      TEST_EQUAL(entries[index].blocks, 1);
    }
  }
  TEST_CHECK(found);
  type_text("CAT\n");
  run_frames(200);
  read_screen();
  expect_screen_contains("PROBE   .BAS   1K");
}

/* Without a disc, the ROM finds the drive not ready and says so. */
static void amsdos_reports_a_missing_disc(void) {
  if (!power_on("cpc6128.rom", 0x20000, true, true)) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_text("CAT\n");
  run_frames(150);
  read_screen();
  expect_screen_contains("Drive A: disc missing");
  expect_screen_contains("Retry, Ignore or Cancel?");
}

int main(int argc, char **argv) {
  if (argc > 1) {
    rom_directory = argv[1];
  }
  if (argc > 2) {
    disc_directory = argv[2];
  }
  TEST_RUN(the_6128_boots_to_its_prompt);
  TEST_RUN(the_664_boots_to_its_prompt);
  TEST_RUN(the_464_boots_to_its_prompt);
  TEST_RUN(basic_does_arithmetic_it_is_typed);
  TEST_RUN(every_character_types_as_itself);
  TEST_RUN(a_typed_line_can_change_the_border);
  TEST_RUN(the_refresh_link_chooses_the_crtc_table);
  TEST_RUN(amsdos_catalogues_the_disc);
  TEST_RUN(amsdos_loads_a_file_off_the_disc);
  TEST_RUN(a_shaker_module_runs_off_the_disc);
  TEST_RUN(amsdos_saves_a_file_onto_the_disc);
  TEST_RUN(amsdos_reports_a_missing_disc);
  return TEST_REPORT("firmware");
}
