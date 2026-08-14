/*
 * z80_test — tests for what the SingleStepTests corpus cannot see.
 *
 * The corpus proves instruction semantics per T-state from an independent
 * source, which is why none of it is restated here. These tests cover the
 * rest: the reset contract, the invariants of our own micro-program
 * machinery, and the pin behaviour no corpus test asserts.
 */
#include <string.h>

#include "test.h"
#include "z80.h"

static uint8_t ram[0x10000];

/* The longest real instruction is 23 T-states (EX (SP),IX and the DD CB
   forms); past this budget an instruction is hung, not slow. */
#define TICK_BUDGET 64

#define INSTRUCTION_HUNG 0
#define INSTRUCTION_OVERFLOWED (-1)

static void load(uint16_t address, const uint8_t *bytes, size_t length) {
  for (size_t index = 0; index < length; index++) {
    ram[(uint16_t)(address + index)] = bytes[index];
  }
}

static uint64_t service_bus(uint64_t pins) {
  if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
    return z80_set_data(pins, ram[z80_address(pins)]);
  }
  if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
    ram[z80_address(pins)] = z80_data(pins);
    return pins;
  }
  if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
    return z80_set_data(pins, 0);
  }
  return pins;
}

/* Returns the T-states taken, or one of the outcomes above. wait_pattern is
   a repeating eight-tick mask of the ticks to hold WAIT on. */
static int run_instruction_waiting(z80_t *cpu, unsigned wait_pattern) {
  uint64_t pins = 0;
  for (int ticks = 1; ticks <= TICK_BUDGET; ticks++) {
    if ((wait_pattern >> (ticks % 8)) & 1) {
      pins |= Z80_WAIT;
    } else {
      pins &= ~Z80_WAIT;
    }
    pins = z80_tick(cpu, pins);
    if (cpu->program_length > Z80_MAX_MACHINE_CYCLES) {
      return INSTRUCTION_OVERFLOWED;
    }
    pins = service_bus(pins);
    if (z80_instruction_complete(cpu)) {
      return ticks;
    }
  }
  return INSTRUCTION_HUNG;
}

static int run_instruction(z80_t *cpu) { return run_instruction_waiting(cpu, 0); }

static int run_instruction_holding_wait(z80_t *cpu, int first_ticks) {
  uint64_t pins = 0;
  for (int ticks = 1; ticks <= TICK_BUDGET; ticks++) {
    if (ticks <= first_ticks) {
      pins |= Z80_WAIT;
    } else {
      pins &= ~Z80_WAIT;
    }
    pins = z80_tick(cpu, pins);
    pins = service_bus(pins);
    if (z80_instruction_complete(cpu)) {
      return ticks;
    }
  }
  return INSTRUCTION_HUNG;
}

static void test_reset_state(void) {
  z80_t cpu;
  z80_init(&cpu);
  TEST_EQUAL(cpu.a, 0xFF);
  TEST_EQUAL(cpu.f, 0xFF);
  TEST_EQUAL(cpu.sp, 0xFFFF);
  TEST_EQUAL(cpu.pc, 0);
  TEST_EQUAL(cpu.i, 0);
  TEST_EQUAL(cpu.r, 0);
  TEST_EQUAL(cpu.im, 0);
  TEST_CHECK(!cpu.iff1);
  TEST_CHECK(!cpu.iff2);
  TEST_CHECK(!cpu.halted);
  TEST_CHECK(z80_instruction_complete(&cpu));
}

/* An instruction needing more cycles than Z80_MAX_MACHINE_CYCLES overwrites
   the state beside the micro-program, so this sweep guards that bound as much
   as it guards against hangs. */
static void run_page(const char *page, const uint8_t *lead, size_t lead_length) {
  for (unsigned opcode = 0; opcode <= 0xFF; opcode++) {
    z80_t cpu;
    z80_init(&cpu);
    cpu.pc = 0x1000;
    cpu.sp = 0x8000;
    memset(ram, 0, sizeof ram);

    uint8_t bytes[4];
    size_t length = 0;
    for (size_t index = 0; index < lead_length; index++) {
      bytes[length++] = lead[index];
    }
    bytes[length++] = (uint8_t)opcode;
    load(cpu.pc, bytes, length);

    const int ticks = run_instruction(&cpu);
    if (ticks == INSTRUCTION_OVERFLOWED) {
      TEST_FAIL("opcode %s%02X needs more than %d machine cycles", page, opcode,
                Z80_MAX_MACHINE_CYCLES);
      return;
    }
    if (ticks == INSTRUCTION_HUNG) {
      TEST_FAIL("opcode %s%02X did not finish within %d T-states", page, opcode, TICK_BUDGET);
      return;
    }
  }
}

