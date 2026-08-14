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
  /* I/O cycles are four T-states: the silicon inserts one wait state itself,
     so the pulse sits in the third and data passes in the fourth */
  IO_READ_T1,
  IO_READ_T2,
  IO_READ_T3,
  IO_READ_T4,
  IO_WRITE_T1,
  IO_WRITE_T2,
  IO_WRITE_T3,
  IO_WRITE_T4,
  /* The maskable interrupt acknowledge cycle: M1 with IORQ instead of MREQ,
     and two wait states the CPU inserts itself so a daisy chain has time to
     settle. External WAIT extends it further, as on any other cycle. */
  INT_ACK_T1,
  INT_ACK_T2,
  INT_ACK_WAIT,
  INT_ACK_T3,
  INT_ACK_T4,
  STRETCH_T, /* internal T-states: bus released, address held, counted down */
};

/* z80_t.accepting: which interrupt sequence is running instead of an
   instruction. NMI's acknowledge is an ordinary opcode fetch whose byte is
   discarded and whose PC does not move; INT's is the cycle above. */
enum {
  ACCEPT_NONE = 0,
  ACCEPT_NMI,
  ACCEPT_INT,
};

/* z80_micro_op.cycle: what kind of machine cycle. Any cycle can carry stretch
   T-states at its end (read-modify-write reads, the second write of
   EX (SP),HL); CYCLE_INTERNAL is stretch alone, the bus idling while the CPU
   computes (16-bit arithmetic, PUSH's pre-decrement setup). */
enum {
  CYCLE_MEM_READ = 0,
  CYCLE_MEM_WRITE,
  CYCLE_IO_READ,
  CYCLE_IO_WRITE,
  CYCLE_INTERNAL,
};

/* z80_micro_op.address: where the cycle's address comes from. Indirect
   accesses go through WZ, as on the silicon: decode preloads WZ (from BC, DE
   or the nn operand read into Z and W) and the cycle consumes it.
   WZ ("MEMPTR") rules per "MEMPTR, esoteric register of the Zilog Z80"
   (Boo-boo et al.), mirrored at
   https://raw.githubusercontent.com/floooh/emu-info/master/z80/memptr_eng.txt:
   single accesses leave WZ = address + 1; LD (addr),A leaves W = A. */
enum {
  ADDRESS_NONE = 0,       /* internal cycles leave the bus address alone */
  ADDRESS_PC_INCREMENT,   /* the address is PC, which moves past it */
  ADDRESS_HL,             /* (HL) operands do not involve WZ */
  ADDRESS_WZ_INCREMENT,   /* the address is WZ, then WZ = WZ + 1 */
  ADDRESS_WZ,             /* the address is WZ, untouched */
  ADDRESS_WZ_THEN_A_HIGH, /* store-A quirk: WZ = A:((WZ + 1) & 0xFF) */
  ADDRESS_WZ_DECREMENT,   /* the address is WZ, then WZ = WZ - 1 (IND, OUTD) */
  ADDRESS_SP_INCREMENT,   /* the address is SP, then SP = SP + 1 (POP) */
  ADDRESS_SP_DECREMENT,   /* SP = SP - 1 first, then it is the address (PUSH) */
  ADDRESS_SP,
  ADDRESS_SP_PLUS_1,
  ADDRESS_DE, /* the LD block instructions write through DE */
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
  DATA_LATCH,   /* plain temporary; LD (HL),n leaves WZ untouched, so Z cannot carry its n */
  DATA_ALU,     /* the arriving byte feeds the latched ALU operation */
  DATA_INC_DEC, /* the arriving byte is incremented/decremented into the latch */
  DATA_F,       /* the flag register moved wholesale (PUSH/POP AF), not via the ALU */
  DATA_PC_LOW,  /* CALL and RST push the resume address */
  DATA_PC_HIGH,
  DATA_CB,           /* the arriving byte runs the latched CB operation into the latch */
  DATA_BIT,          /* the arriving byte is bit-tested; X/Y leak from W (see z80_bit) */
  DATA_INDEX_LOW,    /* L, or IXL/IYL under the active prefix */
  DATA_INDEX_HIGH,   /* H, or IXH/IYH under the active prefix */
  DATA_DISPLACEMENT, /* (IX+d)/(IY+d): the byte folds into WZ as the effective address */
  DATA_DDCB,         /* DD CB: the sub-opcode arrives as data and appends its own cycles */
  DATA_IN,           /* IN r,(C): flags from the value; register y, or none for y=6 */
  DATA_ROTATE_DIGIT, /* RRD/RLD: nibbles turn between A and the latch */
  DATA_BLOCK_LD,     /* LDI/LDD family: transfer bookkeeping on arrival */
  DATA_BLOCK_CP,
  DATA_BLOCK_IN,
  DATA_BLOCK_OUT,
  DATA_NONE, /* reads as zero: OUT (C),0, the NMOS behavior */
};

/* z80_t.finish: work applied when the micro-program ends, for results that
   only exist after the bus cycles ran. */
enum {
  FINISH_NONE = 0,
  FINISH_HL_FROM_WZ,       /* EX (SP),HL: the value read into WZ becomes HL */
  FINISH_PC_FROM_WZ,       /* jumps, calls, returns: the target travels through WZ */
  FINISH_JUMP_RELATIVE,    /* JR and DJNZ: PC moves by the signed displacement in the latch */
  FINISH_BLOCK_LD,         /* LDI/LDD family: DE moves; repeats rewind */
  FINISH_BLOCK_IN,         /* INI/IND family: HL moves; repeats rewind */
  FINISH_BLOCK_CP_REPEAT,  /* CPIR/CPDR taking another round */
  FINISH_BLOCK_OUT_REPEAT, /* OTIR/OTDR taking another round */
  FINISH_DDCB_COPY,        /* DD CB with z!=6 also lands the result in r[z] */
  FINISH_PC_FROM_VECTOR,   /* interrupt mode 2: the address read from the table */
};

/* z80_t.alu_operation: the alu table per the decoding doc, indexed by y, plus
   the increment/decrement pair which shares the deferred-compute path. */
enum {
  ALU_ADD = 0,
  ALU_ADC,
  ALU_SUB,
  ALU_SBC,
  ALU_AND,
  ALU_XOR,
  ALU_OR,
  ALU_CP,
  OPERATION_INCREMENT,
  OPERATION_DECREMENT,
};

/* CPIR/CPDR decide whether to repeat only once the compared byte arrives, and
   DD CB learns its operation from a byte read as data, so operand handlers
   extend the running program themselves. */
static void z80_append_internal(z80_t *cpu, uint8_t ticks);
static void z80_append_cycle(z80_t *cpu, uint8_t cycle, uint8_t address, uint8_t data);
static void z80_append_cycle_stretched(z80_t *cpu, uint8_t cycle, uint8_t address, uint8_t data,
                                       uint8_t stretch);

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

/* Under DD/FD, references to H and L become the index register's halves —
   except when the instruction addresses (IX+d), where decode uses the plain
   table instead. */
static uint8_t *z80_register8_indexed(z80_t *cpu, uint8_t index) {
  if (index == 4) {
    if (cpu->index_mode == 1) {
      return &cpu->ixh;
    }
    if (cpu->index_mode == 2) {
      return &cpu->iyh;
    }
  } else if (index == 5) {
    if (cpu->index_mode == 1) {
      return &cpu->ixl;
    }
    if (cpu->index_mode == 2) {
      return &cpu->iyl;
    }
  }
  return z80_register8(cpu, index);
}

static void z80_set_flags(z80_t *cpu, uint8_t flags) {
  cpu->f = flags;
  cpu->q = flags; /* Q mirrors F whenever an instruction writes flags */
}

/* S and Z from the result; X and Y copied from bits 3 and 5 of xy_source,
   which is the result for everything except CP, where the operand leaks
   through ("The Undocumented Z80 Documented" ch. 2). */
