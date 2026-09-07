/*
 * spectrum_timing_test — how long each instruction takes, and where in the
 * frame it was asked.
 *
 * On this machine an instruction's duration is not a property of the
 * instruction. The ULA settles its argument with the processor by stopping
 * the clock, so the same opcode begun a few T-states apart costs different
 * amounts, and a program's speed is a property of where in the frame it
 * started. Nothing in the SingleStepTests corpus can see that: it runs the
 * processor with its wait pin released throughout. spectrum_test grades a
 * handful of positions picked by hand; this walks every T-state of the first
 * displayed line, a line in the middle, the last, and the border either side,
 * for one instruction of every shape the published table charges differently:
 * each place a cycle can fall, each of the four rows an I/O access is charged
 * by, a conditional taken and not taken, and the internal T-states that hold
 * the IR pair rather than an address the instruction chose.
 *
 * What each should cost is worked out here from the published rule, written
 * out below in its own numbers. It is deliberately not asked of ula.c: a test
 * that asked the emulator what it owed would agree with the emulator's own
 * mistakes.
 *
 * Sources:
 * - "Contended memory" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended_memory — the delay owed
 *   at each T-state of a frame ("If the contended memory is accessed 14335
 *   ... tstates after an interrupt ... the Z80 will be delayed for 6
 *   tstates"), and the instruction breakdown table, which gives for each
 *   opcode "the pattern of contention that is applied ... which is
 *   essentially equivalent to when T1 operations happen in each
 *   instruction". Every pattern below is transcribed from that table.
 * - "Contended I/O" (Sinclair Wiki),
 *   https://sinclair.wiki.zxnet.co.uk/wiki/Contended%20I/O — the four rows an
 *   I/O access is charged by, keyed on the port's high byte and its lowest
 *   bit: "N:1, C:3", "N:4", "C:1, C:3" and "C:1, C:1, C:1, C:1".
 * - libspectrum's timings.c (Philip Kendall, Fuse),
 *   https://sourceforge.net/p/fuse-emulator/libspectrum/ci/master/tree/timings.c
 *   — the 312 lines a frame runs to, which neither page above gives and which
 *   the delay's wraparound is worked out against.
 */
#include <string.h>

#include "spectrum.h"
#include "test.h"

static uint8_t rom[SPECTRUM_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_48K];
static spectrum_t spectrum;

/* Where the instruction under test is put: in the ROM, which the ULA never
   wants, so the only charges are the ones its operands earn. */
#define UNDER_TEST 0x0100

/* Longer than any instruction here and every charge a line can put on it, so
   a program that never finishes is caught rather than hung on. */
#define MOST_TSTATES 400

/* The published rule, in the published numbers. The first T-state at which
   an access to contended memory is delayed, the delay owed at each of the
   eight that follow, how many T-states of a line are spent on the picture and
   how many the line lasts, and how many lines carry one. */
#define FIRST_DELAYED_TSTATE 14335
#define DISPLAYED_TSTATES_IN_A_LINE 128
#define TSTATES_IN_A_LINE 224
#define DISPLAYED_LINES 192
#define LINES_IN_A_FRAME 312
#define TSTATES_IN_A_FRAME (TSTATES_IN_A_LINE * LINES_IN_A_FRAME)

static int published_delay(uint32_t at) {
  static const int owed[8] = {6, 5, 4, 3, 2, 1, 0, 0};
  if (at < FIRST_DELAYED_TSTATE) {
    return 0;
  }
  const uint32_t since = at - FIRST_DELAYED_TSTATE;
  if (since >= (uint32_t)DISPLAYED_LINES * TSTATES_IN_A_LINE) {
    return 0;
  }
  const uint32_t into_the_line = since % TSTATES_IN_A_LINE;
  if (into_the_line >= DISPLAYED_TSTATES_IN_A_LINE) {
    return 0;
  }
  return owed[into_the_line % 8];
}

/* One entry of a pattern: an address held on the bus for so many T-states,
   and whether it is one the ULA wants. A charge falls at the head of each. */
typedef struct {
  bool wanted;
  int tstates;
} cycle;

#define WANTED true
#define NOT_WANTED false

