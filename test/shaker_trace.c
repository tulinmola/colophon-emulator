/*
 * shaker_trace.c — see shaker_trace.h.
 */
#include "shaker_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool tracing;
static char trace_path[512];
static int trace_file = -1;
static long first_frame = -1000000;
static long last_frame = 1000000000;
static long lowest_pc;
static long highest_pc = 0xFFFF;
static bool syncs_wanted;
static bool reads_wanted;

static long ticks_into_the_group;
static char group_key[16];
static bool group_announced = true;

static uint64_t pins_before;
static bool hsync_before;
static uint8_t r52_before;
static bool request_before;

/* A setting of the form from:to, both ends within bounds and in order. One
   that does not read leaves the range whole and says so, rather than tracing
   something other than what was asked for without a word. */
static void read_range(const char *name, int base, long lowest, long highest, long *from,
                       long *to) {
  const char *text = getenv(name);
  if (text == NULL) {
    return;
  }
  char *end;
  errno = 0;
  long first = strtol(text, &end, base);
  if (errno == 0 && end != text && *end == ':') {
    const char *rest = end + 1;
    long second = strtol(rest, &end, base);
    if (errno == 0 && end != rest && *end == '\0' && lowest <= first && first <= second &&
        second <= highest) {
      *from = first;
      *to = second;
      return;
    }
  }
  fprintf(stderr, "shaker: %s wants from:to, and %s is not that; tracing all of it\n", name, text);
}

static void read_events(void) {
  const char *text = getenv("SHAKER_TRACE_EVENTS");
  if (text == NULL) {
    return;
  }
  char words[128];
  snprintf(words, sizeof words, "%s", text);
  for (char *word = strtok(words, ","); word != NULL; word = strtok(NULL, ",")) {
    if (strcmp(word, "syncs") == 0) {
      syncs_wanted = true;
    } else if (strcmp(word, "reads") == 0) {
      reads_wanted = true;
    } else {
      fprintf(stderr, "shaker: SHAKER_TRACE_EVENTS knows syncs and reads, not %s\n", word);
    }
  }
}

void shaker_trace_configure(void) {
  const char *prefix = getenv("SHAKER_TRACE");
  if (prefix == NULL || *prefix == '\0') {
    return;
  }
  tracing = true;
  read_range("SHAKER_TRACE_FRAMES", 10, -1000000, 1000000000, &first_frame, &last_frame);
  read_range("SHAKER_TRACE_PC", 16, 0, 0xFFFF, &lowest_pc, &highest_pc);
  read_events();
}

void shaker_trace_begin(const char *module, uint8_t crtc_type) {
  if (tracing) {
    snprintf(trace_path, sizeof trace_path, "%s-%s-crtc%u.txt", getenv("SHAKER_TRACE"), module,
             crtc_type);
  }
}

/* A frame as the report numbers it: the key press at and below zero, and
   frame N once N frames have run since the key was let go. */
static long frame(void) {
  long quotient = ticks_into_the_group / CPC_TICKS_PER_STANDARD_FRAME;
  if (ticks_into_the_group < 0 && ticks_into_the_group % CPC_TICKS_PER_STANDARD_FRAME != 0) {
    quotient--;
  }
  return quotient + 1;
}

/* The child _exit()s, which neither flushes stdio nor runs anything
   registered to run at exit, so every line goes out with write(2) as it is
   made. The file is opened on the first line of all. */
