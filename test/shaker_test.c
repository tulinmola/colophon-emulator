/*
 * shaker_test — run Longshot's Shaker modules and record what they display.
 *
 * Shaker is the CRTC acid test: five modules of menus, each menu key a
 * battery of tests that drive the 6845 and the Gate Array into the corners
 * the demoscene found, and show what the machine does beside what real
 * silicon did. It is judged by its author, not by us — which is the whole
 * reason to run it.
 *
 * This is the instrument, not yet the verdict. It boots the disc, walks
 * each module's own menu, presses every key that belongs to this machine's
 * CRTC type, and records what comes back: the screen read as text through
 * the character table the ROM carries, and the beam's path as a PNG. What
 * it cannot yet do is decide every screen, because Shaker says so
 * differently in each group — some print a value beside the value they
 * expected, some print a legend and leave the verdict to the picture, and
 * some ask the reader to watch a line flash. A group earns a rule here
 * the day its convention is read off its own output; until then it earns a
 * record.
 *
 * The screen is read where it lies rather than where it should lie. These
 * tests move the picture on purpose — that is frequently the thing being
 * tested — so the character grid is found in the frame each time. Text that
 * cannot be read is reported as such rather than repaired: an unreadable
 * legend is evidence about the picture, not a fault in the reader.
 *
 * Needs the firmware and disc images: run `make roms` and `make discs`.
 *
 * Sources:
 * - "Shaker" (Longshot / Logon System), https://shaker.logonsystem.eu/ —
 *   the suite itself, and the recordings from real machines of each CRTC
 *   type that its tests are written against. Technical information sourced
 *   from the "Amstrad CPC CRTC Compendium" by Longshot (CC BY-NC-ND).
 */
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* fork, waitpid and an anonymous shared mapping, so the five modules
   run at once. These are POSIX rather than C99; the C library declares them
   without being asked, and nothing in the build asks. */
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cpc.h"
#include "dsk.h"
#include "png.h"
#include "shaker_trace.h"
#include "test.h"

/* The modules draw in mode 2: 80 characters of 8 pixels across 25 rows of
   8 scanlines, one sample to the pixel. */
#define COLUMNS 80
#define ROWS 25
#define GLYPH_WIDTH 8
#define GLYPH_HEIGHT 8

/* The firmware's character table, eight bytes a code, which is how the ROM
   draws them and how we read them back. Shaker's own text uses it. */
#define FONT_IN_ROM 0x3800
#define FIRST_CODE 32
#define LAST_CODE 126

/* Where the picture sits when nothing has moved it. */
#define NOMINAL_LEFT 272
#define NOMINAL_TOP 70

/* The boot screen stops changing at frame 42; this waits well past it, as
   cpc_firmware_test does. The module then loads off the disc, which the slowest
   of the five finishes inside 400 frames. A key must be held long enough
   for the fifty-times-a-second scan to see it. */
#define FRAMES_TO_PROMPT 78
#define FRAMES_TO_LOAD_MODULE 400
#define FRAMES_KEY_HELD 5

/* A group is watched until it stops telling us anything new: a group of
   eight tests has finished inside a second of the machine's own time, and
   one of four hundred is still printing after ten, so a fixed watch is
   either too short for one or wasted on the other. A frame is 312 lines of
   64 characters, so fifty of them make a second.

   The quiet the watch waits for is longer than any a module has been seen
   to keep. Module B's interrupt-delay group thinks for a hundred frames
   before printing anything at all, and module A's HSYNC group turns its
   byte over on the same beat, so a threshold near either would end the
   watch in the middle of the test — and a group cut off says nothing about
   being cut off, which is why the standing records it separately.

   The cap is for the groups that never stop, which are the ones drawing
   something that moves. Module B's status-register check is quieter than
   any of those: it waits past two hundred frames before its first screen,
   and a threshold that suited the groups a type 0 runs cut it off. */
#define SAMPLE_STEP_FRAMES 5
#define MAX_SAMPLES_WITHOUT_NEWS 60
#define MAX_SAMPLES_PER_GROUP 300

/* Every path this file builds names a directory the sweep was pointed at,
   so the bound is the host's rather than ours. */
#define MAX_PATH_LENGTH 4096

#define MAX_GROUPS 32
#define MAX_CAPTURES 12

/* A key on the menu: one character, or a name like COPY. Nothing longer
   than RETURN appears in any of the five. */
#define MAX_KEY_LENGTH 6

static uint8_t ram[0x20000];
static uint8_t rom[0x8000];
static uint8_t amsdos[0x4000];
static uint8_t framebuffer[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT];
static uint8_t disc_image[256 * 1024];

static uint8_t pixels[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT * 3];
static floppy_t disc;

/* Locating the grid scores two hundred and eighty-nine candidates, each of
   them two thousand cells, so a cell is named some six hundred thousand
   times a sample. The eight bytes of a glyph read as one 64-bit number, so
   the table is indexed once and looked up rather than walked. Should two
   codes ever share a bitmap the lower keeps it, which is the answer walking
   the table gave; none of the three firmware ROMs holds such a pair. */
#define GLYPH_INDEX_SLOTS 256

static struct {
  uint64_t bitmap;
  char code;
} glyph_index[GLYPH_INDEX_SLOTS];

static uint64_t bitmap_of_glyph(const uint8_t glyph[8]) {
  uint64_t bitmap = 0;
  for (int scanline = 0; scanline < GLYPH_HEIGHT; scanline++) {
    bitmap = (bitmap << 8) | glyph[scanline];
  }
  return bitmap;
}

/* Most glyphs end on a blank scanline, so the low byte alone crowds the
   table into a few slots; the whole word folds into the byte instead. */
static size_t slot_of_bitmap(uint64_t bitmap) {
  uint64_t folded = bitmap ^ (bitmap >> 32);
  folded ^= folded >> 16;
  folded ^= folded >> 8;
  return (size_t)(folded % GLYPH_INDEX_SLOTS);
}

/* Open addressing, and ninety-five codes in 256 slots leave free ones, so
   a lookup for a bitmap the table does not hold always meets one and stops.
   A free slot is one whose code is still NUL, which no printable code is. */
static void build_glyph_index(void) {
  memset(glyph_index, 0, sizeof glyph_index);
  for (int code = FIRST_CODE; code <= LAST_CODE; code++) {
    uint64_t bitmap = bitmap_of_glyph(rom + FONT_IN_ROM + (size_t)code * 8);
    size_t slot = slot_of_bitmap(bitmap);
    while (glyph_index[slot].code != '\0' && glyph_index[slot].bitmap != bitmap) {
      slot = (slot + 1) % GLYPH_INDEX_SLOTS;
    }
    if (glyph_index[slot].code == '\0') {
      glyph_index[slot].bitmap = bitmap;
      glyph_index[slot].code = (char)code;
    }
  }
}

/* The machine is file-scope because the controller's pointers to its drives
   point into it: cpc_init sets fdc.drives[unit] to &cpc.drives[unit], so a
   copy of the structure holds addresses inside the original. That is what
   makes the save and restore below a round trip rather than a clone — a
   saved copy is not a usable machine, and the day this becomes a local or
   an array the controller would drive the wrong drive. */
static cpc_t cpc;

/* Which CRTC the machine under test is built with, named on the command
   line and a type 0 when it is not. Held here rather than read back from
   the machine, because the modules run in forked children and the
   scoreboard's head is written by the parent, which powers nothing on and
   would name a type it never built. */
static uint8_t crtc_type = 0;
static cpc_t saved_cpc;
static uint8_t saved_ram[sizeof ram];
static floppy_t saved_disc;

static char screen[ROWS][COLUMNS + 1];
static char previous_screen[ROWS][COLUMNS + 1];
/* The menu as it stood before a key was pressed. A group that never put
   anything else on the screen either did nothing or was watched too briefly
   to catch it, and the standing has to be able to say so: without it, a
   group cut short reads exactly like a group that ran and finished. */
static char menu_screen[ROWS][COLUMNS + 1];
static int grid_left = NOMINAL_LEFT;
static int grid_top = NOMINAL_TOP;
static const char *rom_directory = "roms";
static const char *disc_directory = "test/data/discs";
static const char *report_directory = "build/shaker";
static const char *scoreboard_on_record = "test/shaker-scoreboard-crtc0.txt";

typedef struct {
  char key[MAX_KEY_LENGTH + 1];
  char label[COLUMNS + 1];
  int declared_tests; /* what the label claims, 0 when it claims nothing */
  bool applies_to_this_crtc_type;
} group;

static group groups[MAX_GROUPS];
static int group_count;

/* The module records hold everything a run saw, including how the picture
   looked getting there — a screen count moves when a frame lands a sample
   earlier, and a record that moves for that reason cannot be compared. The
   scoreboard holds only what Shaker said, and it is gathered here before
   any of it is written so that the count can stand at the head of the file,
   where a reader wants it. */
#define MAX_SCOREBOARD 65536
static char scoreboard[MAX_SCOREBOARD];
static size_t scoreboard_length;
static bool scoreboard_overflowed;
/* The line a module run leaves for the sweep to print, rather than printing
   it itself, so that five at once still report module by module. It holds
   the report's path and the two counts beside it. */
#define MAX_MODULE_SUMMARY (MAX_PATH_LENGTH + 256)
static char module_summary[MAX_MODULE_SUMMARY];

static int total_groups_run;
static int total_groups_skipped;
static int total_groups_graded;
static int total_verdicts;
static int total_verdicts_wrong;

static void add_to_scoreboard(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  size_t room = sizeof scoreboard - scoreboard_length;
  int written = vsnprintf(scoreboard + scoreboard_length, room, format, arguments);
  va_end(arguments);
  if (written < 0 || (size_t)written >= room) {
    scoreboard_overflowed = true;
    return;
  }
  scoreboard_length += (size_t)written;
}

/* The key is padded to the width of the longest a menu names and the label
   to the width of the widest the reader can hold, so that the standings
   stand in a column a human reads down. */
static void write_scoreboard_line(const char *module, const group *entry, const char *standing) {
  char named[MAX_KEY_LENGTH + 8];
  snprintf(named, sizeof named, "%.1s (%.*s)", module, MAX_KEY_LENGTH, entry->key);
  add_to_scoreboard("%-11s %-*s %s\n", named, COLUMNS, entry->label, standing);
}

static bool load_file(const char *directory, const char *file, uint8_t *into, size_t capacity,
                      size_t *length, const char *remedy) {
  char path[MAX_PATH_LENGTH];
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

static void run_frames(long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    shaker_trace_tick(&cpc, cpc_tick(&cpc));
  }
}

static void press(keyboard_key key, bool shifted) {
  if (shifted) {
    keyboard_press(&cpc.keyboard, CPC_SHIFT);
  }
  keyboard_press(&cpc.keyboard, key);
  run_frames(FRAMES_KEY_HELD);
  keyboard_release_all(&cpc.keyboard);
  run_frames(FRAMES_KEY_HELD);
}

static void type_text(const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    bool shifted = false;
    keyboard_key key = *at == '\n' ? CPC_RETURN : cpc_key_for_character(*at, &shifted);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("this keyboard has no '%c'", *at);
      return;
    }
    press(key, shifted);
  }
}

/* The menus offer keys that carry no character, which is why
   cpc_key_for_character cannot reach them. Every key a menu names is
   either one character or one of these. */
static const struct {
  const char *name;
  keyboard_key key;
} named_keys[] = {
    {"COPY", CPC_COPY},    {"CAPS", CPC_CAPS_LOCK}, {"TAB", CPC_TAB},     {"RETURN", CPC_RETURN},
    {"CTRL", CPC_CONTROL}, {"F0", CPC_FUNCTION_0},  {"SPACE", CPC_SPACE},
};

static keyboard_key key_named(const char *name, bool *shifted) {
  *shifted = false;
  for (size_t index = 0; index < sizeof named_keys / sizeof named_keys[0]; index++) {
    if (strcmp(named_keys[index].name, name) == 0) {
      return named_keys[index].key;
    }
  }
  if (name[1] != '\0') {
    return KEYBOARD_NO_KEY;
  }
  char character = name[0];
  if (character >= 'A' && character <= 'Z') {
    character = (char)(character + ('a' - 'A'));
  }
  return cpc_key_for_character(character, shifted);
}

static bool power_on(void) {
  memset(ram, 0, sizeof ram);
  memset(framebuffer, 0, sizeof framebuffer);
  if (!load_file(rom_directory, "cpc6128.rom", rom, sizeof rom, NULL, "make roms") ||
      !load_file(rom_directory, "amsdos.rom", amsdos, sizeof amsdos, NULL, "make roms")) {
    return false;
  }
  size_t length = 0;
  if (!load_file(disc_directory, "shaker27.dsk", disc_image, sizeof disc_image, &length,
                 "make discs")) {
    return false;
  }
  const char *problem = NULL;
  if (!dsk_read(&disc, disc_image, length, &problem)) {
    TEST_FAIL("shaker27.dsk: %s", problem);
    return false;
  }
  build_glyph_index();
  cpc_init(&cpc, ram, sizeof ram, rom, crtc_type);
  cpc_set_upper_rom(&cpc, 0, rom + 0x4000);
  cpc_fit_disc_interface(&cpc, true);
  cpc_set_upper_rom(&cpc, 7, amsdos);
  cpc_connect_monitor(&cpc, framebuffer);
  cpc_set_links(&cpc, true, CPC_MANUFACTURER_AMSTRAD);
  cpc_insert_disc(&cpc, 0, &disc);
  return true;
}

