/*
 * crtc.c — counters and triggers, as the Compendium teaches.
 */
#include "crtc.h"

/* Type 0 — Compendium ch. 4.3. R3 carries the VSYNC width in its high
   nibble and the HSYNC width in its low one; of R8 the two interlace bits
   and the two that skew the display are read, and the cursor's own skew is
   stored and no more, no host here wiring that pin. R16/R17 are the
   lightpen latches, read-only. */
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

void crtc_init(crtc_t *crtc) {
  *crtc = (crtc_t){0};
  crtc->vsync_armed = true;
  crtc->c9_processing_managed = true;
}

/* Moving C4 lifts the VSYNC block, because the comparison with R7 has
   changed; setting it to the value it already held does not (ch. 16.3). */
static void enter_character_row(crtc_t *crtc, uint8_t row) {
  uint8_t next = row & C4_BITS;
  if (next != crtc->c4) {
    crtc->vsync_blocked = false;
  }
  crtc->c4 = next;
}

/* R8's bits 5 and 4 carry the SKEW-DISPTMG field. Ch. 19.1's table gives
   it to types 0, 3 and 4 and withholds it from 1 and 2, and names its four
   values: Non Skew, one-character skew, two-character skew, and the
   Non-output that ch. 19.2 calls the BORDER ON function. */
#define SKEW_NONE 0
#define SKEW_ONE_CHARACTER 1
#define SKEW_TWO_CHARACTERS 2
#define SKEW_BORDER_ON 3

static uint8_t display_skew(const crtc_t *crtc) { return (crtc->registers[8] >> 4) & 3; }

/* Either interlace mode is asked for by R8's low bit, and the BORDER ON
   function takes that half of the register with it: "if the BORDER ON
   function is activated, the INTERLACE function on the 2 least significant
   bits is not considered", which ch. 19.2 marks as wanting further
   investigation and no evidence from outside this repository grades. */
static bool interlace_asked(const crtc_t *crtc) {
  return display_skew(crtc) != SKEW_BORDER_ON && (crtc->registers[8] & 1) != 0;
}

/* The line either interlace mode adds at the end of a frame is added on the
   parity R6 anticipated (ch. 19.6.1). */
static bool interlace_line_asked_for(const crtc_t *crtc) {
  return interlace_asked(crtc) && crtc->parity_r6;
}

/* The interlace video mode as R8 holds it, which is not always as the
   counters have it: R8's two low bits both set ask for it, and the chip
   takes it up at the head of the next line (ch. 19.1, 19.8.1). */
static bool interlace_video_asked(const crtc_t *crtc) {
  return interlace_asked(crtc) && (crtc->registers[8] & 2) != 0;
}

/* ParityC9, which fills bit 0 of the raster address in the interlace video
   mode: "ParityC9 = C4.0 xor ParityFrame" (ch. 19.5.2). Where R9 is odd the
   rows come out alternately even-lined and odd-lined as C4 advances, which
   is how a pair of them keeps the same length on both frames; where R9 is
   even every row takes the frame's own parity. The Compendium holds this in
   a state it updates at a row's end and only while R9 is odd, which leaves
   it stale where R9 is even; read from C4 it says the same thing, except
   where R9's own parity is changed inside a line — the stored state would
   carry the old parity to the row's end, and this one moves the raster
   address under the line being drawn, which is the very thing ch. 19.8.1
   gives the delayed take-up to prevent. Nothing we can run grades it. */
static bool parity_c9(const crtc_t *crtc) {
  if ((crtc->registers[9] & 1) != 0 && (crtc->c4 & 1) != 0) {
    return !crtc->parity_frame;
  }
  return crtc->parity_frame;
}

/* C9.VMA, the raster address (ch. 19.8.1). It is what leaves the chip on
   RA, what R9 is measured against, and what the late VSYNC of an odd row is
   timed by. In the interlace video mode a row covers two rows' worth of
   memory and the two frames take alternate lines of it, while the counter
   itself goes on counting by one — which is the half of this the
   Compendium's own tables get wrong. The shift carries out of five bits
   rather than widening, so a row entered off its parity comes round to its
   limit instead of missing it: for an R9 of 6, at C9=19. */
static uint8_t c9_vma(const crtc_t *crtc) {
  if (!crtc->interlace_video_mode) {
    return crtc->c9;
  }
  return (uint8_t)((((unsigned)crtc->c9 << 1) | (parity_c9(crtc) ? 1u : 0u)) & C9_BITS);
}

