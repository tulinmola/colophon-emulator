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

  /* The request pins go out before WAIT is sampled and stay out for as long
     as it holds, which is what the datasheet's timing diagram shows: the
     bus is steady from one stalled tick to the next, not from before the
     stall began. */
  uint64_t pins = 0;
  uint64_t bus_while_stalled = 0;
  int stalls = 0;
  for (int ticks = 1; ticks <= 8; ticks++) {
    const uint8_t step_before = cpu.step;
    pins = z80_tick(&cpu, pins | Z80_WAIT);
    if (cpu.step == step_before) {
      if (stalls > 0) {
        TEST_EQUAL(pins & ~Z80_WAIT, bus_while_stalled);
      }
      bus_while_stalled = pins & ~Z80_WAIT;
      stalls++;
    }
    pins = service_bus(pins);
  }
  TEST_CHECK(stalls > 1);
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
    /* WAIT held on even ticks, which is where the second T-state of the
       opening fetch falls: every instruction stalls at least once. */
    const int waited_ticks = run_instruction_waiting(&waited, 0x55);

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

/* Runs one instruction with the given input pins held, and then whatever
   interrupt sequence follows it, answering the acknowledge with `vector`.
   Returns the T-states for both together, so an acceptance costs the total
   minus the instruction's own length. */
static int run_with_interrupt(z80_t *cpu, uint64_t inputs, uint8_t vector) {
  uint64_t pins = 0;
  for (int ticks = 1; ticks <= TICK_BUDGET; ticks++) {
    pins = z80_tick(cpu, pins | inputs);
    if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
      pins = z80_set_data(pins, vector);
    } else {
      pins = service_bus(pins);
    }
    if (z80_instruction_complete(cpu)) {
      return ticks;
    }
  }
  return INSTRUCTION_HUNG;
}

static void start_at(z80_t *cpu, const uint8_t *program, size_t length) {
  z80_init(cpu);
  cpu->pc = 0x1000;
  cpu->sp = 0x8000;
  memset(ram, 0, sizeof ram);
  load(cpu->pc, program, length);
}

static uint16_t pushed_word(const z80_t *cpu) {
  return (uint16_t)(ram[cpu->sp] | (ram[(uint16_t)(cpu->sp + 1)] << 8));
}

/* 11 T-states, and the acknowledge is an ordinary opcode fetch whose byte is
   discarded — not the IORQ cycle a maskable interrupt uses. IFF2 survives so
   RETN can put it back. */
static void test_nmi_acceptance(void) {
  z80_t cpu;
  const uint8_t program[] = {0x00}; /* NOP */
  start_at(&cpu, program, sizeof program);
  cpu.iff1 = cpu.iff2 = true;

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_NMI, 0xFF), 4 + 11);
  TEST_EQUAL(cpu.pc, 0x0066);
  TEST_CHECK(!cpu.iff1);
  TEST_CHECK(cpu.iff2);
  TEST_EQUAL(cpu.sp, 0x7FFE);
  TEST_EQUAL(pushed_word(&cpu), 0x1001);
  TEST_EQUAL(cpu.r, 2); /* the instruction's fetch and the acknowledge's */
}

/* Edge-triggered: a level that stays asserted arms nothing more. */
static void test_nmi_triggers_once_per_edge(void) {
  z80_t cpu;
  const uint8_t program[] = {0x00, 0x00};
  start_at(&cpu, program, sizeof program);

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_NMI, 0xFF), 4 + 11);
  TEST_EQUAL(cpu.pc, 0x0066);

  cpu.pc = 0x1001;
  TEST_EQUAL(run_with_interrupt(&cpu, Z80_NMI, 0xFF), 4); /* still high: no edge */
  TEST_EQUAL(cpu.pc, 0x1002);
}

static void test_maskable_interrupt_obeys_iff1(void) {
  z80_t cpu;
  const uint8_t program[] = {0x00};
  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = cpu.iff2 = false;

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4);
  TEST_EQUAL(cpu.pc, 0x1001);
}

static void test_interrupt_mode_1(void) {
  z80_t cpu;
  const uint8_t program[] = {0x00};
  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = cpu.iff2 = true;

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4 + 13);
  TEST_EQUAL(cpu.pc, 0x0038);
  TEST_CHECK(!cpu.iff1);
  TEST_CHECK(!cpu.iff2); /* unlike NMI, a maskable interrupt clears both */
  TEST_EQUAL(pushed_word(&cpu), 0x1001);
}

/* Mode 2 reads its destination from I:vector. The vector's low bit is not
   forced even, whatever Zilog's manual says, so an odd one is used whole and
   the second read crosses into the next page. */