static uint8_t z80_flags_from_result(uint8_t result, uint8_t xy_source) {
  uint8_t flags = (uint8_t)(result & Z80_FLAG_S);
  if (result == 0) {
    flags |= Z80_FLAG_Z;
  }
  flags |= xy_source & (Z80_FLAG_X | Z80_FLAG_Y);
  return flags;
}

static uint8_t z80_flag_parity(uint8_t value) {
  uint8_t folded = value;
  folded ^= folded >> 4;
  folded ^= folded >> 2;
  folded ^= folded >> 1;
  return (folded & 1) ? 0 : Z80_FLAG_PV;
}

static void z80_alu(z80_t *cpu, uint8_t operation, uint8_t value) {
  const uint8_t a = cpu->a;
  uint8_t flags;
  switch (operation) {
    case ALU_ADD:
    case ALU_ADC: {
      const unsigned carry = operation == ALU_ADC ? (cpu->f & Z80_FLAG_C) : 0;
      const unsigned result = a + value + carry;
      const uint8_t result8 = (uint8_t)result;
      flags = z80_flags_from_result(result8, result8);
      if ((a ^ value ^ result) & 0x10) {
        flags |= Z80_FLAG_H;
      }
      if ((~(a ^ value) & (a ^ result)) & 0x80) {
        flags |= Z80_FLAG_PV; /* overflow: operands agree in sign, result does not */
      }
      if (result > 0xFF) {
        flags |= Z80_FLAG_C;
      }
      cpu->a = result8;
      break;
    }
    case ALU_SUB:
    case ALU_SBC:
    case ALU_CP: {
      const unsigned carry = operation == ALU_SBC ? (cpu->f & Z80_FLAG_C) : 0;
      const unsigned result = a - value - carry;
      const uint8_t result8 = (uint8_t)result;
      flags = z80_flags_from_result(result8, operation == ALU_CP ? value : result8) | Z80_FLAG_N;
      if ((a ^ value ^ result) & 0x10) {
        flags |= Z80_FLAG_H;
      }
      if (((a ^ value) & (a ^ result)) & 0x80) {
        flags |= Z80_FLAG_PV;
      }
      if (result & 0x100) {
        flags |= Z80_FLAG_C;
      }
      if (operation != ALU_CP) {
        cpu->a = result8;
      }
      break;
    }
    default: { /* AND, XOR, OR */
      const uint8_t result8 = operation == ALU_AND   ? (a & value)
                              : operation == ALU_XOR ? (a ^ value)
                                                     : (a | value);
      flags = z80_flags_from_result(result8, result8) | z80_flag_parity(result8);
      if (operation == ALU_AND) {
        flags |= Z80_FLAG_H;
      }
      cpu->a = result8;
      break;
    }
  }
  z80_set_flags(cpu, flags);
}

/* The accumulator rotates touch only H, N, C and the X/Y copies (from the
   result); S, Z and P/V survive. Their CB-prefixed forms set everything. */
static void z80_rotate_accumulator(z80_t *cpu, uint8_t which) {
  const uint8_t a = cpu->a;
  uint8_t carry_out;
  uint8_t result;
  switch (which) {
    case 0: /* RLCA */
      carry_out = a >> 7;
      result = (uint8_t)((a << 1) | carry_out);
      break;
    case 1: /* RRCA */
      carry_out = a & 1;
      result = (uint8_t)((a >> 1) | (carry_out << 7));
      break;
    case 2: /* RLA */
      carry_out = a >> 7;
      result = (uint8_t)((a << 1) | (cpu->f & Z80_FLAG_C));
      break;
    default: /* RRA */
      carry_out = a & 1;
      result = (uint8_t)((a >> 1) | ((cpu->f & Z80_FLAG_C) << 7));
      break;
  }
  cpu->a = result;
  z80_set_flags(cpu, (uint8_t)((cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV)) |
                               (result & (Z80_FLAG_X | Z80_FLAG_Y)) | carry_out));
}

/* DAA per "The Undocumented Z80 Documented" ch. 4: the correction is 0x06,
   0x60 or 0x66 chosen from H, C and the digit values; N decides its sign. */
static void z80_daa(z80_t *cpu) {
  const uint8_t a = cpu->a;
  uint8_t correction = 0;
  uint8_t flags = cpu->f & Z80_FLAG_N;
  if ((cpu->f & Z80_FLAG_H) || (a & 0x0F) > 9) {
    correction = 0x06;
  }
  if ((cpu->f & Z80_FLAG_C) || a > 0x99) {
    correction |= 0x60;
    flags |= Z80_FLAG_C;
  }
  const uint8_t result = (uint8_t)((cpu->f & Z80_FLAG_N) ? a - correction : a + correction);
  flags |= (a ^ result) & Z80_FLAG_H;
  flags |= z80_flags_from_result(result, result) | z80_flag_parity(result);
  cpu->a = result;
  z80_set_flags(cpu, flags);
}

/* SCF and CCF copy X/Y from ((Q ^ F) | A): when the previous instruction
   wrote flags, A alone decides; when it did not, F's old bits leak in too.
   Zilog NMOS behavior, cracked by Patrik Rak (2012), verified against every
   test. Q here is the previous instruction's, captured before decode resets
   it. */
static void z80_carry_flag(z80_t *cpu, bool invert, uint8_t previous_q) {
  uint8_t flags = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
  flags |= ((previous_q ^ cpu->f) | cpu->a) & (Z80_FLAG_X | Z80_FLAG_Y);
  if (invert) {
    flags |= (cpu->f & Z80_FLAG_C) ? Z80_FLAG_H : Z80_FLAG_C;
  } else {
    flags |= Z80_FLAG_C;
  }
  z80_set_flags(cpu, flags);
}

/* The CB rotate/shift table: RLC RRC RL RR SLA SRA SLL SRL, indexed by y.
   Unlike the accumulator forms these set the full flag set. SLL is the
   undocumented shift that fills bit 0 with one. */
static uint8_t z80_rotate_shift(z80_t *cpu, uint8_t which, uint8_t value) {
  const uint8_t carry_in = cpu->f & Z80_FLAG_C;
  uint8_t carry_out;
  uint8_t result;
  switch (which) {
    case 0: /* RLC */
      carry_out = value >> 7;
      result = (uint8_t)((value << 1) | carry_out);
      break;
    case 1: /* RRC */
      carry_out = value & 1;
      result = (uint8_t)((value >> 1) | (carry_out << 7));
      break;
    case 2: /* RL */
      carry_out = value >> 7;
      result = (uint8_t)((value << 1) | carry_in);
      break;
    case 3: /* RR */
      carry_out = value & 1;
      result = (uint8_t)((value >> 1) | (carry_in << 7));
      break;
    case 4: /* SLA */
      carry_out = value >> 7;
      result = (uint8_t)(value << 1);
      break;
    case 5: /* SRA */
      carry_out = value & 1;
      result = (uint8_t)((value >> 1) | (value & 0x80));
      break;
    case 6: /* SLL */
      carry_out = value >> 7;
      result = (uint8_t)((value << 1) | 1);
      break;
    default: /* SRL */
      carry_out = value & 1;
      result = (uint8_t)(value >> 1);
      break;
  }
  z80_set_flags(
      cpu, (uint8_t)(z80_flags_from_result(result, result) | z80_flag_parity(result) | carry_out));
  return result;
}

/* BIT: Z and P/V both report the tested bit clear, S only lights for bit 7,
   H is set, C survives. X/Y leak from xy_source: the tested value for the
   register forms, W for BIT n,(HL) — the leak that revealed WZ's existence. */
static void z80_bit(z80_t *cpu, uint8_t bit, uint8_t value, uint8_t xy_source) {
  const uint8_t masked = (uint8_t)(value & (1 << bit));
  uint8_t flags = (uint8_t)((cpu->f & Z80_FLAG_C) | Z80_FLAG_H);
  flags |= xy_source & (Z80_FLAG_X | Z80_FLAG_Y);
  if (masked == 0) {
    flags |= Z80_FLAG_Z | Z80_FLAG_PV;
  }
  flags |= masked & Z80_FLAG_S;
  z80_set_flags(cpu, flags);
}

