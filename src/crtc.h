/*
 * crtc.h — the 6845 CRTC, stepped at its character clock.
 *
 * One crtc_tick() call advances the chip by one character (one CCLK cycle)
 * and returns its output pins: the memory address, the raster address, the
 * syncs and the display enable. The machine wiring decides what those pins
 * mean; this file knows nothing about any machine. Register access arrives
 * asynchronously through crtc_access(), the way the E strobe reaches the
 * chip regardless of CCLK.
 *
 * Type 0 is the type implemented, and eight things are not its alone. The
 * first is what a machine can read of the chip, which is what a program names
 * it by (ch. 28.1.8, 28.1.9). Every type answers: types 0, 1 and 2 from ch.
 * 21.2's table, with the status register type 1 alone has, and the two ASICs
 * from the table of eight ch. 21.2.3 prints, taken by the low three bits of
 * the number — R16, R17, R10, R11, R12, R13, R14, R15 — whose R10 and R11
 * rows hold no register but "Asic CRTC Status 1" and "Asic CRTC Status 2",
 * and whose status port "is a mirror of the read port" (ch. 21.3.1). A
 * program names the chip by this and nothing else. One bit of those two bytes
 * is left standing at its idle, status 1's fifth: the chapter gives it two
 * rows that cannot share an idle value and settles neither the character its
 * count begins at nor why one spans fifteen lines where a width of nothing
 * runs sixteen. What a CRTC=3 machine is here is worth saying too, because
 * the chip does not decide it. Ch. 29.1 tells the two ASICs apart by the
 * computer around them — a CPC PLUS answers an unlock sequence and carries a
 * PPI its ASIC emulates badly, a CPC LOWCOST has neither — and ch. 28.1.10
 * believes one bit of status 2 differs between them as well, "subject to
 * additional tests", without saying how. This repository has no PLUS, so a
 * machine built as a type 3 is a chip in a computer that was never sold, and
 * what reads it finds the one that was. The second is the syncs. A line sync
 * asked for with a width of nothing is none at all on types 0 and 1 and
 * sixteen characters on types 2, 3 and 4: ch. 14.1's table gives "No Hsync"
 * in one column and "16 nop" in the next, and "on CRTC's 2, 3 and 4, the
 * HSYNC lasts 16 µsec when R3=0" (ch. 28.1.5). No graded line moves on it,
 * and one picture group runs further through its own tests on the two ASICs.
 * A zero written into a sync already running is a count to reach, met only
 * when C3l comes round, so the sync runs on to sixteen on every type here, as
 * ch. 14.5 has it on all but a type 1 and ch. 14.5.1 and 14.5.3 draw it; a
 * type 1 cancels the sync instead (ch. 14.5), and that is not here. Only a
 * zero in force before the character C0 meets R2 on gives types 0 and 1 no
 * sync at all. One landing inside the sync's first microsecond is not here
 * either: on types 0 and 1, ch. 14.5.4 has an OUT there end the sync early —
 * it "interrupts the HSYNC prematurely", a pulse shorter than a character,
 * which a chip reporting its pins once a character cannot give — and an OUTI
 * stop it before it starts ("prevents the HSYNC from starting"), where either
 * write reaches this chip after that character's comparison and the sync runs
 * on to sixteen. Types 1 and 2 cannot program the VSYNC's length, so it "is
 * fixed at 16" whatever R3h holds (ch. 16), and a pulse either of them is
 * made to begin by a write to R7 is counted "as if the VSYNC had started
 * when C0=0" and spends a line fewer than a type 0's (ch. 16.4.2, 16.4.3),
 * where one begun on a frame's own half line keeps all sixteen (ch. 28.1.4).
 * Neither of them takes the whole line below either, which ch. 19.7.1 gives
 * "CRTC's 0, 3 and 4" alone. No line on the disc moves on the shortened
 * count or the whole line withheld: what stands behind those two is those
 * chapters' sentences and tests of our own. One more divergence of that
 * pulse stands beside those two, and this one the disc does grade. An equality
 * a program makes by hand at the head of a line is a blocked VSYNC here on a
 * type 0, and on the two ASICs, which keep that type's block for no reason of
 * their own chapter (crtc.c): "the VSYNC is triggered immediately if it was not
 * already in progress, except if this modification occurs when C0vs=0 or C0vs=1
 * ... we are in a BLOCKED VSYNC" is ch. 16.4.1.1's, that type's own chapter,
 * and ch. 16.4.2 answers for a type 1 with no exception at all: "if R7 is
 * modified with the value of C4, then VSYNC is triggered immediately". A type 2
 * has its own chapter's answer, "triggered immediately, except during the HSYNC
 * period (C0=R2 to C0=R2+R3), which triggers the GHOST VSYNC" (ch. 16.4.3), a
 * pulse that counts its lines and prevents another "but without the VSYNC pin
 * being enabled". That GHOST is here, and so is the one the chapter gives the
 * equality C4 walks into during a sync, "If the VSYNC condition occurs during a
 * HSYNC from C0=R2 to C0=R2+R3 ... then the CRTC generates a GHOST VSYNC": the
 * sync is in progress where it ran into the character or ended on it, and not
 * where it only begins there, "When R2=0, the HSYNC starts on C0=0, but the
 * VSYNC has had time to be processed" (ch. 15.4.4). Shaker's C (P) bears it
 * out on a line where the machine gives silicon's value (crtc.c);
 * B (3) "FAKE VSYNC ON CRTC 2" sets R2 to 50 and R3 to 14, under which ch. 7.3
 * says a type 2 raises no VSYNC and the chip raises none, and the frame
 * sync it then tries to raise through the 8255's port B is not wired here
 * (cpc.h). A MID-VSYNC, begun at C0=R0/2 on an even frame, takes the GHOST by
 * the same reading where the sync runs into or ends on that character, which no
 * chapter covers and nothing grades. The two ASICs are refused by a rule of
 * their own besides. "VSYNC starts when C4=R7 and C9=C0=0", and "if R7 is
 * modified with the value of C4 while C0>0 and/or C9>0, it will not trigger
 * CRTC VSYNC" (ch. 16.4.4), which ch. 19.7.1 draws as the exception to
 * every other type — "VSYNC occurs when C4 is equal to R7 on any position
 * of C0 (except on CRTC's 3 and 4, which dictate that C4=C9=C0=0)" — so an
 * equality made by hand anywhere but a frame's corner is spent on them and
 * the pulse waits for the corner after, save where the type 0 block they
 * keep holds it past that corner (crtc.c). A MID-VSYNC is the one thing that
 * moves the character and the delay of ch. 19.7.1's other exception the one
 * thing that moves the line. An R7 of 0 puts the equality on the character
 * the frame's parity turns on, and these two read it first: "the management
 * of the VSYNC has priority over the assignment of ParityFrame", so "if
 * ParityFrame was odd, then there will be no MID-VSYNC ... although
 * ParityFrame has change to Even" (ch. 19.7.3), where the other three turn
 * the parity and read after it (ch. 19.7.2). Shaker's C (S) and the one line
 * of its C (O) at an R7 of 0 come right on both records by it. The chapter
 * makes the comparison at the corner and only delays the start, where this
 * chip makes it where the start falls — R0/2 for a MID-VSYNC, the row's
 * second line for ch. 19.7.1's delay — so an equality made or unmade between
 * the two is read here and not on silicon. An R7 written onto C4 inside a
 * row's first line, from C0=2 up to that start, raises the pulse here where
 * ch. 16.4.4 says it "will not trigger"; and an equality standing at the
 * corner and undone before the start — R7 rewritten, or R8 given up, which
 * leaves that frame no VSYNC at all — loses one silicon raises. Nothing
 * grades any of it. What ch.
 * 16.4.4 denies them beside the corner is not here: "there is no VSYNC
 * reentrancy protection mechanism on these circuits", where this one blocks a
 * second pulse on every type. No line of any record moves on the corner or on
 * the reentrancy — the group that watches this pin to catch a row counter
 * overrunning came right on both by the frame's end these two take where a line
 * ends, below, and the board's microsecond, and neither the corner nor the
 * reentrancy touched one of its lines. The eighth below reaches the line of the
 * disc that grades it on the type 1 and type 2 records by their early reading
 * of the equality alone, with or without a type 0's block. The third is
 * how a type 1 reads R9 in the interlace video mode, and the disc grades it
 * too: its odd-lined rows come of an even R9 where a type 0's come of an odd
 * one (ch. 19.5.3, 19.8.2), and it reads the limit down to that parity where a
 * type 0 reads it up — a character of N lines wanting "the value N-1" of it
 * where a type 0 asks "value N-2" (ch. 19.4.1, 19.4.2), which is why a type 0
 * and a type 1 want R9 "programmed respectively with 6 and 7" for rows of the
 * same four lines (ch. 28.1.7). Entering that mode in the middle of a row still
 * takes the doubling up a line later, as ch. 19.8.1 gives a type 0; the parity
 * ch. 19.8.2 fixes at the write is fixed there now, and the fourth
 * below says how. A type 2 shares none of the third: ch. 19.4.3 and 19.5.4
 * give it an interlace of its own, in which "parity is respected whatever the
 * values of R9 and C4". Its row is not halved in the video mode as every
 * other type's is — "in 'Interlace' mode, C9 is compared with R9 in a
 * conventional way to process C4" (ch. 19.8.3), so "when R9 = 7, we have C4
 * characters of 8 lines for each frame" (ch. 19.4.3) — and what steps by two
 * is a display counter of its own, C9.IVM, the address being "(C9.IVM*2) or
 * Parity". That counter is also where this chip leaves its video pointer, "in
 * order to update the VMA video pointer without C4 being incremented": twice
 * to a row where R9 is odd and once where it is even, the row's own end
 * falling a line short of the counter there and losing the transfer
 * altogether (ch. 19.4.3, 19.8.3). The parity that counter is paired with is
 * the frame's own and nothing besides: ch. 19.5.4 lists the parities this
 * chip keeps and ParityC9 is not among them, so "parity is respected whatever
 * the values of R9 and C4" where the other four balance a pair of rows
 * between two frames. Ch. 19.8.3's eight switching diagrams hold us to it —
 * they give an even frame's C4=1 row the same eight addresses as its C4=0
 * row, where a balanced parity alternates them. One of that interlace's
 * rules is not here. The mode is taken up at the next C0=0 as ch. 19.8.1
 * gives a type 0, where ch. 19.8.3 has this chip take it in the middle of the
 * line that asks — "this translation between C9 and C9.VMA is immediately
 * considered ... including during the line, from position C0 where R8 is
 * modified" — which Shaker's C (6) and B (2) are aimed at and neither grades
 * yet. The other is here: a mode switched on during a frame's first line
 * under an odd frame makes that line "an additional line, and a new line 0
 * will follow the old line 0, which will extend the size of the frame by R0
 * µsec" (ch. 19.6.3, and ch. 19.5.4 in nearly the same words), C9 and C9.IVM
 * being "cleared on the 2nd line" (ch. 19.8.3). The first of ch. 19.8.3's
 * switching diagrams draws no such line; three passages of prose are taken
 * against it, and so is the disc. Ch. 19.5.4 gives a program that line to
 * "determine parity", and the routine Shaker runs to settle the parity before
 * its C (O), C (P), C (S) and C (8) reads it there and turns the parity once
 * where it is not the one wanted. Without the line the routine read the same
 * answer on either parity, so those groups were graded on whichever this chip
 * woke on; with it C (S) and all four of C (O)'s wrong lines come right, as
 * do the two C (P) then had wrong; C (8), which says its piece in a picture,
 * holds that picture still where it had rolled; and the scoreboard stands the
 * same whichever parity the chip wakes on. Ch. 19.6.3's converse is here only
 * for that first line: "if the IVM mode is disabled during the additional
 * line ... then C4 will not be automatically reset to 0 on the next line. C9
 * will count until it reaches R9", which leaves a first line given the mode
 * and back counting on as it would have, and that the paragraph reaches that
 * line at all is our reading. Of the line a frame's end adds, whose C4 above
 * R4 the sentence names, it is not here: that line sends C4 home whatever R8
 * has become. Shaker's C (P) turns the mode off on that line on a type 2, and
 * two of its graded lines are wrong for want of the rule: silicon takes
 * #0B1C and #0020 to reach C4=0 where this chip takes #0004. The fourth is
 * the frame parity itself. Types 1, 3 and 4 anticipate none of it: ParityFrame
 * "switch between each frame when C4 = C9 = C0 = 0" and does so "whatever the
 * value of R8" (ch. 19.5.3, 19.5.5), where a type 0 and a type 2 take the
 * parity R6 anticipated and hold it for ever once C4 can no longer reach R6
 * (ch. 19.5.2, 19.5.4) — so those three cannot be frozen, and cannot be made
 * to add the interlace line to every frame. That line follows each type's own
 * parity (ch. 19.6.1 to 19.6.4). A type 1 holds ParityC9 as a state rather
 * than a sum, and so do types 3 and 4 by a rule of their own — bare, on the
 * write that turns the video mode on, reversed on an odd R9 where a type 1
 * reverses on an even one, and leaving their own C9 where it stands (ch.
 * 19.5.5, 19.8.4) — because an R8 write sets it outright on all three: "these
 * updates are performed on the 3rd and 4th µseconds of the OUT(C),C
 * instruction", and toggling the mode "on and off on an even C9 line,
 * regardless of the value of R9" sets the parity even, which is the only
 * means a program has of choosing a field on this type (ch. 19.5.3). The
 * write settles C9's low bit with it, that bit being ParityC9 and not the
 * count's own while the mode stands ("C9 = ParityC9", ch. 19.8.2), and the
 * chapter names the cost: "deactivate the IVM mode can also modify C9, and
 * modify the end condition of character C4". So a pulse landing on an odd C9
 * while the frame's parity is even puts the counter back to the line before,
 * and the frame runs a line longer than its neighbour. The disc grades the
 * divergence: Shaker's C (S) and C (O) both came right and fell silent on the
 * type 1 record, and C (3) grades nothing at all now, where six of its
 * thirteen lines were wrong. On the three records below it those two stood
 * wrong until those types were given rules of their own — the type 2 the
 * first line its mode can lengthen a frame by, above, and the two ASICs the
 * order in which they read an R7 of 0, in the second. The write is
 * skipped while the doubling stands, because the C9 those rules move is ch.
 * 19.8.2's — the one counter a type 1 keeps, which is the address — and this
 * chip keeps a count and a parity instead, ch. 19.8.1's arrangement for a
 * type 0. The two are handed back to each other wherever a mode is taken up
 * or given up inside a row, which is what the thirteenth of those tests
 * measures: its pulse spans a line, so the doubling starts between its two
 * writes. The rules themselves are graded by the sixteen scenarios ch. 19.5.3
 * draws on its own following pages, each named for the Shaker test that
 * exercises it and each drawing C9 beside the two parities, and all sixteen
 * are a test here — all but one of the rules dies when it is taken away. The
 * one that does not is the parity the frame takes back when the mode is left,
 * which a pulse cannot show because ParityC9's turn and C4's correction
 * cancel: the disc moves on it, in a group that says so in a picture. The
 * fifth is where a frame's additional lines are counted. "On CRTCs 0, 3 and
 * 4, there is no specific C5 counter and C9 is used for comparison with R5.
 * On CRTCs 1 and 2, there is a specific counter C5 used in conjunction with
 * C9" (ch. 11.1), so on those two the row goes on being counted and C4 on
 * advancing through the lines — "regardless of the value of R4 each time
 * C9=R9, as long as C5 has not reached R5" — where the other three hold the
 * row where it stands. No line the disc grades moves on the counter itself:
 * what stands behind that is ch. 11.2.2 and 11.2.3's own tables and a test of
 * our own. A type 1 alone latches a state with the counter, "if R5>0 when C4
 * should return to 0 at the end of the frame", which an R5 taken back to 0
 * does not clear — "the state is not deactivated, C4 does not return to 0 and
 * C5 loops" — so a program can hold a frame open and close it on a line of
 * its own choosing, and that is here. The hold is not endless: "C4, however,
 * continues to be compared to R4 to process the change from C4 to 0", and on
 * the row that comparison comes round on C4 goes back to 0 while "the
 * additional management, however, remains activated" — the state is simply
 * taken again on the comparison that activates it, finds the R5 the program
 * cancelled, and is not taken, so the run ends where C5 next comes round to
 * it (ch. 11.3.2). Ch. 11.3.1 is headed "CRTC's 0, 2" and gives its two the
 * plain overflow of the counter, so neither of them takes the state. Types 3
 * and 4 read R5 as they read R9, reached or passed rather than equal — "if R5
 * is modified with a value below C9+1, then the line is considered the last",
 * and "whether with R5 or R9, it is impossible to overflow C9" (ch. 11.3.3) —
 * so an R5 dropped under the count ends that padding where it stands, where
 * the other three spend the counter's whole round getting back to it. The
 * disc's C (E) came right in all three of its readings on that. One thing
 * that counter carries is not here: a line too narrow to reach the disarm
 * gives types 1 and 2 no additional line where it gives type 0 one, ch.
 * 13.2's window being a type 0's. Types 3 and 4 do decide the frame's end,
 * and the padding after it, where the line ends, from R4, R9 and R5 as they
 * then stand and from the interlace line as C0=R0 answered it. "The
 * modification of register 4 is considered immediately at the end of the
 * line", so "if R4 is updated with a value less than C4, then there is
 * overflow of the C4 counter", and R4 written with the value of C4 on a row's
 * last line ends the frame (ch. 12.5); R9 does the same, ch. 10.3.4.1's table
 * giving the next line 0 in both C9 and C4 whatever line of a frame's last
 * row an R9 of 0 is written on; and "R5 management is considered on each C0
 * position" (ch. 11.4.1). A type 0's own exceptions — a last line unmade at
 * C0=1 spent on an adjustment, and R5's deadline (ch. 10.3.1.2, 12.2, 13.2) —
 * are not theirs: a line of two characters, too narrow for a type 0's disarm,
 * takes no padding for its narrowness on these two, whose R0 "accepts all
 * values without causing any problems for other counters" (ch. 13.5). A line of
 * one character still freezes them, as it freezes every type here, and the C4
 * increment it lands on a last line opens a type 0's adjustment on them too
 * (ch. 13.2.6); and R5 admitting a row whose C4 has passed R4 is a type 0's
 * rule (ch. 11.2.2), carried over. Shaker's E (4) came right on both records on
 * the frame's end taken where the line ends, all twelve of the readings its
 * program makes on each — the record holds none of them, the group printing
 * only where it disagrees — with the microsecond the board gives these two an
 * OUT (cpc.c, ch. 4.4.4), the later of its two writes otherwise arriving on the
 * line's last character, where it overflows, rather than on the next line's
 * first, where silicon has it too late. The exception ch. 12.5 gives a type 3
 * alone is not here: an R4 of 0 written on C0=0 is taken before C4 goes to 0
 * "if the I/O on the CRTC is performed at the same time as a ROM selection",
 * which "is active during the I/O if bit 5 of the Z80A B register is 0", and
 * nothing here tells the chip what else an access selects. The disc names both
 * ASICs a type 4, and asks for the exception of neither. Every other behaviour
 * below is type 0's whatever the type is set to, and a number naming none of
 * the five is neither refused nor corrected. One of those is worth naming
 * because the disc grades it: a type 1 takes R4 written with the value C4
 * already holds as the frame's end wherever on the line it lands — "if we were
 * on the last line (C9=R9), then C9 goes to 0, C4=0" (ch. 12.3) — where a type
 * 0 reads that comparison only while C0 is under 2, and it is a type 0's window
 * this chip keeps; types 1 and 2 are also left a type 0's R5 deadline,
 * ch. 11.4.1 notwithstanding. The sixth is the border R6 asks for. Where R6 is
 * 0 a frame's first line is a conflict on types 0 and 2 and comes out an
 * alternation of bordered and displayed bytes, cancellable until C0 meets R1
 * and definitive after it (ch. 18.3.2); a type 1 borders outright on an R6 of 0
 * "without the condition C4=R6 being required" and gives the border up with the
 * register, except where the write was made while C4 stood at 0, which keeps it
 * for the frame (ch. 18.2.3, 18.3.3); and types 3 and 4 have no conflict and
 * test R6 where a line begins rather than through it (ch. 18.2.4, 18.3.4). No
 * line the disc grades moves on any of the three — what grades them is those
 * chapters and tests of our own, and Shaker says its piece there in pictures.
 * The seventh is where a type 1 reads its offset: R12/R13 reach VMA itself, and
 * reach it at the head of every line the frame's first character row spends
 * with C4 at 0, where the other four load both pointers once and only where C4,
 * C9 and C0 stand together at 0 (ch. 17.4.2, 20.3.2). C4 at 0 is that chip's
 * plainest case and not its rule. Ch. 11.2.4 keeps the same update through the
 * C4 of 1 a run of additional lines gives it, where the run opened with C4 at 0
 * — "if C4=0 before the additional management" — and that is here, along with
 * the exception it ends on: an R4 moved at C0=R0 above 0 takes the carry away.
 * Ch. 11.6 keeps the update past C4 altogether where the border on a row's last
 * line is missed, and that is not here. The disc grades none of the seventh in
 * words and says it in a picture: its E (2), "CRTC 1 VMA TRT C4=R4=0 ON ADJ
 * LINE C4=1 ON NO-EXTENT FRAME", settles on "YOU'VE WON THIS STAGE" with the
 * carry and on "IF YOU CAN READ THIS...YOUR EMULATOR HAS A PROBLEM" without it,
 * and nothing else on the disc moves either way.
 *
 * The eighth is how wide the window is in which a write can still reach the
 * comparisons a character clock settles. None of these chips has quite
 * finished deciding a line's end on the clock that took it: a write finishing
 * on that same clock still moves R0 under the comparison already made, so the
 * line that had ended did not end and the counter goes on from the character
 * it stood on. A type 1 holds that window open across the character after it
 * as well. Ch. 13.6 draws a chronogram to a type, and the relation it is
 * about is kept here: ch. 13.6.2's table runs to five placements where ch.
 * 13.6.1's and ch. 13.6.3's run to four, the extra one being where the OUT
 * has wrapped the line at the old width and the OUTI has not, so a type 1's
 * OUTI reaches one placement beyond every other chip's. Ch. 13.7.1 names the
 * reason "an internal processing phase shift between this CRTC and CRTCs 0
 * and 2", ch. 13.7.1.1 says of the OUTI on that chip that "the comparison of
 * C0 with R0 ... takes place after R0 is updated at the 5th µsecond of the
 * instruction of the OUTI instruction" (the doubled words are the chapter's
 * own), and ch. 13.3's third note works the example.
 *
 * Two things about the width are ours rather than the chapters', and both are
 * one microsecond of the same thing. Measured against ch. 13.6, every
 * placement this chip accepts sits one character later than the chronograms
 * draw it: ch. 13.6.1's last OUTI row, ch. 13.6.3's third and ch. 13.6.2's
 * fifth all draw wrapping the placement this chip lets run on. What the
 * chronograms are about survives that shift, because both sides of it move
 * together — a type 1's OUTI reaches one placement beyond every other chip's
 * here as it does there — but the shift itself is unexplained, and the window
 * is what carries it. The second thing follows from the first: the window is
 * reached only by a write the board reports finishing on the character clock,
 * which behind a Gate Array the OUTI's I/O does and the OUT's does not, so the
 * two instructions stand one microsecond apart there where ch. 13.3 sets them
 * two "in principle". Both readings follow a recording from silicon against a
 * diagram, and both are what carry B (6)'s three R0.JIT lines on the type 0,
 * type 1 and type 2 records, while leaving every frame of the demo records
 * where it stood; the type 4 record grades them too and agrees in all three
 * since the board gives the ASICs their syncs a character late (cpc.c), the
 * group counting from an interrupt. Behind the two ASICs the board takes the
 * OUT's write in the microsecond ch. 4.4.4 names, and on the next character
 * clock by its own choice (cpc.c), so both instructions reach the window there,
 * and their last placements in time stand the one microsecond apart ch. 13.6.3
 * draws for those two. The *Not yet* list below carries what the shift leaves
 * unsettled. A character was drawn in that second microsecond and it counts:
 * the counter goes on from what was drawn and not from the ending, or the line
 * comes out a microsecond too long. On a line of one character that second
 * microsecond is the next line's end, and a type 1's write reaches past it to
 * the end before, which is why the chip keeps two. Ch. 13.6.2 draws the
 * "Previous R0=0" line with a type 1's OUTI going on a character further than
 * ch. 13.6.1 draws a type 0's or 2's at both placements they give, and B (6)'s
 * "OUTI ON C0=0,R0=0" asks the same of the type 1 record.
 *
 * The same character-clock edge reaches another comparison, the C4/R7 equality
 * that starts a VSYNC, which types 1 and 2 read on the character a write lands
 * on where the rest read it on the one after. Ch. 16.4.2 times it for a type 1
 * — a PPI read answers such a write "at the earliest 5 μsec after" where
 * ch. 16.4.1.1 has a type 0 read "6 µsec later" — and ch. 16.4.3 gives a
 * type 2 only "triggered immediately", which Shaker's B (6) settles on that
 * character. What it buys a program is ch. 16.4's last chance, "up to the last
 * µsecond preceding C4=R7", one microsecond later than the other three have
 * it: C4 has already walked onto the value being written, and they do not read
 * that equality until the character after, where the block of the second
 * exception above has already spent it. The board says which edge an access
 * landed on, through a pin no chip has, and one that cannot say leaves it low,
 * which costs every type its window.
 *
 * Two things the chip cannot take back. The characters it has already drawn
 * are one: their pins were handed over before the write arrived, so a machine
 * spends one character of a line that turned out not to have ended, or two
 * where a type 1's write reached the second. A sync begun on them is begun, a
 * byte fetched from the next line's start is fetched. The comparisons those
 * characters would have made are the other. Undoing what the ending did is
 * not making them, and all but two are gone. One made is ch. 13.2.4's C9
 * processing management, which "would in principle be activated on C0=1 if C0
 * succeeded in reaching this value" and which C0 now stands on, or stood on
 * for the character the ending was taken on, a line of one character being
 * the only line that has not already been given it back. The other is a
 * type 1's comparison of the width on the character drawn since, "C0 is
 * compared with the new value of R0" (ch. 13.3, note 3): where the new width
 * names the character the ending was taken on, the line ends there and the
 * character drawn since is the next line's head, decided as any head is. The
 * VSYNC's own authorization is not treated as the C9 management is, and that is
 * an asymmetry rather than a finding: it is raised at C0=2 by a step of exactly
 * the same kind (ch. 13.2.2), a character that never asks for it costs the
 * following line its VSYNC, and on a line of two characters — a width a
 * program building a screen out of invisible lines reaches for — that loses a
 * pulse the chip raises. Nothing we have read or measured says which of the
 * two the silicon does. The border R1 asks for is the second cost, and the
 * plainest of them. Where a line ends on the character before R1, the
 * character the counter goes on to is the one a wider line would have met R1
 * on — and that comparison is among the lost, so the rest of the line stands
 * displayed where a chip that had been that wide all along borders it.
 * Shaker's B (6) grades the window at both its widths and neither cost: its
 * "4TH uSec ON C0=0" comes right on the type 0, type 1 and type 2 records
 * with the first character, its "5TH uSec ON C0=0" for a type 1 with the
 * second, and its "R7 LAST CHANCE 4TH uSec" on the type 1 and type 2 records
 * with the other comparison.
 *
 * Implemented: the frame construction of Compendium ch. 6 as type 0
 * (HD6845S/UM6845) performs it — the character its line ending is decided on
 * held open for a write to cancel it, which is every type's here and the eighth
 * above says what that costs, a width of nothing putting the counter at nothing
 * (the chapters put that only from a counter already home — "when R0 is 0 and
 * C0=0, then C0 remains at 0", ch. 13.2.6 — so the reach past C0=0 is ours on
 * Shaker's B (6) alone, which grades it on four of the five types and agrees
 * on all four of them; it is not the overflow a width narrowed under a running
 * counter gives), its register widths, its VMA/VMA' reload rules, the counter
 * widths a program can overrun, the last line decided while C0 is 0 or 1 and
 * made at C0=2 by a write that lands there, the vertical adjustment a type 0
 * spends on C9 — armed on every last line, taken back where R5 is cancelled in
 * time or the last line itself is unmade, opened by an R4 or R9 moved under a
 * standing last line, and past taking back once begun — and the block that
 * stops one VSYNC condition serving twice. Of R8 everything but the cursor's
 * skew is read: the frame parity this chip keeps in two states rather than one,
 * the line either interlace mode adds to the end of an even frame, the
 * MID-VSYNC that holds an even frame's VSYNC back to the middle of its line —
 * which, beginning away from a line's head, then runs longer than R3 asks for —
 * the whole line an odd frame's VSYNC waits where the video mode gives its rows
 * an odd number of lines (ch. 19.5.2, 19.7.1), and the counting of the interlace
 * video mode, where the raster address becomes C9 doubled with a parity filling
 * the bit it leaves and R9 is read up to that same parity — the doubling taken
 * up a line after the write that asks for it, the parity in the limit at once,
 * which is what leaves a row entered or left off its parity counting the long
 * way round, and what a program reads its own frame parity from (ch. 19.8.1).
 * Above those sit the SKEW-DISPTMG functions, which ch. 19.1 gives this type and
 * withholds from types 1 and 2: a delay of one character or two on both edges of
 * the R1 border, or the display shut outright, which takes the interlace bits
 * with it (ch. 19.2). Nothing outside this repository grades those: what stands
 * behind them is a reading of ch. 19.2's diagrams and no evidence we did not
 * write. The interlace line is asked for on the deadline of its own that ch.
 * 11.9 gives it, later than R5's three characters and read on the last line a
 * frame has — which may be one of R5's own, so a program may add the line or
 * take it back from inside an adjustment, and a frame R5 asked nothing of is
 * held open for it. Shaker's C (P) grades that reach into an adjustment on a
 * type 2: were the deadline read only on the last line of a frame's last row,
 * never on an adjustment line, two more of the group's graded lines would come
 * out wrong. It bounds the character too. Read at C0=#29, where one of its
 * tests writes R8 on a last line, or at any character before it, the deadline
 * puts at least one more line wrong; from C0=#2A to R0, #3F there, the group
 * cannot tell one character from another. R0 is the chapter's word.
 * C0 names the character being drawn and holds it for that whole microsecond,
 * which is what a positional register write needs; the last line and the
 * vertical adjustment already read it, and both are settled at the characters
 * ch. 13.2.1 settles them at rather than wherever they next stand. A VSYNC is
 * authorized by the state ch. 13.2.2 has the chip raise at C0=2 and cancel at
 * the next C0=0, so a line too short to reach C0=2 costs the next one its VSYNC
 * and spends the equality besides; and an equality made by hand at a line's head
 * is a blocked VSYNC rather than a VSYNC, where past C0=1 it triggers where it
 * stands (ch. 16.4.1.1). A line of one character never reaches C0=1, so "C9
 * processing management" is never enabled again and C4, C9 and the VSYNC's own
 * line count freeze where they stand, which is why a pulse begun there never
 * ends; the arming the last managed line made still lands, and a C4 increment is
 * all of it that can, once — and where that increment falls on a last line it is
 * ch. 13.2.6's worked table beginning rather than a row ending, so the
 * adjustment outlives the widening and C9 is measured against R5 for the rest of
 * the frame (ch. 13.2.1, 13.2.4, 13.2.6, 16.4.1.2). Taking that up costs
 * Shaker's C (P) its graded line at most boot phases, and the line is worth less
 * than it looks: read at three in forty on a chip altered in no way at all, and
 * at six in forty on this one, always with the same right answer. The HSYNC's
 * width is counted per character rather than per line and goes on counting. R4
 * and R9 written under a frozen chip are read here, where ch. 13.2.1 says they
 * are no longer considered and ch. 13.2.4 then wants them for the last line it
 * assesses at C0=0; the chapter is in two minds and this is our reading. The
 * adjustment is armed as the chip arms it: ch. 12.1 gives the window, "this
 * management of additional line(s) is managed when C0<2", ch. 13.2.5 has the
 * chip "assess whether it is on the last line, and if so, arm an internal flag
 * by default" there, and leaves C0=2 to "assess the conditions for disarming ...
 * in particular by testing the value of R5". That assessment takes an arm back
 * on R5 alone, so a last line unmade at C0=0 unmakes the arming with it (ch.
 * 12.2) — the interlace line is left out, its question being put "on the last
 * line of a frame" (ch. 11.9) and this one no longer being one, which is our
 * reading and ungraded; an R5 above 0 still admits a line whose C4 has gone past
 * R4, which the assessment cannot see; and an adjustment already begun is past
 * both (ch. 13.2.6), which is what keeps ch. 10.3.1.2's exception alive now the
 * disarm asks only what ch. 13.2.5 says it asks. A line too short to reach the
 * disarm keeps what it was armed with, as ch. 11.2.2 and ch. 12.2 have it at "R0
 * < 2" — the disarm being read at C0=3, where a write made at C0=2 has landed,
 * and again at the head of the line after one of three characters, which has no
 * such character of its own. What such a line draws is here with it. Ch. 11.2.2
 * lists the ways an adjustment comes about with R5 at 0 — an R4 or R9 moved at
 * C0=1 of a last line, "or if C0 can never reach 2 because R0 < 2" — and ch.
 * 13.2.1 and ch. 13.2.5 say what that second one draws, both inside their own
 * R0=1 case: it lasts "1 line of 2 usec before ceasing (C4+1, C9=0)", and only
 * "on the next line" does "the end of additional management reset C4 and C9 to
 * 0". So a narrow line is entered before it is measured and a wider one measured
 * before it is entered, which is ch. 13.2.4's reminder and what Shaker's graded
 * E (1) holds this chip to: it times an R5 cancelled on a 64-character last
 * line, and a chip that gave a line there too would answer four of its seven a
 * line too long. Ch. 13.2.5's own account of that ending — that it stops "when
 * the calculated C9 becomes equal to R5" — cannot produce the picture it draws
 * two sentences later, C9 not being zeroed once C4 has left R4; the picture is
 * followed here and the mechanism is not, and nothing outside this repository
 * grades the narrow line either way. The ceasing after one line is the R0=1
 * case's own; a line of one character keeps its run instead, and "it is then R5
 * which controls the end ... To stop this management, program R5 with C9+1" (ch.
 * 13.2.6). Absent for want of a pin: the cursor, its own skew and the light pen,
 * which no host here wires. Two HSYNCs cannot be contiguous on this type — "if
 * position C0=R2 is encountered when C3l reaches R3l, and R3l has not been
 * modified on this position" — which is what keeps a line shorter than its own
 * sync out of the endless HSYNC the other types fall into (ch. 15.3.1, 15.3.2).
 * A write to R3 there is the exception, taken as the modification whatever value
 * it carries, and the sync it lets through carries the old count on rather than
 * starting from nothing, which is where a R2.JIT HSYNC begins (ch. 15.3.3). That
 * chapter's own example moves R2 as well and comes out right without the
 * exception being reached at all, the new R3l carrying the old sync past its
 * ending comparison on its own. R3l updated during a HSYNC is here as C3l
 * counts it: a value C3l has yet to reach ends the sync where C3l meets it,
 * and one it has reached or passed overflows it and runs the sync round to
 * it (ch. 14.5, and the diagrams of ch. 14.5.1 to 14.5.3). R3.JIT is not:
 * ch. 14.5.4 has the value C3l stands at, written by an OUT on its own
 * character, stop the sync on types 0, 1 and 2, where here it overflows, as
 * ch. 14.5.3 draws it for the two ASICs, on which "this technique does not
 * work" (ch. 14.5.4). Not yet: the gap the R2.JIT leaves, the chip
 * beginning the second sync "around 3.5 Pixel-M2 after the one that has just
 * ended" where a pin reported once per character can only stay high, so the
 * Gate Array is given one sync here where the chip gives it two; a write that
 * changes R3l on that character, seen here by the comparison that ends the sync
 * as well, where the chip's is not; the R0 of ch. 13.7.2 enlarged on the
 * character C0 names 1, where the old value ends the line and the new one counts
 * C0 on; whatever would place this chip's line endings where ch. 13.6 draws
 * them — ch. 13.6.1's last OUTI row, ch. 13.6.3's third and ch. 13.6.2's fifth
 * all draw wrapping a placement this chip lets run on, and Shaker's B (6) says it
 * runs on, so the disc is followed and every placement here sits one character
 * late against all three chronograms, a shift the window above compensates and
 * nothing yet explains, and the reason the two microseconds ch. 13.3 sets "in
 * principle" between an OUT(C),R8 and an OUTI stand at one here behind a Gate
 * Array, the OUT never being rescued there where the OUTI is; what the
 * chronograms are about, a type 1's OUTI one placement beyond the rest,
 * survives it; the first of ch. 16.3's two protections against one C4/R7
 * equality raising two pulses, "it is not possible to trigger or inhibit a
 * VSYNC during a VSYNC. Thus, modifying the value of R7 with a value of C4
 * reached during the VSYNC does not cause a new VSYNC", where an R7 write here
 * brings the comparison round again whenever it is made, a pulse standing or
 * not, and only the second protection — the equality having to change — is
 * kept; the freeze ch. 13.2.1 puts on a line too narrow to reach C0=1, kept
 * here for every type where the chapters give it to type 0 alone — "R0 accepts
 * all values without causing any problem for other counters" (ch. 13.3, 13.4;
 * ch. 13.5 says "problems"), and for three of them in as many words, "if R0 is
 * 0, then C9 and R4 continue to be managed normally" (ch. 13.3, 13.5) — the
 * plain absence of it being measured and refused, a type 1 demo losing 866
 * frames to it and a disc group that stabilizes an R0=0 line stabilizing no
 * longer, so what those four want in its place is not simply nothing; the rest
 * of what ch. 13.2.1 gives a line's first three microseconds — the counter
 * updates those characters schedule for a later one; and the character a write
 * of R1 lands on, which keeps the display it was given where ch. 17.5.1 borders
 * it too, R1's equality being taken where it is written as well as where it
 * stands (ch. 17.3). Every other comparison is made where it stands. Nor is the
 * read table of ch. 21.2.3: it names a register by three bits rather than five
 * and mirrors itself onto the status port, where types 3 and 4 are answered here
 * as a type 2 is. Two of the border's rules move DISPLAY ENABLE inside a
 * character — the byte of border at C0=R0 on a line where R1 was never reached
 * and no skew stands ready to defer it to a whole character of its own (ch.
 * 17.6.2, 19.2.4), and the byte-by-byte alternation an R6 of 0 makes on a
 * frame's first line (ch. 18.3.2) — and both are here, which is why a tick
 * reports that pin for each of the two bytes a character is drawn from rather
 * than once.
 *
 * Technical information sourced from the "Amstrad CPC CRTC Compendium" by
 * Longshot (CC BY-NC-ND).
 *
 * Sources:
 * - "The Amstrad CPC CRTC Compendium" v1.11 (Longshot / Logon System),
 *   https://shaker.logonsystem.eu/ACCC1.11-EN.pdf — the counter names we
 *   adopt as it asks (ch. 3.1), the frame construction (ch. 6), the type-0
 *   register file and access ports (ch. 4.3), VMA/VMA' and their reload
 *   rules (ch. 20).
 * - "The CRTC" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/crtc — the
 *   register overview and the five types.
 */
