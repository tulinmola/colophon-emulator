/*
 * command_line_test — the emulator's command line, run as a user runs it.
 *
 * The binary under test is the one the build made, given firmware this test
 * writes: a CPC ROM whose program reads the CRTC's two ports as a program
 * does to learn which CRTC it has (Compendium ch. 28.1.8, 28.1.9), leaves
 * the answers in RAM for a snapshot to carry out, then changes an ink's
 * colour twice a line, for a screenshot to show wherever a change falls in
 * the display; a 48K ROM and a second CPC ROM that record every key pressed
 * while they run; and a +2's two images, which show in RAM which of them
 * ran. Everything it reads and writes is in the directory it is handed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* open, dup2, fork, execv, alarm and waitpid, to run the binary with no
   shell between, its output in files, and read how it exited. These are
   POSIX rather than C99, as the Shaker tier's fork is. */
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "png_read.h"
#include "test.h"

static const char *emulator;
static const char *directory;

/* An SNA is a 256-byte header and then the RAM
   (https://cpctech.cpcwiki.de/docs/snapshot.html). */
#define SNAPSHOT_HEADER_SIZE 256

/* Where the probe leaves its answers: in the bank the base configuration
   shows at &8000, which is the RAM's own &8000 on every CPC. */
#define ANSWERS 0x8000

/* The raster as the beam paints it, one sample to a pixel. */
#define RASTER_WIDTH 1024
#define RASTER_HEIGHT 312

/* Seconds a run may take before it is taken for hung; one takes well under
   one. */
#define PATIENCE 60

static void path_to(char *path, size_t size, const char *name) {
  int length = snprintf(path, size, "%s/%s", directory, name);
  if (length < 0 || (size_t)length >= size) {
    fprintf(stderr, "the path to %s in %s is too long\n", name, directory);
    exit(1);
  }
}

static bool write_file(const char *name, const uint8_t *contents, size_t size) {
  char path[1024];
  path_to(path, sizeof path, name);
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return false;
  }
  bool written = fwrite(contents, 1, size, file) == size;
  return fclose(file) == 0 && written;
}

/* The whole file, or NULL if it is missing, empty or cannot be read whole. */
static uint8_t *read_file(const char *name, size_t *size_out) {
  char path[1024];
  path_to(path, sizeof path, name);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return NULL;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  uint8_t *contents = size > 0 ? malloc((size_t)size) : NULL;
  size_t read = contents == NULL ? 0 : fread(contents, 1, (size_t)size, file);
  fclose(file);
  if (contents == NULL || read != (size_t)size) {
    free(contents);
    return NULL;
  }
  *size_out = read;
  return contents;
}

static void read_its_standard_error(char *text, size_t room) {
  char path[1024];
  path_to(path, sizeof path, "stderr.txt");
  FILE *file = fopen(path, "rb");
  size_t read = file == NULL ? 0 : fread(text, 1, room - 1, file);
  if (file != NULL) {
    fclose(file);
  }
  text[read] = '\0';
}

static bool file_exists(const char *name) {
  char path[1024];
  path_to(path, sizeof path, name);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return false;
  }
  fclose(file);
  return true;
}

static void remove_file(const char *name) {
  char path[1024];
  path_to(path, sizeof path, name);
  remove(path);
}

static bool redirect(int descriptor, const char *name) {
  char path[1024];
  path_to(path, sizeof path, name);
  int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return file >= 0 && dup2(file, descriptor) >= 0 && close(file) == 0;
}

/* Runs the emulator with `arguments`, which end at a NULL, on this test's
   ROMs, asking it for `name`.sna and `name`.png, the whole raster at one
   image line to each raster line, with whatever a run before left under
   those names removed first. The asking comes after the command and its
   snapshot and before the rest of `arguments`, so that an option of the
   test's own, given later, is the one taken. What it prints is kept in
   stdout.txt and stderr.txt. Returns its exit status, 127 where the binary
   could not be run, or -1 where it was never started or did not exit. */
