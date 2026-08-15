/*
 * main.c — the emulator's command line.
 *
 * The core allocates nothing and does no I/O; this side does both, so that
 * a WASM host can hand the same core its own buffers and never inherit a
 * FILE pointer. Everything here is deterministic: a run is a machine, a ROM
 * and a number of frames, and the same three produce the same bytes.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpc.h"
#include "png.h"
#include "snapshot.h"

/* One machine per name the --machine option accepts. A name earns its place
   here the day the machine behind it boots to its prompt, not before. */
typedef struct {
  const char *name;
  const char *rom_file;
  uint32_t ram_size;
  const char *description;
} machine_t;

static const machine_t machines[] = {
    {"cpc6128", "cpc6128.rom", 0x20000, "Amstrad CPC 6128, 128K, BASIC 1.1"},
    {"cpc664", "cpc664.rom", 0x10000, "Amstrad CPC 664, 64K, BASIC 1.1"},
    {"cpc464", "cpc464.rom", 0x10000, "Amstrad CPC 464, 64K, BASIC 1.0"},
};
static const size_t machine_count = sizeof machines / sizeof machines[0];

/* The window the monitor's raster is cropped to: the picture with a border
   around it, and the frame flyback left out. The display sits at samples
   272-911 of 1024 and lines 70-269 of 312, so this is 64 samples of border
   either side and a little over 30 lines above and below. */
#define CROP_LEFT 208
#define CROP_TOP 34
#define CROP_WIDTH 768
#define CROP_HEIGHT 272

/* The 6128's boot screen stops changing at frame 42, measured by counting
   the text's pixels frame by frame; the other two settle sooner. Twice that
   costs a fraction of a second and leaves room for a machine that dawdles.
   Wait states moved this only from 39: the firmware's boot waits on the
   300Hz ticker far more than it computes, so a CPU a quarter slower barely
   shows. */
#define DEFAULT_FRAMES 78

/* The firmware scans the keyboard once a frame, off the 50Hz tick, so a key
   must be held for at least one scan to be seen and released for at least
   one more to be seen let go. Three frames each way is comfortable and
   still types nine characters a second of emulated time. */
#define FRAMES_KEY_HELD 3
#define FRAMES_KEY_RELEASED 3

typedef struct {
  const machine_t *machine;
  const char *rom_directory;
  const char *screenshot_path;
  const char *snapshot_path; /* one to load, for `run` */
  const char *save_path;     /* one to write when the frames are done */
  const char *text;
  long frames;
  bool full_raster;
  bool double_lines;
  bool fifty_hz;
} options_t;

static void print_usage(FILE *out) {
  fprintf(out, "usage: emulator boot [options]\n");
  fprintf(out, "       emulator run SNAPSHOT.sna [options]\n\n");
  fprintf(out, "  boot starts a machine from reset; run picks one up from a\n");
  fprintf(out, "  snapshot. Both then run for a number of frames, type what\n");
  fprintf(out, "  they are told to, and write out what they are asked for.\n\n");
  fprintf(out, "  --machine NAME      which machine to build (default cpc6128)\n");
  for (size_t index = 0; index < machine_count; index++) {
    fprintf(out, "                        %-10s %s\n", machines[index].name,
            machines[index].description);
  }
  fprintf(out, "  --roms DIRECTORY    where the ROM images are (default roms)\n");
  fprintf(out, "  --frames N          frames to run before typing (default %d)\n", DEFAULT_FRAMES);
  fprintf(out, "  --type TEXT         type this once the machine has booted\n");
  fprintf(out, "                        \\n Return  \\t Tab  \\e Esc  \\b Del  \\\\ backslash\n");
  fprintf(out, "  --sixty-hz          wire the refresh link for 60Hz\n");
  fprintf(out, "  --screenshot PATH   write the screen here as a PNG\n");
  fprintf(out, "  --save PATH         write the machine here as an SNA snapshot\n");
  fprintf(out, "  --full-raster       the whole beam path, sync and blanking and all\n");
  fprintf(out, "  --no-double         one image line per raster line, squashed\n");
}

static const machine_t *machine_named(const char *name) {
  for (size_t index = 0; index < machine_count; index++) {
    if (strcmp(machines[index].name, name) == 0) {
      return &machines[index];
    }
  }
  return NULL;
}

