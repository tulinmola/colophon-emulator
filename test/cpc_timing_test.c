/*
 * cpc_timing_test — how long each instruction takes on a CPC, in
 * microseconds, where inside one its write reaches the CRTC, and what taking
 * an interrupt costs.
 *
 * The Gate Array holds the CPU off the RAM for three cycles in four, so
 * every machine cycle stretches until the next window and every instruction
 * ends on a microsecond boundary. Nothing about that is visible in the Z80
 * alone, and nothing in the SingleStepTests corpus measures it: the corpus
 * runs the CPU with WAIT released throughout.
 *
 * What judges it instead is two tables of measured durations, made
 * independently and agreeing row for row. Both are quoted below; a row
 * where they differ is marked and tested against neither until someone
 * settles it on hardware.
 *
 * A duration cannot say where inside a run the write lands, and two of them
 * are set here against the microsecond the Compendium puts each on.
 *
 * Sources:
 * - "Timings" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/instrtim.html — "The following table
 *   gives the complete execution time for all CPU instructions. These
 *   timings have been measured."
 * - "The Amstrad CPC CRTC Compendium" v1.11 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.11-EN.pdf ch. 26, "Duration of
 *   instr. on the CPC".
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 */
#include <string.h>

#include "cpc.h"
#include "test.h"

static uint8_t ram[0x20000];
static uint8_t lower_rom[0x4000];
static cpc_t cpc;

/* Where the instruction under test is placed, clear of the reset vector. */
#define UNDER_TEST 0x0040

typedef struct {
  const char *mnemonic;
  uint8_t opcodes[4];
  uint8_t length;
  uint8_t microseconds;
} timing;

/* Both sources agree on every row here. Where an instruction's duration
   depends on which way it went, the condition is arranged to hold. */
