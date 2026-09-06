/*
 * crtc.c — counters and triggers, as the Compendium teaches.
 */
#include "crtc.h"

/* Type 0 — Compendium ch. 4.3. R3 carries the VSYNC width in its high
   nibble and the HSYNC width in its low one; R8's interlace and skew bits
   are stored but nothing reads them yet. R16/R17 are the lightpen latches,
   read-only. */
static const uint8_t writable_bits[18] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F, 0xF3,
    0x1F, 0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF, 0x00, 0x00,
};

/* The counters are narrower than the bytes that hold them. This is what a
   program overruns when it writes a limit below the counter watching it:
   the counter runs to its own top and loops, rather than never matching
   again (ch. 10.3.1.1, 12.1). */
#define C4_BITS 0x7F
#define C9_BITS 0x1F

void crtc_init(crtc_t *crtc) { *crtc = (crtc_t){0}; }

/* Moving C4 lifts the VSYNC block, because the comparison with R7 has
   changed; setting it to the value it already held does not (ch. 16.3). */
static void enter_character_row(crtc_t *crtc, uint8_t row) {
  uint8_t next = row & C4_BITS;
  if (next != crtc->c4) {
    crtc->vsync_blocked = false;
  }
  crtc->c4 = next;
}

/* C3h counts VSYNC scanlines on its 4 bits, so a width of 0 runs the full 16
   (ch. 6.1.2). */
static void enter_scanline(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->vsync) {
    crtc->c3h = (crtc->c3h + 1) & 0x0F;
    if (crtc->c3h == (r[3] >> 4)) {
      crtc->vsync = false;
    }
  }

  if (crtc->last_line) {
    crtc->c9 = 0;
    enter_character_row(crtc, 0);
  } else if (crtc->c9_against_r5 && crtc->c4 != r[4]) {
    /* The adjustment's own lines. C9 counts against R5 here, and C4 stands
       still because it no longer matches R4 — the line that carried it past
       R4 was the last line, counted the ordinary way (ch. 11.2.2). */
    if (((crtc->c9 + 1) & C9_BITS) == r[5]) {
      crtc->c9_against_r5 = false;
      crtc->in_vertical_adjustment = false;
      crtc->c9 = 0;
      enter_character_row(crtc, 0);
    } else {
      crtc->c9 = (uint8_t)((crtc->c9 + 1) & C9_BITS);
    }
  } else if (crtc->c9 == r[9]) {
    crtc->c9 = 0;
    enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
  } else {
    crtc->c9 = (uint8_t)((crtc->c9 + 1) & C9_BITS);
  }
}

/* C0 names the character being drawn and holds it for the whole of that
   microsecond, which is the position the Compendium gives a register write
   (ch. 13.2.1). Nothing reads it there yet.

   The first tick draws rather than advances, because a chip that has drawn
   nothing has no character to leave behind. That cannot be a sentinel in
   C0: R0 takes all 256 values C0 does, and 255 is one a program can write,
   so a sentinel there would read as the end of a 256-character line. */
static void enter_character(crtc_t *crtc) {
  if (!crtc->has_drawn_a_character) {
    crtc->has_drawn_a_character = true;
    return;
  }
  const uint8_t *r = crtc->registers;
  crtc->vma = (crtc->vma + 1) & 0x3FFF;
  if (crtc->hsync) {
    crtc->c3l = (crtc->c3l + 1) & 0x0F;
    if (crtc->c3l == (r[3] & 0x0F)) {
      crtc->hsync = false;
    }
  }
  if (crtc->c0 != r[0]) {
    crtc->c0++;
    if (crtc->c0 == 0) {
      /* R0 was moved under C0 and the counter came back the long way. */
      crtc->c0_reached_r0 = false;
    }
    return;
  }
  crtc->c0 = 0;
  crtc->c0_reached_r0 = true;
  enter_scanline(crtc);
}

/* Decided while C0 is 0 or 1, and this type "no longer repeats this test on
   the other values of C0>1" (ch. 12.2, 10.3.1.2), so a register written
   later in the line can neither take the state back nor set it.
   Re-evaluating it on an R4 or R9 update is type 2's rule (ch. 12.4.1). */
static void decide_last_line(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 < 2) {
    crtc->last_line = crtc->c4 == r[4] && crtc->c9 == r[9] && !crtc->in_vertical_adjustment;
  }
}