/* Returns false having reported the problem. */
static bool parse_options(int argc, char **argv, int from, options_t *options) {
  for (int index = from; index < argc; index++) {
    const char *argument = argv[index];
    const char *value = (index + 1 < argc) ? argv[index + 1] : NULL;
    if (strcmp(argument, "--full-raster") == 0) {
      options->full_raster = true;
      continue;
    }
    if (strcmp(argument, "--no-double") == 0) {
      options->double_lines = false;
      continue;
    }
    if (strcmp(argument, "--sixty-hz") == 0) {
      options->fifty_hz = false;
      continue;
    }
    if (value == NULL) {
      fprintf(stderr, "%s needs a value\n", argument);
      return false;
    }
    index++;
    if (strcmp(argument, "--machine") == 0) {
      options->machine = machine_named(value);
      if (options->machine == NULL) {
        fprintf(stderr, "no machine is called %s\n", value);
        return false;
      }
    } else if (strcmp(argument, "--roms") == 0) {
      options->rom_directory = value;
    } else if (strcmp(argument, "--screenshot") == 0) {
      options->screenshot_path = value;
    } else if (strcmp(argument, "--type") == 0) {
      options->text = value;
    } else if (strcmp(argument, "--save") == 0) {
      options->save_path = value;
    } else if (strcmp(argument, "--frames") == 0) {
      char *end = NULL;
      options->frames = strtol(value, &end, 10);
      if (*end != '\0' || options->frames < 1) {
        fprintf(stderr, "--frames wants a positive number, not %s\n", value);
        return false;
      }
    } else {
      fprintf(stderr, "no such option: %s\n", argument);
      return false;
    }
  }
  return true;
}

static bool load_rom(const char *directory, const char *file, uint8_t *rom, size_t size) {
  char path[1024];
  int length = snprintf(path, sizeof path, "%s/%s", directory, file);
  if (length < 0 || (size_t)length >= sizeof path) {
    fprintf(stderr, "the path to %s is too long\n", file);
    return false;
  }
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    fprintf(stderr, "run 'make roms' to fetch the firmware images\n");
    return false;
  }
  size_t read = fread(rom, 1, size, handle);
  fclose(handle);
  if (read != size) {
    fprintf(stderr, "%s holds %zu bytes, expected %zu\n", path, read, size);
    return false;
  }
  return true;
}

/* Crop the raster and turn hardware colour codes into pixels. */
static uint8_t *render(const uint8_t *framebuffer, const options_t *options, uint32_t *width_out,
                       uint32_t *height_out) {
  uint32_t left = options->full_raster ? 0 : CROP_LEFT;
  uint32_t top = options->full_raster ? 0 : CROP_TOP;
  uint32_t width = options->full_raster ? CPC_FRAMEBUFFER_WIDTH : CROP_WIDTH;
  uint32_t lines = options->full_raster ? CPC_FRAMEBUFFER_HEIGHT : CROP_HEIGHT;
  uint32_t repeat = options->double_lines ? 2 : 1;
  uint32_t height = lines * repeat;

  uint8_t *pixels = malloc((size_t)width * height * 3);
  if (pixels == NULL) {
    fprintf(stderr, "cannot hold a %ux%u image\n", width, height);
    return NULL;
  }
  uint8_t *out = pixels;
  for (uint32_t line = 0; line < lines; line++) {
    const uint8_t *row = framebuffer + (size_t)(top + line) * CPC_FRAMEBUFFER_WIDTH + left;
    for (uint32_t again = 0; again < repeat; again++) {
      for (uint32_t column = 0; column < width; column++) {
        uint32_t rgb = gate_array_rgb(row[column]);
        *out++ = (uint8_t)(rgb >> 16);
        *out++ = (uint8_t)(rgb >> 8);
        *out++ = (uint8_t)rgb;
      }
    }
  }
  *width_out = width;
  *height_out = height;
  return pixels;
}

/* Read a whole file into a fresh buffer; the caller frees it. */
static uint8_t *read_file(const char *path, size_t *size_out) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    return NULL;
  }
  fseek(handle, 0, SEEK_END);
  long size = ftell(handle);
  fseek(handle, 0, SEEK_SET);
  if (size <= 0) {
    fprintf(stderr, "%s is empty\n", path);
    fclose(handle);
    return NULL;
  }
  uint8_t *contents = malloc((size_t)size);
  if (contents == NULL || fread(contents, 1, (size_t)size, handle) != (size_t)size) {
    fprintf(stderr, "cannot read %s\n", path);
    free(contents);
    fclose(handle);
    return NULL;
  }
  fclose(handle);
  *size_out = (size_t)size;
  return contents;
}

static bool write_file(const char *path, const uint8_t *contents, size_t size) {
  FILE *handle = fopen(path, "wb");
  if (handle == NULL) {
    fprintf(stderr, "cannot open %s for writing\n", path);
    return false;
  }
  bool ok = fwrite(contents, 1, size, handle) == size;
  if (fclose(handle) != 0 || !ok) {
    fprintf(stderr, "cannot write %s\n", path);
    return false;
  }
  return true;
}

static bool save_snapshot(const cpc_t *cpc, const char *path) {
  size_t size = snapshot_size(cpc);
  uint8_t *contents = malloc(size);
  if (contents == NULL) {
    fprintf(stderr, "cannot hold a snapshot of %zu bytes\n", size);
    return false;
  }
  const char *problem = NULL;
  bool ok = snapshot_save(cpc, contents, size, &problem);
  if (!ok) {
    fprintf(stderr, "cannot make a snapshot: %s\n", problem);
  } else {
    ok = write_file(path, contents, size);
  }
  free(contents);
  return ok;
}

static void run_frames(cpc_t *cpc, long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    cpc_tick(cpc);
  }
}

