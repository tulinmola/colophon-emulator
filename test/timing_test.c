/*
 * timing_test — how long each instruction takes on a CPC, in microseconds.
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
 * Sources:
 * - "Timings" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/instrtim.html — "The following table
 *   gives the complete execution time for all CPU instructions. These
 *   timings have been measured."
 * - "The Amstrad CPC CRTC Compendium" v1.10 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.10-EN.pdf ch. 26, "Duration of
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
static const timing timings[] = {
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
};

static void power_on(void) {
  memset(ram, 0, sizeof ram);
  memset(lower_rom, 0, sizeof lower_rom); /* NOPs everywhere */
  cpc_init(&cpc, ram, sizeof ram, lower_rom);
}

/* An instruction is measured by repeating it and timing the steady state,
   never by running one alone. The Gate Array's alignment padding falls on
   whichever cycle asks for the bus next, so a lone instruction hands part
   of its cost to its successor: INC BC after a NOP takes six T-states, but
   INC BC after INC BC takes eight, and eight is what the tables record.
   The first copies are run to settle, the rest are timed. */
#define SETTLING_COPIES 3
#define TIMED_COPIES 8

static int measure(const timing *entry, bool branch) {
  power_on();
  for (int copy = 0; copy < SETTLING_COPIES + TIMED_COPIES; copy++) {
    memcpy(lower_rom + UNDER_TEST + (size_t)copy * entry->length, entry->opcodes, entry->length);
  }
  cpc.cpu.pc = UNDER_TEST;
  cpc.cpu.sp = 0x8000;
  cpc.cpu.h = 0x90; /* (HL) points at RAM, clear of the program */
  cpc.cpu.l = 0x00;
  cpc.cpu.ixh = 0x90;
  cpc.cpu.iyh = 0x90;
  cpc.cpu.b = branch ? 2 : 1;    /* DJNZ falls through when B-1 is zero */
  cpc.cpu.c = branch ? 2 : 1;    /* BC drives the block instructions */
  cpc.cpu.f = branch ? 0 : 0x40; /* Z set makes NZ conditions fall through */
  cpc.cpu.d = 0x91;
  cpc.cpu.e = 0x00;

  uint16_t timed_from = (uint16_t)(UNDER_TEST + SETTLING_COPIES * entry->length);
  for (int guard = 0; guard < 4000; guard++) {
    if (z80_instruction_complete(&cpc.cpu) && cpc.cpu.pc == timed_from) {
      break;
    }
    cpc_tick(&cpc);
  }

  int ticks = 0;
  for (int copy = 0; copy < TIMED_COPIES; copy++) {
    for (int guard = 0; guard < 400; guard++) {
      cpc_tick(&cpc);
      ticks++;
      if (z80_instruction_complete(&cpc.cpu)) {
        break;
      }
    }
  }
  if (ticks % (4 * TIMED_COPIES) != 0) {
    TEST_FAIL("%s took %d T-states over %d copies, not a whole number of microseconds each",
              entry->mnemonic, ticks, TIMED_COPIES);
    return -1;
  }
  return ticks / (4 * TIMED_COPIES);
}

static void check(const timing *entries, size_t count, bool branch) {
  for (size_t index = 0; index < count; index++) {
    int measured = measure(&entries[index], branch);
    if (measured != entries[index].microseconds) {
      TEST_FAIL("%s took %dus, both tables say %d", entries[index].mnemonic, measured,
                entries[index].microseconds);
    }
  }
}

static void every_instruction_takes_whole_microseconds(void) {
  check(timings, sizeof timings / sizeof timings[0], false);
}

int main(void) {
  TEST_RUN(every_instruction_takes_whole_microseconds);
  return TEST_REPORT("timing");
}