/* INC and DEC preserve carry; overflow marks the signed boundary crossings. */
static uint8_t z80_increment(z80_t *cpu, uint8_t value) {
  const uint8_t result = (uint8_t)(value + 1);
  uint8_t flags = (uint8_t)((cpu->f & Z80_FLAG_C) | z80_flags_from_result(result, result));
  if ((result & 0x0F) == 0) {
    flags |= Z80_FLAG_H;
  }
  if (result == 0x80) {
    flags |= Z80_FLAG_PV;
  }
  z80_set_flags(cpu, flags);
  return result;
}

static uint8_t z80_decrement(z80_t *cpu, uint8_t value) {
  const uint8_t result = (uint8_t)(value - 1);
  uint8_t flags =
      (uint8_t)((cpu->f & Z80_FLAG_C) | z80_flags_from_result(result, result) | Z80_FLAG_N);
  if ((result & 0x0F) == 0x0F) {
    flags |= Z80_FLAG_H;
  }
  if (result == 0x7F) {
    flags |= Z80_FLAG_PV;
  }
  z80_set_flags(cpu, flags);
  return result;
}

/* 16-bit register pair table per the decoding doc: BC DE HL SP, indexed by
   p. The HL slot is where the DD/FD prefixes substitute IX/IY. */
static uint16_t z80_register_pair(z80_t *cpu, uint8_t index) {
  switch (index) {
    case 0:
      return (uint16_t)((cpu->b << 8) | cpu->c);
    case 1:
      return (uint16_t)((cpu->d << 8) | cpu->e);
    case 2:
      if (cpu->index_mode == 1) {
        return (uint16_t)((cpu->ixh << 8) | cpu->ixl);
      }
      if (cpu->index_mode == 2) {
        return (uint16_t)((cpu->iyh << 8) | cpu->iyl);
      }
      return (uint16_t)((cpu->h << 8) | cpu->l);
    default:
      return cpu->sp;
  }
}

static void z80_set_register_pair(z80_t *cpu, uint8_t index, uint16_t value) {
  switch (index) {
    case 0:
      cpu->b = (uint8_t)(value >> 8);
      cpu->c = (uint8_t)(value & 0xFF);
      break;
    case 1:
      cpu->d = (uint8_t)(value >> 8);
      cpu->e = (uint8_t)(value & 0xFF);
      break;
    case 2:
      *z80_register8_indexed(cpu, 4) = (uint8_t)(value >> 8);
      *z80_register8_indexed(cpu, 5) = (uint8_t)(value & 0xFF);
      break;
    default:
      cpu->sp = value;
      break;
  }
}

/* ADD HL,ss touches only H, N, C and the X/Y copies — taken from the high
   byte of the result; S, Z and P/V survive. WZ = HL + 1, from the pre-add HL. */
static void z80_add16(z80_t *cpu, uint16_t value) {
  const uint16_t hl = z80_register_pair(cpu, 2); /* IX/IY under a prefix */
  cpu->wz = (uint16_t)(hl + 1);
  const unsigned result = hl + value;
  uint8_t flags = cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV);
  flags |= ((hl ^ value ^ result) >> 8) & Z80_FLAG_H; /* carry out of bit 11 */
  if (result > 0xFFFF) {
    flags |= Z80_FLAG_C;
  }
  flags |= (result >> 8) & (Z80_FLAG_X | Z80_FLAG_Y);
  z80_set_register_pair(cpu, 2, (uint16_t)result);
  z80_set_flags(cpu, flags);
}

/* ADC/SBC HL set the full flag set from the 16-bit result: S from bit 15,
   X/Y from the high byte, H from bit 11, overflow across the word. */
static void z80_adc16(z80_t *cpu, uint16_t value, bool subtract) {
  const uint16_t hl = (uint16_t)((cpu->h << 8) | cpu->l);
  cpu->wz = (uint16_t)(hl + 1);
  const unsigned carry = cpu->f & Z80_FLAG_C;
  unsigned result;
  uint8_t flags;
  if (subtract) {
    result = hl - value - carry;
    flags = Z80_FLAG_N;
    if (((hl ^ value) & (hl ^ result)) & 0x8000) {
      flags |= Z80_FLAG_PV;
    }
  } else {
    result = hl + value + carry;
    flags = 0;
    if ((~(hl ^ value) & (hl ^ result)) & 0x8000) {
      flags |= Z80_FLAG_PV;
    }
  }
  if ((hl ^ value ^ result) & 0x1000) {
    flags |= Z80_FLAG_H;
  }
  if (result & 0x10000) {
    flags |= Z80_FLAG_C;
  }
  const uint16_t result16 = (uint16_t)result;
  flags |= (result16 >> 8) & (Z80_FLAG_S | Z80_FLAG_X | Z80_FLAG_Y);
  if (result16 == 0) {
    flags |= Z80_FLAG_Z;
  }
  cpu->h = (uint8_t)(result16 >> 8);
  cpu->l = (uint8_t)(result16 & 0xFF);
  z80_set_flags(cpu, flags);
}

/* Repeating block instructions rewind PC onto the prefix and leave
   WZ = PC + 1; X/Y are replaced from PC's high byte. Derived from the
   recorded traces — a page-crossing case distinguishes PC from PC + 1. */
static void z80_block_repeat(z80_t *cpu) {
  cpu->pc = (uint16_t)(cpu->pc - 2);
  cpu->wz = (uint16_t)(cpu->pc + 1);
  z80_set_flags(cpu, (uint8_t)((cpu->f & ~(Z80_FLAG_X | Z80_FLAG_Y)) |
                               ((cpu->pc >> 8) & (Z80_FLAG_X | Z80_FLAG_Y))));
}

/* The block I/O flag rules ("The Undocumented Z80 Documented" ch. 4): S, Z,
   X, Y from the decremented B; N from the transferred byte's top bit; H and C
   from the byte plus an adjuster (C±1 for IN, the moved L for OUT)
   overflowing; P/V a parity mix of both. */
static void z80_block_io_flags(z80_t *cpu, uint8_t value, uint8_t adjusted) {
  const unsigned sum = value + adjusted;
  uint8_t flags = z80_flags_from_result(cpu->b, cpu->b);
  if (value & 0x80) {
    flags |= Z80_FLAG_N;
  }
  if (sum > 0xFF) {
    flags |= Z80_FLAG_H | Z80_FLAG_C;
  }
  flags |= z80_flag_parity((uint8_t)((sum & 7) ^ cpu->b));
  z80_set_flags(cpu, flags);
}

/* Repeating I/O forms additionally rewrite H and P/V from B and the carry —
   behavior the community only pinned down in the 2010s; these formulas are
   fitted to and verified against every recorded repeat case. */
static void z80_block_io_repeat_flags(z80_t *cpu) {
  const uint8_t b = cpu->b;
  uint8_t flags = cpu->f;
  unsigned even = (flags & Z80_FLAG_PV) ? 1 : 0;
  flags &= (uint8_t)~(Z80_FLAG_H | Z80_FLAG_PV);
  if (flags & Z80_FLAG_C) {
    if (flags & Z80_FLAG_N) {
      if ((b & 0x0F) == 0x00) {
        flags |= Z80_FLAG_H;
      }
      even ^= (z80_flag_parity((uint8_t)((b - 1) & 7)) ? 1u : 0u) ^ 1u;
    } else {
      if ((b & 0x0F) == 0x0F) {
        flags |= Z80_FLAG_H;
      }
      even ^= (z80_flag_parity((uint8_t)((b + 1) & 7)) ? 1u : 0u) ^ 1u;
    }
  } else {
    even ^= (z80_flag_parity((uint8_t)(b & 7)) ? 1u : 0u) ^ 1u;
  }
  if (even) {
    flags |= Z80_FLAG_PV;
  }
  z80_set_flags(cpu, flags);
}

/* HL, DE and BC move by one during the block instructions; bit 3 of the
   opcode picks the direction. */
