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
 * neither worked out from anything here.
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

static spectrum_t spectrum;
static uint8_t rom[SPECTRUM_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_48K];
static uint8_t framebuffer[SPECTRUM_FRAMEBUFFER_WIDTH * SPECTRUM_FRAMEBUFFER_HEIGHT];

static void power_on(uint32_t ram_size) {
  memset(rom, 0x00, sizeof rom);
  memset(ram, 0x00, sizeof ram);
  memset(framebuffer, 0xFF, sizeof framebuffer);
  spectrum_init(&spectrum, ram, ram_size, rom);
  spectrum_connect_monitor(&spectrum, framebuffer);
}

static void rom_program(const uint8_t *bytes, size_t length) { memcpy(rom, bytes, length); }

static void put_at(uint16_t address, const uint8_t *bytes, size_t length) {
  memcpy(rom + address, bytes, length);
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
    for (int tick = 0; tick < SPECTRUM_TICKS_PER_FRAME; tick++) {
      spectrum_tick(&spectrum);
    }
  }
}

static void reset_fetches_from_the_rom(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0x3E, 0x42, 0x76}; /* LD A,&42 : HALT */
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a, 0x42);
}

static void ram_answers_from_4000_and_the_rom_refuses_writes(void) {
  power_on(SPECTRUM_RAM_48K);
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
  power_on(SPECTRUM_RAM_16K);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x7FFF), 0x00);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8000), 0xFF);
  spectrum_poke(&spectrum, 0x8000, 0x5A);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8000), 0xFF);

  power_on(SPECTRUM_RAM_48K);
  spectrum_poke(&spectrum, 0x8000, 0x5A);
  TEST_EQUAL(spectrum_peek(&spectrum, 0x8000), 0x5A);
  TEST_EQUAL(spectrum_peek(&spectrum, 0xFFFF), 0x00);
}

static void every_even_port_reaches_the_ula(void) {
  power_on(SPECTRUM_RAM_48K);
  /* LD A,3 : OUT (&FE),A : LD A,5 : OUT (&FF),A : HALT — the odd port
     should leave the border where the even one put it. */
  const uint8_t program[] = {0x3E, 0x03, 0xD3, 0xFE, 0x3E, 0x05, 0xD3, 0xFF, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.ula.border, 3);

  /* And an even port that is not &FE reaches it just the same. */
  power_on(SPECTRUM_RAM_48K);
  const uint8_t other[] = {0x3E, 0x06, 0xD3, 0x40, 0x76}; /* OUT (&40),A */
  rom_program(other, sizeof other);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.ula.border, 6);
}

static void a_pressed_key_reads_low_on_its_own_half_row(void) {
  power_on(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_ENTER); /* half-row 6, bit 0 */
  /* LD BC,&BFFE : IN A,(C) : HALT — A14 low selects half-row 6. */
  const uint8_t program[] = {0x01, 0xFE, 0xBF, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x1F, 0x1E); /* bit 0 pulled down */
  TEST_EQUAL(spectrum.cpu.a & 0xA0, 0xA0); /* bits 5 and 7 read high */
}

static void a_half_row_that_is_not_selected_reads_nothing(void) {
  power_on(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_ENTER);
  /* LD BC,&FEFE : IN A,(C) : HALT — A8 low selects half-row 0, not 6. */
  const uint8_t program[] = {0x01, 0xFE, 0xFE, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x1F, 0x1F);
}

static void selected_half_rows_read_together(void) {
  power_on(SPECTRUM_RAM_48K);
  keyboard_press(&spectrum.keyboard, SPECTRUM_CAPS_SHIFT);   /* half-row 0, bit 0 */
  keyboard_press(&spectrum.keyboard, SPECTRUM_SYMBOL_SHIFT); /* half-row 7, bit 1 */
  /* LD BC,&7EFE : IN A,(C) : HALT — A8 and A15 both low. */
  const uint8_t program[] = {0x01, 0xFE, 0x7E, 0xED, 0x78, 0x76};
  rom_program(program, sizeof program);
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x1F, 0x1C); /* both bits down at once */
}

