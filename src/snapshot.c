/*
 * snapshot.c — the header, field by field.
 */
#include <string.h>

#include "snapshot.h"

/* Where each field sits in the header, in the order the format lists them.
   Named rather than counted, because an off-by-one here is a machine that
   resumes with the wrong accumulator and no obvious reason why. */
#define AT_IDENTIFIER 0x00
#define AT_VERSION 0x10
#define AT_F 0x11
#define AT_A 0x12
#define AT_C 0x13
#define AT_B 0x14
#define AT_E 0x15
#define AT_D 0x16
#define AT_L 0x17
#define AT_H 0x18
#define AT_R 0x19
#define AT_I 0x1A
#define AT_IFF1 0x1B /* the format calls this IFF0 */
#define AT_IFF2 0x1C /* and this IFF1 */
#define AT_IX 0x1D
#define AT_IY 0x1F
#define AT_SP 0x21
#define AT_PC 0x23
#define AT_INTERRUPT_MODE 0x25
#define AT_SHADOW_F 0x26
#define AT_SHADOW_A 0x27
#define AT_SHADOW_C 0x28
#define AT_SHADOW_B 0x29
#define AT_SHADOW_E 0x2A
#define AT_SHADOW_D 0x2B
#define AT_SHADOW_L 0x2C
#define AT_SHADOW_H 0x2D
#define AT_SELECTED_PEN 0x2E
#define AT_PALETTE 0x2F /* 17: pens 0-15, then the border */
#define AT_ROM_AND_MODE 0x40
#define AT_RAM_CONFIGURATION 0x41
#define AT_SELECTED_CRTC_REGISTER 0x42
#define AT_CRTC_REGISTERS 0x43 /* 18 */
#define AT_UPPER_ROM 0x55
#define AT_PPI_A 0x56
#define AT_PPI_B 0x57
#define AT_PPI_C 0x58
#define AT_PPI_CONTROL 0x59
#define AT_SELECTED_PSG_REGISTER 0x5A
#define AT_PSG_REGISTERS 0x5B /* 16 */
#define AT_DUMP_KILOBYTES 0x6B

static const char identifier[8] = {'M', 'V', ' ', '-', ' ', 'S', 'N', 'A'};

static uint16_t read16(const uint8_t *bytes, size_t at) {
  return (uint16_t)(bytes[at] | (bytes[at + 1] << 8));
}

static void write16(uint8_t *bytes, size_t at, uint16_t value) {
  bytes[at] = (uint8_t)value;
  bytes[at + 1] = (uint8_t)(value >> 8);
}

size_t snapshot_size(const cpc_t *cpc) {
  /* The format writes 64K or 128K and nothing between. */
  return SNAPSHOT_HEADER_SIZE + (cpc->ram_size >= 0x20000 ? 0x20000u : 0x10000u);
}

