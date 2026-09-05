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
 * it cannot do is decide which of those screens is a pass, because Shaker
 * says so differently in each group — some print a value beside the value
 * they expected, some print a legend and leave the verdict to the picture,
 * and some ask the reader to watch a line flash. A group earns a rule here
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpc.h"
#include "dsk.h"
#include "png.h"
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
   firmware_test does. The module then loads off the disc, which the slowest
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
   something that moves. */
#define SAMPLE_STEP_FRAMES 5
#define MAX_SAMPLES_WITHOUT_NEWS 40
#define MAX_SAMPLES_PER_GROUP 300

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

/* The machine is file-scope because the controller's pointers to its drives
   point into it: cpc_init sets fdc.drives[unit] to &cpc.drives[unit], so a
   copy of the structure holds addresses inside the original. That is what
   makes the save and restore below a round trip rather than a clone — a
   saved copy is not a usable machine, and the day this becomes a local or
   an array the controller would drive the wrong drive. */
static cpc_t cpc;
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
static const char *scoreboard_on_record = "test/shaker-scoreboard.txt";

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
static int total_groups_run;
static int total_groups_skipped;
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

static void run_frames(long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    cpc_tick(&cpc);
  }
}

static void press(keyboard_key key, bool shifted) {
  if (shifted) {
    keyboard_press(&cpc.keyboard, KEYBOARD_SHIFT);
  }
  keyboard_press(&cpc.keyboard, key);
  run_frames(FRAMES_KEY_HELD);
  keyboard_release_all(&cpc.keyboard);
  run_frames(FRAMES_KEY_HELD);
}

static void type_text(const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    bool shifted = false;
    keyboard_key key = *at == '\n' ? KEYBOARD_RETURN : keyboard_key_for_character(*at, &shifted);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("this keyboard has no '%c'", *at);
      return;
    }
    press(key, shifted);
  }
}

/* The menus offer keys that carry no character, which is why
   keyboard_key_for_character cannot reach them. Every key a menu names is
   either one character or one of these. */