static void test_unprefixed_page_completes(void) { run_page("", NULL, 0); }

static void test_cb_page_completes(void) {
  const uint8_t lead[] = {0xCB};
  run_page("CB ", lead, sizeof lead);
}

static void test_ed_page_completes(void) {
  const uint8_t lead[] = {0xED};
  run_page("ED ", lead, sizeof lead);
}

static void test_dd_page_completes(void) {
  const uint8_t lead[] = {0xDD};
  run_page("DD ", lead, sizeof lead);
}

static void test_fd_page_completes(void) {
  const uint8_t lead[] = {0xFD};
  run_page("FD ", lead, sizeof lead);
}

static void test_ddcb_page_completes(void) {
  const uint8_t lead[] = {0xDD, 0xCB, 0x02}; /* prefixes, then the displacement */
  run_page("DD CB ", lead, sizeof lead);
}

static void test_fdcb_page_completes(void) {
  const uint8_t lead[] = {0xFD, 0xCB, 0x02};
  run_page("FD CB ", lead, sizeof lead);
}

/* Stalls a cycle where it actually sits on the bus, and on the tick after,
   where WAIT is sampled. Same duration means the cycle ignored the pin. */
static void check_cycle_honours_wait(const char *what, const uint8_t *program, size_t length,
                                     uint64_t asserted, uint64_t forbidden) {
  z80_t cpu;
  uint32_t cycle_ticks = 0;
  int plain_ticks = 0;

  z80_init(&cpu);
  cpu.pc = 0x1000;
  memset(ram, 0, sizeof ram);
  load(cpu.pc, program, length);
  uint64_t pins = 0;
  for (int ticks = 1; ticks < 30; ticks++) {
    pins = z80_tick(&cpu, pins);
    if ((pins & asserted) == asserted && (pins & forbidden) == 0) {
      cycle_ticks |= (1u << ticks) | (1u << (ticks + 1));
    }
    pins = service_bus(pins);
    if (z80_instruction_complete(&cpu)) {
      plain_ticks = ticks;
      break;
    }
  }
  if (cycle_ticks == 0) {
    TEST_FAIL("no %s cycle happens in this instruction", what);
    return;
  }

  z80_init(&cpu);
  cpu.pc = 0x1000;
  memset(ram, 0, sizeof ram);
  load(cpu.pc, program, length);
  pins = 0;
  for (int ticks = 1; ticks <= TICK_BUDGET; ticks++) {
    if (ticks < 31 && ((cycle_ticks >> ticks) & 1)) {
      pins |= Z80_WAIT;
    } else {
      pins &= ~Z80_WAIT;
    }
    pins = z80_tick(&cpu, pins);
    pins = service_bus(pins);
    if (z80_instruction_complete(&cpu)) {
      if (ticks <= plain_ticks) {
        TEST_FAIL("the %s cycle ignores WAIT: %d T-states either way", what, ticks);
      }
      return;
    }
  }
  TEST_FAIL("the %s cycle never finished", what);
}

static void test_wait_stretches_every_kind_of_cycle(void) {
  const uint8_t fetch[] = {0x00};          /* NOP */
  const uint8_t memory_read[] = {0x7E};    /* LD A,(HL) */
  const uint8_t memory_write[] = {0x77};   /* LD (HL),A */
  const uint8_t port_read[] = {0xDB, 20};  /* IN A,(20) */
  const uint8_t port_write[] = {0xD3, 20}; /* OUT (20),A */
  check_cycle_honours_wait("opcode fetch", fetch, sizeof fetch, Z80_M1 | Z80_MREQ | Z80_RD, 0);
  check_cycle_honours_wait("memory read", memory_read, sizeof memory_read, Z80_MREQ | Z80_RD,
                           Z80_M1);
  check_cycle_honours_wait("memory write", memory_write, sizeof memory_write, Z80_MREQ | Z80_WR, 0);
  check_cycle_honours_wait("port read", port_read, sizeof port_read, Z80_IORQ | Z80_RD, 0);
  check_cycle_honours_wait("port write", port_write, sizeof port_write, Z80_IORQ | Z80_WR, 0);
}