bool snapshot_load(cpc_t *cpc, const uint8_t *bytes, size_t length, const char **problem) {
  if (length < SNAPSHOT_HEADER_SIZE) {
    *problem = "too short to hold a snapshot header";
    return false;
  }
  if (memcmp(bytes + AT_IDENTIFIER, identifier, sizeof identifier) != 0) {
    *problem = "does not begin with \"MV - SNA\"";
    return false;
  }
  uint8_t version = bytes[AT_VERSION];
  if (version < 1 || version > 3) {
    *problem = "is a snapshot version this does not read";
    return false;
  }
  size_t dump = (size_t)read16(bytes, AT_DUMP_KILOBYTES) * 1024;
  if (dump != 0x10000 && dump != 0x20000) {
    *problem = "declares a memory dump that is neither 64K nor 128K";
    return false;
  }
  if (length < SNAPSHOT_HEADER_SIZE + dump) {
    *problem = "is shorter than the memory dump it declares";
    return false;
  }
  if (dump > cpc->ram_size) {
    *problem = "holds more RAM than this machine has";
    return false;
  }

  /* The CPU resumes between instructions: everything mid-flight in its
     micro-program is cleared, then the registers are laid in. */
  z80_init(&cpc->cpu);
  cpc->cpu.f = bytes[AT_F];
  cpc->cpu.a = bytes[AT_A];
  cpc->cpu.c = bytes[AT_C];
  cpc->cpu.b = bytes[AT_B];
  cpc->cpu.e = bytes[AT_E];
  cpc->cpu.d = bytes[AT_D];
  cpc->cpu.l = bytes[AT_L];
  cpc->cpu.h = bytes[AT_H];
  cpc->cpu.r = bytes[AT_R];
  cpc->cpu.i = bytes[AT_I];
  cpc->cpu.iff1 = (bytes[AT_IFF1] & 1) != 0;
  cpc->cpu.iff2 = (bytes[AT_IFF2] & 1) != 0;
  cpc->cpu.ixl = bytes[AT_IX];
  cpc->cpu.ixh = bytes[AT_IX + 1];
  cpc->cpu.iyl = bytes[AT_IY];
  cpc->cpu.iyh = bytes[AT_IY + 1];
  cpc->cpu.sp = read16(bytes, AT_SP);
  cpc->cpu.pc = read16(bytes, AT_PC);
  cpc->cpu.im = bytes[AT_INTERRUPT_MODE] & 0x03;
  cpc->cpu.af_ = (uint16_t)((bytes[AT_SHADOW_A] << 8) | bytes[AT_SHADOW_F]);
  cpc->cpu.bc_ = (uint16_t)((bytes[AT_SHADOW_B] << 8) | bytes[AT_SHADOW_C]);
  cpc->cpu.de_ = (uint16_t)((bytes[AT_SHADOW_D] << 8) | bytes[AT_SHADOW_E]);
  cpc->cpu.hl_ = (uint16_t)((bytes[AT_SHADOW_H] << 8) | bytes[AT_SHADOW_L]);

  /* Bit 4 of the pen byte names the border, as it does on the port. */
  uint8_t pen = bytes[AT_SELECTED_PEN];
  cpc->gate_array.pen = (pen & 0x10) ? 16 : (pen & 0x0F);
  for (int index = 0; index < 17; index++) {
    cpc->gate_array.inks[index] = bytes[AT_PALETTE + index] & 0x1F;
  }
  /* The stored byte is the last RMR command written, so it goes back in
     through the same door. A mode change would normally wait for the next
     HSYNC; a machine being restored has no such moment coming. */
  gate_array_write(&cpc->gate_array, bytes[AT_ROM_AND_MODE]);
  cpc->gate_array.mode = cpc->gate_array.mode_pending;

  /* Only the configuration is stored, without the two bits that mark a
     command as the PAL's. */
  cpc->mmr = (uint8_t)(0xC0 | (bytes[AT_RAM_CONFIGURATION] & 0x3F));
  cpc->upper_rom_number = bytes[AT_UPPER_ROM];

  /* Through the chip's own bus, so its writable-bit masks apply and a
     snapshot cannot install a value no 6845 could hold. Selecting each
     register in turn clobbers the selection, so that goes back last. */
  for (int index = 0; index < 18; index++) {
    crtc_access(&cpc->crtc, CRTC_CS | crtc_set_data(0, (uint8_t)index));
    crtc_access(&cpc->crtc, CRTC_CS | CRTC_RS | crtc_set_data(0, bytes[AT_CRTC_REGISTERS + index]));
  }
  crtc_access(&cpc->crtc, CRTC_CS | crtc_set_data(0, bytes[AT_SELECTED_CRTC_REGISTER]));

  /* The control word first, because setting it clears the output latches.
     The format stores inputs for A and B, outputs for C. */
  ppi_write(&cpc->ppi, PPI_CONTROL, bytes[AT_PPI_CONTROL] | 0x80);
  cpc->ppi.input[PPI_PORT_A] = bytes[AT_PPI_A];
  cpc->ppi.input[PPI_PORT_B] = bytes[AT_PPI_B];
  cpc->ppi.output[PPI_PORT_C] = bytes[AT_PPI_C];

  cpc->psg.selected = bytes[AT_SELECTED_PSG_REGISTER] & 0x0F;
  for (int index = 0; index < 16; index++) {
    cpc->psg.registers[index] = bytes[AT_PSG_REGISTERS + index];
  }

  memcpy(cpc->ram, bytes + SNAPSHOT_HEADER_SIZE, dump);
  cpc_remap(cpc);
  return true;
}