static const timing repeated[] = {
    {"NOP", {0x00}, 1, 1},
    {"LD BC,nnnn", {0x01, 0x34, 0x12}, 3, 3},
    {"INC BC", {0x03}, 1, 2},
    {"INC B", {0x04}, 1, 1},
    {"LD B,n", {0x06, 0x42}, 2, 2},
    {"RLCA", {0x07}, 1, 1},
    {"EX AF,AF'", {0x08}, 1, 1},
    {"ADD HL,BC", {0x09}, 1, 3},
    {"LD A,(BC)", {0x0A}, 1, 2},
    {"LD (BC),A", {0x02}, 1, 2},
    {"LD (nnnn),HL", {0x22, 0x00, 0x90}, 3, 5},
    {"LD HL,(nnnn)", {0x2A, 0x00, 0x90}, 3, 5},
    {"DAA", {0x27}, 1, 1},
    {"LD (nnnn),A", {0x32, 0x00, 0x90}, 3, 4},
    {"LD A,(nnnn)", {0x3A, 0x00, 0x90}, 3, 4},
    {"INC (HL)", {0x34}, 1, 3},
    {"LD (HL),n", {0x36, 0x42}, 2, 3},
    {"SCF", {0x37}, 1, 1},
    {"LD B,C", {0x41}, 1, 1},
    {"LD B,(HL)", {0x46}, 1, 2},
    {"LD (HL),B", {0x70}, 1, 2},
    {"ADD A,B", {0x80}, 1, 1},
    {"ADD A,(HL)", {0x86}, 1, 2},
    {"AND B", {0xA0}, 1, 1},
    {"CP (HL)", {0xBE}, 1, 2},
    {"POP BC", {0xC1}, 1, 3},
    {"PUSH BC", {0xC5}, 1, 4},
    {"ADD A,n", {0xC6, 0x01}, 2, 2},
    {"EXX", {0xD9}, 1, 1},
    {"EX DE,HL", {0xEB}, 1, 1},
    {"EX (SP),HL", {0xE3}, 1, 6},
    {"LD SP,HL", {0xF9}, 1, 2},
    {"DI", {0xF3}, 1, 1},
    {"EI", {0xFB}, 1, 1},
    {"IN A,(n)", {0xDB, 0x00}, 2, 3},
    {"OUT (n),A", {0xD3, 0x00}, 2, 3},
    /* CB page */
    {"RLC B", {0xCB, 0x00}, 2, 2},
    {"RLC (HL)", {0xCB, 0x06}, 2, 4},
    {"BIT 0,B", {0xCB, 0x40}, 2, 2},
    {"BIT 0,(HL)", {0xCB, 0x46}, 2, 3},
    {"SET 0,(HL)", {0xCB, 0xC6}, 2, 4},
    /* ED page */
    {"NEG", {0xED, 0x44}, 2, 2},
    {"IM 1", {0xED, 0x56}, 2, 2},
    {"LD I,A", {0xED, 0x47}, 2, 3},
    {"LD A,I", {0xED, 0x57}, 2, 3},
    {"SBC HL,BC", {0xED, 0x42}, 2, 4},
    {"ADC HL,BC", {0xED, 0x4A}, 2, 4},
    {"LD (nnnn),BC", {0xED, 0x43, 0x00, 0x90}, 4, 6},
    {"LD BC,(nnnn)", {0xED, 0x4B, 0x00, 0x90}, 4, 6},
    {"RLD", {0xED, 0x6F}, 2, 5},
    {"RRD", {0xED, 0x67}, 2, 5},
    {"ED nop", {0xED, 0x00}, 2, 2},
    {"IN B,(C)", {0xED, 0x40}, 2, 4},
    {"OUT (C),B", {0xED, 0x41}, 2, 4},
    /* DD page */
    {"ADD IX,BC", {0xDD, 0x09}, 2, 4},
    {"LD IX,nnnn", {0xDD, 0x21, 0x34, 0x12}, 4, 4},
    {"INC IX", {0xDD, 0x23}, 2, 3},
    {"INC IXH", {0xDD, 0x24}, 2, 2},
    {"LD IXH,n", {0xDD, 0x26, 0x42}, 3, 3},
    {"INC (IX+d)", {0xDD, 0x34, 0x00}, 3, 6},
    {"LD (IX+d),n", {0xDD, 0x36, 0x00, 0x42}, 4, 6},
    {"LD B,IXH", {0xDD, 0x44}, 2, 2},
    {"LD B,(IX+d)", {0xDD, 0x46, 0x00}, 3, 5},
    {"ADD A,(IX+d)", {0xDD, 0x86, 0x00}, 3, 5},
    {"PUSH IX", {0xDD, 0xE5}, 2, 5},
    /* The one row where the two sources disagree: cpctech says 5, the
       Compendium 4. The Compendium is followed here, because cpctech
       contradicts itself — it gives LD IX,nnnn 4us, and that instruction has
       the identical machine cycles (4,4,3,3). Its POP IX sits directly under
       PUSH IX at 5, which is what a slipped row looks like. */
    {"POP IX", {0xDD, 0xE1}, 2, 4},
    {"EX (SP),IX", {0xDD, 0xE3}, 2, 7},
    {"LD SP,IX", {0xDD, 0xF9}, 2, 3},
    /* DD CB page */
    {"RLC (IX+d)", {0xDD, 0xCB, 0x00, 0x06}, 4, 7},
    {"BIT 0,(IX+d)", {0xDD, 0xCB, 0x00, 0x46}, 4, 6},
    {"SET 0,(IX+d)", {0xDD, 0xCB, 0x00, 0xC6}, 4, 7},
    /* Conditions that fail: none of these touches a flag, so every copy
       falls through exactly as the first one did. */
    {"JR NZ (not taken)", {0x20, 0x00}, 2, 2},
    {"JP NZ (not taken)", {0xC2, 0x00, 0x00}, 3, 3},
    {"RET NZ (not taken)", {0xC0}, 1, 2},
    {"CALL NZ (not taken)", {0xC4, 0x00, 0x00}, 3, 3},
};

/* Instructions that jump to themselves, or repeat in place. The operands
   are the address the copy is placed at, so each execution lands back on
   its own first byte. */
