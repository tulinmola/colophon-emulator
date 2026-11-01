/*
 * ay38910_test — selecting, writing, reading, the chip select, the port the
 * keyboard uses, and the sound: by GI's formulas and figures, by ACID, and
 * clock by clock against the die's model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ay38910.h"
#include "test.h"

static ay38910_t psg;

static void select_register(uint8_t number) { ay38910_access(&psg, AY38910_SELECT, number); }

static void reset_state(void) {
  ay38910_init(&psg);
  TEST_EQUAL(psg.selected, 0);
  TEST_CHECK(!psg.chip_selected);
  TEST_EQUAL(psg.registers[0], 0);
  TEST_CHECK(ay38910_port_a_is_input(&psg));
}

/* Reset sets the flip-flop that leaves the chip unselected (deathsoft and
   lvd's model, PSG_SEL), so a write needs an address before it. */
static void a_reset_chip_takes_no_write_until_it_is_selected(void) {
  ay38910_init(&psg);
  ay38910_access(&psg, AY38910_WRITE, 0x55);
  TEST_EQUAL(psg.registers[0], 0);
  select_register(0);
  ay38910_access(&psg, AY38910_WRITE, 0x55);
  TEST_EQUAL(psg.registers[0], 0x55);
}

static void a_register_holds_what_it_is_given(void) {
  ay38910_init(&psg);
  select_register(8);
  ay38910_access(&psg, AY38910_WRITE, 0x0D);
  TEST_EQUAL(psg.registers[8], 0x0D);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x0D);
}

static void a_register_keeps_only_the_bits_it_has(void) {
  ay38910_init(&psg);
  select_register(1); /* the high half of a tone period: four bits */
  ay38910_access(&psg, AY38910_WRITE, 0xFF);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x0F);
  select_register(6); /* the noise period: five bits */
  ay38910_access(&psg, AY38910_WRITE, 0xFF);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x1F);
  select_register(0); /* the low half: all eight */
  ay38910_access(&psg, AY38910_WRITE, 0xFF);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0xFF);
}

/* "Unless otherwise specified, address bits DA7--DA4 are programmed to
   recognize only a 0000 code" (GI's data manual, §2.1.1). */
static void only_an_address_below_sixteen_selects_the_chip(void) {
  for (int address = 0; address < 256; address++) {
    ay38910_init(&psg);
    select_register(0);
    select_register((uint8_t)address);
    TEST_CHECK(psg.chip_selected == (address < 16));
  }
}

/* Kevin Thacker's ACID "invalid register write test", written for real
   CPCs and Spectrums (psg.asm, invalid_reg_write_test,
   https://github.com/floooh/emu-info/tree/93d132f/tests/cpc/acid/psg): with
   every register filled from its reg_data, a write of &33 after each
   address from 16 to 255 changes none of them. The note "failed, got 1,
   expected 0 on cpc6128 with type 0" stands between this test and the
   next, which is disabled; both report in that form, and the file's notes
   mostly sit above the test they describe. */
static void no_address_above_fifteen_writes_a_register(void) {
  static const uint8_t reg_data[16] = {1, 2, 3, 4, 5, 6, 7, 0xC8, 9, 10, 11, 12, 13, 14, 15, 16};
  ay38910_init(&psg);
  for (int index = 0; index < 16; index++) {
    select_register((uint8_t)index);
    ay38910_access(&psg, AY38910_WRITE, reg_data[index]);
  }
  for (int address = 16; address < 256; address++) {
    select_register((uint8_t)address);
    ay38910_access(&psg, AY38910_WRITE, 0x33);
    for (int index = 0; index < 16; index++) {
      TEST_EQUAL(psg.registers[index], reg_data[index]);
    }
  }
}

/* "A valid high order address latches the register address" (§2.1.1), and
   one that is not valid latches nothing, so the number selected before
   stays; on the die it also takes PSG_SEL low (ay38910.h), so the write goes
   nowhere. */
static void an_address_above_fifteen_latches_nothing(void) {
  ay38910_init(&psg);
  select_register(2);
  ay38910_access(&psg, AY38910_WRITE, 0x11);
  select_register(0x13);
  TEST_EQUAL(psg.selected, 2);
  ay38910_access(&psg, AY38910_WRITE, 0x22);
  for (int index = 0; index < 16; index++) {
    TEST_EQUAL(psg.registers[index], index == 2 ? 0x11 : 0);
  }
}

/* An unselected chip's "bidirectional buffers are forced to a high
   impedance state" (§2.1.1). */
