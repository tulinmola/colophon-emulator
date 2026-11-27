/*
 * spectrum_test — the machine, running programs written here.
 *
 * The board's own decisions are what is under test: what answers where in
 * memory, which ports reach the ULA, how the keyboard hangs off the address
 * lines, that the ULA's interrupt reaches the processor, and what the chip
 * charges the processor for the bus. The firmware is not involved; the
 * programs are a few bytes each, put in the ROM.
 *
 * The T-state counts at the end are the published ones, transcribed from the
 * pattern given for each opcode and from the four rows given for a port —
 * neither worked out from anything here. Where they fall is counted as ula.h
 * counts. The +2's tests that follow them move the same counts to its ULA's
 * frame, 27 T-states later, and take the rest from the measurements and
 * documents each cites. The wiki says it counts from the same T-state, but its numbers
 * agree with Fuse's, whose processor can answer the interrupt a T-state
 * sooner, so each of them is one more here: its first contended 14335 is
 * 14336.
 *
 * Sources:
 * - "Contended memory" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended%20memory — the delay
 *   owed at each T-state of a frame, and the instruction breakdown table,
 *   which gives every opcode as the addresses it puts on the bus and how
 *   long each stands there.
 * - "Contended I/O" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended%20I/O — the four rows
 *   by which a port access is charged.
 */
#include <string.h>

#include "spectrum.h"
#include "test.h"
#include "tzx.h"

/* Room for either model: each reads the first ram_size bytes of the RAM
   and the first of the ROM its own ROM's size. */
static spectrum_t spectrum;
static uint8_t rom[SPECTRUM_PLUS2_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_128K];
static uint8_t framebuffer[SPECTRUM_LARGEST_FRAMEBUFFER];

static void power_on(spectrum_model model, uint32_t ram_size) {
  memset(rom, 0x00, sizeof rom);
  memset(ram, 0x00, sizeof ram);
  memset(framebuffer, 0xFF, sizeof framebuffer);
  spectrum_init(&spectrum, model, ram, ram_size, rom);
  spectrum_connect_monitor(&spectrum, framebuffer);
}

static void power_on_48k(uint32_t ram_size) { power_on(SPECTRUM_48K, ram_size); }

static void power_on_plus2(void) { power_on(SPECTRUM_PLUS2, SPECTRUM_RAM_128K); }

/* One of a +2's eight banks, as the RAM holds them, in order. */
static uint8_t *bank(unsigned number) { return ram + (size_t)number * SPECTRUM_BANK_SIZE; }

static void rom_program(const uint8_t *bytes, size_t length) { memcpy(rom, bytes, length); }

static void put_at(uint16_t address, const uint8_t *bytes, size_t length) {
  memcpy(rom + address, bytes, length);
}

/* The processor back at the top, with the rest of the machine left standing —
   which powering on again would not do, since it takes the deck out too. */
static void restart_the_processor(void) {
  z80_init(&spectrum.cpu);
  spectrum.held_ticks = 0;
  spectrum.port_charges_left = 0;
  spectrum.reading_port = false;
  spectrum.latching_bank_register = false;
}

/* Run until the processor halts, or give up. Returns whether it halted. */
static bool run_to_halt(void) {
  for (long tick = 0; tick < 200000; tick++) {
    spectrum_tick(&spectrum);
    if (spectrum.cpu.halted) {
      return true;
    }
  }
  return false;
}

static void run_frames(int frames) {
  for (int frame = 0; frame < frames; frame++) {
    for (uint32_t tick = 0; tick < spectrum.ula.frame.ticks_per_frame; tick++) {
      spectrum_tick(&spectrum);
    }
  }
}

static void reset_fetches_from_the_rom(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0x3E, 0x42, 0x76}; /* LD A,&42 : HALT */
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a, 0x42);
}

static void ram_answers_from_4000_and_the_rom_refuses_writes(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  /* LD A,&5A : LD (&4000),A : LD (&0000),A : LD B,(HL) with HL=0 : HALT */
  const uint8_t program[] = {0x3E, 0x5A,       /* LD A,&5A       */
                             0x32, 0x00, 0x40, /* LD (&4000),A   */
                             0x32, 0x00, 0x00, /* LD (&0000),A   */
                             0x21, 0x00, 0x00, /* LD HL,&0000    */
                             0x46,             /* LD B,(HL)      */
                             0x76};            /* HALT           */
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(ram[0], 0x5A);         /* the write to RAM stuck */
  TEST_EQUAL(spectrum.cpu.b, 0x3E); /* the ROM still holds its first byte */
  TEST_EQUAL(spectrum_peek(&spectrum, 0x0000), 0x3E);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x4000), 0x5A);
}

static void a_16k_machine_does_not_answer_above_7fff(void) {
  power_on_48k(SPECTRUM_RAM_16K);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x7FFF), 0x00);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8000), 0xFF);
  spectrum_poke(&spectrum, 0x8000, 0x5A);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8000), 0xFF);

  power_on_48k(SPECTRUM_RAM_48K);
  spectrum_poke(&spectrum, 0x8000, 0x5A);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8000), 0x5A);
  TEST_EQUAL(spectrum_peek(&spectrum, 0xFFFF), 0x00);
}

static void every_even_port_reaches_the_ula(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  /* LD A,3 : OUT (&FE),A : LD A,5 : OUT (&FF),A : HALT — the odd port
     should leave the border where the even one put it. */
  const uint8_t program[] = {0x3E, 0x03, 0xD3, 0xFE, 0x3E, 0x05, 0xD3, 0xFF, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.ula.border, 3);

  /* And an even port that is not &FE reaches it just the same. */
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t other[] = {0x3E, 0x06, 0xD3, 0x40, 0x76}; /* OUT (&40),A */
  rom_program(other, sizeof other);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.ula.border, 6);
}

static void a_pressed_key_reads_low_on_its_own_half_row(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_ENTER); /* half-row 6, bit 0 */
  /* LD BC,&BFFE : IN A,(C) : HALT — A14 low selects half-row 6. */
  const uint8_t program[] = {0x01, 0xFE, 0xBF, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x1F, 0x1E); /* bit 0 pulled down */
  TEST_EQUAL(spectrum.cpu.a & 0xA0, 0xA0); /* bits 5 and 7 read high */
}

static void a_half_row_that_is_not_selected_reads_nothing(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_ENTER);
  /* LD BC,&FEFE : IN A,(C) : HALT — A8 low selects half-row 0, not 6. */
  const uint8_t program[] = {0x01, 0xFE, 0xFE, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x1F, 0x1F);
}

static void selected_half_rows_read_together(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_CAPS_SHIFT);   /* half-row 0, bit 0 */
  keyboard_press(&spectrum.keyboard, SPECTRUM_SYMBOL_SHIFT); /* half-row 7, bit 1 */
  /* LD BC,&7EFE : IN A,(C) : HALT — A8 and A15 both low. */
  const uint8_t program[] = {0x01, 0xFE, 0x7E, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x1F, 0x1C); /* both bits down at once */
}

