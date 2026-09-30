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
#define C5_BITS 0x1F

void crtc_init(crtc_t *crtc, uint8_t type) {
  *crtc = (crtc_t){0};
  crtc->type = type;
  crtc->vsync_armed = true;
  crtc->c9_processing_managed = true;
}

/* The types an R8 write hands ParityC9 outright, which is why they cannot read it back off the row.
   A type 2 is not one of them: ch. 19.5.4 lists every parity state that chip keeps and ParityC9 is
   not among them — it names ParityFrame and ParityR6 and says only that "C9 parity is managed only
   when R8 = 3 to update C9" — so there is no state for a write to set, and the parity is the
   frame's own, which is what parity_c9 answers. A type 1 takes it either way the mode is going and
   with C4's correction (ch. 19.5.3); types 3 and 4 take it only on the way in and bare — "when R8
   changes to 1 or 3, Parityc9=C9.0", and again in ch. 19.8.4: "when R8 goes from 0 to 3 (mode IVM
   on), ParityC9 state is immediately assigned with the parity of the current C9" (ch. 19.5.5,
   19.8.4). */
static bool holds_its_c9_parity(const crtc_t *crtc) {
  return crtc->type == 1 || crtc->type == 3 || crtc->type == 4;
}

/* And whether R9 as it stands is the parity that row makes the two frames
   share, which is where the state turns over with C4: an even R9 on a type
   1, an odd one on types 3 and 4, whose R9 is programmed a type 0's way. */
static bool reverses_its_parity_on_this_r9(const crtc_t *crtc) {
  return holds_its_c9_parity(crtc) && ((crtc->registers[9] & 1) != 0) != (crtc->type == 1);
}

/* Moving C4 lifts the VSYNC block, because the comparison with R7 has
   changed; setting it to the value it already held does not (ch. 16.3). */
