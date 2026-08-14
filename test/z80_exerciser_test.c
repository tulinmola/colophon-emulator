/*
 * z80_exerciser_test — runs Frank Cringle's Z80 instruction set exerciser.
 *
 * Where the SingleStepTests corpus checks each instruction alone from a clean
 * state, the exerciser runs a real program: millions of instructions in a row,
 * each inheriting whatever the last one left behind, sweeping operands and
 * flags and checking the results against CRCs recorded on real hardware. It
 * proves nothing about timing, interrupts or I/O — only that the arithmetic
 * survives contact with itself.
 *
 * The programs are CP/M binaries and use two of its console calls, which is
 * the whole of the environment they need.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80.h"

static uint8_t ram[0x10000];

#define BDOS_ENTRY 0x0005
#define PROGRAM_START 0x0100
#define WARM_BOOT 0x0000

static int groups_passed;
static int groups_failed;
static char line[256];
static size_t line_length;

/* The exerciser reports one group per line, ending in OK or ERROR. */
static void console(char character) {
  putchar(character);
  if (character == '\n') {
    line[line_length] = '\0';
    if (strstr(line, "ERROR") != NULL) {
      groups_failed++;
    } else if (strstr(line, "OK") != NULL) {
      groups_passed++;
    }
    line_length = 0;
    fflush(stdout);
  } else if (character != '\r' && line_length + 1 < sizeof line) {
    line[line_length++] = character;
  }
}

/* Function 2 writes the character in E; function 9 writes the $-terminated
   string at DE. */
static void bdos_call(const z80_t *cpu) {
  if (cpu->c == 2) {
    console((char)cpu->e);
  } else if (cpu->c == 9) {
    uint16_t address = (uint16_t)((cpu->d << 8) | cpu->e);
    while (ram[address] != '$') {
      console((char)ram[address++]);
    }
  }
}

static int load_program(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    return 0;
  }
  const size_t length = fread(&ram[PROGRAM_START], 1, sizeof ram - PROGRAM_START, file);
  fclose(file);
  return length > 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <exerciser.com> [group limit]\n", argv[0]);
    return 2;
  }
  const long group_limit = argc > 2 ? strtol(argv[2], NULL, 10) : 0;

  memset(ram, 0, sizeof ram);
  if (!load_program(argv[1])) {
    return 2;
  }
  ram[BDOS_ENTRY] = 0xC9; /* the call returns; we do the work at the boundary */

  z80_t cpu;
  z80_init(&cpu);
  cpu.pc = PROGRAM_START;
  cpu.sp = 0xF000;

  uint64_t pins = 0;
  unsigned long long ticks = 0;
  for (;;) {
    pins = z80_tick(&cpu, pins);
    if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
      pins = z80_set_data(pins, ram[z80_address(pins)]);
    } else if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
      ram[z80_address(pins)] = z80_data(pins);
    }
    ticks++;
    if (!z80_instruction_complete(&cpu)) {
      continue;
    }
    if (cpu.pc == BDOS_ENTRY) {
      bdos_call(&cpu);
    } else if (cpu.pc == WARM_BOOT) {
      break;
    }
    if (group_limit > 0 && groups_passed + groups_failed >= group_limit) {
      break;
    }
  }

  if (line_length > 0) {
    putchar('\n'); /* the exerciser signs off without one */
  }
  printf("%s: %d groups pass, %d fail (%llu T-states, %.0f minutes of a 4 MHz Z80)\n", argv[1],
         groups_passed, groups_failed, ticks, (double)ticks / 4000000.0 / 60.0);
  return groups_failed != 0 || groups_passed == 0;
}