static const timing self_looping[] = {
    {"JP nnnn", {0xC3, UNDER_TEST, 0x00}, 3, 3},
    {"JP NZ (taken)", {0xC2, UNDER_TEST, 0x00}, 3, 3},
    {"JP (HL)", {0xE9}, 1, 1},
    {"JP (IX)", {0xDD, 0xE9}, 2, 2},
    {"JR d", {0x18, 0xFE}, 2, 3},
    {"JR NZ (taken)", {0x20, 0xFE}, 2, 3},
    {"DJNZ (taken)", {0x10, 0xFE}, 2, 4},
    {"LDIR (repeating)", {0xED, 0xB0}, 2, 6},
    {"CPIR (repeating)", {0xED, 0xB1}, 2, 6},
};

static void power_on(void) {
  memset(ram, 0, sizeof ram);
  memset(lower_rom, 0, sizeof lower_rom); /* NOPs everywhere */
  cpc_init(&cpc, ram, sizeof ram, lower_rom, 0);
}

/* Two ways to hold an instruction still long enough to time it, because
   the Gate Array's alignment padding falls on whichever cycle asks for the
   bus next: a lone instruction hands part of its cost to its successor, so
   INC BC after a NOP takes six T-states where INC BC after INC BC takes
   eight, and eight is what the tables record.

   REPEATED lays copies end to end. It suits anything that leaves PC to run
   on and does not change what the next copy will do.

   SELF_LOOPING places one copy that jumps to itself, so each execution is
   already a steady-state iteration. It is the only way to reach the taken
   side of a branch, and the only way to reach a block instruction while it
   is still repeating.

   Either way the first copies are run to settle and the rest are timed.

   Out of reach with both: DJNZ not taken, which needs B reset before every
   execution; RET cc and CALL cc taken, which need a prepared stack and form
   loops whose cost includes an instruction not yet measured here; and the
   plain RET, CALL and RST for the same reason. Naming them is better than
   letting their absence read as coverage. */
#define SETTLING_COPIES 3
#define TIMED_COPIES 8

typedef enum {
  REPEATED,
  SELF_LOOPING,
} measurement;

/* Run one instruction to completion, returning the T-states it took. */
static int one_instruction(void) {
  for (int ticks = 1; ticks < 400; ticks++) {
    cpc_tick(&cpc);
    if (z80_instruction_complete(&cpc.cpu)) {
      return ticks;
    }
  }
  return 0;
}

static int measure(const timing *entry, measurement how) {
  power_on();
  int copies = how == REPEATED ? SETTLING_COPIES + TIMED_COPIES : 1;
  for (int copy = 0; copy < copies; copy++) {
    memcpy(lower_rom + UNDER_TEST + (size_t)copy * entry->length, entry->opcodes, entry->length);
  }
  cpc.cpu.pc = UNDER_TEST;
  cpc.cpu.sp = 0x8000;
  cpc.cpu.d = 0x91; /* (DE) is where the block instructions write */
  cpc.cpu.e = 0x00;
  if (how == REPEATED) {
    cpc.cpu.h = 0x90; /* (HL) points at RAM, clear of the program */
    cpc.cpu.l = 0x00;
    cpc.cpu.ixh = 0x90;
    cpc.cpu.iyh = 0x90;
    cpc.cpu.f = 0x40; /* Z set, so an NZ condition falls through */
  } else {
    /* Everything a self-looping instruction might jump through points at
       the instruction itself, and the counters are set far enough from
       their limits that none of them runs out mid-measurement. */
    cpc.cpu.h = UNDER_TEST >> 8;
    cpc.cpu.l = UNDER_TEST & 0xFF;
    cpc.cpu.ixh = UNDER_TEST >> 8;
    cpc.cpu.ixl = UNDER_TEST & 0xFF;
    cpc.cpu.b = 0xFF;
    cpc.cpu.c = 0x00;
    cpc.cpu.a = 0xAA; /* matches nothing CPIR will read */
    cpc.cpu.f = 0x00; /* Z clear, so an NZ condition is taken */
  }

  for (int copy = 0; copy < SETTLING_COPIES; copy++) {
    if (one_instruction() == 0) {
      TEST_FAIL("%s never finished", entry->mnemonic);
      return -1;
    }
  }

  int ticks = 0;
  for (int copy = 0; copy < TIMED_COPIES; copy++) {
    ticks += one_instruction();
  }
  if (ticks % (4 * TIMED_COPIES) != 0) {
    TEST_FAIL("%s took %d T-states over %d executions, not a whole number of microseconds each",
              entry->mnemonic, ticks, TIMED_COPIES);
    return -1;
  }
  return ticks / (4 * TIMED_COPIES);
}