/* An R5 seen before C0 reaches 3 spends the line on a vertical adjustment
   instead of ending the frame, which is why the two states are exclusive.
   C4 standing past R4 does not disqualify the line: the overflow rule is
   written "excluding vertical adjustment", and an adjustment that finishes
   returns C4 to 0 from wherever it had climbed (ch. 11.2.2, 12.1, 12.2).
   This is the way back for a program that moved R4 under its own counter,
   and the reason a split screen resynchronises instead of drifting. */
static void begin_vertical_adjustment(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 < 3 && r[5] != 0 && crtc->c4 >= r[4] && crtc->c9 == r[9]) {
    crtc->c9_against_r5 = true;
    crtc->in_vertical_adjustment = true;
    crtc->last_line = false;
  }
}

/* VMA reloads from the VMA' latch where a scanline begins, and on the
   frame's first character both take R12/R13 — type 0 reloads when C4, C9
   and C0 stand at zero (ch. 20.3.1). VMA' then captures VMA where C0
   reaches R1 on the row's last scanline, so the next row starts R1
   characters further on (ch. 20.3.3). */
static void move_video_pointer(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 == 0) {
    if (crtc->c4 == 0 && crtc->c9 == 0) {
      crtc->vma_ = (uint16_t)(((r[12] << 8) | r[13]) & 0x3FFF);
    }
    crtc->vma = crtc->vma_;
  }
  if (crtc->c0 == r[1] && crtc->c9 == r[9]) {
    crtc->vma_ = crtc->vma;
  }
}

/* HSYNC begins on the character where C0 meets R2 (ch. 6.1.2). A width of
   zero is no HSYNC at all on this type — the 16 the other types read there
   is what a program uses to tell them apart (ch. 14.1, 14.5, 28.1.5). VSYNC
   begins where C4 meets R7, which is why writing R7 the value C4 already
   holds starts one where it stands; the block keeps that same equality from
   starting a second (ch. 16.3, 16.4.1). Each width is counted off where the
   counter it rides advances, so the ends are in enter_character and
   enter_scanline rather than here. */
static void begin_syncs(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 == r[2] && !crtc->hsync && (r[3] & 0x0F) != 0) {
    crtc->hsync = true;
    crtc->c3l = 0;
  }
  if (crtc->c4 == r[7] && !crtc->vsync && !crtc->vsync_blocked) {
    crtc->vsync = true;
    crtc->vsync_blocked = true;
    crtc->c3h = 0;
  }
}

/* DISPLAY ENABLE is two latches the equalities throw rather than two
   comparisons standing (ch. 6.1.3, 17.1, 18.1). R1's opens where the line
   begins and shuts where C0 meets R1; when R1 is 0 both fall on the same
   character and the opening wins (ch. 18.3.1). R6's shuts where C4 meets R6
   and nothing but a new frame opens it, and while it is shut R1 has no say
   (ch. 18.2.1, 18.2.2). The first line of a frame is exempt, which is what
   leaves an R6 of 0 cancellable there (ch. 18.3.2). */
static void throw_display_latches(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  bool first_line = crtc->c4 == 0 && crtc->c9 == 0;
  if (crtc->c0 == r[1]) {
    crtc->display_r1 = true;
  }
  if (crtc->c0 == 0 && crtc->c0_reached_r0) {
    crtc->display_r1 = false;
  }
  if (first_line && crtc->c0 == 0) {
    crtc->display_r6 = false;
  } else if (crtc->c4 == r[6] && !first_line) {
    crtc->display_r6 = true;
  }
}

static uint64_t pins_of(const crtc_t *crtc) {
  bool display = !crtc->display_r1 && !crtc->display_r6;
  return (uint64_t)(crtc->vma & 0x3FFF) | ((uint64_t)(crtc->c9 & 0x1F) << 24) |
         (display ? CRTC_DISPTMG : 0) | (crtc->hsync ? CRTC_HSYNC : 0) |
         (crtc->vsync ? CRTC_VSYNC : 0);
}

uint64_t crtc_tick(crtc_t *crtc) {
  enter_character(crtc);
  decide_last_line(crtc);
  begin_vertical_adjustment(crtc);
  move_video_pointer(crtc);
  begin_syncs(crtc);
  throw_display_latches(crtc);
  return pins_of(crtc);
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
    uint8_t mask = writable_bits[crtc->address_register];
    if (mask != 0) {
      crtc->registers[crtc->address_register] = crtc_data(pins) & mask;
      if (crtc->address_register == 7) {
        /* The other half of the block: writing R7 changes the comparison
           whatever the value written, so it can serve again (ch. 16.3). */
        crtc->vsync_blocked = false;
      }
    }
  }
  return pins;
}