static void the_ula_interrupts_once_a_frame(void) {
  power_on(SPECTRUM_RAM_48K);
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
  power_on(SPECTRUM_RAM_48K);
  rom_program(program, sizeof program);
  spectrum.ear = false;
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x40, 0x00);

  power_on(SPECTRUM_RAM_48K);
  rom_program(program, sizeof program);
  spectrum.ear = true;
  TEST_CHECK(run_to_halt());
  TEST_EQUAL(spectrum.cpu.a & 0x40, 0x40);
}

static void an_odd_port_reaches_nothing_and_floats(void) {
  power_on(SPECTRUM_RAM_48K);
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
   the acknowledge cycle puts IORQ on the bus on its second T-state.

   Later frames cannot be pinned this way — the handler's own length is not
   a multiple of the halted processor's four-T-state cycle, so acceptance
   walks a few T-states each frame, and the acknowledge cycle falls outside
   the window entirely whenever the processor latches the line on its last
   T-state. That the interrupt is nonetheless taken every frame is what
   the_ula_interrupts_once_a_frame checks. */
static void the_first_interrupt_is_accepted_sixteen_tstates_in(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0xED, 0x56, 0xFB, 0x76, 0x18, 0xFD}; /* IM 1 : EI : HALT : JR */
  rom_program(program, sizeof program);
  const uint8_t handler[] = {0xFB, 0xED, 0x4D}; /* EI : RETI */
  put_at(0x0038, handler, sizeof handler);

  int acknowledged_at = -1;
  for (int tick = 0; tick < SPECTRUM_TICKS_PER_FRAME; tick++) {
    uint32_t at = spectrum.ula.frame_tick;
    uint64_t pins = spectrum_tick(&spectrum);
    if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
      acknowledged_at = (int)at;
      break;
    }
  }
  TEST_EQUAL(acknowledged_at, 17);
}

static void the_border_reaches_the_framebuffer(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0x3E, 0x02, 0xD3, 0xFE, 0x76}; /* border 2, then wait */
  rom_program(program, sizeof program);
  run_frames(2);

  /* A line below the picture is border from end to end. Its samples sit
     SPECTRUM_PICTURE_SHIFT lines above where the beam drew them. */
  int line = ULA_FIRST_DISPLAY_LINE + ULA_DISPLAY_LINES + 4 - SPECTRUM_PICTURE_SHIFT;
  TEST_EQUAL(framebuffer[line * SPECTRUM_FRAMEBUFFER_WIDTH + 200], 2);
  TEST_EQUAL(framebuffer[line * SPECTRUM_FRAMEBUFFER_WIDTH + 430], 2);
}

static void the_picture_lands_where_the_frame_sync_leaves_it(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t program[] = {0x3E, 0x02, 0xD3, 0xFE, 0x76};
  rom_program(program, sizeof program);
  /* Paper 5 across the whole screen, so the picture differs from border 2. */
  memset(ram, 0x00, 6144);
  memset(ram + 6144, 0x28, 768);
  run_frames(2);

  /* Walk down the middle of the raster and find where the picture starts. */
  int top = -1;
  for (int line = 0; line < SPECTRUM_FRAMEBUFFER_HEIGHT; line++) {
    if (framebuffer[line * SPECTRUM_FRAMEBUFFER_WIDTH + 200] == 5) {
      top = line;
      break;
    }
  }
  TEST_EQUAL(top, ULA_FIRST_DISPLAY_LINE - SPECTRUM_PICTURE_SHIFT);
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
  } while ((!z80_instruction_complete(&spectrum.cpu) || spectrum.held_ticks > 0) &&
           tstates < MAX_TSTATES);
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
  power_on(SPECTRUM_RAM_48K);
  const uint8_t nop[] = {0x00};
  TEST_EQUAL(tstates_for(nop, sizeof nop, 0), 4);     /* pc:4 */
  TEST_EQUAL(tstates_for(nop, sizeof nop, 14335), 4); /* even in the slot */
}

/* Every row of the published table opens with pc:4, and for a program in the
   screen's own bank that is where most of what it pays comes from. */
