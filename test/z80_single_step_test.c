/*
 * z80_single_step_test — runs the SingleStepTests Z80 suite against our core.
 *
 * Test data: https://github.com/SingleStepTests/z80 (MIT), fetched by
 * tools/fetch-tests.sh into test/data/v1/. Each file holds 1000 tests for one
 * opcode: initial/final CPU+RAM state plus the expected bus state after every
 * T-state ("sampled between cycles"). Their flag string is four positions,
 * "rwmi" for READ/WRITE/MREQ/IORQ, '-' when inactive.
 *
 * Bus convention learned from their traces: a read's data value appears on the
 * sample FOLLOWING the read pulse (memory answers, then the value sits on the
 * bus during the next T-state); a write's value appears on the write pulse
 * itself; every other sample has a disconnected ("null") data bus. The harness
 * mirrors that: it records the value it fed after servicing a read, the CPU's
 * data pins on a write pulse, and null otherwise.
 */
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "z80.h"

static uint8_t ram[0x10000];

static int get_int(const json_value *object, const char *key) {
  const json_value *value = json_get(object, key);
  return value ? (int)value->number : 0;
}

static void load_state(z80_t *cpu, const json_value *state) {
  z80_init(cpu);
  cpu->pc = (uint16_t)get_int(state, "pc");
  cpu->sp = (uint16_t)get_int(state, "sp");
  cpu->a = (uint8_t)get_int(state, "a");
  cpu->f = (uint8_t)get_int(state, "f");
  cpu->b = (uint8_t)get_int(state, "b");
  cpu->c = (uint8_t)get_int(state, "c");
  cpu->d = (uint8_t)get_int(state, "d");
  cpu->e = (uint8_t)get_int(state, "e");
  cpu->h = (uint8_t)get_int(state, "h");
  cpu->l = (uint8_t)get_int(state, "l");
  cpu->i = (uint8_t)get_int(state, "i");
  cpu->r = (uint8_t)get_int(state, "r");
  cpu->wz = (uint16_t)get_int(state, "wz");
  cpu->ixh = (uint8_t)(get_int(state, "ix") >> 8);
  cpu->ixl = (uint8_t)(get_int(state, "ix") & 0xFF);
  cpu->iyh = (uint8_t)(get_int(state, "iy") >> 8);
  cpu->iyl = (uint8_t)(get_int(state, "iy") & 0xFF);
  cpu->af_ = (uint16_t)get_int(state, "af_");
  cpu->bc_ = (uint16_t)get_int(state, "bc_");
  cpu->de_ = (uint16_t)get_int(state, "de_");
  cpu->hl_ = (uint16_t)get_int(state, "hl_");
  cpu->im = (uint8_t)get_int(state, "im");
  cpu->iff1 = get_int(state, "iff1") != 0;
  cpu->iff2 = get_int(state, "iff2") != 0;
  cpu->ei = get_int(state, "ei") != 0;
  cpu->p = (uint8_t)get_int(state, "p");
  cpu->q = (uint8_t)get_int(state, "q");
  memset(ram, 0, sizeof ram);
  const json_value *entries = json_get(state, "ram");
  for (size_t k = 0; entries && k < entries->length; k++) {
    const json_value *pair = &entries->items[k];
    ram[(uint16_t)pair->items[0].number] = (uint8_t)pair->items[1].number;
  }
}

#define CHECK(field, expr)                                                                         \
  do {                                                                                             \
    int want = get_int(final, field);                                                              \
    int got = (int)(expr);                                                                         \
    if (want != got) {                                                                             \
      if (verbose) {                                                                               \
        printf("    %s: got %d, want %d\n", field, got, want);                                     \
      }                                                                                            \
      mismatch_count++;                                                                            \
    }                                                                                              \
  } while (0)

static int check_final(const z80_t *cpu, const json_value *final, int verbose) {
  int mismatch_count = 0;
  CHECK("pc", cpu->pc);
  CHECK("sp", cpu->sp);
  CHECK("a", cpu->a);
  CHECK("f", cpu->f);
  CHECK("b", cpu->b);
  CHECK("c", cpu->c);
  CHECK("d", cpu->d);
  CHECK("e", cpu->e);
  CHECK("h", cpu->h);
  CHECK("l", cpu->l);
  CHECK("i", cpu->i);
  CHECK("r", cpu->r);
  CHECK("wz", cpu->wz);
  CHECK("ix", (cpu->ixh << 8) | cpu->ixl);
  CHECK("iy", (cpu->iyh << 8) | cpu->iyl);
  CHECK("af_", cpu->af_);
  CHECK("bc_", cpu->bc_);
  CHECK("de_", cpu->de_);
  CHECK("hl_", cpu->hl_);
  CHECK("im", cpu->im);
  CHECK("iff1", cpu->iff1);
  CHECK("iff2", cpu->iff2);
  CHECK("ei", cpu->ei);
  CHECK("p", cpu->p);
  CHECK("q", cpu->q);
  const json_value *entries = json_get(final, "ram");
  for (size_t k = 0; entries && k < entries->length; k++) {
    const json_value *pair = &entries->items[k];
    uint16_t address = (uint16_t)pair->items[0].number;
    uint8_t want = (uint8_t)pair->items[1].number;
    if (ram[address] != want) {
      if (verbose) {
        printf("    ram[%04X]: got %02X, want %02X\n", address, ram[address], want);
      }
      mismatch_count++;
    }
  }
  return mismatch_count;
}

