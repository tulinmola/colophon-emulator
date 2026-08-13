#include "z80.h"

#include <string.h>

void z80_init(z80_t *cpu) {
  memset(cpu, 0, sizeof *cpu);
  cpu->a = 0xFF;
  cpu->f = 0xFF;
  cpu->sp = 0xFFFF;
}

/* T-state indices. Every instruction begins with the shared M1 (opcode fetch)
   machine cycle; decode at M1_T4 fills the instruction's micro-program, and
   the cycles below execute it. Each cycle's pin sequence and WAIT sampling is
   written once here, never per opcode. */
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

/* z80_micro_op.cycle: what kind of machine cycle */
enum {
  CYCLE_MEM_READ = 0,
  CYCLE_MEM_WRITE,
};

/* z80_micro_op.address: where the cycle's address comes from. Indirect
   accesses go through WZ, as on the silicon: decode preloads WZ (from BC, DE
   or the nn operand read into Z and W) and the cycle consumes it.
   WZ ("MEMPTR") rules per "MEMPTR, esoteric register of the Zilog Z80"
   (Boo-boo et al.), mirrored at
   https://raw.githubusercontent.com/floooh/emu-info/master/z80/memptr_eng.txt:
   single accesses leave WZ = address + 1; LD (addr),A leaves W = A. */
enum {
  ADDRESS_PC_INCREMENT = 0, /* the address is PC, which moves past it */
  ADDRESS_HL,               /* (HL) operands do not involve WZ */
  ADDRESS_WZ_INCREMENT,     /* the address is WZ, then WZ = WZ + 1 */
  ADDRESS_WZ,               /* the address is WZ, untouched */
  ADDRESS_WZ_THEN_A_HIGH,   /* store-A quirk: WZ = A:((WZ + 1) & 0xFF) */
};

/* z80_micro_op.data: which operand a cycle reads into or writes from.
   0..7 is the documented 8-bit register table (see z80_decode); the rest are
   internals the table cannot name. */