static void save_machine(void) {
  saved_cpc = cpc;
  saved_disc = disc;
  memcpy(saved_ram, ram, sizeof ram);
}

/* The framebuffer is not restored: a frame of standard timing repaints the
   whole raster, and a group that does not is a group whose picture is the
   thing being recorded. */
static void restore_machine(void) {
  cpc = saved_cpc;
  disc = saved_disc;
  memcpy(ram, saved_ram, sizeof ram);
}

/* The colour standing behind the text: whatever most of the display area
   is, which survives a program choosing its own inks. */
static uint8_t paper_colour(int left, int top) {
  int counts[32] = {0};
  for (int row = 0; row < ROWS * GLYPH_HEIGHT; row++) {
    int y = top + row;
    if (y < 0 || y >= CPC_FRAMEBUFFER_HEIGHT) {
      continue;
    }
    const uint8_t *frame_row = framebuffer + (size_t)y * CPC_FRAMEBUFFER_WIDTH;
    for (int column = 0; column < COLUMNS * GLYPH_WIDTH; column += 4) {
      int x = left + column;
      if (x < 0 || x >= CPC_FRAMEBUFFER_WIDTH) {
        continue;
      }
      counts[frame_row[x] & 0x1F]++;
    }
  }
  int best = 0;
  for (int colour = 1; colour < 32; colour++) {
    if (counts[colour] > counts[best]) {
      best = colour;
    }
  }
  return (uint8_t)best;
}

/* The picture's own corner. Everything outside the display carries the
   border colour, so the first wide line that does not is where the beam
   began drawing. A frame that ended early leaves a stub of the one before
   it at the top of the raster, and a line narrow enough to be that stub is
   not the corner. */
static bool find_display_corner(int *corner_left, int *corner_top) {
  uint8_t border = cpc.gate_array.inks[16];
  const int narrowest_line = COLUMNS * GLYPH_WIDTH / 4;
  int found_left = CPC_FRAMEBUFFER_WIDTH;
  int found_top = -1;
  for (int y = 0; y < CPC_FRAMEBUFFER_HEIGHT; y++) {
    int first = -1;
    int drawn = 0;
    for (int x = 0; x < CPC_FRAMEBUFFER_WIDTH; x++) {
      if (framebuffer[(size_t)y * CPC_FRAMEBUFFER_WIDTH + (size_t)x] != border) {
        if (first < 0) {
          first = x;
        }
        drawn++;
      }
    }
    if (drawn < narrowest_line) {
      continue;
    }
    if (found_top < 0) {
      found_top = y;
    }
    if (first < found_left) {
      found_left = first;
    }
  }
  if (found_top < 0) {
    return false;
  }
  *corner_left = found_left;
  *corner_top = found_top;
  return true;
}

/* Two readings of the same cell. One lying wholly inside the frame needs
   no bounds test at all, and it is the loop a sweep spends most of itself
   in; a picture these tests have pushed off the edge takes the second,
   which asks of every pixel and reads what lies outside as paper. */
static uint64_t cut_bitmap(int left, int top, int row, int column, uint8_t behind) {
  int cell_left = left + column * GLYPH_WIDTH;
  int cell_top = top + row * GLYPH_HEIGHT;
  uint64_t bitmap = 0;
  if (cell_left >= 0 && cell_left + GLYPH_WIDTH <= CPC_FRAMEBUFFER_WIDTH && cell_top >= 0 &&
      cell_top + GLYPH_HEIGHT <= CPC_FRAMEBUFFER_HEIGHT) {
    const uint8_t *cell_row =
        framebuffer + (size_t)cell_top * CPC_FRAMEBUFFER_WIDTH + (size_t)cell_left;
    for (int scanline = 0; scanline < GLYPH_HEIGHT; scanline++) {
      uint8_t bits = 0;
      for (int pixel = 0; pixel < GLYPH_WIDTH; pixel++) {
        if (cell_row[pixel] != behind) {
          bits |= (uint8_t)(0x80u >> pixel);
        }
      }
      bitmap = (bitmap << 8) | bits;
      cell_row += CPC_FRAMEBUFFER_WIDTH;
    }
    return bitmap;
  }
  for (int scanline = 0; scanline < GLYPH_HEIGHT; scanline++) {
    uint8_t bits = 0;
    for (int pixel = 0; pixel < GLYPH_WIDTH; pixel++) {
      int x = cell_left + pixel;
      int y = cell_top + scanline;
      if (x < 0 || x >= CPC_FRAMEBUFFER_WIDTH || y < 0 || y >= CPC_FRAMEBUFFER_HEIGHT) {
        continue;
      }
      if (framebuffer[(size_t)y * CPC_FRAMEBUFFER_WIDTH + (size_t)x] != behind) {
        bits |= (uint8_t)(0x80u >> pixel);
      }
    }
    bitmap = (bitmap << 8) | bits;
  }
  return bitmap;
}

static char code_of_bitmap(uint64_t bitmap) {
  size_t slot = slot_of_bitmap(bitmap);
  while (glyph_index[slot].code != '\0') {
    if (glyph_index[slot].bitmap == bitmap) {
      return glyph_index[slot].code;
    }
    slot = (slot + 1) % GLYPH_INDEX_SLOTS;
  }
  return '?';
}

/* Codes drawn from one row of pixels. A grid laid over a pattern of
   horizontal stripes — which these tests paint by the screenful — names
   every cell one of these, and would otherwise outscore the true text. */
static bool code_is_a_single_rule(char code) { return strchr("_-.,'`~", code) != NULL; }

/* A grid that is off by a single pixel turns every glyph into something the
   table does not hold, so what a candidate can name is sharp enough to find
   the picture with. */
static void score_grid(int left, int top, int *glyphs_named, int *glyphs_drawn) {
  uint8_t behind = paper_colour(left, top);
  *glyphs_named = 0;
  *glyphs_drawn = 0;
  for (int row = 0; row < ROWS; row++) {
    for (int column = 0; column < COLUMNS; column++) {
      uint64_t bitmap = cut_bitmap(left, top, row, column, behind);
      if (bitmap == 0) {
        continue;
      }
      (*glyphs_drawn)++;
      char code = code_of_bitmap(bitmap);
      if (code != '?' && !code_is_a_single_rule(code)) {
        (*glyphs_named)++;
      }
    }
  }
}

/* A screen with nothing drawn on it has no proportion; -1 says so, where a
   hundred would claim a grid that was never tested. */
#define NOTHING_DRAWN (-1)
#define ENOUGH_TO_JUDGE 40

static int proportion_named(int glyphs_named, int glyphs_drawn) {
  if (glyphs_drawn < ENOUGH_TO_JUDGE) {
    return NOTHING_DRAWN;
  }
  return glyphs_named * 100 / glyphs_drawn;
}

/* Find the picture. These tests move it on purpose, sometimes by a hundred
   scanlines, so the corner is found in the frame and the phase within the
   character settled by trying the neighbourhood. The grid last used is kept
   while it still reads. */
static void locate_grid(void) {
  int glyphs_named = 0;
  int glyphs_drawn = 0;
  score_grid(grid_left, grid_top, &glyphs_named, &glyphs_drawn);
  if (proportion_named(glyphs_named, glyphs_drawn) >= 90) {
    return;
  }
  int corner_left = NOMINAL_LEFT;
  int corner_top = NOMINAL_TOP;
  if (!find_display_corner(&corner_left, &corner_top)) {
    return;
  }
  /* A grid shifted by one character names almost as much as the true one,
     the first column falling off one edge and a partial character arriving
     at the other. What separates them is the cell it cannot read, so an
     unnamed glyph counts against a candidate as much as a named one counts
     for it. */
  int best = 2 * glyphs_named - glyphs_drawn;
  int best_left = grid_left;
  int best_top = grid_top;
  for (int top = corner_top - GLYPH_HEIGHT; top <= corner_top + GLYPH_HEIGHT; top++) {
    for (int left = corner_left - GLYPH_WIDTH; left <= corner_left + GLYPH_WIDTH; left++) {
      score_grid(left, top, &glyphs_named, &glyphs_drawn);
      if (2 * glyphs_named - glyphs_drawn > best) {
        best = 2 * glyphs_named - glyphs_drawn;
        best_left = left;
        best_top = top;
      }
    }
  }
  grid_left = best_left;
  grid_top = best_top;
}

/* Returns the percentage of drawn glyphs the table could name, or
   NOTHING_DRAWN when there was too little on the screen to judge. */
static int read_screen(void) {
  memcpy(previous_screen, screen, sizeof screen);
  locate_grid();
  uint8_t behind = paper_colour(grid_left, grid_top);
  int glyphs_drawn = 0;
  int glyphs_named = 0;
  for (int row = 0; row < ROWS; row++) {
    for (int column = 0; column < COLUMNS; column++) {
      uint64_t bitmap = cut_bitmap(grid_left, grid_top, row, column, behind);
      char found = bitmap == 0 ? ' ' : code_of_bitmap(bitmap);
      if (bitmap != 0) {
        glyphs_drawn++;
        if (found != '?') {
          glyphs_named++;
        }
      }
      screen[row][column] = found;
    }
    screen[row][COLUMNS] = '\0';
  }
  return proportion_named(glyphs_named, glyphs_drawn);
}

static bool write_raster(const char *path) {
  for (size_t index = 0; index < (size_t)CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT; index++) {
    uint32_t rgb = gate_array_rgb(framebuffer[index]);
    pixels[index * 3] = (uint8_t)(rgb >> 16);
    pixels[index * 3 + 1] = (uint8_t)(rgb >> 8);
    pixels[index * 3 + 2] = (uint8_t)rgb;
  }
  return png_write(path, pixels, CPC_FRAMEBUFFER_WIDTH, CPC_FRAMEBUFFER_HEIGHT);
}

static void trim_trailing_newline(char *text) {
  size_t length = strlen(text);
  while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
    text[--length] = '\0';
  }
}

static void trim_trailing_spaces(char *text) {
  size_t length = strlen(text);
  while (length > 0 && text[length - 1] == ' ') {
    text[--length] = '\0';
  }
}

