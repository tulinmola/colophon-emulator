/*
 * spectrum_snapshot.c — the header, field by field.
 */
#include "spectrum_snapshot.h"

#include <string.h>

/* Where each field sits, in the order the format lists them. Named rather
   than counted, because an off-by-one here is a machine that resumes with
   the wrong accumulator and no obvious reason why. */
#define AT_I 0x00
#define AT_SHADOW_HL 0x01
#define AT_SHADOW_DE 0x03
#define AT_SHADOW_BC 0x05
#define AT_SHADOW_AF 0x07
#define AT_HL 0x09
#define AT_DE 0x0B
#define AT_BC 0x0D
#define AT_IY 0x0F
#define AT_IX 0x11
#define AT_INTERRUPT 0x13 /* bit 2 holds IFF2, and nothing holds IFF1 */
#define AT_R 0x14
#define AT_AF 0x15
#define AT_SP 0x17
#define AT_INTERRUPT_MODE 0x19
#define AT_BORDER 0x1A
#define AT_RAM 0x1B

#define IFF2_BIT 0x04

static uint16_t read16(const uint8_t *bytes, size_t at) {
  return (uint16_t)(bytes[at] | (bytes[at + 1] << 8));
}

static void write16(uint8_t *bytes, size_t at, uint16_t value) {
  bytes[at] = (uint8_t)value;
  bytes[at + 1] = (uint8_t)(value >> 8);
}

size_t spectrum_snapshot_size(const spectrum_t *spectrum) {
  return spectrum->ram_size == SPECTRUM_RAM_48K ? SPECTRUM_SNAPSHOT_SIZE : 0;
}

bool spectrum_snapshot_load(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                            const char **problem) {
  if (length != SPECTRUM_SNAPSHOT_SIZE) {
    *problem = "is not 49179 bytes, which is the only length a 48K snapshot has";
    return false;
  }
  if (spectrum->ram_size != SPECTRUM_RAM_48K) {
    *problem = "describes a 48K machine, and this one is not";
    return false;
  }

  uint16_t stack = read16(bytes, AT_SP);
  if (stack < SPECTRUM_RAM_BASE || stack == 0xFFFF) {
    *problem = "puts its stack where the program counter cannot have been pushed";
    return false;
  }

  z80_t *cpu = &spectrum->cpu;
  z80_init(cpu);
  cpu->i = bytes[AT_I];
  cpu->hl_ = read16(bytes, AT_SHADOW_HL);
  cpu->de_ = read16(bytes, AT_SHADOW_DE);
  cpu->bc_ = read16(bytes, AT_SHADOW_BC);
  cpu->af_ = read16(bytes, AT_SHADOW_AF);
  uint16_t hl = read16(bytes, AT_HL);
  uint16_t de = read16(bytes, AT_DE);
  uint16_t bc = read16(bytes, AT_BC);
  uint16_t iy = read16(bytes, AT_IY);
  uint16_t ix = read16(bytes, AT_IX);
  cpu->h = (uint8_t)(hl >> 8);
  cpu->l = (uint8_t)hl;
  cpu->d = (uint8_t)(de >> 8);
  cpu->e = (uint8_t)de;
  cpu->b = (uint8_t)(bc >> 8);
  cpu->c = (uint8_t)bc;
  cpu->iyh = (uint8_t)(iy >> 8);
  cpu->iyl = (uint8_t)iy;
  cpu->ixh = (uint8_t)(ix >> 8);
  cpu->ixl = (uint8_t)ix;
  cpu->r = bytes[AT_R];
  uint16_t af = read16(bytes, AT_AF);
  cpu->a = (uint8_t)(af >> 8);
  cpu->f = (uint8_t)af;
  cpu->sp = stack;
  cpu->im = bytes[AT_INTERRUPT_MODE] & 0x03;

  /* The header carries IFF2 alone. A real restart is a RETN, which copies
     IFF2 into IFF1; doing that here rather than executing one leaves the
     machine where a RETN would have left it, without the two T-states and
     without needing the ROM to hold one. */
  cpu->iff2 = (bytes[AT_INTERRUPT] & IFF2_BIT) != 0;
  cpu->iff1 = cpu->iff2;

  memcpy(spectrum->ram, bytes + AT_RAM, SPECTRUM_RAM_48K);

  /* And the program counter off the machine's own stack, where writing it
     put it. */
  uint16_t stacked = (uint16_t)(spectrum_peek(spectrum, cpu->sp) |
                                (spectrum_peek(spectrum, (uint16_t)(cpu->sp + 1)) << 8));
  cpu->pc = stacked;
  cpu->sp = (uint16_t)(cpu->sp + 2);

  ula_write(&spectrum->ula, bytes[AT_BORDER] & 0x07);
  return true;
}

bool spectrum_snapshot_save(const spectrum_t *spectrum, uint8_t *bytes, size_t capacity,
                            const char **problem) {
  if (spectrum->ram_size != SPECTRUM_RAM_48K) {
    *problem = "only a 48K machine fits this format";
    return false;
  }
  if (capacity < SPECTRUM_SNAPSHOT_SIZE) {
    *problem = "needs 49179 bytes to write into";
    return false;
  }
  if (!z80_instruction_complete(&spectrum->cpu)) {
    *problem = "cannot be taken mid-instruction";
    return false;
  }

  const z80_t *cpu = &spectrum->cpu;
  /* The format has nowhere for the program counter, so it is written where a
     PUSH would have put it and the two bytes that were there are lost. They
     are lost in the snapshot and not in the machine: a running machine is
     not damaged by being written down. */
  uint16_t stack = (uint16_t)(cpu->sp - 2);
  if (stack < SPECTRUM_RAM_BASE || cpu->sp < SPECTRUM_RAM_BASE) {
    *problem = "has its stack below the RAM, where the program counter cannot be pushed";
    return false;
  }

  memset(bytes, 0, SPECTRUM_SNAPSHOT_SIZE);
  bytes[AT_I] = cpu->i;
  write16(bytes, AT_SHADOW_HL, cpu->hl_);
  write16(bytes, AT_SHADOW_DE, cpu->de_);
  write16(bytes, AT_SHADOW_BC, cpu->bc_);
  write16(bytes, AT_SHADOW_AF, cpu->af_);
  write16(bytes, AT_HL, (uint16_t)((cpu->h << 8) | cpu->l));
  write16(bytes, AT_DE, (uint16_t)((cpu->d << 8) | cpu->e));
  write16(bytes, AT_BC, (uint16_t)((cpu->b << 8) | cpu->c));
  write16(bytes, AT_IY, (uint16_t)((cpu->iyh << 8) | cpu->iyl));
  write16(bytes, AT_IX, (uint16_t)((cpu->ixh << 8) | cpu->ixl));
  bytes[AT_INTERRUPT] = cpu->iff2 ? IFF2_BIT : 0x00;
  bytes[AT_R] = cpu->r;
  write16(bytes, AT_AF, (uint16_t)((cpu->a << 8) | cpu->f));
  write16(bytes, AT_SP, stack);
  bytes[AT_INTERRUPT_MODE] = cpu->im;
  bytes[AT_BORDER] = spectrum->ula.border;
  memcpy(bytes + AT_RAM, spectrum->ram, SPECTRUM_RAM_48K);
  write16(bytes, AT_RAM + (size_t)(stack - SPECTRUM_RAM_BASE), cpu->pc);
  return true;
}