typedef struct {
  const char *mnemonic;
  uint8_t opcodes[4];
  uint8_t length;
  cycle pattern[10];
  uint8_t cycles;
  /* The interrupt register, which puts the address of an internal T-state
     where the ULA can want it, and the flags, which decide whether a
     conditional instruction takes the cycles the table brackets. */
  uint8_t interrupt_register;
  uint8_t flags;
  /* An I/O access is charged by its own rule rather than at the head of one
     cycle, so a pattern that ends in one says which port it reaches. */
  bool ends_in_a_port;
  uint16_t port;
} timing;

/* The program counter is in the ROM; HL and DE point into the sixteen
   kilobytes the ULA reads; SP is at 0x8000, so a push writes 0x7FFF and
   0x7FFE, which are wanted, and a pop reads 0x8000 upwards, which are not.
   Every pattern is the "ULA" column of the instruction breakdown table. */
#define WORKING_HL 0x4000
#define WORKING_DE 0x4001
#define WORKING_IX 0x4000
#define WORKING_SP 0x8000
/* An interrupt register that puts the IR pair in the ULA's sixteen kilobytes,
   for the instructions whose internal T-states hold that address. */
#define WORKING_I 0x40

static const timing instructions[] = {
    {"NOP", {0x00}, 1, {{NOT_WANTED, 4}}, 1, 0, 0, false, 0},
    {"LD A,(HL)", {0x7E}, 1, {{NOT_WANTED, 4}, {WANTED, 3}}, 2, 0, 0, false, 0},
    {"LD (HL),A", {0x77}, 1, {{NOT_WANTED, 4}, {WANTED, 3}}, 2, 0, 0, false, 0},
    {"LD A,(DE)", {0x1A}, 1, {{NOT_WANTED, 4}, {WANTED, 3}}, 2, 0, 0, false, 0},
    {"INC (HL)",
     {0x34},
     1,
     {{NOT_WANTED, 4}, {WANTED, 3}, {WANTED, 1}, {WANTED, 3}},
     4,
     0,
     0,
     false,
     0},
    {"INC BC", {0x03}, 1, {{NOT_WANTED, 4}, {NOT_WANTED, 1}, {NOT_WANTED, 1}}, 3, 0, 0, false, 0},
    {"ADD HL,BC",
     {0x09},
     1,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1}},
     8,
     0,
     0,
     false,
     0},
    {"JR 0",
     {0x18, 0x00},
     2,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 3},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1}},
     7,
     0,
     0,
     false,
     0},
    {"PUSH BC",
     {0xC5},
     1,
     {{NOT_WANTED, 4}, {NOT_WANTED, 1}, {WANTED, 3}, {WANTED, 3}},
     4,
     0,
     0,
     false,
     0},
    {"POP BC", {0xC1}, 1, {{NOT_WANTED, 4}, {NOT_WANTED, 3}, {NOT_WANTED, 3}}, 3, 0, 0, false, 0},
    {"BIT 0,(HL)",
     {0xCB, 0x46},
     2,
     {{NOT_WANTED, 4}, {NOT_WANTED, 4}, {WANTED, 3}, {WANTED, 1}},
     4,
     0,
     0,
     false,
     0},
    {"SET 0,(HL)",
     {0xCB, 0xC6},
     2,
     {{NOT_WANTED, 4}, {NOT_WANTED, 4}, {WANTED, 3}, {WANTED, 1}, {WANTED, 3}},
     5,
     0,
     0,
     false,
     0},
    {"RLD",
     {0xED, 0x6F},
     2,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 4},
      {WANTED, 3},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 3}},
     8,
     0,
     0,
     false,
     0},
    {"LDI",
     {0xED, 0xA0},
     2,
     {{NOT_WANTED, 4}, {NOT_WANTED, 4}, {WANTED, 3}, {WANTED, 3}, {WANTED, 1}, {WANTED, 1}},
     6,
     0,
     0,
     false,
     0},
    {"CPI",
     {0xED, 0xA1},
     2,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 4},
      {WANTED, 3},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1}},
     8,
     0,
     0,
     false,
     0},
    {"EX (SP),HL",
     {0xE3},
     1,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 3},
      {NOT_WANTED, 3},
      {NOT_WANTED, 1},
      {NOT_WANTED, 3},
      {NOT_WANTED, 3},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1}},
     8,
     0,
     0,
     false,
     0},
    /* The four shapes an I/O access comes in: the ULA's own port and another,
       each reached from an address the ULA wants and one it does not. */
    {"IN A,(&FE)", {0xDB, 0xFE}, 2, {{NOT_WANTED, 4}, {NOT_WANTED, 3}}, 2, 0, 0, true, 0x00FE},
    {"IN A,(&FF)", {0xDB, 0xFF}, 2, {{NOT_WANTED, 4}, {NOT_WANTED, 3}}, 2, 0, 0, true, 0x00FF},
    {"IN B,(C)", {0xED, 0x40}, 2, {{NOT_WANTED, 4}, {NOT_WANTED, 4}}, 2, 0, 0, true, 0x40FE},
    {"OUT (C),B", {0xED, 0x41}, 2, {{NOT_WANTED, 4}, {NOT_WANTED, 4}}, 2, 0, 0, true, 0x40FE},
    /* The fourth row, which the other three never reach: a port the ULA does
       not answer, at an address that looks to it like contended memory. It is
       the only row charged at every one of its four T-states. */
    {"IN B,(C) at &40FF",
     {0xED, 0x40},
     2,
     {{NOT_WANTED, 4}, {NOT_WANTED, 4}},
     2,
     0,
     0,
     true,
     0x40FF},

    /* An address given outright rather than held in a register. */
    {"LD A,(&4000)",
     {0x3A, 0x00, 0x40},
     3,
     {{NOT_WANTED, 4}, {NOT_WANTED, 3}, {NOT_WANTED, 3}, {WANTED, 3}},
     4,
     0,
     0,
     false,
     0},

    /* An index register and a displacement, whose five internal T-states hold
       the program counter and not the address they are working out. */
    {"LD A,(IX+0)",
     {0xDD, 0x7E, 0x00},
     3,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 4},
      {NOT_WANTED, 3},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {WANTED, 3}},
     9,
     0,
     0,
     false,
     0},

    /* A conditional, both ways: the table brackets the cycles a branch costs
       only when it is taken, and the flags below decide which. */
    {"JR NZ,0 taken",
     {0x20, 0x00},
     2,
     {{NOT_WANTED, 4},
      {NOT_WANTED, 3},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1},
      {NOT_WANTED, 1}},
     7,
     0,
     0x00,
     false,
     0},
    {"JR Z,0 not taken", {0x28, 0x00}, 2, {{NOT_WANTED, 4}, {NOT_WANTED, 3}}, 2, 0, 0x00, false, 0},

    /* The same two instructions with the interrupt register pointed at the
       screen's memory, so the internal T-states that hold the IR pair are
       addresses the ULA wants and are charged for. */
    {"ADD HL,BC with I in the screen",
     {0x09},
     1,
     {{NOT_WANTED, 4},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1},
      {WANTED, 1}},
     8,
     WORKING_I,
     0,
     false,
     0},
    {"PUSH BC with I in the screen",
     {0xC5},
     1,
     {{NOT_WANTED, 4}, {WANTED, 1}, {WANTED, 3}, {WANTED, 3}},
     4,
     WORKING_I,
     0,
     false,
     0},
};