static void an_opcode_fetch_is_charged_where_the_program_lies(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint16_t in_the_screen = SPECTRUM_RAM_BASE;
  ram[in_the_screen - SPECTRUM_RAM_BASE] = 0x00; /* NOP — pc:4 and nothing more */
  TEST_EQUAL(tstates_at(in_the_screen, 14335), 4 + 6);
  TEST_EQUAL(tstates_at(in_the_screen, 14336), 4 + 5);
  TEST_EQUAL(tstates_at(in_the_screen, 14341), 4);
  TEST_EQUAL(tstates_at(in_the_screen, 14342), 4);
  TEST_EQUAL(tstates_at(in_the_screen, 14343), 4 + 6);
  TEST_EQUAL(tstates_at(in_the_screen, 0), 4);
}

static void a_read_of_the_screens_memory_waits_for_the_slot(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t load[] = {0x7E}; /* LD A,(HL) — pc:4,hl:3 */
  spectrum.cpu.h = 0x40;
  spectrum.cpu.l = 0x00;

  /* The fetch is four T-states, so the read falls on 14335 when the
     instruction starts on 14331, and the chip owes six there. */
  TEST_EQUAL(tstates_for(load, sizeof load, 14331), 7 + 6);
  TEST_EQUAL(tstates_for(load, sizeof load, 14332), 7 + 5);
  TEST_EQUAL(tstates_for(load, sizeof load, 14337), 7); /* the two it owes nothing */
  TEST_EQUAL(tstates_for(load, sizeof load, 14338), 7);
  TEST_EQUAL(tstates_for(load, sizeof load, 14339), 7 + 6); /* and the slot begins again */
  TEST_EQUAL(tstates_for(load, sizeof load, 0), 7);         /* off the picture, nothing */
}

static void a_read_of_memory_the_ula_does_not_want_is_never_charged(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t load[] = {0x7E}; /* LD A,(HL) with HL above the ULA's reach */
  spectrum.cpu.h = 0x80;
  spectrum.cpu.l = 0x00;
  for (uint32_t at = 14331; at < 14331 + 8; at++) {
    TEST_EQUAL(tstates_for(load, sizeof load, at), 7);
  }
}

/* The 16K and 48K ULAs charge for the bus whether or not a request is on it,
   so an instruction that holds an address between two accesses pays for the
   T-state in between as well. */
static void an_internal_tstate_is_charged_like_an_access(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t increment[] = {0x34}; /* INC (HL) — pc:4,hl:3,hl:1,hl(write):3 */
  spectrum.cpu.h = 0x40;
  spectrum.cpu.l = 0x00;
  /* Eleven T-states of book time; the read falls on 14335 and is charged
     six, the internal T-state on 14344 and is charged five, and the write on
     14350, where the chip owes nothing. */
  TEST_EQUAL(tstates_for(increment, sizeof increment, 14331), 11 + 6 + 5);
}

static void a_push_is_charged_for_both_halves_of_the_address(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t push[] = {0xC5}; /* PUSH BC — pc:4,ir:1,sp-1:3,sp-2:3 */
  spectrum.cpu.sp = 0x8000;      /* so the two writes land on 0x7FFF and 0x7FFE */
  /* The refresh address is in the ROM and costs nothing; the writes fall on
     14336 and 14344, and the chip owes five at each. */
  TEST_EQUAL(tstates_for(push, sizeof push, 14331), 11 + 5 + 5);
}

/* A machine is not settled the moment the processor is between instructions:
   an instruction whose last T-state is itself charged leaves the clock
   stopped past its end, and a snapshot taken there would lose the rest. */
static void a_hold_the_last_tstate_earned_outlives_the_instruction(void) {
  power_on(SPECTRUM_RAM_48K);
  const uint8_t block[] = {0xED, 0xA0}; /* LDI — pc:4,pc+1:4,hl:3,de:3,de:1 x2 */
  spectrum.cpu.h = 0x80;                /* reading from outside the screen */
  spectrum.cpu.d = 0x40;                /* and writing into it */
  put_at(PROGRAM_ADDRESS, block, sizeof block);
  spectrum.cpu.pc = PROGRAM_ADDRESS;
  ula_seek(&spectrum.ula, 14320);
  do {
    spectrum_tick(&spectrum);
  } while (!z80_instruction_complete(&spectrum.cpu));
  /* The last of the two internal T-states landed on 14335, and the chip owes
     six there — after the instruction the processor has finished. */
  TEST_EQUAL(spectrum.held_ticks, 6);
  spectrum_finish_instruction(&spectrum);
  TEST_EQUAL(spectrum.held_ticks, 0);
  TEST_EQUAL(spectrum.ula.frame_tick, 14320 + 22);
}