#ifndef COLOPHON_CRTC_H
#define COLOPHON_CRTC_H

#include <stdbool.h>
#include <stdint.h>

/* Bus pin layout in the 64-bit pin mask:
 * bits 0..13  MA0..MA13 (memory address, a character/word address)
 * bits 16..23 D0..D7    (data bus, the same lanes z80.h uses)
 * bits 24..28 RA0..RA4  (raster address)
 * bits 29..   control pins, names as the datasheets print them, and two the
 *             chip has no pin for: the second byte of DISPLAY ENABLE, and
 *             the character clock an access landed on (both below) */
/* DISPLAY ENABLE. The chip has one such pin, and this reports it twice: two
 * of type 0's rules move it half a character — one of them only while no
 * skew stands ready to defer that border to a whole character of its own —
 * and the machine fetches two bytes for every character the chip names, so
 * what a tick owes the machine is the pin as each byte finds it (ch.
 * 17.6.2, 18.3.2, 19.2.4). The datasheet's
 * name is kept for the first, which is where the pin stands when the
 * character begins; the second wears a name of its own because the
 * datasheet has none for it. */
#define CRTC_DISPTMG (1ULL << 29)
#define CRTC_DISPTMG_SECOND_BYTE (1ULL << 35)
#define CRTC_HSYNC (1ULL << 30)
#define CRTC_VSYNC (1ULL << 31)
#define CRTC_CS (1ULL << 32) /* input: chip select */
#define CRTC_RS (1ULL << 33) /* input: register select (0 address, 1 data) */
#define CRTC_RW (1ULL << 34) /* input: direction; the datasheet's R/W, 1 = read */
/* No pin of the chip's, and no name in any datasheet: an access the board
 * finished on the edge the character clock falls on rather than inside the
 * microsecond that follows it. The chip's own timing turns on the
 * difference — ch. 13.7.1 names "an internal processing phase shift between
 * this CRTC and CRTCs 0 and 2" — and a board that cannot say which edge an
 * access landed on simply leaves this low, which costs every type the window
 * its line ending is kept in, a type 1 the second character of that window
 * besides, and types 1 and 2 their early C4/R7 reading. */