static void check(const timing *entries, size_t count, measurement how) {
  for (size_t index = 0; index < count; index++) {
    int measured = measure(&entries[index], how);
    if (measured < 0) {
      continue;
    }
    if (measured != entries[index].microseconds) {
      TEST_FAIL("%s took %dus, both tables say %d", entries[index].mnemonic, measured,
                entries[index].microseconds);
    }
  }
}

/* Where inside an instruction its write reaches the CRTC, which no duration
   can say: OUT (C),r puts it on its second-to-last microsecond and OUTI on
   its last, so two instructions land a microsecond apart from the same
   start. The "just in time" techniques a demo uses to move a register on the
   character it is read at stand or fall on which.

   "an output entry with an "OUT(C),R8" occurs on the 3rd NOP for a CRTC
   equipped with a GATE ARRAY, and on the 4th NOP for an ASIC that emulates a
   CRTC (CRTC's 3 and 4)", and "the update of a CRTC register takes place on
   the 5th µsec of the OUTI instruction, regardless of the type of CRTC,
   while there is a difference of 1 µsec when the update takes place with the
   OUT(C),R8 instruction" (ch. 4.4.4). Ch. 13.7.1, writing about a type 1,
   names the same two microseconds.

   Counted from the character the opcode fetch falls on and read off the
   register rather than off the bus, because the microsecond the chapter
   names is the one the CRTC takes the value in, and the Gate Array holds an
   I/O cycle up across several characters after the processor has raised it.
   A NOP is a microsecond is a character here: the Gate Array "gives a 1 MHz
   rate for the AY-3-8912, the CRTC, and clocks the Z80A at 4 MHz" (ch.
   4.4.4), which is why the third NOP is two characters after the first.

   The ASIC's extra microsecond is not here: cpc.c wires a Gate Array
   whatever the chip is built as, and the head of crtc.h leaves the per-type
   divergences of this timing out of what it claims. */
static void an_io_cycle_falls_where_its_instruction_puts_it(void) {
  static const struct {
    const char *mnemonic;
    uint8_t opcodes[2];
    uint8_t b; /* OUTI puts B on the bus already decremented, so both address &BD00 */
    uint8_t characters_after_the_fetch;
  } cases[] = {
      {"OUT (C),C", {0xED, 0x49}, 0xBD, 2}, /* the 3rd microsecond */
      {"OUTI", {0xED, 0xA3}, 0xBE, 4},      /* and the 5th */
  };
  for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
    power_on();
    /* A line wide enough for the characters to be told apart, and then a
       register clear of R0 for the instruction to land in. */
    crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0));
    crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 63));
    crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 12));
    memcpy(lower_rom + UNDER_TEST, cases[index].opcodes, 2);
    cpc.cpu.pc = UNDER_TEST;
    cpc.cpu.sp = 0x8000;
    cpc.cpu.b = cases[index].b;
    cpc.cpu.c = 0x2A; /* what OUT (C),C carries */
    cpc.cpu.h = 0x90;
    cpc.cpu.l = 0x00;
    ram[0x9000] = 0x2A; /* and what OUTI fetches */
    int fetch = -1;
    int landed = -1;
    for (int tick = 0; tick < 200 && landed < 0; tick++) {
      cpc_tick(&cpc);
      if (fetch < 0 && (cpc.pins & (Z80_M1 | Z80_MREQ)) == (Z80_M1 | Z80_MREQ) &&
          z80_address(cpc.pins) == UNDER_TEST) {
        fetch = cpc.crtc.c0;
      }
      if (fetch >= 0 && cpc.crtc.registers[12] == 0x2A) {
        landed = cpc.crtc.c0;
      }
    }
    if (fetch < 0 || landed < 0) {
      TEST_FAIL("%s never reached the CRTC", cases[index].mnemonic);
      continue;
    }
    if (landed - fetch != cases[index].characters_after_the_fetch) {
      TEST_FAIL("%s reached the CRTC %d characters after its fetch, the Compendium says %d",
                cases[index].mnemonic, landed - fetch, cases[index].characters_after_the_fetch);
    }
  }
}