static bool begins_with(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* A list of types is written as numbers with any of . + , - / between them:
   "CRTC 0.1.2", "CRTC 3+4", "CRTC 3/4". Each is read whole, so a 10 names
   neither a 1 nor a 0. The list ends at the first character that is neither
   a digit nor a separator, because Shaker's prose is full of register names
   that would otherwise lend a list a digit it never meant — "C0io=#00"
   stands in a line of this very test. A number past the types that exist is
   held at one rather than wrapped into one that does. */
static bool names_the_crtc_type(const char *from, const char *to, uint8_t type) {
  const char *at = from;
  while (at < to) {
    if (*at == '.' || *at == '+' || *at == ',' || *at == '-' || *at == '/') {
      at++;
      continue;
    }
    if (!isdigit((unsigned char)*at)) {
      return false;
    }
    unsigned named = 0;
    while (at < to && isdigit((unsigned char)*at)) {
      named = named < 256 ? named * 10 + (unsigned)(*at - '0') : 256;
      at++;
    }
    if (named == type) {
      return true;
    }
  }
  return false;
}

/* Which CRTC type a group belongs to is stated at the head of its label and
   nowhere else: "CRTC 2 RVMB" is type 2's, "CRTC 0.2" is shared, "ALL" is
   everyone's. A type named later in a label is a remark about the test, not
   its scope — "ALL : CRTC 3/4 PARITY" belongs to every type, and "SHAKER
   KILLER 2 (WARNING : NOT RELIABLE ON CRTC 1)" is a caution. The one label
   that overrides its own head is the one that says so: "OPEN TO OTHER
   CRTC'S", which Shaker prints where a type's test has been found to hold
   for the rest. A label writes its list the way a bracket's clause does. */
static bool applies_to_crtc_type(const char *label, uint8_t type) {
  if (strstr(label, "OPEN TO OTHER") != NULL) {
    return true;
  }
  const char *at = label;
  while (*at == ' ') {
    at++;
  }
  if (!begins_with(at, "CRTC")) {
    return true;
  }
  at += 4;
  while (*at == ' ') {
    at++;
  }
  if (!isdigit((unsigned char)*at)) {
    return true;
  }
  return names_the_crtc_type(at, at + strlen(at), type);
}

/* The whole bracket has to match: a label can carry "(2.1.0)" beside its
   count, and a conversion that stopped where the digits ran out would read
   that as a number of tests. */
static int declared_test_count(const char *label) {
  const char *at = label;
  while ((at = strchr(at, '(')) != NULL) {
    char *after_digits = NULL;
    long count = strtol(at + 1, &after_digits, 10);
    if (after_digits != at + 1 && count >= 0 && count <= INT_MAX &&
        (begins_with(after_digits, " TST)") || begins_with(after_digits, " INTERACTIVE TST)"))) {
      return (int)count;
    }
    at++;
  }
  return 0;
}

/* A menu key between its brackets is one character, or one of the names
   above. Nothing else is one, which is what tells a key from the "(4 TST)"
   and "(R0=3)" a label also carries — and from the "(06)" module B prints
   in its first line, where the menu reads "(xx)" and the module fills in
   the value R9 stands at. */
static bool key_at(const char *line, size_t *length) {
  if (*line != '(') {
    return false;
  }
  const char *close = strchr(line, ')');
  if (close == NULL) {
    return false;
  }
  size_t between = (size_t)(close - line) - 1;
  if (between > MAX_KEY_LENGTH) {
    return false;
  }
  if (between == 1) {
    if ((line[1] >= 'A' && line[1] <= 'Z') || (line[1] >= '0' && line[1] <= '9')) {
      *length = between + 1;
      return true;
    }
    return false;
  }
  for (size_t index = 0; index < sizeof named_keys / sizeof named_keys[0]; index++) {
    const char *name = named_keys[index].name;
    if (strlen(name) == between && strncmp(line + 1, name, between) == 0) {
      *length = between + 1;
      return true;
    }
  }
  return false;
}

static void add_group(const char *key, size_t key_length, const char *label, size_t label_length) {
  if (group_count == MAX_GROUPS) {
    return;
  }
  group *entry = &groups[group_count];
  memcpy(entry->key, key, key_length);
  entry->key[key_length] = '\0';
  if (label_length > COLUMNS) {
    label_length = COLUMNS;
  }
  memcpy(entry->label, label, label_length);
  entry->label[label_length] = '\0';
  trim_trailing_spaces(entry->label);
  const char *from = entry->label;
  while (*from == ' ') {
    from++;
  }
  memmove(entry->label, from, strlen(from) + 1);
  entry->declared_tests = declared_test_count(entry->label);
  entry->applies_to_this_crtc_type = applies_to_crtc_type(entry->label, crtc_type);
  group_count++;
}

/* The module's own menu is the list of what it offers: every line that
   opens with a key in brackets. Reading it rather than holding a copy here
   means a later Shaker is walked as it stands. A line can carry two groups
   — module B offers "(CTRL) R5 SCANNER / (COPY) R5 T2" — so each is cut at
   the next key rather than at the end of the line. */
static void read_menu(void) {
  group_count = 0;
  for (int row = 0; row < ROWS && group_count < MAX_GROUPS; row++) {
    size_t key_length = 0;
    if (!key_at(screen[row], &key_length)) {
      continue;
    }
    const char *line = screen[row];
    size_t at = 0;
    while (line[at] != '\0' && group_count < MAX_GROUPS) {
      if (!key_at(line + at, &key_length)) {
        at++;
        continue;
      }
      size_t label_from = at + key_length + 1;
      size_t label_to = label_from;
      size_t ignored = 0;
      while (line[label_to] != '\0' && !key_at(line + label_to, &ignored)) {
        label_to++;
      }
      add_group(line + at + 1, key_length - 1, line + label_from, label_to - label_from);
      at = label_to;
    }
  }
}

/* Every distinct screen a group put up. What the group is saying is
   somewhere in this set, and which part of it is the verdict is the
   question each group answers differently. */
typedef struct {
  char text[ROWS][COLUMNS + 1];
  int percentage_named;
  long frame;
  int seen;
  char raster_file[64];
} capture;

static capture captures[MAX_CAPTURES];
static int capture_count;
static int captures_dropped;

/* A beam path is a megabyte, and a sweep of every module would be a
   thousand of them, so they are written only when a group is asked for by
   name. */
static bool keep_rasters;

/* Some groups grade themselves. Most of them keep both values on the one
   line: the value the machine produced stands last before a bracket and the
   value real silicon produced stands inside it. Longshot writes that six
   ways:

       >>>>>> DELAY TO VSYNC:#0030 (EXP:#00F7)  WRONG
       RESULT:#8700 WRONG (EXP:#4E40)
       R5 PREV=20. ON C4=R4=#26/C9=R9=7/C0io=#00, R5=0, CPU TO C4=0:#0084 (exp:#0004)
       R5=1 / ON 1ST ADD LINE, R5=0 / CPU TO NEW FRAME:#0080 (#0080 expected)
       R7=0/VSYNC/R4=0 VSIZE=#0044 (CRTC 0.1.2:#44 / 3.4:#FFFF=DEADLOCK)
       2B         :#CC >WRONG (Exp #C4)

   Three of them never write WRONG at all, so the values decide and the word
   only corroborates. They are compared as numbers because #0032 and #32
   are one measurement written two ways.

   A value before the bracket is what tells a grading from a legend naming
   the value a test is about to check, which carries no measurement of its
   own. A line of that second shape is not always idle: where its bracket
   names silicon's value, it is the title one group writes over the rows it
   is about to measure, and the reader for that is further down.

   A rendering seen and not yet read: "(#40 0/16 or #44)" and "(C2/C2 or
   C5/C5 or C2/C5)" in module D's (I), where several answers are allowed and
   one of them is a pair. Both stand right today, so reading them would add
   a denominator and no knowledge, and misreading them would cost both. */
#define MAX_VERDICTS 192
typedef struct {
  char text[COLUMNS + 1];
  bool failed;
} verdict;

static verdict verdicts[MAX_VERDICTS];
static int verdict_count;
static int verdicts_dropped;

/* Longshot prints a measurement in four hex digits at most, and one more is
   allowed here. A longer run is something else: the modules print
   #0D0E0F10, four values run together, which is not one measurement. The
   digits are read one at a time rather than handed to strtoul, which takes
   a 0x prefix straight past any count of them and saturates — and two
   values that saturate agree with each other. */
#define MAX_VALUE_DIGITS 5

static bool hex_digits_at(const char *at, unsigned long *value) {
  unsigned long parsed = 0;
  int digits = 0;
  while (isxdigit((unsigned char)at[digits])) {
    char digit = at[digits];
    parsed = parsed * 16 +
             (unsigned long)(digit <= '9' ? digit - '0' : tolower((unsigned char)digit) - 'a' + 10);
    if (++digits > MAX_VALUE_DIGITS) {
      return false;
    }
  }
  if (digits == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool hex_value_at(const char *at, unsigned long *value) {
  return *at == '#' && hex_digits_at(at + 1, value);
}

static bool matches_ignoring_case(const char *at, const char *lowercase, size_t length) {
  for (size_t index = 0; index < length; index++) {
    if (tolower((unsigned char)at[index]) != lowercase[index]) {
      return false;
    }
  }
  return true;
}

/* The word, then the value: with a colon between them as modules C, D and
   E write it, spelt out as module C's (E) does, or with neither as modules
   B and D do — `(Exp #C4)`, `(Exp#00)`. The value has to be there: what
   keeps a title out is the rule above, that a line carrying no
   measurement of its own is not a grading here, which is what leaves module
   D's `TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)` to the
   reader below that takes it for the title it is, and grades the rows that
   follow it against the value it carries.

   Module B's (R) is a two-column table whose rows carry two tests each, and
   a row of it grades the test whose value stands nearest the bracket while
   the other goes unread. That costs nothing today, because only a failing
   test names a value for silicon there and no row holds two of those; what
   takes its forty-eight tests down to the five recorded is Shaker's silence
   on the ones that pass. The word WRONG is looked for in the whole line
   though, so a row that ever carried both a bracket and another column's
   failure would condemn the test it graded. No such row is printed. */
static bool names_the_expected_value(const char *opening, const char *closing) {
  for (const char *at = opening; at + 3 < closing; at++) {
    if (!matches_ignoring_case(at, "exp", 3)) {
      continue;
    }
    if (at[3] == ':') {
      return true;
    }
    if (closing - at >= 8 && matches_ignoring_case(at + 3, "ected", 5)) {
      return true;
    }
    const char *value = at + 3;
    while (value < closing && *value == ' ') {
      value++;
    }
    if (value < closing && *value == '#') {
      return true;
    }
  }
  return false;
}

static bool holds_text(const char *from, const char *to, const char *lowercase) {
  size_t length = strlen(lowercase);
  for (const char *at = from; at + length <= to; at++) {
    if (matches_ignoring_case(at, lowercase, length)) {
      return true;
    }
  }
  return false;
}

/* A clause names the types it speaks for behind the word CRTC. One naming
   none at all is not this machine's unless it gathers the rest. */
static bool clause_speaks_for_crtc_type(const char *from, const char *to, uint8_t type) {
  const char *at = from;
  while (at + 4 <= to && !matches_ignoring_case(at, "crtc", 4)) {
    at++;
  }
  if (at + 4 > to) {
    return false;
  }
  for (at += 4; at < to && *at == ' '; at++) {
  }
  return names_the_crtc_type(at, to, type);
}

/* The value a clause names, wearing the # of a measurement or going
   without, and set off by spaces or not. It has to end where its digits
   end, or a word of hex letters standing where a value belongs would be
   read as a number — DEADLOCK as #DEAD. Shaker writes that word after a
   type list and not after a colon, so this guards a shape it has not
   printed; the clause it does print, "3.4:#FFFF=DEADLOCK", stops at the
   equals whether the rule is here or not. */
static bool clause_value(const char *at, const char *end, unsigned long *value) {
  while (at < end && *at == ' ') {
    at++;
  }
  if (at < end && *at == '#') {
    at++;
  }
  const char *after = at;
  while (after < end && isxdigit((unsigned char)*after)) {
    after++;
  }
  if (after == at || (after < end && isalnum((unsigned char)*after))) {
    return false;
  }
  return hex_digits_at(at, value);
}

/* Some groups name silicon's value for each CRTC type rather than for the
   machine in front of them: "(CRTC 0.3.4:3F2/CRTC 1.2:1F4)", "(CRTC 3+4:#58/
   OTHERS:#59)". Slashes separate the clauses, each naming the types it
   speaks for and then their value. The clause to read is the one naming the
   type this machine was built as, or failing that the one gathering the
   rest — and a clause that names it and then no value this reader can read
   takes the whole bracket down with it, rather than letting the rest be
   answered in its place.

   Naming a type is what tells this rendering from a legend keyed by value,
   "(00:C4ovf 01:C4=0)" or "(01:IO>=5TH NOP / 00:IO ON 4TH NOP)": a clause
   claims this machine by the word CRTC and the digits behind it, or by
   gathering the rest, so a legend's 00 claims nothing on any type. */
static bool value_for_this_crtc_type(const char *opening, const char *closing, uint8_t type,
                                     unsigned long *value) {
  bool found = false;
  for (const char *clause = opening + 1; clause < closing;) {
    const char *end = clause;
    while (end < closing && *end != '/') {
      end++;
    }
    const char *colon = clause;
    while (colon < end && *colon != ':') {
      colon++;
    }
    if (colon < end) {
      unsigned long named;
      bool readable = clause_value(colon + 1, end, &named);
      if (clause_speaks_for_crtc_type(clause, colon, type)) {
        if (!readable) {
          return false;
        }
        *value = named;
        return true;
      }
      if (readable && !found && holds_text(clause, colon, "others")) {
        *value = named;
        found = true;
      }
    }
    clause = end + 1;
  }
  return found;
}

static bool last_hex_value_before(const char *from, const char *to, unsigned long *value) {
  bool found = false;
  for (const char *at = from; at < to; at++) {
    unsigned long parsed;
    if (hex_value_at(at, &parsed)) {
      *value = parsed;
      found = true;
    }
  }
  return found;
}

static bool first_hex_value_between(const char *from, const char *to, unsigned long *value) {
  for (const char *at = from; at < to; at++) {
    if (hex_value_at(at, value)) {
      return true;
    }
  }
  return false;
}

/* A verdict a group states in numbers: its own value beside silicon's, in a
   bracket this reader knows how to pair with the value standing before it. */
static bool read_a_measured_verdict(const char *line, uint8_t type, bool *failed) {
  for (const char *opening = strchr(line, '('); opening != NULL;
       opening = strchr(opening + 1, '(')) {
    const char *closing = strchr(opening, ')');
    if (closing == NULL) {
      return false;
    }
    unsigned long expected;
    if (names_the_expected_value(opening, closing)) {
      /* A bracket naming silicon's value but carrying no number is not a
         grading this reader knows, and reading past it would pair a later
         bracket with a number standing inside this one. */
      if (!first_hex_value_between(opening, closing, &expected)) {
        return false;
      }
    } else if (!value_for_this_crtc_type(opening, closing, type, &expected)) {
      continue;
    }
    unsigned long produced;
    if (!last_hex_value_before(line, opening, &produced)) {
      return false;
    }
    *failed = produced != expected || strstr(line, "WRONG") != NULL;
    return true;
  }
  return false;
}

/* An expectation a group puts on a title rather than on the line it grades.
   Module D's (R) prints "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK
   (EXP #5F)" and then one line for each standing it puts the chip in, each
   ending in the value that standing produced. A verdict of the group's own
   names silicon's value the same way, so what tells the two apart is the
   measurement a verdict carries before the bracket and a title does not —
   and the reading is asked only of lines the readers above left ungraded,
   which is where that difference has already been made. A bracket naming
   the word and no value is no title: the group would have nothing to grade
   its rows against. No other group on either record is graded this way, and
   a second one would arrive there as lines the record has never held. */
static bool read_a_governing_expectation(const char *line, unsigned long *expected) {
  const char *opening = strchr(line, '(');
  const char *closing = opening == NULL ? NULL : strchr(opening, ')');
  return closing != NULL && names_the_expected_value(opening, closing) &&
         first_hex_value_between(opening, closing, expected);
}

/* And a line that title governs: a value at its end and no bracket of its
   own to be paired with. The shape is not rare — module C's (O) prints one
   every other row — and what keeps this reading off those is the word EXP in
   the title above, and nothing else. So the run is held narrowly: it ends
   at the first line that is not one of its own, whatever else that line is,
   and at the screen's end, and it is read afresh for every screen. */
static bool read_a_governed_measurement(const char *line, unsigned long *produced) {
  return strchr(line, '(') == NULL && last_hex_value_before(line, line + strlen(line), produced);
}

/* Whether a line ends with the given word, the screens being padded with
   spaces to their full width. */
static bool ends_with(const char *line, const char *word) {
  size_t length = strlen(line);
  while (length > 0 && line[length - 1] == ' ') {
    length--;
  }
  size_t wanted = strlen(word);
  return length >= wanted && memcmp(line + length - wanted, word, wanted) == 0;
}

/* And a verdict a group states in a word, having made the comparison itself
   and kept silicon's value to itself: "TEST C: 5F,5F: WRONG!", "OUTI ON
   C0=0,R0=0, RES: #1F :WRONG!", "C4=>00 IF 1:0 WRONG", and against them the
   ":GOOD" the same group prints where the machine answered. The word is
   read only at the end of a line and only where no bracket was found, so
   that a line which does name a value is paired with it as before — ">WRONG
   (Exp #C4)" carries the word in the middle, and "(EXP:#00F7)  WRONG" at
   the end, and both are graded on their numbers.

   Trusting the word is not what trusting a bracket is: nothing here can
   check it, and a group that says only this contributes to the count only
   while it disagrees. Longshot's word is still better evidence than our
   own, which is why it is taken at all. */
static bool read_a_declared_verdict(const char *line, bool *failed) {
  if (ends_with(line, "WRONG") || ends_with(line, "WRONG!")) {
    *failed = true;
    return true;
  }
  if (ends_with(line, ":GOOD")) {
    *failed = false;
    return true;
  }
  return false;
}

/* And a verdict a group speaks in a sentence, having graded itself and put
   the answer on the screen in words rather than in a value beside silicon's.
   Module E's (2) settles on "YOU'VE WON THIS STAGE (UNLESS A PROBLEM IS
   INDICATED ON THIS PAGE)" where this machine is right about the offset a
   frame's padding carries, and on "IF YOU CAN READ THIS...YOUR EMULATOR HAS
   A PROBLEM" where it is not. Which of the two a machine settles on is the
   verdict; a machine that passes does show the second, while the page is
   still being drawn, for about seven frames. What it never does is settle
   with both standing, which is the whole of the reading: the offset carries
   one of them out of the window and leaves the other. Measured both ways,
   with the rule that group grades in and put back.

   As with the other verdicts a group states in words, nothing here can check
   them; Longshot's sentence is still better evidence than our own. */
static bool read_a_spoken_verdict(const char *line, bool *failed) {
  if (strstr(line, "YOUR EMULATOR HAS A PROBLEM") != NULL) {
    *failed = true;
    return true;
  }
  if (strstr(line, "YOU'VE WON THIS STAGE") != NULL) {
    *failed = false;
    return true;
  }
  return false;
}

/* Whether a bracket on the line hands the passing outcome to named CRTC
   types, and if it does, whether this machine is one of them. "(OK FOR CRT
   3+4 ONLY)" is an OK a type 0 was never going to have. The disc writes both
   "CRT" and "CRTC" for the same thing. */
static bool the_ok_is_handed_to_types(const char *line, uint8_t type, bool *ours) {
  for (const char *opening = strchr(line, '('); opening != NULL;
       opening = strchr(opening + 1, '(')) {
    const char *closing = strchr(opening, ')');
    if (closing == NULL) {
      return false;
    }
    if (!holds_text(opening, closing, "ok for")) {
      continue;
    }
    for (const char *at = opening; at + 3 <= closing; at++) {
      if (!matches_ignoring_case(at, "crt", 3)) {
        continue;
      }
      const char *numbers = at + 3;
      if (numbers < closing && (*numbers == 'c' || *numbers == 'C')) {
        numbers++;
      }
      while (numbers < closing && *numbers == ' ') {
        numbers++;
      }
      *ours = names_the_crtc_type(numbers, closing, type);
      return true;
    }
  }
  return false;
}

/* And a verdict a group states in a word of its own. Shaker rings a failure
   with x's, a line ending ":xKOx", where it prints a bare "KO" for an
   outcome it is only naming, as module A's group 4 does in saying which of
   two ways its counter went. The x's are what keep this reading off those.

   The ring is read only where a bracket says which CRTC types the passing
   outcome belongs to. The group that prints these grades some of its lines
   for one type and some for another, and says so in that bracket where it
   says so at all: "UPD R9=1 WHEN C9=3>>C9=0 (OK FOR CRT 3+4 ONLY)" is a
   ring this machine was always going to earn. A ring with no bracket names
   no type, and this reader has no way to learn which machine it was written
   for — module C's group R grades the same question for a type 0 and
   answers it the other way, so taking an unnamed ring for this machine's
   own would book a right answer as a fault.

   The bare ":OK" beside them is not read either. The same group prints it
   where the machine answered — "UPD R4=0 WHEN C4=1 & C9=7 >> C4=2 (Ovf)
   :OK" — and group 4 prints the same bare word for the branch its counter
   took, so the word alone cannot be told apart by its shape. */
static bool read_a_marked_failure(const char *line, uint8_t type, bool *failed) {
  bool ours = false;
  if (!ends_with(line, ":xKOx") || !the_ok_is_handed_to_types(line, type, &ours)) {
    return false;
  }
  *failed = ours;
  return true;
}

static bool read_verdict(const char *line, uint8_t type, bool *failed) {
  return read_a_measured_verdict(line, type, failed) || read_a_marked_failure(line, type, failed) ||
         read_a_declared_verdict(line, failed) || read_a_spoken_verdict(line, failed);
}

static bool appears_in_previous_screen(const char *line) {
  for (int row = 0; row < ROWS; row++) {
    char earlier[COLUMNS + 1];
    memcpy(earlier, previous_screen[row], sizeof earlier);
    trim_trailing_spaces(earlier);
    const char *from = earlier;
    if (*from == '?') {
      from++;
    }
    while (*from == ' ') {
      from++;
    }
    if (strcmp(from, line) == 0) {
      return true;
    }
  }
  return false;
}

/* A group that runs more tests than the screen holds scrolls the early ones
   away, so verdicts are collected as they pass. A line is taken only once
   it has stood still for a sample: a screen read while the module was
   repainting splices the left of one line onto the right of another, and
   such a line would otherwise be recorded as a grading of its own. A line
   carrying a glyph the table could not name is refused outright — the word
   WRONG can be lost to a bad read where the brackets survive, and a
   half-read failure must never be counted as agreement. */
/* A page carrying both of the sentences above is one still being drawn, and
   neither of them is its verdict. The line that stands still for a sample is
   what this reader records, and the pair very nearly clears that bar: it
   stands some seven frames and dies two before the next sample falls, which
   is a margin and not a rule — a module settling three frames later would
   hand a correct machine a failure. So the pair is refused where it stands
   rather than left to the sampler's phase. */
static bool the_page_speaks_both_verdicts(void) {
  bool won = false;
  bool problem = false;
  for (int row = 0; row < ROWS; row++) {
    won = won || strstr(screen[row], "YOU'VE WON THIS STAGE") != NULL;
    problem = problem || strstr(screen[row], "YOUR EMULATOR HAS A PROBLEM") != NULL;
  }
  return won && problem;
}

static int collect_verdicts(int percentage_named) {
  /* What this screen carries, in the order Shaker wrote it. */
  verdict standing[ROWS];
  int standing_count = 0;
  const bool still_drawing = the_page_speaks_both_verdicts();
  unsigned long governing = 0;
  bool governs = false;
  for (int row = 0; row < ROWS; row++) {
    char line[COLUMNS + 1];
    const char *from = screen[row];
    /* The display's own left edge can fall inside a character, leaving one
       cell at the head of every row that no table can name. That is the
       picture's margin, not the module's text; a glyph lost anywhere after
       it is the text, and refuses the line below. */
    if (*from == '?') {
      from++;
    }
    while (*from == ' ') {
      from++;
    }
    size_t length = strlen(from);
    if (length > COLUMNS) {
      length = COLUMNS;
    }
    memcpy(line, from, length);
    line[length] = '\0';
    trim_trailing_spaces(line);
    bool failed = false;
    bool graded = read_verdict(line, crtc_type, &failed);
    const bool torn = strchr(line, '?') != NULL;
    unsigned long expected = 0;
    if (!graded && !torn && read_a_governing_expectation(line, &expected)) {
      governing = expected;
      governs = true;
      continue;
    }
    unsigned long produced = 0;
    if (governs && !graded && !torn && read_a_governed_measurement(line, &produced)) {
      failed = produced != governing;
      graded = true;
    } else {
      /* A title the reader took never reaches here: a second one takes the
         first one's place above, and the rows under it answer to the new
         expectation. */
      governs = false;
    }
    if (!graded || torn) {
      continue;
    }
    bool spoken = false;
    if (still_drawing && read_a_spoken_verdict(line, &spoken)) {
      continue;
    }
    if (!appears_in_previous_screen(line)) {
      continue;
    }
    memcpy(standing[standing_count].text, line, strlen(line) + 1);
    standing[standing_count].failed = failed;
    standing_count++;
  }

  /* Two of a group's tests can agree in the same words, and the record owes
     both a line — module E's (6) prints eleven and two of them read the
     same the day the machine gets them right. So a line counts as held not
     when the record holds one like it, but when it holds as many as this
     screen shows down to here. Shaker writes its verdicts downward, so that
     count is an identity where the text alone is not.

     It is an identity only where the whole screen stands behind it. A group
     that redraws its list from the top leaves the tail of the last pass
     above the head of the next one until the beam has covered it, and a
     frame caught in between shows the same test twice with unread rows in
     the gap. Such a frame may still add a line the record has never held —
     that is how a group's later tests are gathered at all — but it may not
     add a second copy of one it holds.

     A whole read is a filter and not a proof: a tear whose seam fell on a
     row boundary, or one a module had blanked rather than left dirty, would
     leave every glyph readable and be believed. And the filter costs
     something today. Module E's (6) prints its second identical verdict on
     partly-read screens too, where it is refused, and is recorded only
     because the group settles on a screen read whole; a group whose screen
     carries an unreadable band for as long as it runs could never record a
     duplicate at all. The splice this wants in the end — each screen's run
     of verdicts laid on the record at its longest overlap with the record's
     tail — is deferred, not answered. */
  bool read_whole = percentage_named == 100;
  int added = 0;
  for (int index = 0; index < standing_count; index++) {
    int shown_here = 0;
    for (int earlier = 0; earlier <= index; earlier++) {
      if (strcmp(standing[earlier].text, standing[index].text) == 0) {
        shown_here++;
      }
    }
    int held = 0;
    for (int kept = 0; kept < verdict_count; kept++) {
      if (strcmp(verdicts[kept].text, standing[index].text) == 0) {
        held++;
      }
    }
    if (held >= shown_here || (held > 0 && !read_whole)) {
      continue;
    }
    if (verdict_count == MAX_VERDICTS) {
      verdicts_dropped++;
      added++;
      continue;
    }
    verdicts[verdict_count] = standing[index];
    verdict_count++;
    added++;
  }
  return added;
}

/* Most of these groups say what they have to say in the picture rather than
   in words, so a screen kept by name keeps its beam path too. When the
   record is full the newest replaces the last kept, because a test that
   works through a list puts its result at the end. */
static bool capture_screen(const char *module, const char *key, long frame, int percentage_named) {
  for (int index = 0; index < capture_count; index++) {
    if (memcmp(captures[index].text, screen, sizeof screen) == 0) {
      captures[index].seen++;
      return false;
    }
  }
  capture *taken = NULL;
  if (capture_count < MAX_CAPTURES) {
    taken = &captures[capture_count++];
  } else {
    taken = &captures[MAX_CAPTURES - 1];
    captures_dropped++;
  }
  memcpy(taken->text, screen, sizeof screen);
  taken->percentage_named = percentage_named;
  taken->frame = frame;
  taken->seen = 1;
  taken->raster_file[0] = '\0';
  if (!keep_rasters) {
    return true;
  }
  char path[MAX_PATH_LENGTH];
  snprintf(taken->raster_file, sizeof taken->raster_file, "%.4s-%.7s-%ld.png", module, key, frame);
  snprintf(path, sizeof path, "%s/%s", report_directory, taken->raster_file);
  if (!write_raster(path)) {
    taken->raster_file[0] = '\0';
  }
  return true;
}

static void print_capture(FILE *report, const capture *taken) {
  fprintf(report, "  --- frame %ld, ", taken->frame);
  if (taken->percentage_named == NOTHING_DRAWN) {
    fprintf(report, "too little drawn to judge");
  } else {
    fprintf(report, "%d%% of the drawn glyphs named", taken->percentage_named);
  }
  fprintf(report, ", seen %d time%s", taken->seen, taken->seen == 1 ? "" : "s");
  if (taken->raster_file[0] != '\0') {
    fprintf(report, ", beam in %s", taken->raster_file);
  }
  fprintf(report, "\n");
  for (int row = 0; row < ROWS; row++) {
    const char *line = taken->text[row];
    bool blank = true;
    for (int column = 0; column < COLUMNS; column++) {
      if (line[column] != ' ') {
        blank = false;
      }
    }
    if (!blank) {
      fprintf(report, "  %2d |%s|\n", row, line);
    }
  }
}

static void run_group(const char *module, const group *entry, FILE *report) {
  restore_machine();
  shaker_trace_group(&cpc, entry->key, 2L * FRAMES_KEY_HELD);
  grid_left = NOMINAL_LEFT;
  grid_top = NOMINAL_TOP;
  capture_count = 0;
  captures_dropped = 0;
  verdict_count = 0;
  verdicts_dropped = 0;

  bool shifted = false;
  keyboard_key key = key_named(entry->key, &shifted);
  if (key == KEYBOARD_NO_KEY) {
    fprintf(report, "\n(%s) %s\n  this keyboard has no key named %s\n", entry->key, entry->label,
            entry->key);
    write_scoreboard_line(module, entry, "no key of that name");
    return;
  }
  press(key, shifted);

  long frame = 0;
  int samples_without_news = 0;
  int samples = 0;
  while (samples < MAX_SAMPLES_PER_GROUP && samples_without_news < MAX_SAMPLES_WITHOUT_NEWS) {
    run_frames(SAMPLE_STEP_FRAMES);
    frame += SAMPLE_STEP_FRAMES;
    samples++;
    int percentage_named = read_screen();
    bool news = capture_screen(module, entry->key, frame, percentage_named);
    if (collect_verdicts(percentage_named) > 0) {
      news = true;
    }
    samples_without_news = news ? 0 : samples_without_news + 1;
  }
  bool still_going = samples_without_news < MAX_SAMPLES_WITHOUT_NEWS;
  bool drew_its_own_screen = false;
  for (int index = 0; index < capture_count; index++) {
    if (memcmp(captures[index].text, menu_screen, sizeof menu_screen) != 0) {
      drew_its_own_screen = true;
    }
  }

  fprintf(report, "\n(%s) %s\n", entry->key, entry->label);
  if (entry->declared_tests > 0) {
    fprintf(report, "  the label declares %d test%s\n", entry->declared_tests,
            entry->declared_tests == 1 ? "" : "s");
  }
  fprintf(report, "  %d distinct screen%s in %ld frames%s", capture_count,
          capture_count == 1 ? "" : "s", frame, still_going ? ", and still going" : "");
  if (captures_dropped > 0) {
    fprintf(report, ", and %d more this record had no room for", captures_dropped);
  }
  fprintf(report, "\n");

  int wrong = 0;
  for (int index = 0; index < verdict_count; index++) {
    if (verdicts[index].failed) {
      wrong++;
    }
  }
  if (verdict_count > 0) {
    fprintf(report, "  %d self-graded line%s, %d of them wrong%s\n", verdict_count,
            verdict_count == 1 ? "" : "s", wrong,
            verdicts_dropped > 0 ? ", and more than this record holds" : "");
    for (int index = 0; index < verdict_count; index++) {
      fprintf(report, "  %s %s\n", verdicts[index].failed ? "!" : " ", verdicts[index].text);
    }
  }

  /* The standing carries the two things that do not move when a frame
     lands a sample earlier: what Shaker said, and whether it had finished
     saying it. */
  char standing[96];
  size_t at = 0;
  if (verdict_count > 0) {
    at += (size_t)snprintf(standing + at, sizeof standing - at, "%d graded, %d wrong",
                           verdict_count, wrong);
  } else if (drew_its_own_screen) {
    at += (size_t)snprintf(standing + at, sizeof standing - at, "recorded, ungraded");
  } else {
    at += (size_t)snprintf(standing + at, sizeof standing - at, "showed only the menu");
  }
  if (verdicts_dropped > 0) {
    at += (size_t)snprintf(standing + at, sizeof standing - at, ", and more than the record holds");
  }
  if (still_going) {
    snprintf(standing + at, sizeof standing - at, ", cut off at the cap");
  }
  write_scoreboard_line(module, entry, standing);
  for (int index = 0; index < verdict_count; index++) {
    add_to_scoreboard("    %s %s\n", verdicts[index].failed ? "!" : " ", verdicts[index].text);
  }
  total_verdicts += verdict_count;
  total_verdicts_wrong += wrong;
  if (verdict_count > 0) {
    total_groups_graded++;
  }
  for (int index = 0; index < capture_count; index++) {
    print_capture(report, &captures[index]);
  }
}

static void run_module(const char *module, const char *only_group) {
  if (!power_on()) {
    return;
  }
  grid_left = NOMINAL_LEFT;
  grid_top = NOMINAL_TOP;
  char command[64];
  snprintf(command, sizeof command, "RUN\"SHAKE27%s\n", module);
  run_frames(FRAMES_TO_PROMPT);
  type_text(command);
  run_frames(FRAMES_TO_LOAD_MODULE);

  int readable = read_screen();
  if (readable < 90) {
    TEST_FAIL("module %s: only %d%% of the menu could be read", module, readable);
    return;
  }
  read_menu();
  if (group_count == 0) {
    TEST_FAIL("module %s: the menu offers nothing this reader recognises", module);
    return;
  }
  memcpy(menu_screen, screen, sizeof menu_screen);
  save_machine();
  TEST_CHECK(cpc.fdc.drives[0] == &cpc.drives[0]);

  char path[MAX_PATH_LENGTH];
  snprintf(path, sizeof path, "%s/module-%s.txt", report_directory, module);
  FILE *report = fopen(path, "w");
  if (report == NULL) {
    TEST_FAIL("cannot write %s — run 'make test-shaker', which makes %s", path, report_directory);
    return;
  }

  int applicable = 0;
  for (int index = 0; index < group_count; index++) {
    if (groups[index].applies_to_this_crtc_type) {
      applicable++;
    }
  }
  fprintf(report, "%s\n", screen[0]);
  fprintf(report, "%d groups on the menu, %d of them this machine's CRTC type\n", group_count,
          applicable);
  fprintf(report, "\nthe menu, as this reader parsed it:\n");
  for (int index = 0; index < group_count; index++) {
    fprintf(report, "  (%s) %s\n", groups[index].key, groups[index].label);
  }

  int ran = 0;
  int skipped = 0;
  for (int index = 0; index < group_count; index++) {
    const group *entry = &groups[index];
    if (only_group != NULL && strcmp(only_group, entry->key) != 0) {
      continue;
    }
    if (!entry->applies_to_this_crtc_type) {
      fprintf(report, "\n(%s) %s\n  another CRTC type's test; not run\n", entry->key, entry->label);
      write_scoreboard_line(module, entry, "another CRTC type's");
      total_groups_skipped++;
      skipped++;
      continue;
    }
    run_group(module, entry, report);
    total_groups_run++;
    ran++;
  }
  if (fclose(report) != 0) {
    TEST_FAIL("module %s: %s was not written whole", module, path);
    return;
  }
  if (only_group != NULL && ran == 0 && skipped == 0) {
    TEST_FAIL("module %s has no group (%s)", module, only_group);
    return;
  }
  snprintf(module_summary, sizeof module_summary,
           "  module %s: %d groups run, %d left to another CRTC type, report in %s\n", module, ran,
           skipped, path);
}

static const char *only_module;
static const char *only_group;

/* One test per module: a module that cannot be reached at all is a failure,
   while the groups inside it are a record. The disc holds SHAKE27A through
   SHAKE27E. */
static bool wanted(const char *module) {
  return only_module == NULL || strcmp(only_module, module) == 0;
}

static void module_a_is_recorded(void) {
  if (wanted("A")) {
    run_module("A", only_group);
  }
}

static void module_b_is_recorded(void) {
  if (wanted("B")) {
    run_module("B", only_group);
  }
}

static void module_c_is_recorded(void) {
  if (wanted("C")) {
    run_module("C", only_group);
  }
}

static void module_d_is_recorded(void) {
  if (wanted("D")) {
    run_module("D", only_group);
  }
}

static void module_e_is_recorded(void) {
  if (wanted("E")) {
    run_module("E", only_group);
  }
}

/* A group's line, so that a difference can be reported under the group it
   belongs to rather than by a line number alone: eleven verdicts under one
   group all look alike out of context. */
static bool is_a_group_line(const char *line) {
  return line[0] >= 'A' && line[0] <= 'E' && line[1] == ' ' && line[2] == '(';
}

/* Puts a screen in front of the reader, in the previous frame as well as
   this one, since a line is only collected once it has stood still. */
static void show_screen(const char *const *rows, int count) {
  memset(screen, 0, sizeof screen);
  memset(previous_screen, 0, sizeof previous_screen);
  for (int row = 0; row < ROWS; row++) {
    const char *text = row < count ? rows[row] : "";
    snprintf(screen[row], sizeof screen[row], "%s", text);
    snprintf(previous_screen[row], sizeof previous_screen[row], "%s", text);
  }
}

/* Two of a group's tests can agree word for word — module E's (6) prints
   eleven verdicts and its #007F twice — and a screen read whole is the
   warrant for recording both. A screen read in part is not: a group that
   redraws its list leaves the tail of the last pass above the head of the
   next, and a frame caught in between shows a test twice with unread rows
   in the gap, as module D's (R) does. Such a frame may still bring a line
   the record has never held, which is how a group's later tests are
   gathered at all.

   The rows below are E (6)'s own, and the gap is what an unread row reads
   as. The percentages are handed in rather than measured, so that the line
   between the two readings can be walked: 93 is what the frame carrying
   the phantom read, and 100 what the frame warranting the repeat read. */
static void a_screen_read_in_part_cannot_repeat_a_verdict(void) {
  static const char *const settled[] = {
      ">>>>>> DELAY TO VSYNC:#007F (EXP:#007F)",
      ">>>>>> DELAY TO VSYNC:#009F (EXP:#009F)",
      ">>>>>> DELAY TO VSYNC:#007F (EXP:#007F)",
  };
  static const char *const torn[] = {
      ">>>>>> DELAY TO VSYNC:#007F (EXP:#007F)",
      "????????????????????????????????????????????????????????????????????????????????",
      ">>>>>> DELAY TO VSYNC:#007F (EXP:#007F)",
      ">>>>>> DELAY TO VSYNC:#001B (EXP:#001B)",
  };

  /* Read whole, the two that agree are two tests and the record owes both. */
  verdict_count = 0;
  verdicts_dropped = 0;
  show_screen(settled, 3);
  TEST_EQUAL(collect_verdicts(100), 3);
  TEST_EQUAL(collect_verdicts(100), 0);

  /* Torn, the repeat is a remnant and is refused, while the line the record
     has never held is taken. */
  verdict_count = 0;
  show_screen(torn, 4);
  TEST_EQUAL(collect_verdicts(55), 2);

  /* And one unread glyph in a corner is a tear as far as this goes: the
     rule asks for the whole screen and not for most of it. */
  verdict_count = 0;
  show_screen(torn, 4);
  TEST_EQUAL(collect_verdicts(99), 2);

  /* The same rows read whole are three verdicts again. */
  verdict_count = 0;
  show_screen(torn, 4);
  TEST_EQUAL(collect_verdicts(100), 3);

  verdict_count = 0;
  memset(screen, 0, sizeof screen);
  memset(previous_screen, 0, sizeof previous_screen);
}

/* The page module E's (2) draws while it is still drawing carries both of
   its sentences, and neither is a verdict until one of them is left standing
   alone. Sampled twice, as a page that stood still would be, it yields
   nothing; the page it settles on yields the one it kept. */
static void a_page_that_speaks_both_verdicts_yields_neither(void) {
  static const char *const still_drawing[] = {
      "CRTC 1  VMA UPDATE ON SPEC ADJ FROM C4=0 WHEN R4=0 PAGE 1",
      "IF YOU CAN READ THIS...YOUR EMULATOR HAS A PROBLEM",
      "YOU'VE WON THIS STAGE (UNLESS A PROBLEM IS INDICATED ON THIS PAGE)",
  };
  static const char *const settled[] = {
      "CRTC 1  VMA UPDATE ON SPEC ADJ FROM C4=0 WHEN R4=0 PAGE 1",
      "YOU'VE WON THIS STAGE (UNLESS A PROBLEM IS INDICATED ON THIS PAGE)",
  };
  static const char *const settled_wrong[] = {
      "CRTC 1  VMA UPDATE ON SPEC ADJ FROM C4=0 WHEN R4=0 PAGE 1",
      "IF YOU CAN READ THIS...YOUR EMULATOR HAS A PROBLEM",
  };
  verdict_count = 0;
  verdicts_dropped = 0;
  show_screen(still_drawing, 3);
  TEST_EQUAL(collect_verdicts(100), 0);
  TEST_EQUAL(collect_verdicts(100), 0);

  verdict_count = 0;
  show_screen(settled, 2);
  TEST_EQUAL(collect_verdicts(100), 1);
  TEST_CHECK(!verdicts[0].failed);

  verdict_count = 0;
  show_screen(settled_wrong, 2);
  TEST_EQUAL(collect_verdicts(100), 1);
  TEST_CHECK(verdicts[0].failed);

  verdict_count = 0;
  memset(screen, 0, sizeof screen);
  memset(previous_screen, 0, sizeof previous_screen);
}

/* A title's expectation reaches the measured lines beneath it and stops
   where they stop. */
static void a_title_carries_the_expectation_for_the_lines_below_it(void) {
  static const char *const page[] = {
      "CRTC 1  VSYNC TORTURE (LOCK MECHANISM)",
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
      "  R7=0/R4=1/C0=00 VSYNC=#5E",
      "THE SCREEN MUST BE STABLE VERTICALLY",
      "  R7=0/R4=1/C0=01 VSYNC=#5F",
  };
  /* Built: two values on the governed row, and a blank one below it. No
     screen on the disc writes either — the rows of the one title it prints
     run unbroken under it — so the blank stands for what a run must not
     reach across, and the two values for which of them is the measurement. */
  static const char *const stopped_by_a_blank[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=#5E/R4=1/C0=00 VSYNC=#5F",
      "",
      "  R7=0/R4=1/C0=3F VSYNC=#5E",
  };
  static const char *const after_a_verdict[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
      ">>>>>> DELAY TO VSYNC:#007F (EXP:#007F)",
      "  R7=0/R4=1/C0=00 VSYNC=#5E",
  };
  static const char *const after_a_tear[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
      "  R7=0/R4=1/C0=?? VSYNC=#5F",
      "  R7=0/R4=1/C0=00 VSYNC=#5E",
  };
  static const char *const title_alone[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
  };
  static const char *const measurement_alone[] = {
      "  R7=0/R4=1/C0=00 VSYNC=#5E",
  };

  verdict_count = 0;
  verdicts_dropped = 0;
  show_screen(page, 6);
  TEST_EQUAL(collect_verdicts(100), 2);
  TEST_CHECK(!verdicts[0].failed);
  TEST_CHECK(verdicts[1].failed);

  /* A blank row ends the run as any other line does, and what is read from
     a governed line is the value at its end, not the first it carries. */
  verdict_count = 0;
  show_screen(stopped_by_a_blank, 4);
  TEST_EQUAL(collect_verdicts(100), 1);
  TEST_CHECK(!verdicts[0].failed);

  /* A verdict of the group's own ends the run, and so does a line lost to a
     tear: the measurement below each is left unread. */
  verdict_count = 0;
  show_screen(after_a_verdict, 4);
  TEST_EQUAL(collect_verdicts(100), 2);
  verdict_count = 0;
  show_screen(after_a_tear, 4);
  TEST_EQUAL(collect_verdicts(100), 1);

  /* And no title reaches past its own screen. */
  verdict_count = 0;
  show_screen(title_alone, 1);
  TEST_EQUAL(collect_verdicts(100), 0);
  show_screen(measurement_alone, 1);
  TEST_EQUAL(collect_verdicts(100), 0);

  verdict_count = 0;
  memset(screen, 0, sizeof screen);
  memset(previous_screen, 0, sizeof previous_screen);
}

/* And the lines a title does not govern, each of which a reader could take
   for one of its own. A row carrying a bracket of its own is not one of
   these — module C's (O) prints "(NEXT FRAME +1=#0A20, +2=#1120)" between
   every pair of its measurements, naming values nobody has measured — a
   title lost to a tear is no title, a verdict of the group's own is not a
   title however its bracket reads, and a key in front of a title takes the
   bracket the reading looks in, which is the first on the line and now the
   key's own. Only the rows quoted from a module's screen are the disc's;
   the pages they stand in are built. */
static void a_title_is_told_from_the_lines_it_does_not_govern(void) {
  static const char *const bracketed_row[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
      "     (NEXT FRAME +1=#0A20, +2=#1120)",
      "  R7=0/R4=1/C0=00 VSYNC=#5E",
  };
  static const char *const torn_title[] = {
      "TST COMP C4/R7 ACTIVE DUR?NG VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5E",
  };
  static const char *const a_verdict_of_its_own[] = {
      ">>>>>> DELAY TO VSYNC:#0032 (EXP:#0032)",
      "  R7=0/R4=1/C0=3F VSYNC=#5E",
  };
  static const char *const a_key_in_front[] = {
      "(O) ALL      : INTERLACE VSYNC NIGHTMARE (EXP:#5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5E",
  };
  static const char *const two_titles[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5E)",
      "  R7=0/R4=1/C0=00 VSYNC=#5E",
  };
  /* A row beneath a title may still speak for itself, and then its own word
     is the verdict: module B's (6) writes this one, where the value is not
     the one a title asks for and the group passes the test anyway. */
  static const char *const a_row_that_grades_itself[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)",
      "  OUTI ON R0.JIT 5TH uSec ON C0=1 - RES: #26 :GOOD",
  };
  /* No group prints a title that grades itself. The reader answers for one
     all the same: taken for a title it would lose the verdict standing on
     it and put every row below against a number nobody was measured on. */
  static const char *const a_title_that_grades_itself[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)  WRONG",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
  };
  /* And built for the reason the "(EXP: none)" line in the table of
     renderings is: a bracket that names silicon's value and then does not
     give it leaves the group nothing to grade its rows against. */
  static const char *const a_title_naming_no_value[] = {
      "TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP: none)",
      "  R7=0/R4=1/C0=3F VSYNC=#5F",
  };

  verdict_count = 0;
  verdicts_dropped = 0;
  show_screen(bracketed_row, 4);
  TEST_EQUAL(collect_verdicts(100), 1);

  verdict_count = 0;
  show_screen(torn_title, 2);
  TEST_EQUAL(collect_verdicts(55), 0);

  verdict_count = 0;
  show_screen(a_verdict_of_its_own, 2);
  TEST_EQUAL(collect_verdicts(100), 1);
  TEST_CHECK(!verdicts[0].failed);

  verdict_count = 0;
  show_screen(a_key_in_front, 2);
  TEST_EQUAL(collect_verdicts(100), 0);

  /* A second title takes the first one's place, and its own rows with it. */
  verdict_count = 0;
  show_screen(two_titles, 4);
  TEST_EQUAL(collect_verdicts(100), 2);
  TEST_CHECK(!verdicts[0].failed);
  TEST_CHECK(!verdicts[1].failed);

  verdict_count = 0;
  show_screen(a_row_that_grades_itself, 2);
  TEST_EQUAL(collect_verdicts(100), 1);
  TEST_CHECK(!verdicts[0].failed);

  verdict_count = 0;
  show_screen(a_title_that_grades_itself, 2);
  TEST_EQUAL(collect_verdicts(100), 1);
  TEST_CHECK(verdicts[0].failed);

  verdict_count = 0;
  show_screen(a_title_naming_no_value, 2);
  TEST_EQUAL(collect_verdicts(100), 0);

  verdict_count = 0;
  memset(screen, 0, sizeof screen);
  memset(previous_screen, 0, sizeof previous_screen);
}

