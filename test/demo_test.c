/*
 * demo_test — play a demo to the end and record what the machine drew.
 *
 * Shaker grades the chips against silicon; this grades nothing. It plays
 * real software from a real disc and writes down the picture, frame by
 * frame, so that a change to the emulator that moves any of it moves a
 * line of a file a human reads before keeping. What the record is worth is
 * the comparison that was made when it was accepted: the frames were set
 * against a capture of the same demo running on a real CPC, part by part.
 * The capture is nobody's to redistribute and is not here; the record it
 * accepted is.
 *
 * A frame is taken at the monitor's retrace and nowhere else. The beam
 * paints the raster over a whole frame's worth of ticks, so a framebuffer
 * read between two retraces holds the top of one frame above the rest of
 * the one before — which is a tear in anything that redraws the whole
 * screen each frame, and looks exactly like a fault in the machine.
 *
 * Needs the firmware and disc images: run `make roms` and `make discs`.
 *
 * Sources:
 * - "Batman Forever" (Batman Group, 2011), the demo itself, published at
 *   https://files.scene.org/view/parties/2011/forever11/cpc/demo/batman_forever.zip
 *   — a 6128 demo that reprograms the CRTC line by line, splits its frames,
 *   and blanks the screen between its parts, which is what makes it worth
 *   playing here. The archive carries four images and no notes of its own.
 * - The release's entry at https://www.pouet.net/prod.php?which=56761, which
 *   is where it asks for a CRTC of type 0, 1, 3 or 4 and 128K. That the
 *   one-disc image wants a double-sided drive is read from the image rather
 *   than from anyone's notes: its extended DSK header records two sides, and
 *   the demo stops with "WRONG DISK FOR YOUR CONFIGURATION" in the machine's
 *   built-in drive, which has one head. Drive B has two.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* opendir and its kin, to clear the pictures a previous run left. These are
   POSIX rather than C99, as the Shaker tier's fork is. */
#include <dirent.h>

#include "cpc.h"
#include "dsk.h"
#include "png.h"
#include "test.h"

/* The demo's one-disc release is double-sided, which the machine's built-in
   drive is not, so it goes in drive B and is started from there. */
#define DISC_IMAGE "batman-forever.dsk"
#define COMMAND "|b:run\"disc"

/* The boot takes 78 frames to reach the prompt, as the firmware tier has
   it; a key must be held long enough for the fifty-times-a-second scan to
   see it. The demo's startup screen asks for a key, and 500 frames is well
   past the moment it appears. */
#define FRAMES_TO_PROMPT 78
#define FRAMES_KEY_HELD 5
/* Not a frame the record keeps: the press runs the machine on while the key
   is held, so a capture point spent on it would leave a hole in the
   record's cadence. */
#define FRAME_OF_THE_KEYPRESS 501

/* The window the picture is cut from, as the command line cuts it: the
   display with a border around it, and the frame flyback left out. */
#define CROP_LEFT 208
#define CROP_TOP 34
#define CROP_WIDTH 768
#define CROP_HEIGHT 272

/* Enough for the largest release of the demo, which is double-sided. */
#define DISC_IMAGE_CAPACITY (1 << 20)
#define MAX_PATH_LENGTH 512

/* How many frames of a moved record are worth a picture. A record that
   moves everywhere is a change to look at rather than a gallery to read. */
#define PICTURES_KEPT 8

static uint8_t ram[0x20000];
static uint8_t rom[0x8000];
static uint8_t amsdos[0x4000];
static uint8_t disc_image[DISC_IMAGE_CAPACITY];
static size_t disc_image_length;
static uint8_t framebuffer[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT];
static cpc_t cpc;
static floppy_t disc;

static const char *rom_directory = "roms";
static const char *disc_directory = "test/data/discs";
static const char *report_directory = "build/demos";
static const char *record_path = "test/demo-batman-forever-crtc0.txt";
static uint8_t crtc_type;
static long frames_to_play = 34000;
static bool the_whole_demo = true;
static long frames_between_captures = 25;