static void an_unselected_chip_leaves_the_bus_as_it_found_it(void) {
  ay38910_init(&psg);
  select_register(8);
  ay38910_access(&psg, AY38910_WRITE, 0x0D);
  select_register(0xF8);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0xA5), 0xA5);
}

static void an_address_that_matches_selects_the_chip_again(void) {
  ay38910_init(&psg);
  select_register(0x40);
  select_register(4);
  TEST_CHECK(psg.chip_selected);
  ay38910_access(&psg, AY38910_WRITE, 0x33);
  TEST_EQUAL(psg.registers[4], 0x33);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x33);
}

static void a_selection_lasts_until_the_next_one(void) {
  ay38910_init(&psg);
  select_register(4);
  ay38910_access(&psg, AY38910_WRITE, 0x11);
  ay38910_access(&psg, AY38910_WRITE, 0x22);
  TEST_EQUAL(psg.registers[4], 0x22);
  TEST_EQUAL(psg.registers[5], 0);
}

static void the_inactive_function_touches_nothing(void) {
  ay38910_init(&psg);
  select_register(2);
  ay38910_access(&psg, AY38910_INACTIVE, 0x99);
  TEST_EQUAL(psg.selected, 2);
  TEST_EQUAL(psg.registers[2], 0);
  /* Inactive leaves the bus as it found it. */
  TEST_EQUAL(ay38910_access(&psg, AY38910_INACTIVE, 0x99), 0x99);
}

static void port_a_reads_the_matrix_while_it_is_an_input(void) {
  ay38910_init(&psg);
  ay38910_present_port_a(&psg, 0xBE);
  select_register(AY38910_PORT_A);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0xBE);
}

static void port_a_reads_its_own_latch_once_it_is_an_output(void) {
  ay38910_init(&psg);
  ay38910_present_port_a(&psg, 0xBE);
  select_register(AY38910_MIXER);
  ay38910_access(&psg, AY38910_WRITE, 0x40); /* bit 6: port A becomes an output */
  TEST_CHECK(!ay38910_port_a_is_input(&psg));
  select_register(AY38910_PORT_A);
  ay38910_access(&psg, AY38910_WRITE, 0x17);
  TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x17);
}

static void write_register(uint8_t number, uint8_t value) {
  select_register(number);
  ay38910_access(&psg, AY38910_WRITE, value);
}

static void run_clocks(long clocks) {
  for (long clock = 0; clock < clocks; clock++) {
    ay38910_tick(&psg);
  }
}

/* Clocks from now until the given output next changes, or -1. */
static long clocks_to_change(int channel, long limit) {
  uint8_t level = psg.output[channel];
  for (long clock = 1; clock <= limit; clock++) {
    ay38910_tick(&psg);
    if (psg.output[channel] != level) {
      return clock;
    }
  }
  return -1;
}

/* "f_T = f_CLOCK / 16TP" (GI's data manual, §3.1), so each half of the
   wave lasts 8·TP clocks; a period of 0 behaves as 1 on the die. */
static void a_tone_lasts_sixteen_clocks_a_period_step(void) {
  static const uint16_t periods[] = {1, 2, 3, 100, 0xFFF};
  for (size_t index = 0; index < sizeof periods / sizeof periods[0]; index++) {
    ay38910_init(&psg);
    write_register(AY38910_MIXER, 0x3E); /* tone on A, the rest off */
    write_register(8, 15);
    write_register(0, (uint8_t)periods[index]);
    write_register(1, (uint8_t)(periods[index] >> 8));
    clocks_to_change(0, 8L * 0x1000 * 2);
    for (int edge = 0; edge < 4; edge++) {
      TEST_EQUAL(clocks_to_change(0, 8L * 0x1000 * 2), 8L * periods[index]);
    }
  }
  ay38910_init(&psg);
  write_register(AY38910_MIXER, 0x3E);
  write_register(8, 15);
  clocks_to_change(0, 64);
  TEST_EQUAL(clocks_to_change(0, 64), 8);
}

/* "f_N = f_CLOCK / 16NP" (§3.2): the register moves once every 16·NP
   clocks. Its taps at stages 17 and 14 give the longest sequence such a
   register has, 2^17 - 1 moves. */