enum {
  REGISTER_B = 0,
  REGISTER_C,
  REGISTER_D,
  REGISTER_E,
  REGISTER_H,
  REGISTER_L,
  REGISTER_A = 7,
  DATA_Z, /* WZ low byte: operand and address latch */
  DATA_W, /* WZ high byte */
  DATA_SP_LOW,
  DATA_SP_HIGH,
  DATA_LATCH, /* plain temporary; LD (HL),n leaves WZ untouched, so Z cannot carry its n */
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

static uint8_t z80_get_operand(z80_t *cpu, uint8_t code) {
  switch (code) {
    case DATA_Z:
      return (uint8_t)(cpu->wz & 0xFF);
    case DATA_W:
      return (uint8_t)(cpu->wz >> 8);
    case DATA_SP_LOW:
      return (uint8_t)(cpu->sp & 0xFF);
    case DATA_SP_HIGH:
      return (uint8_t)(cpu->sp >> 8);
    case DATA_LATCH:
      return cpu->data_latch;
    default:
      return *z80_register8(cpu, code);
  }
}

static void z80_set_operand(z80_t *cpu, uint8_t code, uint8_t value) {
  switch (code) {
    case DATA_Z:
      cpu->wz = (uint16_t)((cpu->wz & 0xFF00) | value);
      break;
    case DATA_W:
      cpu->wz = (uint16_t)((cpu->wz & 0x00FF) | (value << 8));
      break;
    case DATA_SP_LOW:
      cpu->sp = (uint16_t)((cpu->sp & 0xFF00) | value);
      break;
    case DATA_SP_HIGH:
      cpu->sp = (uint16_t)((cpu->sp & 0x00FF) | (value << 8));
      break;
    case DATA_LATCH:
      cpu->data_latch = value;
      break;
    default:
      *z80_register8(cpu, code) = value;
      break;
  }
}

static void z80_append_cycle(z80_t *cpu, uint8_t cycle, uint8_t address, uint8_t data) {
  cpu->program[cpu->program_length++] = (z80_micro_op){cycle, address, data};
}

static void z80_instruction_done(z80_t *cpu) {
  cpu->q = 0; /* every instruction so far leaves flags alone */
  cpu->ei = false;
  cpu->p = 0;
  cpu->program_length = 0;
  cpu->program_index = 0;
  cpu->step = M1_T1;
}

/* Starts the micro-program's next machine cycle, or the next instruction when
   the program is exhausted. Address side effects (PC and WZ movement) happen
   here, at the cycle boundary. */
static void z80_start_next_cycle(z80_t *cpu) {
  if (cpu->program_index == cpu->program_length) {
    z80_instruction_done(cpu);
    return;
  }
  const z80_micro_op operation = cpu->program[cpu->program_index++];
  cpu->operand_data = operation.data;
  switch (operation.address) {
    case ADDRESS_PC_INCREMENT:
      cpu->operand_address = cpu->pc++;
      break;
    case ADDRESS_HL:
      cpu->operand_address = (uint16_t)((cpu->h << 8) | cpu->l);
      break;
    case ADDRESS_WZ_INCREMENT:
      cpu->operand_address = cpu->wz++;
      break;
    case ADDRESS_WZ:
      cpu->operand_address = cpu->wz;
      break;
    default: /* ADDRESS_WZ_THEN_A_HIGH */
      cpu->operand_address = cpu->wz;
      cpu->wz = (uint16_t)((cpu->a << 8) | ((cpu->wz + 1) & 0xFF));
      break;
  }
  cpu->step = operation.cycle == CYCLE_MEM_READ ? MEM_READ_T1 : MEM_WRITE_T1;
}

/* Opcode fields x/y/z/p/q per "Decoding Z80 Opcodes" (Cristian Dinu),
   http://www.z80.info/decoding.htm — the octal structure the silicon decodes:
   x = bits 7..6, y = bits 5..3, z = bits 2..0, y = 2p + q. The rp table (BC
   DE HL SP) is indexed by p. */
static const uint8_t register_pair_low[4] = {REGISTER_C, REGISTER_E, REGISTER_L, DATA_SP_LOW};
static const uint8_t register_pair_high[4] = {REGISTER_B, REGISTER_D, REGISTER_H, DATA_SP_HIGH};

static void z80_decode(z80_t *cpu) {
  const uint8_t x = cpu->opcode >> 6;
  const uint8_t y = (cpu->opcode >> 3) & 7;
  const uint8_t z = cpu->opcode & 7;
  const uint8_t p = y >> 1;
  const uint8_t q = y & 1;

  if (x == 0) {
    switch (z) {
      case 1:
        if (q == 0) { /* LD rp[p],nn */
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, register_pair_low[p]);
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, register_pair_high[p]);
        }
        break;
      case 2:
        switch (p) {
          case 0: /* LD (BC),A / LD A,(BC) */
          case 1: /* LD (DE),A / LD A,(DE) */
            cpu->wz =
                p == 0 ? (uint16_t)((cpu->b << 8) | cpu->c) : (uint16_t)((cpu->d << 8) | cpu->e);
            if (q == 0) {
              z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ_THEN_A_HIGH, REGISTER_A);
            } else {
              z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ_INCREMENT, REGISTER_A);
            }
            break;
          case 2: /* LD (nn),HL / LD HL,(nn) */
            z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
            z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W);
            if (q == 0) {
              z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ_INCREMENT, REGISTER_L);
              z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ, REGISTER_H);
            } else {
              z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ_INCREMENT, REGISTER_L);
              z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ, REGISTER_H);
            }
            break;
          default: /* LD (nn),A / LD A,(nn) */
            z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
            z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W);
            if (q == 0) {
              z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ_THEN_A_HIGH, REGISTER_A);
            } else {
              z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ_INCREMENT, REGISTER_A);
            }
            break;
        }
        break;
      case 6:
        if (y == 6) { /* LD (HL),n */
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_LATCH);
          z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_HL, DATA_LATCH);
        } else { /* LD r,n */
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, y);
        }
        break;
      default: /* 0x00 NOP; the rest of the x=0 page is not implemented yet */
        break;
    }
  } else if (x == 1 && cpu->opcode != 0x76) { /* the LD r,r' page; 0x76 is HALT */
    if (z == 6) {                             /* LD r,(HL) */
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_HL, y);
    } else if (y == 6) { /* LD (HL),r */
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_HL, z);
    } else {
      *z80_register8(cpu, y) = *z80_register8(cpu, z);
    }
  }
  /* everything unimplemented decodes to an empty program and runs as NOP, so
     a machine can keep ticking */
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
      z80_start_next_cycle(cpu);
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
      z80_set_operand(cpu, cpu->operand_data, z80_data(pins));
      pins &= ~Z80_OUT_PINS;
      z80_start_next_cycle(cpu);
      break;

    case MEM_WRITE_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->operand_address);
      cpu->step = MEM_WRITE_T2;
      break;

    case MEM_WRITE_T2:
      pins = z80_set_data(pins, z80_get_operand(cpu, cpu->operand_data)) | Z80_MREQ | Z80_WR;
      cpu->step = MEM_WRITE_T3;
      break;

    case MEM_WRITE_T3:
      if (pins & Z80_WAIT) {
        return pins;
      }
      pins &= ~Z80_OUT_PINS;
      z80_start_next_cycle(cpu);
      break;

    default: /* unreachable; recover to an instruction boundary */
      cpu->step = M1_T1;
      break;
  }
  return pins;
}