static void the_ula_interrupts_once_a_frame(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  /* IM 1 : EI : wait: HALT : JR wait — the jump matters. RETI returns past
     the HALT, so without it the processor walks off through the ROM. */
  const uint8_t program[] = {0xED, 0x56, 0xFB, 0x76, 0x18, 0xFD};
  rom_program(program, sizeof program);
  /* The handler counts itself into RAM and goes back to waiting. */
  const uint8_t handler[] = {0x21, 0x00, 0x40, /* LD HL,&4000 */
                             0x34,             /* INC (HL)    */
                             0xFB,             /* EI          */
                             0xED, 0x4D};      /* RETI        */
  put_at(0x0038, handler, sizeof handler);

  for (int frame = 1; frame <= 20; frame++) {
    run_frames(1);
    if (ram[0] != frame) {
      TEST_FAIL("after %d frames the handler has run %u times", frame, ram[0]);
      break;
    }
  }
  TEST_EQUAL(spectrum.cpu.im, 1);
}

static void the_ear_socket_reads_on_bit_6(void) {
  /* LD BC,&FEFE : IN A,(C) : HALT */
  const uint8_t program[] = {0x01, 0xFE, 0xFE, 0xED, 0x78, 0x76};
  power_on_48k(SPECTRUM_RAM_48K);
  rom_program(program, sizeof program);
  spectrum.ear = false;
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x40, 0x00);

  power_on_48k(SPECTRUM_RAM_48K);
  rom_program(program, sizeof program);
  spectrum.ear = true;
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x40, 0x40);
}

/* Putnik's Issue 3 column, 0.34V, 0.66V, 3.56V and 3.7V for bits 4 and 3
   written as 00, 01, 10 and 11 (https://zx48.8bitchip.info/cassport.htm),
   less the two junctions at the 0.7V each spectrum.h takes them at. */
static void bit_4_sounds_the_speaker_and_bit_3_alone_does_not(void) {
  const struct {
    uint8_t written;
    uint16_t tenths_of_millivolts;
  } rows[] = {{0x00, 0}, {0x08, 0}, {0x10, 21600}, {0x18, 23000}};
  for (size_t row = 0; row < sizeof rows / sizeof rows[0]; row++) {
    /* LD A,n : OUT (&FE),A : HALT */
    const uint8_t program[] = {0x3E, rows[row].written, 0xD3, 0xFE, 0x76};
    power_on_48k(SPECTRUM_RAM_48K);
    rom_program(program, sizeof program);
    TEST_CHECK(run_to_halt());
    TEST_EQUAL(spectrum_sound_speaker(&spectrum), rows[row].tenths_of_millivolts);
  }
}

static void an_odd_port_reaches_nothing_and_floats(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_ENTER);
  /* LD BC,&BFFF : IN A,(C) : HALT — the same half-row as would answer at
     &BFFE, but A0 is high, so the ULA does not. */
  const uint8_t program[] = {0x01, 0xFF, 0xBF, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a, 0xFF);
}

/* The processor and the ULA must not drift apart by a T-state, and the
   first interrupt of all is where that can be pinned against arithmetic:
   IM 1 takes eight T-states, EI four and HALT four, so the processor
   reaches an instruction boundary at sixteen. Acceptance begins there and
   the acknowledge cycle puts IORQ on the bus in the first of its two wait
   states, its third T-state (Zilog UM0080, Figure 9).

   Later frames cannot be pinned this way — the handler's own length is not
   a multiple of the halted processor's four-T-state cycle, so acceptance
   walks a few T-states each frame, and the acknowledge cycle falls outside
   the window entirely whenever the processor latches the line on its last
   T-state. That the interrupt is nonetheless taken every frame is what
   the_ula_interrupts_once_a_frame checks. */
static void the_first_interrupt_is_accepted_sixteen_tstates_in(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0xED, 0x56, 0xFB, 0x76, 0x18, 0xFD}; /* IM 1 : EI : HALT : JR */
  rom_program(program, sizeof program);
  const uint8_t handler[] = {0xFB, 0xED, 0x4D}; /* EI : RETI */
  put_at(0x0038, handler, sizeof handler);

  int acknowledged_at = -1;
  for (uint32_t tick = 0; tick < spectrum.ula.frame.ticks_per_frame; tick++) {
    uint32_t at = spectrum.ula.frame_tick;
    uint64_t pins = spectrum_tick(&spectrum);
    if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
      acknowledged_at = (int)at;
      break;
    }
  }
  TEST_EQUAL(acknowledged_at, 18);
}

/* The raster the header gives hosts to size their buffers by is the one
   each model's ULA draws, and the largest framebuffer holds either. */
static void the_raster_is_the_frame_the_ula_draws(void) {
  const struct {
    spectrum_model model;
    uint32_t ram_size;
    uint32_t width;
    uint32_t height;
    uint32_t clock_hertz;
  } models[] = {
      {SPECTRUM_48K, SPECTRUM_RAM_48K, SPECTRUM_48K_RASTER_WIDTH, SPECTRUM_48K_RASTER_HEIGHT,
       SPECTRUM_48K_CLOCK_HERTZ},
      {SPECTRUM_PLUS2, SPECTRUM_RAM_128K, SPECTRUM_PLUS2_RASTER_WIDTH, SPECTRUM_PLUS2_RASTER_HEIGHT,
       SPECTRUM_PLUS2_CLOCK_HERTZ},
  };
  for (size_t index = 0; index < sizeof models / sizeof models[0]; index++) {
    power_on(models[index].model, models[index].ram_size);
    const ula_frame *frame = &spectrum.ula.frame;
    TEST_EQUAL(frame->ticks_per_line * ULA_SAMPLES_PER_TICK, models[index].width);
    TEST_EQUAL(frame->lines_per_frame, models[index].height);
    TEST_EQUAL(frame->ticks_per_frame, frame->ticks_per_line * frame->lines_per_frame);
    TEST_CHECK(models[index].width * models[index].height <= SPECTRUM_LARGEST_FRAMEBUFFER);
    TEST_EQUAL(spectrum_clock_hertz(&spectrum), models[index].clock_hertz);
  }
}

static void the_border_reaches_the_framebuffer(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0x3E, 0x02, 0xD3, 0xFE, 0x76}; /* border 2, then wait */
  rom_program(program, sizeof program);
  run_frames(2);

  /* A line below the picture is border from end to end. Its samples sit
     SPECTRUM_PICTURE_SHIFT lines above where the beam drew them. */
  int line = spectrum.ula.frame.first_display_line + ULA_DISPLAY_LINES + 4 - SPECTRUM_PICTURE_SHIFT;
  TEST_EQUAL(framebuffer[line * SPECTRUM_48K_RASTER_WIDTH + 200], 2);
  TEST_EQUAL(framebuffer[line * SPECTRUM_48K_RASTER_WIDTH + 430], 2);
}

static void the_picture_lands_where_the_frame_sync_leaves_it(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0x3E, 0x02, 0xD3, 0xFE, 0x76};
  rom_program(program, sizeof program);
  /* Paper 5 across the whole screen, so the picture differs from border 2. */
  memset(ram, 0x00, 6144);
  memset(ram + 6144, 0x28, 768);
  run_frames(2);

  /* Walk down the middle of the raster and find where the picture starts. */
  int top = -1;
  for (int line = 0; line < SPECTRUM_48K_RASTER_HEIGHT; line++) {
    if (framebuffer[line * SPECTRUM_48K_RASTER_WIDTH + 200] == 5) {
      top = line;
      break;
    }
  }
  TEST_EQUAL(top, spectrum.ula.frame.first_display_line - SPECTRUM_PICTURE_SHIFT);
}