/* Ch. 13.6.2's chronogram, walked. It takes a line of R0=#3F, writes #7F
   across its last characters a microsecond at a time, and draws C0's own
   sequence for each placement: where the update is "ok (just in time)" the
   line runs on, "3b 3c 3d 3e 3f 40 41 42", and where it is "not considered
   (too late)" it wraps, "3b 3c 3d 3e 3f 0 1 2". The line is read off C0 for
   that reason and not off any register.

   Two instructions are drawn against each other, and the prose beside the
   diagram sets their distance: "in principle, the OUT(C),R8 instruction must
   start 2 µseconds later than the OUTI instruction to obtain the same
   result", the one taking its I/O on its 3rd microsecond and the other on
   its 5th (ch. 13.3, note 3). A type 1 keeps one placement more than that,
   because there "the comparison of C0 with R0 takes place after the
   assignment of R0 with the new value in some cases", and ch. 13.7.1 calls
   the reason "an internal processing phase shift between this CRTC and
   CRTCs 0 and 2".

   So the two windows are compared with each other rather than with any
   absolute lead, which is what the chapter states and what survives a change
   of instruction lengths: one microsecond apart on the type that takes the
   shift, two on the four that do not.

   Only the first character of that type's window is measured here. The walk
   stops at the character the wrap is taken on, so a write rescuing the line
   from the character after it arrives after the walk has already stopped and
   cannot be seen. What the second character does is graded in the chip's own
   suite, where the pin is driven directly and C0 is read out character by
   character. Shaker's B (6) grades the difference,
   and its "4TH uSec ON C0=0" came right when a write landing on the
   character clock was let take a type 1's line end back.

   Types 3 and 4 are not walked. Ch. 4.4.4 puts their OUT's entry a
   microsecond later than the other three's — "an output entry with an
   OUT(C),R8 occurs on the 3rd NOP for a CRTC equipped with a GATE ARRAY,
   and on the 4th NOP for an ASIC that emulates a CRTC" — so the two
   instructions stand one microsecond apart on them and not two, which is
   how ch. 13.6.3 draws it. That microsecond is not here, cpc.c wiring a
   Gate Array whatever the chip is built as, and rows asserting two would
   state a machine that never shipped and would have to be edited for a
   correct change to land. crtc.h's list of what is missing carries it
   instead.

   The leads are counted in microseconds from the start of the code under
   test and straddle the end of a 64-character line, which falls within
   them: a placement below the window is too early for the walk to mean
   anything, so the edge found is checked to be inside it and not at its
   floor. */
static const int earliest_placement = 54;
static const int latest_placement = 70;