static void write_line(const char *line, int length) {
  if (trace_file < 0) {
    trace_file = open(trace_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (trace_file < 0) {
      fprintf(stderr, "shaker: cannot write a trace to %s\n", trace_path);
      tracing = false;
      return;
    }
  }
  if (length > 0) {
    (void)!write(trace_file, line, (size_t)length);
  }
}

static void emit(const cpc_t *cpc, const char *what) {
  char line[160];
  int length;
  if (!group_announced) {
    group_announced = true;
    length = snprintf(line, sizeof line, "---- group %s\n", group_key);
    write_line(line, length);
  }
  length = snprintf(line, sizeof line, "f%-5ld C0=%02X C9=%02X C4=%02X q%u pc=%04X  %s\n", frame(),
                    cpc->crtc.c0, cpc->crtc.c9, cpc->crtc.c4, cpc->gate_array.cpu_phase,
                    cpc->cpu.pc, what);
  write_line(line, length);
}

static void remember(const cpc_t *cpc, uint64_t pins) {
  pins_before = pins;
  hsync_before = (cpc->crtc_pins & CRTC_HSYNC) != 0;
  r52_before = cpc->gate_array.r52;
  request_before = gate_array_interrupt(&cpc->gate_array);
}

void shaker_trace_group(const cpc_t *cpc, const char *key, long frames_before_counting) {
  if (!tracing) {
    return;
  }
  /* The machine has just been put back to where the group starts from, so
     what the last tick of the previous group left is no measure of change. */
  remember(cpc, cpc->pins);
  ticks_into_the_group = -frames_before_counting * CPC_TICKS_PER_STANDARD_FRAME;
  snprintf(group_key, sizeof group_key, "%s", key);
  group_announced = false;
}

/* The first tick of a cycle the Gate Array holds over several: a device is
   touched once per cycle, and so is the trace. */
static bool cycle_begins(uint64_t pins, uint64_t wanted) {
  return (pins & wanted) == wanted && (pins_before & wanted) != wanted;
}

static void trace_the_processor(const cpc_t *cpc, uint64_t pins) {
  char what[64];
  if (cycle_begins(pins, Z80_M1 | Z80_IORQ)) {
    emit(cpc, "interrupt acknowledged");
    return;
  }
  uint16_t address = z80_address(pins);
  if (cycle_begins(pins, Z80_IORQ | Z80_WR)) {
    /* Devices decode single address lines: the Gate Array answers A15 high
       and A14 low, the CRTC A14 low with A9 and A8 choosing its function
       ("I/O port allocation", Rison & Thacker). */
    if ((address & 0xC000) == 0x4000) {
      snprintf(what, sizeof what, "Gate Array <- %02X", z80_data(pins));
      emit(cpc, what);
    }
    if ((address & 0x4000) == 0 && (address & 0x0300) == 0x0000) {
      snprintf(what, sizeof what, "select R%u", cpc->crtc.address_register);
      emit(cpc, what);
    } else if ((address & 0x4000) == 0 && (address & 0x0300) == 0x0100) {
      snprintf(what, sizeof what, "R%u <- %02X", cpc->crtc.address_register, z80_data(pins));
      emit(cpc, what);
    }
  }
  if (reads_wanted && cycle_begins(pins, Z80_IORQ | Z80_RD) && (pins & Z80_M1) == 0) {
    snprintf(what, sizeof what, "in %04X -> %02X", address, z80_data(pins));
    emit(cpc, what);
  }
}

static void trace_the_machine(const cpc_t *cpc) {
  char what[64];
  bool request = gate_array_interrupt(&cpc->gate_array);
  if (request != request_before) {
    emit(cpc, request ? "interrupt requested" : "interrupt withdrawn");
  }
  if (!syncs_wanted) {
    return;
  }
  bool hsync = (cpc->crtc_pins & CRTC_HSYNC) != 0;
  if (hsync != hsync_before) {
    emit(cpc, hsync ? "HSYNC begins" : "HSYNC ends");
  }
  if (cpc->gate_array.r52 != r52_before) {
    snprintf(what, sizeof what, "R52 %u -> %u", r52_before, cpc->gate_array.r52);
    emit(cpc, what);
  }
}

void shaker_trace_tick(const cpc_t *cpc, uint64_t pins) {
  if (!tracing) {
    return;
  }
  long now = frame();
  if (now >= first_frame && now <= last_frame) {
    if (cpc->cpu.pc >= lowest_pc && cpc->cpu.pc <= highest_pc) {
      trace_the_processor(cpc, pins);
    }
    trace_the_machine(cpc);
  }
  ticks_into_the_group++;
  remember(cpc, pins);
}