/* What the published rule says an instruction costs, begun at this T-state.
   A charge falls at the head of a cycle whose address the ULA wants, and the
   cycle's own T-states follow it. */
static int published_tstates(const timing *entry, uint32_t at) {
  uint32_t t = at;
  for (uint8_t index = 0; index < entry->cycles; index++) {
    if (entry->pattern[index].wanted) {
      t += (uint32_t)published_delay(t % TSTATES_IN_A_FRAME);
    }
    t += (uint32_t)entry->pattern[index].tstates;
  }
  if (entry->ends_in_a_port) {
    /* "Contended I/O": four rows, keyed on whether the port's high byte is
       in 0x40 to 0x7F and whether its lowest bit is set. Each row is one
       T-state, charged or not. */
    const bool high = (entry->port >> 8) >= 0x40 && (entry->port >> 8) <= 0x7F;
    const bool ula = (entry->port & 1) == 0;
    const bool charged[4] = {high, high || ula, high && !ula, high && !ula};
    for (int row = 0; row < 4; row++) {
      if (charged[row]) {
        t += (uint32_t)published_delay(t % TSTATES_IN_A_FRAME);
      }
      t += 1;
    }
  }
  return (int)(t - at);
}

/* T-states from the start of one opcode fetch to the start of the next, which
   is what the published patterns measure: a hold an instruction earns at its
   last T-state falls before the next fetch and not inside itself. */
