/*
 * spectrum_test — the machine, running programs written here.
 *
 * The board's own decisions are what is under test: what answers where in
 * memory, which ports reach the ULA, how the keyboard hangs off the address
 * lines, and that the ULA's interrupt reaches the processor. The firmware is
 * not involved; the programs are a few bytes each, put in the ROM the
 * processor resets into.
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
  TEST_RUN(the_border_reaches_the_framebuffer);
  TEST_RUN(the_picture_lands_where_the_frame_sync_leaves_it);
  return TEST_REPORT("spectrum");
}