/* Spot checks against the published matrix, not against the table restated:
   a letter, its capital, a digit, a red legend, and the two kinds of
   character no key carries. */
static void the_keyboard_finds_a_character_where_it_is_printed(void) {
  spectrum_shift shift = SPECTRUM_WITH_CAPS_SHIFT;
  TEST_EQUAL(spectrum_key_for_character('p', &shift), SPECTRUM_KEY(5, 0));
  TEST_EQUAL(shift, SPECTRUM_NO_SHIFT);
  TEST_EQUAL(spectrum_key_for_character('Z', &shift), SPECTRUM_KEY(0, 1));
  TEST_EQUAL(shift, SPECTRUM_WITH_CAPS_SHIFT);
  TEST_EQUAL(spectrum_key_for_character('7', &shift), SPECTRUM_KEY(4, 3));
  TEST_EQUAL(shift, SPECTRUM_NO_SHIFT);
  TEST_EQUAL(spectrum_key_for_character('+', &shift), SPECTRUM_KEY(6, 2));
  TEST_EQUAL(shift, SPECTRUM_WITH_SYMBOL_SHIFT);
  TEST_EQUAL(spectrum_key_for_character('"', &shift), SPECTRUM_KEY(5, 0));
  TEST_EQUAL(shift, SPECTRUM_WITH_SYMBOL_SHIFT);
  TEST_EQUAL(spectrum_key_for_character(' ', &shift), SPECTRUM_SPACE);
  TEST_EQUAL(shift, SPECTRUM_NO_SHIFT);
  /* The pound sign has a key and no ASCII; a token is not a character. */
  TEST_EQUAL(spectrum_key_for_character('~', &shift), KEYBOARD_NO_KEY);
  TEST_EQUAL(spectrum_key_for_character('`', &shift), KEYBOARD_NO_KEY);
}

/* The printed keyboard, transcribed a second time and from the same source,
   so that a slip in either copy shows. Positions and legends from "Sinclair
   ZX Specifications" (Martin Korth), Spectrum Keyboard Assignment. */
static void every_legend_is_where_the_key_is_printed(void) {
  static const struct {
    char character;
    int half_row;
    int bit;
    spectrum_shift shift;
  } printed[] = {
      {'z', 0, 1, SPECTRUM_NO_SHIFT},
      {'x', 0, 2, SPECTRUM_NO_SHIFT},
      {'c', 0, 3, SPECTRUM_NO_SHIFT},
      {'v', 0, 4, SPECTRUM_NO_SHIFT},
      {'a', 1, 0, SPECTRUM_NO_SHIFT},
      {'s', 1, 1, SPECTRUM_NO_SHIFT},
      {'d', 1, 2, SPECTRUM_NO_SHIFT},
      {'f', 1, 3, SPECTRUM_NO_SHIFT},
      {'g', 1, 4, SPECTRUM_NO_SHIFT},
      {'q', 2, 0, SPECTRUM_NO_SHIFT},
      {'w', 2, 1, SPECTRUM_NO_SHIFT},
      {'e', 2, 2, SPECTRUM_NO_SHIFT},
      {'r', 2, 3, SPECTRUM_NO_SHIFT},
      {'t', 2, 4, SPECTRUM_NO_SHIFT},
      {'1', 3, 0, SPECTRUM_NO_SHIFT},
      {'2', 3, 1, SPECTRUM_NO_SHIFT},
      {'3', 3, 2, SPECTRUM_NO_SHIFT},
      {'4', 3, 3, SPECTRUM_NO_SHIFT},
      {'5', 3, 4, SPECTRUM_NO_SHIFT},
      {'0', 4, 0, SPECTRUM_NO_SHIFT},
      {'9', 4, 1, SPECTRUM_NO_SHIFT},
      {'8', 4, 2, SPECTRUM_NO_SHIFT},
      {'7', 4, 3, SPECTRUM_NO_SHIFT},
      {'6', 4, 4, SPECTRUM_NO_SHIFT},
      {'p', 5, 0, SPECTRUM_NO_SHIFT},
      {'o', 5, 1, SPECTRUM_NO_SHIFT},
      {'i', 5, 2, SPECTRUM_NO_SHIFT},
      {'u', 5, 3, SPECTRUM_NO_SHIFT},
      {'y', 5, 4, SPECTRUM_NO_SHIFT},
      {'l', 6, 1, SPECTRUM_NO_SHIFT},
      {'k', 6, 2, SPECTRUM_NO_SHIFT},
      {'j', 6, 3, SPECTRUM_NO_SHIFT},
      {'h', 6, 4, SPECTRUM_NO_SHIFT},
      {' ', 7, 0, SPECTRUM_NO_SHIFT},
      {'m', 7, 2, SPECTRUM_NO_SHIFT},
      {'n', 7, 3, SPECTRUM_NO_SHIFT},
      {'b', 7, 4, SPECTRUM_NO_SHIFT},
      /* The red legends, the ones ASCII can hold. */
      {':', 0, 1, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'?', 0, 3, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'/', 0, 4, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'<', 2, 3, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'>', 2, 4, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'!', 3, 0, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'@', 3, 1, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'#', 3, 2, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'$', 3, 3, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'%', 3, 4, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'_', 4, 0, SPECTRUM_WITH_SYMBOL_SHIFT},
      {')', 4, 1, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'(', 4, 2, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'\'', 4, 3, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'&', 4, 4, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'"', 5, 0, SPECTRUM_WITH_SYMBOL_SHIFT},
      {';', 5, 1, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'=', 6, 1, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'+', 6, 2, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'-', 6, 3, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'^', 6, 4, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'.', 7, 2, SPECTRUM_WITH_SYMBOL_SHIFT},
      {',', 7, 3, SPECTRUM_WITH_SYMBOL_SHIFT},
      {'*', 7, 4, SPECTRUM_WITH_SYMBOL_SHIFT},
  };
  for (size_t index = 0; index < sizeof printed / sizeof printed[0]; index++) {
    spectrum_shift shift = SPECTRUM_WITH_CAPS_SHIFT;
    keyboard_key key = spectrum_key_for_character(printed[index].character, &shift);
    keyboard_key want = SPECTRUM_KEY(printed[index].half_row, printed[index].bit);
    if (key != want || shift != printed[index].shift) {
      TEST_FAIL("'%c' is at half-row %d bit %d shift %d, printed on %d/%d shift %d",
                printed[index].character, key / 8, key % 8, shift, printed[index].half_row,
                printed[index].bit, printed[index].shift);
    }
  }
}

/* Somewhere with room around it, for the programs whose timing is under
   test rather than their result. */
#define PROGRAM_ADDRESS 0x0100
/* Longer than any instruction and every charge a line of the picture can put
   on it, so a program that never finishes is caught rather than hung on. */
#define MAX_TSTATES 400

/* T-states from the start of one opcode fetch to the start of the next.
   That is what the published patterns measure: a hold an instruction earns
   at its last T-state falls before the next fetch, not inside itself. */
static int tstates_at(uint16_t address, uint32_t frame_tick) {
  spectrum.cpu.pc = address;
  ula_seek(&spectrum.ula, frame_tick);
  int tstates = 0;
  do {
    spectrum_tick(&spectrum);
    tstates++;
  } while (!spectrum_instruction_complete(&spectrum) && tstates < MAX_TSTATES);
  return tstates;
}