static void the_noise_moves_every_sixteen_clocks_a_period_step(void) {
  static const uint8_t periods[] = {0, 1, 2, 31};
  for (size_t index = 0; index < sizeof periods / sizeof periods[0]; index++) {
    ay38910_init(&psg);
    write_register(6, periods[index]);
    uint8_t period = periods[index] == 0 ? 1 : periods[index];
    uint32_t last = psg.noise_register;
    long since = -1;
    int moves = 0;
    for (long clock = 0; clock < 16L * 32 * 10 && moves < 6; clock++) {
      ay38910_tick(&psg);
      if (since >= 0) {
        since++;
      }
      if (psg.noise_register != last) {
        if (since >= 0) {
          TEST_EQUAL(since, 16L * period);
          moves++;
        }
        since = 0;
        last = psg.noise_register;
      }
    }
    TEST_EQUAL(moves, 6);
  }
  ay38910_init(&psg);
  write_register(6, 1);
  run_clocks(16L * 4);
  uint32_t start = psg.noise_register;
  long moves = 0;
  do {
    run_clocks(16);
    moves++;
  } while (psg.noise_register != start && moves < 200000);
  TEST_EQUAL(moves, 131071);
}

/* The levels each shape takes over its first cycles, as GI's Fig. 7 draws
   them, given cycle and step. */
static uint8_t drawn_level(uint8_t shape, int cycle, int step) {
  bool continuing = shape & 0x08, attack = shape & 0x04, alternate = shape & 0x02,
       hold = shape & 0x01;
  uint8_t rising = (uint8_t)step, falling = (uint8_t)(15 - step);
  if (cycle == 0) {
    return attack ? rising : falling;
  }
  if (!continuing) {
    return 0;
  }
  if (hold) {
    return (attack != alternate) ? 15 : 0;
  }
  bool up = alternate && (cycle & 1) ? !attack : attack;
  return up ? rising : falling;
}

/* "EP IS THE ENVELOPE PERIOD (DURATION OF ONE CYCLE)" and a cycle has
   sixteen states (GI's data manual, Fig. 7 and §3.5.2): each level lasts
   16·EP clocks. */
static void every_envelope_shape_takes_the_levels_its_figure_draws(void) {
  for (uint8_t shape = 0; shape < 16; shape++) {
    ay38910_init(&psg);
    write_register(AY38910_MIXER, 0x3F); /* the amplitude alone */
    write_register(8, 0x10);
    write_register(11, 3);
    write_register(12, 0);
    write_register(13, shape);
    /* Steps from the restart: the first level, then one every 16·EP. */
    long first = clocks_to_change(0, 16L * 3 * 2);
    TEST_CHECK(first > 0 || shape == 0x0B || shape == 0x0D || (shape & 0x08) == 0);
    ay38910_init(&psg);
    write_register(AY38910_MIXER, 0x3F);
    write_register(8, 0x10);
    write_register(11, 3);
    write_register(12, 0);
    write_register(13, shape);
    TEST_EQUAL(psg.output[0], drawn_level(shape, 0, 0));
    /* Align on the first step, then read the middle of each level. */
    long offset = 0;
    while (psg.envelope_step == 0 && offset < 16L * 3 * 2) {
      ay38910_tick(&psg);
      offset++;
    }
    run_clocks(8L * 3);
    for (int cycle = 0; cycle < 4; cycle++) {
      for (int step = cycle == 0 ? 1 : 0; step < 16; step++) {
        TEST_EQUAL(psg.output[0], drawn_level(shape, cycle, step));
        run_clocks(16L * 3);
      }
    }
  }
}

/* "Disabling noise and tone does not turn off a channel" (§3.3). */
static void a_channel_with_tone_and_noise_disabled_gives_its_amplitude(void) {
  ay38910_init(&psg);
  write_register(AY38910_MIXER, 0x3F);
  write_register(9, 11);
  for (int clock = 0; clock < 4096; clock++) {
    ay38910_tick(&psg);
    TEST_EQUAL(psg.output[1], 11);
  }
  write_register(9, 4);
  TEST_EQUAL(psg.output[1], 4);
}

/* The die restarts the envelope at every write to register 13, the same
   shape too, and clears the period's count, so the first step comes the
   same number of steps after the write wherever in its period the write
   falls. The divider is not cleared, so the writes here keep its phase. */
static void a_write_to_register_13_restarts_the_envelope_and_its_period(void) {
  long first_steps[5];
  for (int phase = 0; phase < 5; phase++) {
    ay38910_init(&psg);
    write_register(AY38910_MIXER, 0x3F);
    write_register(10, 0x10);
    write_register(11, 5);
    write_register(13, 0x08); /* falling, repeating */
    run_clocks(1000 + 8L * phase);
    write_register(13, 0x08);
    TEST_EQUAL(psg.output[2], 15);
    first_steps[phase] = clocks_to_change(2, 16L * 5 * 2);
  }
  /* Each write falls at the divider's phase 0, with f1 high, so the count
     is held until the compare 3 clocks on; the die's model puts the first
     step 16·EP clocks after that compare, 83 after the write. */
  for (int phase = 0; phase < 5; phase++) {
    TEST_EQUAL(first_steps[phase], 3 + 16 * 5);
  }
}

