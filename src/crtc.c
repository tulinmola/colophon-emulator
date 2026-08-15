/*
 * crtc.c — counters and triggers, as the Compendium teaches.
 */
#include "crtc.h"

/* Writable bits per register on type 0 — Compendium ch. 4.3. R3 carries the
   VSYNC width in its high nibble and the HSYNC width in its low one; R8's
   interlace and skew bits are stored but nothing reads them yet. R16/R17
   are the lightpen latches, read-only. */
static const uint8_t register_widths[18] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F, 0xF3,
    0x1F, 0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF, 0x00, 0x00,
};

void crtc_init(crtc_t *crtc) { *crtc = (crtc_t){0}; }

uint64_t crtc_tick(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;

  /* A scanline begins: VMA reloads from the VMA' latch. On the frame's
     first character both take R12/R13 — type 0 reloads when C4, C9 and C0
     stand at zero (Compendium ch. 20.3.1). */
  if (crtc->c0 == 0) {
    if (crtc->c4 == 0 && crtc->c9 == 0 && !crtc->in_vertical_adjustment) {
      crtc->vma_ = (uint16_t)(((r[12] << 8) | r[13]) & 0x3FFF);
    }
    crtc->vma = crtc->vma_;
  }

  /* The row latch: VMA' captures VMA when C0 reaches R1 on the row's last
     scanline, and the next row starts R1 characters further on (ch.
     20.3.3). */
  if (crtc->c0 == r[1] && crtc->c9 == r[9]) {
    crtc->vma_ = crtc->vma;
  }

  /* HSYNC begins on the character where C0 meets R2 (ch. 6.1.2). */
  if (crtc->c0 == r[2] && !crtc->hsync) {
    crtc->hsync = true;
    crtc->c3l = 0;
  }

  bool display = crtc->c0 < r[1] && crtc->c4 < r[6];
  uint64_t pins = (uint64_t)(crtc->vma & 0x3FFF) | ((uint64_t)(crtc->c9 & 0x1F) << 24) |
                  (display ? CRTC_DISPTMG : 0) | (crtc->hsync ? CRTC_HSYNC : 0) |
                  (crtc->vsync ? CRTC_VSYNC : 0);

  /* Advance to the next character. */
  crtc->vma = (crtc->vma + 1) & 0x3FFF;
  if (crtc->hsync) {
    crtc->c3l = (crtc->c3l + 1) & 0x0F;
    if (crtc->c3l == (r[3] & 0x0F)) {
      crtc->hsync = false;
    }
  }
  if (crtc->c0 != r[0]) {
    crtc->c0++;
    return pins;
  }
  crtc->c0 = 0;

  /* A scanline ended. C3h counts VSYNC scanlines on its 4 bits, so a width
     of 0 runs the full 16 (ch. 6.1.2). */
  if (crtc->vsync) {
    crtc->c3h = (crtc->c3h + 1) & 0x0F;
    if (crtc->c3h == (r[3] >> 4)) {
      crtc->vsync = false;
    }
  }

  if (crtc->in_vertical_adjustment) {
    crtc->c5++;
    crtc->c9++;
    if (crtc->c5 == r[5]) {
      crtc->in_vertical_adjustment = false;
      crtc->c5 = 0;
      crtc->c9 = 0;
      crtc->c4 = 0;
      if (r[7] == 0 && !crtc->vsync) {
        crtc->vsync = true;
        crtc->c3h = 0;
      }
    }
  } else if (crtc->c9 == r[9]) {
    crtc->c9 = 0;
    if (crtc->c4 == r[4]) {
      if (r[5] == 0) {
        crtc->c4 = 0;
        if (r[7] == 0 && !crtc->vsync) {
          crtc->vsync = true;
          crtc->c3h = 0;
        }
      } else {
        crtc->in_vertical_adjustment = true;
        crtc->c5 = 0;
      }
    } else {
      crtc->c4++;
      /* VSYNC begins when C4 reaches R7, from the next scanline (ch.
         6.1.2). */
      if (crtc->c4 == r[7] && !crtc->vsync) {
        crtc->vsync = true;
        crtc->c3h = 0;
      }
    }
  } else {
    crtc->c9++;
  }
  return pins;
}

uint64_t crtc_access(crtc_t *crtc, uint64_t pins) {
  if (!(pins & CRTC_CS)) {
    return pins;
  }
  if (pins & CRTC_RW) {
    /* Type 0 drives the bus only for R12-R17 (Compendium ch. 4.3); the
       address register and the write-only registers leave it floating, and
       a nonexistent register reads 0. */
    if (!(pins & CRTC_RS)) {
      return pins;
    }
    uint8_t value = 0;
    if (crtc->address_register >= 12 && crtc->address_register <= 17) {
      value = crtc->registers[crtc->address_register];
    }
    return crtc_set_data(pins, value);
  }
  if (!(pins & CRTC_RS)) {
    crtc->address_register = crtc_data(pins) & 0x1F;
    return pins;
  }
  if (crtc->address_register < 18) {
    uint8_t mask = register_widths[crtc->address_register];
    if (mask != 0) {
      crtc->registers[crtc->address_register] = crtc_data(pins) & mask;
    }
  }
  return pins;
}
