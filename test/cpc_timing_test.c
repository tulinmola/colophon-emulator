/*
 * cpc_timing_test — how long each instruction takes on a CPC, in
 * microseconds, where inside one its write reaches the CRTC, the picture or
 * the video mode, and what taking an interrupt costs.
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
 * are set here against the microsecond the Compendium puts each on, with
 * three memory writes against the video fetch they meet and a mode written
 * against the line sync that takes it.
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
   The two ASICs take the OUT's write a character later, on its 4th
   microsecond, and the OUTI's on its 5th as the rest do. */
static void an_io_cycle_falls_where_its_instruction_puts_it(void) {
  static const struct {
    const char *mnemonic;
    uint8_t type;
    uint8_t opcodes[2];
    uint8_t b; /* OUTI puts B on the bus already decremented, so both address &BD00 */
    uint8_t characters_after_the_fetch;
  } cases[] = {
      {"OUT (C),C", 0, {0xED, 0x49}, 0xBD, 2}, /* the 3rd microsecond */
      {"OUTI", 0, {0xED, 0xA3}, 0xBE, 4},      /* and the 5th */
      {"OUT (C),C", 3, {0xED, 0x49}, 0xBD, 3}, /* the 4th, behind an ASIC */
      {"OUTI", 3, {0xED, 0xA3}, 0xBE, 4},      /* and still the 5th */
      {"OUT (C),C", 4, {0xED, 0x49}, 0xBD, 3}, {"OUTI", 4, {0xED, 0xA3}, 0xBE, 4},
  };
  for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
    memset(ram, 0, sizeof ram);
    memset(lower_rom, 0, sizeof lower_rom);
    cpc_init(&cpc, ram, sizeof ram, lower_rom, cases[index].type);
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
      TEST_FAIL("%s reached a type %u CRTC %d characters after its fetch, the Compendium says %d",
                cases[index].mnemonic, cases[index].type, landed - fetch,
                cases[index].characters_after_the_fetch);
    }
  }
}

/* Where a memory write meets the picture. The Gate Array reads the two
   bytes of each character at the address the CRTC hands over, and a byte the
   processor writes there is seen or not according to which of the two comes
   first in the microsecond. Ch. 8 draws three instructions begun at C0vs 00
   against the video pointer, a byte at a time, and says "the RAM reading by
   the GATE ARRAY/ASIC for data modified by the Z80A is the same for all
   CPC's": LD (HL),reg8 with reg8=#FF shows nothing new at pointers 0 and 1
   and #FF at 2 and 3 (8.1); LD (aaaa),HL with H=#FF and L=#55 shows 00 00
   at aaaa=4 and 5, 55 00 at 6 and 55 FF at 7 (8.2); PUSH DE with D=#FF and
   E=#55 shows, at SP-2 and SP-1, 00 00 at SP=4, 00 FF at 5, 6 and 7, and
   55 FF at 8 (8.3). The pointer is counted from the address handed over on
   the character the opcode fetch falls on, as the test above counts the
   CRTC's characters, and the instruction is reached through a run of NOPs
   rather than straight from reset. The three differ in where a write's WR
   first falls against the character clock: LD (HL),r's on the clock
   itself, the other two's before it, under the Gate Array's wait. */
#define POINTERS_WATCHED 12
#define NOT_FETCHED 0x100

static uint16_t fetches_from_the_opcode_fetch(uint8_t type, const uint8_t *opcodes, size_t length,
                                              uint16_t hl, uint16_t sp,
                                              int seen[POINTERS_WATCHED]) {
  memset(ram, 0, sizeof ram);
  memset(lower_rom, 0, sizeof lower_rom);
  cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
  crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0));
  crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 63));
  memcpy(lower_rom + UNDER_TEST, opcodes, length);
  cpc.cpu.pc = UNDER_TEST - 8;
  cpc.cpu.sp = sp;
  cpc.cpu.a = 0xFF;
  cpc.cpu.h = (uint8_t)(hl >> 8);
  cpc.cpu.l = (uint8_t)hl;
  cpc.cpu.d = 0xFF;
  cpc.cpu.e = 0x55;
  for (int pointer = 0; pointer < POINTERS_WATCHED; pointer++) {
    seen[pointer] = NOT_FETCHED;
  }
  int at_the_fetch = -1;
  for (int tick = 0; tick < 200; tick++) {
    cpc_tick(&cpc);
    if (at_the_fetch < 0 && (cpc.pins & (Z80_M1 | Z80_MREQ)) == (Z80_M1 | Z80_MREQ) &&
        z80_address(cpc.pins) == UNDER_TEST) {
      at_the_fetch = cpc_video_address(&cpc);
    }
    if (at_the_fetch < 0 || !gate_array_character_clock(&cpc.gate_array)) {
      continue;
    }
    for (int half = 0; half < 2; half++) {
      int pointer = cpc_video_address(&cpc) + half - at_the_fetch;
      if (pointer >= 0 && pointer < POINTERS_WATCHED) {
        seen[pointer] = cpc.gate_array.latched_bytes[half];
      }
    }
  }
  return (uint16_t)at_the_fetch;
}

