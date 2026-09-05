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
#include "dsk.h"
#include "png.h"
#include "snapshot.h"
#include "spectrum.h"

/* One machine per name the --machine option accepts. A name earns its place
   here the day the machine behind it boots to its prompt, not before. */
#define CPC_DEFAULT_FRAMES 78

/* A Spectrum shows its copyright message by frame 50 and its cursor by 65,
   but does not take a keystroke until 85 — measured by typing PRINT 2+2 one
   frame later each time and reading the answer back off the screen. Before
   that the first key is dropped and the rest arrive as nonsense. This is
   half again as long, which leaves room and still costs under three seconds
   of the machine's own time. */
#define SPECTRUM_DEFAULT_FRAMES 128

/* The window a machine's raster is cropped to: the picture with a border
   around it, and the frame flyback left out. On a CPC the display sits at
   samples 272-911 of 1024 and lines 70-269 of 312, so this is 64 samples of
   border either side and a little over 30 lines above and below. */
#define CPC_CROP_LEFT 208
#define CPC_CROP_TOP 34
#define CPC_CROP_WIDTH 768
#define CPC_CROP_HEIGHT 272

/* The Spectrum's picture sits at samples 144-399 of 448 and, once the frame
   sync has taken its eight lines, at raster lines 56-247 of 312. This is the
   whole 24 T-states of border the ULA draws either side, and enough lines
   above and below to leave the window four across for three down. */
#define SPECTRUM_CROP_LEFT 96
#define SPECTRUM_CROP_TOP 20
#define SPECTRUM_CROP_WIDTH 352
#define SPECTRUM_CROP_HEIGHT 264

typedef enum {
  MACHINE_CPC,
  MACHINE_SPECTRUM,
} machine_kind;

typedef struct {
  machine_kind kind;
  const char *name;
  const char *rom_file;
  uint32_t ram_size;
  bool disc_interface; /* built in; a 464 gets one plugged in with a disc */
  const char *description;

  /* The raster this machine's monitor paints, the window a picture is cut
     from it, and what one sample of it is worth on the cable. A host cannot
     ask the core any of this: the core hands over colour codes and leaves
     the reading of them to whoever plugged the monitor in. */
  uint32_t raster_width;
  uint32_t raster_height;
  uint32_t crop_left;
  uint32_t crop_top;
  uint32_t crop_width;
  uint32_t crop_height;
  uint32_t (*sample_rgb)(uint8_t sample);
  /* Frames to run before typing. Measured per machine: a keystroke sent
     before the firmware is reading the keyboard is not queued, it is lost,
     and what follows it lands in the wrong order. */
  long default_frames;
  /* A CPC's raster is sixteen samples to the microsecond and 312 lines, so
     its picture is far wider than it is tall until every line is drawn
     twice. A Spectrum's is already near enough square. */
  bool double_lines;
} machine_t;

/* The picture each machine paints, named by field so that adding one to the
   struct cannot silently shift another out of place. */
#define CPC_RASTER                                                                                 \
  .raster_width = CPC_FRAMEBUFFER_WIDTH, .raster_height = CPC_FRAMEBUFFER_HEIGHT,                  \
  .crop_left = CPC_CROP_LEFT, .crop_top = CPC_CROP_TOP, .crop_width = CPC_CROP_WIDTH,              \
  .crop_height = CPC_CROP_HEIGHT, .sample_rgb = gate_array_rgb,                                    \
  .default_frames = CPC_DEFAULT_FRAMES, .double_lines = true
#define SPECTRUM_RASTER                                                                            \
  .raster_width = SPECTRUM_FRAMEBUFFER_WIDTH, .raster_height = SPECTRUM_FRAMEBUFFER_HEIGHT,        \
  .crop_left = SPECTRUM_CROP_LEFT, .crop_top = SPECTRUM_CROP_TOP,                                  \
  .crop_width = SPECTRUM_CROP_WIDTH, .crop_height = SPECTRUM_CROP_HEIGHT, .sample_rgb = ula_rgb,   \
  .default_frames = SPECTRUM_DEFAULT_FRAMES, .double_lines = false