static int emulate(const char *name, const char *const arguments[]) {
  char snapshot[64];
  char screenshot[64];
  snprintf(snapshot, sizeof snapshot, "%s.sna", name);
  snprintf(screenshot, sizeof screenshot, "%s.png", name);
  remove_file(snapshot);
  remove_file(screenshot);
  remove_file("stdout.txt");
  remove_file("stderr.txt");
  char snapshot_path[1024];
  char screenshot_path[1024];
  path_to(snapshot_path, sizeof snapshot_path, snapshot);
  path_to(screenshot_path, sizeof screenshot_path, screenshot);

  const char *always[] = {"--roms",        directory,     "--save",       snapshot_path,
                          "--full-raster", "--no-double", "--screenshot", screenshot_path};
  const size_t always_count = sizeof always / sizeof always[0];
  const char *line[32];
  size_t count = 0;
  line[count++] = emulator;
  size_t index = 0;
  size_t leading = arguments[0] != NULL && strcmp(arguments[0], "run") == 0 ? 2 : 1;
  for (; index < leading && arguments[index] != NULL; index++) {
    line[count++] = arguments[index];
  }
  for (size_t always_index = 0; always_index < always_count; always_index++) {
    line[count++] = always[always_index];
  }
  for (; arguments[index] != NULL; index++) {
    if (count + 1 >= sizeof line / sizeof line[0]) {
      TEST_FAIL("%s is given more arguments than the line holds", name);
      return -1;
    }
    line[count++] = arguments[index];
  }
  line[count] = NULL;

  fflush(stdout);
  pid_t child = fork();
  if (child == 0) {
    if (redirect(STDOUT_FILENO, "stdout.txt") && redirect(STDERR_FILENO, "stderr.txt")) {
      alarm(PATIENCE);
      execv(emulator, (char *const *)line);
      perror(emulator);
    }
    _exit(127);
  }
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

/* Fails, with what the run said, when it does not exit with `expected`. */
static bool exits_with(const char *name, const char *const arguments[], int expected) {
  int status = emulate(name, arguments);
  if (status == expected) {
    return true;
  }
  char said[2048];
  read_its_standard_error(said, sizeof said);
  TEST_FAIL("%s exited %d, not %d, saying \"%s\"", name, status, expected, said);
  return false;
}

/* Refused: an exit with status 1, `reason` alone on standard error, and
   nothing written. */
static void refused_saying(const char *const arguments[], const char *reason) {
  if (!exits_with("refused", arguments, 1)) {
    return;
  }
  char said[2048];
  char expected[2048];
  read_its_standard_error(said, sizeof said);
  snprintf(expected, sizeof expected, "%s\n", reason);
  if (strcmp(said, expected) != 0) {
    TEST_FAIL("it said \"%s\", not \"%s\"", said, reason);
  }
  TEST_CHECK(!file_exists("refused.sna"));
  TEST_CHECK(!file_exists("refused.png"));
}

static size_t append(uint8_t *program, size_t length, const uint8_t *bytes, size_t count) {
  memcpy(program + length, bytes, count);
  return length + count;
}

/* A select-and-write pair for one CRTC register: OUT (C),C to &BCnn, then
   to &BDvv. */
static size_t append_crtc_write(uint8_t *program, size_t length, uint8_t number, uint8_t value) {
  const uint8_t bytes[] = {0x01, number, 0xBC, 0xED, 0x49, 0x01, value, 0xBD, 0xED, 0x49};
  return append(program, length, bytes, sizeof bytes);
}

/* LD B,&pp and IN A,(C), from port &pp00 + C with C left by the select
   before, then LD (address),A. */
static size_t append_read_to(uint8_t *program, size_t length, uint8_t port, uint16_t address) {
  const uint8_t bytes[] = {0x06, port, 0xED, 0x78, 0x32, (uint8_t)address, (uint8_t)(address >> 8)};
  return append(program, length, bytes, sizeof bytes);
}

/* The CPC firmware this test is run on, the 464's and the 6128's alike: the
   standard 50Hz screen, as the firmware programs it, and R15 for types 3 and
   4 to answer R31 with; the CRTC asked what it is; and a loop of 64
   microseconds that writes pen 0's ink blue and, 30 microseconds later,
   bright white. The display, all pen 0 over RAM that is zeros, lasts 40 of
   the line's 64 microseconds, so wherever the loop falls in the line one
   write or both change its colour, at the same places on every line. An OUT
   is four microseconds on the CPC, a NOP one and a JR three; the ROM is
   zeros, and a zero is a NOP. The 6128's disc interface is handed an AMSDOS
   of zeros, which nothing here calls. */
static void write_cpc_roms(void) {
  static uint8_t program[0x8000];
  size_t length = 0;
  const uint8_t standard_screen[][2] = {{0, 63}, {1, 40}, {2, 46}, {3, 0x8E}, {4, 38},
                                        {6, 25}, {7, 30}, {9, 7},  {12, 0x30}};
  for (size_t index = 0; index < sizeof standard_screen / sizeof standard_screen[0]; index++) {
    length =
        append_crtc_write(program, length, standard_screen[index][0], standard_screen[index][1]);
  }
  length = append_crtc_write(program, length, 15, 0x5A);

  const uint8_t select_r12[] = {0x01, 0x0C, 0xBC, 0xED, 0x49}; /* LD BC,&BC0C; OUT (C),C */
  length = append(program, length, select_r12, sizeof select_r12);
  length = append_read_to(program, length, 0xBF, ANSWERS);
  length = append_read_to(program, length, 0xBE, ANSWERS + 1);
  const uint8_t select_r31[] = {0x01, 0x1F, 0xBC, 0xED, 0x49}; /* LD BC,&BC1F; OUT (C),C */
  length = append(program, length, select_r31, sizeof select_r31);
  length = append_read_to(program, length, 0xBF, ANSWERS + 2);

  const uint8_t mode_pen_and_colours[] = {
      0x01, 0x81, 0x7F, 0xED, 0x49, /* LD BC,&7F81; OUT (C),C: RMR, mode 1, both ROMs */
      0x01, 0x00, 0x7F, 0xED, 0x49, /* LD BC,&7F00; OUT (C),C: PENR, pen 0 */
      0x01, 0x44, 0x7F, 0x1E, 0x4B, /* LD BC,&7F44; LD E,&4B: blue, bright white */
  };
  length = append(program, length, mode_pen_and_colours, sizeof mode_pen_and_colours);
  size_t loop = length;
  const uint8_t out_c[] = {0xED, 0x49};
  const uint8_t out_e[] = {0xED, 0x59};
  length = append(program, length, out_c, sizeof out_c);
  length += 26;
  length = append(program, length, out_e, sizeof out_e);
  length += 27;
  program[length++] = 0x18; /* JR loop */
  program[length] = (uint8_t)(loop - (length + 1));
  TEST_CHECK(write_file("cpc464.rom", program, sizeof program));
  TEST_CHECK(write_file("cpc6128.rom", program, sizeof program));

  static const uint8_t amsdos[0x4000];
  TEST_CHECK(write_file("amsdos.rom", amsdos, sizeof amsdos));
}

/* Where the key recorders below keep what they read: a byte for each of a
   Spectrum's eight half-rows, from A8's to A15's, or each of a CPC's ten
   lines, each the AND of every reading, so a bit clear is a key that was
   down at some time. */
#define KEYS_SEEN 0x8000
#define SPECTRUM_HALF_ROWS 8
#define CPC_KEY_LINES 10

/* A 48K ROM that records every key pressed while it runs, reading each
   half-row through port &FE. */
static void write_spectrum_rom(void) {
  static uint8_t rom[0x4000];
  const uint8_t start[] = {0xF3, 0x21, 0x00, 0x80}; /* DI : LD HL,KEYS_SEEN */
  size_t length = append(rom, 0, start, sizeof start);
  for (int row = 0; row < SPECTRUM_HALF_ROWS; row++) {
    const uint8_t clear[] = {0x36, 0xFF, 0x23}; /* LD (HL),&FF : INC HL */
    length = append(rom, length, clear, sizeof clear);
  }
  size_t loop = length;
  const uint8_t restart[] = {0x21, 0x00, 0x80}; /* LD HL,KEYS_SEEN */
  length = append(rom, length, restart, sizeof restart);
  for (int row = 0; row < SPECTRUM_HALF_ROWS; row++) {
    /* LD BC,port : IN A,(C) : AND (HL) : LD (HL),A : INC HL, the port's
       high byte low on the half-row's one address line */
    const uint8_t read[] = {0x01, 0xFE, (uint8_t)~(1 << row), 0xED, 0x78, 0xA6, 0x77, 0x23};
    length = append(rom, length, read, sizeof read);
  }
  const uint8_t jump[] = {0xC3, (uint8_t)loop, (uint8_t)(loop >> 8)}; /* JP to the loop */
  append(rom, length, jump, sizeof jump);
  TEST_CHECK(write_file("spectrum48.rom", rom, sizeof rom));
}

/* A +2's two ROM images: the first pages in the second, with bank 0 at
   &C000, and the second writes &2B to &8000 where the first would write
   &0A, so that which one ran shows in RAM, and loops a byte further on. */
static void write_plus2_roms(void) {
  static uint8_t first[0x4000];
  static uint8_t second[0x4000];
  const uint8_t pages[] = {
      0xF3,             /* DI */
      0x01, 0xFD, 0x7F, /* LD BC,&7FFD */
      0x3E, 0x10,       /* LD A,&10: the second ROM, bank 0, unlocked */
      0xED, 0x79,       /* OUT (C),A; the next fetch, at 8, is the second's */
  };
  const uint8_t marks_first[] = {0x3E, 0x0A, 0x32, 0x00, 0x80, 0x18, 0xFE};
  /* the same, with a NOP in front of its loop, so that where it loops
     tells the two apart as well */
  const uint8_t marks_second[] = {0x3E, 0x2B, 0x32, 0x00, 0x80, 0x00, 0x18, 0xFE};
  memcpy(first, pages, sizeof pages);
  memcpy(first + 8, marks_first, sizeof marks_first);
  memcpy(second + 8, marks_second, sizeof marks_second);
  TEST_CHECK(write_file("spectrumplus2-0.rom", first, sizeof first));
  TEST_CHECK(write_file("spectrumplus2-1.rom", second, sizeof second));
}

/* A CPC ROM that records every key pressed while it runs, reading each of
   the ten lines through the 8255 and the sound chip's register 14 as the
   firmware reads them. It is written as the 664's, a ROM the other tests
   leave alone. */
static void write_cpc_key_recorder(void) {
  static uint8_t rom[0x8000];
  const uint8_t start[] = {
      0xF3,                         /* DI */
      0x01, 0x82, 0xF7, 0xED, 0x49, /* LD BC,&F782 : OUT (C),C: A and C out */
      0x21, 0x00, 0x80,             /* LD HL,KEYS_SEEN */
  };
  size_t length = append(rom, 0, start, sizeof start);
  for (int line = 0; line < CPC_KEY_LINES; line++) {
    const uint8_t clear[] = {0x36, 0xFF, 0x23}; /* LD (HL),&FF : INC HL */
    length = append(rom, length, clear, sizeof clear);
  }
  size_t loop = length;
  const uint8_t select[] = {
      0x21, 0x00, 0x80,             /* LD HL,KEYS_SEEN */
      0x01, 0x0E, 0xF4, 0xED, 0x49, /* LD BC,&F40E : OUT (C),C: register 14 */
      0x01, 0xC0, 0xF6, 0xED, 0x49, /* LD BC,&F6C0 : OUT (C),C: select it */
      0x01, 0x00, 0xF6, 0xED, 0x49, /* LD BC,&F600 : OUT (C),C: inactive */
      0x01, 0x92, 0xF7, 0xED, 0x49, /* LD BC,&F792 : OUT (C),C: A in */
  };
  length = append(rom, length, select, sizeof select);
  for (int line = 0; line < CPC_KEY_LINES; line++) {
    const uint8_t read[] = {
        0x01, (uint8_t)(0x40 + line),
        0xF6, 0xED,
        0x49, /* LD BC,&F64n : OUT (C),C: read line n */
        0x06, 0xF4,
        0xED, 0x78, /* LD B,&F4 : IN A,(C) */
        0xA6, 0x77,
        0x23, /* AND (HL) : LD (HL),A : INC HL */
    };
    length = append(rom, length, read, sizeof read);
  }
  const uint8_t release[] = {
      0x01, 0x82,          0xF7,
      0xED, 0x49, /* LD BC,&F782 : OUT (C),C: A out */
      0x01, 0x00,          0xF6,
      0xED, 0x49,                                /* LD BC,&F600 : OUT (C),C: inactive */
      0xC3, (uint8_t)loop, (uint8_t)(loop >> 8), /* JP to the loop */
  };
  append(rom, length, release, sizeof release);
  TEST_CHECK(write_file("cpc664.rom", rom, sizeof rom));
}

static bool files_agree(const char *name, const char *other) {
  size_t size = 0;
  size_t other_size = 0;
  uint8_t *contents = read_file(name, &size);
  uint8_t *other_contents = read_file(other, &other_size);
  bool agree = contents != NULL && other_contents != NULL && size == other_size &&
               memcmp(contents, other_contents, size) == 0;
  free(contents);
  free(other_contents);
  return agree;
}

/* The three answers a snapshot carries out: R12 at the read port, R12 at the
   status port, R31 at the read port. */
static bool answers_in(const char *snapshot, uint8_t answers[3]) {
  size_t size = 0;
  uint8_t *contents = read_file(snapshot, &size);
  if (contents == NULL || size < SNAPSHOT_HEADER_SIZE + 0x10000) {
    TEST_FAIL("%s is missing or holds no 64K", snapshot);
    free(contents);
    return false;
  }
  memcpy(answers, contents + SNAPSHOT_HEADER_SIZE + ANSWERS, 3);
  free(contents);
  return true;
}

static uint8_t *whole_raster(const char *picture) {
  char path[1024];
  path_to(path, sizeof path, picture);
  char problem[256];
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t *pixels = png_read(path, &width, &height, problem, sizeof problem);
  if (pixels == NULL) {
    TEST_FAIL("%s", problem);
    return NULL;
  }
  if (width != RASTER_WIDTH || height != RASTER_HEIGHT) {
    TEST_FAIL("%s is %ux%u, not the whole raster", picture, width, height);
    free(pixels);
    return NULL;
  }
  return pixels;
}

/* A colour written to the Gate Array shows half a byte, four samples,
   sooner on a type 3 than on a type 4 (the drawings of Compendium ch.
   9.2.2). In mode 2 alone a type 4 changes it a pixel earlier than in its
   other modes, "0.0625 µsec (1 pixel mode 2)" (ch. 9.2.2), which this Gate
   Array does not yet draw (cpc.c); the loop runs in mode 1. Types 3 and 4
   are the same chip here and answer alike, so this is the one way their
   pictures differ: wherever they do, type 3 shows for four samples the
   colour type 4 changes to after them, and type 4 still the one before. */
static void type_3s_colours_come_half_a_byte_before_type_4s(const char *type_3,
                                                            const char *type_4) {
  uint8_t *sooner = whole_raster(type_3);
  uint8_t *later = whole_raster(type_4);
  int changes = 0;
  for (size_t line = 0; sooner != NULL && later != NULL && line < RASTER_HEIGHT; line++) {
    const uint8_t *row_sooner = sooner + line * RASTER_WIDTH * 3;
    const uint8_t *row_later = later + line * RASTER_WIDTH * 3;
    size_t at = 0;
    while (at < RASTER_WIDTH) {
      if (memcmp(row_sooner + at * 3, row_later + at * 3, 3) == 0) {
        at++;
        continue;
      }
      size_t first = at;
      while (at < RASTER_WIDTH && memcmp(row_sooner + at * 3, row_later + at * 3, 3) != 0) {
        at++;
      }
      changes++;
      if (first == 0 || at == RASTER_WIDTH || at - first != 4) {
        TEST_FAIL("line %zu differs from sample %zu to %zu", line, first, at);
        continue;
      }
      for (size_t sample = first; sample < at; sample++) {
        TEST_CHECK(memcmp(row_sooner + sample * 3, row_later + at * 3, 3) == 0);
        TEST_CHECK(memcmp(row_later + sample * 3, row_later + (first - 1) * 3, 3) == 0);
      }
    }
  }
  TEST_CHECK(changes > 0);
  free(sooner);
  free(later);
}

static void a_type_outside_0_to_4_is_refused(void) {
  const char *refused[] = {"5", "-1", "x", "", "4x", "+3", "03", " 3"};
  for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
    const char *arguments[] = {"boot", "--machine",   "cpc464",       "--frames",
                               "1",    "--crtc-type", refused[index], NULL};
    char reason[64];
    snprintf(reason, sizeof reason, "--crtc-type wants a CRTC type from 0 to 4, not %s",
             refused[index]);
    refused_saying(arguments, reason);
  }
}