#define CRTC_ON_THE_CHARACTER_CLOCK (1ULL << 36)

static inline uint16_t crtc_ma(uint64_t pins) { return (uint16_t)(pins & 0x3FFF); }
static inline uint8_t crtc_ra(uint64_t pins) { return (uint8_t)((pins >> 24) & 0x1F); }
static inline uint8_t crtc_data(uint64_t pins) { return (uint8_t)((pins >> 16) & 0xFF); }
static inline uint64_t crtc_set_data(uint64_t pins, uint8_t data) {
  return (pins & ~0xFF0000ULL) | ((uint64_t)data << 16);
}

typedef struct crtc_t {
  /* Which of the five types this is, numbered as ch. 4.2's table numbers
     them: 0 is Hitachi's HD6845S and UMC's UM6845, 1 UMC's UM6845R, 2
     Motorola's MC6845, and 3 and 4 the Amstrad ASICs that emulate one. That
     table splits type 1 into a 1-A and a 1-B, two behaviours observed of
     the one part, which nothing here tells apart. */
  uint8_t type;

  /* R0-R17, selected through the address register. Writes are masked to the
     documented type-0 widths (Compendium ch. 4.3); R16/R17 ignore writes. */
  uint8_t registers[18];
  uint8_t address_register; /* AR, 5 bits: the register number a select names */

  /* Counters, named as the Compendium names them (ch. 3.1). Each is
     narrower than the byte holding it, and a program can leave one above
     its limit; the widths are what bring it back (ch. 10.3.1.1, 12.1). */
  uint8_t c0; /* horizontal character counter, 8 bits; after a tick it names
                 the character that tick drew */
  /* The scanline within the character row. On a type 1 its low bit is not
     the count's alone: an R8 write settles it with ParityC9 (ch. 19.5.3),
     and where the interlace video mode stands the address is this doubled
     with that parity filling the bit — a type 2 doubling c9_ivm in its place
     — which c9_vma answers and this does not (ch. 19.8.1, 19.8.3). */
  uint8_t c9;
  /* C9.IVM, a type 2's display counter, held in the five bits C9 has because
     the Compendium gives this one no width of its own: that chip measures R9
     against C9 "in a conventional way to process C4" and keeps this second
     counter for "displaying and managing video pointer updating", the
     address being "(C9.IVM*2) or Parity" (ch. 19.8.3). It is why "when R9 =
     7, we have C4 characters of 8 lines for each frame with an update of
     video pointer every 4 lines" (ch. 19.4.3): the row keeps the height R9
     asks for, where the other four are programmed half of one, and it is
     this counter rather than the row that halves. Only a type 2 reads it, and
     only the paths a type 2 takes maintain it: the other four carry the field
     and leave it standing through their frames' additional lines, where they
     count on C9 and this chip counts on C5. */
  uint8_t c9_ivm;
  uint8_t c4; /* character row counter, 7 bits */
  /* C5, the Vertical Total Adjust Counter, 5 bits: the counter types 1 and
     2 keep the frame's additional lines on, "used in conjunction with C9 to
     allow management of characters within the adjustment lines", where
     types 0, 3 and 4 have none and spend C9 on those lines (ch. 11.1). */
  uint8_t c5;
  uint8_t c3l; /* HSYNC width counter, 4 bits: R3 low nibble. A nibble of 0
                  is no HSYNC at all on types 0 and 1, where types 2, 3 and 4
                  read it as 16 (ch. 14.1, 28.1.5) */
  uint8_t c3h; /* VSYNC scanline counter, 4 bits: R3 high nibble, 0 counts 16,
                  and types 1 and 2 never read that nibble at all (ch. 16) */
  /* Frames since power-on, kept for the one status bit that counts them:
     "bit 3 of status 2 toggles from 1 to 0 and vice versa over the entire
     frame every 16 frames" on types 3 and 4 (ch. 21.3.4.2). It is ours
     rather than the Compendium's, which names no counter behind that bit. */
  uint8_t frames_counted;

  /* This line ends the frame. Decided while C0 is 0 or 1 and held for the
     rest of the line, so a register written afterwards cannot take it back;
     and once more at C0=2 where it is not yet true, which is where a write
     made at C0=1 lands (ch. 10.3.1.2, 12.2). The two ASICs decide it again
     where the line ends, and that is the answer the frame ends on (ch.
     12.5). */
  bool last_line;
  bool vertical_adjustment_armed;
  /* Whether an adjustment has actually begun, as against being armed for
     one: "the additional management being in progress, it can no longer be
     cancelled on C0=2" (ch. 13.2.6), which is the character this file reads
     at C0=3, where a write made at C0=2 has landed, and again at the head of
     the line after one of three characters. */
  bool vertical_adjustment_in_progress;
  /* And whether the line it is giving is the last of them, which only a line
     too narrow to be disarmed can need: such an adjustment lasts "1 line of
     2 usec before ceasing (C4+1, C9=0)", and it is the line after that on
     which "the end of additional management reset C4 and C9 to 0" (ch.
     13.2.1). */
  bool adjustment_on_its_last_line;
  /* Whether the last run to open took the state ch. 11.3.2 has a type 1
     latch — "if R5>0 when C4 should return to 0 at the end of the frame
     (C4=R4, C9=R9)" — which an R5 taken back to 0 does not clear. Taken
     afresh wherever that comparison holds, so that no frame is held by the
     R5 of the frame before it, and false on the other four types. */
  bool r5_opened_the_run;
  /* And whether that run began where a type 1's offset carries into it: "if
     C4=0 before the additional management, then VMA is updated with R12/R13
     and not VMA', and this as long as C4=1" (ch. 11.2.4), the C4 read being
     the one before the run's own increment. Ch. 11.2.4 takes it away again
     where the run was made by an R4 moved at C0=R0 above 0, which
     r4_moved_at_a_lines_end carries to the open. The decision is taken there
     and not retaken, so a hold that brings C4 round to 0 inside a standing
     run under ch. 11.3.2 keeps the answer the open gave; nothing grades
     that. */
  bool adjustment_opened_at_c4_of_zero;
  /* The last R4 write of this frame, and whether it was the one the
     exception names: above 0 — the seven bits R4 keeps, so a written &80 is
     not — and made where C0 stood on R0. Written afresh by every R4 write,
     so a later one anywhere takes an earlier one's answer away. */
  bool r4_moved_at_a_lines_end;

  /* Whether the chip stood on a frame's first character last time it was
     asked. ParityFrame turns as that character is entered (ch. 19.5.2), and
     a chip frozen on it enters nothing: without this the parity would turn
     under a still picture every microsecond. */
  bool stood_on_the_frame_head;

  /* Frame parity, which the Compendium keeps in two states rather than one
     on a type 0 and a type 2 (ch. 19.5.2, 19.5.4). ParityFrame is this
     frame's, taken from ParityR6 at the frame's first character; ParityR6
     anticipates the next frame's where C4 stands on R6, and it does so
     whatever R8 holds. Where R6 stands above R4 C4 never reaches it, and
     both freeze — which is how a program stops those two alternating, and
     how it asks for the extra line on every frame rather than every other.
     Types 1, 3 and 4 keep the one state: ParityFrame turns over at each
     frame's own head, ParityR6 goes unread, and no R6 can stop them
     (ch. 19.5.3, 19.5.5). True is odd, and the power-on zeroes leave a type
     0 or a type 2 on an even first frame where the other three turn at that
     first head and so begin odd; real silicon wakes on whatever it wakes
     on, and every frame after inherits the phase. The document turns
     ParityR6 "when C4 reaches R6" and we read that comparison standing, as
     the same C4/R6 comparison is read for DISPLAY ENABLE (ch. 18.2.1); the
     two part company only for an R6 written mid-frame onto the row the chip
     already stands on, and nothing we can run grades that. */
  bool parity_frame;
  bool parity_r6;
  /* ParityC9 as the three types that are handed it keep it: a state of
     their own rather than a sum of the others, because an R8 write sets
     it outright on those and a sum cannot be told what to hold. On a
     type 1 the write takes it either way the mode is going, corrects it
     for C4 where R9 is even, and reaches C9 itself — the write settles
     that counter's low bit, except while the doubling stands, where the
     bit it would settle is the one c9_vma fills (ch. 19.5.3). Types 3
     and 4 take it only on the way in and bare: "when R8 changes to 1 or
     3, Parityc9=C9.0" (ch. 19.5.5, 19.8.4), with no ParityFrame moved,
     no correction for C4, and their own C9 left where it stands — "as
     CRTC 0, C9 does not change during the line" — and they reverse it on
     an odd R9 where a type 1 reverses it on an even one, their R9 being
     programmed a type 0's way. Ch. 19.8.4 gives those two a type 1's
     counting in the mode, one counter that is the address, so the count
     and the parity are handed back to each other at the mode's edges for
     them as well. Types 0 and 2 are answered by parity_c9() from R9, C4 and
     ParityFrame, which is a type 0's counting and a type 0's for a type 2 as
     well — that chip respects its parity "whatever the values of R9 and C4"
     (ch. 19.5.4), and the divergence is declared at the head of this file.
     No line on any record reaches the two ASICs' share of
     this: what stands behind it is ch. 19.8.4's algorithm, the two frames it
     works by hand, two of the twenty-two counting cases it draws beside
     them, and tests of ours. A row of theirs whose counter is sent past its
     limit now comes home rather than walking to 31, which is ch. 10.3.4.1's
     "more complex" comparison and ch. 19.8.4's "C9 >= R9": the disc's A (U)
     came right on it and two of C (O)'s three lines with it. One rule of
     theirs is still not here — the line an interlaced frame adds leaves
     their C4 where it is and carries no parity of its own, "C9 will always
     be 0, even if the other lines are odd on C4=R4" (ch. 19.6.4) — and
     nothing on either record grades it now, the group that did having
     drawn nothing past its menu since the machine began naming itself. */
  bool parity_c9_held;
  /* What R8 answered at C0=R0, which is where ch. 11.9 asks it and a
     microsecond before the line it decides could begin. */
  bool interlace_line_owed;
  /* The one line interlace adds after the R5 lines, and one to a frame
     however the adjustment carrying it ends (ch. 11.9, 19.6.1). */
  bool interlace_line_given;
  /* The interlace video mode as the counters see it, which is not quite as
     R8 holds it: the mode a write asks for is taken up at the next C0=0,
     after that line's own R9 test (ch. 19.8.1). */
  bool interlace_video_mode;
  /* Whether the interlace video mode was switched on during a type 2's
     first line under an odd frame, so that the frame begins again after that
     line unless the mode is given up first: "this line will become an
     additional line, and a new line 0 will follow the old line 0, which will
     extend the size of the frame by R0 µsec" (ch. 19.6.3). */
  bool frame_begins_again;
  /* A chip that has drawn nothing has no character to leave behind, so the
     first tick draws one instead of advancing past one. Zero is the
     power-on state, which is what leaves the first tick drawing a line's
     first character rather than its second. */
  bool has_drawn_a_character;
  /* No type has quite finished deciding a line's end on the clock that took
     it, so a write landing there can still cancel the wrap; a type 1 keeps
     the clock after that one as well (ch. 13.3, note 3). Two rooms, because
     on a line of one character a type 1's write reaches the end taken a
     character before the one standing: the first holds the end just taken,
     the second the one before it. */
  struct crtc_t *line_end_rooms;
  bool a_line_end_is_kept;
  bool the_line_end_before_is_kept;
  /* And whether a character has been drawn since it ended, which a type 1
     lets happen: its window is two characters wide, and the counter goes on
     from what was drawn rather than from the ending (ch. 13.3, note 3). */
  bool a_character_was_drawn_since;
  /* One C4/R7 equality raises one VSYNC: the comparison must change, by C4
     moving or R7 being written, before it raises another (ch. 16.3). */
  bool vsync_blocked;
  /* The state ch. 13.2.2 has the chip raise at C0=2 so that the next C0=0
     may read C4 against R7. No line has run at power-on and nothing has
     raised it, so this chip wakes holding it, which is the state a chip
     that has been running is in. */
  bool vsync_armed;
  /* "C9 processing management", which ch. 13.2.4 has the chip disable at
     C0=0 and enable again at C0=1, so that a line of one character leaves
     it disabled and C4, C9 and the VSYNC's line count frozen where they
     stand. It wakes standing for the same reason the one above does. */
  bool c9_processing_managed;
  /* And the one thing that still lands while it is disabled: "if C9 had
     reached R9 on the first C0=0, then the reset of C9 had been armed as
     well as the increment to C4. With C9 being frozen, only C4 will
     increment" (ch. 13.2.4). */
  bool c4_increment_armed;
  /* A VSYNC can begin anywhere in a line — where R7 is written the value C4
     already holds, and on every even frame of an interlace mode. C3h is
     then initialized at the next C0=0 instead of being advanced there, so
     the part line it began in is not one of R3's (ch. 16.4.1). */
  bool vsync_began_mid_line;
  /* And whether that late start was the half line an even interlaced frame
     raises its VSYNC on rather than an R7 written to meet C4, which is the
     only one of the two the shortened count belongs to (ch. 16.4.2). */
  bool vsync_began_on_its_half_line;

  /* DISPLAY ENABLE is two latches rather than two comparisons (ch. 6.1.3,
     18.2.1). The R1 one opens at the head of every line and shuts where C0
     meets R1, on that same character when R1 is 0 (ch. 17.1); the R6 one,
     once shut, is shut for the frame, and it outranks the other. */
  bool display_r1;
  /* What that latch was one character ago and two, because the SKEW-DISPTMG
     functions hold the display enable back by one or the other before it
     leaves the chip (ch. 19.2.3). */
  bool display_r1_earlier[2];
  bool display_r6;
  /* And the same condition as the type 1 status register reports it, which
     turns over a character earlier than the border itself is thrown: "bit 5
     of the Status register is updated when C0=R0" (ch. 21.3.3), where the
     border belongs to the line it is drawn on. The two are kept apart so
     that reading the port cannot move the picture. */
  bool status_border_r6;
  /* The line ran its length. Only the C0 that returns to 0 from R0 opens the
     display again — one that got there by overflowing 255 does not (ch.
     17.1). */
  bool c0_reached_r0;

  /* VMA and VMA', the two internal pointers (ch. 20): VMA runs, one
     character per tick; VMA' is the transient row latch that captures VMA
     at C0=R1 on a row's last scanline. The underscore renders the prime
     mark. */
  uint16_t vma;
  uint16_t vma_;

  bool hsync;
  /* Whether the character being drawn is the one an HSYNC ended on: "two
     HSYNC's cannot be contiguous if position C0=R2 is encountered when C3l
     reaches R3l, and R3l has not been modified on this position"
     (ch. 15.3.1). */
  bool hsync_ended_here;
  /* Whether R3 was written in time to be read on that character, which is
     the modification that same sentence exempts. */
  bool r3_written_for_this_character;
  bool vsync;
  /* A type 2's VSYNC begun while its HSYNC is in progress, which counts its
     lines and holds another off as any VSYNC does, "but without the VSYNC
     pin being enabled" (ch. 16.4.3, 15.4.4): the GHOST VSYNC. */
  bool vsync_is_a_ghost;
  /* Whether an R7 a type 2 took off the character clock made the C4/R7
     equality while its HSYNC was in progress. The equality is read on the
     next character, and ch. 15.4.4 draws every such write from C0=R2 to
     C0=R2+R3 triggering a GHOST, the last included; a write that makes no
     equality leaves C4 to walk into one on its own terms. */
  bool r7_was_written_in_the_hsync;
} crtc_t;