static int measured_tstates(const timing *entry, uint32_t at) {
  memcpy(rom + UNDER_TEST, entry->opcodes, entry->length);
  spectrum.cpu.pc = UNDER_TEST;
  spectrum.cpu.h = WORKING_HL >> 8;
  spectrum.cpu.l = WORKING_HL & 0xFF;
  spectrum.cpu.d = WORKING_DE >> 8;
  spectrum.cpu.e = WORKING_DE & 0xFF;
  spectrum.cpu.ixh = WORKING_IX >> 8;
  spectrum.cpu.ixl = WORKING_IX & 0xFF;
  spectrum.cpu.sp = WORKING_SP;
  spectrum.cpu.i = entry->interrupt_register;
  spectrum.cpu.f = entry->flags;
  spectrum.cpu.b = (uint8_t)(entry->port >> 8);
  spectrum.cpu.c = (uint8_t)entry->port;
  spectrum.held_ticks = 0;
  spectrum.port_charges_left = 0;
  ula_seek(&spectrum.ula, at);

  int tstates = 0;
  do {
    spectrum_tick(&spectrum);
    tstates++;
  } while ((!z80_instruction_complete(&spectrum.cpu) || spectrum.held_ticks > 0) &&
           tstates < MOST_TSTATES);
  return tstates;
}

/* How many mismatches to describe before the count alone will do. */
#define MOST_REPORTED 8

static int mismatches;

static void walk(uint32_t from, uint32_t to) {
  for (size_t index = 0; index < sizeof instructions / sizeof instructions[0]; index++) {
    const timing *entry = &instructions[index];
    for (uint32_t at = from; at <= to; at++) {
      const int want = published_tstates(entry, at);
      const int got = measured_tstates(entry, at);
      if (got == want) {
        continue;
      }
      mismatches++;
      if (mismatches <= MOST_REPORTED) {
        TEST_FAIL("%s begun at %u took %d T-states, the published rule says %d", entry->mnemonic,
                  at, got, want);
      }
    }
  }
}

static void power_on(void) {
  memset(rom, 0x00, sizeof rom);
  memset(ram, 0x00, sizeof ram);
  spectrum_init(&spectrum, ram, SPECTRUM_RAM_48K, rom);
}

/* The whole of the first line that carries picture, and the border in front
   of it: every phase of the eight, the T-state the charging begins on, and
   the point in the line where it stops. */
static void the_first_displayed_line_agrees_with_the_published_rule(void) {
  power_on();
  walk(FIRST_DELAYED_TSTATE - 24, FIRST_DELAYED_TSTATE + TSTATES_IN_A_LINE);
}

/* A line in the middle of the picture, where nothing about the frame is
   ending: the same rule again, and the arithmetic that carries it from one
   line to the next. */
static void a_line_in_the_middle_agrees_with_the_published_rule(void) {
  power_on();
  const uint32_t middle =
      FIRST_DELAYED_TSTATE + (uint32_t)(DISPLAYED_LINES / 2) * TSTATES_IN_A_LINE;
  walk(middle, middle + TSTATES_IN_A_LINE);
}

/* And the last, where the charging stops for the frame rather than for the
   line — an instruction begun inside it can run out the other side. */
static void the_last_displayed_line_agrees_with_the_published_rule(void) {
  power_on();
  const uint32_t last = FIRST_DELAYED_TSTATE + (uint32_t)(DISPLAYED_LINES - 1) * TSTATES_IN_A_LINE;
  walk(last, last + TSTATES_IN_A_LINE + 24);
}

/* The border owes nothing at all, at either end of the frame. */
static void the_border_is_never_charged(void) {
  power_on();
  walk(0, 64);
  walk(TSTATES_IN_A_FRAME - 64, TSTATES_IN_A_FRAME - 1);
}

int main(void) {
  TEST_RUN(the_first_displayed_line_agrees_with_the_published_rule);
  TEST_RUN(a_line_in_the_middle_agrees_with_the_published_rule);
  TEST_RUN(the_last_displayed_line_agrees_with_the_published_rule);
  TEST_RUN(the_border_is_never_charged);
  if (mismatches > MOST_REPORTED) {
    printf("  and %d more\n", mismatches - MOST_REPORTED);
  }
  return TEST_REPORT("spectrum timing");
}
