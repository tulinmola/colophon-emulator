/*
 * spectrum_firmware_test — boot the real firmware and read the screen back.
 *
 * The acceptance tier for the machine. Everything here is judged by Sinclair
 * and Amstrad rather than by us: the boot messages and the menu are theirs,
 * the arithmetic is BASIC's, and the letters are identified by looking each
 * glyph up in the character table the 48K ROM carries at &3D00 — eight
 * bytes per code from 32 up, which is how the firmware draws them in the
 * first place, on a +2 from the same ROM in its second half. A test that
 * recognised letters by our own table would only prove we agree with
 * ourselves.
 *
 * The screen is read twice by two routes: out of the display file, and out
 * of the framebuffer the monitor painted. Only the second goes through the
 * serialiser, the sync separator and the beam, so agreeing means the whole
 * video path holds and not merely the memory under it.
 *
 * Needs the firmware images: run `make roms`.
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
/* Where the +2's second ROM, the 48K's, begins in its one chip. */
#define PLUS2_ROM_1 0x4000

/* Either model's boot message or menu is on screen well before this; the
   ROMs spend most of it testing the RAM. */
#define FRAMES_TO_PROMPT 100
/* The firmware scans the keyboard off the 50Hz interrupt and debounces, so
   a key must be held for several scans and released for several more. */
#define FRAMES_PER_KEY 6

static const char *rom_directory = "roms";
static uint8_t rom[SPECTRUM_PLUS2_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_128K];
static uint8_t framebuffer[SPECTRUM_LARGEST_FRAMEBUFFER];
static spectrum_t spectrum;

static int raster_width(void) { return spectrum.ula.frame.ticks_per_line * ULA_SAMPLES_PER_TICK; }

/* The character table the screen is read by: the 48K ROM's, which a +2
   carries as its second. */
static const uint8_t *font(void) {
  return rom + (spectrum.model == SPECTRUM_PLUS2 ? PLUS2_ROM_1 : 0) + FONT_IN_ROM;
}

/* The display file the ULA is drawing: a 48K's first 16K of RAM, a +2's
   bank 5 or bank 7. */
static const uint8_t *display_file(void) {
  if (spectrum.model == SPECTRUM_48K) {
    return ram;
  }
  unsigned bank = (spectrum.bank_register & SPECTRUM_BANK_SCREEN) ? 7 : 5;
  return ram + (size_t)bank * SPECTRUM_BANK_SIZE;
}

/* The display's top-left sample in the framebuffer: a retrace and a left
   border across, and as far down as the frame sync left the picture. */
static int display_left(void) {
  return (spectrum.ula.frame.retrace_ticks + ULA_LEFT_BORDER_TICKS) * ULA_SAMPLES_PER_TICK;
}

static int display_top(void) {
  return spectrum.ula.frame.first_display_line - SPECTRUM_PICTURE_SHIFT;
}

/* One 16K image, into the ROM at an offset. */
static bool load_rom(const char *name, size_t at) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", rom_directory, name);
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    TEST_FAIL("cannot open %s — run 'make roms'", path);
    return false;
  }
  size_t read = fread(rom + at, 1, 0x4000, file);
  fclose(file);
  if (read != 0x4000) {
    TEST_FAIL("%s is %zu bytes, expected %d", path, read, 0x4000);
    return false;
  }
  return true;
}

static void power_on(spectrum_model model, uint32_t ram_size) {
  memset(ram, 0x00, sizeof ram);
  memset(framebuffer, 0x00, sizeof framebuffer);
  spectrum_init(&spectrum, model, ram, ram_size, rom);
  spectrum_connect_monitor(&spectrum, framebuffer);
}

static bool power_on_48k(void) {
  if (!load_rom("spectrum48.rom", 0)) {
    return false;
  }
  power_on(SPECTRUM_48K, SPECTRUM_RAM_48K);
  return true;
}

static bool power_on_plus2(void) {
  if (!load_rom("spectrumplus2-0.rom", 0) || !load_rom("spectrumplus2-1.rom", PLUS2_ROM_1)) {
    return false;
  }
  power_on(SPECTRUM_PLUS2, SPECTRUM_RAM_128K);
  return true;
}