/* The same, with the program in the ROM, which the ULA never wants — so the
   only charges are the ones its operands earn. */
static int tstates_for(const uint8_t *program, size_t length, uint32_t frame_tick) {
  put_at(PROGRAM_ADDRESS, program, length);
  return tstates_at(PROGRAM_ADDRESS, frame_tick);
}

/* The published pattern for each of these is in the comment beside it, in
   the notation "Contended memory" uses: an address and how many T-states are
   spent with it on the bus. A charge falls at the head of each, and only
   where the address is one the ULA wants. */
static void an_uncontended_instruction_takes_its_book_time(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t nop[] = {0x00};
  TEST_EQUAL(tstates_for(nop, sizeof nop, 0), 4);     /* pc:4 */
  TEST_EQUAL(tstates_for(nop, sizeof nop, 14336), 4); /* even in the slot */
}

/* Every row of the published table opens with pc:4, and for a program in the
   screen's own bank that is where most of what it pays comes from. */
static void an_opcode_fetch_is_charged_where_the_program_lies(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint16_t in_the_screen = SPECTRUM_RAM_BASE;
  ram[in_the_screen - SPECTRUM_RAM_BASE] = 0x00; /* NOP — pc:4 and nothing more */
  TEST_EQUAL(tstates_at(in_the_screen, 14336), 4 + 6);
  TEST_EQUAL(tstates_at(in_the_screen, 14337), 4 + 5);
  TEST_EQUAL(tstates_at(in_the_screen, 14342), 4);
  TEST_EQUAL(tstates_at(in_the_screen, 14343), 4);
  TEST_EQUAL(tstates_at(in_the_screen, 14344), 4 + 6);
  TEST_EQUAL(tstates_at(in_the_screen, 0), 4);
}

static void a_read_of_the_screens_memory_waits_for_the_slot(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t load[] = {0x7E}; /* LD A,(HL) — pc:4,hl:3 */
  spectrum.cpu.h = 0x40;
  spectrum.cpu.l = 0x00;

  /* The fetch is four T-states, so the read falls on 14336 when the
     instruction starts on 14332, and the chip owes six there. */
  TEST_EQUAL(tstates_for(load, sizeof load, 14332), 7 + 6);
  TEST_EQUAL(tstates_for(load, sizeof load, 14333), 7 + 5);
  TEST_EQUAL(tstates_for(load, sizeof load, 14338), 7); /* the two it owes nothing */
  TEST_EQUAL(tstates_for(load, sizeof load, 14339), 7);
  TEST_EQUAL(tstates_for(load, sizeof load, 14340), 7 + 6); /* and the slot begins again */
  TEST_EQUAL(tstates_for(load, sizeof load, 0), 7);         /* off the picture, nothing */
}

static void a_read_of_memory_the_ula_does_not_want_is_never_charged(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t load[] = {0x7E}; /* LD A,(HL) with HL above the ULA's reach */
  spectrum.cpu.h = 0x80;
  spectrum.cpu.l = 0x00;
  for (uint32_t at = 14332; at < 14332 + 8; at++) {
    TEST_EQUAL(tstates_for(load, sizeof load, at), 7);
  }
}

/* The 16K and 48K ULAs charge for the bus whether or not a request is on it,
   so an instruction that holds an address between two accesses pays for the
   T-state in between as well. */
static void an_internal_tstate_is_charged_like_an_access(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t increment[] = {0x34}; /* INC (HL) — pc:4,hl:3,hl:1,hl(write):3 */
  spectrum.cpu.h = 0x40;
  spectrum.cpu.l = 0x00;
  /* Eleven T-states of book time; the read falls on 14336 and is charged
     six, the internal T-state on 14345 and is charged five, and the write on
     14351, where the chip owes nothing. */
  TEST_EQUAL(tstates_for(increment, sizeof increment, 14332), 11 + 6 + 5);
}

static void a_push_is_charged_for_both_halves_of_the_address(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t push[] = {0xC5}; /* PUSH BC — pc:4,ir:1,sp-1:3,sp-2:3 */
  spectrum.cpu.sp = 0x8000;      /* so the two writes land on 0x7FFF and 0x7FFE */
  /* The refresh address is in the ROM and costs nothing; the writes fall on
     14337 and 14345, and the chip owes five at each. */
  TEST_EQUAL(tstates_for(push, sizeof push, 14332), 11 + 5 + 5);
}

/* A machine is not settled the moment the processor is between instructions:
   an instruction whose last T-state is itself charged leaves the clock
   stopped past its end, and a snapshot taken there would lose the rest. */
static void a_hold_the_last_tstate_earned_outlives_the_instruction(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  const uint8_t block[] = {0xED, 0xA0}; /* LDI — pc:4,pc+1:4,hl:3,de:3,de:1 x2 */
  spectrum.cpu.h = 0x80;                /* reading from outside the screen */
  spectrum.cpu.d = 0x40;                /* and writing into it */
  put_at(PROGRAM_ADDRESS, block, sizeof block);
  spectrum.cpu.pc = PROGRAM_ADDRESS;
  ula_seek(&spectrum.ula, 14321);
  /* The processor's half of it alone, deliberately: this is the one test that
     has to stand between the two, where the processor has finished and the
     machine has not. Anywhere else spectrum_instruction_complete is the
     question worth asking. */
  do {
    spectrum_tick(&spectrum);
  } while (!z80_instruction_complete(&spectrum.cpu));
  /* The last of the two internal T-states landed on 14336, and the chip owes
     six there — after the instruction the processor has finished. */
  TEST_EQUAL(spectrum.held_ticks, 6);
  spectrum_finish_instruction(&spectrum);
  TEST_EQUAL(spectrum.held_ticks, 0);
  TEST_EQUAL(spectrum.ula.frame_tick, 14321 + 22);
}

/* A port is charged by two rules at once, in the four combinations below. */
static void a_port_is_charged_by_its_low_bit_and_its_high_byte(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  /* Reading into A rather than B, so that the port in BC survives the
     instruction that used it. */
  const uint8_t in[] = {0xED, 0x78}; /* IN A,(C) — pc:4,pc+1:4,I/O */

  /* Three starts, at which the access itself falls on the head of a slot,
     one T-state into it, and on the last of the two the chip leaves alone. */
  spectrum.cpu.b = 0x00; /* not contended memory, the ULA's port: N:1, C:3 */
  spectrum.cpu.c = 0xFE;
  TEST_EQUAL(tstates_for(in, sizeof in, 14332), 12 + 1);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12 + 6);
  TEST_EQUAL(tstates_for(in, sizeof in, 14336), 12 + 5);

  spectrum.cpu.b = 0x40; /* contended memory and the ULA's port: C:1, C:3 */
  spectrum.cpu.c = 0xFE;
  TEST_EQUAL(tstates_for(in, sizeof in, 14332), 12 + 2);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12 + 6);
  TEST_EQUAL(tstates_for(in, sizeof in, 14336), 12 + 6);

  spectrum.cpu.b = 0x40; /* contended memory, not the ULA's port: C:1 four times */
  spectrum.cpu.c = 0xFF;
  TEST_EQUAL(tstates_for(in, sizeof in, 14332), 12 + 8);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12 + 12);
  TEST_EQUAL(tstates_for(in, sizeof in, 14336), 12 + 12);

  spectrum.cpu.b = 0x00; /* neither: N:4, and nothing is owed anywhere */
  spectrum.cpu.c = 0xFF;
  TEST_EQUAL(tstates_for(in, sizeof in, 14332), 12);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12);
  TEST_EQUAL(tstates_for(in, sizeof in, 14336), 12);
}