static const machine_t machines[] = {
    {.kind = MACHINE_CPC,
     .name = "cpc6128",
     .rom_file = "cpc6128.rom",
     .ram_size = 0x20000,
     .disc_interface = true,
     .description = "Amstrad CPC 6128, 128K, BASIC 1.1",
     CPC_RASTER},
    {.kind = MACHINE_CPC,
     .name = "cpc664",
     .rom_file = "cpc664.rom",
     .ram_size = 0x10000,
     .disc_interface = true,
     .description = "Amstrad CPC 664, 64K, BASIC 1.1",
     CPC_RASTER},
    {.kind = MACHINE_CPC,
     .name = "cpc464",
     .rom_file = "cpc464.rom",
     .ram_size = 0x10000,
     .disc_interface = false,
     .description = "Amstrad CPC 464, 64K, BASIC 1.0",
     CPC_RASTER},
    {.kind = MACHINE_SPECTRUM,
     .name = "spectrum48",
     .rom_file = "spectrum48.rom",
     .ram_size = SPECTRUM_RAM_48K,
     .disc_interface = false,
     .description = "Sinclair ZX Spectrum 48K",
     SPECTRUM_RASTER},
};
static const size_t machine_count = sizeof machines / sizeof machines[0];

/* The 6128's boot screen stops changing at frame 42, measured by counting
   the text's pixels frame by frame; the other two settle sooner. Twice that
   costs a fraction of a second and leaves room for a machine that dawdles.
   Wait states moved this only from 39: the firmware's boot waits on the
   300Hz ticker far more than it computes, so a CPU a quarter slower barely
   shows. */

/* Both firmwares scan the keyboard off their 50Hz interrupt, so a key must
   be held for at least one scan to be seen and released for at least one
   more to be seen let go. Three frames each way is comfortable on either
   machine and still types nine characters a second of emulated time. */
#define FRAMES_KEY_HELD 3
#define FRAMES_KEY_RELEASED 3

