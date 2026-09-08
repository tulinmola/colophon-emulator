/*
 * spectrum_interrupt_test — the interrupt as the machine delivers it.
 *
 * z80_test grades the processor's half of this: what an acceptance costs,
 * what it pushes, and which of the two flip-flops it clears. None of that
 * says when the interrupt arrives, and on this machine the when is the whole
 * of it. The ULA holds the line low for the first thirty-two T-states of a
 * frame and no longer, and the processor looks at the line only on the last
 * T-state of an instruction — so whether a frame's interrupt is taken at all
 * depends on where the program happened to be standing, and a program that
 * needs its timing to hold synchronises on the interrupt for exactly that
 * reason.
 *
 * What the machine leaves behind is graded here too, because it is what an
 * interrupted program comes back to: the address pushed, the stack pointer,
 * the refresh counter, and both flip-flops.
 *
 * Sources:
 * - libspectrum's timings.c (Philip Kendall, Fuse),
 *   https://sourceforge.net/p/fuse-emulator/libspectrum/ci/master/tree/timings.c
 *   — the interrupt held 32 T-states from the start of a frame.
 * - "The Undocumented Z80 Documented" (Sean Young),
 *   https://raw.githubusercontent.com/floooh/emu-info/master/z80/z80-documented.pdf
 *   — an acceptance is 13 T-states in mode 1 and 19 in mode 2, a
 *   non-maskable one 11; the acknowledge cycle increments R like any other
 *   opcode fetch; a maskable acceptance clears both flip-flops where a
 *   non-maskable one clears IFF1 alone.
 * - "The ZX Spectrum FAQ" (World of Spectrum),
 *   https://worldofspectrum.org/faq/reference/48kreference.htm — nothing on
 *   this board drives the data bus during an acknowledge, so the byte read is
 *   the floating &FF, which is why a mode 2 table on a Spectrum is 257 bytes
 *   and is read at I:&FF.
 */
#include <string.h>

#include "spectrum.h"
#include "test.h"

static uint8_t rom[SPECTRUM_ROM_SIZE];
static uint8_t ram[SPECTRUM_RAM_48K];
static spectrum_t spectrum;

/* In the ROM, clear of the reset vector and of &0038. */
#define UNDER_TEST 0x0100
#define WORKING_SP 0x8000

/* The published numbers, transcribed rather than taken from the machine. */
#define INTERRUPT_HELD_TSTATES 32
#define MODE_1_TSTATES 13
#define MODE_2_TSTATES 19

/* Longer than any instruction and any acceptance together. */
#define MOST_TSTATES 200

static void power_on(void) {
  memset(rom, 0x00, sizeof rom);
  memset(ram, 0x00, sizeof ram);
  spectrum_init(&spectrum, ram, SPECTRUM_RAM_48K, rom);
}

/* What an acceptance left behind, so one run can be asked several questions. */
typedef struct {
  bool taken;
  int acknowledged_at; /* the T-state the acknowledge put IORQ on the bus */
  int tstates;         /* the instruction and the acceptance together */
  uint16_t pc;
  uint16_t sp;
  uint16_t pushed;
  uint8_t r;
  bool iff1;
  bool iff2;
} acceptance;

static uint16_t word_at(uint16_t address) {
  return (uint16_t)(spectrum_peek(&spectrum, address) |
                    (spectrum_peek(&spectrum, (uint16_t)(address + 1)) << 8));
}

/* A machine standing at `at` with the interrupt enabled and one instruction
   in front of it. Left ready to run rather than run, so a test that needs a
   vector table in the RAM can write one first. */