/* Kevin Thacker's ACID "reg 8 h/w envelope update test" (psg.asm,
   reg_upd_test): with the envelope driving a channel, its amplitude
   register reads back the &10 written to it however the level moves. */
static void an_amplitude_reads_back_what_was_written_while_the_envelope_runs(void) {
  ay38910_init(&psg);
  write_register(8, 0x10);
  write_register(11, 0x30);
  write_register(12, 0);
  write_register(13, 0x30);
  select_register(8);
  for (int read = 0; read < 256; read++) {
    TEST_EQUAL(ay38910_access(&psg, AY38910_READ, 0), 0x10);
    run_clocks(97);
  }
}

/* deathsoft and lvd's Verilog model of the die (rtl/ay_model.v at
   cf8d14d), run under Icarus Verilog 12.0 with each trireg kept as a wire
   holding its last value, gave the levels in test/ay38910_model/: each
   scenario's three outputs, sampled late in every clock. Clock N there is
   its rising edge, then the clock's bus operation, then the sample; a
   reset holds the chip from a quarter into a clock until a quarter into
   the clock N later, and a write latches its address and writes inside
   one clock. */
static const char *const model_scenarios[] = {
    "address",
    "channels_bc",
    "envelope_period0",
    "envelope_period_carried",
    "envelope_period_phase",
    "envelope_restart_phase_period1",
    "envelope_restart_phase_period3",
    "envelope_shape_00",
    "envelope_shape_01",
    "envelope_shape_02",
    "envelope_shape_03",
    "envelope_shape_04",
    "envelope_shape_05",
    "envelope_shape_06",
    "envelope_shape_07",
    "envelope_shape_08",
    "envelope_shape_09",
    "envelope_shape_10",
    "envelope_shape_11",
    "envelope_shape_12",
    "envelope_shape_13",
    "envelope_shape_14",
    "envelope_shape_15",
    "minimal_restart",
    "mixer",
    "mixer_mid_tone",
    "noise",
    "noise_long",
    "noise_long_p3",
    "noise_period_carried",
    "noise_period_phase",
    "r13_phase_ep",
    "r13_restart_strides",
    "r13_zero_count",
    "reset",
    "retrigger",
    "sampling",
    "shapes_ep0",
    "shapes_ep2",
    "tone_coarse_phase",
    "tone_period",
    "tone_period_change",
    "tone_period_change_after_toggle",
    "tones_alone",
    "unheld_between",
    "unselected",
    "write_r8",
};

#define MODEL_DIRECTORY "test/ay38910_model/"
#define MOST_CHANGES 8192

typedef struct {
  long clock;
  uint8_t levels[AY38910_CHANNELS];
} change_t;

static change_t changes[MOST_CHANGES];

/* The numbers on a line, decimal or 0x hexadecimal, after `skip` words,
   up to a '#' or anything that is not one. */
static int numbers_on(const char *line, int skip, long *values, int most) {
  const char *at = line;
  for (int word = 0; word < skip; word++) {
    while (*at == ' ' || *at == '\t') {
      at++;
    }
    while (*at != '\0' && *at != ' ' && *at != '\t' && *at != '\n') {
      at++;
    }
  }
  int count = 0;
  while (count < most) {
    char *end = NULL;
    long value = strtol(at, &end, 0);
    if (end == at) {
      break;
    }
    values[count++] = value;
    at = end;
  }
  return count;
}

static long read_levels(const char *name, size_t *count) {
  char path[256];
  snprintf(path, sizeof path, MODEL_DIRECTORY "%s.levels", name);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    TEST_FAIL("cannot read %s", path);
    return -1;
  }
  char line[128];
  long end = -1;
  *count = 0;
  while (fgets(line, sizeof line, file) != NULL) {
    long values[4];
    if (line[0] == '#') {
      continue;
    }
    if (strncmp(line, "end ", 4) == 0 && numbers_on(line, 1, &end, 1) == 1) {
      break;
    }
    if (numbers_on(line, 0, values, 4) == 4 && *count < MOST_CHANGES) {
      changes[*count] =
          (change_t){values[0], {(uint8_t)values[1], (uint8_t)values[2], (uint8_t)values[3]}};
      (*count)++;
    }
  }
  fclose(file);
  return end;
}

typedef struct {
  long clock;
  long held_until; /* the clock in whose middle reset lets go */
  size_t change;
  size_t changes;
  long mismatches;
} replay_t;