static bool load_file(const char *directory, const char *name, uint8_t *into, size_t capacity,
                      size_t *length, const char *how) {
  char path[MAX_PATH_LENGTH];
  snprintf(path, sizeof path, "%s/%s", directory, name);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    TEST_FAIL("%s is missing; run `%s`", path, how);
    return false;
  }
  size_t read = fread(into, 1, capacity, file);
  fclose(file);
  /* A read that filled the buffer is a file this one cannot hold, since
     every file here is smaller than the room kept for it. */
  if (read == capacity && length != NULL) {
    TEST_FAIL("%s does not fit in %zu bytes", path, capacity);
    return false;
  }
  if (length != NULL) {
    *length = read;
  } else if (read != capacity) {
    TEST_FAIL("%s holds %zu bytes, expected %zu", path, read, capacity);
    return false;
  }
  return true;
}

static bool power_on(void) {
  memset(ram, 0, sizeof ram);
  memset(framebuffer, 0, sizeof framebuffer);
  if (!load_file(rom_directory, "cpc6128.rom", rom, sizeof rom, NULL, "make roms") ||
      !load_file(rom_directory, "amsdos.rom", amsdos, sizeof amsdos, NULL, "make roms")) {
    return false;
  }
  size_t length = 0;
  if (!load_file(disc_directory, DISC_IMAGE, disc_image, sizeof disc_image, &length,
                 "make discs")) {
    return false;
  }
  disc_image_length = length;
  const char *problem = NULL;
  if (!dsk_read(&disc, disc_image, length, &problem)) {
    TEST_FAIL("%s: %s", DISC_IMAGE, problem);
    return false;
  }
  cpc_init(&cpc, ram, sizeof ram, rom, crtc_type);
  cpc_set_upper_rom(&cpc, 0, rom + 0x4000);
  cpc_fit_disc_interface(&cpc, true);
  cpc_set_upper_rom(&cpc, 7, amsdos);
  cpc_connect_monitor(&cpc, framebuffer);
  cpc_set_links(&cpc, true, CPC_MANUFACTURER_AMSTRAD);
  cpc_insert_disc(&cpc, 1, &disc);
  return true;
}

/* One frame as the monitor counts them, which is one retrace to the next.
   A machine that raises no frame sync at all would never return, so the
   wait is bounded by the ticks a standard frame takes and a half. */
static long frames_without_a_sync;

static void run_frame(void) {
  uint16_t before = cpc.monitor.beam_y;
  for (long tick = 0; tick < 3 * CPC_TICKS_PER_STANDARD_FRAME / 2; tick++) {
    cpc_tick(&cpc);
    if (cpc.monitor.beam_y == 0 && before != 0) {
      return;
    }
    before = cpc.monitor.beam_y;
  }
  frames_without_a_sync++;
}

static void run_frames(long frames) {
  for (long frame = 0; frame < frames; frame++) {
    run_frame();
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

static bool type_text(const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    bool shifted = false;
    keyboard_key key = *at == '\n' ? CPC_RETURN : cpc_key_for_character(*at, &shifted);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("this keyboard has no '%c'", *at);
      return false;
    }
    press(key, shifted);
  }
  return true;
}

static uint64_t fnv1a(const uint8_t *bytes, size_t length) {
  uint64_t hash = 0xCBF29CE484222325u;
  for (size_t at = 0; at < length; at++) {
    hash = (hash ^ bytes[at]) * 0x100000001B3u;
  }
  return hash;
}

/* What the record holds of a frame is the whole raster, sync and blanking
   and all, so that a change to any of it moves a line. */
static uint64_t hash_of_the_frame(void) { return fnv1a(framebuffer, sizeof framebuffer); }

/* The pictures of a run are the pictures of that run: one left behind by an
   earlier one says a frame moved when it has not. */
static void clear_the_pictures(void) {
  DIR *directory = opendir(report_directory);
  if (directory == NULL) {
    return;
  }
  for (struct dirent *entry = readdir(directory); entry != NULL; entry = readdir(directory)) {
    if (strncmp(entry->d_name, "moved-", 6) != 0) {
      continue;
    }
    char path[MAX_PATH_LENGTH];
    snprintf(path, sizeof path, "%s/%s", report_directory, entry->d_name);
    remove(path);
  }
  closedir(directory);
}