/* The first slots' bytes, each its own, so a read names the fetch it
   caught: the first eight characters' top rows and their attributes. */
static void fill_the_first_slots(void) {
  for (int column = 0; column < 8; column++) {
    ram[ula_display_address(0, column) - SPECTRUM_RAM_BASE] = (uint8_t)(0x10 + column);
    ram[ula_attribute_address(0, column) - SPECTRUM_RAM_BASE] = (uint8_t)(0x40 + column);
  }
}

/* IN A,(&FF) begun at a T-state, with A holding the port's high byte, and
   what it read. */
static uint8_t floating_read(uint8_t high_byte, uint32_t frame_tick) {
  const uint8_t in[] = {0xDB, 0xFF}; /* IN A,(&FF) — pc:4,pc+1:3,I/O */
  spectrum.cpu.a = high_byte;
  tstates_for(in, sizeof in, frame_tick);
  return spectrum.cpu.a;
}

/* "Floating bus" (Sinclair Wiki): the first slot fetches a byte, its
   attribute, the next byte and its attribute on 14338 to 14341 by the
   wiki's count, 14339 to 14342 here, and then leaves the bus idle for four.
   The processor takes the byte on the read's last T-state, ten after an
   uncharged IN A,(n) begins. */
static void an_unattached_port_reads_what_the_ula_is_fetching(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  fill_the_first_slots();
  TEST_EQUAL(floating_read(0x00, 14328), 0xFF);
  TEST_EQUAL(floating_read(0x00, 14329), 0x10); /* the picture's first byte */
  TEST_EQUAL(floating_read(0x00, 14330), 0x40); /* its attribute */
  TEST_EQUAL(floating_read(0x00, 14331), 0x11);
  TEST_EQUAL(floating_read(0x00, 14332), 0x41);
  for (uint32_t at = 14333; at < 14337; at++) {
    TEST_EQUAL(floating_read(0x00, at), 0xFF);
  }
  TEST_EQUAL(floating_read(0x00, 14337), 0x12); /* the next slot */
}

/* A port whose high byte is contended is charged on all four T-states,
   each weighed where it would begin and held before it ("Contended I/O":
   C:1 four times), so the last runs only where the slot leaves the bus to
   the processor: on its seventh T-state, an attribute's fetch, or its
   eighth, idle. Worked out from that rule, an IN begun at 14326 takes the
   first slot's second attribute; at 14328 the second slot's; at 14336 and
   14344 the third's and fourth's; and from 14325 to 14345 nothing else. */
static void a_contended_ports_byte_is_taken_after_its_hold(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  fill_the_first_slots();
  for (uint32_t at = 14325; at <= 14345; at++) {
    uint8_t want = 0xFF;
    if (at == 14326) {
      want = 0x41;
    } else if (at == 14328) {
      want = 0x43;
    } else if (at == 14336) {
      want = 0x45;
    } else if (at == 14344) {
      want = 0x47;
    }
    uint8_t got = floating_read(0x40, at);
    if (got != want) {
      TEST_FAIL("IN A,(&FF) with A=&40 begun at %u read &%02X, expected &%02X", at, got, want);
    }
  }
}

/* A host watching the bus sees a port's byte as it stands on each T-state
   from the request on, and the byte the processor takes on the tick after
   the last that shows the request. A slowed read of the floating bus is
   where the two differ: begun at 14326, the request stands on an idle
   T-state and the byte is taken from the first slot's second attribute. */
static void a_ports_byte_is_on_the_bus_the_tick_it_is_taken(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  fill_the_first_slots();
  const uint8_t in[] = {0xDB, 0xFF}; /* IN A,(&FF) with A=&40 */
  put_at(PROGRAM_ADDRESS, in, sizeof in);
  spectrum.cpu.pc = PROGRAM_ADDRESS;
  spectrum.cpu.a = 0x40;
  ula_seek(&spectrum.ula, 14326);
  int at_the_request = -1;
  int after_it = -1;
  bool requesting = false;
  do {
    uint64_t pins = spectrum_tick(&spectrum);
    bool request = (pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD);
    if (request && !requesting) {
      at_the_request = z80_data(pins);
    } else if (!request && requesting) {
      after_it = z80_data(pins);
    }
    requesting = request;
  } while (!spectrum_instruction_complete(&spectrum));
  TEST_EQUAL(at_the_request, 0xFF);
  TEST_EQUAL(after_it, 0x41);
  TEST_EQUAL(spectrum.cpu.a, 0x41);
}

/* The first sample of a colour on a raster line, or -1. */
static int first_sample_of(int framebuffer_row, uint8_t colour) {
  for (int x = 0; x < SPECTRUM_48K_RASTER_WIDTH; x++) {
    if ((framebuffer[framebuffer_row * SPECTRUM_48K_RASTER_WIDTH + x] & 0x0F) == colour) {
      return x;
    }
  }
  return -1;
}

/* Rak's table has Bobrowski's btime meet the picture's left edge with a
   change of border for an OUT whose machine cycle begins on any T-state from
   14112 to 14115, a line above the picture; taken every four T-states, one
   begun a T-state later lands a character on. OUT (n),A's machine cycle
   begins seven after the instruction. */
static void the_border_changes_on_a_characters_edge(void) {
  const uint8_t out[] = {0xD3, 0xFE}; /* OUT (&FE),A — pc:4,pc+1:3,I/O */
  const int line_above = 64 - 1 - SPECTRUM_PICTURE_SHIFT;
  const int left_edge = (48 + ULA_LEFT_BORDER_TICKS) * ULA_SAMPLES_PER_TICK;
  for (int later = 0; later < 2; later++) {
    /* The beam runs on from power-on, so the monitor has the frame; only
       the processor is started where the OUT is to begin. */
    power_on_48k(SPECTRUM_RAM_48K);
    put_at(PROGRAM_ADDRESS, out, sizeof out);
    run_frames(2);
    while (spectrum.ula.frame_tick != (uint32_t)(14115 - 7 + later)) {
      spectrum_tick(&spectrum);
    }
    restart_the_processor();
    spectrum.cpu.pc = PROGRAM_ADDRESS;
    spectrum.cpu.a = 0x02;
    for (int tick = 0; tick < 300; tick++) {
      spectrum_tick(&spectrum);
    }
    TEST_EQUAL(first_sample_of(line_above, 2), left_edge + later * 8);
  }
}

/* The ULA stops the processor's clock, not the tape's: a held T-state is one
   the deck plays straight through, and a loader that lost T-states to
   contention would measure every edge wrong. */
