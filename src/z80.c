#include "z80.h"

#include <string.h>

void z80_init(z80_t *cpu) {
  memset(cpu, 0, sizeof *cpu);
  cpu->a = 0xFF;
  cpu->f = 0xFF;
  cpu->sp = 0xFFFF;
}

/* The M1 (opcode fetch) machine cycle is shared by every instruction;
   multi-cycle instructions continue in steps appended after M1_T4. */
enum {
  M1_T1 = 0,
  M1_T2,
  M1_T3,
  M1_T4,
};

uint64_t z80_tick(z80_t *cpu, uint64_t pins) {
  switch (cpu->step) {
    case M1_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->pc) | Z80_M1;
      cpu->step = M1_T2;
      break;

    case M1_T2:
      pins |= Z80_MREQ | Z80_RD;
      cpu->pc++;
      cpu->step = M1_T3;
      break;

    case M1_T3:
      /* WAIT holds the machine cycle for as long as it is asserted; this is how
         a machine stretches cycles (the CPC Gate Array asserts it 3 of every 4
         clocks). Provisional: exact sample timing to be validated when the Gate
         Array is wired up. */
      if (pins & Z80_WAIT) {
        return pins;
      }
      cpu->opcode = z80_data(pins);
      pins = z80_set_address(pins & ~Z80_OUT_PINS, (uint16_t)((cpu->i << 8) | cpu->r)) | Z80_RFSH;
      /* R is a 7-bit counter; bit 7 changes only via LD R,A. */
      cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F);
      cpu->step = M1_T4;
      break;

    case M1_T4:
      pins = (pins & ~Z80_OUT_PINS) | Z80_RFSH;
      switch (cpu->opcode) {
        case 0x00: /* NOP */
        default:   /* unimplemented: runs as NOP so a machine can keep ticking */
          break;
      }
      cpu->q = 0; /* no opcode so far modifies flags */
      cpu->ei = false;
      cpu->p = 0;
      cpu->step = M1_T1;
      break;

    default: /* unreachable; recover to an instruction boundary */
      cpu->step = M1_T1;
      break;
  }
  return pins;
}