/* Whatever the value, and whichever comes first, the option or the machine. */
static void a_spectrum_refuses_any_type(void) {
  const char *reason = "spectrum48 has no CRTC; --crtc-type is for cpc6128, cpc664 and cpc464";
  const char *type_first[] = {"boot",       "--crtc-type", "0", "--machine",
                              "spectrum48", "--frames",    "1", NULL};
  refused_saying(type_first, reason);
  const char *out_of_range[] = {"boot", "--machine",   "spectrum48", "--frames",
                                "1",    "--crtc-type", "9",          NULL};
  refused_saying(out_of_range, reason);
  const char *without[] = {"boot", "--machine", "spectrum48", "--frames", "1", NULL};
  TEST_CHECK(exits_with("spectrum", without, 0));
}

/* Each type answers as the Compendium's tables have it. Type 0 reads R12 to
   R17, and "an attempt to read another register (0 to 255) returns the value
   0" (ch. 21.2.1). Types 1 and 2 read R14 to R17 and 0 for the rest, except
   that on type 1 "register 31 ... returns a non-zero value" (ch. 21.2.2).
   The status port is a bus nobody drives on types 0 and 2 — "my CPC CRTC 2
   always returns 255 ... my CPC CRTC 0 randomly returns 255 or 127" (ch.
   21.3.2) — and a type 1's status register, whose bits 0 to 4 and 7 read 0
   (ch. 21.3.3). Types 3 and 4 read a table of eight by the register's low
   three bits, so R12 is R12 and R31 is R15 (ch. 21.2.3), and their status
   port "is a mirror of the read port" (ch. 21.3.1). */