static void write_picture(long frame) {
  static uint8_t pixels[CROP_WIDTH * CROP_HEIGHT * 3];
  for (int y = 0; y < CROP_HEIGHT; y++) {
    for (int x = 0; x < CROP_WIDTH; x++) {
      uint32_t colour = gate_array_rgb(
          framebuffer[(size_t)(CROP_TOP + y) * CPC_FRAMEBUFFER_WIDTH + CROP_LEFT + x]);
      uint8_t *pixel = pixels + ((size_t)y * CROP_WIDTH + x) * 3;
      pixel[0] = (uint8_t)(colour >> 16);
      pixel[1] = (uint8_t)(colour >> 8);
      pixel[2] = (uint8_t)colour;
    }
  }
  char path[MAX_PATH_LENGTH];
  snprintf(path, sizeof path, "%s/moved-%05ld.png", report_directory, frame);
  if (!png_write(path, pixels, CROP_WIDTH, CROP_HEIGHT)) {
    printf("demo: cannot write %s\n", path);
  }
}

/* The record as it stands, read before the demo runs so that a frame which
   has moved can be kept as a picture while the machine is on it. */
static struct {
  long frames[4096];
  uint64_t hashes[4096];
  int count;
} on_record;

static void read_the_record(void) {
  FILE *file = fopen(record_path, "r");
  if (file == NULL) {
    return;
  }
  char line[256];
  while (fgets(line, sizeof line, file) != NULL) {
    if (on_record.count == (int)(sizeof on_record.frames / sizeof on_record.frames[0])) {
      TEST_FAIL("%s holds more frames than this reader keeps; the pictures of a moved frame\n"
                "    would stop at that point",
                record_path);
      break;
    }
    char *after_the_frame = NULL;
    long frame = strtol(line, &after_the_frame, 10);
    if (after_the_frame == line) {
      continue; /* the prose at the head of the record */
    }
    char *after_the_hash = NULL;
    unsigned long long hash = strtoull(after_the_frame, &after_the_hash, 16);
    if (after_the_hash == after_the_frame) {
      continue;
    }
    on_record.frames[on_record.count] = frame;
    on_record.hashes[on_record.count] = (uint64_t)hash;
    on_record.count++;
  }
  fclose(file);
}

static bool the_record_holds(long frame, uint64_t hash) {
  for (int index = 0; index < on_record.count; index++) {
    if (on_record.frames[index] == frame) {
      return on_record.hashes[index] == hash;
    }
  }
  return false;
}

static void play_the_demo(FILE *written) {
  int pictures = 0;
  for (long frame = 1; frame <= frames_to_play; frame++) {
    if (frame == FRAME_OF_THE_KEYPRESS) {
      press(CPC_SPACE, false);
      frame += 2L * FRAMES_KEY_HELD;
    }
    run_frame();
    if (frame % frames_between_captures != 0) {
      continue;
    }
    uint64_t hash = hash_of_the_frame();
    fprintf(written, "%05ld %016llx\n", frame, (unsigned long long)hash);
    if (on_record.count > 0 && !the_record_holds(frame, hash) && pictures < PICTURES_KEPT) {
      write_picture(frame);
      pictures++;
    }
  }
}

static void trim_trailing_newline(char *line) {
  size_t length = strlen(line);
  while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
    line[--length] = '\0';
  }
}

static char written_path[MAX_PATH_LENGTH];