/* R9 read up to the nearest line of ParityC9's own parity, which is the
   limit a row ends on while the raster address carries parity in bit 0. The
   Compendium says this three ways that do not agree — "R9 + ParityFrame"
   (ch. 19.8.1), "R9 or ParityC9" (its note), and ch. 19.3.3's "it suffices
   to ignore bit 0 ... and to manage this bit 0 as that of frame parity" —
   and all three coincide where R9 is even. Where it is odd only this one
   answers the table in ch. 19.5.2, which for R9=7 runs a row of even lines
   to 8 and a row of odd lines to 7: the five-then-four that holds a pair of
   rows at nine lines. Reading up carries out of five bits at an R9 of 31,
   where it gives a limit of 0 and rows one line long — undocumented, and
   the alternative is a limit no address can reach and a frame that never
   ends. */
static uint8_t r9_with_parity(const crtc_t *crtc) {
  unsigned r9 = crtc->registers[9];
  return (uint8_t)((r9 + ((r9 ^ (parity_c9(crtc) ? 1u : 0u)) & 1u)) & C9_BITS);
}

/* Writing R8 moves two things and they do not move together (ch. 19.8.1):
   the parity in the limit follows R8 at once — "the parity is however
   considered immediately for R9" — while the doubling of what is measured
   against it waits for the next C0=0. So the line a mode is entered on
   measures C9 against a limit that already carries parity, and the line it
   is left on measures the raster address against one that no longer does.
   Both counting bugs the Compendium offers a program as a way of reading
   its current parity are those two lines: a mode entered on C9=R9 with an
   odd parity finds the limit a line higher and overruns, and one left on
   C9.VMA=R9+1 finds it a line lower (ch. 19.5.2).

   On the line a mode is entered the limit takes ParityC9 and not
   ParityFrame. The two differ only where R9 is odd, and every table that
   enters a mode does so either with an even R9 or at C9=0, where the
   comparison cannot bite — so the Compendium settles this nowhere.
   ParityC9 is the bit that will fill the address, and a limit of the other
   parity is one no address of that row could ever meet. */
static bool row_is_on_its_last_scanline(const crtc_t *crtc) {
  return c9_vma(crtc) == (interlace_video_asked(crtc) ? r9_with_parity(crtc) : crtc->registers[9]);
}

/* A frame begins where the last one is done with, whatever the counters
   read on the way: C4 of 127 carries the interlace line itself to a C0, C4
   and C9 all zero, and a frame that read its own head off those would renew
   the line under the line it had just given and never end. One interlace
   line to a frame (ch. 11.9), and the frame it was given to is what spends
   it, not the adjustment that carried it. */
static void begin_frame(crtc_t *crtc) {
  crtc->vertical_adjustment_in_progress = false;
  crtc->c9 = 0;
  crtc->interlace_line_given = false;
  enter_character_row(crtc, 0);
}

/* A line that never reaches C0=1 leaves C9's management disabled, and then
   "all of the CRTC counters are frozen as long as R0=0" (ch. 13.2.1) — the
   VSYNC's line counter with them, which is why a VSYNC begun there "is not
   deactivated if R3h was worth 1" (ch. 16.4.1.2). One thing still lands:
   the C4 increment the last managed boundary armed, and once only, because
   "this increment is deactivated because it has taken place" (ch. 13.2.4).
   In a line that does reach C0=1 the arming and the landing fall on the
   same boundary, so nothing here is felt. */
