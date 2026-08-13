/*
 * z80.h — Zilog Z80 CPU, cycle-stepped.
 *
 * One z80_tick() call advances the CPU by one T-state (one clock cycle). The
 * CPU is not a controller: it is a chip that reads and drives bus pins each
 * tick, and the machine wiring around it decides what those pins mean. All
 * machine-specific timing (e.g. the Amstrad CPC's 4T alignment) is produced
 * by wiring — this file knows nothing about any machine.
 *
 * Sources:
 * - "The Undocumented Z80 Documented" (Sean Young),
 *   https://raw.githubusercontent.com/floooh/emu-info/master/z80/z80-documented.pdf
 *   — register model incl. WZ, reset state, undocumented flag behavior.
 * - SingleStepTests/z80, https://github.com/SingleStepTests/z80 (MIT) — our
 *   per-cycle ground truth. We adopt its bus conventions: MREQ/RD and MREQ/WR
 *   pulse for a single T-state, and the refresh address I:R is on the address
 *   bus during T3/T4 of an opcode fetch. Its `q` (flags-modified tracker) and
 *   `p` (LD A,I / LD A,R tracker) internals are modeled as state fields.
 * - "A new cycle-stepped Z80 emulator" (Andre Weissflog),
 *   https://floooh.github.io/2021/12/17/cycle-stepped-z80.html — the
 *   architectural pattern: pin-mask bus, CPU as an ordinary tickable chip. We
 *   follow its pin-layout convention (address bits 0-15, data bits 16-23) and
 *   its tick API.
 */
#ifndef COLOPHON_Z80_H
#define COLOPHON_Z80_H

#include <stdbool.h>
#include <stdint.h>

/* Bus pin layout in the 64-bit pin mask:
 * bits 0..15  A0..A15 (address bus)
 * bits 16..23 D0..D7  (data bus)
 * bits 24..   control pins
 * Pin names are the datasheet's. Bit set = pin asserted: on silicon most of
 * these are active-low (/MREQ), but the mask models assertion, not voltage. */
#define Z80_M1 (1ULL << 24)    /* opcode fetch cycle in progress */
#define Z80_MREQ (1ULL << 25)  /* memory request */
#define Z80_IORQ (1ULL << 26)  /* I/O request */
#define Z80_RD (1ULL << 27)    /* read */
#define Z80_WR (1ULL << 28)    /* write */
#define Z80_RFSH (1ULL << 29)  /* refresh address on bus */
#define Z80_HALT (1ULL << 30)  /* CPU halted */
#define Z80_WAIT (1ULL << 31)  /* input: stretch the current machine cycle */
#define Z80_INT (1ULL << 32)   /* input: maskable interrupt request */
#define Z80_NMI (1ULL << 33)   /* input: non-maskable interrupt request */
#define Z80_RESET (1ULL << 34) /* input: reset */

/* Pins driven by the CPU, cleared and re-driven every tick. Input pins
 * (WAIT/INT/NMI/RESET) and the data bus are owned by the machine wiring. */
#define Z80_OUT_PINS (Z80_M1 | Z80_MREQ | Z80_IORQ | Z80_RD | Z80_WR | Z80_RFSH | Z80_HALT)

static inline uint16_t z80_address(uint64_t pins) { return (uint16_t)(pins & 0xFFFF); }
static inline uint8_t z80_data(uint64_t pins) { return (uint8_t)((pins >> 16) & 0xFF); }
static inline uint64_t z80_set_address(uint64_t pins, uint16_t address) {
  return (pins & ~0xFFFFULL) | address;
}
static inline uint64_t z80_set_data(uint64_t pins, uint8_t data) {
  return (pins & ~0xFF0000ULL) | ((uint64_t)data << 16);
}

/* Flag register bits. X and Y are the undocumented copies of result bits 3
 * and 5 ("The Undocumented Z80 Documented" ch. 2). */
#define Z80_FLAG_C (1 << 0)  /* carry */
#define Z80_FLAG_N (1 << 1)  /* add/subtract */
#define Z80_FLAG_PV (1 << 2) /* parity/overflow */
#define Z80_FLAG_X (1 << 3)
#define Z80_FLAG_H (1 << 4) /* half carry */
#define Z80_FLAG_Y (1 << 5)
#define Z80_FLAG_Z (1 << 6) /* zero */
#define Z80_FLAG_S (1 << 7) /* sign */

/* One machine cycle of an instruction's micro-program: what kind of cycle,
 * where its address comes from, which operand it moves, and how many internal
 * T-states stretch it at the end (see z80.c). */
typedef struct {
  uint8_t cycle;
  uint8_t address;
  uint8_t data;
  uint8_t stretch;
} z80_micro_op;

typedef struct {
  /* main register set */
  uint8_t a, f, b, c, d, e, h, l;
  /* shadow set AF' BC' DE' HL': the underscore renders the prime mark */
  uint16_t af_, bc_, de_, hl_;
  uint16_t ix, iy, sp, pc;
  uint16_t wz; /* internal address latch ("MEMPTR") */
  uint8_t i, r;
  uint8_t im; /* interrupt mode 0..2 */
  bool iff1, iff2;
  bool halted; /* HALT executed: fetches idle as NOPs, PC held, HALT pin asserted */
  bool ei;     /* EI just executed: interrupt acceptance blocked for one instruction */
  uint8_t p;   /* last instruction was LD A,I / LD A,R (IFF2-read bug tracking) */
  uint8_t q;   /* copy of F if the last instruction modified flags, else 0 */

  /* cycle-stepping state: which T-state of the current instruction is next */
  uint8_t step;
  uint8_t opcode;
  /* the instruction's machine cycles after M1, filled at decode */
  z80_micro_op program[4];
  uint8_t program_length;
  uint8_t program_index;
  /* latched for the machine cycle in progress */
  uint16_t operand_address;
  uint8_t operand_data;      /* operand code, see z80.c */
  uint8_t stretch_remaining; /* internal T-states left in the current cycle */
  uint8_t data_latch;        /* internal temporary for operands in flight that must not touch WZ */
  uint8_t alu_operation;     /* operation latched at decode for cycles that compute, see z80.c */
  uint8_t finish;            /* work applied when the program ends, see z80.c */
} z80_t;

/* Reset state per "The Undocumented Z80 Documented": AF=FFFF, SP=FFFF,
 * PC/I/R/IM zero, interrupts disabled; remaining registers are undefined on
 * real silicon (zero here, for determinism). */
void z80_init(z80_t *cpu);

/* Advance one T-state. Takes the current bus pins, returns the new ones. */
uint64_t z80_tick(z80_t *cpu, uint64_t pins);

#endif