static void the_tape_runs_on_while_the_clock_is_held(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  static const uint8_t image[] = {0x02, 0x00, 0xFF, 0x55};
  static tape_t tape;
  static tzx_t reader;
  const char *problem = NULL;
  TEST_CHECK(tzx_open(&reader, image, sizeof image, SPECTRUM_48K_CLOCK_HERTZ / 1000, TZX_SPECTRUM,
                      &problem));
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);
  tape_play(&tape);
  spectrum_tick(&spectrum); /* the deck takes up its first pulse */

  const uint32_t left = tape.ticks_left;
  spectrum.held_ticks = 200; /* as a contended access leaves it */
  for (int tick = 0; tick < 100; tick++) {
    spectrum_tick(&spectrum);
  }
  TEST_EQUAL(spectrum.held_ticks, 100);    /* the processor stood still */
  TEST_EQUAL(tape.ticks_left, left - 100); /* and the tape did not */
}

/* And what the deck presents is what bit 6 reads: a tape in the deck drives
   the socket, where `ear` answers for a machine with none. */
static void a_tape_in_the_deck_is_what_bit_6_reads(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  static const uint8_t image[] = {0x02, 0x00, 0xFF, 0x55};
  static tape_t tape;
  static tzx_t reader;
  const char *problem = NULL;
  TEST_CHECK(tzx_open(&reader, image, sizeof image, SPECTRUM_48K_CLOCK_HERTZ / 1000, TZX_SPECTRUM,
                      &problem));
  tape_init(&tape);
  tape_insert(&tape, tzx_next_pulse, &reader);
  spectrum_insert_tape(&spectrum, &tape);
  tape_play(&tape);

  /* The tape starts low, and `ear` is set the other way to prove which of the
     two the socket is hearing. */
  spectrum.ear = true;
  const uint8_t program[] = {0x01, 0xFE, 0xFE, 0xED, 0x78, 0x76}; /* LD BC,&FEFE : IN A,(C) */
  rom_program(program, sizeof program);
  spectrum_tick(&spectrum);
  TEST_CHECK(!tape_level(&tape));
  restart_the_processor();
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x40, 0x00);

  /* Play the tape on to the first edge, past the cassette's leader (tzx.h),
     and read again. The deck plays it alone: the machine would play it
     tick for tick the same, and nothing else it does over the leader is
     graded. */
  uint32_t guard = 0;
  while (!tape_level(&tape) && guard++ < 20 * SPECTRUM_48K_CLOCK_HERTZ) {
    tape_tick(&tape);
  }
  TEST_CHECK(tape_level(&tape));
  spectrum.ear = false;
  restart_the_processor();
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x40, 0x40);
}

/* The +2's first T-state of contention, a late 7K010's (ula.h), and the
   first fetch, three after it. */
#define PLUS2_FIRST_CONTENDED 14363
#define PLUS2_FIRST_FETCH (PLUS2_FIRST_CONTENDED + 3)

/* A program in both of the +2's ROMs, so that one which pages in the other
   runs on in it to the end. A 48K reads the first alone. */
static void program_in_both_roms(const uint8_t *bytes, size_t length) {
  memcpy(rom, bytes, length);
  memcpy(rom + 0x4000, bytes, length);
}

/* LD BC,port : LD A,value : OUT (C),A : HALT */
static void write_port_and_halt(uint16_t port, uint8_t value) {
  const uint8_t program[] = {
      0x01, (uint8_t)port, (uint8_t)(port >> 8), 0x3E, value, 0xED, 0x79, 0x76,
  };
  program_in_both_roms(program, sizeof program);
  restart_the_processor();
  TEST_CHECK(run_to_halt());
}

/* LD BC,port : IN A,(C) : HALT, and what it read. */
static uint8_t read_port_and_halt(uint16_t port) {
  const uint8_t program[] = {0x01, (uint8_t)port, (uint8_t)(port >> 8), 0xED, 0x78, 0x76};
  program_in_both_roms(program, sizeof program);
  restart_the_processor();
  TEST_CHECK(run_to_halt());
  return spectrum.cpu.a;
}

/* "On power up, or after reset the bank register is cleared and loads page
   0 at address C000H, selects the 128K Spectrum ROM at address 0000, and
   informs the ULA that screen accesses are from page 5" (the 128 service
   manual, §4), and banks 5 and 2 stand at 0x4000 and 0x8000. */
static void a_plus2_powers_on_with_its_first_rom_and_banks_5_2_and_0(void) {
  power_on_plus2();
  rom[0x0000] = 0x12;
  rom[0x4000] = 0x34;
  TEST_EQUAL(spectrum.bank_register, 0x00);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x0000), 0x12);
  spectrum_poke(&spectrum, 0x4000, 0x55);
  spectrum_poke(&spectrum, 0x8000, 0x22);
  spectrum_poke(&spectrum, 0xC000, 0x10);
  TEST_EQUAL(bank(5)[0], 0x55);
  TEST_EQUAL(bank(2)[0], 0x22);
  TEST_EQUAL(bank(0)[0], 0x10);
  spectrum_poke(&spectrum, 0x0000, 0x99);
  TEST_EQUAL(rom[0x0000], 0x12);
}

static void the_bank_register_pages_the_top_bank_and_the_rom(void) {
  power_on_plus2();
  for (unsigned number = 0; number < 8; number++) {
    bank(number)[0x0123] = (uint8_t)(0xB0 + number);
  }
  rom[0x4000 + 0x0042] = 0x48;
  write_port_and_halt(0x7FFD, 0x13); /* bank 3, ROM 1 */
  TEST_EQUAL(spectrum.bank_register, 0x13);
  TEST_EQUAL(spectrum_peek(&spectrum, 0xC123), 0xB3);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x4123), 0xB5);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8123), 0xB2);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x0042), 0x48);
  for (unsigned number = 0; number < 8; number++) {
    spectrum.bank_register = (uint8_t)number;
    TEST_EQUAL(spectrum_peek(&spectrum, 0xC123), 0xB0 + number);
  }
}

/* The register is six flip-flops, so a byte's top two bits are lost, and
   the sixth holds the clock off until reset. */
static void the_lock_holds_the_bank_register_until_reset(void) {
  power_on_plus2();
  write_port_and_halt(0x7FFD, 0xE1); /* bank 1, locked, and two bits with nowhere to go */
  TEST_EQUAL(spectrum.bank_register, 0x21);
  write_port_and_halt(0x7FFD, 0x07);
  TEST_EQUAL(spectrum.bank_register, 0x21);
  power_on_plus2();
  TEST_EQUAL(spectrum.bank_register, 0x00);
}

/* BANK is A15 and A1 low and nothing else, so a port with A0 low as well
   pages the machine and sets the border at once, and a port with either
   line high does not page it. */
static void any_port_with_a15_and_a1_low_reaches_the_bank_register(void) {
  power_on_plus2();
  write_port_and_halt(0x7FFC, 0x12);
  TEST_EQUAL(spectrum.bank_register, 0x12);
  TEST_EQUAL(spectrum.ula.border, 0x02);

  power_on_plus2();
  write_port_and_halt(0x0001, 0x03);
  TEST_EQUAL(spectrum.bank_register, 0x03);

  power_on_plus2();
  write_port_and_halt(0x7FFF, 0x03); /* A1 high */
  TEST_EQUAL(spectrum.bank_register, 0x00);
  write_port_and_halt(0xFFFD, 0x03); /* A15 high: the sound chip's */
  TEST_EQUAL(spectrum.bank_register, 0x00);
}

/* A read is decoded as a write is, and the register latches what the
   processor takes. Away from the screen's fetches that is the floating
   bus's 0xFF, which pages in bank 7, the second screen and the 48K ROM, and
   locks them there. */