bool snapshot_save(const cpc_t *cpc, uint8_t *bytes, size_t capacity, const char **problem) {
  size_t needed = snapshot_size(cpc);
  if (capacity < needed) {
    *problem = "there is not room for the snapshot";
    return false;
  }
  /* The format records a PC and no way to say the instruction at it is
     half done, so this is refused rather than written wrong. See
     cpc_finish_instruction. */
  if (!z80_instruction_complete(&cpc->cpu)) {
    *problem = "the CPU is in the middle of an instruction";
    return false;
  }
  memset(bytes, 0, needed);
  memcpy(bytes + AT_IDENTIFIER, identifier, sizeof identifier);
  bytes[AT_VERSION] = 1;

  bytes[AT_F] = cpc->cpu.f;
  bytes[AT_A] = cpc->cpu.a;
  bytes[AT_C] = cpc->cpu.c;
  bytes[AT_B] = cpc->cpu.b;
  bytes[AT_E] = cpc->cpu.e;
  bytes[AT_D] = cpc->cpu.d;
  bytes[AT_L] = cpc->cpu.l;
  bytes[AT_H] = cpc->cpu.h;
  bytes[AT_R] = cpc->cpu.r;
  bytes[AT_I] = cpc->cpu.i;
  bytes[AT_IFF1] = cpc->cpu.iff1 ? 1 : 0;
  bytes[AT_IFF2] = cpc->cpu.iff2 ? 1 : 0;
  bytes[AT_IX] = cpc->cpu.ixl;
  bytes[AT_IX + 1] = cpc->cpu.ixh;
  bytes[AT_IY] = cpc->cpu.iyl;
  bytes[AT_IY + 1] = cpc->cpu.iyh;
  write16(bytes, AT_SP, cpc->cpu.sp);
  write16(bytes, AT_PC, cpc->cpu.pc);
  bytes[AT_INTERRUPT_MODE] = cpc->cpu.im;
  bytes[AT_SHADOW_F] = (uint8_t)cpc->cpu.af_;
  bytes[AT_SHADOW_A] = (uint8_t)(cpc->cpu.af_ >> 8);
  bytes[AT_SHADOW_C] = (uint8_t)cpc->cpu.bc_;
  bytes[AT_SHADOW_B] = (uint8_t)(cpc->cpu.bc_ >> 8);
  bytes[AT_SHADOW_E] = (uint8_t)cpc->cpu.de_;
  bytes[AT_SHADOW_D] = (uint8_t)(cpc->cpu.de_ >> 8);
  bytes[AT_SHADOW_L] = (uint8_t)cpc->cpu.hl_;
  bytes[AT_SHADOW_H] = (uint8_t)(cpc->cpu.hl_ >> 8);

  bytes[AT_SELECTED_PEN] = cpc->gate_array.pen >= 16 ? 0x10 : cpc->gate_array.pen;
  for (int index = 0; index < 17; index++) {
    bytes[AT_PALETTE + index] = cpc->gate_array.inks[index] & 0x1F;
  }
  /* Reassembled from the state the register left behind, because the chip
     keeps what the byte meant rather than the byte. */
  bytes[AT_ROM_AND_MODE] =
      (uint8_t)(0x80 | (cpc->gate_array.upper_rom_enabled ? 0 : 0x08) |
                (cpc->gate_array.lower_rom_enabled ? 0 : 0x04) | (cpc->gate_array.mode & 0x03));
  bytes[AT_RAM_CONFIGURATION] = cpc->mmr & 0x3F;
  bytes[AT_UPPER_ROM] = cpc->upper_rom_number;

  bytes[AT_SELECTED_CRTC_REGISTER] = cpc->crtc.address_register;
  for (int index = 0; index < 18; index++) {
    bytes[AT_CRTC_REGISTERS + index] = cpc->crtc.registers[index];
  }

  bytes[AT_PPI_A] = cpc->ppi.input[PPI_PORT_A];
  bytes[AT_PPI_B] = cpc->ppi.input[PPI_PORT_B];
  bytes[AT_PPI_C] = cpc->ppi.output[PPI_PORT_C];
  bytes[AT_PPI_CONTROL] = cpc->ppi.control | 0x80;

  bytes[AT_SELECTED_PSG_REGISTER] = cpc->psg.selected;
  for (int index = 0; index < 16; index++) {
    bytes[AT_PSG_REGISTERS + index] = cpc->psg.registers[index];
  }

  size_t dump = needed - SNAPSHOT_HEADER_SIZE;
  write16(bytes, AT_DUMP_KILOBYTES, (uint16_t)(dump / 1024));
  memcpy(bytes + SNAPSHOT_HEADER_SIZE, cpc->ram, dump);
  return true;
}