static void a_memory_write_reaches_the_picture_where_its_instruction_puts_it(void) {
  static const uint8_t nop[] = {0x00};
  static const struct {
    uint8_t pointer;
    uint8_t first;
  } ld_hl[] = {{0, 0x00}, {1, 0x00}, {2, 0xFF}, {3, 0xFF}};
  static const struct {
    uint8_t pointer;
    uint8_t first;
    uint8_t second;
  } ld_nn[] = {{4, 0x00, 0x00}, {5, 0x00, 0x00}, {6, 0x55, 0x00}, {7, 0x55, 0xFF}},
    push[] = {{4, 0x00, 0x00}, {5, 0x00, 0xFF}, {6, 0x00, 0xFF}, {7, 0x00, 0xFF}, {8, 0x55, 0xFF}};
  int seen[POINTERS_WATCHED];
  for (uint8_t type = 0; type <= 4; type++) {
    uint16_t base = fetches_from_the_opcode_fetch(type, nop, sizeof nop, 0x9000, 0x8000, seen);
    for (size_t index = 0; index < sizeof ld_hl / sizeof ld_hl[0]; index++) {
      static const uint8_t ld_hl_a[] = {0x77};
      fetches_from_the_opcode_fetch(type, ld_hl_a, sizeof ld_hl_a,
                                    (uint16_t)(base + ld_hl[index].pointer), 0x8000, seen);
      if (seen[ld_hl[index].pointer] != ld_hl[index].first) {
        TEST_FAIL("type %u: LD (HL),A at pointer %u showed %02X, ch. 8.1 shows %02X", type,
                  ld_hl[index].pointer, seen[ld_hl[index].pointer], ld_hl[index].first);
      }
    }
    for (size_t index = 0; index < sizeof ld_nn / sizeof ld_nn[0]; index++) {
      uint16_t aaaa = (uint16_t)(base + ld_nn[index].pointer);
      const uint8_t ld_nn_hl[] = {0x22, (uint8_t)aaaa, (uint8_t)(aaaa >> 8)};
      fetches_from_the_opcode_fetch(type, ld_nn_hl, sizeof ld_nn_hl, 0xFF55, 0x8000, seen);
      if (seen[ld_nn[index].pointer] != ld_nn[index].first ||
          seen[ld_nn[index].pointer + 1] != ld_nn[index].second) {
        TEST_FAIL("type %u: LD (aaaa),HL at pointer %u showed %02X %02X, ch. 8.2 shows %02X %02X",
                  type, ld_nn[index].pointer, seen[ld_nn[index].pointer],
                  seen[ld_nn[index].pointer + 1], ld_nn[index].first, ld_nn[index].second);
      }
    }
    for (size_t index = 0; index < sizeof push / sizeof push[0]; index++) {
      static const uint8_t push_de[] = {0xD5};
      fetches_from_the_opcode_fetch(type, push_de, sizeof push_de, 0x9000,
                                    (uint16_t)(base + push[index].pointer), seen);
      if (seen[push[index].pointer - 2] != push[index].first ||
          seen[push[index].pointer - 1] != push[index].second) {
        TEST_FAIL(
            "type %u: PUSH DE with SP at pointer %u showed %02X %02X, ch. 8.3 shows %02X %02X",
            type, push[index].pointer, seen[push[index].pointer - 2], seen[push[index].pointer - 1],
            push[index].first, push[index].second);
      }
    }
  }
}

/* Where a mode an OUT writes is taken: ch. 9.3.2's diagram walked, R2=46 and
   R3=14 with mode 2 in force and an OUT(C),r8 asking for mode 0 begun at
   each C0vs from 43 to 52, its write "effective on the 3rd μsecond" (ch.
   9.3.1). On types 0, 1 and 2 those begun up to 49 change the line's mode
   and the rest leave mode 2 standing until the next sync; on the two ASICs
   one more is taken (ch. 9.3.3), their syncs reaching the Gate Array a
   character late. Read at C0=61, after the sync. */