static void enter_character_row(crtc_t *crtc, uint8_t row) {
  uint8_t next = row & C4_BITS;
  if (next != crtc->c4) {
    crtc->vsync_blocked = false;
    /* And a line that was to begin a type 2's frame again is no longer the
       frame's first, which ch. 19.8.3 names "When C4=C9=0". Only a line of one
       character landing an armed C4 increment can move C4 under it. */
    crtc->frame_begins_again = false;
    /* "ParityC9 is reversed with each C4 increasing when R9 is peer", which
       on a type 1 is an even R9 (ch. 19.5.3); types 3 and 4 reverse it on an
       odd one — "if R9 is odd, C9's parity switches each time C4 changes"
       (ch. 19.8.4) — which is the same balancing read off the other parity,
       as their R9 is programmed a type 0's way. */
    if (reverses_its_parity_on_this_r9(crtc)) {
      crtc->parity_c9_held = !crtc->parity_c9_held;
    }
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

/* Whether this type counts the frame's additional lines on a counter of
   their own, leaving the row to go on counting itself: "on CRTCs 0, 3 and
   4, there is no specific C5 counter and C9 is used for comparison with R5.
   On CRTCs 1 and 2, there is a specific counter C5 used in conjunction with
   C9" (ch. 11.1). What the two do with C4 follows from that — "on CRTC's 1
   and 2, C4 increments regardless of the value of R4 each time C9=R9, as
   long as C5 has not reached R5", where a type 0 "is incremented only on
   the last line" and types 3 and 4 not at all. That last is not here: those
   two are given a type 0's single increment. */
static bool counts_the_adjustment_on_c5(const crtc_t *crtc) {
  return crtc->type == 1 || crtc->type == 2;
}

/* Whether this type settles the coming frame's parity ahead of the frame, on
   the row R6 names. Types 0 and 2 do (ch. 19.5.2, 19.5.4); types 1, 3 and 4
   keep no such state and turn the parity over at each frame's own head
   (ch. 19.5.3, 19.5.5). */
static bool anticipates_the_parity(const crtc_t *crtc) {
  return crtc->type == 0 || crtc->type == 2;
}

/* The line either interlace mode adds at the end of a frame is added on the
   parity R6 anticipated, on a type 0 and a type 2 (ch. 19.6.1, 19.6.3).
   Types 1, 3 and 4 anticipate nothing and ask on the frame's own parity:
   "if ParityFrame is even, then an additional line and a MID-VSYNC are
   scheduled" (ch. 19.5.3, 19.5.5), and for those three the line "does not
   depend on the C4=R6 equivalence, unlike CRTC's 0 and 2" (ch. 19.6.2,
   19.6.4). What ch. 19.6.4 asks of a type 3 or 4 besides is not here: that
   "C4 is not incremented (unlike all other CRTC's)" for that line, and that
   the line takes no parity with it — "the additional line generated does not
   consider parity states as on other CRTC's. C9 will always be 0, even if
   the other lines are odd on C4=R4". */
static bool interlace_line_asked_for(const crtc_t *crtc) {
  return interlace_asked(crtc) &&
         (anticipates_the_parity(crtc) ? crtc->parity_r6 : !crtc->parity_frame);
}

/* The interlace video mode as R8 holds it, which is not always as the
   counters have it: R8's two low bits both set ask for it, and the chip
   takes it up at the head of the next line (ch. 19.1, 19.8.1). */
static bool interlace_video_asked(const crtc_t *crtc) {
  return interlace_asked(crtc) && (crtc->registers[8] & 2) != 0;
}

/* ParityC9, which fills bit 0 of the raster address in the interlace video
   mode: "ParityC9 = C4.0 xor ParityFrame" (ch. 19.5.2). Where R9 is odd the
   rows come out alternately even-lined and odd-lined as C4 advances on a
   type 0, which
   is how a pair of them keeps the same length on both frames; where R9 is
   even every row takes the frame's own parity. The Compendium holds this in
   a state it updates at a row's end and only while R9 is odd, which leaves
   it stale where R9 is even; read from C4 it says the same thing, except
   where R9's own parity is changed inside a line — the stored state would
   carry the old parity to the row's end, and this one moves the raster
   address under the line being drawn, which is the very thing ch. 19.8.1
   gives the delayed take-up to prevent. Nothing we can run grades it. */
static bool parity_c9(const crtc_t *crtc) {
  if (holds_its_c9_parity(crtc)) {
    return crtc->parity_c9_held;
  }
  /* A type 2 takes the frame's parity and nothing besides. Its "C9
     management has been carried out in a simple way, not without introducing
     some constraints (R9 odd for example)", and so "parity is respected
     whatever the values of R9 and C4", where "on the other CRTCs, R9 defines
     a total number of lines to share between 2 frames, which is a problem
     when this number of lines is odd, and requires some adjustments in order
     to balance the lines between 2 frames" (ch. 19.5.4). Its own tables show
     the difference: ch. 19.8.3's switching diagrams give an even frame's C4=1
     row the same eight addresses as its C4=0 row, where the balancing below
     would alternate them. */
  if (crtc->type == 2) {
    return crtc->parity_frame;
  }
  /* A type 0 takes it from the row instead: where a row is an odd number of
     lines the two frames must share them, so an odd C4 runs on the
     parity opposite the frame's. The three above never reach here — a
     type 1 reads the same rule off an even R9 rather than an odd one,
     which the reversal above carries, and all three keep the state
     instead because a write can set it against the row. */
  bool odd_lined_rows = (crtc->registers[9] & 1) != 0;
  if (odd_lined_rows && (crtc->c4 & 1) != 0) {
    return !crtc->parity_frame;
  }
  return crtc->parity_frame;
}

/* C9.VMA, the raster address (ch. 19.8.1). It is what leaves the chip on
   RA and what the late VSYNC of an odd row is timed by, and on every type
   but one it is also what R9 is measured against. In the interlace video
   mode a row covers two rows' worth of memory and the two frames take
   alternate lines of it, while the counter itself goes on counting by one —
   which is the half of this the Compendium's own tables get wrong. The shift
   carries out of five bits rather than widening, so a row entered off its
   parity comes round to its limit instead of missing it: for an R9 of 6, at
   C9=19, which is a comparison a type 2 does not make. */
static uint8_t c9_vma(const crtc_t *crtc) {
  if (!crtc->interlace_video_mode) {
    return crtc->c9;
  }
  /* "Another counter, C9.IVM, is used for displaying and managing video
     pointer updating ... C9.VMA=(C9.IVM*2) or Parity" (ch. 19.8.3), and it is
     a type 2's alone. */
  unsigned count = crtc->type == 2 ? crtc->c9_ivm : crtc->c9;
  return (uint8_t)(((count << 1) | (parity_c9(crtc) ? 1u : 0u)) & C9_BITS);
}

/* The line the count goes on from, where the doubling is about to start or
   stop under it. Ch. 19.8.2 gives a type 1 one counter and it is the
   address, and ch. 19.8.4 gives types 3 and 4 the same one — "C9 = C9+2",
   then "C9 = C9 or ParityC9": in the mode it steps by two with ParityC9 in the bit it leaves,
   and "as soon as R8 returns to 0, the counting logic normally resumes" —
   from the line the address had reached. This chip keeps that as a count
   and a parity, ch. 19.8.1's arrangement, so the two have to be handed back
   to each other wherever a mode is taken up or given up in the middle of a
   row: doubled where the doubling stops, halved where it starts, which is
   also where the bit ch. 19.5.3 has the write settle arrives when the write
   was too early to see it. At a row's head both are zero and neither moves,
   which is every frame that asks for the mode and keeps it. The frame's
   padding is measured against whatever this hands back, so an edge inside
   R5's lines reconciles that comparison too — and the count it is measured
   against while the mode stands is the count and not the address, where ch.
   11.3.3 asks for "the number of the next additional line (C9+1)" with C9
   the address. That reading is older than this work and is not here. */
static uint8_t c9_the_count_goes_on_from(const crtc_t *crtc) {
  bool asked = interlace_video_asked(crtc);
  if (!holds_its_c9_parity(crtc) || crtc->interlace_video_mode == asked) {
    return crtc->c9;
  }
  return asked ? (uint8_t)(crtc->c9 >> 1) : c9_vma(crtc);
}

/* R9 read to the nearest line of ParityC9's own parity — up on a type 0
   and on types 3 and 4, whose rows end where the address reaches or passes
   R9 ("If C9 >= R9", ch. 19.8.4), which is the first line of the row's own
   parity at R9 or above; down on a type 1 below — which is the limit a row
   ends on while the
   raster address carries parity in bit 0. On a type 0: The
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
  unsigned off_by_one = (r9 ^ (parity_c9(crtc) ? 1u : 0u)) & 1u;
  /* A type 1 reads it down to that parity where a type 0 reads it up. Ch.
     19.8.2 gives that type's counting outright and ends a row on "(C9 and
     %11110) == (R9 and %11110) (test C9/R9 excluding parity)", which is the
     limit read down; ch. 19.4.2 says the same from the programmer's side,
     R9 wanting "the value N-1" for a character of N lines where ch. 19.4.1
     asks a type 0 for "value N-2". It is why the two want R9 "programmed
     respectively with 6 and 7" for the same four lines (ch. 28.1.7).
     Reading down carries out of five bits at an R9 of 0, where ch. 19.8.2's
     own counting also runs a row of sixteen, as reading up does at 31. */
  if (crtc->type == 1) {
    return (uint8_t)((r9 - off_by_one) & C9_BITS);
  }
  return (uint8_t)((r9 + off_by_one) & C9_BITS);
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
   parity is one no address of that row could ever meet. All of this is the
   four types that halve a row; the first lines below are the fifth. */
static bool row_is_on_its_last_scanline(const crtc_t *crtc) {
  /* A type 2 measures the count and not the address: "in 'Interlace' mode, C9
     is compared with R9 in a conventional way to process C4" (ch. 19.8.3), so
     its row is R9+1 lines whatever the mode — "when R9 = 7, we have C4
     characters of 8 lines for each frame" (ch. 19.4.3, a chapter about this
     type). The other four ask their programmer for half of it instead, R9
     holding a pair of frames' lines rather than one frame's: "value N-2" on a
     type 0 and on types 3 and 4, which "shares this feature with CRTC 0", and
     "the value N-1 in the 2 interlace modes" on a type 1 (ch. 19.4.1, 19.4.2,
     19.4.4). */
  if (crtc->type == 2) {
    return crtc->c9 == crtc->registers[9];
  }
  /* The two ASICs end a row wherever the address has reached or passed R9 —
     ch. 19.8.4's "C9 >= R9" — and a counter sent beyond it comes home rather
     than walking the five bits round: "if R9 is changed with a value less than
     or equal to C9, then C9 changes to 0 on the next line, and C4 goes to 0
     (if C4=R4) otherwise C4=C4+1 ... it is impossible to 'overflow' C9 on
     these CRTC's" (ch. 10.3.4.1), which ch. 11.3.3 says of R5 and R9 both.
     The register is compared bare because the address carries ParityC9 in its
     low bit: the first address at or past R9 is R9 read up to that parity, so
     the comparison finds the parity limit without being given it. */
  if (crtc->type == 3 || crtc->type == 4) {
    return c9_vma(crtc) >= crtc->registers[9];
  }
  /* The other three end a row on an equality, which is their own counting: ch.
     10.3.1.1 gives a type 0 the walk outright, "it will count to its maximum
     value (31) before looping back to 0". The limit keeps the parity only for
     as long as the register asks for the mode — read against the bare register
     the line a mode is left on misses its limit by one — and a type 0's limit
     loses that parity the moment the register does, which is the counting its
     own chapter describes. A type 1's row ends on ch. 19.8.2's comparison with
     the parity left out. */
  bool parity_in_the_limit = interlace_video_asked(crtc);
  return c9_vma(crtc) == (parity_in_the_limit ? r9_with_parity(crtc) : crtc->registers[9]);
}

/* C9.IVM goes on, which it does on every line of every row: "if (C9==R9/2)
   then C9.IVM=0 ... else C9.IVM=C9.IVM+1", and a row's own end zeroes it
   beside C9 — "C9=0 ; Management of C4 (C4++ or C4=0) ; C9.IVM=0". "This
   management of C9.IVM takes place all the time, including when the IVM mode
   is not activated" (ch. 19.8.3). The chapter gives the reset twice and not
   in the same terms: the algorithm turns it on C9 reaching R9/2, ch. 19.4.3
   on the counter itself reaching "the value of R9 'outside parity'". The two
   name the same line of every row whose head has zeroed the counter, and part
   only on a row entered out of phase with it, which is nowhere here. The
   algorithm is what is taken. */
static void c9_ivm_goes_on(crtc_t *crtc) {
  crtc->c9_ivm =
      crtc->c9 == (crtc->registers[9] >> 1) ? 0 : (uint8_t)((crtc->c9_ivm + 1) & C9_BITS);
}

/* A frame begins where the last one is done with, whatever the counters
   read on the way: C4 of 127 carries the interlace line itself to a C0, C4
   and C9 all zero, and a frame that read its own head off those would renew
   the line under the line it had just given and never end. One interlace
   line to a frame (ch. 11.9), and the frame it was given to is what spends
   it, not the adjustment that carried it. */
static void begin_frame(crtc_t *crtc) {
  crtc->vertical_adjustment_in_progress = false;
  crtc->adjustment_opened_at_c4_of_zero = false;
  crtc->r4_moved_at_a_lines_end = false;
  crtc->adjustment_on_its_last_line = false;
  crtc->c9 = 0;
  /* A frame ends on the branch that zeroes both, "C9=0 ; Management of C4
     (C4++ or C4=0) ; C9.IVM=0" (ch. 19.8.3), and a counter carried into a row
     would show it the four addresses of a row it is not on. */
  crtc->c9_ivm = 0;
  crtc->c5 = 0;
  crtc->interlace_line_given = false;
  crtc->frames_counted++;
  enter_character_row(crtc, 0);
}

/* "CRTC 1 activates an internal additional management state if R5>0 when C4
   should return to 0 at the end of the frame (C4=R4, C9=R9)" (ch. 11.3.2).
   The parenthesis is read here rather than assumed from the run, because the
   chip reaches this rule at characters where it does not hold: a run opened
   because an R4 or R9 write unmade the last line under it is a frame
   postponed rather than ended, and one the R5 window admits with C4 already
   past R4 was never going to return C4 to 0 either. It also holds inside a
   run already standing, where C4 comes back round to R4. Ch. 11.3.1 is
   headed "CRTC's 0, 2" and gives those two the plain overflow and no state
   at all, which is why only a type 1 takes anything. */
static bool takes_the_r5_state(const crtc_t *crtc) {
  return crtc->type == 1 && crtc->registers[5] != 0 && crtc->c4 == crtc->registers[4] &&
         row_is_on_its_last_scanline(crtc);
}

/* A run of additional lines opening. Taking the state here rather than
   reading it line by line is what keeps a frame from being decided by the R5
   of the frame before it. */
static void open_the_adjustment(crtc_t *crtc) {
  if (crtc->vertical_adjustment_in_progress) {
    return;
  }
  crtc->vertical_adjustment_in_progress = true;
  crtc->r5_opened_the_run = takes_the_r5_state(crtc);
  /* "However, if R4 was modified to C0==R0 with R4>0, then VMA is not
     updated with R12/R13 when C4=1" (ch. 11.2.4), read from the last R4 the
     frame was given: a later write anywhere takes an earlier one's answer
     away, which is wider than the chapter's own sentence and what nothing
     here tells apart, since an R4 above 0 left standing from an earlier line
     moves the row the run opens on instead. */
  crtc->adjustment_opened_at_c4_of_zero = crtc->c4 == 0 && !crtc->r4_moved_at_a_lines_end;
}

/* How many lines a VSYNC lasts. "This number of lines can be programmed on
   CRTC's 0, 3 and 4 (via register R3h). It is fixed at 16 for CRTC's 1 and
   2 (and for CRTC's 0, 3, 4 when R3h=0)" (ch. 16). C3h counts on four bits,
   so a limit of 0 is the same comparison as a limit of 16, and the two
   types that cannot be programmed simply never read R3h. */
static uint8_t vsync_lines(const crtc_t *crtc) {
  if (crtc->type == 1 || crtc->type == 2) {
    return 0;
  }
  return (uint8_t)(crtc->registers[3] >> 4);
}

/* Whether this type decides the frame's end, and the padding after it, where
   the line ends rather than at its head. The two ASICs do: "the modification
   of register 4 is considered immediately at the end of the line", so that
   "if R4 is updated with a value less than C4, then there is overflow of the
   C4 counter" (ch. 12.5), and an R9 written at or under C9 on a frame's last
   row ends the frame on the next line, ch. 10.3.4.1's table giving the next
   line 0 in both C9 and C4 whatever C9 the write found. The padding follows
   from the same moment: "R5 management is considered on each C0 position" on
   types 1 to 4 (ch. 11.4.1). Type 0 decides both while C0 is 0 or 1 and holds
   them, turns a last line unmade at C0=1 into an adjustment, and gives R5 a
   deadline of its own (ch. 10.3.1.2, 12.2, 13.2) — its own chapters'
   exceptions, which the ASICs' do not share. Types 1 and 2 are left a type
   0's, ch. 11.4.1 notwithstanding. */
static bool takes_the_frame_end_where_the_line_ends(const crtc_t *crtc) {
  return crtc->type == 3 || crtc->type == 4;
}

/* A line that never reaches C0=1 leaves C9's management disabled, and then
   "all of the CRTC counters are frozen as long as R0=0" (ch. 13.2.1) — the
   VSYNC's line counter with them, which is why a VSYNC begun there "is not
   deactivated if R3h was worth 1" (ch. 16.4.1.2). One thing still lands:
   the C4 increment the last managed boundary armed, and once only, because
   "this increment is deactivated because it has taken place" (ch. 13.2.4) —
   and where it falls on a last line an adjustment begins with it (ch.
   13.2.6). In a line that does reach C0=1 the arming and the landing fall on the
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
      /* On a last line that increment is the additional management
         beginning, not a row advance: "C4 is incremented (i.e. 1). We are on
         additional management", and it "will remain so when C0 can once
         again exceed 1" (ch. 13.2.6). The chapter's condition is C9=R9 and
         C4=R4, which is the last line itself — an arm the R5 window admitted
         with C4 already past R4 is the neighbouring case, "C4's last
         hiccup", which that page gives the increment and no management.
         Saying so is what keeps the C0<2 assessment later in this tick from
         taking the arming back, C4 having just moved off R4 and the last
         line with it. */
      if (crtc->last_line) {
        open_the_adjustment(crtc);
      }
      enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
    }
    /* The one register the freeze does not shut out: "updates to registers
       R4, R5 and R9 are no longer considered as long as R0=0. On the other
       hand, R8 continues to be considered each time C0=0" (ch. 13.2.1). A
       frozen chip's address stands where it stood, so the count and the
       parity are handed back to each other here as they are at a running
       line's end, or the doubling would move an address the freeze is
       holding still. C9.IVM stands with them, a counter going on under a
       frozen line being another way to move that address. */
    crtc->c9 = c9_the_count_goes_on_from(crtc);
    crtc->interlace_video_mode = interlace_video_asked(crtc);
    return;
  }
  /* C3h counts VSYNC scanlines on its 4 bits, so a width of 0 runs the full
     16 (ch. 6.1.2), which is every VSYNC on the two types that cannot
     program one. */
  if (crtc->vsync_began_mid_line) {
    /* A VSYNC that began away from the head of a line has its counter
       initialized at the next C0=0 rather than advanced there, which leaves
       the pulse longer than R3's high nibble by the rest of the line it
       began in: one begun during line 1 of 16 ends at the end of line 17
       (ch. 16.4.1). Types 1 and 2 initialize it with 1 instead and spend a
       line fewer, "the 'triggered' activation of the VSYNC count(ing) the
       line as if the VSYNC had started when C0=0" (ch. 16.4.2, 16.4.3;
       ch. 28.1.3 says the same of a type 1 alone, being a chapter about
       telling the types apart rather than about all of them). Both chapters
       scope it to a VSYNC begun because R7 was written to meet C4, which
       the half line an even interlaced frame begins on is not: one of those
       "starts when R7 was programmed before C4=R7" and so "lasts 16 lines"
       (ch. 28.1.4). */
    bool counts_from_one =
        (crtc->type == 1 || crtc->type == 2) && !crtc->vsync_began_on_its_half_line;
    crtc->c3h = counts_from_one ? 1 : 0;
    crtc->vsync_began_mid_line = false;
  } else if (crtc->vsync) {
    crtc->c3h = (crtc->c3h + 1) & 0x0F;
    if (crtc->c3h == vsync_lines(crtc)) {
      crtc->vsync = false;
    }
  }

  if (takes_the_frame_end_where_the_line_ends(crtc) && !crtc->vertical_adjustment_in_progress) {
    bool row_ends = row_is_on_its_last_scanline(crtc);
    crtc->last_line = crtc->c4 == r[4] && row_ends;
    /* The padding the head of the line armed by default is asked again of R5
       as it now stands, and of the interlace line as C0=R0 answered it, which
       is where ch. 11.9 puts that question on every type. R5 admitting a row
       whose C4 has gone past R4 is a type 0's rule (ch. 11.2.2), carried
       over, the ASICs' chapters saying nothing of it. */
    crtc->vertical_adjustment_armed = row_ends && ((crtc->last_line && crtc->interlace_line_owed) ||
                                                   (r[5] != 0 && crtc->c4 >= r[4]));
  }
  /* The first line a type 2 turned into an additional line ends as that line
     ends a frame, and a new line 0 follows it. An adjustment armed on that
     same line comes first, which is our ordering and nothing grades. */
  bool frame_begins_again = crtc->frame_begins_again;
  crtc->frame_begins_again = false;
  if (crtc->vertical_adjustment_armed && counts_the_adjustment_on_c5(crtc)) {
    /* The row keeps its own count through all of it — "C9 is zeroed when
       C9=R9 and C4 is incremented" (ch. 11.2.3) — and C5 alone counts the
       lines, the run ending "when the number of the next additional line
       (C5+1 on CRTC 2, C9+1 on CRTC 0) reaches R5" (ch. 11.3.1, 11.3.2).
       The first line is entered before it is counted, ch. 11.2.3's table
       opening at C5=0 on it. Interlace asks for its own "after the R5 lines
       if necessary" (ch. 19.6.2, 19.6.3), which makes it the last of them. */
    uint8_t next_c5 =
        crtc->vertical_adjustment_in_progress ? (uint8_t)((crtc->c5 + 1) & C5_BITS) : 0;
    open_the_adjustment(crtc);
    /* The state a type 1 latches is not cleared by taking R5 back to 0:
       "the state is not deactivated, C4 does not return to 0 and C5 loops".
       Only a later R5 closes it — "if C5+1 reaches an R5>0, then the
       additional management changes C4 to 0 before deactivating its state"
       (ch. 11.3.2) — so a program can hold a frame open as long as it likes
       and close it on a line of its own choosing, which is what that
       chapter offers it.

       The hold is not endless, because "C4, however, continues to be
       compared to R4 to process the change from C4 to 0": C4 climbs the
       seven bits it has and comes back round to R4, which is where the
       state is read again. */
    bool r5_taken_to_zero_mid_run = crtc->r5_opened_the_run && r[5] == 0;
    bool c4_came_round_to_r4 =
        r5_taken_to_zero_mid_run && crtc->c4 == r[4] && row_is_on_its_last_scanline(crtc);
    bool r5_lines_spent = next_c5 == r[5] && !r5_taken_to_zero_mid_run;
    bool interlace_line_falls_here =
        r5_lines_spent && crtc->interlace_line_owed && !crtc->interlace_line_given;
    if (crtc->interlace_line_given || (r5_lines_spent && !interlace_line_falls_here)) {
      crtc->vertical_adjustment_armed = false;
      begin_frame(crtc);
    } else {
      crtc->interlace_line_given = crtc->interlace_line_given || interlace_line_falls_here;
      crtc->c5 = next_c5;
      if (c4_came_round_to_r4) {
        /* "The additional management, however, remains activated": the run
           does not end on that comparison, the state is only taken afresh
           on it — and the R5 the program cancelled is the R5 it reads, so
           the state is not taken again. C5 goes on counting from where it
           stood, and the run ends where it wraps round to the 0 R5 now
           holds, which is ch. 11.3.1's overflow arriving late. */
        crtc->r5_opened_the_run = takes_the_r5_state(crtc);
        crtc->c9 = 0;
        crtc->c9_ivm = 0;
        enter_character_row(crtc, 0);
      } else if (row_is_on_its_last_scanline(crtc)) {
        crtc->c9 = 0;
        crtc->c9_ivm = 0;
        enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
      } else {
        c9_ivm_goes_on(crtc);
        crtc->c9 = (uint8_t)((c9_the_count_goes_on_from(crtc) + 1) & C9_BITS);
      }
    }
  } else if (crtc->vertical_adjustment_armed) {
    /* This is the counting of the three that keep no C5 (ch. 11.1), which
       since ch. 11.3.3 entered below is no longer one counting for all three.
       R5 is a quantity of lines, and C9 is compared with R9 before its
       increment: the line the chip would move to becomes the adjustment's
       own unless that line has reached R5 (ch. 13.2.4). Once C4 has left R4
       behind, C9 can no longer be zeroed, which is what lets it climb past
       R9 to reach an R5 larger than a row (ch. 11.2.2). "If C9==R9 then
       C4=C4+1, and in additional management only once if C4 was worth R4"
       (ch. 13.2.4) — so both the zeroing and the increment ride on the row
       having reached its last line, and C4 returns to 0 when the adjustment
       is done whatever R4 holds by then. */
    bool row_ended_on_r4 = row_is_on_its_last_scanline(crtc) && crtc->c4 == r[4];
    uint8_t next_c9 =
        row_ended_on_r4 ? 0 : (uint8_t)((c9_the_count_goes_on_from(crtc) + 1) & C9_BITS);
    /* The two ASICs end the run wherever the count has reached or passed R5,
       which is the comparison their row's own limit takes: "if R5 is modified
       with a value below C9+1, then the line is considered the last and
       additional management ends", and "whether with R5 or R9, it is
       impossible to overflow C9" (ch. 11.3.3). The other three are left the
       equality and spend the counter's whole round getting back to it. What
       keeps this off an ordinary frame is the zeroing above: a frame's own
       last line hands this a count of 0, which no R5 above it can meet. */
    bool ends_on_the_count_reached = crtc->type == 3 || crtc->type == 4;
    bool r5_lines_spent = ends_on_the_count_reached ? next_c9 >= r[5] : next_c9 == r[5];
    /* An adjustment a narrow line brought is entered before it is measured.
       Ch. 11.2.2 lists the ways one comes about with R5 at 0 — an R4 or R9
       moved at C0=1, "or if C0 can never reach 2 because R0 < 2" — and ch.
       13.2.1 and ch. 13.2.5, both of them inside their own R0=1 case, give
       that one a line: it lasts "1 line of 2 usec before ceasing (C4+1,
       C9=0)", and only "on the next line" does "the end of additional
       management reset C4 and C9 to 0". Wider lines measure first, which is
       ch. 13.2.4's reminder and what Shaker's graded E (1) holds us to — it
       times an R5 cancelled on a 64-character last line, and a chip that
       gave a line there would answer four of its seven a line too long. */
    bool entered_by_a_narrow_line = r[0] < 2 && r[5] == 0 && !crtc->vertical_adjustment_in_progress;
    if (r5_lines_spent && crtc->interlace_line_owed && !crtc->interlace_line_given) {
      /* The R5 lines are spent and interlace asks for one more, which is
         the last of them (ch. 19.6.1). C4 has already been incremented once
         for all the additional lines there are. */
      crtc->interlace_line_given = true;
      open_the_adjustment(crtc);
      crtc->c9 = next_c9;
      if (row_ended_on_r4) {
        enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
      }
    } else if ((r5_lines_spent && !entered_by_a_narrow_line) || crtc->adjustment_on_its_last_line ||
               crtc->interlace_line_given) {
      crtc->vertical_adjustment_armed = false;
      begin_frame(crtc);
    } else {
      /* And the ceasing after one line is the R0=1 case's alone. A line of
         one character keeps its run instead: "it will remain so when C0 can
         once again exceed 1. It is then R5 which controls the end ... To
         stop this management, program R5 with C9+1" (ch. 13.2.6). */
      crtc->adjustment_on_its_last_line = r5_lines_spent && r[0] == 1;
      open_the_adjustment(crtc);
      crtc->c9 = next_c9;
      if (row_ended_on_r4) {
        enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
      }
    }
  } else if (crtc->last_line || frame_begins_again) {
    begin_frame(crtc);
  } else if (row_is_on_its_last_scanline(crtc)) {
    crtc->c9 = 0;
    crtc->c9_ivm = 0;
    enter_character_row(crtc, (uint8_t)(crtc->c4 + 1));
  } else {
    c9_ivm_goes_on(crtc);
    crtc->c9 = (uint8_t)((c9_the_count_goes_on_from(crtc) + 1) & C9_BITS);
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
      row_is_on_its_last_scanline(crtc) &&
      (!crtc->vertical_adjustment_armed || crtc->c4 == r[4] || counts_the_adjustment_on_c5(crtc));
  crtc->c9_processing_managed = false;
}