static void run_frames(int frames) {
  for (int frame = 0; frame < frames; frame++) {
    for (uint32_t tick = 0; tick < spectrum.ula.frame.ticks_per_frame; tick++) {
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

/* The glyph in the ROM's own table, or '?' for a shape it does not draw. */
static char character_for(const uint8_t glyph[8]) {
  for (int code = FIRST_CODE; code < 128; code++) {
    if (memcmp(font() + (size_t)(code - FIRST_CODE) * 8, glyph, 8) == 0) {
      return (char)code;
    }
  }
  return '?';
}

static void screen_from_memory(char text[ULA_ROWS][ULA_COLUMNS + 1]) {
  for (int row = 0; row < ULA_ROWS; row++) {
    for (int column = 0; column < ULA_COLUMNS; column++) {
      uint8_t glyph[8];
      for (int line = 0; line < 8; line++) {
        glyph[line] =
            display_file()[ula_display_address(row * 8 + line, column) - SPECTRUM_RAM_BASE];
      }
      text[row][column] = character_for(glyph);
    }
    text[row][ULA_COLUMNS] = '\0';
  }
}

/* The same reading, taken off the framebuffer. Whichever colour fills most
   of a cell is its paper; the rest is ink. */
static void screen_from_framebuffer(char text[ULA_ROWS][ULA_COLUMNS + 1]) {
  for (int row = 0; row < ULA_ROWS; row++) {
    for (int column = 0; column < ULA_COLUMNS; column++) {
      int counts[16] = {0};
      for (int line = 0; line < 8; line++) {
        for (int pixel = 0; pixel < 8; pixel++) {
          int y = display_top() + row * 8 + line;
          int x = display_left() + column * 8 + pixel;
          counts[framebuffer[y * raster_width() + x] & 0x0F]++;
        }
      }
      int paper = 0;
      for (int colour = 1; colour < 16; colour++) {
        if (counts[colour] > counts[paper]) {
          paper = colour;
        }
      }
      uint8_t glyph[8];
      for (int line = 0; line < 8; line++) {
        uint8_t bits = 0;
        for (int pixel = 0; pixel < 8; pixel++) {
          int y = display_top() + row * 8 + line;
          int x = display_left() + column * 8 + pixel;
          if ((framebuffer[y * raster_width() + x] & 0x0F) != paper) {
            bits |= (uint8_t)(0x80 >> pixel);
          }
        }
        glyph[line] = bits;
      }
      text[row][column] = character_for(glyph);
    }
    text[row][ULA_COLUMNS] = '\0';
  }
}

/* The message is Sinclair's, and its first character is the copyright sign,
   which the ROM's table draws at code 127. */
static void the_48k_boots_to_its_copyright_message(void) {
  if (!power_on_48k()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  char expected[ULA_COLUMNS + 1];
  snprintf(expected, sizeof expected, "%c 1982 Sinclair Research Ltd", 127);
  if (strncmp(text[ULA_ROWS - 1], expected, strlen(expected)) != 0) {
    TEST_FAIL("bottom line reads |%s|", text[ULA_ROWS - 1]);
  }
}

static void the_screen_reads_the_same_through_the_beam(void) {
  if (!power_on_48k()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  char from_memory[ULA_ROWS][ULA_COLUMNS + 1];
  char from_beam[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(from_memory);
  screen_from_framebuffer(from_beam);
  /* Read off the beam alone first, so that two blank screens agreeing
     cannot pass for a working video path. */
  char expected[ULA_COLUMNS + 1];
  snprintf(expected, sizeof expected, "%c 1982 Sinclair Research Ltd", 127);
  if (strncmp(from_beam[ULA_ROWS - 1], expected, strlen(expected)) != 0) {
    TEST_FAIL("the beam's bottom line reads |%s|", from_beam[ULA_ROWS - 1]);
  }
  for (int row = 0; row < ULA_ROWS; row++) {
    if (strcmp(from_memory[row], from_beam[row]) != 0) {
      TEST_FAIL("row %d: display file |%s| but beam |%s|", row, from_memory[row], from_beam[row]);
      return;
    }
  }
}

static void basic_does_arithmetic_it_is_typed(void) {
  if (!power_on_48k()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  /* At the K cursor, P is the PRINT keyword; + is symbol shift with K. */
  type_key(SPECTRUM_KEY(5, 0), KEYBOARD_NO_KEY);       /* P */
  type_key(SPECTRUM_KEY(3, 1), KEYBOARD_NO_KEY);       /* 2 */
  type_key(SPECTRUM_KEY(6, 2), SPECTRUM_SYMBOL_SHIFT); /* + */
  type_key(SPECTRUM_KEY(3, 1), KEYBOARD_NO_KEY);       /* 2 */
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);

  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  TEST_EQUAL(text[0][0], '4');
  if (strncmp(text[ULA_ROWS - 1], "0 OK", 4) != 0) {
    TEST_FAIL("bottom line reads |%s|, expected BASIC's OK report", text[ULA_ROWS - 1]);
  }
}

/* The table of key legends, judged by the firmware rather than by itself:
   the characters are looked up, the keys held down, and what BASIC makes of
   them read back off the screen. `p` at the K cursor is the PRINT keyword,
   the quotes are symbol-shifted, and the letters inside them arrive as
   letters because by then the cursor is in L mode. */
static void the_keyboard_types_what_is_printed_on_it(void) {
  if (!power_on_48k()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  const char *line = "p\"a1z.\"";
  for (const char *at = line; *at != '\0'; at++) {
    spectrum_shift shift = SPECTRUM_NO_SHIFT;
    keyboard_key key = spectrum_key_for_character(*at, &shift);
    if (key == KEYBOARD_NO_KEY) {
      TEST_FAIL("no key carries '%c'", *at);
      return;
    }
    keyboard_key held = KEYBOARD_NO_KEY;
    if (shift == SPECTRUM_WITH_CAPS_SHIFT) {
      held = SPECTRUM_CAPS_SHIFT;
    } else if (shift == SPECTRUM_WITH_SYMBOL_SHIFT) {
      held = SPECTRUM_SYMBOL_SHIFT;
    }
    type_key(key, held);
  }
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);

  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  if (strncmp(text[0], "a1z.", 4) != 0) {
    TEST_FAIL("the top line reads |%s|, expected what was typed", text[0]);
  }
}

/* The ROM's own tape loader, which measures the time between edges on the
   EAR line and has no other way of knowing what it is being given. Entered
   with the flag byte it expects in A, the destination in IX, the length in
   DE and carry set to load rather than verify; it returns with carry set if
   the block arrived and its checksum agreed.

   Sources:
   - "The complete Spectrum ROM disassembly" (Ian Logan and Frank O'Hara),
     the LD-BYTES routine at &0556 and its entry conditions. */
#define LD_BYTES 0x0556

/* A header's pilot is 8063 pulses of 2168 T-states, which is five seconds,
   and a whole tape is that twice over with a second of pause between,
   behind the cassette's leader tape, which is not quite six (tzx.h). Twenty
   seconds is room for all of it. */
#define MOST_FRAMES_TO_LOAD 1000

/* One headerless block: the flag a data block carries, sixteen bytes, and
   the checksum the ROM will insist on, which is every byte before it
   exclusive or'd together. Each test below wraps it differently. */
#define BLOCK_BYTES 18
static void fill_block(uint8_t *block) {
  static const uint8_t bytes[BLOCK_BYTES - 1] = {0xFF, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                                 'I',  'J', 'K', 'L', 'M', 'N', 'O', 'P'};
  memcpy(block, bytes, sizeof bytes);
  block[BLOCK_BYTES - 1] = 0;
  for (size_t index = 0; index + 1 < BLOCK_BYTES; index++) {
    block[BLOCK_BYTES - 1] ^= block[index];
  }
}

/* Play an image to LD-BYTES and hold it to the block coming back whole. */
static void the_rom_loads_the_block_from(const uint8_t *image, uint32_t length) {
  if (!power_on_48k()) {
    return;
  }
  uint8_t block[BLOCK_BYTES];
  fill_block(block);

  static tape_t tape;
  static tzx_t reader;
  const char *problem = NULL;
  if (!tzx_open(&reader, image, length,
                (tzx_machine){.rules = TZX_SPECTRUM, .clock_hertz = SPECTRUM_48K_CLOCK_HERTZ},
                &problem)) {
    TEST_FAIL("the tape was refused: %s", problem);
    return;
  }
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);
  tape_play(&tape);

  /* LD IX,&9000 : LD DE,16 : LD A,&FF : SCF : CALL LD-BYTES : HALT */
  static const uint8_t program[] = {0xDD,          0x21, 0x00, 0x90, 0x11, 0x10,
                                    0x00,          0x3E, 0xFF, 0x37, 0xCD, LD_BYTES & 0xFF,
                                    LD_BYTES >> 8, 0x76};
  for (size_t index = 0; index < sizeof program; index++) {
    spectrum_poke(&spectrum, (uint16_t)(0x8000 + index), program[index]);
  }
  spectrum.cpu.pc = 0x8000;
  spectrum.cpu.sp = 0x7FF0;

  for (long tick = 0; tick < MOST_FRAMES_TO_LOAD * (long)spectrum.ula.frame.ticks_per_frame;
       tick++) {
    spectrum_tick(&spectrum);
    if (spectrum.cpu.halted) {
      break;
    }
  }
  if (!spectrum.cpu.halted) {
    TEST_FAIL("the loader never returned");
    return;
  }
  TEST_CHECK((spectrum.cpu.f & Z80_FLAG_C) != 0); /* the block arrived whole */
  for (int index = 0; index < 16; index++) {
    uint8_t want = block[1 + index];
    uint8_t got = spectrum_peek(&spectrum, (uint16_t)(0x9000 + index));
    if (got != want) {
      TEST_FAIL("byte %d of the block loaded as &%02X, not &%02X", index, got, want);
      return;
    }
  }
}

static void a_block_loads_through_the_roms_own_loader(void) {
  static uint8_t image[2 + BLOCK_BYTES] = {BLOCK_BYTES, 0x00};
  fill_block(image + 2);
  the_rom_loads_the_block_from(image, sizeof image);
}

#define TZX_HEAD 'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20

/* The shape most images of Speedlock tapes give a block: its leader written
   as a loop of short tones, and its syncs and data as blocks of their own.
   Ten passes of 322 pulses make a leader of 3220, the ROM's own length for
   data near enough. Played once it would be over inside the second LD-BYTES
   waits, after the first edge it hears, before it starts counting a leader. */
static void a_leader_written_as_a_loop_loads_through_the_roms_own_loader(void) {
  static uint8_t image[] = {
      TZX_HEAD,    0x24, 0x0A, 0x00,             /* loop, ten passes */
      0x12,        0x78, 0x08, 0x42, 0x01,       /* a tone of 322 pulses of 2168 */
      0x25,                                      /* loop end */
      0x13,        0x02, 0x9B, 0x02, 0xDF, 0x02, /* two syncs, 667 and 735 */
      0x14,        0x57, 0x03, 0xAE, 0x06,       /* pure data: bits of 855 and 1710 */
      0x08,        0xE8, 0x03,                   /* every bit of the last byte, a second's pause */
      BLOCK_BYTES, 0x00, 0x00,                   /* the block's length */
      0,           0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  fill_block(image + sizeof image - BLOCK_BYTES);
  the_rom_loads_the_block_from(image, sizeof image);
}

/* A block asking for no pause and then a stop, which is how some images of
   Speedlock 4 tapes end their parts. With a data block's 3223 pilot pulses,
   two syncs and an even number of bit pulses, the last pulse is low, and the
   edge that ends it is the last bit's second: the reader has to give it
   before the stop, or LD-BYTES waits for it in vain. */
static void a_block_ending_low_before_a_stop_loads_through_the_roms_own_loader(void) {
  static uint8_t image[] = {
      TZX_HEAD, 0x10, 0x00, 0x00, BLOCK_BYTES, 0x00, /* a standard block, no pause */
      0,        0,    0,    0,    0,           0,    0, 0,    0,    0,    0,
      0,        0,    0,    0,    0,           0,    0, 0x20, 0x00, 0x00, /* and a stop */
  };
  fill_block(image + 10 + 5);
  the_rom_loads_the_block_from(image, sizeof image);
}

/* A header naming a BASIC program that autostarts at line 10, and the
   program: 10 PRINT "TAPE OK". Both blocks carry the flag and checksum a
   tape carries. */
static uint8_t program_tape[] = {
    0x13, 0x00, 0x00,                                            /* block, flag: a header */
    0x00,                                                        /* type 0: a program */
    't',  'a',  'p',  'e',  ' ', ' ', ' ', ' ', ' ', ' ',        /* its name */
    0x0F, 0x00,                                                  /* the program is 15 bytes */
    0x0A, 0x00,                                                  /* autostart at line 10 */
    0x0F, 0x00,                                                  /* and no variables after it */
    0x00,                                                        /* checksum, filled in below */
    0x11, 0x00, 0xFF,                                            /* block, flag: data */
    0x00, 0x0A, 0x0B, 0x00,                                      /* line 10, eleven bytes of it */
    0xF5, 0x22, 'T',  'A',  'P', 'E', ' ', 'O', 'K', 0x22, 0x0D, /* PRINT "TAPE OK" */
    0x00,                                                        /* checksum, filled in below */
};

/* A block's checksum is every byte of it from the flag onwards, exclusive
   or'd together, and it is the last byte of the block. */
static void sum_the_program_tape(void) {
  static const size_t header_at = 2, header_checksum_at = 20;
  static const size_t data_at = 23, data_checksum_at = 39;
  program_tape[header_checksum_at] = 0;
  program_tape[data_checksum_at] = 0;
  for (size_t index = header_at; index < header_checksum_at; index++) {
    program_tape[header_checksum_at] ^= program_tape[index];
  }
  for (size_t index = data_at; index < data_checksum_at; index++) {
    program_tape[data_checksum_at] ^= program_tape[index];
  }
}

static tape_t program_deck;
static tzx_t program_reader;

/* The program's tape in the machine's deck, at the machine's own clock. */
static bool insert_the_program_tape(void) {
  sum_the_program_tape();
  const char *problem = NULL;
  if (!tzx_open(
          &program_reader, program_tape, (uint32_t)sizeof program_tape,
          (tzx_machine){.rules = TZX_SPECTRUM, .clock_hertz = spectrum_clock_hertz(&spectrum)},
          &problem)) {
    TEST_FAIL("the tape was refused: %s", problem);
    return false;
  }
  tape_init(&program_deck);
  tape_insert(&program_deck, tzx_next_pulse, &program_reader);
  spectrum_insert_tape(&spectrum, &program_deck);
  return true;
}

/* The whole path a person takes: type LOAD "" at the prompt, press PLAY,
   and let the firmware find a program on the tape and run it. Nothing here
   reaches into the machine — the header is read by the ROM, the block is
   loaded by the ROM, and BASIC runs what arrives. */
static void a_program_loads_off_a_tape_and_runs(void) {
  if (!power_on_48k() || !insert_the_program_tape()) {
    return;
  }
  tape_t *tape = &program_deck;
  run_frames(FRAMES_TO_PROMPT);
  type_key(SPECTRUM_KEY(6, 3), KEYBOARD_NO_KEY);       /* J, which is LOAD */
  type_key(SPECTRUM_KEY(5, 0), SPECTRUM_SYMBOL_SHIFT); /* " */
  type_key(SPECTRUM_KEY(5, 0), SPECTRUM_SYMBOL_SHIFT); /* " again, which the ROM */
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);           /*   only takes if it saw the */
  tape_play(tape);                                     /*   first one let go */
  run_frames(MOST_FRAMES_TO_LOAD);

  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  /* The ROM prints what it found on the tape, and then the program it
     started prints for itself. */
  if (strncmp(text[1], "Program: tape", 13) != 0) {
    TEST_FAIL("the header line reads |%s|, expected what the ROM found", text[1]);
    return;
  }
  if (strncmp(text[2], "TAPE OK", 7) != 0) {
    TEST_FAIL("the line under it reads |%s|, expected what the program printed", text[2]);
    return;
  }
  if (strncmp(text[ULA_ROWS - 1], "0 OK, 10:1", 10) != 0) {
    TEST_FAIL("the report reads |%s|, expected BASIC's after line 10", text[ULA_ROWS - 1]);
  }
}

static void type_beep_1_0(void) {
  type_key(SPECTRUM_CAPS_SHIFT, SPECTRUM_SYMBOL_SHIFT); /* the E cursor */
  type_key(SPECTRUM_KEY(0, 1), SPECTRUM_SYMBOL_SHIFT);  /* BEEP */
  type_key(SPECTRUM_KEY(3, 0), KEYBOARD_NO_KEY);        /* 1 */
  type_key(SPECTRUM_KEY(7, 3), SPECTRUM_SYMBOL_SHIFT);  /* , */
  type_key(SPECTRUM_KEY(4, 0), KEYBOARD_NO_KEY);        /* 0 */
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
}

/* "the pitch is given in semitones above middle C" (Steven Vickers, ZX
   Spectrum BASIC Programming, chapter 19,
   https://worldofspectrum.org/ZXBasicManual/zxmanchap19.html), so BEEP 1,0
   is middle C, which the ROM plays by flipping bit 4 of port &FE once a
   half cycle with HL = &066A ("The complete Spectrum ROM disassembly", Ian
   Logan and Frank O'Hara, BEEPER at &03B5). Counted from that listing at
   the Z80's documented timings, each half cycle is 4·HL + 118 T-states, so
   a whole cycle is 13,372, 261.74Hz against 261.63Hz. An OUT to the ULA is
   charged up to six T-states inside the picture, once and ahead of its
   write every time, so an interval between two rising edges holds two
   OUTs' charges and can be up to twelve longer. A hundred cycles of the
   note, once it has begun, are heard where the board sends its sound,
   which bit 4 raises on either model. */
static void hear_middle_c(void) {
  uint16_t level = spectrum_sound(&spectrum);
  long since_rising = -1;
  long shortest = 0;
  int cycles = 0;
  for (long tick = 0; tick < 50 * (long)spectrum.ula.frame.ticks_per_frame && cycles < 100;
       tick++) {
    spectrum_tick(&spectrum);
    uint16_t now = spectrum_sound(&spectrum);
    if (since_rising >= 0) {
      since_rising++;
    }
    if (now > level) {
      if (since_rising >= 0) {
        if (since_rising < 13372 || since_rising > 13372 + 12) {
          TEST_FAIL("a cycle took %ld T-states", since_rising);
        }
        shortest = cycles == 0 || since_rising < shortest ? since_rising : shortest;
        cycles++;
      }
      since_rising = 0;
    }
    level = now;
  }
  TEST_EQUAL(cycles, 100);
  TEST_EQUAL(shortest, 13372);
}

static void beep_1_0_plays_middle_c(void) {
  if (!power_on_48k()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_beep_1_0();
  hear_middle_c();
}

/* Whether a line of the screen, read off the display file, holds what is
   expected from a column on. */
static bool reads(int row, int column, const char *expected) {
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  if (strncmp(text[row] + column, expected, strlen(expected)) != 0) {
    TEST_FAIL("row %d reads |%s|, expected |%s| at column %d", row, text[row], expected, column);
    return false;
  }
  return true;
}

/* Type text as the keyboard carries it, each character with the shift it
   needs. */
static void type_text(const char *text) {
  for (const char *at = text; *at != '\0'; at++) {
    spectrum_shift shift = SPECTRUM_NO_SHIFT;
    keyboard_key key = spectrum_key_for_character(*at, &shift);
    keyboard_key held = KEYBOARD_NO_KEY;
    if (shift == SPECTRUM_WITH_CAPS_SHIFT) {
      held = SPECTRUM_CAPS_SHIFT;
    } else if (shift == SPECTRUM_WITH_SYMBOL_SHIFT) {
      held = SPECTRUM_SYMBOL_SHIFT;
    }
    type_key(key, held);
  }
}

/* The +2's menu: the 128's without the Tape Tester. Paul Farrow's page on
   the +2's ROMs: "The Tape Tester menu option and routine were removed",
   and "ROM routine entry addresses were shifted due to the above changes"
   (http://www.fruitcake.plus.com/Sinclair/Spectrum128/ROMDisassembly/Spectrum128ROMDisassembly2.htm).
   Before the menu, the 128's ROM 0 pages each of the eight banks in and
   checks it, and would stop at a coloured border if one failed (Farrow's
   disassembly of it, cited in ula.h). */
#define PLUS2_MENU_COLUMN 8
#define PLUS2_MENU_FIRST_ROW 8

static void the_plus2_boots_to_its_menu(void) {
  if (!power_on_plus2()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  const char *items[] = {"Tape Loader", "128 BASIC", "Calculator", "48 BASIC"};
  for (int item = 0; item < 4; item++) {
    if (!reads(PLUS2_MENU_FIRST_ROW + item, PLUS2_MENU_COLUMN, items[item])) {
      return;
    }
  }
  reads(ULA_ROWS - 1, 14, "Electronics plc");
}

static void the_plus2s_menu_reads_the_same_through_the_beam(void) {
  if (!power_on_plus2()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  char from_memory[ULA_ROWS][ULA_COLUMNS + 1];
  char from_beam[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(from_memory);
  screen_from_framebuffer(from_beam);
  if (strncmp(from_beam[PLUS2_MENU_FIRST_ROW] + PLUS2_MENU_COLUMN, "Tape Loader", 11) != 0) {
    TEST_FAIL("the beam's first item reads |%s|", from_beam[PLUS2_MENU_FIRST_ROW]);
  }
  for (int row = 0; row < ULA_ROWS; row++) {
    if (strcmp(from_memory[row], from_beam[row]) != 0) {
      TEST_FAIL("row %d: display file |%s| but beam |%s|", row, from_memory[row], from_beam[row]);
      return;
    }
  }
}

/* "In 128 BASIC none of these special key entries apply. Instead, you type
   each word in full, letter by letter" (Spectrum 128 Introduction, Sinclair
   Research, 1986, transcribed by Damien Burke,
   https://jacekmatulewski.fizyka.umk.pl/zx/doc/man128.txt). The menu moves
   on CAPS SHIFT and 6, the cursor down. */
static void the_plus2s_128_basic_does_arithmetic_typed_letter_by_letter(void) {
  if (!power_on_plus2()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_key(SPECTRUM_KEY(4, 4), SPECTRUM_CAPS_SHIFT);
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);
  type_text("print 2+2");
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);
  if (reads(1, 0, "4")) {
    reads(ULA_ROWS - 1, 0, "0 OK");
  }
}

/* The menu's 48 BASIC writes 0x30 to the bank register — the 48K ROM, and
   the lock — and starts that ROM afresh (Farrow, at the 128's L1B47, "Force
   48 mode"), whose message the +2 carries as "© 1982 Amstrad". */
static void the_plus2s_48_basic_locks_the_paging(void) {
  if (!power_on_plus2()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  for (int item = 0; item < 3; item++) {
    type_key(SPECTRUM_KEY(4, 4), SPECTRUM_CAPS_SHIFT);
  }
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_TO_PROMPT);
  TEST_EQUAL(spectrum.bank_register, SPECTRUM_BANK_ROM | SPECTRUM_BANK_LOCK);
  char expected[ULA_COLUMNS + 1];
  snprintf(expected, sizeof expected, "%c 1982 Amstrad", 127);
  reads(ULA_ROWS - 1, 0, expected);
}

/* PLAY "c" is middle C, which the ROM's note table plays on channel A with
   a tone period of 0x01A8, 424 (Farrow: "Octave 5, Note 60 - C ... C4
   Middle C"). A tone channel's wave lasts 16 clocks of the sound chip a
   period (ay38910.h), and the chip runs at half the processor's clock, so
   the wave is 2·16·424 = 13,568 T-states. It is heard where the +2 sends its
   sound, the modulator's input, which rises as the channel falls. */
static void play_c_sounds_middle_c_through_the_sound_chip(void) {
  if (!power_on_plus2()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_key(SPECTRUM_KEY(4, 4), SPECTRUM_CAPS_SHIFT);
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_PER_KEY);
  type_text("play \"c\"");
  keyboard_press(&spectrum.keyboard, SPECTRUM_ENTER);
  run_frames(FRAMES_PER_KEY);
  keyboard_release_all(&spectrum.keyboard);

  TEST_EQUAL(spectrum.psg.registers[0] | spectrum.psg.registers[1] << 8, 424);
  uint16_t level = spectrum_sound(&spectrum);
  long since_rising = -1;
  int waves = 0;
  for (long tick = 0; tick < 10 * (long)spectrum.ula.frame.ticks_per_frame && waves < 20; tick++) {
    spectrum_tick(&spectrum);
    uint16_t now = spectrum_sound(&spectrum);
    if (since_rising >= 0) {
      since_rising++;
    }
    if (now > level) {
      if (since_rising >= 0) {
        TEST_EQUAL(since_rising, 13568);
        waves++;
      }
      since_rising = 0;
    }
    level = now;
  }
  TEST_EQUAL(waves, 20);
}

/* BEEP in the +2's 48 BASIC is the 48K's BEEPER: compared byte for byte,
   the +2's second ROM differs from the 48K's at the 128's patches, its tape
   message and its copyright, none of them near the routine. It plays on the
   ULA's MIC pin, by bit 4. */
static void beep_1_0_plays_middle_c_on_the_plus2(void) {
  if (!power_on_plus2()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  for (int item = 0; item < 3; item++) {
    type_key(SPECTRUM_KEY(4, 4), SPECTRUM_CAPS_SHIFT);
  }
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY);
  run_frames(FRAMES_TO_PROMPT);
  type_beep_1_0();
  hear_middle_c();
}

/* The +2's menu's Tape Loader loads the same program off the same tape,
   played at the +2's own clock, and the program prints for itself. The
   option copies LOAD "" into the line editing area and executes it, as the
   128's L1AF1 does in Farrow's disassembly. */
static void the_plus2s_tape_loader_loads_a_program_and_runs_it(void) {
  if (!power_on_plus2() || !insert_the_program_tape()) {
    return;
  }
  run_frames(FRAMES_TO_PROMPT);
  type_key(SPECTRUM_ENTER, KEYBOARD_NO_KEY); /* the menu's first item */
  tape_play(&program_deck);
  run_frames(MOST_FRAMES_TO_LOAD);
  char text[ULA_ROWS][ULA_COLUMNS + 1];
  screen_from_memory(text);
  for (int row = 0; row < ULA_ROWS; row++) {
    if (strncmp(text[row], "TAPE OK", 7) == 0) {
      return;
    }
  }
  TEST_FAIL("nothing on the screen reads TAPE OK; its top line reads |%s|", text[0]);
}

int main(int argc, char **argv) {
  if (argc > 1) {
    rom_directory = argv[1];
  }
  TEST_RUN(the_48k_boots_to_its_copyright_message);
  TEST_RUN(the_screen_reads_the_same_through_the_beam);
  TEST_RUN(basic_does_arithmetic_it_is_typed);
  TEST_RUN(the_keyboard_types_what_is_printed_on_it);
  TEST_RUN(a_block_loads_through_the_roms_own_loader);
  TEST_RUN(a_leader_written_as_a_loop_loads_through_the_roms_own_loader);
  TEST_RUN(a_block_ending_low_before_a_stop_loads_through_the_roms_own_loader);
  TEST_RUN(a_program_loads_off_a_tape_and_runs);
  TEST_RUN(beep_1_0_plays_middle_c);
  TEST_RUN(the_plus2_boots_to_its_menu);
  TEST_RUN(the_plus2s_menu_reads_the_same_through_the_beam);
  TEST_RUN(the_plus2s_128_basic_does_arithmetic_typed_letter_by_letter);
  TEST_RUN(the_plus2s_48_basic_locks_the_paging);
  TEST_RUN(play_c_sounds_middle_c_through_the_sound_chip);
  TEST_RUN(beep_1_0_plays_middle_c_on_the_plus2);
  TEST_RUN(the_plus2s_tape_loader_loads_a_program_and_runs_it);
  return TEST_REPORT("spectrum firmware");
}