static void start(const uint8_t *program, size_t length, uint8_t mode, uint8_t i, uint32_t at) {
  power_on();
  memcpy(rom + UNDER_TEST, program, length);
  spectrum.cpu.pc = UNDER_TEST;
  spectrum.cpu.sp = WORKING_SP;
  spectrum.cpu.im = mode;
  spectrum.cpu.i = i;
  /* Something the ROM under HL does not hold, so a compare that repeats
     repeats rather than stopping on its first byte. */
  spectrum.cpu.a = 0xFF;
  spectrum.cpu.iff1 = true;
  spectrum.cpu.iff2 = true;
  spectrum.cpu.r = 0;
  ula_seek(&spectrum.ula, at);
}

/* Runs what is standing there, and whatever acceptance follows it. */
static acceptance run(void) {
  acceptance answer = {false, -1, 0, 0, 0, 0, 0, false, false};
  for (; answer.tstates < MOST_TSTATES; answer.tstates++) {
    const uint32_t now = spectrum.ula.frame_tick;
    const uint64_t pins = spectrum_tick(&spectrum);
    if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ) && !answer.taken) {
      answer.taken = true;
      answer.acknowledged_at = (int)now;
    }
    if (answer.taken && spectrum_instruction_complete(&spectrum)) {
      answer.tstates++;
      break;
    }
  }
  answer.pc = spectrum.cpu.pc;
  answer.sp = spectrum.cpu.sp;
  answer.pushed = word_at(spectrum.cpu.sp);
  answer.r = spectrum.cpu.r;
  answer.iff1 = spectrum.cpu.iff1;
  answer.iff2 = spectrum.cpu.iff2;
  return answer;
}

static acceptance run_from(const uint8_t *program, size_t length, uint8_t mode, uint8_t i,
                           uint32_t at) {
  start(program, length, mode, i, at);
  return run();
}