/* Lines Shaker printed, one of each rendering the reader knows, taken from
   the records or from the scoreboard as it stood on a day the machine was
   getting them wrong. */
typedef struct {
  const char *line;
  bool graded;
  bool failed;
} verdict_case;

static const verdict_case printed_lines[] = {
    /* Silicon's value named in the bracket, four ways. */
    {">>>>>> DELAY TO VSYNC:#0030 (EXP:#00F7)  WRONG", true, true},
    {">>>>>> DELAY TO VSYNC:#0032 (EXP:#0032)", true, false},
    {"RESULT:#8700 WRONG (EXP:#4E40)", true, true},
    {"R5=1 / ON 1ST ADD LINE, R5=0 / CPU TO NEW FRAME:#0080 (#0080 expected)", true, false},
    {"R5 PREV=20. ON C4=R4=#26/C9=R9=7/C0io=#00, R5=0, CPU TO C4=0:#0084 (exp:#0004)", true, true},
    /* And the word with no colon at all, which modules B and D write. The
       first is a row of a two-column table: the test whose value stands
       nearest the bracket is the one graded, and the other goes unread. */
    {"2B         :#CC >WRONG (Exp #C4)        FD CB 00 16:#CC", true, true},
    {"Unbreakable DD Prefix on Pending Int #00 (Exp#00), On R52:#0E18 (Exp#0E18)", true, false},
    {"Break ED xx on Pending Int #00 (Exp#00)", true, false},
    /* A title over the rows that follow it, naming what they are checked
       against and measuring nothing itself, and one of those rows: a value
       at the end and nothing to pair it with, which is a verdict only while
       the title above it stands. */
    {"TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK (EXP #5F)", false, false},
    {"  R7=0/R4=1/C0=3F VSYNC=#5F", false, false},
    /* And named for each type, where this machine reads the clause that
       speaks for a type 0 — by its number, or by gathering the rest. */
    {"R3h=0.UPD R3h=8 ON 8th LINE. DELAY VSYNC OFF=#0032 (CRTC 0.3.4:032/CRTC 1.2:23A)", true,
     false},
    {"R3h=0.UPP R3h=8 ON 9th LINE. DELAY VSYNC OFF=#03F2 (CRTC 0.3.4:3F2/CRTC 1.2:1F4)", true,
     false},
    {"R7=0/VSYNC/R4=0 VSIZE=#0044 (CRTC 0.1.2:#44 / 3.4:#FFFF=DEADLOCK)", true, false},
    {"TEST INT ON INST DEC DE   :#58 (CRTC 3+4:#58/ OTHERS:#59)", true, true},
    /* A group that judged itself and printed only the verdict, which is a
       grading with no value of silicon's in it — with the mark on a type 1's
       groups and without it on a type 0's, and answered by the ":GOOD" the
       same group prints where the machine agreed. */
    {"TEST C: 5F,5F: WRONG!", true, true},
    {"OUTI ON C0=0,R0=0, RES: #1F :WRONG!", true, true},
    {"C4==R4 & C9<>R9: UPD R9=C9  WHEN C0==1. C4=>00 IF 1:0 WRONG", true, true},
    {"OUTI ON R7 LAST CHANCE 5TH uSec ON C0=0 - RES: #1E :GOOD", true, false},
    /* A failure the disc rings with x's, and the clause that hands the
       passing outcome to types this machine is not one of — where the
       marked failure is the answer asked for rather than a fault. A ring
       with no such bracket names no type and is not read at all, nor is a
       bracket that names types without handing them the pass. */
    {"PREV R9=7 R4=38 >> UPD R4=1 WHEN C4=1 & C9=7 >> C4=0       :xKOx", false, false},
    {"X (CRTC 3+4 ONLY):xKOx", false, false},
    {"PREV R9=7 >> UPD R9=1 WHEN C9=3>>C9=0 (OK FOR CRT 3+4 ONLY):xKOx", true, false},
    /* A bare KO names which of two ways a counter went and grades nothing,
       which is why only the ringed one is read. */
    {"UPDATE R0=7F, OUT ON HCC=3E :KO", false, false},
    {"UPDATE R0=7F, OUT ON HCC=39 :OK", false, false},
    {"OK: C0=..3F..40..41.. / KO: C0=..3F..00..01..", false, false},
    /* A verdict a group speaks in a sentence, which was left unread while
       nothing here could tell what a sentence meant. Module E's (2) settled
       this pair: put to it both ways, with the rule that group grades in and
       put back, it settles on the first where the machine is right and on
       the second where it is wrong. */
    {"YOU'VE WON THIS STAGE (UNLESS A PROBLEM IS INDICATED ON THIS PAGE)", true, false},
    {"IF YOU CAN READ THIS...YOUR EMULATOR HAS A PROBLEM", true, true},
    /* And the two that keep their sentences unread. The first says which
       part a chip is rather than whether it is right, in the same breath as
       the pair above — which is why the reading is keyed on the fault it
       names and not on the words that lead up to it. The second belongs to a
       group whose neighbouring lines turn on one of them flashing, which no
       reader that samples five frames apart can judge. */
    {"BUT IF YOU CAN READ THIS, YOUR CRTC 1 IS 1-B !!", false, false},
    {"VERY BAD TRIP FOR YOUR EMULATOR!!!", false, false},
    /* A legend keyed by value rather than by type, which names no type and
       must not be read as one. */
    {"PREV R9=7 R4=1 >> UPD R4=3 WHEN C4=1 & C9=7 (LAST LINE):01 (00:C4ovf 01:C4=0)", false, false},
    {"PREV R9=7 R4=1 >> UPD R4=0 WHEN C4=1 & C9=7 (UPD FROM C0vsio)(01:C4=0 00:C4 ovf)", false,
     false},
    /* Several answers allowed, a rendering not yet read. */
    {"TEST INT ON INST SET n,(IX+n'):#40 (#40 0/16 or #44)", false, false},
    {"TEST INT ON INST CP (IX+n):#C5,#C5 (C2/C2 or C5/C5 or C2/C5)", false, false},
    /* Brackets naming the values a test is about to check, carrying no
       measurement of their own. */
    {"CRTC 0  1 CSYNC 4us (R2=#2E) VS 2xCSYNC 2us (2 flip/flop) (+R2=#33)", false, false},
    {"     (NEXT FRAME +1=#0A20, +2=#1120)", false, false},
};