#define OUT_AT 0x0200

static void a_mode_written_by_an_out_is_taken_where_ch9_draws_it(void) {
  for (uint8_t type = 0; type <= 4; type++) {
    int at_the_fetch = -1;
    for (int start = 43; start <= 52; start++) {
      int nops = 8;
      if (at_the_fetch >= 0) {
        nops = 8 + (start - at_the_fetch + 64) % 64;
      }
      memset(ram, 0, sizeof ram);
      memset(lower_rom, 0, sizeof lower_rom);
      cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
      static const uint8_t standard[][2] = {{0, 63}, {1, 40}, {2, 46}, {3, 0x8E}, {7, 30}};
      for (size_t index = 0; index < sizeof standard / sizeof standard[0]; index++) {
        crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, standard[index][0]));
        crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, standard[index][1]));
      }
      gate_array_write(&cpc.gate_array, 0x82); /* RMR: mode 2, the ROMs left on */
      cpc.gate_array.mode = 2;
      lower_rom[OUT_AT] = 0xED; /* OUT (C),C */
      lower_rom[OUT_AT + 1] = 0x49;
      cpc.cpu.pc = (uint16_t)(OUT_AT - nops);
      cpc.cpu.b = 0x7F;
      cpc.cpu.c = 0x80; /* RMR: mode 0 */
      int fetched_on = -1;
      int read = -1;
      for (int tick = 0; tick < 2000 && read < 0; tick++) {
        cpc_tick(&cpc);
        if (fetched_on < 0 && (cpc.pins & (Z80_M1 | Z80_MREQ)) == (Z80_M1 | Z80_MREQ) &&
            z80_address(cpc.pins) == OUT_AT) {
          fetched_on = cpc.crtc.c0;
        }
        if (fetched_on >= 0 && cpc.crtc.c0 == 61 && gate_array_character_clock(&cpc.gate_array)) {
          read = cpc.gate_array.mode;
        }
      }
      if (at_the_fetch < 0) {
        at_the_fetch = fetched_on;
        start--; /* the first pass only finds where eight NOPs put the fetch */
        continue;
      }
      if (fetched_on != start) {
        TEST_FAIL("type %u: the OUT was fetched on C0=%d, not %d", type, fetched_on, start);
        continue;
      }
      int last_taken = type >= 3 ? 50 : 49;
      int expected = start <= last_taken ? 0 : 2;
      if (read != expected) {
        TEST_FAIL("type %u: an OUT begun at C0vs %d left mode %d after the sync, ch. 9.3 has %d",
                  type, start, read, expected);
      }
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
   absolute lead, which is what the chapter states and what survives a
   change of instruction lengths. What comes out is not the chapter's two
   microseconds, though, and the reason is worth having in one place: the
   chip offers a window in which a write can still cancel a line's ending,
   the board reports a write finishing on the character clock, and behind a
   Gate Array only the OUTI's entry lands on that clock — the OUT's falls a
   quarter later. So there the OUT is never rescued where the OUTI is, and
   the two stand one microsecond apart on the types that keep one character
   of window and level on the type that keeps two. Which instruction reaches the
   window is a Z80 and Gate Array fact; the window itself is the chip's.

   What still holds of the chapter is the distance between the
   instructions' own entries, the OUT's on its 3rd microsecond, its 4th
   behind an ASIC, and the OUTI's on its 5th, which the test above this one
   pins where they fall.
   And the relation ch. 13.6 is really about survives whole: a type 1's
   OUTI reaches one placement beyond every other chip's, which is the
   difference the two cross-type assertions below measure.

   The walk watches three characters past the wrap rather than stopping at
   it. A rescue from the character the ending was decided on shows up as C0
   stepping from #3F to #40 on the wrap's own tick; a rescue from the
   character after it, which a type 1 alone takes, only shows up a
   microsecond later, and a walk that stopped at the wrap could not see it
   at all. This one could not, for as long as it existed, and reported a
   type 1's window as one character wide while the chip gave it two.

   Types 3 and 4 are walked against ch. 13.6.3, whose chronogram takes both
   instructions' writes on the line's last character: an OUT started at #3C,
   its write on its 4th microsecond, and an OUTI at #3B, on its 5th. The OUT
   comes out as drawn, a microsecond earlier than ch. 13.6.1 draws a type 0's:
   its write, held to the next character clock, lands inside the microsecond
   after it and past the take-back window (cpc.c). The OUTI keeps the
   placement it has on a type 0, the window's microsecond more than either
   chapter draws, so the two stand level there where the chapter has them a
   microsecond apart.

   The leads are counted in microseconds from the start of the code under
   test and straddle the end of a 64-character line, which falls within
   them: a placement below the window is too early for the walk to mean
   anything, so the edge found is checked to be inside it and not at its
   floor. */