static const uint8_t nop[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* Mode 1 sends the processor to &0038 and costs thirteen T-states beyond the
   instruction it waited for. */
static void mode_one_costs_thirteen_and_goes_to_the_restart(void) {
  const acceptance taken = run_from(nop, sizeof nop, 1, 0x00, 0);
  TEST_CHECK(taken.taken);
  TEST_EQUAL(taken.tstates, 4 + MODE_1_TSTATES);
  TEST_EQUAL(taken.pc, 0x0038);
  TEST_EQUAL(taken.sp, WORKING_SP - 2);
  TEST_EQUAL(taken.pushed, UNDER_TEST + 1); /* the instruction after the one that ran */
  TEST_EQUAL(taken.r, 2);                   /* the instruction's fetch, and the acknowledge's */
  TEST_CHECK(!taken.iff1);
  TEST_CHECK(!taken.iff2); /* a maskable acceptance clears both */
}

/* Mode 2 costs nineteen and reads where it is going from a table. Nothing on
   this board drives the bus during the acknowledge, so the byte is &FF and
   the entry read is the one at I:&FF — which is why a Spectrum's table has to
   be 257 bytes long and why its last entry straddles the page. */
static void mode_two_costs_nineteen_and_reads_the_table_at_ff(void) {
  start(nop, sizeof nop, 2, 0x40, 0);
  const uint16_t table = 0x40FF; /* I:&FF, which is where the floating bus points */
  ram[table - SPECTRUM_RAM_BASE] = 0x21;
  ram[table + 1 - SPECTRUM_RAM_BASE] = 0x43;

  const acceptance taken = run();
  TEST_CHECK(taken.taken);
  TEST_EQUAL(taken.tstates, 4 + MODE_2_TSTATES);
  TEST_EQUAL(taken.pc, 0x4321); /* the two bytes the table holds, the second one a page on */
  TEST_EQUAL(taken.sp, WORKING_SP - 2);
}

/* The line is low for the first thirty-two T-states and the processor looks
   at it on an instruction's last T-state, so the last instruction that can
   catch a frame's interrupt is the one whose last T-state is the
   thirty-second. An instruction that ends a T-state later waits a whole
   frame — which is why a program that must not miss one keeps its loop
   short, and why one that must not take one early sits in HALT. */
static void walk_the_edge(const char *what, const uint8_t *program, size_t length,
                          uint32_t tstates) {
  for (uint32_t at = 0; at <= INTERRUPT_HELD_TSTATES + tstates; at++) {
    const acceptance answer = run_from(program, length, 1, 0x00, at);
    const bool ends_inside = at + tstates - 1 < INTERRUPT_HELD_TSTATES;
    if (answer.taken != ends_inside) {
      TEST_FAIL("%s begun at %u was %s, and its last T-state is %u", what, at,
                answer.taken ? "taken" : "missed", at + tstates - 1);
    }
  }
}

static void an_instruction_catches_it_only_if_it_ends_inside_the_window(void) {
  walk_the_edge("a NOP", nop, sizeof nop, 4);

  /* A halted processor is fetching the same instruction over and over, four
     T-states at a time, so it reaches a boundary as often as a NOP does and
     answers the line on the same edge. */
  const uint8_t halt[] = {0x76};
  walk_the_edge("a HALT", halt, sizeof halt, 4);

  /* And the two repeating shapes, which are twenty-one T-states apiece while
     they repeat: one that writes on its last cycle and one that reads. */
  const uint8_t ldir[] = {0xED, 0xB0};
  walk_the_edge("an LDIR", ldir, sizeof ldir, 21);
  const uint8_t cpir[] = {0xED, 0xB1};
  walk_the_edge("a CPIR", cpir, sizeof cpir, 21);
}

/* A repeating instruction is interrupted between its iterations, and what is
   pushed is the instruction itself rather than what follows it — so the
   handler returns into the middle of the copy and it carries on. */
static void a_repeating_instruction_is_pushed_at_its_own_address(void) {
  const uint8_t ldir[] = {0xED, 0xB0};
  const acceptance answer = run_from(ldir, sizeof ldir, 1, 0x00, 8);
  TEST_CHECK(answer.taken);
  TEST_EQUAL(answer.tstates, 21 + MODE_1_TSTATES);
  TEST_EQUAL(answer.pushed, UNDER_TEST);
  TEST_EQUAL(answer.r, 3); /* two fetches for the prefix and the opcode, and the acknowledge */
}

/* A halted processor is woken by the interrupt, and what is pushed is the
   instruction behind the HALT and not the HALT itself — so it is not halted
   again on the way back. */
static void a_halted_processor_is_woken_and_does_not_halt_again(void) {
  const uint8_t halt[] = {0x76};
  const acceptance answer = run_from(halt, sizeof halt, 1, 0x00, 0);
  TEST_CHECK(answer.taken);
  TEST_EQUAL(answer.pc, 0x0038);
  TEST_EQUAL(answer.pushed, UNDER_TEST + 1);
}

/* The acknowledge always falls in the top border, where the ULA wants nothing
   and owes nothing — so an acceptance costs what the processor costs and not
   a T-state more, wherever in the window it begins. */
static void an_acceptance_is_never_charged_for_the_bus(void) {
  for (uint32_t at = 0; at < INTERRUPT_HELD_TSTATES; at += 4) {
    const acceptance answer = run_from(nop, sizeof nop, 1, 0x00, at);
    if (!answer.taken) {
      TEST_FAIL("a NOP begun at %u never took the interrupt", at);
      continue;
    }
    TEST_EQUAL(answer.tstates, 4 + MODE_1_TSTATES);
  }
}

int main(void) {
  TEST_RUN(mode_one_costs_thirteen_and_goes_to_the_restart);
  TEST_RUN(mode_two_costs_nineteen_and_reads_the_table_at_ff);
  TEST_RUN(an_instruction_catches_it_only_if_it_ends_inside_the_window);
  TEST_RUN(a_repeating_instruction_is_pushed_at_its_own_address);
  TEST_RUN(a_halted_processor_is_woken_and_does_not_halt_again);
  TEST_RUN(an_acceptance_is_never_charged_for_the_bus);
  return TEST_REPORT("spectrum interrupt");
}