/* Hold a key, with shift if the character needs it, then let go. */
static void press_and_release(cpc_t *cpc, keyboard_key key, bool shifted) {
  if (shifted) {
    keyboard_press(&cpc->keyboard, KEYBOARD_SHIFT);
  }
  keyboard_press(&cpc->keyboard, key);
  run_frames(cpc, FRAMES_KEY_HELD);
  keyboard_release_all(&cpc->keyboard);
  run_frames(cpc, FRAMES_KEY_RELEASED);
}

/* Type text, taking the escapes the usage message lists. Returns false
 * having reported a character this keyboard cannot produce. */
static bool type_text(cpc_t *cpc, const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    char character = *at;
    keyboard_key key = KEYBOARD_NO_KEY;
    bool shifted = false;
    if (character == '\\' && at[1] != '\0') {
      at++;
      switch (*at) {
        case 'n':
          key = KEYBOARD_RETURN;
          break;
        case 't':
          key = KEYBOARD_TAB;
          break;
        case 'e':
          key = KEYBOARD_ESCAPE;
          break;
        case 'b':
          key = KEYBOARD_DELETE;
          break;
        case '\\':
          key = keyboard_key_for_character('\\', &shifted);
          break;
        default:
          fprintf(stderr, "no such escape: \\%c\n", *at);
          return false;
      }
    } else {
      key = keyboard_key_for_character(character, &shifted);
      if (key == KEYBOARD_NO_KEY) {
        fprintf(stderr, "this keyboard has no '%c'\n", character);
        return false;
      }
    }
    press_and_release(cpc, key, shifted);
  }
  return true;
}

static int run_machine(int argc, char **argv, bool from_snapshot) {
  options_t options = {
      .machine = &machines[0],
      .rom_directory = "roms",
      .screenshot_path = NULL,
      .text = NULL,
      .snapshot_path = NULL,
      .save_path = NULL,
      .frames = DEFAULT_FRAMES,
      .full_raster = false,
      .double_lines = true,
      .fifty_hz = true,
  };
  int first_option = 2;
  if (from_snapshot) {
    if (argc < 3 || argv[2][0] == '-') {
      fprintf(stderr, "run needs a snapshot to start from\n");
      return 1;
    }
    options.snapshot_path = argv[2];
    first_option = 3;
  }
  if (!parse_options(argc, argv, first_option, &options)) {
    return 1;
  }

  static uint8_t rom[0x8000];
  if (!load_rom(options.rom_directory, options.machine->rom_file, rom, sizeof rom)) {
    return 1;
  }

  uint8_t *ram = calloc(options.machine->ram_size, 1);
  uint8_t *framebuffer = calloc((size_t)CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT, 1);
  cpc_t *cpc = calloc(1, sizeof *cpc);
  if (ram == NULL || framebuffer == NULL || cpc == NULL) {
    fprintf(stderr, "cannot hold the machine\n");
    free(cpc);
    free(framebuffer);
    free(ram);
    return 1;
  }

  /* The operating system fills the lower 16K, BASIC the upper as ROM 0. */
  cpc_init(cpc, ram, options.machine->ram_size, rom);
  cpc_set_upper_rom(cpc, 0, rom + 0x4000);
  cpc_connect_monitor(cpc, framebuffer);
  cpc_set_links(cpc, options.fifty_hz, CPC_MANUFACTURER_AMSTRAD);

  int status = 0;
  if (options.snapshot_path != NULL) {
    size_t size = 0;
    uint8_t *contents = read_file(options.snapshot_path, &size);
    if (contents == NULL) {
      status = 1;
    } else {
      const char *problem = NULL;
      if (!snapshot_load(cpc, contents, size, &problem)) {
        fprintf(stderr, "%s %s\n", options.snapshot_path, problem);
        status = 1;
      }
      free(contents);
    }
  }
  if (status == 0) {
    run_frames(cpc, options.frames);
  }
  if (status == 0 && options.text != NULL && !type_text(cpc, options.text)) {
    status = 1;
  }
  if (status == 0 && options.save_path != NULL) {
    cpc_finish_instruction(cpc);
    if (!save_snapshot(cpc, options.save_path)) {
      status = 1;
    }
  }
  if (status == 0 && options.screenshot_path != NULL) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t *pixels = render(framebuffer, &options, &width, &height);
    if (pixels == NULL || !png_write(options.screenshot_path, pixels, width, height)) {
      status = 1;
    } else {
      printf("%s: %ld frames, %ux%u to %s\n", options.machine->name, options.frames, width, height,
             options.screenshot_path);
    }
    free(pixels);
  } else if (status == 0) {
    printf("%s: %ld frames\n", options.machine->name, options.frames);
  }

  free(cpc);
  free(framebuffer);
  free(ram);
  return status;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(stderr);
    return 1;
  }
  if (strcmp(argv[1], "boot") == 0) {
    return run_machine(argc, argv, false);
  }
  if (strcmp(argv[1], "run") == 0) {
    return run_machine(argc, argv, true);
  }
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
    print_usage(stdout);
    return 0;
  }
  fprintf(stderr, "no such command: %s\n", argv[1]);
  print_usage(stderr);
  return 1;
}