static void test_wait_adds_one_tstate_per_held_tick(void) {
  int duration[10];
  for (int held = 0; held < 10; held++) {
    z80_t cpu;
    z80_init(&cpu);
    cpu.pc = 0x1000;
    memset(ram, 0, sizeof ram);
    const uint8_t program[] = {0x00}; /* NOP */
    load(cpu.pc, program, sizeof program);
    duration[held] = run_instruction_holding_wait(&cpu, held);
  }
  TEST_EQUAL(duration[0], 4);

  int first_stretch = 0;
  while (first_stretch < 10 && duration[first_stretch] == duration[0]) {
    first_stretch++;
  }
  TEST_CHECK(first_stretch < 10);
  for (int held = first_stretch; held < 10; held++) {
    TEST_EQUAL(duration[held], duration[0] + (held - first_stretch) + 1);
  }
}

static void test_wait_holds_the_bus_steady(void) {
  z80_t cpu;
  z80_init(&cpu);
  cpu.pc = 0x1000;
  memset(ram, 0, sizeof ram);
  const uint8_t program[] = {0x00};
  load(cpu.pc, program, sizeof program);

  uint64_t pins = 0;
  int stalls = 0;
  for (int ticks = 1; ticks <= 8; ticks++) {
    const uint8_t step_before = cpu.step;
    const uint64_t bus_before = pins & ~Z80_WAIT;
    pins = z80_tick(&cpu, pins | Z80_WAIT);
    if (cpu.step == step_before) {
      TEST_EQUAL(pins & ~Z80_WAIT, bus_before);
      stalls++;
    }
    pins = service_bus(pins);
  }
  TEST_CHECK(stalls > 0);
}

/* Field by field rather than memcmp: the struct has padding, and padding
   bytes are nobody's business. */
static const char *first_difference(const z80_t *left, const z80_t *right) {
#define COMPARE(field)                                                                             \
  if (left->field != right->field) {                                                               \
    return #field;                                                                                 \
  }
  COMPARE(a)
  COMPARE(f)
  COMPARE(b)
  COMPARE(c)
  COMPARE(d)
  COMPARE(e)
  COMPARE(h)
  COMPARE(l)
  COMPARE(af_)
  COMPARE(bc_)
  COMPARE(de_)
  COMPARE(hl_)
  COMPARE(ixh)
  COMPARE(ixl)
  COMPARE(iyh)
  COMPARE(iyl)
  COMPARE(sp)
  COMPARE(pc)
  COMPARE(wz)
  COMPARE(i)
  COMPARE(r)
  COMPARE(im)
  COMPARE(iff1)
  COMPARE(iff2)
  COMPARE(halted)
  COMPARE(ei)
  COMPARE(p)
  COMPARE(q)
#undef COMPARE
  return NULL;
}

static void test_wait_changes_timing_only(void) {
  static uint8_t ram_after_plain[0x10000];
  for (unsigned opcode = 0; opcode <= 0xFF; opcode++) {
    const uint8_t program[] = {(uint8_t)opcode, 0x37, 0x21, 0x8A};

    z80_t plain;
    z80_init(&plain);
    plain.pc = 0x1000;
    plain.sp = 0x8000;
    memset(ram, 0, sizeof ram);
    load(plain.pc, program, sizeof program);
    const int plain_ticks = run_instruction(&plain);
    memcpy(ram_after_plain, ram, sizeof ram);

    z80_t waited;
    z80_init(&waited);
    waited.pc = 0x1000;
    waited.sp = 0x8000;
    memset(ram, 0, sizeof ram);
    load(waited.pc, program, sizeof program);
    const int waited_ticks = run_instruction_waiting(&waited, 0xAA);

    if (waited_ticks <= plain_ticks) {
      TEST_FAIL("opcode %02X took %d T-states waiting, %d without", opcode, waited_ticks,
                plain_ticks);
      return;
    }
    const char *difference = first_difference(&plain, &waited);
    if (difference != NULL) {
      TEST_FAIL("opcode %02X reaches a different %s when made to wait", opcode, difference);
      return;
    }
    if (memcmp(ram_after_plain, ram, sizeof ram) != 0) {
      TEST_FAIL("opcode %02X writes different memory when made to wait", opcode);
      return;
    }
  }
}