static void check_answers(const char *snapshot, int type) {
  uint8_t answers[3];
  if (!answers_in(snapshot, answers)) {
    return;
  }
  uint8_t r12 = answers[0];
  uint8_t status = answers[1];
  uint8_t r31 = answers[2];
  switch (type) {
    case 0:
      TEST_EQUAL(r12, 0x30);
      TEST_EQUAL(status | 0x80, 0xFF);
      TEST_EQUAL(r31, 0);
      break;
    case 1:
      TEST_EQUAL(r12, 0);
      TEST_EQUAL(status & 0x9F, 0);
      TEST_CHECK(r31 != 0);
      break;
    case 2:
      TEST_EQUAL(r12, 0);
      TEST_EQUAL(status, 0xFF);
      TEST_EQUAL(r31, 0);
      break;
    default:
      TEST_EQUAL(r12, 0x30);
      TEST_EQUAL(status, 0x30);
      TEST_EQUAL(r31, 0x5A);
      break;
  }
}

static void each_type_answers_as_its_own(void) {
  for (int type = 0; type <= 4; type++) {
    char number[2] = {(char)('0' + type), '\0'};
    char name[16];
    char snapshot[32];
    snprintf(name, sizeof name, "crtc%d", type);
    snprintf(snapshot, sizeof snapshot, "%s.sna", name);
    const char *arguments[] = {"boot",   "--crtc-type", number, "--machine",
                               "cpc464", "--frames",    "5",    NULL};
    if (exits_with(name, arguments, 0)) {
      check_answers(snapshot, type);
    }
  }
  type_3s_colours_come_half_a_byte_before_type_4s("crtc3.png", "crtc4.png");

  const char *on_a_6128[] = {"boot", "--machine",   "cpc6128", "--frames",
                             "5",    "--crtc-type", "3",       NULL};
  if (exits_with("cpc6128", on_a_6128, 0)) {
    check_answers("cpc6128.sna", 3);
  }
}