static void z80_move_pair(uint8_t *high, uint8_t *low, bool decrement) {
  const uint16_t moved = (uint16_t)(((*high << 8) | *low) + (decrement ? -1 : 1));
  *high = (uint8_t)(moved >> 8);
  *low = (uint8_t)(moved & 0xFF);
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
    case DATA_INDEX_LOW:
      return *z80_register8_indexed(cpu, 5);
    case DATA_INDEX_HIGH:
      return *z80_register8_indexed(cpu, 4);
    case DATA_F:
      return cpu->f;
    case DATA_PC_LOW:
      return (uint8_t)(cpu->pc & 0xFF);
    case DATA_PC_HIGH:
      return (uint8_t)(cpu->pc >> 8);
    case DATA_NONE:
      return 0;
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
    case DATA_INDEX_LOW:
      *z80_register8_indexed(cpu, 5) = value;
      break;
    case DATA_INDEX_HIGH:
      *z80_register8_indexed(cpu, 4) = value;
      break;
    case DATA_DISPLACEMENT:
      cpu->wz = (uint16_t)(z80_register_pair(cpu, 2) + (int8_t)value);
      break;
    case DATA_DDCB:
      cpu->alu_operation = value;
      if (((value >> 6) & 3) == 1) { /* BIT y,(IX+d) */
        z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_WZ, DATA_BIT, 1);
      } else {
        z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_WZ, DATA_CB, 1);
        z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ, DATA_LATCH);
        if ((value & 7) != 6) {
          /* the undocumented copy: the memory result also lands in r[z] */
          cpu->finish = FINISH_DDCB_COPY;
        }
      }
      break;
    case DATA_ALU:
      z80_alu(cpu, cpu->alu_operation, value);
      break;
    case DATA_INC_DEC:
      cpu->data_latch = cpu->alu_operation == OPERATION_INCREMENT ? z80_increment(cpu, value)
                                                                  : z80_decrement(cpu, value);
      break;
    case DATA_F:
      cpu->f = value; /* wholesale load, not a computation: Q stays clear */
      break;
    case DATA_CB: {
      const uint8_t x = cpu->alu_operation >> 6;
      const uint8_t y = (cpu->alu_operation >> 3) & 7;
      if (x == 0) {
        cpu->data_latch = z80_rotate_shift(cpu, y, value);
      } else if (x == 2) { /* RES */
        cpu->data_latch = (uint8_t)(value & ~(1 << y));
      } else { /* SET */
        cpu->data_latch = (uint8_t)(value | (1 << y));
      }
      break;
    }
    case DATA_BIT:
      z80_bit(cpu, (cpu->alu_operation >> 3) & 7, value, (uint8_t)(cpu->wz >> 8));
      break;
    case DATA_IN:
      z80_set_flags(cpu, (uint8_t)((cpu->f & Z80_FLAG_C) | z80_flags_from_result(value, value) |
                                   z80_flag_parity(value)));
      if (cpu->alu_operation != 6) {
        *z80_register8(cpu, cpu->alu_operation) = value;
      }
      break;
    case DATA_ROTATE_DIGIT:
      if (cpu->alu_operation == 4) { /* RRD */
        cpu->data_latch = (uint8_t)((cpu->a << 4) | (value >> 4));
        cpu->a = (uint8_t)((cpu->a & 0xF0) | (value & 0x0F));
      } else { /* RLD */
        cpu->data_latch = (uint8_t)((value << 4) | (cpu->a & 0x0F));
        cpu->a = (uint8_t)((cpu->a & 0xF0) | (value >> 4));
      }
      z80_set_flags(cpu, (uint8_t)((cpu->f & Z80_FLAG_C) | z80_flags_from_result(cpu->a, cpu->a) |
                                   z80_flag_parity(cpu->a)));
      break;
    case DATA_BLOCK_LD: {
      cpu->data_latch = value;
      const bool decrement = (cpu->alu_operation & 0x08) != 0;
      z80_move_pair(&cpu->h, &cpu->l, decrement);
      z80_move_pair(&cpu->b, &cpu->c, true);
      /* X from bit 3 and Y from bit 1 of A plus the moved byte */
      const uint8_t mixed = (uint8_t)(cpu->a + value);
      uint8_t flags = (uint8_t)((cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_C)) |
                                (mixed & Z80_FLAG_X) | ((mixed & 0x02) << 4));
      if (cpu->b | cpu->c) {
        flags |= Z80_FLAG_PV;
      }
      z80_set_flags(cpu, flags);
      break;
    }
    case DATA_BLOCK_CP: {
      const bool decrement = (cpu->alu_operation & 0x08) != 0;
      const uint8_t a = cpu->a;
      const uint8_t result = (uint8_t)(a - value);
      z80_move_pair(&cpu->h, &cpu->l, decrement);
      z80_move_pair(&cpu->b, &cpu->c, true);
      cpu->wz = (uint16_t)(cpu->wz + (decrement ? -1 : 1));
      const uint8_t half = (a ^ value ^ result) & 0x10;
      const uint8_t mixed = (uint8_t)(result - (half ? 1 : 0));
      uint8_t flags = (uint8_t)((cpu->f & Z80_FLAG_C) | Z80_FLAG_N | half | (result & Z80_FLAG_S) |
                                (mixed & Z80_FLAG_X) | ((mixed & 0x02) << 4));
      if (result == 0) {
        flags |= Z80_FLAG_Z;
      }
      if (cpu->b | cpu->c) {
        flags |= Z80_FLAG_PV;
      }
      z80_set_flags(cpu, flags);
      if ((cpu->alu_operation & 0x10) && (cpu->b | cpu->c) && result != 0) {
        z80_append_internal(cpu, 5);
        cpu->finish = FINISH_BLOCK_CP_REPEAT;
      }
      break;
    }
    case DATA_BLOCK_IN:
      cpu->data_latch = value;
      cpu->b--;
      z80_block_io_flags(cpu, value, (uint8_t)(cpu->c + ((cpu->alu_operation & 0x08) ? -1 : 1)));
      break;
    case DATA_BLOCK_OUT:
      cpu->data_latch = value;
      cpu->b--;
      z80_move_pair(&cpu->h, &cpu->l, (cpu->alu_operation & 0x08) != 0);
      cpu->wz = (uint16_t)((cpu->b << 8) | cpu->c); /* the port, with B decremented */
      z80_block_io_flags(cpu, value, cpu->l);
      break;
    default:
      *z80_register8(cpu, code) = value;
      break;
  }
}

static void z80_append_cycle_stretched(z80_t *cpu, uint8_t cycle, uint8_t address, uint8_t data,
                                       uint8_t stretch) {
  cpu->program[cpu->program_length++] = (z80_micro_op){cycle, address, data, stretch};
}

static void z80_append_cycle(z80_t *cpu, uint8_t cycle, uint8_t address, uint8_t data) {
  z80_append_cycle_stretched(cpu, cycle, address, data, 0);
}

static void z80_append_internal(z80_t *cpu, uint8_t ticks) {
  z80_append_cycle_stretched(cpu, CYCLE_INTERNAL, ADDRESS_NONE, DATA_NONE, ticks);
}

/* Ends the instruction, and decides whether an interrupt is taken instead of
   the next one. Interrupts are never accepted at the end of a prefix fetch —
   the prefix and the opcode it modifies are one instruction — so a run of DD
   or FD bytes locks out INT and NMI alike for its whole length.
   Sources: "The Undocumented Z80 Documented" (Sean Young) ch. 5, and the
   netlist traces in https://floooh.github.io/2021/12/06/z80-instruction-timing.html */