static int run_test(const json_value *test, int verbose) {
  const json_value *name = json_get(test, "name");
  const json_value *cycles = json_get(test, "cycles");
  const json_value *ports = json_get(test, "ports");
  size_t port_index = 0;
  z80_t cpu;
  load_state(&cpu, json_get(test, "initial"));

  int mismatch_count = 0;
  uint64_t pins = 0;
  int data_was_fed = 0;
  uint8_t fed_data = 0;
  for (size_t k = 0; k < cycles->length; k++) {
    pins = z80_tick(&cpu, pins);

    uint16_t address = z80_address(pins);
    int data = -1; /* null: nobody drives the data bus */
    if (data_was_fed) {
      data = fed_data;
    } else if (pins & Z80_WR) {
      data = z80_data(pins);
    }
    char flags[5] = "----";
    if (pins & Z80_RD)
      flags[0] = 'r';
    if (pins & Z80_WR)
      flags[1] = 'w';
    if (pins & Z80_MREQ)
      flags[2] = 'm';
    if (pins & Z80_IORQ)
      flags[3] = 'i';

    const json_value *want = &cycles->items[k];
    const json_value *want_address = &want->items[0];
    const json_value *want_data = &want->items[1];
    const char *want_flags = want->items[2].string;
    int bad = 0;
    if (want_address->type != JSON_NULL && (uint16_t)want_address->number != address)
      bad = 1;
    int want_data_value = want_data->type == JSON_NULL ? -1 : (int)want_data->number;
    if (want_data_value != data)
      bad = 1;
    if (strcmp(want_flags, flags) != 0)
      bad = 1;
    if (bad) {
      if (verbose) {
        printf("    cycle %zu: got [%04X, %d, %s], want [%04X, %d, %s]\n", k + 1, address, data,
               flags, want_address->type == JSON_NULL ? 0 : (int)want_address->number,
               want_data_value, want_flags);
      }
      mismatch_count++;
    }

    /* service the bus for the next tick */
    data_was_fed = 0;
    if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
      fed_data = ram[address];
      pins = z80_set_data(pins, fed_data);
      data_was_fed = 1;
    } else if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
      ram[address] = z80_data(pins);
    } else if ((pins & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD)) {
      /* each I/O pulse consumes the test's next ports entry, which states the
         expected address, the value, and the direction */
      fed_data = 0;
      if (ports && port_index < ports->length) {
        const json_value *entry = &ports->items[port_index++];
        if ((uint16_t)entry->items[0].number != address || entry->items[2].string[0] != 'r') {
          mismatch_count++;
        }
        fed_data = (uint8_t)entry->items[1].number;
      } else {
        mismatch_count++;
      }
      pins = z80_set_data(pins, fed_data);
      data_was_fed = 1;
    } else if ((pins & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR)) {
      if (ports && port_index < ports->length) {
        const json_value *entry = &ports->items[port_index++];
        if ((uint16_t)entry->items[0].number != address || entry->items[2].string[0] != 'w' ||
            (uint8_t)entry->items[1].number != z80_data(pins)) {
          mismatch_count++;
        }
      } else {
        mismatch_count++;
      }
    }
  }
  mismatch_count += check_final(&cpu, json_get(test, "final"), verbose);

  if (mismatch_count && verbose) {
    printf("  FAIL %s (%d mismatches)\n", name && name->string ? name->string : "?",
           mismatch_count);
  }
  return mismatch_count ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <test.json>...\n", argv[0]);
    return 2;
  }
  int failed_files = 0;
  long total_tests = 0, total_passed = 0;
  for (int i = 1; i < argc; i++) {
    char error[256];
    json_value *root = json_parse_file(argv[i], error, sizeof error);
    if (!root) {
      fprintf(stderr, "%s: %s\n", argv[i], error);
      return 2;
    }
    int pass = 0, fail = 0;
    for (size_t k = 0; k < root->length; k++) {
      if (run_test(&root->items[k], fail < 3)) {
        fail++;
      } else {
        pass++;
      }
    }
    putchar(fail ? 'F' : '.');
    fflush(stdout);
    if (fail) {
      printf("\n%s: %d/%d pass\n", argv[i], pass, pass + fail);
      failed_files++;
    }
    total_tests += pass + fail;
    total_passed += pass;
    json_free(root);
  }
  printf("\nz80 single-step: %d files, %ld/%ld tests pass\n", argc - 1, total_passed, total_tests);
  return failed_files ? 1 : 0;
}
