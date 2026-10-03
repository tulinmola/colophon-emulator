/*
 * bench — how fast the machine runs, measured on the work it is for.
 *
 * It plays the demo the software tier plays, with nothing watching, and
 * says how long the processor spent on it and how many times real time that
 * is. Processor time rather than the wall clock, because nothing here waits
 * for anything: the two differ only by what else the machine was doing.
 * There is no threshold here and no pass or fail: machines differ, and a
 * number is only worth the machine it was taken on. What it is for is the
 * before and after of a change made for speed, taken on one machine,
 * minutes apart.
 *
 * Frames are counted in ticks rather than at the monitor's retrace, so that
 * two builds are asked for exactly the same work even where one of them
 * changes what the beam does.
 *
 * Needs the firmware and disc images: run `make roms` and `make discs`.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpc.h"
#include "dsk.h"

#define DISC_IMAGE "batman-forever.dsk"
#define COMMAND "|b:run\"disc\n"
#define FRAMES_TO_PROMPT 78
#define FRAMES_KEY_HELD 5
#define FRAME_OF_THE_KEYPRESS 501
/* The frames a run takes at the least: the key press and the frames it is
   held for, which the loop below plays and counts. Three minutes of demo by
   default, which is long enough for the parts to vary and short enough to
   run twice while deciding something. */
#define FRAMES_LEAST (FRAME_OF_THE_KEYPRESS + 2 * FRAMES_KEY_HELD)
#define FRAMES_BY_DEFAULT 8000
#define DISC_IMAGE_CAPACITY (1 << 20)
#define MAX_PATH_LENGTH 512

static uint8_t ram[0x20000];
static uint8_t rom[0x8000];
static uint8_t amsdos[0x4000];
static uint8_t disc_image[DISC_IMAGE_CAPACITY];
static uint8_t framebuffer[CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT];
static cpc_t cpc;
static floppy_t disc;

/* A file the whole of which must fit: a benchmark run on half a ROM or a
   truncated disc is a number that stands for nothing. */
static size_t read_file(const char *directory, const char *name, uint8_t *into, size_t capacity,
                        bool exactly) {
  char path[MAX_PATH_LENGTH];
  snprintf(path, sizeof path, "%s/%s", directory, name);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "bench: %s is missing; run `make roms` and `make discs`\n", path);
    exit(1);
  }
  size_t read = fread(into, 1, capacity, file);
  fclose(file);
  if (exactly ? read != capacity : read == capacity) {
    fprintf(stderr, "bench: %s holds %zu bytes, and this reader keeps %zu\n", path, read, capacity);
    exit(1);
  }
  return read;
}

static void run_frames(long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    cpc_tick(&cpc);
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

int main(int argc, char **argv) {
  const char *rom_directory = argc > 1 ? argv[1] : "roms";
  const char *disc_directory = argc > 2 ? argv[2] : "test/data/discs";
  /* Below the key press the demo is still on the screen that asks for one,
     which is a different program to measure and not comparable with a run
     that reaches the demo proper. */
  long frames = FRAMES_BY_DEFAULT;
  if (argc > 3 && argv[3][0] != '\0') {
    char *after = NULL;
    errno = 0;
    frames = strtol(argv[3], &after, 10);
    if (after == argv[3] || *after != '\0' || errno != 0 || frames < FRAMES_LEAST ||
        frames > LONG_MAX / CPC_TICKS_PER_STANDARD_FRAME) {
      fprintf(stderr, "bench: frames to play must be a number from %d up, and %s is not\n",
              FRAMES_LEAST, argv[3]);
      return 1;
    }
  }

  read_file(rom_directory, "cpc6128.rom", rom, sizeof rom, true);
  read_file(rom_directory, "amsdos.rom", amsdos, sizeof amsdos, true);
  size_t length = read_file(disc_directory, DISC_IMAGE, disc_image, sizeof disc_image, false);
  const char *problem = NULL;
  if (!dsk_read(&disc, disc_image, length, &problem)) {
    fprintf(stderr, "bench: %s: %s\n", DISC_IMAGE, problem);
    return 1;
  }

  cpc_init(&cpc, ram, sizeof ram, rom, 0);
  cpc_set_upper_rom(&cpc, 0, rom + 0x4000);
  cpc_fit_disc_interface(&cpc, true);
  cpc_set_upper_rom(&cpc, 7, amsdos);
  cpc_connect_monitor(&cpc, framebuffer);
  cpc_set_links(&cpc, true, CPC_MANUFACTURER_AMSTRAD);
  cpc_insert_disc(&cpc, 1, &disc);

  /* The boot and the command are not timed: the clock starts where the demo
     does. */
  run_frames(FRAMES_TO_PROMPT);
  for (const char *at = COMMAND; *at != '\0'; at++) {
    bool shifted = false;
    keyboard_key key = *at == '\n' ? CPC_RETURN : cpc_key_for_character(*at, &shifted);
    if (key == KEYBOARD_NO_KEY) {
      fprintf(stderr, "bench: this keyboard has no '%c'\n", *at);
      return 1;
    }
    press(key, shifted);
  }

  clock_t started = clock();
  for (long frame = 1; frame <= frames; frame++) {
    if (frame == FRAME_OF_THE_KEYPRESS) {
      press(CPC_SPACE, false);
      frame += 2L * FRAMES_KEY_HELD;
    }
    run_frames(1);
  }
  double seconds = (double)(clock() - started) / CLOCKS_PER_SEC;
  printf("bench: %ld frames of the demo in %.2f seconds, %.1f times real time\n", frames, seconds,
         (double)frames / 50.0 / seconds);
  return 0;
}