/* Compared whole with the type 0 run the test before made, snapshot and
   picture; a type 1's snapshot differs from it, so the comparison has
   something in it. */
static void no_type_builds_a_type_0(void) {
  const char *arguments[] = {"boot", "--machine", "cpc464", "--frames", "5", NULL};
  TEST_CHECK(exits_with("unnamed", arguments, 0));
  TEST_CHECK(files_agree("unnamed.sna", "crtc0.sna"));
  TEST_CHECK(files_agree("unnamed.png", "crtc0.png"));
  TEST_CHECK(file_exists("crtc1.sna"));
  TEST_CHECK(!files_agree("unnamed.sna", "crtc1.sna"));
}

/* The snapshot of the type 0 run each_type_answers_as_its_own made, run on
   as a type 3 and as a type 4. A type 0's answers are still in both runs'
   memory, so the program was picked up rather than begun again. Their
   pictures differ as type 3's and type 4's do; were the option ignored, both
   would be built as the default type 0 and would not differ at all. */
static void run_builds_the_type_named(void) {
  char snapshot[1024];
  path_to(snapshot, sizeof snapshot, "crtc0.sna");
  const char *as_type_3[] = {"run", snapshot,      "--machine", "cpc464", "--frames",
                             "5",   "--crtc-type", "3",         NULL};
  const char *as_type_4[] = {"run", snapshot,      "--machine", "cpc464", "--frames",
                             "5",   "--crtc-type", "4",         NULL};
  TEST_CHECK(exits_with("run3", as_type_3, 0));
  TEST_CHECK(exits_with("run4", as_type_4, 0));
  uint8_t saved[3];
  uint8_t as_3[3];
  uint8_t as_4[3];
  if (answers_in("crtc0.sna", saved) && answers_in("run3.sna", as_3) &&
      answers_in("run4.sna", as_4)) {
    TEST_CHECK(memcmp(as_3, saved, 3) == 0);
    TEST_CHECK(memcmp(as_4, saved, 3) == 0);
  }
  type_3s_colours_come_half_a_byte_before_type_4s("run3.png", "run4.png");
}