/* Power-on as the given type. Real silicon leaves the register file
 * undefined; zeroes here, for determinism, which leaves the chip before a
 * line's first character — with the VSYNC's authorization and the
 * management of C9 the two states that wake standing. */
void crtc_init(crtc_t *crtc, uint8_t type);

/* Advance one character clock. Returns the output pins. */
uint64_t crtc_tick(crtc_t *crtc);

/* Somewhere for the chip to keep the character boundaries it may have to take
 * back: two chips' worth, rooms[0] and rooms[1]. No type has quite finished
 * deciding a line's end on the clock that took it, so a write arriving there
 * can still cancel the wrap, and a type 1 keeps the clock after that one as
 * well — which on a line of one character is the clock of the next end, so
 * the chip keeps what it stood on before each of the last two wraps. A chip
 * given none never takes a wrap back, which is no type's behaviour here and
 * what a host that cannot time its accesses gets. */
void crtc_give_line_end_rooms(crtc_t *crtc, crtc_t rooms[2]);

/* One bus transaction: CS, RS, RW and the data lanes in; the data lanes out
 * when the chip drives them. Where it does not — a read this type never
 * answers — the data passes through untouched, the bus left floating for
 * the machine to interpret. */
uint64_t crtc_access(crtc_t *crtc, uint64_t pins);

#endif