static const struct {
  const char *name;
  keyboard_key key;
} named_keys[] = {
    {"COPY", KEYBOARD_COPY},     {"CAPS", KEYBOARD_CAPS_LOCK}, {"TAB", KEYBOARD_TAB},
    {"RETURN", KEYBOARD_RETURN}, {"CTRL", KEYBOARD_CONTROL},   {"F0", KEYBOARD_FUNCTION_0},
    {"SPACE", KEYBOARD_SPACE},
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
  return keyboard_key_for_character(character, shifted);
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
  cpc_init(&cpc, ram, sizeof ram, rom);
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
    for (int column = 0; column < COLUMNS * GLYPH_WIDTH; column += 4) {
      int x = left + column;
      int y = top + row;
      if (x < 0 || x >= CPC_FRAMEBUFFER_WIDTH || y < 0 || y >= CPC_FRAMEBUFFER_HEIGHT) {
        continue;
      }
      counts[framebuffer[(size_t)y * CPC_FRAMEBUFFER_WIDTH + (size_t)x] & 0x1F]++;
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

static void cut_glyph(int left, int top, int row, int column, uint8_t behind, uint8_t glyph[8]) {
  for (int scanline = 0; scanline < GLYPH_HEIGHT; scanline++) {
    uint8_t bits = 0;
    for (int pixel = 0; pixel < GLYPH_WIDTH; pixel++) {
      int x = left + column * GLYPH_WIDTH + pixel;
      int y = top + row * GLYPH_HEIGHT + scanline;
      if (x < 0 || x >= CPC_FRAMEBUFFER_WIDTH || y < 0 || y >= CPC_FRAMEBUFFER_HEIGHT) {
        continue;
      }
      if (framebuffer[(size_t)y * CPC_FRAMEBUFFER_WIDTH + (size_t)x] != behind) {
        bits |= (uint8_t)(0x80u >> pixel);
      }
    }
    glyph[scanline] = bits;
  }
}

static char code_of_glyph(const uint8_t glyph[8]) {
  for (int code = FIRST_CODE; code <= LAST_CODE; code++) {
    if (memcmp(rom + FONT_IN_ROM + (size_t)code * 8, glyph, 8) == 0) {
      return (char)code;
    }
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
      uint8_t glyph[8];
      cut_glyph(left, top, row, column, behind, glyph);
      bool blank = true;
      for (int scanline = 0; scanline < GLYPH_HEIGHT; scanline++) {
        if (glyph[scanline] != 0) {
          blank = false;
        }
      }
      if (blank) {
        continue;
      }
      (*glyphs_drawn)++;
      char code = code_of_glyph(glyph);
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
      uint8_t glyph[8];
      cut_glyph(grid_left, grid_top, row, column, behind, glyph);
      bool blank = true;
      for (int scanline = 0; scanline < GLYPH_HEIGHT; scanline++) {
        if (glyph[scanline] != 0) {
          blank = false;
        }
      }
      char found = blank ? ' ' : code_of_glyph(glyph);
      if (!blank) {
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
  for (size_t index = 0; index < CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT; index++) {
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

/* Which CRTC type a group belongs to is stated at the head of its label and
   nowhere else: "CRTC 2 RVMB" is type 2's, "CRTC 0.2" is shared, "ALL" is
   everyone's. A type named later in a label is a remark about the test, not
   its scope — "ALL : CRTC 3/4 PARITY" belongs to every type, and "SHAKER
   KILLER 2 (WARNING : NOT RELIABLE ON CRTC 1)" is a caution. The one label
   that overrides its own head is the one that says so: "OPEN TO OTHER
   CRTC'S", which Shaker prints where a type's test has been found to hold
   for the rest. */
static bool applies_to_type_0(const char *label) {
  if (strstr(label, "OPEN TO OTHER") != NULL) {
    return true;
  }
  const char *at = label;
  while (*at == ' ') {
    at++;
  }
  if (strncmp(at, "CRTC", 4) != 0) {
    return true;
  }
  at += 4;
  while (*at == ' ') {
    at++;
  }
  if (*at < '0' || *at > '9') {
    return true;
  }
  for (; (*at >= '0' && *at <= '9') || *at == '/' || *at == '.'; at++) {
    if (*at == '0') {
      return true;
    }
  }
  return false;
}

/* The whole bracket has to match: a label can carry "(2.1.0)" beside its
   count, and a conversion that stopped early would read that as a number of
   tests. The width keeps a screenful of arbitrary digits inside an int. */
static int declared_test_count(const char *label) {
  const char *at = label;
  while ((at = strchr(at, '(')) != NULL) {
    int count = 0;
    int end = 0;
    if ((sscanf(at, "(%8d TST)%n", &count, &end) == 1 && end > 0) ||
        (sscanf(at, "(%8d INTERACTIVE TST)%n", &count, &end) == 1 && end > 0)) {
      return count;
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
  entry->applies_to_this_crtc_type = applies_to_type_0(entry->label);
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

/* Some groups grade themselves. Where they do, the line carries the value
   the machine produced, then in brackets the value real silicon produced,
   and the word WRONG when the two disagree:

       >>>>>> DELAY TO VSYNC:#0030 (EXP:#00F7)  WRONG

   That is Longshot's convention, read off the modules' own output. The
   value before the bracket is what tells a grading from a legend naming
   the value a test is about to check, which carries no measurement of its
   own. Only this convention is counted; a group that states its verdict
   another way is recorded and left ungraded. */
#define MAX_VERDICTS 192
static char verdicts[MAX_VERDICTS][COLUMNS + 1];
static int verdict_count;
static int verdicts_dropped;

static bool is_verdict(const char *line) {
  const char *expected = strstr(line, "(EXP");
  if (expected == NULL) {
    return false;
  }
  for (const char *at = line; at < expected; at++) {
    if (*at == '#') {
      return true;
    }
  }
  return false;
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
static int collect_verdicts(void) {
  int added = 0;
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
    if (!is_verdict(line) || strchr(line, '?') != NULL) {
      continue;
    }
    if (!appears_in_previous_screen(line)) {
      continue;
    }
    bool known = false;
    for (int index = 0; index < verdict_count; index++) {
      if (strcmp(verdicts[index], line) == 0) {
        known = true;
      }
    }
    if (known) {
      continue;
    }
    if (verdict_count == MAX_VERDICTS) {
      verdicts_dropped++;
      added++;
      continue;
    }
    memcpy(verdicts[verdict_count++], line, strlen(line) + 1);
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
  char path[4096];
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
    bool news = capture_screen(module, entry->key, frame, read_screen());
    if (collect_verdicts() > 0) {
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
    if (strstr(verdicts[index], "WRONG") != NULL) {
      wrong++;
    }
  }
  if (verdict_count > 0) {
    fprintf(report, "  %d self-graded line%s, WRONG in %d of them%s\n", verdict_count,
            verdict_count == 1 ? "" : "s", wrong,
            verdicts_dropped > 0 ? ", and more than this record holds" : "");
    for (int index = 0; index < verdict_count; index++) {
      fprintf(report, "  %s %s\n", strstr(verdicts[index], "WRONG") != NULL ? "!" : " ",
              verdicts[index]);
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
    add_to_scoreboard("    %s %s\n", strstr(verdicts[index], "WRONG") != NULL ? "!" : " ",
                      verdicts[index]);
  }
  total_verdicts += verdict_count;
  total_verdicts_wrong += wrong;
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

  char path[4096];
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
  printf("  module %s: %d groups run, %d left to another CRTC type, report in %s\n", module, ran,
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
   heading all look alike out of context. */
static bool is_a_group_line(const char *line) {
  return line[0] >= 'A' && line[0] <= 'E' && line[1] == ' ' && line[2] == '(';
}

/* A difference here is not a fault to be fixed but a reading to be judged,
   so every line that moved is counted, the first is named under its group,
   and the choice of what to keep is left to the human. */
static void the_scoreboard_matches_the_one_on_record(void) {
  if (only_module != NULL || only_group != NULL) {
    return;
  }
  char written_path[4096];
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
  while (true) {
    char *from_written = fgets(written_line, sizeof written_line, written);
    char *from_record = fgets(recorded_line, sizeof recorded_line, on_record);
    if (from_written == NULL && from_record == NULL) {
      break;
    }
    line++;
    if (from_written != NULL && from_record != NULL && strcmp(written_line, recorded_line) == 0) {
      if (is_a_group_line(recorded_line)) {
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
  if (only_group != NULL && only_module == NULL) {
    printf("shaker: a group needs the module it belongs to — pass MODULE too\n");
    return 1;
  }

  TEST_RUN(module_a_is_recorded);
  TEST_RUN(module_b_is_recorded);
  TEST_RUN(module_c_is_recorded);
  TEST_RUN(module_d_is_recorded);
  TEST_RUN(module_e_is_recorded);

  /* A run of one module or one group has walked part of the menu, and a
     part is not something the record can be set against. It writes its
     module records and stops there, so that the file the comparison reads
     is always a whole sweep. */
  if (only_module != NULL || only_group != NULL) {
    return TEST_REPORT("shaker");
  }

  char scoreboard_path[4096];
  snprintf(scoreboard_path, sizeof scoreboard_path, "%s/scoreboard.txt", report_directory);
  FILE *file = fopen(scoreboard_path, "w");
  if (file == NULL) {
    printf("shaker: cannot write %s\n", scoreboard_path);
    return 1;
  }
  fprintf(file, "What Longshot's Shaker 2.7 said about this machine, a type 0 CRTC,\n");
  fprintf(file, "module by module and in the order each module's own menu prints.\n\n");
  fprintf(file, "%d groups run, %d left to another CRTC type.\n", total_groups_run,
          total_groups_skipped);
  fprintf(file, "%d self-graded lines, WRONG in %d of them.\n\n", total_verdicts,
          total_verdicts_wrong);
  fprintf(file, "A group stands as \"recorded, ungraded\" when it drew a screen of its own but\n");
  fprintf(file, "said what it had to say in a picture or a legend rather than in words this\n");
  fprintf(file, "reader can score. It was run and kept, not skipped. A line marked ! is a test\n");
  fprintf(file, "Longshot's own module says this machine fails.\n\n");
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

  TEST_RUN(the_scoreboard_matches_the_one_on_record);
  return TEST_REPORT("shaker");
}