/* The bytes a snapshot holds from `at`, or false having said why not. */
static bool snapshot_bytes(const char *name, size_t size, size_t at, uint8_t *bytes, size_t count) {
  size_t read = 0;
  uint8_t *contents = read_file(name, &read);
  bool whole = contents != NULL && read == size;
  if (!whole) {
    TEST_FAIL("%s is missing or not %zu bytes", name, size);
  } else {
    memcpy(bytes, contents + at, count);
  }
  free(contents);
  return whole;
}

/* A 48K snapshot is its 27-byte header and then the RAM from &4000
   (the World of Spectrum FAQ's "Emulator file formats"); a 128K one lays
   bank 5 there and bank 2, which a +2 shows at &8000, after it, and holds
   its program counter at 49179 and its bank register at 49181. */
#define SPECTRUM_HEADER 27
#define SPECTRUM_48K_SNAPSHOT (SPECTRUM_HEADER + 0xC000)
#define PLUS2_SNAPSHOT 131103
#define PLUS2_PC 49179
#define PLUS2_BANK_REGISTER 49181

/* The +2 runs its second ROM from the second image it is given, and a
   snapshot of it resumes there. */
static void a_plus2_pages_in_its_second_image(void) {
  const char *boot[] = {"boot", "--machine", "spectrumplus2", "--frames", "2", NULL};
  if (!exits_with("plus2", boot, 0)) {
    return;
  }
  char snapshot[1024];
  path_to(snapshot, sizeof snapshot, "plus2.sna");
  const char *resumed[] = {"run", snapshot, "--machine", "spectrumplus2", "--frames", "1", NULL};
  if (!exits_with("plus2again", resumed, 0)) {
    return;
  }
  const char *names[] = {"plus2.sna", "plus2again.sna"};
  for (size_t index = 0; index < 2; index++) {
    uint8_t marked = 0;
    uint8_t fields[3];
    if (snapshot_bytes(names[index], PLUS2_SNAPSHOT, SPECTRUM_HEADER + 0x4000, &marked, 1) &&
        snapshot_bytes(names[index], PLUS2_SNAPSHOT, PLUS2_PC, fields, 3)) {
      TEST_EQUAL(marked, 0x2B);                     /* the second image's mark */
      TEST_EQUAL(fields[0] | fields[1] << 8, 0x0E); /* the second image's loop */
      TEST_EQUAL(fields[PLUS2_BANK_REGISTER - PLUS2_PC], 0x10);
    }
  }
}