static void z80_instruction_done(z80_t *cpu) {
  cpu->program_length = 0;
  cpu->program_index = 0;
  cpu->accepting = ACCEPT_NONE;
  if (cpu->prefix != 0) {
    cpu->step = M1_T1;
    return;
  }
  if (cpu->nmi_pending) {
    cpu->nmi_pending = false;
    cpu->iff1 = false; /* IFF2 keeps the old value; RETN restores from it */
    cpu->halted = false;
    cpu->q = 0;
    cpu->accepting = ACCEPT_NMI;
    cpu->step = M1_T1;
    return;
  }
  if (cpu->int_line && cpu->iff1 && !cpu->ei && !cpu->interrupt_shadow) {
    cpu->iff1 = cpu->iff2 = false;
    cpu->halted = false;
    /* On NMOS parts IFF2 is cleared before LD A,I / LD A,R copy it, so an
       interrupt landing on either instruction leaves P/V reporting interrupts
       disabled when they were enabled. Zilog acknowledged this in the 1989
       Data Book's Q&A and fixed it on CMOS; the CPC's Z80 is NMOS. It cannot
       be done inside the instruction, which is why `p` records that one ran. */
    if (cpu->p) {
      cpu->f &= (uint8_t)~Z80_FLAG_PV;
    }
    cpu->q = 0;
    cpu->accepting = ACCEPT_INT;
    cpu->step = INT_ACK_T1;
    return;
  }
  cpu->step = M1_T1;
}

/* Starts the micro-program's next machine cycle, or the next instruction when
   the program is exhausted. Address side effects (PC and WZ movement) happen
   here, at the cycle boundary. */
static void z80_start_next_cycle(z80_t *cpu) {
  if (cpu->program_index == cpu->program_length) {
    switch (cpu->finish) {
      case FINISH_HL_FROM_WZ:
        z80_set_register_pair(cpu, 2, cpu->wz); /* IX/IY under a prefix */
        break;
      case FINISH_PC_FROM_WZ:
        cpu->pc = cpu->wz;
        break;
      case FINISH_JUMP_RELATIVE:
        cpu->pc = (uint16_t)(cpu->pc + (int8_t)cpu->data_latch);
        cpu->wz = cpu->pc;
        break;
      case FINISH_BLOCK_LD:
        z80_move_pair(&cpu->d, &cpu->e, (cpu->alu_operation & 0x08) != 0);
        if ((cpu->alu_operation & 0x10) && (cpu->b | cpu->c)) {
          z80_block_repeat(cpu);
        }
        break;
      case FINISH_BLOCK_IN:
        z80_move_pair(&cpu->h, &cpu->l, (cpu->alu_operation & 0x08) != 0);
        if ((cpu->alu_operation & 0x10) && cpu->b) {
          z80_block_repeat(cpu);
          z80_block_io_repeat_flags(cpu);
        }
        break;
      case FINISH_BLOCK_CP_REPEAT:
        z80_block_repeat(cpu);
        break;
      case FINISH_BLOCK_OUT_REPEAT:
        z80_block_repeat(cpu);
        z80_block_io_repeat_flags(cpu);
        break;
      case FINISH_DDCB_COPY:
        *z80_register8(cpu, cpu->alu_operation & 7) = cpu->data_latch;
        break;
      case FINISH_PC_FROM_VECTOR:
        cpu->pc = (uint16_t)((cpu->wz & 0xFF00) | cpu->data_latch);
        cpu->wz = cpu->pc;
        break;
      default:
        break;
    }
    z80_instruction_done(cpu);
    return;
  }
  const z80_micro_op operation = cpu->program[cpu->program_index++];
  cpu->operand_data = operation.data;
  cpu->stretch_remaining = operation.stretch;
  switch (operation.address) {
    case ADDRESS_NONE:
      break;
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
    case ADDRESS_WZ_THEN_A_HIGH:
      cpu->operand_address = cpu->wz;
      cpu->wz = (uint16_t)((cpu->a << 8) | ((cpu->wz + 1) & 0xFF));
      break;
    case ADDRESS_WZ_DECREMENT:
      cpu->operand_address = cpu->wz--;
      break;
    case ADDRESS_DE:
      cpu->operand_address = (uint16_t)((cpu->d << 8) | cpu->e);
      break;
    case ADDRESS_SP_INCREMENT:
      cpu->operand_address = cpu->sp++;
      break;
    case ADDRESS_SP_DECREMENT:
      cpu->operand_address = --cpu->sp;
      break;
    case ADDRESS_SP:
      cpu->operand_address = cpu->sp;
      break;
    default: /* ADDRESS_SP_PLUS_1 */
      cpu->operand_address = (uint16_t)(cpu->sp + 1);
      break;
  }
  switch (operation.cycle) {
    case CYCLE_MEM_READ:
      cpu->step = MEM_READ_T1;
      break;
    case CYCLE_MEM_WRITE:
      cpu->step = MEM_WRITE_T1;
      break;
    case CYCLE_IO_READ:
      cpu->step = IO_READ_T1;
      break;
    case CYCLE_IO_WRITE:
      cpu->step = IO_WRITE_T1;
      break;
    default: /* CYCLE_INTERNAL: stretch only, bus untouched */
      cpu->step = STRETCH_T;
      break;
  }
}

/* Opcode fields x/y/z/p/q per "Decoding Z80 Opcodes" (Cristian Dinu),
   http://www.z80.info/decoding.htm — the octal structure the silicon decodes:
   x = bits 7..6, y = bits 5..3, z = bits 2..0, y = 2p + q. The rp table (BC
   DE HL SP) is indexed by p. */
static const uint8_t register_pair_low[4] = {REGISTER_C, REGISTER_E, DATA_INDEX_LOW, DATA_SP_LOW};
static const uint8_t register_pair_high[4] = {REGISTER_B, REGISTER_D, DATA_INDEX_HIGH,
                                              DATA_SP_HIGH};

/* the rp2 table (PUSH/POP): BC DE HL AF, indexed by p; the HL slot follows
   the active index prefix */
static const uint8_t register_pair2_low[4] = {REGISTER_C, REGISTER_E, DATA_INDEX_LOW, DATA_F};
static const uint8_t register_pair2_high[4] = {REGISTER_B, REGISTER_D, DATA_INDEX_HIGH, REGISTER_A};

/* the cc table per the decoding doc: NZ Z NC C PO PE P M — the flag chosen
   by the index's high bits, the sense by its low bit */
static bool z80_condition(const z80_t *cpu, uint8_t index) {
  uint8_t flag;
  switch (index >> 1) {
    case 0:
      flag = Z80_FLAG_Z;
      break;
    case 1:
      flag = Z80_FLAG_C;
      break;
    case 2:
      flag = Z80_FLAG_PV;
      break;
    default:
      flag = Z80_FLAG_S;
      break;
  }
  const bool set = (cpu->f & flag) != 0;
  return (index & 1) ? set : !set;
}

/* (HL) becomes (IX+d)/(IY+d) under a prefix: the displacement is read and
   folded into WZ, five T-states of address arithmetic follow, and the memory
   cycles then go through WZ — which is why WZ ends as the effective address. */
static uint8_t z80_indexed_address(z80_t *cpu) {
  if (cpu->index_mode == 0) {
    return ADDRESS_HL;
  }
  z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_DISPLACEMENT);
  z80_append_internal(cpu, 5);
  return ADDRESS_WZ;
}

/* The CB page: rotates/shifts (x=0), BIT (x=1), RES (x=2), SET (x=3), all on
   r[z]. The (HL) forms read with one stretch T-state and, except BIT, write
   back; BIT only looks. */
static void z80_decode_cb(z80_t *cpu) {
  const uint8_t x = cpu->opcode >> 6;
  const uint8_t y = (cpu->opcode >> 3) & 7;
  const uint8_t z = cpu->opcode & 7;
  if (z == 6) {
    cpu->alu_operation = cpu->opcode;
    if (x == 1) { /* BIT y,(HL) */
      z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_HL, DATA_BIT, 1);
    } else {
      z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_HL, DATA_CB, 1);
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_HL, DATA_LATCH);
    }
    return;
  }
  uint8_t *reg = z80_register8(cpu, z);
  switch (x) {
    case 0:
      *reg = z80_rotate_shift(cpu, y, *reg);
      break;
    case 1:
      z80_bit(cpu, y, *reg, *reg);
      break;
    case 2:
      *reg = (uint8_t)(*reg & ~(1 << y));
      break;
    default:
      *reg = (uint8_t)(*reg | (1 << y));
      break;
  }
}

/* The ED page. x=1 holds the documented instructions; x=2, z<=3, y>=4 the
   block instructions; everything else is a NONI that runs as NOP. */