static void test_interrupt_mode_2_uses_the_whole_vector(void) {
  z80_t cpu;
  const uint8_t program[] = {0x00};
  start_at(&cpu, program, sizeof program);
  cpu.im = 2;
  cpu.i = 0x80;
  cpu.iff1 = cpu.iff2 = true;
  ram[0x80FF] = 0x34;
  ram[0x8100] = 0x12;

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4 + 19);
  TEST_EQUAL(cpu.pc, 0x1234);
  TEST_EQUAL(pushed_word(&cpu), 0x1001);
}

/* EI leaves interrupts blocked for exactly one instruction, so that a routine
   can end with EI followed by RET without being re-entered on its own stack. */
static void test_ei_blocks_for_one_instruction(void) {
  z80_t cpu;
  const uint8_t program[] = {0xFB, 0x00, 0x00}; /* EI; NOP; NOP */
  start_at(&cpu, program, sizeof program);
  cpu.im = 1;

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4);
  TEST_EQUAL(cpu.pc, 0x1001);
  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4 + 13);
  TEST_EQUAL(cpu.pc, 0x0038);
  TEST_EQUAL(pushed_word(&cpu), 0x1002);
}

/* A prefix and the opcode it modifies are one instruction; taking an interrupt
   between them would lose the prefix and change what the opcode means. */
static void test_prefix_blocks_acceptance(void) {
  z80_t cpu;
  const uint8_t program[] = {0xDD, 0x00}; /* DD NOP */
  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = cpu.iff2 = true;

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4 + 4 + 13);
  TEST_EQUAL(pushed_word(&cpu), 0x1002); /* past both bytes, not between them */
}

static void test_interrupt_wakes_a_halted_cpu(void) {
  z80_t cpu;
  const uint8_t program[] = {0x76}; /* HALT */
  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = cpu.iff2 = true;

  TEST_EQUAL(run_instruction(&cpu), 4);
  TEST_CHECK(cpu.halted);

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4 + 13);
  TEST_CHECK(!cpu.halted);
  TEST_EQUAL(cpu.pc, 0x0038);
  TEST_EQUAL(pushed_word(&cpu), 0x1001); /* the instruction after HALT */
}

/* RETN restores IFF1 during the next opcode fetch, too late for an interrupt
   at the end of RETN itself. Only an earlier NMI can leave the flip-flops
   disagreeing, which is the only time this is observable. */
static void test_retn_blocks_for_one_instruction(void) {
  z80_t cpu;
  const uint8_t program[] = {0xED, 0x45, 0x00}; /* RETN; NOP */
  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = false; /* as an accepted NMI leaves them */
  cpu.iff2 = true;
  cpu.sp = 0x7FFE;
  ram[0x7FFE] = 0x20;
  ram[0x7FFF] = 0x10; /* return to 0x1020 */
  ram[0x1020] = 0x00; /* a NOP waiting there */

  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 14);
  TEST_EQUAL(cpu.pc, 0x1020);
  TEST_CHECK(cpu.iff1);
  TEST_EQUAL(run_with_interrupt(&cpu, Z80_INT, 0xFF), 4 + 13);
  TEST_EQUAL(cpu.pc, 0x0038);
}

/* On NMOS parts IFF2 is cleared before LD A,I copies it, so an interrupt
   landing on that instruction leaves P/V claiming interrupts were disabled.
   Zilog acknowledged it in 1989 and fixed it on CMOS; the CPC is NMOS. */
static void test_ld_a_i_interrupted_clears_parity(void) {
  z80_t cpu;
  const uint8_t program[] = {0xED, 0x57}; /* LD A,I */

  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = cpu.iff2 = true;
  run_instruction(&cpu);
  TEST_CHECK((cpu.f & Z80_FLAG_PV) != 0); /* undisturbed, it reports IFF2 */

  start_at(&cpu, program, sizeof program);
  cpu.im = 1;
  cpu.iff1 = cpu.iff2 = true;
  run_with_interrupt(&cpu, Z80_INT, 0xFF);
  TEST_CHECK((cpu.f & Z80_FLAG_PV) == 0);
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
  TEST_RUN(test_nmi_acceptance);
  TEST_RUN(test_nmi_triggers_once_per_edge);
  TEST_RUN(test_maskable_interrupt_obeys_iff1);
  TEST_RUN(test_interrupt_mode_1);
  TEST_RUN(test_interrupt_mode_2_uses_the_whole_vector);
  TEST_RUN(test_ei_blocks_for_one_instruction);
  TEST_RUN(test_prefix_blocks_acceptance);
  TEST_RUN(test_interrupt_wakes_a_halted_cpu);
  TEST_RUN(test_retn_blocks_for_one_instruction);
  TEST_RUN(test_ld_a_i_interrupted_clears_parity);
  return TEST_REPORT("z80");
}