static void an_outi_keeps_a_type_1_a_microsecond_longer_than_the_rest(void) {
  static const uint8_t types[] = {0, 1, 2};
  int outi_edge[3] = {-1, -1, -1};
  int out_edge[3] = {-1, -1, -1};
  for (size_t index = 0; index < sizeof types / sizeof types[0]; index++) {
    uint8_t type = types[index];
    int edges[2] = {-1, -1};
    /* The OUT first, then the OUTI, each walked until the line stops running
       on; an OUTI decrements B before its write, so both reach &BD00. */
    static const uint8_t opcodes[2] = {0x49, 0xA3};
    static const uint8_t b[2] = {0xBD, 0xBE};
    for (int which = 0; which < 2; which++) {
      for (int lead = earliest_placement; lead <= latest_placement && edges[which] < 0; lead++) {
        memset(ram, 0, sizeof ram);
        memset(lower_rom, 0, sizeof lower_rom); /* NOPs in front of it */
        cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
        crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0));
        crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 0x3F));
        crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0)); /* R0 stays selected */
        lower_rom[UNDER_TEST + lead] = 0xED;
        lower_rom[UNDER_TEST + lead + 1] = opcodes[which];
        ram[0x9000] = 0x7F; /* the width the write hands over */
        cpc.cpu.pc = UNDER_TEST;
        cpc.cpu.b = b[which];
        cpc.cpu.c = 0x7F;
        cpc.cpu.h = 0x90;
        cpc.cpu.sp = 0x8000;
        bool ran_on = false;
        uint8_t previous = cpc.crtc.c0;
        for (int tick = 0; tick < 4 * 90; tick++) {
          cpc_tick(&cpc);
          if (previous == 0x3F && cpc.crtc.c0 == 0x40) {
            ran_on = true;
          }
          if (previous == 0x3F && cpc.crtc.c0 == 0) {
            break;
          }
          previous = cpc.crtc.c0;
        }
        if (!ran_on) {
          edges[which] = lead; /* the first placement that is too late */
        }
      }
      /* Inside the window rather than at its floor: an edge found at the
         first placement would mean the walk began past the line's end and
         the difference below would be measured off a truncated window. */
      TEST_CHECK(edges[which] > earliest_placement);
    }
    if (edges[0] < 0 || edges[1] < 0) {
      continue;
    }
    TEST_EQUAL(edges[0] - edges[1], type == 1 ? 1 : 2);
    out_edge[index] = edges[0];
    outi_edge[index] = edges[1];
  }
  /* And where the microsecond falls, which the difference alone cannot say:
     ch. 13.6.2 holds the OUTI's placements one row longer than ch. 13.6.1
     does and leaves the OUT's where they are, so it is the OUTI that gains
     the microsecond on a type 1 and not the OUT that loses one. A change
     that moved both windows together would keep every difference above. */
  TEST_EQUAL(outi_edge[1] - outi_edge[0], 1);
  TEST_EQUAL(out_edge[1], out_edge[0]);
  TEST_EQUAL(outi_edge[2], outi_edge[0]);
  TEST_EQUAL(out_edge[2], out_edge[0]);
}

/* How long an interrupt costs, which no duration in the tables above covers.
   "The Z80A RST #38 instruction lasts 4 µsec when called by code. When an
   interrupt occurs, the call in #38 lasts 5 µsec" (ch. 27.4), and the chapter
   offers its own way of telling: "to test it, just compare the time taken by
   an RST #38 and the time taken by an interrupt with a fixed time code on
   19968 NOP's". Both halves of that comparison are run below.

   Both agree. The microsecond beyond the RST's four is the acknowledge's:
   Zilog's Figure 9 (UM0080) has the processor add two wait states to it
   and sample WAIT in the second alone, so the Gate Array's pattern
   stretches it once rather than twice. Shaker's D (I) times the first
   interrupt into a field of DEC DEs entered exactly one interrupt period
   after the last, and reads silicon's value only at five, with the
   request raised where gate_array.c raises it.

   Measured across the interrupt and the instruction after it, because an
   entry that leaves the processor out of step with the character clock is
   paid for by the next instruction rather than by itself. What is measured
   is the cost after a NOP; the cost after an instruction leaving the clock
   on another quarter is not the same, and is not pinned here. */