static void z80_decode_ed(z80_t *cpu) {
  const uint8_t x = cpu->opcode >> 6;
  const uint8_t y = (cpu->opcode >> 3) & 7;
  const uint8_t z = cpu->opcode & 7;
  const uint8_t p = y >> 1;
  const uint8_t q = y & 1;
  static const uint8_t interrupt_mode_table[4] = {0, 0, 1, 2};

  if (x == 1) {
    switch (z) {
      case 0: /* IN r[y],(C); y=6 sets flags only */
        cpu->wz = (uint16_t)((cpu->b << 8) | cpu->c);
        cpu->alu_operation = y;
        z80_append_cycle(cpu, CYCLE_IO_READ, ADDRESS_WZ_INCREMENT, DATA_IN);
        break;
      case 1: /* OUT (C),r[y]; y=6 outputs zero on NMOS parts */
        cpu->wz = (uint16_t)((cpu->b << 8) | cpu->c);
        z80_append_cycle(cpu, CYCLE_IO_WRITE, ADDRESS_WZ_INCREMENT, y == 6 ? DATA_NONE : y);
        break;
      case 2: /* SBC HL,rp[p] (q=0) / ADC HL,rp[p] (q=1) */
        z80_adc16(cpu, z80_register_pair(cpu, p), q == 0);
        z80_append_internal(cpu, 7);
        break;
      case 3: /* LD (nn),rp[p] / LD rp[p],(nn) */
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W);
        if (q == 0) {
          z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ_INCREMENT, register_pair_low[p]);
          z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ, register_pair_high[p]);
        } else {
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ_INCREMENT, register_pair_low[p]);
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ, register_pair_high[p]);
        }
        break;
      case 4: { /* NEG, at every y */
        const uint8_t value = cpu->a;
        cpu->a = 0;
        z80_alu(cpu, ALU_SUB, value);
        break;
      }
      case 5: /* RETN, RETI at y=1: both copy IFF2 into IFF1 and return */
        /* IFF1 is restored during the next opcode fetch, too late for an
           interrupt at the end of this one — but only an earlier NMI can have
           left the two disagreeing. */
        cpu->interrupt_shadow = cpu->iff1 != cpu->iff2;
        cpu->iff1 = cpu->iff2;
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, DATA_Z);
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, DATA_W);
        cpu->finish = FINISH_PC_FROM_WZ;
        break;
      case 6: /* IM im[y] */
        cpu->im = interrupt_mode_table[y & 3];
        break;
      default: /* z == 7 */
        switch (y) {
          case 0: /* LD I,A */
            cpu->i = cpu->a;
            z80_append_internal(cpu, 1);
            break;
          case 1: /* LD R,A: the one way bit 7 of R changes */
            cpu->r = cpu->a;
            z80_append_internal(cpu, 1);
            break;
          case 2: /* LD A,I */
          case 3: /* LD A,R: both read IFF2 into P/V — the interrupt-race bug
                     spot — and mark the P tracker */
            cpu->a = y == 2 ? cpu->i : cpu->r;
            z80_set_flags(cpu,
                          (uint8_t)((cpu->f & Z80_FLAG_C) | z80_flags_from_result(cpu->a, cpu->a) |
                                    (cpu->iff2 ? Z80_FLAG_PV : 0)));
            cpu->p = 1;
            z80_append_internal(cpu, 1);
            break;
          case 4: /* RRD */
          case 5: /* RLD */
            cpu->alu_operation = y;
            cpu->wz = (uint16_t)(((cpu->h << 8) | cpu->l) + 1);
            z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_HL, DATA_ROTATE_DIGIT, 4);
            z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_HL, DATA_LATCH);
            break;
          default: /* y=6,7: NONI */
            break;
        }
        break;
    }
  } else if (x == 2 && z <= 3 && y >= 4) { /* the block instructions */
    const bool decrement = (y & 1) != 0;
    const bool repeat = y >= 6;
    cpu->alu_operation = cpu->opcode;
    switch (z) {
      case 0: /* LDI/LDD/LDIR/LDDR */
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_HL, DATA_BLOCK_LD);
        z80_append_cycle_stretched(cpu, CYCLE_MEM_WRITE, ADDRESS_DE, DATA_LATCH, 2);
        if (repeat && (uint16_t)(((cpu->b << 8) | cpu->c) - 1) != 0) {
          z80_append_internal(cpu, 5);
        }
        cpu->finish = FINISH_BLOCK_LD;
        break;
      case 1: /* CPI/CPD/CPIR/CPDR: the repeat decision needs the byte, so the
                 operand handler appends the extra cycle itself */
        z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_HL, DATA_BLOCK_CP, 5);
        break;
      case 2: /* INI/IND/INIR/INDR: port BC with the initial B */
        cpu->wz = (uint16_t)((cpu->b << 8) | cpu->c);
        z80_append_internal(cpu, 1);
        z80_append_cycle(cpu, CYCLE_IO_READ,
                         decrement ? ADDRESS_WZ_DECREMENT : ADDRESS_WZ_INCREMENT, DATA_BLOCK_IN);
        z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_HL, DATA_LATCH);
        if (repeat && (uint8_t)(cpu->b - 1) != 0) {
          z80_append_internal(cpu, 5);
        }
        cpu->finish = FINISH_BLOCK_IN;
        break;
      default: /* OUTI/OUTD/OTIR/OTDR: port BC with B already decremented */
        z80_append_internal(cpu, 1);
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_HL, DATA_BLOCK_OUT);
        z80_append_cycle(cpu, CYCLE_IO_WRITE,
                         decrement ? ADDRESS_WZ_DECREMENT : ADDRESS_WZ_INCREMENT, DATA_LATCH);
        if (repeat && (uint8_t)(cpu->b - 1) != 0) {
          z80_append_internal(cpu, 5);
          cpu->finish = FINISH_BLOCK_OUT_REPEAT;
        }
        break;
    }
  }
  /* every other ED opcode is a NONI: it decodes to an empty program */
}