/* The fact the walk below rests on, checked directly rather than inferred
   from the difference it produces: an OUTI's entry to the CRTC falls on the
   quarter the board calls the character clock and an OUT (C),r's does not.
   That is a Z80 and Gate Array fact — where each instruction's I/O cycle
   begins against the released WAIT — and it is what decides which of the two
   can reach the window a chip holds a line's ending open in. Behind an ASIC
   both are taken on it, and that is the board's own choice rather than the
   documentation's: ch. 13.6.3 fixes only the microsecond each lands in, the
   last OUT in time starting at #3C and the last OUTI at #3B. An OUT held to
   that clock lands after its tick, past the window, and an OUTI begun on it
   reaches the window as it does behind a Gate Array (cpc.c). Ch. 4.4.4's
   diagram C draws the ASIC's sampling window straddling each edge and names
   no instant inside it. */
static void an_outi_enters_on_the_character_clock_and_an_out_only_behind_an_asic(void) {
  static const struct {
    uint8_t type;
    uint8_t opcode; /* after the ED prefix */
    uint8_t b;      /* so both reach &BD00 */
    bool on_the_clock;
  } instructions[] = {{0, 0x49, 0xBD, false}, {0, 0xA3, 0xBE, true}, {3, 0x49, 0xBD, true},
                      {3, 0xA3, 0xBE, true},  {4, 0x49, 0xBD, true}, {4, 0xA3, 0xBE, true}};
  for (unsigned index = 0; index < sizeof instructions / sizeof *instructions; index++) {
    memset(ram, 0, sizeof ram);
    memset(lower_rom, 0, sizeof lower_rom);
    cpc_init(&cpc, ram, sizeof ram, lower_rom, instructions[index].type);
    crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0)); /* R0 selected */
    lower_rom[UNDER_TEST] = 0xED;
    lower_rom[UNDER_TEST + 1] = instructions[index].opcode;
    ram[0x9000] = 0x2A; /* the width the write hands over */
    cpc.cpu.pc = UNDER_TEST;
    cpc.cpu.b = instructions[index].b;
    cpc.cpu.c = 0x2A;
    cpc.cpu.h = 0x90;
    cpc.cpu.sp = 0x8000;
    int quarter = -1;
    for (int tick = 0; tick < 4 * 12 && quarter < 0; tick++) {
      uint8_t before = cpc.crtc.registers[0];
      cpc_tick(&cpc);
      if (cpc.crtc.registers[0] != before) {
        /* The quarter as the write saw it: cpc_tick advances the phase before
           it steps anything, so this is the value the pin was decided on. */
        quarter = cpc.gate_array.cpu_phase;
      }
    }
    TEST_CHECK(quarter >= 0);
    TEST_EQUAL(quarter == 0, instructions[index].on_the_clock);
  }
}

static const int earliest_placement = 54;
static const int latest_placement = 70;

