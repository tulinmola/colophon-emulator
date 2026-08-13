#include "z80.h"

#include <string.h>

void z80_init(z80_t *cpu) {
  memset(cpu, 0, sizeof *cpu);
  cpu->a = 0xFF;
  cpu->f = 0xFF;
  cpu->sp = 0xFFFF;
}

/* T-state indices. Every instruction begins with the shared M1 (opcode fetch)
   machine cycle; decode at M1_T4 chains the instruction's remaining machine
   cycles. Each cycle's pin sequence and WAIT sampling is written once here,
   never per opcode. */
enum {
  M1_T1 = 0,
  M1_T2,
  M1_T3,
  M1_T4,
  MEM_READ_T1,
  MEM_READ_T2,
  MEM_READ_T3,
  MEM_WRITE_T1,
  MEM_WRITE_T2,
  MEM_WRITE_T3,
};

/* 8-bit register table per the decoding doc (see z80_decode): B C D E H L
   (HL) A. Index 6 is the (HL) memory operand, not a register; decode routes
   it to a memory cycle before this is called. */
static uint8_t *z80_register8(z80_t *cpu, uint8_t index) {
  switch (index) {
    case 0:
      return &cpu->b;
    case 1:
      return &cpu->c;
    case 2:
      return &cpu->d;
    case 3:
      return &cpu->e;
    case 4:
      return &cpu->h;
    case 5:
      return &cpu->l;
    default:
      return &cpu->a;
  }
}

static void z80_instruction_done(z80_t *cpu) {
  cpu->q = 0; /* every instruction so far leaves flags alone */
  cpu->ei = false;
  cpu->p = 0;
  cpu->step = M1_T1;
}

/* Opcode fields x/y/z per "Decoding Z80 Opcodes" (Cristian Dinu),
   http://www.z80.info/decoding.htm — the octal structure the silicon decodes:
   x = bits 7..6, y = bits 5..3, z = bits 2..0. */
static void z80_decode(z80_t *cpu) {
  const uint8_t x = cpu->opcode >> 6;
  const uint8_t y = (cpu->opcode >> 3) & 7;
  const uint8_t z = cpu->opcode & 7;
  if (x == 1 && cpu->opcode != 0x76) { /* the LD r,r' page; 0x76 is HALT */
    const uint16_t hl = (uint16_t)((cpu->h << 8) | cpu->l);
    if (z == 6) { /* LD r,(HL) */
      cpu->operand_address = hl;
      cpu->operand_register = y;
      cpu->step = MEM_READ_T1;
      return;
    }
    if (y == 6) { /* LD (HL),r */
      cpu->operand_address = hl;
      cpu->operand_register = z;
      cpu->step = MEM_WRITE_T1;
      return;
    }
    *z80_register8(cpu, y) = *z80_register8(cpu, z);
    z80_instruction_done(cpu);
    return;
  }
  /* 0x00 NOP; every unimplemented opcode also runs as NOP so a machine can
     keep ticking */
  z80_instruction_done(cpu);
}

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
         Array is wired up. Same for the memory cycles below. */
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
      z80_decode(cpu);
      break;

    case MEM_READ_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->operand_address);
      cpu->step = MEM_READ_T2;
      break;

    case MEM_READ_T2:
      pins |= Z80_MREQ | Z80_RD;
      cpu->step = MEM_READ_T3;
      break;

    case MEM_READ_T3:
      if (pins & Z80_WAIT) {
        return pins;
      }
      *z80_register8(cpu, cpu->operand_register) = z80_data(pins);
      pins &= ~Z80_OUT_PINS;
      z80_instruction_done(cpu);
      break;

    case MEM_WRITE_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->operand_address);
      cpu->step = MEM_WRITE_T2;
      break;

    case MEM_WRITE_T2:
      pins = z80_set_data(pins, *z80_register8(cpu, cpu->operand_register)) | Z80_MREQ | Z80_WR;
      cpu->step = MEM_WRITE_T3;
      break;

    case MEM_WRITE_T3:
      if (pins & Z80_WAIT) {
        return pins;
      }
      pins &= ~Z80_OUT_PINS;
      z80_instruction_done(cpu);
      break;

    default: /* unreachable; recover to an instruction boundary */
      cpu->step = M1_T1;
      break;
  }
  return pins;
}