static void z80_decode(z80_t *cpu) {
  const uint8_t x = cpu->opcode >> 6;
  const uint8_t y = (cpu->opcode >> 3) & 7;
  const uint8_t z = cpu->opcode & 7;
  const uint8_t p = y >> 1;
  const uint8_t q = y & 1;

  /* per-instruction trackers reset at the start so the instruction's own
     work can set them: Q via z80_set_flags, EI and P by their opcodes.
     SCF/CCF read the previous instruction's Q, so it survives in a local. */
  const uint8_t previous_q = cpu->q;
  cpu->q = 0;
  cpu->ei = false;
  cpu->interrupt_shadow = false;
  cpu->p = 0;
  cpu->finish = FINISH_NONE;

  if (cpu->prefix == 0xCB) {
    cpu->prefix = 0;
    z80_decode_cb(cpu);
    return;
  }
  if (cpu->prefix == 0xED) {
    cpu->prefix = 0;
    cpu->index_mode = 0; /* DD ED discards the index prefix */
    z80_decode_ed(cpu);
    return;
  }
  if (cpu->prefix == 0xDD) {
    cpu->index_mode = 1;
  } else if (cpu->prefix == 0xFD) {
    cpu->index_mode = 2;
  } else {
    cpu->index_mode = 0;
  }
  cpu->prefix = 0;

  if (x == 0) {
    switch (z) {
      case 0:
        if (y == 1) { /* EX AF,AF' */
          const uint16_t exchanged = (uint16_t)((cpu->a << 8) | cpu->f);
          cpu->a = (uint8_t)(cpu->af_ >> 8);
          cpu->f = (uint8_t)(cpu->af_ & 0xFF);
          cpu->af_ = exchanged;
        } else if (y == 2) { /* DJNZ d: the B decrement extends M1 one T-state */
          cpu->b--;
          z80_append_internal(cpu, 1);
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_LATCH);
          if (cpu->b != 0) {
            z80_append_internal(cpu, 5);
            cpu->finish = FINISH_JUMP_RELATIVE;
          }
        } else if (y >= 3) { /* JR d and JR cc[y-4],d */
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_LATCH);
          if (y == 3 || z80_condition(cpu, y - 4)) {
            z80_append_internal(cpu, 5);
            cpu->finish = FINISH_JUMP_RELATIVE;
          }
        }
        break;
      case 1:
        if (q == 0) { /* LD rp[p],nn */
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, register_pair_low[p]);
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, register_pair_high[p]);
        } else { /* ADD HL,rp[p] */
          z80_add16(cpu, z80_register_pair(cpu, p));
          z80_append_internal(cpu, 7);
        }
        break;
      case 3: /* INC/DEC rp[p]: computed while the bus idles two T-states */
        z80_set_register_pair(cpu, p, (uint16_t)(z80_register_pair(cpu, p) + (q == 0 ? 1 : -1)));
        z80_append_internal(cpu, 2);
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
              z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ_INCREMENT, DATA_INDEX_LOW);
              z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ, DATA_INDEX_HIGH);
            } else {
              z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ_INCREMENT, DATA_INDEX_LOW);
              z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ, DATA_INDEX_HIGH);
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
      case 4:         /* INC r[y] */
      case 5:         /* DEC r[y] */
        if (y == 6) { /* INC/DEC (HL): read, modify into the latch, write back */
          cpu->alu_operation = z == 4 ? OPERATION_INCREMENT : OPERATION_DECREMENT;
          const uint8_t address = z80_indexed_address(cpu);
          z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, address, DATA_INC_DEC, 1);
          z80_append_cycle(cpu, CYCLE_MEM_WRITE, address, DATA_LATCH);
        } else {
          uint8_t *reg = z80_register8_indexed(cpu, y);
          *reg = z == 4 ? z80_increment(cpu, *reg) : z80_decrement(cpu, *reg);
        }
        break;
      case 6:
        if (y == 6) { /* LD (HL),n; the indexed form reads d first, n stretched */
          if (cpu->index_mode) {
            z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_DISPLACEMENT);
            z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_LATCH, 2);
            z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_WZ, DATA_LATCH);
          } else {
            z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_LATCH);
            z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_HL, DATA_LATCH);
          }
        } else { /* LD r,n */
          const uint8_t target = y == 4 ? DATA_INDEX_HIGH : y == 5 ? DATA_INDEX_LOW : y;
          z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, target);
        }
        break;
      case 7:
        switch (y) {
          case 0:
          case 1:
          case 2:
          case 3:
            z80_rotate_accumulator(cpu, y);
            break;
          case 4:
            z80_daa(cpu);
            break;
          case 5: /* CPL */
            cpu->a = (uint8_t)~cpu->a;
            z80_set_flags(
                cpu, (uint8_t)((cpu->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_PV | Z80_FLAG_C)) |
                               Z80_FLAG_H | Z80_FLAG_N | (cpu->a & (Z80_FLAG_X | Z80_FLAG_Y))));
            break;
          case 6: /* SCF */
            z80_carry_flag(cpu, false, previous_q);
            break;
          default: /* CCF */
            z80_carry_flag(cpu, true, previous_q);
            break;
        }
        break;
      default: /* 0x00 NOP */
        break;
    }
  } else if (x == 1) {         /* the LD r,r' page */
    if (cpu->opcode == 0x76) { /* HALT */
      cpu->halted = true;
    } else if (z == 6) { /* LD r,(HL): the register side stays unmapped */
      z80_append_cycle(cpu, CYCLE_MEM_READ, z80_indexed_address(cpu), y);
    } else if (y == 6) { /* LD (HL),r */
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, z80_indexed_address(cpu), z);
    } else {
      *z80_register8_indexed(cpu, y) = *z80_register8_indexed(cpu, z);
    }
  } else if (x == 2) { /* alu[y] with operand r[z] */
    if (z == 6) {
      cpu->alu_operation = y;
      z80_append_cycle(cpu, CYCLE_MEM_READ, z80_indexed_address(cpu), DATA_ALU);
    } else {
      z80_alu(cpu, y, *z80_register8_indexed(cpu, z));
    }
  } else if (x == 3 && z == 6) { /* alu[y] with immediate operand */
    cpu->alu_operation = y;
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_ALU);
  } else if (x == 3 && z == 0) { /* RET cc[y]: the test extends M1 one T-state */
    z80_append_internal(cpu, 1);
    if (z80_condition(cpu, y)) {
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, DATA_Z);
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, DATA_W);
      cpu->finish = FINISH_PC_FROM_WZ;
    }
  } else if (x == 3 && z == 2) { /* JP cc[y],nn: the operand is read either way */
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W);
    if (z80_condition(cpu, y)) {
      cpu->finish = FINISH_PC_FROM_WZ;
    }
  } else if (x == 3 && z == 4) { /* CALL cc[y],nn: the operand is read either way */
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
    if (z80_condition(cpu, y)) {
      z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W, 1);
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_HIGH);
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_LOW);
      cpu->finish = FINISH_PC_FROM_WZ;
    } else {
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W);
    }
  } else if (x == 3 && z == 7) { /* RST y*8 */
    cpu->wz = (uint16_t)(y * 8);
    z80_append_internal(cpu, 1);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_HIGH);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_LOW);
    cpu->finish = FINISH_PC_FROM_WZ;
  } else if (x == 3 && z == 1) {
    if (q == 0) { /* POP rp2[p]: low byte first, post-increment */
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, register_pair2_low[p]);
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, register_pair2_high[p]);
    } else if (p == 0) { /* RET */
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, DATA_Z);
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP_INCREMENT, DATA_W);
      cpu->finish = FINISH_PC_FROM_WZ;
    } else if (p == 2) { /* JP (HL): no memory access despite the name; WZ untouched */
      cpu->pc = z80_register_pair(cpu, 2);
    } else if (p == 1) { /* EXX */
      uint16_t exchanged = (uint16_t)((cpu->b << 8) | cpu->c);
      cpu->b = (uint8_t)(cpu->bc_ >> 8);
      cpu->c = (uint8_t)(cpu->bc_ & 0xFF);
      cpu->bc_ = exchanged;
      exchanged = (uint16_t)((cpu->d << 8) | cpu->e);
      cpu->d = (uint8_t)(cpu->de_ >> 8);
      cpu->e = (uint8_t)(cpu->de_ & 0xFF);
      cpu->de_ = exchanged;
      exchanged = (uint16_t)((cpu->h << 8) | cpu->l);
      cpu->h = (uint8_t)(cpu->hl_ >> 8);
      cpu->l = (uint8_t)(cpu->hl_ & 0xFF);
      cpu->hl_ = exchanged;
    } else if (p == 3) { /* LD SP,HL */
      cpu->sp = z80_register_pair(cpu, 2);
      z80_append_internal(cpu, 2);
    }
  } else if (x == 3 && z == 3 && y == 0) { /* JP nn */
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W);
    cpu->finish = FINISH_PC_FROM_WZ;
  } else if (x == 3 && z == 3 && y == 4) { /* EX (SP),HL */
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_SP, DATA_Z);
    z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_SP_PLUS_1, DATA_W, 1);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_PLUS_1, DATA_INDEX_HIGH);
    z80_append_cycle_stretched(cpu, CYCLE_MEM_WRITE, ADDRESS_SP, DATA_INDEX_LOW, 2);
    cpu->finish = FINISH_HL_FROM_WZ;
  } else if (x == 3 && z == 3 && y == 5) { /* EX DE,HL */
    uint8_t exchanged = cpu->d;
    cpu->d = cpu->h;
    cpu->h = exchanged;
    exchanged = cpu->e;
    cpu->e = cpu->l;
    cpu->l = exchanged;
  } else if (x == 3 && z == 3 && y == 1) { /* the CB prefix */
    if (cpu->index_mode) {
      /* DD CB d xx: the displacement comes first and the sub-opcode arrives
         as plain data — no M1, no refresh, R only counts the two prefixes */
      z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_DISPLACEMENT);
      z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_DDCB, 2);
    } else { /* one more M1 fetches the opcode */
      cpu->prefix = 0xCB;
    }
  } else if (x == 3 && z == 3 && y == 2) { /* OUT (n),A: port A:n, W preloaded with A */
    cpu->wz = (uint16_t)((cpu->a << 8) | (cpu->wz & 0xFF));
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
    z80_append_cycle(cpu, CYCLE_IO_WRITE, ADDRESS_WZ_THEN_A_HIGH, REGISTER_A);
  } else if (x == 3 && z == 3 && y == 3) { /* IN A,(n): port A:n, no flags */
    cpu->wz = (uint16_t)((cpu->a << 8) | (cpu->wz & 0xFF));
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
    z80_append_cycle(cpu, CYCLE_IO_READ, ADDRESS_WZ_INCREMENT, REGISTER_A);
  } else if (x == 3 && z == 3 && y == 6) { /* DI */
    cpu->iff1 = cpu->iff2 = false;
  } else if (x == 3 && z == 3 && y == 7) { /* EI: acceptance stays blocked one instruction */
    cpu->iff1 = cpu->iff2 = true;
    cpu->ei = true;
  } else if (x == 3 && z == 5 && q == 0) { /* PUSH rp2[p]: high byte first, pre-decrement */
    z80_append_internal(cpu, 1);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, register_pair2_high[p]);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, register_pair2_low[p]);
  } else if (x == 3 && z == 5 && p == 2) { /* the ED prefix */
    cpu->prefix = 0xED;
  } else if (x == 3 && z == 5 && q == 1 && p == 1) { /* the DD prefix */
    cpu->prefix = 0xDD;
  } else if (x == 3 && z == 5 && q == 1 && p == 3) { /* the FD prefix */
    cpu->prefix = 0xFD;
  } else if (x == 3 && z == 5 && p == 0) { /* CALL nn */
    z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_Z);
    z80_append_cycle_stretched(cpu, CYCLE_MEM_READ, ADDRESS_PC_INCREMENT, DATA_W, 1);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_HIGH);
    z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_LOW);
    cpu->finish = FINISH_PC_FROM_WZ;
  }
  /* everything unimplemented decodes to an empty program and runs as NOP, so
     a machine can keep ticking */
}