static void an_outi_keeps_a_type_1_a_microsecond_longer_than_the_rest(void) {
  static const uint8_t types[] = {0, 1, 2, 3, 4};
  int outi_edge[5] = {-1, -1, -1, -1, -1};
  int out_edge[5] = {-1, -1, -1, -1, -1};
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
        bool wrapped = false;
        int characters_since_the_wrap = 0;
        uint8_t previous = cpc.crtc.c0;
        for (int tick = 0; tick < 4 * 90 && !ran_on; tick++) {
          cpc_tick(&cpc);
          if (previous == 0x3F && cpc.crtc.c0 == 0x40) {
            ran_on = true;
          }
          if (previous == 0x3F && cpc.crtc.c0 == 0) {
            wrapped = true;
          }
          if (wrapped && cpc.crtc.c0 != previous) {
            /* The watch does not stop at the wrap. A write reaching the chip
               from a character after it puts C0 back above the width the line
               ended on, where a wrap that stands leaves C0 climbing from
               nothing — so three characters of watching tell the two apart,
               and stopping at the wrap would have seen only the first of
               them. */
            if (cpc.crtc.c0 > 0x3F) {
              ran_on = true;
            }
            if (++characters_since_the_wrap > 3) {
              break;
            }
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
    /* One microsecond, not the chapter's two: the chip offers the window and
       the Z80 decides which instruction reaches it, the OUTI's entry landing
       on the character clock the board reports and the OUT's a quarter
       later, so behind a Gate Array the OUT is never rescued where the OUTI
       is. The distance between the instructions' own entries is untouched
       and the test above this one pins it. Behind an ASIC the two stand
       level: the OUT's write lands inside the microsecond after the clock
       that takes it, past the window (cpc.c), and the OUTI's keeps the
       window, which gives it a microsecond more than ch. 13.6.3 draws as it
       does on every type. */
    TEST_EQUAL(edges[0] - edges[1], type == 0 || type == 2 ? 1 : 0);
    out_edge[index] = edges[0];
    outi_edge[index] = edges[1];
  }
  /* And the relation the chapter is really about, which survives intact:
     ch. 13.6.2 holds the OUTI's placements one row longer than ch. 13.6.1
     does and leaves the OUT's where they are, so it is the OUTI that gains
     the microsecond on a type 1 and not the OUT that loses one. A change
     that moved both windows together would keep every difference above. */
  TEST_EQUAL(outi_edge[1] - outi_edge[0], 1);
  TEST_EQUAL(out_edge[1], out_edge[0]);
  TEST_EQUAL(outi_edge[2], outi_edge[0]);
  TEST_EQUAL(out_edge[2], out_edge[0]);
  /* And ch. 13.6.3 beside ch. 13.6.1: behind an ASIC the OUTI's placements
     are a type 0's, and the OUT's last in time starts a microsecond sooner,
     at #3C where a type 0's starts at #3D. */
  TEST_EQUAL(outi_edge[3], outi_edge[0]);
  TEST_EQUAL(outi_edge[4], outi_edge[0]);
  TEST_EQUAL(out_edge[3], out_edge[0] - 1);
  TEST_EQUAL(out_edge[4], out_edge[3]);
}

/* Ch. 13.6.3's "Previous R0=0" row beside ch. 13.6.1's: an OUT writing R0 on
   a line of one character lets C0 leave 0 a microsecond later behind an ASIC
   than behind a Gate Array, on its fifth microsecond rather than its fourth.
   The same instruction at the same place, so the microsecond is the board's
   alone; Shaker's B (P) starts its OUTs a microsecond early on the ASICs to
   make up for it, and Shakerland photographs the R4 its search finds there. */
static void an_asic_takes_an_out_widening_r0_from_nothing_a_microsecond_late(void) {
  long leaves_nothing_on[5] = {-1, -1, -1, -1, -1};
  for (uint8_t type = 0; type <= 4; type++) {
    memset(ram, 0, sizeof ram);
    memset(lower_rom, 0, sizeof lower_rom); /* NOPs in front of it */
    cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
    crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0)); /* R0 selected, and 0 */
    lower_rom[UNDER_TEST + 20] = 0xED;
    lower_rom[UNDER_TEST + 21] = 0x49; /* OUT (C),C */
    cpc.cpu.pc = UNDER_TEST;
    cpc.cpu.b = 0xBD;
    cpc.cpu.c = 0x10;
    cpc.cpu.sp = 0x8000;
    for (long tick = 0; tick < 4L * 60 && leaves_nothing_on[type] < 0; tick++) {
      cpc_tick(&cpc);
      if (cpc.crtc.c0 != 0) {
        leaves_nothing_on[type] = tick;
      }
    }
    TEST_CHECK(leaves_nothing_on[type] >= 0);
  }
  for (uint8_t type = 1; type <= 4; type++) {
    TEST_EQUAL(leaves_nothing_on[type] - leaves_nothing_on[0], type >= 3 ? 4 : 0);
  }
}