/* Lines built rather than found, each holding one guard that no printed
   line reaches. They are what the guards are for: a rendering nobody prints
   is not a rendering, but a rule nobody exercises is not a rule either. */
static const verdict_case built_lines[] = {
    /* The word inside a line is not the word at its end. A title that
       names what a group is about to test carries no verdict, and nothing
       in it can be paired with a value. */
    {"TEST FOR A WRONG R5 ON THE LAST LINE", false, false},
    /* A legend keyed by value, written in clean digits and with a
       measurement before it, which the word CRTC is what keeps out: the 00
       would otherwise read as a clause for this machine and hand back the
       44 that belongs to the legend. */
    {"X=#0044 (00:44 01:88)", false, false},
    /* Only the rest are gathered, and no type is named at all. */
    {"X=#0022 (OTHERS:#22)", true, false},
    /* Two clauses gather the rest, which Shaker does not write; the first
       of them answers, and this holds that tie-break where it stands. */
    {"X=#0022 (OTHERS:#22/ OTHERS:#33)", true, false},
    /* A register name before the colon lends the clause no type of its
       own, whatever zeroes it carries. */
    {"X=#0022 (CRTC 1.2 R0:#44/ OTHERS:#22)", true, false},
    {"X=#0022 (CRTC 1.2, C0io=#00:#44/ OTHERS:#22)", true, false},
    /* A value set off from its colon by a space is still the value. */
    {"X=#0044 (CRTC 0: #44/ OTHERS:#22)", true, false},
    /* A bracket that names silicon's value and then does not give it takes
       the line down with it, rather than letting a later bracket be paired
       with a number standing inside this one. */
    {"X:#0032 (EXP: none) (EXP:#32)", false, false},
    /* The word only corroborates the numbers, and it is read where they
       agree and it does not. */
    {"RESULT:#0032 WRONG (EXP:#0032)", true, true},
    /* A clause naming a type 0 outranks the one gathering the rest,
       whichever stands first. */
    {"X=#0011 (CRTC 0:#11/ OTHERS:#22)", true, false},
    {"X=#0011 (OTHERS:#22/ CRTC 0:#11)", true, false},
    /* Type 10 is not type 0. */
    {"X=#0022 (CRTC 10:#11/ OTHERS:#22)", true, false},
    /* A bracket that speaks for no type a 0 is. */
    {"DELAY VSYNC OFF=#0032 (CRTC 1.2:23A)", false, false},
    /* A word that merely begins with the letters, naming a value for
       something other than silicon, and a word that is not the word. */
    {"X=#0044 (EXPANSION #44)", false, false},
    {"X=#0044 (EXT #44)", false, false},
    /* A clause for a type 0 whose value cannot be read takes the bracket
       down rather than letting the rest answer in its place. */
    {"X=#0058 (CRTC 0:DEADLOCK/ OTHERS:#22)", false, false},
    {"X=#0058 (CRTC 0:/ OTHERS:#22)", false, false},
    {"X=#0058 (CRTC 0:#FFFFFFFF/ OTHERS:#22)", false, false},
};