bool z80_instruction_complete(const z80_t *cpu) {
  return cpu->step == M1_T1 && cpu->prefix == 0 && cpu->accepting == ACCEPT_NONE;
}

uint64_t z80_tick(z80_t *cpu, uint64_t pins) {
  /* NMI is edge-triggered and latched: a pulse anywhere within an instruction
     is remembered until it can be taken. INT is a level, and only its state
     at the end of an instruction counts — release it early and it is missed. */
  const bool nmi_now = (pins & Z80_NMI) != 0;
  if (nmi_now && !cpu->nmi_previous) {
    cpu->nmi_pending = true;
  }
  cpu->nmi_previous = nmi_now;
  cpu->int_line = (pins & Z80_INT) != 0;

  switch (cpu->step) {
    case M1_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->pc) | Z80_M1;
      if (cpu->halted) {
        pins |= Z80_HALT;
      }
      cpu->step = M1_T2;
      break;

    case M1_T2:
      pins |= Z80_MREQ | Z80_RD;
      /* a halted CPU keeps fetching but stands still, and so does the NMI
         acknowledge, which reads a byte only to throw it away */
      if (!cpu->halted && cpu->accepting != ACCEPT_NMI) {
        cpu->pc++;
      }
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
      cpu->opcode = (cpu->halted || cpu->accepting == ACCEPT_NMI) ? 0x00 : z80_data(pins);
      pins = z80_set_address(pins & ~Z80_OUT_PINS, (uint16_t)((cpu->i << 8) | cpu->r)) | Z80_RFSH;
      /* R is a 7-bit counter; bit 7 changes only via LD R,A. */
      cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F);
      cpu->step = M1_T4;
      break;

    case M1_T4:
      pins = (pins & ~Z80_OUT_PINS) | Z80_RFSH;
      if (cpu->accepting == ACCEPT_NMI) {
        cpu->wz = 0x0066;
        cpu->finish = FINISH_PC_FROM_WZ;
        z80_append_internal(cpu, 1);
        z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_HIGH);
        z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_LOW);
      } else {
        z80_decode(cpu);
      }
      z80_start_next_cycle(cpu);
      break;

    case INT_ACK_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->pc) | Z80_M1;
      cpu->step = INT_ACK_T2;
      break;

    case INT_ACK_T2:
      pins |= Z80_IORQ;
      cpu->stretch_remaining = 2;
      cpu->step = INT_ACK_WAIT;
      break;

    case INT_ACK_WAIT:
      if (pins & Z80_WAIT) {
        return pins;
      }
      if (--cpu->stretch_remaining == 0) {
        cpu->step = INT_ACK_T3;
      }
      break;

    case INT_ACK_T3:
      cpu->data_latch = z80_data(pins); /* the vector, if a device supplied one */
      pins = z80_set_address(pins & ~Z80_OUT_PINS, (uint16_t)((cpu->i << 8) | cpu->r)) | Z80_RFSH;
      cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F);
      cpu->step = INT_ACK_T4;
      break;

    case INT_ACK_T4:
      pins = (pins & ~Z80_OUT_PINS) | Z80_RFSH;
      z80_append_internal(cpu, 1);
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_HIGH);
      z80_append_cycle(cpu, CYCLE_MEM_WRITE, ADDRESS_SP_DECREMENT, DATA_PC_LOW);
      if (cpu->im == 2) {
        /* The vector indexes a table at I:vector. Zilog's manual insists the
           low bit is forced even; Sean Young and J.G. Harston each showed on
           real hardware that it is not, so the byte is used whole and the
           second read may cross into the next page. */
        cpu->wz = (uint16_t)((cpu->i << 8) | cpu->data_latch);
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ_INCREMENT, DATA_LATCH);
        z80_append_cycle(cpu, CYCLE_MEM_READ, ADDRESS_WZ, DATA_W);
        cpu->finish = FINISH_PC_FROM_VECTOR;
      } else {
        /* Mode 0 executes whatever the device puts on the bus; with no device
           driving it that is 0xFF, an RST 38h, which is mode 1's behaviour.
           Only that case is implemented — the CPC never uses mode 0. */
        cpu->wz = 0x0038;
        cpu->finish = FINISH_PC_FROM_WZ;
      }
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
      if (cpu->stretch_remaining) {
        cpu->step = STRETCH_T;
      } else {
        z80_start_next_cycle(cpu);
      }
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
      if (cpu->stretch_remaining) {
        cpu->step = STRETCH_T;
      } else {
        z80_start_next_cycle(cpu);
      }
      break;

    case IO_READ_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->operand_address);
      cpu->step = IO_READ_T2;
      break;

    case IO_READ_T2:
      cpu->step = IO_READ_T3;
      break;

    case IO_READ_T3:
      pins |= Z80_IORQ | Z80_RD;
      cpu->step = IO_READ_T4;
      break;

    case IO_READ_T4:
      if (pins & Z80_WAIT) {
        return pins;
      }
      z80_set_operand(cpu, cpu->operand_data, z80_data(pins));
      pins &= ~Z80_OUT_PINS;
      z80_start_next_cycle(cpu);
      break;

    case IO_WRITE_T1:
      pins = z80_set_address(pins & ~Z80_OUT_PINS, cpu->operand_address);
      cpu->step = IO_WRITE_T2;
      break;

    case IO_WRITE_T2:
      cpu->step = IO_WRITE_T3;
      break;

    case IO_WRITE_T3:
      pins = z80_set_data(pins, z80_get_operand(cpu, cpu->operand_data)) | Z80_IORQ | Z80_WR;
      cpu->step = IO_WRITE_T4;
      break;

    case IO_WRITE_T4:
      if (pins & Z80_WAIT) {
        return pins;
      }
      pins &= ~Z80_OUT_PINS;
      z80_start_next_cycle(cpu);
      break;

    case STRETCH_T:
      if (--cpu->stretch_remaining == 0) {
        z80_start_next_cycle(cpu);
      }
      break;

    default: /* unreachable; recover to an instruction boundary */
      cpu->step = M1_T1;
      break;
  }
  return pins;
}