/* An OUT lowering R0 onto the character it writes on ends the line there
   behind an ASIC as behind a Gate Array, and one writing on the character
   after runs the counter the long way round behind a Gate Array and ends the
   line where it lands behind an ASIC, whose counter "never exceeds the value
   of R0" (ch. 13.6.3) — ours where the width is written under it, the
   chapter's diagrams widening R0 only. Shaker's B (R) writes R0=1 with an
   OUT on C0=1 on every type, its build for the ASICs a microsecond shorter to
   make up for their later interrupt, and Shakerland photographs that line
   ending there on both ASICs: "R0=1/R3=1/R52 CALC=#34 (Exp:#34)(REF #0169)",
   the other types' #0168 and the microsecond ch. 27.6.1 gives their
   interrupt. The board holds the OUT to the next character clock and hands a
   width lowered so to the chip ahead of the comparison made there (cpc.c).
   The width is lowered mid-line here, clear of what a type 0 makes of a line
   of one or two characters. */
static void an_out_lowering_r0_onto_its_own_character_ends_the_line_there(void) {
  static const uint8_t width = 0x30;
  for (uint8_t type = 0; type <= 4; type++) {
    bool seen_on_its_character = false;
    bool seen_on_the_one_after = false;
    for (int lead = 20; lead <= 70; lead++) {
      memset(ram, 0, sizeof ram);
      memset(lower_rom, 0, sizeof lower_rom); /* NOPs in front of it */
      cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
      crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0));
      crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, 0x3F));
      crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 0)); /* R0 stays selected */
      lower_rom[UNDER_TEST + lead] = 0xED;
      lower_rom[UNDER_TEST + lead + 1] = 0x49; /* OUT (C),C */
      cpc.cpu.pc = UNDER_TEST;
      cpc.cpu.b = 0xBD;
      cpc.cpu.c = width;
      cpc.cpu.sp = 0x8000;
      int written_on = -1;
      bool ended_there = false;
      bool ended_a_character_late = false;
      bool ran_past = false;
      uint8_t previous = cpc.crtc.c0;
      for (int tick = 0; tick < 4 * 140 && !ended_there && !ended_a_character_late && !ran_past;
           tick++) {
        uint64_t pins = cpc_tick(&cpc);
        if (written_on < 0 && (pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
          written_on = cpc.crtc.c0; /* the character the processor wrote on */
        }
        if (written_on >= 0 && previous == width && cpc.crtc.c0 == 0) {
          ended_there = true;
        }
        if (written_on >= 0 && previous == width + 1 && cpc.crtc.c0 == 0) {
          ended_a_character_late = true;
        }
        if (written_on >= 0 && cpc.crtc.c0 > width + 1) {
          ran_past = true;
        }
        previous = cpc.crtc.c0;
      }
      if (written_on == width) {
        seen_on_its_character = true;
        TEST_CHECK(ended_there);
      }
      if (written_on == width + 1) {
        seen_on_the_one_after = true;
        TEST_CHECK(type >= 3 ? ended_a_character_late : ran_past);
      }
    }
    TEST_CHECK(seen_on_its_character);
    TEST_CHECK(seen_on_the_one_after);
  }
}

/* An OUT writing R1 onto the character it writes on borders that character
   behind a Gate Array: "the condition C0=R1 is considered immediately on a
   line" (ch. 17.3), and ch. 17.5.1 draws an R1 of 0 written on C0=0 "just in
   time" for it, on types 0, 1 and 2. The character's display enable left the
   chip at its tick, and the Gate Array holds it a microsecond before drawing,
   where the board shuts it (cpc.c). An ASIC takes the OUT a microsecond later
   and no equality from it (ch. 17.5.2): the board holds the write to the next
   character clock, and the character it lands on is drawn even where C0
   stands on the R1 it writes. */