static void check_verdict_case(const char *provenance, const verdict_case *wanted) {
  bool read_as_failed = false;
  bool read_as_graded = read_verdict(wanted->line, 0, &read_as_failed);
  if (read_as_graded != wanted->graded) {
    TEST_FAIL("%s: \"%s\" was %s, where it should be %s", provenance, wanted->line,
              read_as_graded ? "graded" : "left alone", wanted->graded ? "graded" : "left alone");
  } else if (wanted->graded && read_as_failed != wanted->failed) {
    TEST_FAIL("%s: \"%s\" was read as %s", provenance, wanted->line,
              read_as_failed ? "wrong" : "right");
  }
}

static void the_verdict_reader_knows_its_renderings(void) {
  for (size_t index = 0; index < sizeof printed_lines / sizeof printed_lines[0]; index++) {
    check_verdict_case("printed", &printed_lines[index]);
  }
  for (size_t index = 0; index < sizeof built_lines / sizeof built_lines[0]; index++) {
    check_verdict_case("built", &built_lines[index]);
  }
}

/* The same lines and labels read as each of the five machines. Shaker
   states silicon's value for each type in one bracket and gates whole
   groups by the type named at the head of a label, so a reader that follows
   the type the machine was built as is what lets a second type be graded at
   all. A machine built as a type 1 reads the clauses that speak for one;
   the readings for a 2, a 3 and a 4 say how they will be read when a
   machine can be built as those. */