/* A halted Z80 keeps fetching so that the refresh keeps running. */
static void test_halt_keeps_fetching_without_advancing(void) {
  z80_t cpu;
  z80_init(&cpu);
  cpu.pc = 0x1000;
  memset(ram, 0, sizeof ram);
  const uint8_t program[] = {0x76}; /* HALT */
  load(cpu.pc, program, sizeof program);

  TEST_EQUAL(run_instruction(&cpu), 4);
  TEST_CHECK(cpu.halted);
  TEST_EQUAL(cpu.pc, 0x1001);
  const uint8_t refresh_when_halted = cpu.r;

  TEST_CHECK((z80_tick(&cpu, 0) & Z80_HALT) != 0);
  z80_init(&cpu);
  cpu.pc = 0x1000;
  cpu.halted = true;
  cpu.r = refresh_when_halted;
  for (int instruction = 1; instruction <= 3; instruction++) {
    TEST_EQUAL(run_instruction(&cpu), 4);
    TEST_EQUAL(cpu.pc, 0x1000);
    TEST_EQUAL(cpu.r & 0x7F, (refresh_when_halted + instruction) & 0x7F);
  }
}

static void test_state_struct_is_the_whole_machine(void) {
  static uint8_t ram_at_snapshot[0x10000];
  static uint64_t first_run[200];

  z80_t cpu;
  z80_init(&cpu);
  cpu.pc = 0x1000;
  memset(ram, 0, sizeof ram);
  const uint8_t program[] = {
      0x21, 0x00, 0x20, /* LD HL,0x2000 */
      0x36, 0x5A,       /* LD (HL),0x5A */
      0x7E,             /* LD A,(HL)    */
      0x3C,             /* INC A        */
      0x77,             /* LD (HL),A    */
      0x23,             /* INC HL       */
      0x18, 0xF8,       /* JR back to LD (HL),n */
  };
  load(cpu.pc, program, sizeof program);

  uint64_t pins = 0;
  for (int ticks = 0; ticks < 200; ticks++) {
    pins = service_bus(z80_tick(&cpu, pins));
  }

  const z80_t cpu_at_snapshot = cpu;
  const uint64_t pins_at_snapshot = pins;
  memcpy(ram_at_snapshot, ram, sizeof ram);

  for (int ticks = 0; ticks < 200; ticks++) {
    pins = service_bus(z80_tick(&cpu, pins));
    first_run[ticks] = pins;
  }

  cpu = cpu_at_snapshot;
  pins = pins_at_snapshot;
  memcpy(ram, ram_at_snapshot, sizeof ram);

  for (int ticks = 0; ticks < 200; ticks++) {
    pins = service_bus(z80_tick(&cpu, pins));
    if (pins != first_run[ticks]) {
      TEST_FAIL("tick %d differs after restoring the snapshot", ticks);
      return;
    }
  }
}

/* The subtlety: between the two fetches the CPU is at a fetch boundary, which
   is not the same as being between instructions. */
static void test_prefix_is_not_a_complete_instruction(void) {
  z80_t cpu;
  z80_init(&cpu);
  cpu.pc = 0x1000;
  memset(ram, 0, sizeof ram);
  const uint8_t program[] = {0xDD, 0x00};
  load(cpu.pc, program, sizeof program);

  uint64_t pins = 0;
  for (int ticks = 0; ticks < 4; ticks++) {
    pins = z80_tick(&cpu, pins);
    if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
      pins = z80_set_data(pins, ram[z80_address(pins)]);
    }
  }
  TEST_CHECK(!z80_instruction_complete(&cpu));
  TEST_EQUAL(cpu.prefix, 0xDD);

  for (int ticks = 0; ticks < 4; ticks++) {
    pins = z80_tick(&cpu, pins);
    if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
      pins = z80_set_data(pins, ram[z80_address(pins)]);
    }
  }
  TEST_CHECK(z80_instruction_complete(&cpu));
  TEST_EQUAL(cpu.pc, 0x1002);
  TEST_EQUAL(cpu.r, 2); /* one refresh per fetch */
}

int main(void) {
  TEST_RUN(test_reset_state);
  TEST_RUN(test_unprefixed_page_completes);
  TEST_RUN(test_cb_page_completes);
  TEST_RUN(test_ed_page_completes);
  TEST_RUN(test_dd_page_completes);
  TEST_RUN(test_fd_page_completes);
  TEST_RUN(test_ddcb_page_completes);
  TEST_RUN(test_fdcb_page_completes);
  TEST_RUN(test_prefix_is_not_a_complete_instruction);
  TEST_RUN(test_wait_stretches_every_kind_of_cycle);
  TEST_RUN(test_wait_adds_one_tstate_per_held_tick);
  TEST_RUN(test_wait_holds_the_bus_steady);
  TEST_RUN(test_wait_changes_timing_only);
  TEST_RUN(test_halt_keeps_fetching_without_advancing);
  TEST_RUN(test_state_struct_is_the_whole_machine);
  return TEST_REPORT("z80");
}