static void an_out_writing_r1_onto_its_own_character_borders_it(void) {
  static const uint8_t values[10] = {63, 63, 46, 0x8E, 38, 0, 25, 30, 0, 7};
  static const uint8_t width = 0x30;
  for (uint8_t type = 0; type <= 4; type++) {
    bool seen = false;
    for (int lead = 20; lead <= 70 && !seen; lead++) {
      memset(ram, 0, sizeof ram);
      memset(lower_rom, 0, sizeof lower_rom); /* NOPs in front of it */
      cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
      for (size_t reg = 0; reg < sizeof values; reg++) {
        crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, (uint8_t)reg));
        crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, values[reg]));
      }
      crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 1)); /* R1 stays selected */
      lower_rom[UNDER_TEST + lead] = 0xED;
      lower_rom[UNDER_TEST + lead + 1] = 0x49; /* OUT (C),C */
      cpc.cpu.pc = UNDER_TEST;
      cpc.cpu.b = 0xBD;
      cpc.cpu.c = width;
      cpc.cpu.sp = 0x8000;
      for (int tick = 0; tick < 4 * 140; tick++) {
        bool held = cpc.crtc_write_awaits_the_clock;
        uint64_t pins = cpc_tick(&cpc);
        if (type <= 2 && (pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
          if (cpc.crtc.c0 == width) {
            seen = true;
            TEST_EQUAL(cpc.crtc.registers[1], width);
            TEST_CHECK(!cpc.gate_array.latched_display[0]);
            TEST_CHECK(!cpc.gate_array.latched_display[1]);
          }
          break;
        }
        /* Behind an ASIC the write is held at the cycle's first tick and
           lands at the clock after, which is where the character is read. */
        if (type >= 3 && held && !cpc.crtc_write_awaits_the_clock) {
          if (cpc.crtc.c0 == width) {
            seen = true;
            TEST_EQUAL(cpc.crtc.registers[1], width);
            TEST_CHECK(cpc.gate_array.latched_display[0]);
            TEST_CHECK(cpc.gate_array.latched_display[1]);
          }
          break;
        }
      }
    }
    TEST_CHECK(seen);
  }
}

/* Every other register an ASIC's OUT writes keeps the microsecond ch. 4.4.4
   gives it, "on the 3rd NOP for a CRTC equipped with a GATE ARRAY, and on the
   4th NOP for an ASIC that emulates a CRTC": an OUT whose write the processor
   makes on a line's last character reaches an ASIC on the next line's first,
   too late for the end of the line it was made on, which is what Shaker's E
   (4) grades on both ASIC records with R4 and R9 written at a line's end
   (crtc.h). R9 written there with the value C9 holds would end the row on
   that line; behind an ASIC the row runs a line more and ends where C9 has
   passed R9, these two reading R9 reached or passed: "If R9 is changed with
   a value less than or equal to C9, then C9 changes to 0 on the next line"
   (ch. 10.3.4.1). */
static void an_asic_takes_an_r9_written_on_a_lines_last_character_on_the_next(void) {
  static const uint8_t row_line = 3;
  for (uint8_t type = 3; type <= 4; type++) {
    bool seen = false;
    for (int lead = 200; lead <= 280 && !seen; lead++) {
      memset(ram, 0, sizeof ram);
      memset(lower_rom, 0, sizeof lower_rom); /* NOPs in front of it */
      cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
      static const uint8_t setup[][2] = {{0, 0x3F}, {4, 0x26}, {9, 7}};
      for (unsigned index = 0; index < sizeof setup / sizeof *setup; index++) {
        crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, setup[index][0]));
        crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, setup[index][1]));
      }
      crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, 9)); /* R9 stays selected */
      lower_rom[UNDER_TEST + lead] = 0xED;
      lower_rom[UNDER_TEST + lead + 1] = 0x49; /* OUT (C),C */
      cpc.cpu.pc = UNDER_TEST;
      cpc.cpu.b = 0xBD;
      cpc.cpu.c = row_line;
      cpc.cpu.sp = 0x8000;
      int written_on = -1;
      int written_on_line = -1;
      int lines_after[2] = {-1, -1};
      int lines_seen = 0;
      uint8_t previous = cpc.crtc.c0;
      for (int tick = 0; tick < 4 * 450 && lines_seen < 2; tick++) {
        uint64_t pins = cpc_tick(&cpc);
        if (written_on < 0 && (pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
          written_on = cpc.crtc.c0;
          written_on_line = cpc.crtc.c9;
        }
        if (written_on >= 0 && previous == 0x3F && cpc.crtc.c0 == 0) {
          lines_after[lines_seen++] = cpc.crtc.c9;
        }
        previous = cpc.crtc.c0;
      }
      if (written_on == 0x3F && written_on_line == row_line) {
        seen = true;
        TEST_EQUAL(lines_after[0], row_line + 1); /* too late for the line it was made on */
        TEST_EQUAL(lines_after[1], 0);            /* and in time for the next */
      }
    }
    TEST_CHECK(seen);
  }
}

/* An R2 or R3 an ASIC's OUT writes is taken on the character it lands on, the
   board handing it to the chip ahead of that clock's comparison (cpc.c): ch.
   14.9's CRTC 4 schematic draws "R2-JIT / OUT(C),r8" with the sync starting on
   C0=R2, and ch. 14.5.3's "OUT CRTC-R3,5", landing on the character C3l
   reaches 5 on, ends a sync five characters long. An R2 of #20 written on the
   character before #20 starts the line's sync there, and an R3 of 5 written
   on the fifth character of a sync begun at #20 ends it on the sixth. */