/* A Spectrum's snapshot is read as a .z80 when its name ends so, in either
   case, and as an SNA otherwise: the same bytes under the two names load
   once and are refused once. These are a first-version .z80 (Lunter's
   manual for Z80 3.02, §5.5): thirty bytes of registers, the program
   counter at byte 6 and interrupts off, then the 48K uncompressed, in
   which a loop at &C000 keeps the processor off the ROM. */
static void a_z80_is_read_by_its_name(void) {
  static uint8_t file[30 + 0xC000];
  memset(file, 0, sizeof file);
  file[6] = 0x00; /* PC &C000 */
  file[7] = 0xC0;
  file[8] = 0x00; /* SP &BF00 */
  file[9] = 0xBF;
  file[29] = 0x01; /* interrupt mode 1 */
  for (size_t at = 0; at < 0xC000; at++) {
    file[30 + at] = (uint8_t)(at % 249);
  }
  file[30 + 0x8000] = 0x18; /* JR -2, at &C000 */
  file[30 + 0x8001] = 0xFE;
  if (!write_file("GAME.Z80", file, sizeof file) || !write_file("game.sna", file, sizeof file)) {
    TEST_FAIL("could not write the snapshots");
    return;
  }
  char z80[1024];
  char sna[1024];
  path_to(z80, sizeof z80, "GAME.Z80");
  path_to(sna, sizeof sna, "game.sna");
  const char *as_z80[] = {"run", z80, "--machine", "spectrum48", "--frames", "1", NULL};
  const char *as_sna[] = {"run", sna, "--machine", "spectrum48", "--frames", "1", NULL};
  TEST_CHECK(exits_with("fromz80", as_z80, 0));
  TEST_CHECK(exits_with("fromsna", as_sna, 1));
  uint8_t ram[0x40];
  if (snapshot_bytes("fromz80.sna", SPECTRUM_48K_SNAPSHOT, SPECTRUM_HEADER + 0x1000, ram,
                     sizeof ram)) {
    for (size_t at = 0; at < sizeof ram; at++) {
      TEST_EQUAL(ram[at], (0x1000 + at) % 249);
    }
  }
  /* And since the name decides, a Spectrum's SNA is not written under a
     .z80's; a CPC, which reads any name as an SNA, writes one there. */
  char saved_as_z80[1024];
  path_to(saved_as_z80, sizeof saved_as_z80, "saved.Z80");
  remove_file("saved.Z80");
  const char *save_as_z80[] = {"boot", "--machine", "spectrum48", "--save", saved_as_z80, NULL};
  char reason[1200];
  snprintf(reason, sizeof reason,
           "--save writes an SNA, and spectrum48 would read %s back as a .z80", saved_as_z80);
  refused_saying(save_as_z80, reason);
  TEST_CHECK(!file_exists("saved.Z80"));
  const char *cpc_save_as_z80[] = {"boot", "--machine", "cpc6128",    "--frames",
                                   "1",    "--save",    saved_as_z80, NULL};
  TEST_CHECK(exits_with("cpcz80", cpc_save_as_z80, 0));
  TEST_CHECK(file_exists("saved.Z80"));
}