/* The rising edge of the next clock, which a held chip does not see. */
static void edge(replay_t *replay) {
  replay->clock++;
  if (replay->clock <= replay->held_until) {
    ay38910_init(&psg);
  } else {
    ay38910_tick(&psg);
  }
}

static void sample(replay_t *replay, const char *name) {
  while (replay->change + 1 < replay->changes &&
         changes[replay->change + 1].clock <= replay->clock) {
    replay->change++;
  }
  const uint8_t *expected = changes[replay->change].levels;
  if (memcmp(psg.output, expected, AY38910_CHANNELS) != 0 && replay->mismatches++ < 3) {
    TEST_FAIL("%s, clock %ld: %u %u %u where the model has %u %u %u", name, replay->clock,
              psg.output[0], psg.output[1], psg.output[2], expected[0], expected[1], expected[2]);
  }
}

static void replay_scenario(const char *name) {
  replay_t replay = {.clock = -1, .held_until = -1};
  long end = read_levels(name, &replay.changes);
  char path[256];
  snprintf(path, sizeof path, MODEL_DIRECTORY "%s.txt", name);
  FILE *file = fopen(path, "r");
  if (file == NULL || end < 0 || replay.changes == 0) {
    TEST_FAIL("cannot read %s", path);
    if (file != NULL) {
      fclose(file);
    }
    return;
  }
  char line[256];
  while (fgets(line, sizeof line, file) != NULL) {
    char command[16] = "";
    size_t length = strcspn(line, " \t\n");
    if (length == 0 || length >= sizeof command || line[0] == '#') {
      continue;
    }
    memcpy(command, line, length);
    long operands[2] = {0, 0};
    if (numbers_on(line, 1, operands, 2) < 1) {
      continue;
    }
    long first = operands[0], second = operands[1];
    if (strcmp(command, "reset") == 0) {
      edge(&replay);
      ay38910_init(&psg);
      replay.held_until = replay.clock + first;
      sample(&replay, name);
      for (long clock = 1; clock < first; clock++) {
        edge(&replay);
        sample(&replay, name);
      }
    } else if (strcmp(command, "wait") == 0) {
      for (long clock = 0; clock < first; clock++) {
        edge(&replay);
        sample(&replay, name);
      }
    } else {
      edge(&replay);
      if (strcmp(command, "write") == 0) {
        select_register((uint8_t)first);
        ay38910_access(&psg, AY38910_WRITE, (uint8_t)second);
      } else if (strcmp(command, "address") == 0) {
        select_register((uint8_t)first);
      } else if (strcmp(command, "data") == 0) {
        ay38910_access(&psg, AY38910_WRITE, (uint8_t)first);
      }
      sample(&replay, name);
    }
  }
  fclose(file);
  TEST_EQUAL(replay.clock, end);
  TEST_EQUAL(replay.mismatches, 0);
}

static void every_clock_is_the_die_models(void) {
  for (size_t index = 0; index < sizeof model_scenarios / sizeof model_scenarios[0]; index++) {
    replay_scenario(model_scenarios[index]);
  }
}

int main(void) {
  TEST_RUN(reset_state);
  TEST_RUN(a_register_holds_what_it_is_given);
  TEST_RUN(a_register_keeps_only_the_bits_it_has);
  TEST_RUN(a_reset_chip_takes_no_write_until_it_is_selected);
  TEST_RUN(only_an_address_below_sixteen_selects_the_chip);
  TEST_RUN(no_address_above_fifteen_writes_a_register);
  TEST_RUN(an_address_above_fifteen_latches_nothing);
  TEST_RUN(an_unselected_chip_leaves_the_bus_as_it_found_it);
  TEST_RUN(an_address_that_matches_selects_the_chip_again);
  TEST_RUN(a_selection_lasts_until_the_next_one);
  TEST_RUN(the_inactive_function_touches_nothing);
  TEST_RUN(port_a_reads_the_matrix_while_it_is_an_input);
  TEST_RUN(port_a_reads_its_own_latch_once_it_is_an_output);
  TEST_RUN(a_tone_lasts_sixteen_clocks_a_period_step);
  TEST_RUN(the_noise_moves_every_sixteen_clocks_a_period_step);
  TEST_RUN(every_envelope_shape_takes_the_levels_its_figure_draws);
  TEST_RUN(a_channel_with_tone_and_noise_disabled_gives_its_amplitude);
  TEST_RUN(a_write_to_register_13_restarts_the_envelope_and_its_period);
  TEST_RUN(an_amplitude_reads_back_what_was_written_while_the_envelope_runs);
  TEST_RUN(every_clock_is_the_die_models);
  return TEST_REPORT("ay38910");
}