typedef struct {
  const char *line;
  uint8_t type;
  bool graded;
  bool failed;
} verdict_case_by_type;

static const char vsync_off_by_type[] =
    "R3h=0.UPD R3h=8 ON 8th LINE. DELAY VSYNC OFF=#0032 (CRTC 0.3.4:032/CRTC 1.2:23A)";

static const verdict_case_by_type lines_by_type[] = {
    /* The machine measured #0032, which is what a type 0, 3 or 4 owes and
       not what a type 1 or a type 2 does. */
    {vsync_off_by_type, 0, true, false},
    {vsync_off_by_type, 3, true, false},
    {vsync_off_by_type, 1, true, true},
    {vsync_off_by_type, 2, true, true},
    /* The rest are gathered for every type the bracket passes over. */
    {"X=#0011 (CRTC 0:#11/ OTHERS:#22)", 0, true, false},
    {"X=#0011 (CRTC 0:#11/ OTHERS:#22)", 1, true, true},
    /* A ringed failure with the passing outcome handed to types this
       machine is not one of: the answer asked for on a 0, and a fault on a
       3, which the bracket names. */
    {"PREV R9=7 >> UPD R9=1 WHEN C9=3>>C9=0 (OK FOR CRT 3+4 ONLY):xKOx", 0, true, false},
    {"PREV R9=7 >> UPD R9=1 WHEN C9=3>>C9=0 (OK FOR CRT 3+4 ONLY):xKOx", 3, true, true},
    {"X (OK FOR CRTC 3+4 ONLY):xKOx", 0, true, false},
    {"X (OK FOR CRTC 3+4 ONLY):xKOx", 3, true, true},
    /* A bracket that names some types and gathers no rest grades nothing on
       a machine it passes over, and grades on one it names. */
    {"DELAY VSYNC OFF=#0032 (CRTC 1.2:23A)", 0, false, false},
    {"DELAY VSYNC OFF=#0032 (CRTC 1.2:23A)", 1, true, true},
    /* The digits behind the word are read whole whatever the machine, so 10
       is no more a 1 than it is a 0. */
    {"X=#0022 (CRTC 10:#11/ OTHERS:#22)", 0, true, false},
    {"X=#0022 (CRTC 10:#11/ OTHERS:#22)", 1, true, false},
    /* A line already on the record, and a gap written down rather than
       implied away: Shaker carries the second clause's types without
       repeating the word, so this reader does not see them and grades
       nothing on a 3 or a 4 where it grades on a 0, a 1 and a 2. */
    {"R7=0/VSYNC/R4=0 VSIZE=#0044 (CRTC 0.1.2:#44 / 3.4:#FFFF=DEADLOCK)", 0, true, false},
    {"R7=0/VSYNC/R4=0 VSIZE=#0044 (CRTC 0.1.2:#44 / 3.4:#FFFF=DEADLOCK)", 2, true, false},
    {"R7=0/VSYNC/R4=0 VSIZE=#0044 (CRTC 0.1.2:#44 / 3.4:#FFFF=DEADLOCK)", 3, false, false},
    {"R7=0/VSYNC/R4=0 VSIZE=#0044 (CRTC 0.1.2:#44 / 3.4:#FFFF=DEADLOCK)", 4, false, false},
};

typedef struct {
  const char *label;
  uint8_t type;
  bool applies;
} label_case_by_type;

static const label_case_by_type labels_by_type[] = {
    {"CRTC 2 RVMB (22 TST)", 0, false},
    {"CRTC 2 RVMB (22 TST)", 2, true},
    {"CRTC 1 BUG OUTI R0", 1, true},
    {"CRTC 1 BUG OUTI R0", 0, false},
    /* A variant behind the number is still that number. */
    {"CRTC 1-A OR 1-B?", 1, true},
    {"CRTC 1-A OR 1-B?", 0, false},
    /* And a head naming two types speaks for both. */
    {"CRTC 3/4 : STATUS (DEADLOCK MAY HAPPEN IF BAD EMULATION)", 3, true},
    {"CRTC 3/4 : STATUS (DEADLOCK MAY HAPPEN IF BAD EMULATION)", 4, true},
    {"CRTC 3/4 : STATUS (DEADLOCK MAY HAPPEN IF BAD EMULATION)", 0, false},
    {"ALL : CRTC 3/4 PARITY", 0, true},
    {"ALL : CRTC 3/4 PARITY", 1, true},
    {"CRTC 0 R9 UPDATE (OPEN TO OTHER CRTC'S)", 1, true},
};

static void the_reader_follows_the_type_the_machine_was_built_as(void) {
  for (size_t index = 0; index < sizeof lines_by_type / sizeof lines_by_type[0]; index++) {
    const verdict_case_by_type *wanted = &lines_by_type[index];
    bool failed = false;
    bool graded = read_verdict(wanted->line, wanted->type, &failed);
    if (graded != wanted->graded) {
      TEST_FAIL("as a type %u: \"%s\" was %s, where it should be %s", wanted->type, wanted->line,
                graded ? "graded" : "left alone", wanted->graded ? "graded" : "left alone");
    } else if (wanted->graded && failed != wanted->failed) {
      TEST_FAIL("as a type %u: \"%s\" was read as %s", wanted->type, wanted->line,
                failed ? "wrong" : "right");
    }
  }
  for (size_t index = 0; index < sizeof labels_by_type / sizeof labels_by_type[0]; index++) {
    const label_case_by_type *wanted = &labels_by_type[index];
    bool applies = applies_to_crtc_type(wanted->label, wanted->type);
    if (applies != wanted->applies) {
      TEST_FAIL("as a type %u: \"%s\" is %s, where it should be %s", wanted->type, wanted->label,
                applies ? "run" : "left to another type",
                wanted->applies ? "run" : "left to another type");
    }
  }
}

/* A difference here is not a fault to be fixed but a reading to be judged,
   so every line that moved is counted, the first is named under its group,
   and the choice of what to keep is left to the human. */
static void the_scoreboard_matches_the_one_on_record(void) {
  if (only_module != NULL || only_group != NULL) {
    return;
  }
  char written_path[MAX_PATH_LENGTH];
  snprintf(written_path, sizeof written_path, "%s/scoreboard.txt", report_directory);
  FILE *written = fopen(written_path, "r");
  if (written == NULL) {
    TEST_FAIL("no scoreboard was written to %s", written_path);
    return;
  }
  FILE *on_record = fopen(scoreboard_on_record, "r");
  if (on_record == NULL) {
    fclose(written);
    TEST_FAIL("no scoreboard on record at %s, which is read from the directory the tests are\n"
              "    run in. If there truly is none, copy %s there and read it before you commit it.",
              scoreboard_on_record, written_path);
    return;
  }
  char written_line[512] = "";
  char recorded_line[512] = "";
  char under[COLUMNS + 32] = "the head of the file";
  char first_on_record[512] = "";
  char first_now[512] = "";
  int line = 0;
  int first_difference = 0;
  int differences = 0;
  /* One file outlasts the other where a line was added or dropped, and the
     one that ended first is not read again: the walk goes on to the end of
     the longer so that every line that moved is counted. */
  bool written_ended = false;
  bool record_ended = false;
  while (true) {
    char *from_written = written_ended ? NULL : fgets(written_line, sizeof written_line, written);
    char *from_record = record_ended ? NULL : fgets(recorded_line, sizeof recorded_line, on_record);
    written_ended = from_written == NULL;
    record_ended = from_record == NULL;
    if (from_written == NULL && from_record == NULL) {
      break;
    }
    line++;
    if (from_written != NULL && from_record != NULL && strcmp(written_line, recorded_line) == 0) {
      /* The group a line falls under is the last one named above it, so it
         stops being read once the line to report has been found: the walk
         goes on to the end of the file to count the rest. */
      if (first_difference == 0 && is_a_group_line(recorded_line)) {
        snprintf(under, sizeof under, "%s", recorded_line);
        trim_trailing_newline(under);
      }
      continue;
    }
    differences++;
    if (first_difference != 0) {
      continue;
    }
    first_difference = line;
    snprintf(first_on_record, sizeof first_on_record, "%s",
             from_record == NULL ? "(the record ends here)" : recorded_line);
    snprintf(first_now, sizeof first_now, "%s",
             from_written == NULL ? "(the new one ends here)" : written_line);
    trim_trailing_newline(first_on_record);
    trim_trailing_newline(first_now);
  }
  fclose(on_record);
  fclose(written);
  if (differences == 0) {
    return;
  }
  TEST_FAIL("%d line%s of the scoreboard moved; the first is line %d, under %s\n"
            "    on record: %s\n    now:       %s\n    all of it: diff %s %s",
            differences, differences == 1 ? "" : "s", first_difference, under, first_on_record,
            first_now, scoreboard_on_record, written_path);
}