static void enter_scanline(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  /* The disarm is read at C0=3 because a write made at C0=2 lands there, and
     a line of three characters has no C0=3 to read it at — so it is read
     here, where that write has landed just the same. The chip parts its
     narrow lines at "R0 < 2" (ch. 11.2.2, 12.2) and so does this. */
  if (r[0] == 2 && r[5] == 0 && !interlace_line_asked_for(crtc) &&
      !crtc->vertical_adjustment_in_progress) {
    crtc->vertical_adjustment_armed = false;
  }
  if (!crtc->c9_processing_managed) {
    if (crtc->c4_increment_armed) {
      crtc->c4_increment_armed = false;
      enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
    }
    /* The one register the freeze does not shut out: "updates to registers
       R4, R5 and R9 are no longer considered as long as R0=0. On the other
       hand, R8 continues to be considered each time C0=0" (ch. 13.2.1). */
    crtc->interlace_video_mode = interlace_video_asked(crtc);
    return;
  }
  /* C3h counts VSYNC scanlines on its 4 bits, so a width of 0 runs the full
     16 (ch. 6.1.2). */
  if (crtc->vsync_began_mid_line) {
    /* A VSYNC that began away from the head of a line has its counter
       initialized at the next C0=0 rather than advanced there, which leaves
       the pulse longer than R3's high nibble by the rest of the line it
       began in: one begun during line 1 of 16 ends at the end of line 17
       (ch. 16.4.1). */
    crtc->c3h = 0;
    crtc->vsync_began_mid_line = false;
  } else if (crtc->vsync) {
    crtc->c3h = (crtc->c3h + 1) & 0x0F;
    if (crtc->c3h == (r[3] >> 4)) {
      crtc->vsync = false;
    }
  }

  if (crtc->vertical_adjustment_armed) {
    /* R5 is a quantity of lines, and C9 is compared with R9 before its
       increment: the line the chip would move to becomes the adjustment's
       own unless that line has reached R5 (ch. 13.2.4). Once C4 has left R4
       behind, C9 can no longer be zeroed, which is what lets it climb past
       R9 to reach an R5 larger than a row (ch. 11.2.2). "If C9==R9 then
       C4=C4+1, and in additional management only once if C4 was worth R4"
       (ch. 13.2.4) — so both the zeroing and the increment ride on the row
       having reached its last line, and C4 returns to 0 when the adjustment
       is done whatever R4 holds by then. */
    bool row_ended_on_r4 = row_is_on_its_last_scanline(crtc) && crtc->c4 == r[4];
    uint8_t next_c9 = row_ended_on_r4 ? 0 : (uint8_t)((crtc->c9 + 1) & C9_BITS);
    bool r5_lines_spent = next_c9 == r[5];
    if (r5_lines_spent && crtc->interlace_line_owed && !crtc->interlace_line_given) {
      /* The R5 lines are spent and interlace asks for one more, which is
         the last of them (ch. 19.6.1). C4 has already been incremented once
         for all the additional lines there are. */
      crtc->interlace_line_given = true;
      crtc->vertical_adjustment_in_progress = true;
      crtc->c9 = next_c9;
      if (row_ended_on_r4) {
        enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
      }
    } else if (r5_lines_spent || crtc->interlace_line_given) {
      crtc->vertical_adjustment_armed = false;
      begin_frame(crtc);
    } else {
      crtc->vertical_adjustment_in_progress = true;
      crtc->c9 = next_c9;
      if (row_ended_on_r4) {
        enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
      }
    }
  } else if (crtc->last_line) {
    begin_frame(crtc);
  } else if (row_is_on_its_last_scanline(crtc)) {
    crtc->c9 = 0;
    enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
  } else {
    crtc->c9 = (uint8_t)((crtc->c9 + 1) & C9_BITS);
  }

  /* And the doubling R8 asks for is taken up here rather than where it was
     written: the status it sets "will be performed on the next C0=0, after
     the C9/R9 test of the line" (ch. 19.8.1), which is what keeps the
     raster address from moving under a line already being drawn. */
  crtc->interlace_video_mode = interlace_video_asked(crtc);

  /* What the next boundary would do to C4, tested here as the chip tests it
     on the characters C0 names 0, 1 and 2, and kept against a boundary that
     finds C9 frozen (ch. 13.2.4). It must ask what the body above asks, or
     it does not predict it: an adjustment moves C4 once for all its lines
     together, "and in additional management one only once if C4 was worth
     R4" (ch. 13.2.1). */
  crtc->c4_increment_armed =
      row_is_on_its_last_scanline(crtc) && (!crtc->vertical_adjustment_armed || crtc->c4 == r[4]);
  crtc->c9_processing_managed = false;
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
    if (crtc->c0 == 1) {
      /* "C9 processing management ... would in principle be activated on
         C0=1 if C0 succeeded in reaching this value" (ch. 13.2.4). */
      crtc->c9_processing_managed = true;
    }
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
    crtc->last_line = crtc->c4 == r[4] && row_is_on_its_last_scanline(crtc);
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
  /* A last line whose comparison held while C0 was 0 and 1 and does not
     hold now, because R4 or R9 moved under it, spends itself on an
     adjustment instead of ending the frame, and no R5 above 0 is wanted for
     that (ch. 10.3.1.2, 12.2, 13.2.1). Only a write made during the
     character C0 named 1 can leave things so: one made at C0=0 would have
     been seen by the last line's own second look, which is why the state
     and the comparison can disagree here and nowhere else. R8 is a third
     register that can do it, by moving the parity the limit is read up to,
     and the Compendium does not say whether the chip does: it names this
     comparison only where a row ends and where VMA' is captured, and one
     comparator makes it so everywhere. */
  if (crtc->c0 == 2 && crtc->last_line &&
      (crtc->c4 != r[4] || !row_is_on_its_last_scanline(crtc))) {
    /* "The current line becomes the 'first' adjustment line" (ch. 10.3.1.2),
       and a line already begun is past the disarm below (ch. 13.2.6). The
       arming itself was done on the characters C0 named 0 and 1. */
    crtc->vertical_adjustment_in_progress = true;
  }
  /* The chip arms on any last line and asks what it is for afterwards: ch.
     12.1 gives the window, "this management of additional line(s) is managed
     when C0<2", ch. 13.2.5 what happens in it — "the CRTC assesses whether
     it is on the last line, and if so, arms an internal flag by default" —
     and leaves C0=2 to "assess the conditions for disarming ... in
     particular by testing the value of R5". The assessment takes an arm back
     on R5 alone, which is what lets a last line unmade at C0=0 unmake the
     arming with it (ch. 12.2). The interlace line is left out: its own
     question is put "on the last line of a frame" (ch. 11.9), and a line
     whose last-line state has just been unmade is not one. That reading is
     ours, and nothing we can run grades it. */
  if (crtc->c0 < 2 && crtc->last_line) {
    crtc->vertical_adjustment_armed = true;
  } else if (crtc->c0 < 2 && r[5] == 0 && !crtc->vertical_adjustment_in_progress) {
    crtc->vertical_adjustment_armed = false;
  }
  /* R5 admits a line whose C4 has already gone past R4, which that
     assessment cannot see. It counts on the characters C0 names 0, 1 and 2,
     and a write lands in the microsecond after the tick that named it, so
     the last tick that sees one in time is the one naming 3 (ch. 11.2.2,
     12.2, 13.2.1). */
  if (crtc->c0 < 4 && r[5] != 0 && crtc->c4 >= r[4] && row_is_on_its_last_scanline(crtc)) {
    crtc->vertical_adjustment_armed = true;
  }
  /* And the same deadline read the other way: a line armed on an R5 that
     the same line goes on to cancel is disarmed, and the last line it stood
     in front of is simply left standing. What is tested there is both of
     the things that ask for an additional line — "the additional management
     state is deactivated if there was no line programmed (R5=0 or no
     'Interlace Line'" — so a frame the interlace still asks a line of keeps
     its state whatever R5 has become (ch. 13.2.1, 13.2.5). It asks nothing
     of the counters, so it reaches a line the R5 arm admitted with C4
     already past R4 as readily as any other. What it cannot reach is an
     adjustment already begun (ch. 13.2.6), which is the work the counter
     test that stood here before was doing by accident. */
  if (crtc->c0 == 3 && r[5] == 0 && !interlace_line_asked_for(crtc) &&
      !crtc->vertical_adjustment_in_progress) {
    crtc->vertical_adjustment_armed = false;
  }
  /* The line interlace adds is additional-line handling too, and asks for
     no R5 at all (ch. 19.6.1). Ch. 11.9 gives it a deadline of its own,
     later than R5's: the condition "is evaluated on the last line of a
     frame, when C0=R0", and "this latest line can be one of the adjustment
     lines displayed via R5" — so a program may turn the line on or off from
     inside an adjustment, long after R5's own window has shut. The answer
     is kept because the line it decides cannot begin until the next
     character. A frame that would have ended here is held open for it, and
     one already held open by R5 needs no holding. Shaker points its C (P)
     group at this and states its verdict in a picture, so nothing we can
     run grades the deadline: it stands on the chapter's own sentence and
     on our tests. */
  if (crtc->c0 == r[0]) {
    crtc->interlace_line_owed = interlace_line_asked_for(crtc);
    if (crtc->interlace_line_owed && crtc->last_line) {
      crtc->vertical_adjustment_armed = true;
    }
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
  if (crtc->c0 == r[1] && row_is_on_its_last_scanline(crtc)) {
    crtc->vma_ = crtc->vma;
  }
}

/* The equality both ch. 13.2.2 and ch. 16.3 turn on. */
static bool c4_stands_on_r7(const crtc_t *crtc) { return crtc->c4 == crtc->registers[7]; }

/* "Each time C0=2, a state validates the update of C4=R7 on the next C0=0.
   This state is cancelled when C0=0" (ch. 13.2.2). It governs the equality
   C4 walks into and nothing else: "the counter C0 needs to reach the value
   2 on the line preceding that where C4=R7 for a VSYNC to be considered ...
   the VSYNC will be blocked for the condition C4=R7, as if it had had
   place" (ch. 16.4.1.2). Unread is therefore spent, and the block is what
   carries it, so the comparison itself is left to stand as it stands and
   only the liftings of ch. 16.3 bring it round again. C4 cannot move inside
   a line, and where it does move the block is lifted with it. */
static void authorize_vsync(crtc_t *crtc) {
  if (crtc->c0 == 2) {
    crtc->vsync_armed = true;
    return;
  }
  if (crtc->c0 != 0) {
    return;
  }
  if (!crtc->vsync_armed) {
    crtc->vsync_blocked = true;
  }
  crtc->vsync_armed = false;
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
  /* On an even frame in either interlace mode the VSYNC is a MID-VSYNC:
     the C4/R7 equality does not start it where it falls, but where C0
     reaches R0/2, which is the half line the second field is raised by
     (ch. 19.7.2). */
  bool mid_vsync = interlace_asked(crtc) && !crtc->parity_frame;
  /* And where the video mode gives a row an odd number of lines, an odd C4
     of an odd frame starts its VSYNC a line late, at C9.VMA=2 rather than
     at the row's own first line: the two frames' rows are of unequal length
     there, and this is what still leaves their syncs half a line apart
     (ch. 19.5.2, 19.7.1). It never meets a MID-VSYNC, which happens only on
     an even frame. A row of one line never reaches C9.VMA=2 and so raises
     no VSYNC at all, which an R9 of 31 makes of every even-parity row; the
     Compendium describes neither. */
  bool late_vsync =
      crtc->interlace_video_mode && (r[9] & 1) != 0 && (crtc->c4 & 1) != 0 && crtc->parity_frame;
  if (c4_stands_on_r7(crtc) && !crtc->vsync && !crtc->vsync_blocked &&
      (!mid_vsync || crtc->c0 == r[0] / 2) && (!late_vsync || c9_vma(crtc) == 2)) {
    crtc->vsync = true;
    crtc->vsync_blocked = true;
    crtc->c3h = 0;
    crtc->vsync_began_mid_line = crtc->c0 != 0;
  }
}

/* The character the R1 border is raised on: where C0 meets R1, or where C0
   meets R0 having not met R1 all line, since "the condition C0=R1 not being
   met during the line ... the condition C0=R0 therefore replaces the
   condition C0=R1" (ch. 19.2.4). R1 standing beyond the line's end is the
   common way to miss it, but not the only one: an R1 of 0 loses to the
   opening on the character they share (ch. 18.3.1), and an R1 moved behind
   C0 is never met again either, so what is read here is the latch rather
   than the registers.

   The substitution needs somewhere for the border to go. Only a delay gives
   it a character of its own; with none the chip sends the half character of
   ch. 17.6.2 early instead and this latch is left alone, and the BORDER ON
   function is no delay and gets no character either. Where the border is
   handed out is a separate question, which the skew answers. */
static bool border_r1_begins_here(const crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 == r[1]) {
    return true;
  }
  uint8_t skew = display_skew(crtc);
  bool delayed = skew == SKEW_ONE_CHARACTER || skew == SKEW_TWO_CHARACTERS;
  return delayed && !crtc->display_r1 && crtc->c0 == r[0];
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
  /* A skew holds the signal back on its way out rather than moving the
     equalities that throw it, so the latch is thrown where the registers
     say and read out a character or two later. That is what leaves the line
     whole when a delay is taken off after the border it deferred: the
     opening was recorded where the equality fell, and the skew only chooses
     which character hands it to the pin (ch. 19.2.3, 19.2.5.3). */
  crtc->display_r1_earlier[1] = crtc->display_r1_earlier[0];
  crtc->display_r1_earlier[0] = crtc->display_r1;
  if (border_r1_begins_here(crtc)) {
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

/* Two rules move DISPLAY ENABLE half a character, and both move it the same
   way: the byte a character begins with is displayed and the byte it ends
   with is border.

   The first is the line's last character where R1 was never reached. This
   chip runs ahead of the characters the Gate Array is drawing and sends its
   BORDER ON "0.5 µsec too early", so one byte of border stands before C0
   goes to 0 and the BORDER OFF follows on the next character. Where R1 was
   reached the display is off already and there is nothing to move (ch.
   17.6.2). Never reached is wider than R1 standing above R0: an R1 of 0
   loses its own character to the opening (ch. 18.3.1) and so never shuts
   the display at all, and an R1 moved out of C0's way mid-line has not shut
   it either. The Compendium settles neither case outright, and nothing we
   can run grades them.

   The second is a frame's first line where R6 is 0. C4 reaching R6 asks for
   the border and the new frame takes it away again; the two land a byte
   apart, so the line comes out an alternation of displayed and bordered
   bytes with the video pointer counting through both (ch. 18.3.2). */
static uint64_t pins_of(const crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  /* The BORDER ON function is not a count and takes no place among the
     delays: it shuts the display where it stands and leaves the video
     pointer and the R6 border carrying on as they were (ch. 19.2.1). */
  uint8_t skew = display_skew(crtc);
  bool border_r1 = crtc->display_r1;
  if (skew == SKEW_ONE_CHARACTER || skew == SKEW_TWO_CHARACTERS) {
    border_r1 = crtc->display_r1_earlier[skew - 1];
  }
  bool display = !border_r1 && !crtc->display_r6 && skew != SKEW_BORDER_ON;
  bool r6_conflict = crtc->c4 == 0 && crtc->c9 == 0 && r[6] == 0;
  /* Where a delay is programmed the border of a line R1 never reached is a
     character of its own at the deferred place, not the half character the
     chip would otherwise send early (ch. 17.6.2, 19.2.4). */
  bool border_takes_the_second_byte = skew == SKEW_NONE && crtc->c0 == r[0];
  bool second_byte = display && !border_takes_the_second_byte && !r6_conflict;
  return (uint64_t)(crtc->vma & 0x3FFF) | ((uint64_t)c9_vma(crtc) << 24) |
         (display ? CRTC_DISPTMG : 0) | (second_byte ? CRTC_DISPTMG_SECOND_BYTE : 0) |
         (crtc->hsync ? CRTC_HSYNC : 0) | (crtc->vsync ? CRTC_VSYNC : 0);
}

/* ParityFrame takes what ParityR6 anticipated, at the frame's first
   character; ParityR6 then anticipates the next frame's, which is why it is
   read from ParityFrame and settled after it (ch. 19.5.2). Both come before
   the VSYNC is looked at, because "ParityFrame management takes priority
   over VSYNC management" — the case that turns on it is an R7 of 0, where
   the equality and the switch fall on the same character (ch. 19.7.2). */
static void settle_parity(crtc_t *crtc) {
  bool on_the_frame_head = crtc->c0 == 0 && crtc->c4 == 0 && crtc->c9 == 0;
  if (on_the_frame_head && !crtc->stood_on_the_frame_head) {
    crtc->parity_frame = crtc->parity_r6;
  }
  crtc->stood_on_the_frame_head = on_the_frame_head;
  if (crtc->c4 == crtc->registers[6]) {
    crtc->parity_r6 = !crtc->parity_frame;
  }
}

uint64_t crtc_tick(crtc_t *crtc) {
  enter_character(crtc);
  settle_parity(crtc);
  decide_last_line(crtc);
  begin_vertical_adjustment(crtc);
  move_video_pointer(crtc);
  authorize_vsync(crtc);
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
        /* Writing R7 changes the comparison whatever the value written, so
           it can serve again (ch. 16.3) — except that an equality made by
           hand at the head of a line is not a VSYNC but a blocked one: "the
           VSYNC is triggered immediately if it was not already in progress,
           except if this modification occurs when C0vs=0 or C0vs=1. If the
           modification of R7 with the value of C4 took place when C0vs<2,
           we are in a BLOCKED VSYNC" (ch. 16.4.1.1). */
        crtc->vsync_blocked = crtc->c0 < 2 && c4_stands_on_r7(crtc);
      }
    }
  }
  return pins;
}