typedef struct {
  const machine_t *machine;
  const char *rom_directory;
  const char *screenshot_path;
  const char *writes_path;
  const char *snapshot_path;  /* one to load, for `run` */
  const char *save_path;      /* one to write when the frames are done */
  const char *disc_paths[2];  /* images for drives A and B */
  const char *save_disc_path; /* where drive A's disc goes when done */
  const char *text;
  long frames;
  long frames_after; /* to run once the typing is done */
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
  fprintf(out, "  --machine NAME      which machine to build; there is no default\n");
  for (size_t index = 0; index < machine_count; index++) {
    fprintf(out, "                        %-10s %s\n", machines[index].name,
            machines[index].description);
  }
  fprintf(out, "  --roms DIRECTORY    where the ROM images are (default roms)\n");
  fprintf(out, "  --frames N          frames to run before typing (each machine its own)\n");
  fprintf(out, "  --type TEXT         type this once the machine has booted\n");
  fprintf(out, "                        \\n Return  \\t Tab  \\e Esc  \\b Del  \\\\ backslash\n");
  fprintf(out, "  --wait N            frames to run after typing (default 0)\n");
  fprintf(out, "  --sixty-hz          wire the refresh link for 60Hz\n");
  fprintf(out, "  --screenshot PATH   write the screen here as a PNG\n");
  fprintf(out, "  --writes PATH       write a map of memory writes here as a PNG\n");
  fprintf(out, "  --save PATH         write the machine here as an SNA snapshot\n");
  fprintf(out, "  --disc PATH         put this DSK image in drive A\n");
  fprintf(out, "  --disc-b PATH       and this one in drive B\n");
  fprintf(out, "  --save-disc PATH    write drive A's disc here when done\n");
  fprintf(out, "                      the six above, and run, are a CPC's alone\n");
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
    } else if (strcmp(argument, "--writes") == 0) {
      options->writes_path = value;
    } else if (strcmp(argument, "--type") == 0) {
      options->text = value;
    } else if (strcmp(argument, "--save") == 0) {
      options->save_path = value;
    } else if (strcmp(argument, "--disc") == 0) {
      options->disc_paths[0] = value;
    } else if (strcmp(argument, "--disc-b") == 0) {
      options->disc_paths[1] = value;
    } else if (strcmp(argument, "--save-disc") == 0) {
      options->save_disc_path = value;
    } else if (strcmp(argument, "--wait") == 0) {
      char *end = NULL;
      options->frames_after = strtol(value, &end, 10);
      if (end == value || *end != '\0' || options->frames_after < 0) {
        fprintf(stderr, "--wait wants a number of frames, not %s\n", value);
        return false;
      }
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
  const machine_t *machine = options->machine;
  uint32_t left = options->full_raster ? 0 : machine->crop_left;
  uint32_t top = options->full_raster ? 0 : machine->crop_top;
  uint32_t width = options->full_raster ? machine->raster_width : machine->crop_width;
  uint32_t lines = options->full_raster ? machine->raster_height : machine->crop_height;
  uint32_t repeat = options->double_lines && machine->double_lines ? 2 : 1;
  uint32_t height = lines * repeat;

  uint8_t *pixels = malloc((size_t)width * height * 3);
  if (pixels == NULL) {
    fprintf(stderr, "cannot hold a %ux%u image\n", width, height);
    return NULL;
  }
  uint8_t *out = pixels;
  for (uint32_t line = 0; line < lines; line++) {
    const uint8_t *row = framebuffer + (size_t)(top + line) * machine->raster_width + left;
    for (uint32_t again = 0; again < repeat; again++) {
      for (uint32_t column = 0; column < width; column++) {
        uint32_t rgb = machine->sample_rgb(row[column]);
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

/* Writes per byte of RAM, or NULL when nobody asked for the map. The
   machine is told nothing about this: cpc_tick already returns the bus, and
   every write the CPU makes crosses it. */
static uint32_t *cpc_writes;

/* Which byte of RAM the beam painted at each sample of the raster, held as
   the address plus one so that zero means the beam showed no byte there —
   border, sync or blanking. Rewritten every frame, so what survives the run
   is the screen as the machine last drew it.

   It is recorded rather than calculated. Where a byte lands on the picture
   depends on the whole CRTC configuration, and a program that reprograms it
   mid-frame has no single answer; watching the addresses the chip actually
   emits costs the same and stays true through a split screen. */
static uint32_t *cpc_displayed;

/* Base-2 logarithm in 8.8 fixed point: the position of the highest set bit,
   refined by the eight beneath it. Integer-only, so nothing links libm. */
static uint32_t log2_fixed(uint32_t value) {
  uint32_t bit = 31;
  while ((value >> bit) == 0) {
    bit--;
  }
  uint32_t fraction = bit >= 8 ? (value >> (bit - 8)) : (value << (8 - bit));
  return bit * 256 + (fraction & 0xFF);
}

/* Black where nothing was ever written, then a ramp through blue, magenta
   and yellow to white. Logarithmic because the range is four orders of
   magnitude: a byte written once and a stack byte written half a million
   times must both stay legible. */
static uint32_t heat_colour(uint32_t count, uint32_t peak) {
  if (count == 0) {
    return 0x000000;
  }
  uint32_t top = log2_fixed(peak);
  uint32_t level = top == 0 ? 255 : 16 + log2_fixed(count) * 239 / top;
  uint32_t red = 0;
  uint32_t green = 0;
  uint32_t blue = 0;
  if (level < 64) {
    blue = 64 + level * 3;
  } else if (level < 128) {
    red = (level - 64) * 4;
    blue = 255;
  } else if (level < 192) {
    red = 255;
    green = (level - 128) * 4;
    blue = 255 - (level - 128) * 4;
  } else {
    red = 255;
    green = 255;
    blue = (level - 192) * 4;
  }
  return (red << 16) | (green << 8) | blue;
}

/* The heat where the beam put it: the same crop and the same line doubling
   the screenshot uses, so the two images lie over one another. The scale is
   taken over the bytes that reached the screen alone — the firmware's stack
   is written a hundred times harder than any pixel, and letting it set the
   top of the range would flatten everything the picture is for. */
static uint8_t *cpc_render_writes(const options_t *options, uint32_t *width_out,
                                  uint32_t *height_out) {
  const machine_t *machine = options->machine;
  uint32_t left = options->full_raster ? 0 : machine->crop_left;
  uint32_t top = options->full_raster ? 0 : machine->crop_top;
  uint32_t width = options->full_raster ? machine->raster_width : machine->crop_width;
  uint32_t lines = options->full_raster ? machine->raster_height : machine->crop_height;
  uint32_t repeat = options->double_lines && machine->double_lines ? 2 : 1;
  uint32_t height = lines * repeat;

  uint32_t peak = 0;
  for (uint32_t at = 0; at < machine->raster_width * machine->raster_height; at++) {
    if (cpc_displayed[at] != 0 && cpc_writes[cpc_displayed[at] - 1] > peak) {
      peak = cpc_writes[cpc_displayed[at] - 1];
    }
  }

  uint8_t *pixels = malloc((size_t)width * height * 3);
  if (pixels == NULL) {
    fprintf(stderr, "cannot hold a %ux%u image\n", width, height);
    return NULL;
  }
  uint8_t *out = pixels;
  for (uint32_t line = 0; line < lines; line++) {
    const uint32_t *row = cpc_displayed + (size_t)(top + line) * machine->raster_width + left;
    for (uint32_t again = 0; again < repeat; again++) {
      for (uint32_t column = 0; column < width; column++) {
        uint32_t rgb = row[column] == 0 ? 0 : heat_colour(cpc_writes[row[column] - 1], peak);
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

/* Room past a disc image for every track the medium can hold to be
   formatted once more, a revolution's worth each. */
#define FORMAT_ROOM ((size_t)FLOPPY_MAX_CYLINDERS * FLOPPY_MAX_SIDES * FLOPPY_BYTES_PER_REVOLUTION)

/* Read a whole file into a fresh buffer with `room` spare bytes after it;
   the caller frees it. */
static uint8_t *read_file_with_room(const char *path, size_t *size_out, size_t room) {
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
  uint8_t *contents = malloc((size_t)size + room);
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

static uint8_t *read_file(const char *path, size_t *size_out) {
  return read_file_with_room(path, size_out, 0);
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

static bool cpc_save_snapshot(const cpc_t *cpc, const char *path) {
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

static void cpc_record_displayed(const cpc_t *cpc) {
  /* The Gate Array shows a character one microsecond after the CRTC hands
     over its address, so the samples just painted came from the address
     fetched last time — and blanking is judged now, as the chip judges it. */
  static uint16_t pending_address;
  static bool pending_display;

  uint16_t beam_x = cpc->monitor.beam_x;
  uint16_t beam_y = cpc->monitor.beam_y;
  bool blanked = cpc->gate_array.black_hsync || cpc->gate_array.black_vsync;
  if (pending_display && !blanked && beam_y < CPC_FRAMEBUFFER_HEIGHT &&
      beam_x >= GATE_ARRAY_SAMPLES_PER_CHARACTER) {
    uint32_t start =
        (uint32_t)beam_y * CPC_FRAMEBUFFER_WIDTH + beam_x - GATE_ARRAY_SAMPLES_PER_CHARACTER;
    for (uint8_t sample = 0; sample < GATE_ARRAY_SAMPLES_PER_CHARACTER; sample++) {
      /* Two bytes make sixteen samples, eight each, whatever the mode. */
      uint16_t address = pending_address | (sample < 8 ? 0 : 1);
      cpc_displayed[start + sample] = (uint32_t)address + 1;
    }
  }
  pending_address = cpc_video_address(cpc);
  pending_display = (cpc->crtc_pins & CRTC_DISPTMG) != 0;
}

static void cpc_run_frames(cpc_t *cpc, long frames) {
  for (long tick = 0; tick < frames * CPC_TICKS_PER_STANDARD_FRAME; tick++) {
    uint64_t pins = cpc_tick(cpc);
    if (cpc_writes == NULL) {
      continue;
    }
    if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
      /* Counted against the byte it landed in rather than the address it
         was sent to: under banking, two writes to &4000 can reach different
         halves of the machine. */
      uint16_t address = z80_address(pins);
      cpc_writes[cpc->write_page[address >> 14] + (address & 0x3FFF) - cpc->ram]++;
    }
    if (gate_array_character_clock(&cpc->gate_array)) {
      cpc_record_displayed(cpc);
    }
  }
}

/* Hold a key, with shift if the character needs it, then let go. */
static void cpc_press_and_release(cpc_t *cpc, keyboard_key key, bool shifted) {
  if (shifted) {
    keyboard_press(&cpc->keyboard, KEYBOARD_SHIFT);
  }
  keyboard_press(&cpc->keyboard, key);
  cpc_run_frames(cpc, FRAMES_KEY_HELD);
  keyboard_release_all(&cpc->keyboard);
  cpc_run_frames(cpc, FRAMES_KEY_RELEASED);
}

/* Type text, taking the escapes the usage message lists. Returns false
 * having reported a character this keyboard cannot produce. */
static bool cpc_type_text(cpc_t *cpc, const char *text) {
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
    cpc_press_and_release(cpc, key, shifted);
  }
  return true;
}

static void spectrum_run_frames(spectrum_t *spectrum, long frames) {
  for (long frame = 0; frame < frames; frame++) {
    for (int tick = 0; tick < SPECTRUM_TICKS_PER_FRAME; tick++) {
      spectrum_tick(spectrum);
    }
  }
}

/* The screenshot both machines write, from whichever raster they painted. */
static int write_screenshot(const options_t *options, const uint8_t *framebuffer) {
  long frames = options->frames + options->frames_after;
  if (options->screenshot_path == NULL) {
    printf("%s: %ld frames\n", options->machine->name, frames);
    return 0;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t *pixels = render(framebuffer, options, &width, &height);
  int status = 0;
  if (pixels == NULL || !png_write(options->screenshot_path, pixels, width, height)) {
    status = 1;
  } else {
    printf("%s: %ld frames, %ux%u to %s\n", options->machine->name, frames, width, height,
           options->screenshot_path);
  }
  free(pixels);
  return status;
}

/* Hold a key, with whichever shift the character needs, then let go. */
static void spectrum_press_and_release(spectrum_t *spectrum, keyboard_key key,
                                       spectrum_shift shift) {
  if (shift == SPECTRUM_WITH_CAPS_SHIFT) {
    keyboard_press(&spectrum->keyboard, SPECTRUM_CAPS_SHIFT);
  } else if (shift == SPECTRUM_WITH_SYMBOL_SHIFT) {
    keyboard_press(&spectrum->keyboard, SPECTRUM_SYMBOL_SHIFT);
  }
  keyboard_press(&spectrum->keyboard, key);
  spectrum_run_frames(spectrum, FRAMES_KEY_HELD);
  keyboard_release_all(&spectrum->keyboard);
  spectrum_run_frames(spectrum, FRAMES_KEY_RELEASED);
}

static bool spectrum_type_text(spectrum_t *spectrum, const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    char character = *at;
    keyboard_key key = KEYBOARD_NO_KEY;
    spectrum_shift shift = SPECTRUM_NO_SHIFT;
    if (character == '\\' && at[1] != '\0') {
      character = *++at;
      if (character == 'n') {
        key = SPECTRUM_ENTER;
      } else if (character == '\\') {
        key = spectrum_key_for_character('\\', &shift);
      } else {
        fprintf(stderr, "no such escape: \\%c\n", character);
        return false;
      }
    } else {
      key = spectrum_key_for_character(character, &shift);
    }
    if (key == KEYBOARD_NO_KEY) {
      fprintf(stderr, "this keyboard has no '%c'\n", character);
      return false;
    }
    spectrum_press_and_release(spectrum, key, shift);
  }
  return true;
}

/* A Spectrum has no disc, no snapshot and no links to solder, so the options
   that reach those are refused rather than quietly ignored. */
static bool spectrum_refuses(const options_t *options) {
  const struct {
    bool given;
    const char *what;
  } unsupported[] = {
      {options->snapshot_path != NULL, "run"},
      {options->save_path != NULL, "--save"},
      {options->writes_path != NULL, "--writes"},
      {options->disc_paths[0] != NULL || options->disc_paths[1] != NULL, "--disc"},
      {options->save_disc_path != NULL, "--save-disc"},
      {!options->fifty_hz, "--sixty-hz"},
  };
  for (size_t index = 0; index < sizeof unsupported / sizeof unsupported[0]; index++) {
    if (unsupported[index].given) {
      fprintf(stderr, "%s does not do %s yet\n", options->machine->name, unsupported[index].what);
      return true;
    }
  }
  return false;
}

static int run_spectrum(const options_t *options) {
  if (spectrum_refuses(options)) {
    return 1;
  }
  static uint8_t rom[SPECTRUM_ROM_SIZE];
  if (!load_rom(options->rom_directory, options->machine->rom_file, rom, sizeof rom)) {
    return 1;
  }

  size_t raster = (size_t)options->machine->raster_width * options->machine->raster_height;
  uint8_t *ram = calloc(options->machine->ram_size, 1);
  uint8_t *framebuffer = calloc(raster, 1);
  spectrum_t *spectrum = calloc(1, sizeof *spectrum);
  if (ram == NULL || framebuffer == NULL || spectrum == NULL) {
    fprintf(stderr, "cannot hold the machine\n");
    free(spectrum);
    free(framebuffer);
    free(ram);
    return 1;
  }
  spectrum_init(spectrum, ram, options->machine->ram_size, rom);
  spectrum_connect_monitor(spectrum, framebuffer);

  int status = 0;
  spectrum_run_frames(spectrum, options->frames);
  if (options->text != NULL && !spectrum_type_text(spectrum, options->text)) {
    status = 1;
  }
  if (status == 0) {
    spectrum_run_frames(spectrum, options->frames_after);
  }
  if (status == 0) {
    status = write_screenshot(options, framebuffer);
  }

  free(spectrum);
  free(framebuffer);
  free(ram);
  return status;
}

/* One machine per kind, each with its own run: they share the options, the
   rendering and the file handling, and nothing else. */
static int run_cpc(const options_t *options) {
  if (options->save_disc_path != NULL && options->disc_paths[0] == NULL) {
    fprintf(stderr, "--save-disc needs a disc in drive A to write\n");
    return 1;
  }

  static uint8_t rom[0x8000];
  if (!load_rom(options->rom_directory, options->machine->rom_file, rom, sizeof rom)) {
    return 1;
  }
  /* The disc interface brings its own ROM, as upper ROM 7. */
  bool disc_interface = options->machine->disc_interface || options->disc_paths[0] != NULL ||
                        options->disc_paths[1] != NULL;
  static uint8_t amsdos[0x4000];
  if (disc_interface && !load_rom(options->rom_directory, "amsdos.rom", amsdos, sizeof amsdos)) {
    return 1;
  }

  uint8_t *ram = calloc(options->machine->ram_size, 1);
  size_t raster = (size_t)options->machine->raster_width * options->machine->raster_height;
  uint8_t *framebuffer = calloc(raster, 1);
  cpc_t *cpc = calloc(1, sizeof *cpc);
  if (options->writes_path != NULL) {
    cpc_writes = calloc(options->machine->ram_size, sizeof *cpc_writes);
    cpc_displayed = calloc(raster, sizeof *cpc_displayed);
  }
  if (ram == NULL || framebuffer == NULL || cpc == NULL ||
      (options->writes_path != NULL && (cpc_writes == NULL || cpc_displayed == NULL))) {
    fprintf(stderr, "cannot hold the machine\n");
    free(cpc_displayed);
    free(cpc_writes);
    free(cpc);
    free(framebuffer);
    free(ram);
    return 1;
  }

  /* The operating system fills the lower 16K, BASIC the upper as ROM 0. */
  cpc_init(cpc, ram, options->machine->ram_size, rom);
  cpc_set_upper_rom(cpc, 0, rom + 0x4000);
  if (disc_interface) {
    cpc_fit_disc_interface(cpc, true);
    cpc_set_upper_rom(cpc, 7, amsdos);
  }
  cpc_connect_monitor(cpc, framebuffer);
  cpc_set_links(cpc, options->fifty_hz, CPC_MANUFACTURER_AMSTRAD);

  int status = 0;
  /* The discs. An image is read into a buffer of its own that the medium
     borrows for the run, and written back only where asked. */
  uint8_t *images[2] = {NULL, NULL};
  static floppy_t discs[2]; /* the machine borrows them; 190K apiece */
  for (uint8_t drive = 0; drive < 2 && status == 0; drive++) {
    if (options->disc_paths[drive] == NULL) {
      continue;
    }
    size_t size = 0;
    images[drive] = read_file_with_room(options->disc_paths[drive], &size, FORMAT_ROOM);
    const char *problem = NULL;
    if (images[drive] == NULL) {
      status = 1;
    } else if (!dsk_read(&discs[drive], images[drive], size, &problem)) {
      fprintf(stderr, "%s: %s\n", options->disc_paths[drive], problem);
      status = 1;
    } else {
      floppy_give_room(&discs[drive], size + FORMAT_ROOM);
      cpc_insert_disc(cpc, drive, &discs[drive]);
    }
  }
  if (options->snapshot_path != NULL) {
    size_t size = 0;
    uint8_t *contents = read_file(options->snapshot_path, &size);
    if (contents == NULL) {
      status = 1;
    } else {
      const char *problem = NULL;
      if (!snapshot_load(cpc, contents, size, &problem)) {
        fprintf(stderr, "%s %s\n", options->snapshot_path, problem);
        status = 1;
      }
      free(contents);
    }
  }
  if (status == 0) {
    cpc_run_frames(cpc, options->frames);
  }
  if (status == 0 && options->text != NULL && !cpc_type_text(cpc, options->text)) {
    status = 1;
  }
  if (status == 0) {
    cpc_run_frames(cpc, options->frames_after);
  }
  if (status == 0 && options->save_path != NULL) {
    cpc_finish_instruction(cpc);
    if (!cpc_save_snapshot(cpc, options->save_path)) {
      status = 1;
    }
  }
  if (status == 0 && options->save_disc_path != NULL) {
    size_t needed = dsk_write(&discs[0], NULL, 0);
    uint8_t *out = needed == 0 ? NULL : malloc(needed);
    if (needed == 0) {
      fprintf(stderr, "the disc in drive A holds a track the image format cannot describe\n");
      status = 1;
    } else if (out == NULL) {
      fprintf(stderr, "cannot hold the image\n");
      status = 1;
    } else {
      dsk_write(&discs[0], out, needed);
      if (!write_file(options->save_disc_path, out, needed)) {
        status = 1;
      } else {
        printf("%s: disc to %s%s\n", options->machine->name, options->save_disc_path,
               discs[0].modified ? "" : " (unchanged)");
      }
    }
    free(out);
  }
  if (status == 0 && options->writes_path != NULL) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t *pixels = cpc_render_writes(options, &width, &height);
    if (pixels == NULL || !png_write(options->writes_path, pixels, width, height)) {
      status = 1;
    } else {
      printf("%s: cpc_writes %ux%u to %s\n", options->machine->name, width, height,
             options->writes_path);
    }
    free(pixels);
  }
  if (status == 0) {
    status = write_screenshot(options, framebuffer);
  }

  free(images[1]);
  free(images[0]);
  free(cpc_displayed);
  free(cpc_writes);
  free(cpc);
  free(framebuffer);
  free(ram);
  return status;
}

static int run_machine(int argc, char **argv, bool from_snapshot) {
  options_t options = {
      .machine = NULL,
      .rom_directory = "roms",
      .screenshot_path = NULL,
      .writes_path = NULL,
      .text = NULL,
      .snapshot_path = NULL,
      .save_path = NULL,
      .disc_paths = {NULL, NULL},
      .save_disc_path = NULL,
      .frames = -1,
      .frames_after = 0,
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
  if (options.machine == NULL) {
    fprintf(stderr, "which machine? --machine takes one of:\n");
    for (size_t index = 0; index < machine_count; index++) {
      fprintf(stderr, "  %-10s %s\n", machines[index].name, machines[index].description);
    }
    return 1;
  }
  if (options.frames < 0) {
    options.frames = options.machine->default_frames;
  }
  if (options.machine->kind == MACHINE_SPECTRUM) {
    return run_spectrum(&options);
  }
  return run_cpc(&options);
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