/* The five modules share nothing: each boots its own machine, writes its own
   record, and appends its own lines to the scoreboard. So they run at once,
   a child apiece, and what a child has to hand back — its lines, its
   tallies, its failures and the line it reports — is gathered through a
   shared mapping and merged module by module, A through E, so that a sweep
   costs the slowest module rather than the sum of five and leaves the same
   scoreboard behind. Only those closing lines are held back to be printed
   in order; what a module fails on is heard as it happens, out of order and
   named by its own test. */
typedef struct {
  int groups_run;
  int groups_skipped;
  int groups_graded;
  int verdicts;
  int verdicts_wrong;
  int failures;
  char scoreboard[MAX_SCOREBOARD];
  size_t scoreboard_length;
  bool scoreboard_overflowed;
  char summary[MAX_MODULE_SUMMARY];
} module_result;

static const struct {
  void (*run)(void);
  const char *test_name;
  const char *module;
} modules[] = {
    {module_a_is_recorded, "module_a_is_recorded", "A"},
    {module_b_is_recorded, "module_b_is_recorded", "B"},
    {module_c_is_recorded, "module_c_is_recorded", "C"},
    {module_d_is_recorded, "module_d_is_recorded", "D"},
    {module_e_is_recorded, "module_e_is_recorded", "E"},
};
#define MODULE_COUNT (sizeof modules / sizeof modules[0])

static bool run_every_module(void) {
  size_t mapping_length = MODULE_COUNT * sizeof(module_result);
  module_result *results =
      mmap(NULL, mapping_length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (results == MAP_FAILED) {
    printf("shaker: cannot map the memory the five module runs report through\n");
    return false;
  }

  shaker_trace_configure();
  /* Anything the parent has said but not yet written would be inherited by
     every child and said again by each of them. */
  fflush(NULL);
  size_t started = 0;
  pid_t children[MODULE_COUNT];
  while (started < MODULE_COUNT) {
    children[started] = fork();
    if (children[started] == -1) {
      break;
    }
    if (children[started] == 0) {
      /* The parent has run nothing yet, so the tallies this child reports
         are its own from zero. TEST_RUN would have named the test it was
         about to run; nothing else does now. */
      test_current = modules[started].test_name;
      shaker_trace_begin(modules[started].module, crtc_type);
      modules[started].run();
      module_result *result = &results[started];
      result->groups_run = total_groups_run;
      result->groups_skipped = total_groups_skipped;
      result->groups_graded = total_groups_graded;
      result->verdicts = total_verdicts;
      result->verdicts_wrong = total_verdicts_wrong;
      result->failures = test_failures;
      memcpy(result->scoreboard, scoreboard, scoreboard_length);
      result->scoreboard_length = scoreboard_length;
      result->scoreboard_overflowed = scoreboard_overflowed;
      memcpy(result->summary, module_summary, strlen(module_summary) + 1);
      /* _exit does not flush, and a run whose output is a pipe rather than
         a terminal has everything it reported still sitting in a buffer. */
      fflush(NULL);
      _exit(0);
    }
    started++;
  }

  /* A run that was started is waited for whether or not the rest could be,
     so that no child outlives the sweep that asked for it. */
  bool every_module_ran = started == MODULE_COUNT;
  for (size_t index = 0; index < started; index++) {
    int status = 0;
    if (waitpid(children[index], &status, 0) == -1 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
      test_current = modules[index].test_name;
      TEST_FAIL("the run for module %s did not finish", modules[index].module);
      every_module_ran = false;
    }
  }
  /* Counted as the five the sweep owed rather than the few it managed, so
     that the closing line is not a smaller number of tests all passing. */
  for (size_t index = started; index < MODULE_COUNT; index++) {
    test_current = modules[index].test_name;
    test_count++;
    TEST_FAIL("module %s was never started", modules[index].module);
  }

  for (size_t index = 0; index < started; index++) {
    const module_result *result = &results[index];
    /* These five did not go through TEST_RUN — a child ran each — so the
       count and the failures it would have kept are added here by hand. */
    test_count++;
    test_failures += result->failures;
    printf("%s", result->summary);
    total_groups_run += result->groups_run;
    total_groups_skipped += result->groups_skipped;
    total_groups_graded += result->groups_graded;
    total_verdicts += result->verdicts;
    total_verdicts_wrong += result->verdicts_wrong;
    scoreboard_overflowed = scoreboard_overflowed || result->scoreboard_overflowed;
    if (scoreboard_overflowed ||
        scoreboard_length + result->scoreboard_length > sizeof scoreboard) {
      /* Stop appending rather than skip a module and take up the next: a
         scoreboard missing one in the middle reads as though that module
         had nothing to say. The tallies above still count, so the head of
         the file does not undercount what ran. */
      scoreboard_overflowed = true;
      continue;
    }
    memcpy(scoreboard + scoreboard_length, result->scoreboard, result->scoreboard_length);
    scoreboard_length += result->scoreboard_length;
  }
  munmap(results, mapping_length);
  return every_module_ran;
}

static void run_the_readers_own_tests(void) {
  TEST_RUN(the_verdict_reader_knows_its_renderings);
  TEST_RUN(the_reader_follows_the_type_the_machine_was_built_as);
  TEST_RUN(a_screen_read_in_part_cannot_repeat_a_verdict);
  TEST_RUN(a_page_that_speaks_both_verdicts_yields_neither);
  TEST_RUN(a_title_carries_the_expectation_for_the_lines_below_it);
  TEST_RUN(a_title_is_told_from_the_lines_it_does_not_govern);
}

/* The order of these is the Makefile recipe's, and the two move together.
   Every one of them has a default. */
int main(int argc, char **argv) {
  if (argc > 1) {
    rom_directory = argv[1];
  }
  if (argc > 2) {
    disc_directory = argv[2];
  }
  if (argc > 3) {
    report_directory = argv[3];
  }
  if (argc > 4 && argv[4][0] != '\0') {
    scoreboard_on_record = argv[4];
  }
  if (argc > 5 && argv[5][0] != '\0') {
    only_module = argv[5];
  }
  if (argc > 6 && argv[6][0] != '\0') {
    only_group = argv[6];
    keep_rasters = true;
  }
  if (argc > 7 && argv[7][0] != '\0') {
    if (argv[7][1] != '\0' || argv[7][0] < '0' || argv[7][0] > '4') {
      printf("shaker: %s names no CRTC; the types are 0 to 4\n", argv[7]);
      return 1;
    }
    crtc_type = (uint8_t)(argv[7][0] - '0');
  }
  if (only_group != NULL && only_module == NULL) {
    printf("shaker: a group needs the module it belongs to — pass MODULE too\n");
    return 1;
  }

  if (!run_every_module()) {
    return TEST_REPORT("shaker");
  }

  /* The readers borrow the screen and the verdict tally and put both back,
     and nothing after them reads either, so a run of one group answers for
     them as a whole sweep does. They stand before the early return below
     because `make test-shaker MODULE=D GROUP=R` is the run anyone working
     on a reader makes, and that run took the early return. */
  run_the_readers_own_tests();

  /* A run of one module or one group has walked part of the menu, and a
     part is not something the record can be set against. It writes its
     module records and stops there, so that the file the comparison reads
     is always a whole sweep. */
  if (only_module != NULL || only_group != NULL) {
    return TEST_REPORT("shaker");
  }

  char scoreboard_path[MAX_PATH_LENGTH];
  snprintf(scoreboard_path, sizeof scoreboard_path, "%s/scoreboard.txt", report_directory);
  FILE *file = fopen(scoreboard_path, "w");
  if (file == NULL) {
    printf("shaker: cannot write %s\n", scoreboard_path);
    return 1;
  }
  fprintf(file, "What Longshot's Shaker 2.7 said about this machine, built as a type %u\n",
          crtc_type);
  fprintf(file, "CRTC, module by module and in the order each module's own menu prints.\n\n");
  if (crtc_type != 0) {
    fprintf(file, "Built as, and nearly no more: the chip answers a program asking what it\n");
    fprintf(file, "is, and of its own timing keeps only what the head of crtc.h names. The\n");
    fprintf(file, "rest is a type 0's, and most of what is wrong below is that, and is the\n");
    fprintf(file, "work rather than a fault.\n\n");
  }
  fprintf(file, "%d groups run, %d left to another CRTC type.\n", total_groups_run,
          total_groups_skipped);
  /* The denominator is the lines this reader can score and not the groups
     run: most groups state their verdict in a picture. */
  int agreeing = total_verdicts - total_verdicts_wrong;
  int percentage = total_verdicts == 0 ? 0 : (agreeing * 100 + total_verdicts / 2) / total_verdicts;
  fprintf(file, "%d of them printed self-graded lines, %d in all: %d agree with Longshot's\n",
          total_groups_graded, total_verdicts, agreeing);
  fprintf(file, "silicon and %d do not. That is %d%% of what this reader can score, and no\n",
          total_verdicts_wrong, percentage);
  fprintf(file, "measure at all of the other %d groups.\n\n",
          total_groups_run - total_groups_graded);
  fprintf(file, "A group stands as \"recorded, ungraded\" when it drew a screen of its own but\n");
  fprintf(file, "said what it had to say in a picture or a legend rather than in words\n");
  fprintf(file, "this reader can score, or because it names silicon's value only where the\n");
  fprintf(file, "machine differs from it. A group can also be graded in part: where a row\n");
  fprintf(file, "carries more than one test, the one whose value stands nearest the\n");
  fprintf(file, "expectation is the one scored. A group that got no further than its\n");
  fprintf(file, "module's menu stands as \"showed only the menu\": it was run and kept, not\n");
  fprintf(file, "skipped. A line marked ! is a test this machine failed: either the\n");
  fprintf(file, "module said so, or the machine's value differs from the one Longshot's\n");
  fprintf(file, "silicon produced. A module can also state a failure and mean it for\n");
  fprintf(file, "another type: where a line hands the passing outcome to CRTC types this\n");
  fprintf(file, "machine is not one of, the failure it marks is the answer asked for, and\n");
  fprintf(file, "the line stands unmarked. A few lines here are graded against a title\n");
  fprintf(file, "rather than against anything standing on them: their group puts silicon's\n");
  fprintf(file, "value on a title and the machine's on the screen rows beneath it, so what\n");
  fprintf(file, "they were measured against stands on the screen they came from rather\n");
  fprintf(file, "than on the line copied out of it. Only module D's (R) is graded that\n");
  fprintf(file, "way, under the title \"TST COMP C4/R7 ACTIVE DURING VSYNC FOR DEADLOCK\n");
  fprintf(file, "(EXP #5F)\".\n\n");
  fprintf(file, "Not every difference here is this machine's doing. What a module prints\n");
  fprintf(file, "depends on the phase it booted on, and at least one group has been measured\n");
  fprintf(file, "printing a graded line on one phase and none on another with the emulator\n");
  fprintf(file, "altered in no way at all. When a line leaves this record, rule the phase out\n");
  fprintf(file, "before believing the loss.\n\n");
  fprintf(file, "Nor is every line here one the machine drew once. A screen read while it\n");
  fprintf(file, "was being redrawn is spliced from the two halves, and a tear whose seam\n");
  fprintf(file, "falls on a row boundary is believed, so a group can be recorded printing\n");
  fprintf(file, "the same line twice. Count a group's distinct lines before trusting its\n");
  fprintf(file, "standing.\n\n");
  fprintf(file, "A group that names silicon's value only where it differs is worth\n");
  fprintf(file, "knowing about: it grades itself while the machine is wrong and prints its\n");
  fprintf(file, "measurement alone once it is right, so it leaves the tally above by being\n");
  fprintf(file, "agreed with. A group that makes the comparison itself and prints only\n");
  fprintf(file, "its verdict leaves the tally the same way: the word stands while the\n");
  fprintf(file, "machine is wrong and is gone once it is right. A group falling out of\n");
  fprintf(file, "that count is not the same as a group that could not be read.\n\n");
  fprintf(file, "The copy of this file in the test sources is the one on record, and a sweep\n");
  fprintf(file, "fails on the first line where the two differ. The screens behind these\n");
  fprintf(file, "standings are in the module records written beside the sweep's own copy.\n\n");
  if (fwrite(scoreboard, 1, scoreboard_length, file) != scoreboard_length) {
    fclose(file);
    printf("shaker: %s was not written whole\n", scoreboard_path);
    return 1;
  }
  if (fclose(file) != 0 || scoreboard_overflowed) {
    printf("shaker: %s was not written whole\n", scoreboard_path);
    return 1;
  }

  printf("  scoreboard: %d of %d self-graded lines agree with silicon, %d%%, in %d of the "
         "%d groups run\n",
         agreeing, total_verdicts, percentage, total_groups_graded, total_groups_run);

  TEST_RUN(the_scoreboard_matches_the_one_on_record);
  return TEST_REPORT("shaker");
}