static void reading_the_bank_register_pages_with_the_byte_read(void) {
  power_on_plus2();
  TEST_EQUAL(read_port_and_halt(0x7FFD), 0xFF);
  TEST_EQUAL(spectrum.bank_register, 0x3F);

  /* On an even port the ULA answers with the keyboard, and that is the byte
     latched: SPACE is bit 0 of the half-row A15 selects. */
  power_on_plus2();
  keyboard_press(&spectrum.keyboard, SPECTRUM_SPACE);
  TEST_EQUAL(read_port_and_halt(0x7FFC), 0xBE);
  TEST_EQUAL(spectrum.bank_register, 0x3E);
}

/* A 48K has no bank register and no sound chip: the +2's ports reach
   nothing on it. */
static void a_48k_has_no_bank_register_and_no_sound_chip(void) {
  power_on_48k(SPECTRUM_RAM_48K);
  write_port_and_halt(0x7FFD, 0x17);
  TEST_EQUAL(spectrum.bank_register, 0x00);
  write_port_and_halt(0xFFFD, 0x07);
  write_port_and_halt(0xBFFD, 0x3F);
  TEST_EQUAL(spectrum.psg.registers[7], 0x00);
  TEST_EQUAL(read_port_and_halt(0xFFFD), 0xFF);
}

/* OUT (C),A with the port 0x7FFD and A=8, the second screen, whose I/O
   cycle begins at a T-state; and how many of the first line's bitmap bytes
   the ULA still fetches from bank 5's screen. The instruction can begin
   after the line's first fetches, so it is the second screen's bytes that
   are counted, and the rest are the first's. */
static int bytes_fetched_from_the_first_screen(uint32_t io_cycle_begins) {
  power_on_plus2();
  for (int column = 0; column < ULA_COLUMNS; column++) {
    uint16_t at = (uint16_t)(ula_display_address(0, column) - SPECTRUM_RAM_BASE);
    bank(5)[at] = 0x55;
    bank(7)[at] = 0x77;
  }
  const uint8_t out[] = {0xED, 0x79, 0x76}; /* OUT (C),A — pc:4,pc+1:4,I/O : HALT */
  put_at(PROGRAM_ADDRESS, out, sizeof out);
  spectrum.cpu.pc = PROGRAM_ADDRESS;
  spectrum.cpu.b = 0x7F;
  spectrum.cpu.c = 0xFD;
  spectrum.cpu.a = SPECTRUM_BANK_SCREEN;
  ula_seek(&spectrum.ula, io_cycle_begins - 8);
  int second_screen = 0;
  while (spectrum.ula.frame_tick < PLUS2_FIRST_FETCH + ULA_DISPLAY_TICKS) {
    uint32_t at = spectrum.ula.frame_tick;
    spectrum_tick(&spectrum);
    uint32_t since = at - PLUS2_FIRST_FETCH;
    bool bitmap = at >= PLUS2_FIRST_FETCH && since % 8 < 4 && since % 2 == 0;
    if (bitmap && spectrum.ula_bus == 0x77) {
      second_screen++;
    }
  }
  return ULA_COLUMNS - second_screen;
}

/* Alford's run of ptime on a toastrack and a grey +2: the bar of the first
   screen is none of the line for an OUT whose cycle begins at 14358, two
   bytes from 14359 to 14360, four from 14361 to 14368, six from 14369 to
   14376 and eight at 14377 — an early machine's, a T-state later on a late
   one. That the OUT's cycle begins where ptime says is Rak's: he made the
   test so "it always starts the OUT cycle exactly at the advertised T cycle
   even on 128k, despite the contention" (World of Spectrum forum, 2014,
   https://worldofspectrum.org/forums/discussion/comment/757618/#Comment_757618).
   Port 0x7FFD is charged on all four of its T-states, Rak's "C:1x4"
   (spectrum.h). A register that latches on the last of them, after its
   hold, gives this staircase; one that latched a T-state sooner, as the
   processor puts the byte out, gives a step a T-state early at four of its
   eight rows. */
static void the_screen_switches_where_ptime_finds_it(void) {
  const struct {
    uint32_t begins;
    int bytes;
  } rows[] = {
      {14358 + 1, 0}, {14359 + 1, 2}, {14360 + 1, 2}, {14361 + 1, 4},
      {14368 + 1, 4}, {14369 + 1, 6}, {14376 + 1, 6}, {14377 + 1, 8},
  };
  for (size_t row = 0; row < sizeof rows / sizeof rows[0]; row++) {
    int bytes = bytes_fetched_from_the_first_screen(rows[row].begins);
    if (bytes != rows[row].bytes) {
      TEST_FAIL("an OUT begun at %u left %d bytes of the first screen, expected %d",
                rows[row].begins, bytes, rows[row].bytes);
    }
  }
}

/* The ULA wants 0x4000 to 0x7FFF on a +2 as on a 48K, and 0xC000 to 0xFFFF
   whenever an odd bank is paged there: Rak's measurement on a 128, whose
   paging logic the +2 carries (spectrum.h). */
static void an_odd_bank_at_c000_is_contended(void) {
  power_on_plus2();
  const uint8_t load[] = {0x7E}; /* LD A,(HL) — pc:4,hl:3 */
  const uint32_t read_on_the_first = PLUS2_FIRST_CONTENDED - 4;
  for (unsigned number = 0; number < 8; number++) {
    spectrum.bank_register = (uint8_t)number;
    spectrum.cpu.h = 0xC0;
    spectrum.cpu.l = 0x00;
    TEST_EQUAL(tstates_for(load, sizeof load, read_on_the_first), 7 + (number % 2 ? 6 : 0));
    spectrum.cpu.h = 0x40;
    TEST_EQUAL(tstates_for(load, sizeof load, read_on_the_first), 7 + 6);
    spectrum.cpu.h = 0x80;
    TEST_EQUAL(tstates_for(load, sizeof load, read_on_the_first), 7);
  }
}

/* And a port in that range is contended with it, as one at 0x4000 to
   0x7FFF is, whether or not the lock is set: Rak's "contending also ports
   in #C000-#FFFF range in case a contended page is paged in", and
   "Disabling of paging doesn't disable the contention of the high range",
   on a 128. These are the 48K's four-times-charged rows, 27 T-states
   later. */
static void a_port_in_an_odd_bank_is_contended_even_when_locked(void) {
  power_on_plus2();
  const uint8_t in[] = {0xED, 0x78}; /* IN A,(C) — pc:4,pc+1:4,I/O */
  const uint8_t settings[] = {0x01, 0x21};
  for (size_t index = 0; index < sizeof settings; index++) {
    spectrum.bank_register = settings[index];
    spectrum.cpu.b = 0xC0; /* in the odd bank, not the ULA's port and not the sound chip's */
    spectrum.cpu.c = 0xFF;
    TEST_EQUAL(tstates_for(in, sizeof in, 14332 + 27), 12 + 8);
    TEST_EQUAL(tstates_for(in, sizeof in, 14335 + 27), 12 + 12);
    TEST_EQUAL(tstates_for(in, sizeof in, 14336 + 27), 12 + 12);
  }
  spectrum.bank_register = 0x00;
  TEST_EQUAL(tstates_for(in, sizeof in, 14336 + 27), 12);
}