/* A port is charged by two rules at once, in the four combinations below. */
static void a_port_is_charged_by_its_low_bit_and_its_high_byte(void) {
  power_on(SPECTRUM_RAM_48K);
  /* Reading into A rather than B, so that the port in BC survives the
     instruction that used it. */
  const uint8_t in[] = {0xED, 0x78}; /* IN A,(C) — pc:4,pc+1:4,I/O */

  /* Three starts, at which the access itself falls on the head of a slot,
     one T-state into it, and on the last of the two the chip leaves alone. */
  spectrum.cpu.b = 0x00; /* not contended memory, the ULA's port: N:1, C:3 */
  spectrum.cpu.c = 0xFE;
  TEST_EQUAL(tstates_for(in, sizeof in, 14331), 12 + 1);
  TEST_EQUAL(tstates_for(in, sizeof in, 14334), 12 + 6);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12 + 5);

  spectrum.cpu.b = 0x40; /* contended memory and the ULA's port: C:1, C:3 */
  spectrum.cpu.c = 0xFE;
  TEST_EQUAL(tstates_for(in, sizeof in, 14331), 12 + 2);
  TEST_EQUAL(tstates_for(in, sizeof in, 14334), 12 + 6);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12 + 6);

  spectrum.cpu.b = 0x40; /* contended memory, not the ULA's port: C:1 four times */
  spectrum.cpu.c = 0xFF;
  TEST_EQUAL(tstates_for(in, sizeof in, 14331), 12 + 8);
  TEST_EQUAL(tstates_for(in, sizeof in, 14334), 12 + 12);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12 + 12);

  spectrum.cpu.b = 0x00; /* neither: N:4, and nothing is owed anywhere */
  spectrum.cpu.c = 0xFF;
  TEST_EQUAL(tstates_for(in, sizeof in, 14331), 12);
  TEST_EQUAL(tstates_for(in, sizeof in, 14334), 12);
  TEST_EQUAL(tstates_for(in, sizeof in, 14335), 12);
}

/* The ULA stops the processor's clock, not the tape's: a held T-state is one
   the deck plays straight through, and a loader that lost T-states to
   contention would measure every edge wrong. */
static void the_tape_runs_on_while_the_clock_is_held(void) {
  power_on(SPECTRUM_RAM_48K);
  static const uint8_t image[] = {0x02, 0x00, 0xFF, 0x55};
  static tape_t tape;
  const char *problem = NULL;
  tape_init(&tape, SPECTRUM_TICKS_PER_MILLISECOND);
  TEST_CHECK(tape_insert(&tape, image, sizeof image, &problem));
  spectrum_insert_tape(&spectrum, &tape);
  tape_play(&tape);
  spectrum_tick(&spectrum); /* the deck takes up its first pulse */

  const uint32_t left = tape.pulse_ticks_left;
  spectrum.held_ticks = 200; /* as a contended access leaves it */
  for (int tick = 0; tick < 100; tick++) {
    spectrum_tick(&spectrum);
  }
  TEST_EQUAL(spectrum.held_ticks, 100);          /* the processor stood still */
  TEST_EQUAL(tape.pulse_ticks_left, left - 100); /* and the tape did not */
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
  TEST_RUN(an_odd_port_reaches_nothing_and_floats);
  TEST_RUN(the_first_interrupt_is_accepted_sixteen_tstates_in);
  TEST_RUN(the_keyboard_finds_a_character_where_it_is_printed);
  TEST_RUN(every_legend_is_where_the_key_is_printed);
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
  TEST_RUN(the_tape_runs_on_while_the_clock_is_held);
  return TEST_REPORT("spectrum");
}