static void the_frames_match_the_ones_on_record(void) {
  FILE *written = fopen(written_path, "r");
  if (written == NULL) {
    TEST_FAIL("cannot read %s", written_path);
    return;
  }
  FILE *recorded = fopen(record_path, "r");
  if (recorded == NULL) {
    fclose(written);
    TEST_FAIL("no record at %s; the run wrote %s, which a human accepts by comparing it\n"
              "    with a capture of the demo on a real machine before it becomes the record",
              record_path, written_path);
    return;
  }
  long differences = 0;
  long first_difference = 0;
  char first_now[256] = "";
  char first_on_record[256] = "";
  char written_line[256];
  char recorded_line[256];
  bool written_ended = false;
  bool record_ended = false;
  for (long line = 1;; line++) {
    char *from_written = written_ended ? NULL : fgets(written_line, sizeof written_line, written);
    char *from_record = record_ended ? NULL : fgets(recorded_line, sizeof recorded_line, recorded);
    written_ended = from_written == NULL;
    record_ended = from_record == NULL;
    if (from_written == NULL && from_record == NULL) {
      break;
    }
    if (from_written != NULL && from_record != NULL && strcmp(written_line, recorded_line) == 0) {
      continue;
    }
    if (differences++ == 0) {
      first_difference = line;
      snprintf(first_now, sizeof first_now, "%s",
               from_written == NULL ? "(the run ends here)" : written_line);
      snprintf(first_on_record, sizeof first_on_record, "%s",
               from_record == NULL ? "(the record ends here)" : recorded_line);
      trim_trailing_newline(first_now);
      trim_trailing_newline(first_on_record);
    }
  }
  fclose(written);
  fclose(recorded);
  if (differences > 0) {
    TEST_FAIL("%ld line%s of the record moved, the first at line %ld:\n"
              "    on record: %s\n    now:       %s\n"
              "    the frames that moved are in %s, and all of it: diff %s %s",
              differences, differences == 1 ? "" : "s", first_difference, first_on_record,
              first_now, report_directory, record_path, written_path);
  }
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
    record_path = argv[4];
  }
  if (argc > 5 && argv[5][0] != '\0') {
    if (argv[5][1] != '\0' || argv[5][0] < '0' || argv[5][0] > '4') {
      printf("demo: %s names no CRTC; the types are 0 to 4\n", argv[5]);
      return 1;
    }
    crtc_type = (uint8_t)(argv[5][0] - '0');
  }
  if (argc > 6 && argv[6][0] != '\0') {
    char *after = NULL;
    long asked = strtol(argv[6], &after, 10);
    if (after == argv[6] || *after != '\0' || asked <= 0) {
      printf("demo: %s is not a number of frames to play\n", argv[6]);
      return 1;
    }
    frames_to_play = asked;
    the_whole_demo = false;
  }

  if (!power_on()) {
    return TEST_REPORT("demo");
  }
  read_the_record();
  clear_the_pictures();

  snprintf(written_path, sizeof written_path, "%s/frames.txt", report_directory);
  FILE *written = fopen(written_path, "w");
  if (written == NULL) {
    printf("demo: cannot write %s\n", written_path);
    return 1;
  }
  fprintf(written, "What Batman Forever drew on this machine, built as a type %u CRTC: one\n",
          crtc_type);
  fprintf(written, "line per frame kept, the frame's number since the command was typed and a\n");
  fprintf(written, "hash of the whole raster. Frames are taken at the monitor's retrace, every\n");
  fprintf(written, "%ld of them, and %ld are played in all.\n\n", frames_between_captures,
          frames_to_play);
  fprintf(written, "This record grades nothing. ");
  if (crtc_type == 0) {
    fprintf(written, "It says what the machine drew when a human\n");
    fprintf(written, "last compared it, part by part, with a capture of the same demo on real\n");
    fprintf(written, "hardware. ");
  } else {
    /* No capture of this demo on a machine of this type has been set
       against it, and a record must not claim a comparison nobody made. */
    fprintf(written, "No capture of this demo on a real machine of this\n");
    fprintf(written, "type has been set beside it, so it says only what this machine drew on\n");
    fprintf(written, "the day it was taken. ");
  }
  fprintf(written, "A line that moves is a change to look at: the tier writes the\n");
  fprintf(written, "frames that moved as pictures beside this file.\n\n");
  fprintf(written, "The disc played is %s, %zu bytes hashing to %016llx, which\n", DISC_IMAGE,
          disc_image_length, (unsigned long long)fnv1a(disc_image, disc_image_length));
  fprintf(written, "tools/fetch-discs.sh pins: a release other than that one is a record of\n");
  fprintf(written, "its own rather than a machine that has moved. It is started from drive B\n");
  fprintf(written, "with %s, and a key is pressed at frame %d.\n\n", COMMAND,
          FRAME_OF_THE_KEYPRESS);

  run_frames(FRAMES_TO_PROMPT);
  if (!type_text(COMMAND "\n")) {
    fclose(written);
    return TEST_REPORT("demo");
  }
  play_the_demo(written);
  fclose(written);
  if (frames_without_a_sync > 0) {
    printf("demo: %ld frame%s ran their length with no frame sync, and each stands for a\n"
           "      microsecond count rather than a frame the monitor drew\n",
           frames_without_a_sync, frames_without_a_sync == 1 ? "" : "s");
  }

  /* A run cut short has played part of a demo, and a part is not something
     the record can be set against: it writes its frames and stops there. */
  if (!the_whole_demo) {
    printf("demo: %ld frames played and written to %s; the record is set against a whole run\n",
           frames_to_play, written_path);
    return TEST_REPORT("demo");
  }
  TEST_RUN(the_frames_match_the_ones_on_record);
  return TEST_REPORT("demo");
}