/* Drawing a character advances the address and any HSYNC riding on it, and
   the latch that says the sync ended there is this character's alone. */
static void pass_a_character(crtc_t *crtc) {
  crtc->vma = (crtc->vma + 1) & 0x3FFF;
  crtc->hsync_ended_here = false;
  if (crtc->hsync) {
    crtc->c3l = (crtc->c3l + 1) & 0x0F;
    if (crtc->c3l == (crtc->registers[3] & 0x0F)) {
      crtc->hsync = false;
      crtc->hsync_ended_here = true;
    }
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
  /* The first room holds what the ending stood on for the character it was
     taken on, and on a type 1 for the character after that as well. Shaker's
     B (6) is what sets both widths: its "4TH uSec ON C0=0" wants the first of
     every type and its "5TH uSec ON C0=0" the second of a type 1. */
  bool a_line_end_enters_its_second_character =
      crtc->a_line_end_is_kept && crtc->type == 1 && !crtc->a_character_was_drawn_since;
  crtc->the_line_end_before_is_kept = false;
  if (a_line_end_enters_its_second_character) {
    crtc->a_character_was_drawn_since = true;
  } else {
    crtc->a_line_end_is_kept = false;
    crtc->a_character_was_drawn_since = false;
  }
  if (!crtc->has_drawn_a_character) {
    crtc->has_drawn_a_character = true;
    return;
  }
  const uint8_t *r = crtc->registers;
  pass_a_character(crtc);
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
  /* The chip has not quite finished deciding: a write landing on this same
     character clock is still in time to move R0 under the comparison that
     has just been made, so what the chip stood on is kept where it can be
     taken back. Ch. 13.6 draws every type a placement where "Update of R0 ok
     (just in time)" leaves the line running on and the next one where it is
     "not considered (too late)"; which of our microseconds those two land in
     is a question this chip cannot yet answer, its placements measuring one
     late against all three chronograms. The window is what carries that
     microsecond, and crtc.h says what it leaves unsettled. */
  if (crtc->line_end_rooms != 0) {
    /* A line of one character ends again on the second character of a
       type 1's window, and the end before stays within that type's reach:
       ch. 13.6.2 draws its OUTI on the "Previous R0=0" line going on a
       character further than ch. 13.6.1 draws a type 0's or 2's, at both
       placements it gives. */
    if (a_line_end_enters_its_second_character) {
      crtc->line_end_rooms[1] = crtc->line_end_rooms[0];
      crtc->the_line_end_before_is_kept = true;
    }
    crtc->line_end_rooms[0] = *crtc;
    /* One latch in the copy belongs to the character rather than to the
       line: this tick is about to spend the R3 write it records, and a
       line's end is not an R3 write, so the copy carries none back. */
    crtc->line_end_rooms[0].r3_written_for_this_character = false;
    crtc->a_line_end_is_kept = true;
    crtc->a_character_was_drawn_since = false;
  }
  crtc->c0 = 0;
  crtc->c0_reached_r0 = true;
  enter_scanline(crtc);
}

/* Decided while C0 is 0 or 1, and this type "no longer repeats this test on
   the other values of C0>1" (ch. 12.2, 10.3.1.2). A write still reaches that
   window from outside it: a write is in force on the character after the one
   it was made on (ch. 13.2.1), so one made while C0 names 1 is read while it
   names 2. Reading the chapter's window at the character a write lands on,
   where the chapter counts the character it was made on, is ours.

   What such a write does there the chapter gives both ways. It can make the
   comparison hold — "It is therefore not necessary to anticipate the
   programming of R4 (or R9) on the current line for the last line condition
   to be true on the following line. It is possible to modify R4 or R9 on the
   current line as long as C0<2 to validate the 'Last Line' state (and thus
   validate the reset of C4 on the following line)" — and it can break one,
   which "activates the vertical adjustment ... and the current line becomes
   the 'first' additional line": that is the adjustment below rather than a
   state taken back here, so only the making is read at C0=2. Later than that
   it reaches neither: "If R4 and/or R9 are modified mid-line when C0 > 1
   while the last line state is true (and there are no additional lines via
   vertical adjustment), this does not change C4 and C9, which will remain at
   0" (ch. 12.2). A comparison standing again on an R4 or R9 update is type
   2's rule (ch. 12.4.1).

   R8 reaches this comparison as it reaches the converse below, by moving
   the parity the row's limit is read up to, and the chapter names only R4
   and R9. Whether the chip lets it is the same open question that comment
   puts, answered the same way and graded by nothing. */
static void decide_last_line(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 < 2 || (crtc->c0 == 2 && !crtc->last_line)) {
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
  if (crtc->c0 == 2 && crtc->last_line && !takes_the_frame_end_where_the_line_ends(crtc) &&
      (crtc->c4 != r[4] || !row_is_on_its_last_scanline(crtc))) {
    /* "The current line becomes the 'first' adjustment line" (ch. 10.3.1.2),
       and a line already begun is past the disarm below (ch. 13.2.6). The
       arming itself was done on the characters C0 named 0 and 1. */
    open_the_adjustment(crtc);
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
     group at this, and every line it grades in words on the type 0 and type
     2 records agrees, but none of them has been tied to the character the
     deadline names: it stands on the chapter's own sentence and on our
     tests. */
  if (crtc->c0 == r[0]) {
    crtc->interlace_line_owed = interlace_line_asked_for(crtc);
    if (crtc->interlace_line_owed && crtc->last_line) {
      crtc->vertical_adjustment_armed = true;
    }
  }
}

/* A run of additional lines opened while C4 stood at 0 carries a type 1's
   offset into the C4 of 1 the run itself gives it: "if C4=0 before the
   additional management, then VMA is updated with R12/R13 and not VMA', and
   this as long as C4=1 (new value of C4 in additional management) ... it is
   then possible to modify the offset on each line C9 of C4=1 as one would do
   when C4=0" (ch. 11.2.4). Ch. 17.4.2 says the same of the same lines: "if
   C4 is 0 and additional row management begins (because R4=0 and R5>0), then
   C4 will be 1 for the first additional rows ... it is possible to modify the
   offset (R12/R13) on each line of this C4, but not of the following ones".
   The paragraph's own exception is taken at the open above. What is not here
   is the RFD the sentence after it points at (ch. 11.6), where the same
   taking outlives C4 altogether.

   Between the two quoted halves stands "in other words, the management of R1
   for the update of the video pointer no longer takes place", which ch. 11's
   other chapters use for the capture of VMA' at C0=R1 rather than for this
   load. It is read here as the reason the carry ends rather than as a rule
   of its own: ch. 11.6 has that update status put out by the C9=R9 test made
   at C0=R1, which is the line C4 leaves 1 behind on. The capture still
   happens, and a chip that withheld it would pass everything here. */
static bool offset_carries_into_the_adjustment(const crtc_t *crtc) {
  return crtc->type == 1 && crtc->vertical_adjustment_in_progress &&
         crtc->adjustment_opened_at_c4_of_zero && crtc->c4 == 1;
}

/* The scanline VMA' takes VMA on: a row's last, except that a type 2 in the
   interlace video mode leaves the pointer wherever its display counter is
   about to be zeroed instead. "The specific management of assignment of VMA'
   with VMA when C0==R1 is only processed when R8 is equal to 3. When R8 is
   equal to 0 or 2, this assignment takes place only when C9==R9" (ch.
   19.8.3). The line it is processed on is the counter's own: ch. 19.4.3 has
   that counter "initialized ... when it reaches the value of R9 'outside
   parity', in order to update the VMA video pointer without C4 being
   incremented", and the update "no longer takes place when C4 is inc[remented]
   because it takes place only when C9.IVM = R9 (excluding parity)". An odd R9
   meets that twice to a row, at the halfway line and again at the row's end,
   which is the "update of video pointer every 4 lines" an R9 of 7 is described
   by; an even R9 meets it once, the row ending a line before the counter comes
   round, and ch. 19.8.3 says what that costs: "If R9 is even, C9 reaches R9
   before C9.IVM reaches R9 (out of parity) and the VMA' video pointer is not
   transferred into VMA".

   The algorithm alone answers neither R9. It puts VMA'=VMA inside its
   "If (C9==R9/2)" branch and names no transfer at the row's end, which gives an
   odd R9 one update where the chapter's prose asks for two, and — kept beside
   the ordinary row-end capture — gives an even R9 two where that prose says the
   row's end loses it. The counter's own coming-round is the one reading that
   satisfies both. */
static bool the_pointer_is_left_on_this_scanline(const crtc_t *crtc) {
  if (crtc->type == 2 && crtc->interlace_video_mode) {
    return crtc->c9_ivm == (crtc->registers[9] >> 1);
  }
  /* The two ASICs leave the pointer once before a frame's padding and on none
     of the padding itself: "the video pointer is updated before the start of
     the additional lines (VMA'=VMA) when C0=R1. Additional management just
     set's C9 to 0 and compare's C9 with R5 to deactivate this management,
     without updating the video pointer" (ch. 11.2.6). A type 0 does capture
     through its padding, which is ch. 11.2.2's own table walking its pointer a
     row's worth at each of those lines, and the predicate below is that
     chapter's. Their row ending wherever C9 has reached or passed R9 is what
     makes the two part: every padding line meets that, and would leave the
     pointer a row further on for each one of them. */
  if ((crtc->type == 3 || crtc->type == 4) && crtc->vertical_adjustment_in_progress) {
    return false;
  }
  return row_is_on_its_last_scanline(crtc);
}

/* VMA reloads from the VMA' latch where a scanline begins, and on the
   frame's first character both take R12/R13 — type 0 reloads when C4, C9
   and C0 stand at zero (ch. 20.3.1), where a type 1 reloads VMA alone and
   goes on doing it all through the row C4 spends at 0 (ch. 20.3.2, below). VMA' then captures VMA
   where C0 reaches R1 on the scanline the predicate above names, so the next row starts R1
   characters further on (ch. 20.3.3). */
static void move_video_pointer(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  uint16_t offset = (uint16_t)(((r[12] << 8) | r[13]) & 0x3FFF);
  if (crtc->c0 == 0) {
    if (crtc->type != 1 && crtc->c4 == 0 && crtc->c9 == 0) {
      crtc->vma_ = offset;
    }
    crtc->vma = crtc->vma_;
    /* A type 1 takes the offset into the pointer itself — "the VMA pointer
       (and not VMA' as on CRTC 0) is updated using the content of R12/R13"
       (ch. 17.4.2) — and takes it on every line of the frame's first
       character row rather than on that row's first line alone: "CRTC 1 then
       loads VMA with R12/R13 as long as C4=0 and each time C0 returns to 0,
       regardless of the value of C9" (ch. 20.3.2). The chapter names what it
       costs a program written for another chip: the vegetation in Domark's
       "007 The Living Daylights" goes because the address of the score is
       set while C4 is still 0 and lands on the scenery.

       VMA' is left where it was, which is what tells the two chips apart
       where C0 can never reach R1: a type 0 reloads both and repeats one
       line down the frame, and this one draws its first line from R12/R13
       and the rest from a VMA' "frozen on the last known pointer" (ch.
       17.4.2, 11.6). C4 standing at 0 is this chip's plainest case and not
       its rule: ch. 11.2.4 keeps the same taking through the C4 of 1 that a
       run of additional lines gives it, which the predicate above carries,
       and ch. 11.6 keeps it past any C4 at all where a border on a row's
       last line is missed, which is not here. */
    if (crtc->type == 1 && (crtc->c4 == 0 || offset_carries_into_the_adjustment(crtc))) {
      crtc->vma = offset;
    }
  }
  if (crtc->c0 == r[1] && the_pointer_is_left_on_this_scanline(crtc)) {
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

/* HSYNC begins on the character where C0 meets R2 (ch. 6.1.2). A sync asked
   for with a width of zero is none at all on types 0 and 1, and sixteen
   characters on the other three: "on CRTC's 2, 3 and 4, the HSYNC lasts
   16 µsec when R3=0", which is what a program uses to tell them apart
   (ch. 14.1, 14.6, 28.1.5), and ch. 27.6.4 and 27.6.5 draw it, the interrupt
   coming 17 and 18 microseconds after C0 reaches R2. C3l counts on four bits,
   so a width of 0 is met again after sixteen, and a zero written into a sync
   already running runs it on to sixteen on every type, as ch. 14.5 has it on
   all but a type 1; what a type 1 does instead, and ch. 14.5.4's first
   microsecond, are not here (crtc.h). VSYNC begins where C4 meets R7, which
   is why writing R7 the value C4 already holds starts one where it stands;
   the block keeps that same equality from starting a second (ch. 16.3,
   16.4.1). Each width is counted off where the counter it rides advances, so
   the ends are in enter_character and enter_scanline rather than here. */
static void begin_the_hsync(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  /* "On CRTC 0, two HSYNC's cannot be contiguous if position C0=R2 is
     encountered when C3l reaches R3l, and R3l has not been modified on this
     position" (ch. 15.3.1), which is how a line shorter than its own sync
     avoids the endless one the other types fall into: at R0=0, R2=0 and an
     R3l of 1, "on a CRTC 0, the HSYNC will not take place. It will occur on
     the 3rd C0=0" (ch. 15.3.2). A write to R3 there is the exception, and
     the sync it lets through carries the old count on: "a new HSYNC-CRTC
     begins without C3l being zeroed", which is where a R2.JIT HSYNC begins
     (ch. 15.3.3). The write is taken as the modification whatever value it
     carries: an OUT lands a whole byte on R3, and ch. 16.3 states that
     reading outright for R7, "whatever the value written". */
  bool blocked = crtc->hsync_ended_here && !crtc->r3_written_for_this_character;
  bool width_of_nothing_is_sixteen = crtc->type == 2 || crtc->type == 3 || crtc->type == 4;
  bool a_width_asked = (r[3] & 0x0F) != 0 || width_of_nothing_is_sixteen;
  if (crtc->c0 == r[2] && !crtc->hsync && !blocked && a_width_asked) {
    crtc->hsync = true;
    if (!crtc->hsync_ended_here) {
      crtc->c3l = 0;
    }
  }
}

/* Whether an equality a program makes by hand is a blocked VSYNC rather than
   a triggered one. At the head of a line it is on a type 0: "the VSYNC is
   triggered immediately if it was not already in progress, except if this
   modification occurs when C0vs=0 or C0vs=1 ... we are in a BLOCKED VSYNC"
   belongs to ch. 16.4.1.1, a type 0's chapter, and ch. 16.4.2 answers for a
   type 1 with no exception at all — "if R7 is modified with the value of C4,
   then VSYNC is triggered immediately". A type 2 has ch. 16.4.3's answer,
   "triggered immediately, except during the HSYNC period (C0=R2 to C0=R2+R3),
   which triggers the GHOST VSYNC": a pulse that counts its lines and prevents
   another, "but without the VSYNC pin being enabled". The GHOST is not here,
   and the block stands in for it inside that period, keeping the pin low and
   the equality spent. It counts none of the GHOST's lines, so a second
   equality within them — R7 written onto C4 again past the sync, or met on a
   row after — raises a pulse here where the GHOST prevents one. Shaker's
   C (P) bears the stand-in out on a line this reader does not score: of the
   six it tags "UPD R7 IN HSYNC", the one for R8=3, R9=6 and C4=#26 reads
   #0431 against silicon's #0431 with the block, and #0001 without it.

   The two ASICs keep a type 0's block for no reason in their own chapter,
   which gives them "VSYNC starts when C4=R7 and C9=C0=0 ... if R7 is modified
   with the value of C4 while C0>0 and/or C9>0, it will not trigger CRTC
   VSYNC" (ch. 16.4.4), kept in begin_the_vsync. The block tells where their
   start leaves the frame's corner — a MID-VSYNC's R0/2, or the delayed line —
   and where an R4 of 0 brings C4 back to the corner unmoved, when nothing
   lifts it and ch. 16.4.4's "renewed" condition would start a sync there.
   Nothing on the disc grades it: both records swept with it lifted come out
   the same. */
static bool blocks_an_equality_made_by_hand(const crtc_t *crtc) {
  if (crtc->type == 1) {
    return false;
  }
  if (crtc->type == 2) {
    /* The sync running and the character it ended on, which is the period
       "1 µsec longer than the visual size of the HSYNC" read off the chip's
       own state: a width of nothing run to sixteen, a sync carried past R0,
       and an R2 no counter reaches all fall where the sync does. */
    return crtc->hsync || crtc->hsync_ended_here;
  }
  return crtc->c0 < 2;
}

/* The C4/R7 equality, read on every character. Types 1 and 2 read it on the
   character a write lands on as well, where the write lands on the character
   clock (crtc_access). */
static void begin_the_vsync(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  /* The two ASICs start one only at a frame's own corner: "VSYNC starts when
     C4=R7 and C9=C0=0" (ch. 16.4.4), which ch. 19.7.1 draws as the exception
     to every other type — "VSYNC occurs when C4 is equal to R7 on any position
     of C0 (except on CRTC's 3 and 4, which dictate that C4=C9=C0=0)". A
     MID-VSYNC is the one thing that moves the character, its own chapter
     keeping the line and giving up C0: "the VSYNC will start when C0 reaches
     R0/2" (ch. 19.7.3). */
  bool starts_at_a_frames_corner = crtc->type == 3 || crtc->type == 4;
  /* On an even frame in either interlace mode the VSYNC is a MID-VSYNC:
     the C4/R7 equality does not start it where it falls, but where C0
     reaches R0/2, which is the half line the second field is raised by
     (ch. 19.7.2). Where an R7 of 0 puts the equality on the character the
     frame's parity turns on, the two ASICs read it first: "the management of
     the VSYNC has priority over the assignment of ParityFrame ... If
     ParityFrame was odd, then there will be no MID-VSYNC, and VSYNC will
     start on C4=C9=C0=0, although ParityFrame has change to Even" (ch.
     19.7.3), where the other three turn the parity first, "ParityFrame
     management takes priority over VSYNC management" (ch. 19.7.2). Their
     parity turns over at every frame's head whatever R8 holds (ch. 19.5.5),
     so the one an R7 of 0 is read under is the opposite of the one the frame
     stands on. */
  bool read_before_the_parity_turns = starts_at_a_frames_corner && r[7] == 0;
  bool parity_read = read_before_the_parity_turns ? !crtc->parity_frame : crtc->parity_frame;
  bool mid_vsync = interlace_asked(crtc) && !parity_read;
  /* And where the video mode gives a row an odd number of lines, an odd C4
     of an odd frame starts its VSYNC a line late, on the row's second line
     rather than its first: the two frames' rows are of unequal length there,
     and this is what still leaves their syncs half a line apart (ch. 19.5.2,
     19.7.1). The second line is what the chapters ask for and not the
     address 2 one of them names: ch. 19.5.2 gives a type 0 "C4=R7 and
     C9.VMA=2 on the odd C4s", where a row of that type always runs the even
     addresses and 2 is its second line, while of types 3 and 4 ch. 19.5.5
     says only that "the VSYNC is delayed by 1 line" and ch. 19.7.1 that it
     "can then be delayed by one line" — and their ParityC9 is a state a
     write can set against the row, leaving it to walk 1, 3, 5, 7 and never
     reach 2 at all. The two readings part on a type 0 only where a row is
     carried past its limit and frozen with C9 at 17, whose doubling comes
     back round to 2: a sync fifteen lines late answers neither reading, and
     no chapter draws the row that would ask for it. Read as the literal address, such a frame
     raises no VSYNC whatsoever; read as the second line, it takes one on 3, a line late, as the
     chapter asks. The delay never meets a MID-VSYNC, which happens only on an even frame
     or, on the two ASICs, on an odd one at an R7 of 0, whose C4 is even: "MID-VSYNC is not
     cumulative with this line because it cannot occur on an odd frame with an odd C4" (ch. 19.7.1).
     A row of one line has no second line and so raises no VSYNC at all, which an R9 of 31 makes of
     every even-parity row; the Compendium describes that case nowhere. */
  /* Three of the five take that delay at all: "there is also an exception
     on CRTC's 0, 3 and 4 when the line count of a C4 character is odd on an
     odd frame and an odd C4" (ch. 19.7.1), and of a type 1 ch. 19.5.3 says
     outright that "the VSYNC is not delayed from a line on odd C4s when R9
     is even". R9 even is where that type's rows come out odd, where a type
     0's do at R9 odd, which parity_c9 reads for itself. */
  bool delays_a_whole_line = crtc->type != 1 && crtc->type != 2;
  bool late_vsync = delays_a_whole_line && crtc->interlace_video_mode && (r[9] & 1) != 0 &&
                    (crtc->c4 & 1) != 0 && crtc->parity_frame;
  /* The line it starts on: a row's first, except where the exception above
     holds it back to the second. */
  bool at_the_line_it_starts_on =
      late_vsync ? crtc->c9 == 1 : !starts_at_a_frames_corner || crtc->c9 == 0;
  /* And the character: a line's first on the two ASICs, except where a
     MID-VSYNC gives up C0 for half a line instead. */
  bool at_the_character_it_starts_on =
      mid_vsync ? crtc->c0 == r[0] / 2 : !starts_at_a_frames_corner || crtc->c0 == 0;
  if (c4_stands_on_r7(crtc) && !crtc->vsync && !crtc->vsync_blocked && at_the_line_it_starts_on &&
      at_the_character_it_starts_on) {
    crtc->vsync = true;
    crtc->vsync_blocked = true;
    crtc->c3h = 0;
    crtc->vsync_began_mid_line = crtc->c0 != 0;
    crtc->vsync_began_on_its_half_line = mid_vsync;
  }
}

static void begin_syncs(crtc_t *crtc) {
  begin_the_hsync(crtc);
  begin_the_vsync(crtc);
}

/* The other character the R1 border is raised on: where C0 meets R0 having
   not met R1 all line, since "the condition C0=R1 not being
   met during the line ... the condition C0=R0 therefore replaces the
   condition C0=R1" (ch. 19.2.4). R1 standing beyond the line's end is the
   common way to miss it, but not the only one: an R1 moved behind C0 is
   never met again either, so what is read here is the latch rather than
   the registers.

   The substitution needs somewhere for the border to go. Only a delay gives
   it a character of its own; with none the chip sends the half character of
   ch. 17.6.2 early instead and this latch is left alone, and the BORDER ON
   function is no delay and gets no character either. Where the border is
   handed out is a separate question, which the skew answers. */
static bool r0_stands_in_for_r1(const crtc_t *crtc) {
  uint8_t skew = display_skew(crtc);
  bool delayed = skew == SKEW_ONE_CHARACTER || skew == SKEW_TWO_CHARACTERS;
  return delayed && !crtc->display_r1 && crtc->c0 == crtc->registers[0];
}

/* Whether an R6 of 0 on a frame's first line is a conflict for this type:
   "when C4=R6=0 and C9=0 on CRTC's 0 and 2, a conflict occurs (this conflict
   does not exist when R6>0)" (ch. 18.3.2), which is both the alternation the
   line comes out as and the deadline it can be taken back before. A type 1
   meets the same standing and settles it the other way, with a border that
   stands for the rest of the frame (ch. 18.3.3) on top of the one an R6 of 0
   gives it outright, and types 3 and 4 never meet it: "no conflict exists since the management of
   R6=0 does not exist during the line and is tested only once" (ch. 18.3.4). */
static bool takes_the_r6_conflict(const crtc_t *crtc) { return crtc->type == 0 || crtc->type == 2; }

/* And whether C4 is measured against R6 at every character of a line or only
   where the line begins: "the R6 test is done at the beginning of the line
   only. The update of R6 during the line is therefore not considered ... the
   BORDER is activated only when C0 goes to 0 when C4=R6" (ch. 18.2.4), which
   leaves an R6 written mid-line on those two waiting for the next line. That
   same test falls on a frame's own head, where the three others hold the
   equality back: "setting R6 to 0 when C4 and C9 are 0, but C0>0 will have
   no effect before the new frame (or the BORDER will be activated)" (ch.
   18.3.4). */
static bool tests_r6_every_character(const crtc_t *crtc) {
  return crtc->type != 3 && crtc->type != 4;
}

static bool takes_r6_on_a_frames_first_line(const crtc_t *crtc) {
  return crtc->type == 3 || crtc->type == 4;
}

/* A type 1 borders on an R6 of 0 wherever C4 stands: "the value 0 is
   specifically considered and triggers a BORDER without the condition C4=R6
   being required" (ch. 18.2.3). It lasts as long as the register does —
   "when R6 is updated with 0, the BORDER is activated as long as the
   register value is 0" (ch. 18.3.3) — so it is read and not latched, and
   what the same chapter does latch is the write made while C4 is 0. */
static bool r6_of_zero_borders_outright(const crtc_t *crtc) {
  return crtc->type == 1 && crtc->registers[6] == 0;
}

/* DISPLAY ENABLE is two latches the equalities throw rather than two
   comparisons standing (ch. 6.1.3, 17.1, 18.1). R1's opens where the line
   begins and shuts where C0 meets R1; when R1 is 0 both fall on the same
   character and the border wins: "If R1 is zeroed then no more characters
   are displayed, regardless of the CRTC of a CPC" (ch. 17.1), which ch.
   17.3's last diagram draws as whole rows of DISP-OFF and ch. 17.5.1 as
   BORDER for types 0, 1 and 2. Ch. 18.3.1 names the same conflict and
   says "it is the deactivation of the BORDER which is activated", which
   read alone could give the opening the character; the drawings and the
   plain sentence outweigh it. R6's shuts where C4 meets R6
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
  /* The stand-in comes before the opening and the equality after it: only
     C0=R1 itself outranks the line's head, and where R0 is 0 the two share
     every character. */
  if (r0_stands_in_for_r1(crtc)) {
    crtc->display_r1 = true;
  }
  if (crtc->c0 == 0 && crtc->c0_reached_r0) {
    crtc->display_r1 = false;
  }
  if (crtc->c0 == r[1]) {
    crtc->display_r1 = true;
  }
  if (first_line && crtc->c0 == 0) {
    crtc->display_r6 = false;
  }
  /* An R6 of 0 on a type 1 throws no latch: that chip's border is read from
     the register and given up with it, and the only thing that outlives the
     register is the write ch. 18.3.3 keeps for the frame. */
  if (crtc->c4 == r[6] && !r6_of_zero_borders_outright(crtc) &&
      (tests_r6_every_character(crtc) || crtc->c0 == 0) &&
      (!first_line || takes_r6_on_a_frames_first_line(crtc))) {
    crtc->display_r6 = true;
  }
  /* The one thing that makes the first line's cancellable border stand: "in
     this situation however, if R6 is 0 when C0=R1, the BORDER becomes
     definitive" (ch. 18.3.2). Nothing inside the frame opens it again. */
  if (first_line && r[6] == 0 && crtc->c0 == r[1] && takes_the_r6_conflict(crtc)) {
    crtc->display_r6 = true;
  }
}

/* The type 1 status register's bit 5, which reports the BORDER R6 condition
   on the two heads of line the chapter names: "False: C4=C9=C0=0 / True:
   C4=R6 & C9=C0=0" (ch. 21.3.3). It is read at a line's head and not where
   the border itself is thrown, which is why a program is answered for the
   line being drawn rather than for the picture in front of it — "this does
   not necessarily mean that BORDER or CHARACTERS are displayed". An R6 of
   0, written while C4 stands above it, is never seen: the equality does not
   come round again inside the frame, and "if R6=0 while C4>0 and the status
   is 0 (Characters displayed), bit 5 of the status register will continue
   to be 0". */
static void latch_status_border(crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  if (crtc->c0 != 0 || crtc->c9 != 0) {
    return;
  }
  if (crtc->c4 == 0) {
    crtc->status_border_r6 = false;
  } else if (crtc->c4 == r[6]) {
    crtc->status_border_r6 = true;
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
   17.6.2). Never reached is wider than R1 standing above R0: an R1 moved
   out of C0's way mid-line has not shut the display either. The
   Compendium does not settle that case outright, and nothing we can run
   grades it.

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
  bool display = !border_r1 && !crtc->display_r6 && !r6_of_zero_borders_outright(crtc) &&
                 skew != SKEW_BORDER_ON;
  bool r6_conflict = takes_the_r6_conflict(crtc) && crtc->c4 == 0 && crtc->c9 == 0 && r[6] == 0;
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
    /* The other three keep no such anticipation and simply turn over:
       "ParityFrame switch between each frame when C4 = C9 = C0 = 0" and do
       so "whatever the value of R8" (ch. 19.5.3, 19.5.5). Where a type 0 or
       a type 2 holds its parity for ever once C4 can no longer reach R6,
       those three cannot be frozen at all. */
    crtc->parity_frame = anticipates_the_parity(crtc) ? crtc->parity_r6 : !crtc->parity_frame;
    /* "At the beginning of the Frame, ParityC9=ParityFrame" (ch. 19.5.3),
       and the same sentence for types 3 and 4 in ch. 19.5.5. */
    crtc->parity_c9_held = crtc->parity_frame;
  }
  crtc->stood_on_the_frame_head = on_the_frame_head;
  if (crtc->c4 == crtc->registers[6]) {
    crtc->parity_r6 = !crtc->parity_frame;
  }
}

void crtc_give_line_end_rooms(crtc_t *crtc, crtc_t rooms[2]) {
  /* A line's end kept in one room is not kept in another, and a chip given
     none has nowhere to go back to: the end stands only while the room
     holding it does. A host re-pointing the chip at the rooms it already has
     changes nothing, which is what lets a machine do it every tick. */
  if (crtc->line_end_rooms != rooms) {
    crtc->a_line_end_is_kept = false;
    crtc->the_line_end_before_is_kept = false;
  }
  crtc->line_end_rooms = rooms;
}

/* The decisions a character's tick makes once the chip has entered it. They
   read latches that entering it and the character before leave, and a caller
   outside the tick sets the sync's end as the tick would and clears the
   frame's head, the character before having decided nothing; what it would
   have decided is among the comparisons crtc.h counts lost. */
static void decide_on_the_character(crtc_t *crtc) {
  settle_parity(crtc);
  decide_last_line(crtc);
  begin_vertical_adjustment(crtc);
  move_video_pointer(crtc);
  authorize_vsync(crtc);
  begin_syncs(crtc);
  latch_status_border(crtc);
  throw_display_latches(crtc);
}

uint64_t crtc_tick(crtc_t *crtc) {
  enter_character(crtc);
  decide_on_the_character(crtc);
  /* Cleared after the phases rather than before them, a write being made
     between two ticks and read by the second of the two. */
  crtc->r3_written_for_this_character = false;
  return pins_of(crtc);
}

/* STATUS 1, which types 3 and 4 answer in place of R10. "The designers of
   these ASIC's used R10 and R11 as status registers in order to trace a large
   number of events", and ch. 21.3.4.1 tabulates this one bit by bit: each
   names the value the bit takes while its event holds, so all but the first
   stand at 1 and fall to 0 on their character. Ch. 28.1.10 confirms the
   reading of the first, "bit 0 of status 1 which is worth 1 when C0=R0 (0
   otherwise)". The chapter warns what precision they are written at: "several
   bits only change state for 1 µsec". */
static uint8_t status_1(const crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  /* Ch. 4.3's own view of the register gives this byte one fixed bit and
     seven traced ones — "s 1 s s s s s s" against R10's CRTC 3,4 column — so
     every bit but the sixth idles here and is pulled to its event's value. */
  uint8_t status = 0xFE;
  if (crtc->c0 == r[0]) {
    status |= 0x01;
  }
  if (crtc->c0 == r[0] / 2) {
    status &= (uint8_t)~0x02;
  }
  /* "C0=R1-1 (if R0>=R1)", taken at its arithmetic: an R1 of 0 puts the event
     at 255, which only a line of 256 characters ever reaches. */
  if (r[0] >= r[1] && crtc->c0 == (uint8_t)(r[1] - 1)) {
    status &= (uint8_t)~0x04;
  }
  if (crtc->c0 == r[2]) {
    status &= (uint8_t)~0x08;
  }
  /* "C0=R2+R3", where the R3 that ends an HSYNC is its low nibble alone —
     the same table names the other half R3h where it means it — and a nibble
     of 0 ends one sixteen characters on, which is our reading: ch. 28.1.5
     gives the sync that length, and no chapter gives the bit's. */
  uint8_t hsync_width = (r[3] & 0x0F) != 0 ? (uint8_t)(r[3] & 0x0F) : 16;
  if (crtc->c0 == (uint8_t)(r[2] + hsync_width)) {
    status &= (uint8_t)~0x10;
  }
  /* Bit 5 counts lines from the VSYNC and is not answered here. The chapter
     gives it two rows that cannot share an idle value — "R3h>0 : C0=0..R0 on
     the line R3h from Vsync (C4=R7)" takes the bit to 0 on its event where
     "R3h=0 : C0=0..R0 over 15 lines from Vsync (C4=R7)" takes it to 1 — and
     settles neither the character the count begins at nor why the second
     spans fifteen lines where a width of nothing runs sixteen. It stands at
     the idle the rest of this byte keeps, whatever R3h holds. */
  /* "Bit 7 is used to indicate that on the next CRTC character, the less
     significant byte of the video pointer will be reset to 0 (either from an
     overrun on the current VMA pointer, or when this pointer is going to be
     reloaded from VMA' at the end of the line)" (ch. 21.3.4.1). */
  bool pointer_wraps = crtc->c0 < r[0] ? (crtc->vma & 0xFF) == 0xFF : (crtc->vma_ & 0xFF) == 0x00;
  if (pointer_wraps) {
    status &= (uint8_t)~0x80;
  }
  return status;
}

/* STATUS 2, answered in place of R11 (ch. 21.3.4.2). Three of its bits name a
   frame's last character of a kind, one is a timer, and two are wired. */
static uint8_t status_2(const crtc_t *crtc) {
  const uint8_t *r = crtc->registers;
  uint8_t status = 0x37; /* bit 4 "Always 1", bit 6 "Always 0", the rest idle */
  /* "C9=R9" is the documented counter, and on these two in the interlace
     video mode this chip keeps a count and a parity where ch. 19.8.4 keeps
     the address, so the line the chapter means is the one the row actually
     ends on rather than the bare comparison. */
  bool row_and_line_ending = row_is_on_its_last_scanline(crtc) && crtc->c0 == r[0];
  if (row_and_line_ending && crtc->c4 == r[4]) {
    status &= (uint8_t)~0x01; /* "Last char of screen" */
  }
  if (row_and_line_ending && crtc->c4 == (uint8_t)(r[6] - 1)) {
    status &= (uint8_t)~0x02; /* "Last char displayed" */
  }
  if (row_and_line_ending && crtc->c4 == (uint8_t)(r[7] - 1)) {
    status &= (uint8_t)~0x04; /* "Last char before Vsync" */
  }
  /* "Bit 3 of status 2 toggles from 1 to 0 and vice versa over the entire
     frame every 16 frames." */
  if ((crtc->frames_counted & 0x10) != 0) {
    status |= 0x08;
  }
  if (row_is_on_its_last_scanline(crtc)) {
    status &= (uint8_t)~0x20; /* "C9=R9 : C0=0 to R0", the whole line long */
  }
  if (row_and_line_ending || (crtc->c9 == 0 && crtc->c0 < r[0])) {
    status |= 0x80;
  }
  return status;
}

/* What the read port answers. Type 0 reads R12-R17; types 1 and 2 read only
   the cursor and the light pen, and for either "an attempt to read another
   register (0 to 255) returns the value 0" — except register 31 on type 1,
   which "returns a non-zero value (I got 127 or 255)", one UMC defined and
   this model never used (ch. 21.2.1, 21.2.2). Silicon does not settle which
   of the two it gives and this answers the larger.

   Ch. 28.1.9 says of a type 2 that the port "is used to read registers R16
   and R17", where ch. 21.2.2 gives it R14 to R17 as it gives type 1. The
   register table is followed over the identification chapter's summary of
   it, and nothing here grades the difference: the cursor is the only place
   the two disagree, and no suite on this disc reads it.

   Types 3 and 4 read a table of eight instead, "only the 3 least significant
   bits of the selected register number" choosing the row (ch. 21.2.3). The
   chapter prints it, and these are its eight: R16, R17, R10, R11, R12, R13,
   R14, R15, where R10 and R11 hold no register but "Asic CRTC Status 1" and
   "Asic CRTC Status 2". The same disagreement as above sits over it — ch.
   28.1.9's summary names R16, R17, R10, R11, R12 and R13 and leaves the
   cursor out — and the table is followed here as it is there. */
static uint8_t readable_register(const crtc_t *crtc) {
  uint8_t number = crtc->address_register;
  if (crtc->type == 3 || crtc->type == 4) {
    static const uint8_t taken_by_three_bits[8] = {16, 17, 10, 11, 12, 13, 14, 15};
    uint8_t chosen = taken_by_three_bits[number & 7];
    if (chosen == 10) {
      return status_1(crtc);
    }
    if (chosen == 11) {
      return status_2(crtc);
    }
    return crtc->registers[chosen];
  }
  if (number >= 14 && number <= 17) {
    return crtc->registers[number];
  }
  if (crtc->type == 0 && (number == 12 || number == 13)) {
    return crtc->registers[number];
  }
  if (crtc->type == 1 && number == 31) {
    return 0xFF;
  }
  return 0;
}

/* An R8 write that turns the interlace video mode on or off sets a type 1's
   parities outright, and hands types 3 and 4 the one they keep, where types
   0 and 2 take theirs from the counters alone. Ch. 19.5.3 gives a type 1's
   rules and the microseconds they fall on — "these updates are performed on
   the 3rd and 4th µseconds of the OUT(C),C instruction" — and since both
   fall inside the one instruction, they are taken here in that order. What
   they are for is stated plainly: "if IVM mode is toggled on and off on an
   even C9 line, regardless of the value of R9, the parity is set to EVEN. It
   is thus possible to fix the parity quite easily on this CRTC", which is
   the only means a program has of choosing a field on that type. */
static void take_up_r8_parity(crtc_t *crtc, bool was_video_mode) {
  if (!holds_its_c9_parity(crtc) || was_video_mode == interlace_video_asked(crtc)) {
    return;
  }
  if (crtc->type != 1) {
    /* Types 3 and 4 take the line's own parity and nothing else: "when R8
       changes to 1 or 3, Parityc9=C9.0" (ch. 19.5.5, 19.8.4). Their
       ParityFrame is not touched by the write. Taking it on the way in alone
       is a reading: the chapter's "to 1 or 3" covers an R8 driven to 1 from
       0, from 2 or from 3, and none of those is taken here. Nothing can tell
       the two apart, because the bit assigned is the one the address already
       carries while the doubling stands, and a line cannot move under the two
       writes of a pulse that never latches it — so a write on any of those
       edges sets what was already there, and "this C9 parity is managed only
       when R8 = 3 to update C9" while any later return to 3 assigns afresh.
       The toggle a C4 brings reaches nothing a program can see while the mode
       is off, for the same reason, though a test reading the state could hold
       it. Both stand where their chapter puts them. */
    if (interlace_video_asked(crtc)) {
      crtc->parity_c9_held = (c9_vma(crtc) & 1) != 0;
    }
    return;
  }
  /* "C4.0 and not (R9.0)", which stands for the whole of the third rule's
     correction and again for the fourth's. */
  bool c4_carries_it = (crtc->c4 & 1) != 0 && (crtc->registers[9] & 1) == 0;
  /* The third microsecond, whichever way the mode is going: "ParityC9 =
     C9.0", then "ParityC9 = ParityC9 xor (C4.0 and not (R9.0))". The
     frame's own parity is not touched by it. While the mode stands, C9's
     low bit is the parity itself and not the counter's — "C9 = ParityC9"
     (ch. 19.8.2) — so leaving reads back what entering installed, which is
     what makes a pulse of the mode settle the parity at all. */
  bool c9_bit_0 =
      (was_video_mode || crtc->interlace_video_mode) ? crtc->parity_c9_held : (crtc->c9 & 1) != 0;
  crtc->parity_c9_held = c9_bit_0 != c4_carries_it;
  if (interlace_video_asked(crtc)) {
    /* And the fourth, entering: the frame's parity survives only where it
       and ParityC9 were both odd — "Parityframe changes to even, except for
       cases where ParityFrame and ParityC9 were odd before the request". */
    if (!crtc->parity_frame) {
      crtc->parity_c9_held = c4_carries_it;
    }
    crtc->parity_frame = crtc->parity_frame && (crtc->parity_c9_held != c4_carries_it);
  } else {
    /* Leaving, the frame takes what the row held: "ParityFrame=ParityC9". */
    crtc->parity_frame = crtc->parity_c9_held;
  }
  /* And the counter moves with the parity: "the parity and/or bit 0 of
     C9 are updated" by the write itself (ch. 19.5.3), which the same
     page says is not free — "deactivate the IVM mode can also modify C9,
     and modify the end condition of character C4". That a type 1's
     counter moves inside a line at all is ch. 19.5.5's, said of the two
     types that do not: "unlike CRTC's 1 and 2, and as CRTC 0, C9 does
     not change during the line". Ch. 19.5.3's sixteen diagrams draw the
     bit through both writes of a pulse, and a pulse landing on an odd C9
     while the frame's parity is even puts the counter back to the line
     before, which is a line the frame gains. The 3rd microsecond's value
     and the 4th's are one write here, so what the diagrams draw between
     them is not ours to show; what stands after each write is.

     Only while the doubling is not standing, and that is a reading
     rather than a quotation. The C9 the chapter's rules move is ch.
     19.8.2's, the one counter a type 1 keeps, which is the address
     itself; this chip keeps a count and a parity instead, so while the
     mode is latched the bit named here is the one c9_vma already fills
     and this counter's own bit 0 is worth two lines of address. Writing
     it there would spend the parity twice and move the address two
     lines, either way. A pulse that stays inside a line never latches
     the mode, which is where all sixteen diagrams draw it; the counters
     are handed back to each other at the edges instead, where a pulse
     does latch. */
  if (!crtc->interlace_video_mode) {
    crtc->c9 = (uint8_t)((crtc->c9 & ~1u) | (crtc->parity_c9_held ? 1u : 0u));
  }
}

/* What a type 2's chapters call "a noticeable bug on the management of the
   additional line": "if the IVM mode is activated on the first line of an odd
   frame, then this line will become an additional line, and a new line 0 will
   follow the old line 0, which will extend the size of the frame by R0 µsec.
   This is true whatever the value of C0 (0 to R0) on which the IVM mode is
   activated" (ch. 19.6.3, and ch. 19.5.4 in nearly the same words). Ch. 19.8.3
   names that line by its counters, "When C4=C9=0", and says what follows it:
   "C9 and C9.IVM are cleared on the 2nd line". Ch. 19.5.4 says what a program
   has it for: "it is possible to test the existence of the additional line to
   determine parity".

   The first of ch. 19.8.3's own switching diagrams draws the other answer: its
   odd frame, given the mode on C4=0 and C9=0, runs on to C9=1 with no new line
   0. The prose is taken, in three places against one table, and Shaker takes
   it too: the routine settling the parity before its C (O), C (P), C (S)
   and C (8) reads this line to learn which frame it is on, and the three of
   those that grade themselves agree with silicon only where it is here. What
   ch. 19.6.3 says the line shows meanwhile — "the C9 displayed as soon as R8=3
   on this line will be odd (i.e. C9=1)" — is not here: this chip takes the mode
   up at the next C0=0, which the head of crtc.h declares, and the line keeps
   its address to its end. The table cannot say which, every one of its rows
   that carries a write showing the address from before it.

   The mode given up again inside that line takes the line back: "if the IVM
   mode is disabled during the additional line (C4 being then greater than R4),
   then C4 will not be automatically reset to 0 on the next line. C9 will count
   until it reaches R9" (ch. 19.6.3), which on a frame's first line is the count
   going on as it always does. That the paragraph reaches this line as well as
   the one a frame's end adds, whose C4 it names, is our reading, and nothing we
   can run grades it.

   The line after is a frame's head by its counters, and ParityFrame takes
   ParityR6 there as at any other (ch. 19.5.4). Where R6 is 0, C4 has already
   turned ParityR6 on the first line, so the frame goes on even; nothing says
   whether silicon's does, and nothing grades it. */
static void take_up_r8_on_a_frames_first_line(crtc_t *crtc, bool was_video_mode) {
  if (crtc->type != 2 || was_video_mode == interlace_video_asked(crtc)) {
    return;
  }
  crtc->frame_begins_again =
      !was_video_mode && crtc->c4 == 0 && crtc->c9 == 0 && crtc->parity_frame;
}

uint64_t crtc_access(crtc_t *crtc, uint64_t pins) {
  if (!(pins & CRTC_CS)) {
    return pins;
  }
  if (pins & CRTC_RW) {
    if (!(pins & CRTC_RS)) {
      /* The status port. Types 0 and 2 "do not have a status register", and
         what a program reads there is a bus nobody drives: "my CPC CRTC 2
         always returns 255 ... my CPC CRTC 0 randomly returns 255 or 127"
         (ch. 21.3.2), so the pins pass through for the machine to answer.
         Type 1 has one, and drives it (ch. 21.3.1, 21.3.3). Types 3 and 4
         have none of their own either: that port "is a mirror of the read
         port for CRTC's 3 and 4, which handle status differently" (ch.
         21.3.1), the difference being the two status bytes the read port's
         own table carries (ch. 21.2.3, 28.1.8). */
      if (crtc->type == 3 || crtc->type == 4) {
        return crtc_set_data(pins, readable_register(crtc));
      }
      if (crtc->type != 1) {
        return pins;
      }
      /* Bits 0 to 4 and bit 7 are unused and "repeated readings of this
         register return 0 on these bits" (ch. 21.3.3). Bit 6 says a light
         pen reading stands, and no host here wires that pin, so it is 0
         wherever a real one would have something to report. */
      return crtc_set_data(pins, crtc->status_border_r6 ? 0x20 : 0x00);
    }
    return crtc_set_data(pins, readable_register(crtc));
  }
  if (!(pins & CRTC_RS)) {
    crtc->address_register = crtc_data(pins) & 0x1F;
    return pins;
  }
  if (crtc->address_register < 18) {
    uint8_t mask = writable_bits[crtc->address_register];
    if (mask != 0) {
      bool was_video_mode = interlace_video_asked(crtc);
      crtc->registers[crtc->address_register] = crtc_data(pins) & mask;
      if (crtc->address_register == 0 && crtc->registers[0] == 0) {
        /* A width of nothing leaves the counter nothing to count to. The
           chapters put that only from a counter already home — "if R0=0,
           then C0 never reaches 1 (and therefore remains at 0)"
           (ch. 13.2.1), "when R0 is 0 and C0=0, then C0 remains at 0"
           (ch. 13.2.6) — and ch. 6.1.4 draws the rule as the comparison
           itself, "If C0=R0 / Then C0=0", which under a counter past 0 gives
           the overflow a narrowed width gives any running counter instead.
           The reach past C0=0 is this chip's own, and Shaker's B (6) is all
           that stands behind it: its "OUTI ON C0=0,R0=0" writes the width
           seven microseconds before the OUTI's write lands, and reads that
           write landing on C0=0, which a counter spending those microseconds
           climbing cannot do. The disc grades it on the type 0, 1, 2 and 4
           records and agrees on all four, the type 1's once that type's
           write could reach the end before the one standing. The type 3's
           record never grades it, that group not naming the machine there.
           Where a line's end is standing, the take-back below outranks this:
           it restores the character the ending was taken on and the counter
           goes on from there, the premature comparison of ch. 13.3's third
           note being made against a width of nothing like any other. */
        crtc->c0 = 0;
      }
      if (crtc->address_register == 0 && crtc->a_line_end_is_kept &&
          (pins & CRTC_ON_THE_CHARACTER_CLOCK) != 0) {
        /* Ch. 13.3's third note draws it: "on the position where C0 should
           have gone to 0, if R0 is modified on the last µsecond of the OUTI
           instruction, then C0 is compared with the new value of R0, which
           can lead to an overflow of C0". So the line that had just ended did
           not end, and the counter goes on from the characters actually
           drawn. Ch. 13.6.2 gives a type 1's OUTI one placement more than
           ch. 13.6.1 gives the others, which is ch. 13.7.1's "internal
           processing phase shift between this CRTC and CRTCs 0 and 2";
           "the comparison of C0 with R0 ... takes place after R0 is updated
           at the 5th µsecond of the instruction of the OUTI instruction" is
           that chip's own sentence (the doubled words are ch. 13.7.1.1's
           own). The second character this chip gives it is one placement
           past that again, which the disc asks for and the chapter does
           not. */
        /* Where the end before is still kept the write reaches that one,
           and what stands after it is the line as the new width would have
           run it. */
        bool from_the_line_end_before = crtc->the_line_end_before_is_kept;
        crtc_t *room = &crtc->line_end_rooms[from_the_line_end_before ? 1 : 0];
        uint8_t the_character_it_ended_on = room->c0;
        /* Read before the copy comes back, which would carry the room's own
           stale answer to this question. */
        bool a_character_was_drawn_since =
            from_the_line_end_before || crtc->a_character_was_drawn_since;
        if (crtc->registers[0] != the_character_it_ended_on) {
          /* What is taken back is the line's ending, not the write that
             cancelled it: the copy was taken before the host touched the
             chip, so the register file and the register a write is aimed at
             cross over as they stand rather than as they stood. */
          for (unsigned which = 0; which < sizeof crtc->registers; which++) {
            room->registers[which] = crtc->registers[which];
          }
          room->address_register = crtc->address_register;
          *crtc = *room;
          crtc->a_line_end_is_kept = false;
          crtc->the_line_end_before_is_kept = false;
          crtc->a_character_was_drawn_since = false;
          crtc->c0 = (uint8_t)(the_character_it_ended_on + (a_character_was_drawn_since ? 2 : 1));
          /* No decision was made on the characters the line runs on
             through, so the latch says none of them stood on the frame's
             head, whatever the copy says. Where one wraps to C0=0 — a line
             of 256, or of 255 on a type 1's second character — it may have
             stood there; its turn of the parity is lost, or taken by the next
             head where that head is the frame's too. */
          crtc->stood_on_the_frame_head = false;
          if (a_character_was_drawn_since) {
            /* That character was drawn, and the copy predates it. */
            pass_a_character(crtc);
          }
          if (crtc->c0 == 1 || (a_character_was_drawn_since && crtc->c0 == 2)) {
            /* The one line whose management was never given back is the one
               C0 could not carry to 1, and the counter now stands there, or
               stood there on the character the ending was taken on
               (ch. 13.2.4). On every wider line it was given back where the
               chip gives it, and this says again what already stands. */
            crtc->c9_processing_managed = true;
          }
          if (a_character_was_drawn_since &&
              crtc->registers[0] == (uint8_t)(the_character_it_ended_on + 1)) {
            /* The new width names the character the ending was taken on, and
               "C0 is compared with the new value of R0" (ch. 13.3, note 3) on
               the character drawn since: the line ends there and that
               character is the next line's head, decided as any head is, so
               a line of nothing given a width of 1 is a line of two characters
               rather than a counter run round. That end is kept for no write
               to take back, no instruction landing two on a width a
               microsecond apart. */
            crtc->c0 = 0;
            crtc->c0_reached_r0 = true;
            enter_scanline(crtc);
            decide_on_the_character(crtc);
          }
        }
      }
      if (crtc->address_register == 4) {
        crtc->r4_moved_at_a_lines_end = crtc->registers[4] > 0 && crtc->c0 == crtc->registers[0];
      }
      if (crtc->address_register == 6 && crtc->type == 1 && crtc->registers[6] == 0 &&
          crtc->c4 == 0 && crtc->c9 == 0) {
        /* "However, if C4=R6=0 (1st line-character of a new frame) during
           this update, BORDER R6 becomes true first for all the rest of the
           frame, until the new frame" (ch. 18.3.3). */
        crtc->display_r6 = true;
      }
      if (crtc->address_register == 8) {
        take_up_r8_parity(crtc, was_video_mode);
        take_up_r8_on_a_frames_first_line(crtc, was_video_mode);
      }
      if (crtc->address_register == 3) {
        crtc->r3_written_for_this_character = true;
      }
      if (crtc->address_register == 1 && crtc->registers[1] == crtc->c0) {
        /* "The condition C0=R1 is considered immediately on a line" (ch.
           17.3): a write that makes the equality true shuts the display
           for the rest of the character's line, rather than waiting for C0
           to meet the new value again. Ch. 17.5.1 has an R1 of 0 written
           on C0=0 arrive "just in time" on types 0, 1 and 2, and its
           drawing borders that character too; here it keeps the pins it
           was given at its tick. VMA' is not captured by the write. Of a
           type 2 the chapter says "An update of R1 on position C0=R1
           arrives too late. VMA' update has already taken place using the
           old value of R1" (ch. 17.4.3), a type 1 takes it in time (ch.
           17.4.2), and of a type 0 it says nothing. */
        crtc->display_r1 = true;
      }
      if (crtc->address_register == 7) {
        /* Writing R7 changes the comparison whatever the value written, so
           it can serve again (ch. 16.3) — except where an equality made by
           hand is a blocked VSYNC rather than one: on a type 0 "if the
           modification of R7 with the value of C4 took place when C0vs<2, we
           are in a BLOCKED VSYNC" (ch. 16.4.1.1), and the other types as
           blocks_an_equality_made_by_hand says. */
        crtc->vsync_blocked = c4_stands_on_r7(crtc) && blocks_an_equality_made_by_hand(crtc);
        /* And on a type 1 the equality a write makes is read on the character
           the write lands on, not on the one after it, where the board
           reports the write finishing on the character clock. It is the same
           quarter-character ch. 13.7.1's phase shift gives R0, and the
           chapters time it: a PPI read answers such a write "at the earliest
           5 μsec after" on this chip (ch. 16.4.2) where ch. 16.4.1.1 has a
           type 0 read "6 µsec later". What it buys a program is the last
           chance of ch. 16.4 one microsecond later than the rest of them
           have it: C4 has already walked onto the value being written, and
           the others do not read that equality until the character after,
           where the block above has already spent it. A type 2 reads it there
           too, and that rests on the disc rather than on ch. 13.7.1, which
           sets a type 2 with type 0 in its phase: ch. 16.4.3 gives it no
           timing, only "triggered immediately" and a triggered pulse counted
           word for word as ch. 16.4.2 counts a type 1's, and Shaker's B (6)
           settles the character, its "R7 LAST CHANCE 4TH uSec" landing on the
           last character of the row C4 stands on, the one character the
           equality holds, and wanting the sync of a type 2 as of a type 1.
           That the edge is the character clock's for a type 2 as well is
           taken from the type 1, not settled. */
        if ((crtc->type == 1 || crtc->type == 2) && (pins & CRTC_ON_THE_CHARACTER_CLOCK) != 0) {
          begin_the_vsync(crtc);
        }
      }
    }
  }
  return pins;
}