/* Each escape for a Spectrum's key that prints nothing holds it down with
   CAPS SHIFT, as the legends above a 48K's keys read: the arrows over 5 to
   8, DELETE over 0 and BREAK over SPACE; and no other key. Half-row 0 holds
   CAPS SHIFT, 3 the keys 1 to 5, 4 the keys 0 to 6 and 7 SPACE. */
static void each_spectrum_escape_holds_its_key_with_caps_shift(void) {
  static const struct {
    const char *typed;
    size_t row;
    uint8_t bit;
  } escapes[] = {
      {"\\l", 3, 0x10}, /* 5 */
      {"\\d", 4, 0x10}, /* 6 */
      {"\\u", 4, 0x08}, /* 7 */
      {"\\r", 4, 0x04}, /* 8 */
      {"\\b", 4, 0x01}, /* 0 */
      {"\\e", 7, 0x01}, /* SPACE */
  };
  for (size_t index = 0; index < sizeof escapes / sizeof escapes[0]; index++) {
    const char *arguments[] = {"boot", "--machine", "spectrum48",         "--frames",
                               "1",    "--type",    escapes[index].typed, NULL};
    uint8_t seen[SPECTRUM_HALF_ROWS];
    if (!exits_with("keys", arguments, 0) ||
        !snapshot_bytes("keys.sna", SPECTRUM_48K_SNAPSHOT, SPECTRUM_HEADER + KEYS_SEEN - 0x4000,
                        seen, SPECTRUM_HALF_ROWS)) {
      return;
    }
    for (size_t row = 0; row < SPECTRUM_HALF_ROWS; row++) {
      uint8_t expected = 0x1F;
      expected &= (uint8_t)~(row == escapes[index].row ? escapes[index].bit : 0);
      expected &= (uint8_t)~(row == 0 ? 0x01 : 0); /* CAPS SHIFT */
      if ((seen[row] & 0x1F) != expected) {
        TEST_FAIL("%s left half-row %zu at %02X, expected %02X", escapes[index].typed, row,
                  seen[row] & 0x1F, expected);
      }
    }
  }
}

/* A CPC's cursor escapes press its cursor keys, which are line 0's first
   three bits and line 1's first (cpc.h), and no other key. */
static void each_cpc_cursor_escape_presses_its_key(void) {
  static const struct {
    const char *typed;
    size_t line;
    uint8_t bit;
  } escapes[] = {
      {"\\u", 0, 0x01},
      {"\\r", 0, 0x02},
      {"\\d", 0, 0x04},
      {"\\l", 1, 0x01},
  };
  for (size_t index = 0; index < sizeof escapes / sizeof escapes[0]; index++) {
    const char *arguments[] = {"boot",   "--machine",          "cpc664", "--frames", "1",
                               "--type", escapes[index].typed, NULL};
    uint8_t seen[CPC_KEY_LINES];
    if (!exits_with("cursor", arguments, 0) ||
        !snapshot_bytes("cursor.sna", SNAPSHOT_HEADER_SIZE + 0x10000,
                        SNAPSHOT_HEADER_SIZE + KEYS_SEEN, seen, CPC_KEY_LINES)) {
      return;
    }
    for (size_t line = 0; line < CPC_KEY_LINES; line++) {
      uint8_t expected = line == escapes[index].line ? (uint8_t)~escapes[index].bit : 0xFF;
      if (seen[line] != expected) {
        TEST_FAIL("%s left line %zu at %02X, expected %02X", escapes[index].typed, line, seen[line],
                  expected);
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: command_line_test EMULATOR DIRECTORY\n");
    return 1;
  }
  emulator = argv[1];
  directory = argv[2];
  write_cpc_roms();
  write_cpc_key_recorder();
  write_spectrum_rom();
  write_plus2_roms();
  TEST_RUN(a_type_outside_0_to_4_is_refused);
  TEST_RUN(a_spectrum_refuses_any_type);
  TEST_RUN(each_type_answers_as_its_own);
  TEST_RUN(no_type_builds_a_type_0);
  TEST_RUN(run_builds_the_type_named);
  TEST_RUN(a_plus2_pages_in_its_second_image);
  TEST_RUN(a_z80_is_read_by_its_name);
  TEST_RUN(each_spectrum_escape_holds_its_key_with_caps_shift);
  TEST_RUN(each_cpc_cursor_escape_presses_its_key);
  return TEST_REPORT("command line");
}