/* The ULA draws from bank 5, or from bank 7 while bit 3 is set, whatever
   the processor has paged; a read of the floating bus is where its fetch
   shows. An uncharged IN A,(n) takes its byte ten T-states in. */
static void the_ula_fetches_from_the_screen_bit_3_chooses(void) {
  power_on_plus2();
  uint16_t first = (uint16_t)(ula_display_address(0, 0) - SPECTRUM_RAM_BASE);
  bank(5)[first] = 0x55;
  bank(7)[first] = 0x77;
  TEST_EQUAL(floating_read(0x00, PLUS2_FIRST_FETCH - 10), 0x55);
  spectrum.bank_register = SPECTRUM_BANK_SCREEN;
  TEST_EQUAL(floating_read(0x00, PLUS2_FIRST_FETCH - 10), 0x77);
  spectrum.bank_register = 0x07; /* paging bank 7 in moves nothing the ULA sees */
  TEST_EQUAL(floating_read(0x00, PLUS2_FIRST_FETCH - 10), 0x55);
}

/* 0xFFFD selects a register on a write and reads it on a read; 0xBFFD
   writes it, and reads nothing, so the floating bus answers (the 128
   service manual, §5.6.3). */
static void the_sound_chip_answers_at_fffd_and_bffd(void) {
  power_on_plus2();
  write_port_and_halt(0xFFFD, 0x08); /* channel A's amplitude */
  write_port_and_halt(0xBFFD, 0x0F);
  TEST_EQUAL(spectrum.psg.registers[8], 0x0F);
  TEST_EQUAL(read_port_and_halt(0xFFFD), 0x0F);
  TEST_EQUAL(read_port_and_halt(0xBFFD), 0xFF);
  /* Only A15, A14 and A1 are decoded. */
  write_port_and_halt(0xC001, 0x01);
  write_port_and_halt(0x8001, 0x0A);
  TEST_EQUAL(spectrum.psg.registers[1], 0x0A);
  TEST_EQUAL(spectrum.bank_register, 0x00);
}

/* Where the ULA and the sound chip both answer, the processor is taken to
   read the chip, which sits on its side of the resistors in Worsley's
   redraw; nothing found measures it. */
static void the_sound_chip_outshouts_the_ula_on_an_even_port(void) {
  power_on_plus2();
  write_port_and_halt(0xFFFD, 0x00);
  write_port_and_halt(0xBFFD, 0x5A);
  TEST_EQUAL(read_port_and_halt(0xFFFC), 0x5A);
}

/* With nothing in the keypad or RS232 socket, port A is taken to read
   high. */
static void port_a_reads_high_with_nothing_plugged_in(void) {
  power_on_plus2();
  write_port_and_halt(0xFFFD, AY38910_PORT_A);
  TEST_EQUAL(read_port_and_halt(0xFFFD), 0xFF);
}

/* The chip is clocked at half the processor's rate, and, as spectrum.c
   takes it, whether the processor's clock is held or not. */
static void the_sound_chip_runs_at_half_the_clock(void) {
  power_on_plus2();
  uint8_t before = spectrum.psg.divider;
  spectrum.held_ticks = 200;
  for (int tick = 0; tick < 2 * 8 * 10 + 2; tick++) {
    spectrum_tick(&spectrum);
  }
  TEST_EQUAL(spectrum.psg.divider, (before + 1) % 8);
}

/* There is no loudspeaker on a +2. */
static void a_plus2_has_no_loudspeaker(void) {
  power_on_plus2();
  write_port_and_halt(0x00FE, 0x18);
  TEST_CHECK(spectrum.ula.speaker);
  TEST_EQUAL(spectrum_sound_speaker(&spectrum), 0);
}

int main(void) {
  TEST_RUN(reset_fetches_from_the_rom);
  TEST_RUN(ram_answers_from_4000_and_the_rom_refuses_writes);
  TEST_RUN(a_16k_machine_does_not_answer_above_7fff);
  TEST_RUN(every_even_port_reaches_the_ula);
  TEST_RUN(a_pressed_key_reads_low_on_its_own_half_row);
  TEST_RUN(a_half_row_that_is_not_selected_reads_nothing);
  TEST_RUN(selected_half_rows_read_together);
  TEST_RUN(the_ula_interrupts_once_a_frame);
  TEST_RUN(the_ear_socket_reads_on_bit_6);
  TEST_RUN(bit_4_sounds_the_speaker_and_bit_3_alone_does_not);
  TEST_RUN(an_odd_port_reaches_nothing_and_floats);
  TEST_RUN(the_first_interrupt_is_accepted_sixteen_tstates_in);
  TEST_RUN(the_keyboard_finds_a_character_where_it_is_printed);
  TEST_RUN(every_legend_is_where_the_key_is_printed);
  TEST_RUN(the_raster_is_the_frame_the_ula_draws);
  TEST_RUN(the_border_reaches_the_framebuffer);
  TEST_RUN(the_picture_lands_where_the_frame_sync_leaves_it);
  TEST_RUN(an_uncontended_instruction_takes_its_book_time);
  TEST_RUN(an_opcode_fetch_is_charged_where_the_program_lies);
  TEST_RUN(a_read_of_the_screens_memory_waits_for_the_slot);
  TEST_RUN(a_read_of_memory_the_ula_does_not_want_is_never_charged);
  TEST_RUN(an_internal_tstate_is_charged_like_an_access);
  TEST_RUN(a_push_is_charged_for_both_halves_of_the_address);
  TEST_RUN(a_hold_the_last_tstate_earned_outlives_the_instruction);
  TEST_RUN(a_port_is_charged_by_its_low_bit_and_its_high_byte);
  TEST_RUN(an_unattached_port_reads_what_the_ula_is_fetching);
  TEST_RUN(a_contended_ports_byte_is_taken_after_its_hold);
  TEST_RUN(a_ports_byte_is_on_the_bus_the_tick_it_is_taken);
  TEST_RUN(the_border_changes_on_a_characters_edge);
  TEST_RUN(the_tape_runs_on_while_the_clock_is_held);
  TEST_RUN(a_tape_in_the_deck_is_what_bit_6_reads);
  TEST_RUN(a_plus2_powers_on_with_its_first_rom_and_banks_5_2_and_0);
  TEST_RUN(the_bank_register_pages_the_top_bank_and_the_rom);
  TEST_RUN(the_lock_holds_the_bank_register_until_reset);
  TEST_RUN(any_port_with_a15_and_a1_low_reaches_the_bank_register);
  TEST_RUN(reading_the_bank_register_pages_with_the_byte_read);
  TEST_RUN(a_48k_has_no_bank_register_and_no_sound_chip);
  TEST_RUN(the_screen_switches_where_ptime_finds_it);
  TEST_RUN(an_odd_bank_at_c000_is_contended);
  TEST_RUN(a_port_in_an_odd_bank_is_contended_even_when_locked);
  TEST_RUN(the_ula_fetches_from_the_screen_bit_3_chooses);
  TEST_RUN(the_sound_chip_answers_at_fffd_and_bffd);
  TEST_RUN(the_sound_chip_outshouts_the_ula_on_an_even_port);
  TEST_RUN(port_a_reads_high_with_nothing_plugged_in);
  TEST_RUN(the_sound_chip_runs_at_half_the_clock);
  TEST_RUN(a_plus2_has_no_loudspeaker);
  return TEST_REPORT("spectrum");
}