static void an_asic_takes_an_out_moving_its_line_sync_on_the_character_it_lands_on(void) {
  static const struct {
    uint8_t number;
    uint8_t value;
    uint8_t r2, r3;     /* standing before the write */
    uint8_t written_on; /* the C0 the processor writes on */
    uint8_t rises_on;   /* the line's sync: the first character it is up on */
    uint8_t falls_on;   /* and the first after that it is down on */
  } cases[] = {{2, 0x20, 0x30, 0x8E, 0x1F, 0x20, 0x2E}, {3, 0x85, 0x20, 0x8F, 0x24, 0x20, 0x25}};
  for (uint8_t type = 3; type <= 4; type++) {
    for (size_t index = 0; index < sizeof cases / sizeof *cases; index++) {
      bool seen = false;
      for (int lead = 20; lead <= 100 && !seen; lead++) {
        memset(ram, 0, sizeof ram);
        memset(lower_rom, 0, sizeof lower_rom); /* NOPs in front of it */
        cpc_init(&cpc, ram, sizeof ram, lower_rom, type);
        const uint8_t setup[][2] = {{0, 0x3F}, {2, cases[index].r2}, {3, cases[index].r3}};
        for (unsigned row = 0; row < sizeof setup / sizeof *setup; row++) {
          crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, setup[row][0]));
          crtc_access(&cpc.crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, setup[row][1]));
        }
        crtc_access(&cpc.crtc, CRTC_CS | crtc_set_data(0, cases[index].number));
        lower_rom[UNDER_TEST + lead] = 0xED;
        lower_rom[UNDER_TEST + lead + 1] = 0x49; /* OUT (C),C */
        cpc.cpu.pc = UNDER_TEST;
        cpc.cpu.b = 0xBD;
        cpc.cpu.c = cases[index].value;
        cpc.cpu.sp = 0x8000;
        int written_on = -1;
        int rises_on = -1;
        int falls_on = -1;
        for (int tick = 0; tick < 4 * 250; tick++) {
          uint64_t pins = cpc_tick(&cpc);
          bool on_the_clock = gate_array_character_clock(&cpc.gate_array);
          if (written_on < 0 && (pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
            written_on = cpc.crtc.c0;
          }
          if (!on_the_clock) {
            continue;
          }
          if (cpc.crtc.c0 == 0) {
            if (written_on >= 0) {
              break;
            }
            rises_on = falls_on = -1;
          }
          bool hsync = (cpc.crtc_pins & CRTC_HSYNC) != 0;
          if (hsync && rises_on < 0) {
            rises_on = cpc.crtc.c0;
          }
          if (!hsync && rises_on >= 0 && falls_on < 0) {
            falls_on = cpc.crtc.c0;
          }
        }
        if (written_on == cases[index].written_on) {
          seen = true;
          TEST_EQUAL(rises_on, cases[index].rises_on);
          TEST_EQUAL(falls_on, cases[index].falls_on);
        }
      }
      TEST_CHECK(seen);
    }
  }
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
  TEST_RUN(a_memory_write_reaches_the_picture_where_its_instruction_puts_it);
  TEST_RUN(a_mode_written_by_an_out_is_taken_where_ch9_draws_it);
  TEST_RUN(an_outi_enters_on_the_character_clock_and_an_out_only_behind_an_asic);
  TEST_RUN(an_outi_keeps_a_type_1_a_microsecond_longer_than_the_rest);
  TEST_RUN(an_asic_takes_an_out_widening_r0_from_nothing_a_microsecond_late);
  TEST_RUN(an_out_lowering_r0_onto_its_own_character_ends_the_line_there);
  TEST_RUN(an_out_writing_r1_onto_its_own_character_borders_it);
  TEST_RUN(an_asic_takes_an_r9_written_on_a_lines_last_character_on_the_next);
  TEST_RUN(an_asic_takes_an_out_moving_its_line_sync_on_the_character_it_lands_on);
  TEST_RUN(an_interrupt_costs_five_microseconds_where_an_rst_costs_four);
  TEST_RUN(an_instruction_looping_on_itself_costs_the_same);
  return TEST_REPORT("cpc timing");
}