static void an_interrupt_costs_five_microseconds_where_an_rst_costs_four(void) {
  /* The two NOPs the span is measured over: the handler's first and second,
     a microsecond each once the processor is back in step. SETTLED is long
     enough that the cadence is certainly steady, and nothing turns on how
     long — any value from one to fifty-nine gives the same answer. */
  enum { THE_TWO_NOPS = 8, SETTLED = 30 };
  power_on();
  /* A line wide enough to interrupt on, and NOPs everywhere including the
     handler, so every ordinary step is one microsecond. */
  crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0));
  crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 63));
  crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 2));
  crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 46));
  crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 3));
  crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 0x0E));
  cpc.cpu.pc = UNDER_TEST;
  cpc.cpu.sp = 0x8000;
  cpc.cpu.iff1 = true;
  cpc.cpu.iff2 = true;
  cpc.cpu.im = 1;
  long previous = 0;
  long across = 0;
  long after = 0;
  int steady = 0;
  int stage = 0;
  int heard_on = -1;
  long m1_ended_at = -1;
  long withdrawn_at = -1;
  bool acknowledging = false;
  bool requested = false;
  for (long tick = 0; tick < 4000000 && stage < 2; tick++) {
    uint64_t pins = cpc_tick(&cpc);
    bool acknowledge = (pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ);
    if (heard_on < 0 && acknowledging && !acknowledge) {
      heard_on = cpc.gate_array.cpu_phase;
      m1_ended_at = tick;
    }
    acknowledging = acknowledge;
    if (withdrawn_at < 0 && requested && !gate_array_interrupt(&cpc.gate_array)) {
      withdrawn_at = tick;
    }
    requested = gate_array_interrupt(&cpc.gate_array);
    if (!z80_instruction_complete(&cpc.cpu)) {
      continue;
    }
    long step = tick + 1 - previous;
    if (stage == 1) {
      after = step;
      stage = 2;
    } else if (step == 4) {
      steady++;
    } else if (steady < SETTLED) {
      steady = 0;
    }
    if (step > 4 && steady >= SETTLED && stage == 0) {
      across = step;
      stage = 1;
    }
    previous = tick + 1;
  }
  if (stage < 2) {
    TEST_FAIL("no interrupt arrived to time");
    return;
  }
  TEST_EQUAL(across + after - THE_TWO_NOPS, 20);
  /* And the quarter the acknowledge's M1 ends on, which is where the Gate
     Array hears it and where ch. 27.7.1's race in gate_array.c is run. No
     duration fixes it: a cycle can be moved within its microsecond and
     leave every length above unchanged. */
  TEST_EQUAL(heard_on, 1);
  /* Heard there, and not where IORQ began: INT drops on that very cycle. */
  TEST_EQUAL(withdrawn_at, m1_ended_at);

  /* And the other half of the chapter's comparison, which does agree: an
     RST #38 reached from code, looping on itself so that every iteration is
     a settled one. */
  power_on();
  for (int at = UNDER_TEST; at < UNDER_TEST + 64; at++) {
    lower_rom[at] = 0xFF; /* RST #38, each returning into the next */
  }
  lower_rom[0x38] = 0xC9; /* RET, straight back */
  cpc.cpu.pc = UNDER_TEST;
  cpc.cpu.sp = 0x8000;
  cpc.cpu.iff1 = false;
  long rst = 0;
  long ret = 0;
  long settled = 0;
  int taken = 0;
  for (long tick = 0; tick < 4000 && taken < 6; tick++) {
    cpc_tick(&cpc);
    if (!z80_instruction_complete(&cpc.cpu)) {
      continue;
    }
    long step = tick + 1 - settled;
    settled = tick + 1;
    if (++taken > 2) { /* the first pair is entered mid-stride */
      if (taken % 2 == 1) {
        rst = step;
      } else {
        ret = step;
      }
    }
  }
  TEST_EQUAL(rst, 16); /* four microseconds, as ch. 27.4 has it */
  TEST_EQUAL(ret, 12);
}

static void every_instruction_takes_whole_microseconds(void) {
  check(repeated, sizeof repeated / sizeof repeated[0], REPEATED);
}

static void an_instruction_looping_on_itself_costs_the_same(void) {
  check(self_looping, sizeof self_looping / sizeof self_looping[0], SELF_LOOPING);
}

int main(void) {
  TEST_RUN(every_instruction_takes_whole_microseconds);
  TEST_RUN(an_io_cycle_falls_where_its_instruction_puts_it);
  TEST_RUN(an_outi_keeps_a_type_1_a_microsecond_longer_than_the_rest);
  TEST_RUN(an_interrupt_costs_five_microseconds_where_an_rst_costs_four);
  TEST_RUN(an_instruction_looping_on_itself_costs_the_same);
  return TEST_REPORT("cpc timing");
}
