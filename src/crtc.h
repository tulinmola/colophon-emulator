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
 * Type 0 is the type implemented, and thirteen things are not its alone. The
 * first is what a machine can read of the chip, which is what a program names
 * it by (ch. 28.1.8, 28.1.9). Every type answers: types 0, 1 and 2 from ch.
 * 21.2's table, with the status register type 1 alone has, and the two ASICs
 * from the table of eight ch. 21.2.3 prints, taken by the low three bits of
 * the number — R16, R17, R10, R11, R12, R13, R14, R15 — whose R10 and R11
 * rows hold no register but "Asic CRTC Status 1" and "Asic CRTC Status 2",
 * and whose status port "is a mirror of the read port" (ch. 21.3.1). A
 * program names the chip by this and nothing else. One bit of those two bytes
 * is left standing at its idle, bit 5 of status 1: the chapter gives it two
 * rows that cannot share an idle value and settles neither the character its
 * count begins at nor why one spans fifteen lines where a width of nothing
 * runs sixteen. One bit the table calls wired traces something: bit 4 of
 * status 2, "Always 1" (ch. 21.3.4.2), which Shakerland photographs Shaker's D
 * (U) reading 0 all through a frame R5 pads, and its R5 search finding R5 by
 * counting lines until the bit reads 1. Here it reads 1 on a frame nothing
 * pads and on the last character of a padding, the interlace line's included,
 * and 0 elsewhere in a padded frame and where, under the interlace video
 * mode, the line address has passed R9, which that group's plot draws and
 * which outranks a padding's last character; all of it ours, from those
 * photographs. What a CRTC=3 machine is here is worth saying too, because the
 * chip does not decide it. Ch. 29.1 tells the two ASICs apart by the computer
 * around them — a CPC PLUS answers an unlock sequence and carries a PPI its
 * ASIC emulates badly, a CPC LOWCOST has neither — and ch. 28.1.10
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
 * where one begun on a frame's own half line keeps all sixteen, which is ours:
 * ch. 28.1.4 gives sixteen lines to one that "starts when R7 was programmed
 * before C4=R7" and says nothing of the half line. Neither of them takes the
 * whole line below either, which ch. 19.7.1 gives "CRTC's 0, 3 and 4" alone.
 * No line on the disc moves on the shortened count or the whole line
 * withheld: what stands behind those two is those chapters' sentences and
 * tests of our own. One more divergence of that
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
 * the pulse waits for the corner after. A MID-VSYNC is the one thing that
 * moves the character and the delay of ch. 19.7.1's other exception the one
 * thing that moves the line. An R7 of 0 puts the equality on the character
 * the frame's parity turns on, and these two read it first: "the management
 * of the VSYNC has priority over the assignment of ParityFrame", so "if
 * ParityFrame was odd, then there will be no MID-VSYNC ... although
 * ParityFrame has change to Even" (ch. 19.7.3), where the other three turn
 * the parity and read after it (ch. 19.7.2). Shaker's C (S) and the one test
 * of its C (O) at an R7 of 0, which speak only where the machine is wrong,
 * are silent on both records by that order. The chapter
 * makes the comparison at the corner and only delays the start, where this
 * chip makes it where the start falls — R0/2 for a MID-VSYNC, the row's
 * second line for ch. 19.7.1's delay — so an equality made or unmade between
 * the two is read here and not on silicon. An R7 written onto C4 inside a
 * row's first line, from C0=2 up to that start, raises the pulse here where
 * ch. 16.4.4 says it "will not trigger"; and an equality standing at the
 * corner and undone before the start — R7 rewritten, or R8 given up, which
 * leaves that frame no VSYNC at all — loses one silicon raises. Nothing
 * grades any of it. The protection ch. 16.4.4 denies them beside the corner is
 * missing here too: "there is no VSYNC reentrancy protection mechanism on these
 * circuits", and every line C9 begins at 0 lifts their block (crtc.c); so does
 * every R7 write that does not set it, a reading of ours that can move only a
 * start falling after the corner — a held one, or one at a C0 brought to 0 by
 * an R0 written under it — and that nothing grades. No line of any record moves
 * on the corner, and no graded one on the reentrancy; among the ungraded ones,
 * Shaker's D (R) reads by the reentrancy the endless sync its own legend gives
 * these two, "3.4:#FFFF=DEADLOCK". The eighth below reaches the line of the
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
 * ch. 19.8.2 fixes at the write is fixed there, and the fourth
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
 * row, where a balanced parity alternates them. Two more of that interlace's
 * rules are here. The address takes the mode in the middle of the line that
 * asks, where ch. 19.8.1 gives a type 0 the next C0=0 — "this translation
 * between C9 and C9.VMA is immediately considered ... including during the
 * line, from position C0 where R8 is modified" (ch. 19.8.3) — the count and its
 * comparisons going on as before; Shakerland's photograph of Shaker's C (8) F
 * on a real type 2 draws it, and its C (6) draws the line's white segment four
 * microseconds wide as photographed, a microsecond right of where silicon puts
 * it, which is not yet understood. And a mode switched on during a frame's
 * first line under an odd frame makes that line "an additional line, and a new
 * line 0 will follow the old line 0, which will extend the size of the frame by
 * R0 µsec" (ch. 19.6.3, and ch. 19.5.4 in nearly the same words), C9 and C9.IVM
 * being "cleared on the 2nd line" (ch. 19.8.3). The first of ch. 19.8.3's
 * switching diagrams draws no such line; three passages of prose are taken
 * against it, and so is the disc. Ch. 19.5.4 gives a program that line to
 * "determine parity", and the routine Shaker runs to settle the parity before
 * its C (O), C (P), C (S) and C (8) reads the parity off that line and turns
 * it once where it is not the one wanted. C (P) grades the line, C (S) and
 * C (O), which speak only where the machine is wrong, are silent on it, C (8)
 * says it in a picture that stands still, and the scoreboard is the same
 * whichever parity the chip wakes on. Ch. 19.6.3's converse is here as
 * well, in the words ch. 11.9 gives it: where "the interlace mode is disabled
 * (R8=0) while the 'Interlace' line is displayed, then the 'Last Line'
 * condition is cancelled. The current line is no longer considered as an
 * interlace line. C9 then continues to count to R9, and C4 is increasing if
 * it is different from R4" — a type 2's alone, and ch. 19.6.3's "IVM mode"
 * read as ch. 11.9's interlace mode, which is what adds the line. So the
 * frame goes on, and a last line after it — or the line itself, made one by
 * the program — is met as any frame's is. On a first line given the mode, ch.
 * 19.6.3's converse leaves the count going on as it would have, and that it
 * reaches that line at all is our reading. Shaker's C (P) gives up the mode
 * on the line a frame's end adds and grades two frames on a type 2, and this
 * agrees with both: the one that moves R4 onto C4 there (#0020), and the one
 * that leaves C4 past R4 with R5 at 1 (#0B1C), because a type 2 pads no
 * row past R4 (below): C4 runs on round to 0, #0B1C standing to #0020 as
 * the 711 lines of that climb from the head of the line, the second of
 * row #27, stand to eight, R4 being #26 and R9 7. The fourth is the frame
 * parity itself. Types 1, 3 and 4 anticipate none of it: ParityFrame
 * "switch between each frame when C4 = C9 = C0 = 0" and does so "whatever the
 * value of R8" (ch. 19.5.3, 19.5.5), where a type 0 and a type 2 take the
 * parity R6 anticipated and hold it for ever once C4 can no longer reach R6
 * (ch. 19.5.2, 19.5.4) — so those three cannot be frozen, and cannot be made
 * to add the interlace line to every frame. That line follows each type's own
 * parity (ch. 19.6.1 to 19.6.4). A type 1 holds ParityC9 as a state rather
 * than a sum, and so do types 3 and 4 by a rule of their own — bare, on the
 * write that turns the video mode on, reversed on an odd R9 where a type 1
 * reverses on an even one, and leaving their own C9 where it stands (ch.
 * 19.5.5, 19.8.4) — because an R8 write sets it outright on all three, on
 * types 3 and 4 "immediately when R8 goes to 3" (ch. 19.5.5). Ch. 19.5.3
 * times a type 1's, "these updates are performed on the 3rd and 4th µseconds
 * of the OUT(C),C instruction", and gives it a use: toggling the mode "on and
 * off on an even C9 line, regardless of the value of R9" sets the parity
 * even, and "it is thus possible to fix the parity quite easily on this
 * CRTC". On that type the
 * write settles C9's low bit with it, that bit being ParityC9 and not the
 * count's own while the mode stands ("C9 = ParityC9", ch. 19.8.2), and the
 * chapter names the cost: "deactivate the IVM mode can also modify C9, and
 * modify the end condition of character C4". So a pulse landing on an odd C9
 * while the frame's parity is even puts the counter back to the line before,
 * and the frame runs a line longer than its neighbour. The disc measures the
 * divergence: Shaker's C (S), C (O) and C (3), which speak only where the
 * machine is wrong, are silent on the type 1 record by it, and on the records
 * of the type 2 and the two ASICs C (S) and C (O) are silent by rules of those
 * types' own — the type 2 the
 * first line its mode can lengthen a frame by, above, and the two ASICs the
 * order in which they read an R7 of 0, in the second. The write is
 * skipped while the doubling stands, because the C9 those rules move is ch.
 * 19.8.2's — the one counter a type 1 keeps, which is the address — and this
 * chip keeps a count and a parity instead, ch. 19.8.1's arrangement for a
 * type 0. The two are handed back to each other wherever a mode is taken up
 * or given up inside a row, which is what C (3)'s thirteenth test, M,
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
 * row: a type 0 one past R4, types 3 and 4 on R4 itself, "C4 does not increment
 * and is equal to R4" (ch. 11.1), ch. 11.2.1's third column, which Shaker's E
 * (1) grades on both. No line the disc grades moves on the C5 counter itself:
 * what stands behind that is ch. 11.2.1's and 11.2.3's tables and a test of
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
 * disc's C (E) grades that in all three of its readings. A line too
 * narrow to reach the disarm gives types 1 and 2 no additional line where it
 * gives type 0 one, ch. 13.2's window being a type 0's (the eleventh
 * exception). Types 3 and 4 do decide the frame's end, and the
 * padding after it, where the line ends, from R4, R9 and R5 as they
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
 * values without causing any problems for other counters" (ch. 13.5), and a
 * line of one character is a line to them as to types 1 and 2 (the eleventh
 * exception); and R5 admitting a row whose C4 has passed R4 is a type 0's
 * rule (ch. 12.2), carried over. Shaker's E (4) reads the frame's end taken
 * where the line ends twelve times on each 2.7 record, printing a reading only
 * where the machine differs from silicon, and prints none here by the
 * microsecond the board gives these two an
 * OUT (cpc.c, ch. 4.4.4), the later of its two writes otherwise arriving on the
 * line's last character, where it overflows, rather than on the next line's
 * first, where silicon has it too late. The exception ch. 12.5 gives a type 3
 * alone is not here: an R4 of 0 written on C0=0 is taken before C4 goes to 0
 * "if the I/O on the CRTC is performed at the same time as a ROM selection",
 * which "is active during the I/O if bit 5 of the Z80A B register is 0", and
 * nothing here tells the chip what else an access selects. The disc names both
 * ASICs a type 4, and asks for the exception of neither. A type 1 decides its
 * frame's end and the padding after it where the line ends as well, having none
 * of a type 0's exceptions — "everything is pure logic" (ch. 10.3.2.2): R4
 * written with the value C4 holds on a row's last line ends the frame wherever
 * on the line it lands, "if we were on the last line (C9=R9), then C9 goes to
 * 0, C4=0" (ch. 12.3), and an R4 or R9 moved off a last line while R5 asks for
 * padding "is taken into account immediately to postpone the end of the frame"
 * (ch. 11.2.4). Only a write made on the line's last character comes too late
 * for one thing, the padding the last line stood on there, which "remains true"
 * with the lines counted against the registers written (ch. 11.2.4). Shaker's E
 * (1) grades the postponing and that exception, and its A (U), photographed on
 * a real type 1, reads R4 written onto C4 and moved off it on a last line's
 * last four characters as taken.
 * An R5 raised from 0 at C0=R0 also sets off a bug of
 * that chip's own, the RFD (the seventh below). Every other behaviour below
 * is type 0's whatever the type is set to, and a number naming none of the five
 * is neither refused nor corrected: it builds a chip that is none of them, part
 * one type's and part another's. A type 2 asks R5 of its last line to the
 * line's end (crtc.c), and settles that line by ch. 12.4.1's rules rather than
 * a type 0's: at the line's head, with R4 and R9 as the writes made before it
 * left them, refused where the last character of the line before's HSYNC found
 * the comparison true, and to a line an HSYNC begins on or runs over at its
 * head (ch. 15.4.4, 15.6), which ends no frame under Shaker's B (P), whose
 * search sets R2 at 0: photographed on a real type 2, it gives up with "UNABLE
 * TO FIX", as here; and by an R4 or R9 write made outside the
 * HSYNC, while "Last Line Management" stands: the last character of an
 * HSYNC takes it away where it finds the comparison true and gives it back
 * where it finds it false, and a frame's first line waits for the second.
 * The taking away is ours, from a later paragraph of ch. 12.4.1, which assumes
 * it, and from Shaker's C (RETURN), photographed on a real type 2 making
 * no last line of an R9 written after such a sync (crtc.c). A write
 * is judged there on the character it is made on rather than the one after,
 * where this chip reads a written register, as two photographs of Shaker 2.6 on
 * a real type 2 have it (crtc.c); an R4 written during the head's own character
 * has the head compare again with R9 as it found it, and that either register
 * written there can still make the line last is ours. Shaker's E (7) names
 * silicon's value beside any of its timings this chip gets wrong, and names
 * none; its readings of its own carry no value of silicon's. One rule of that
 * chapter is not here: R4 and R9 moved before or during the R5 lines are the
 * values compared through them, so an R9 moved there leaves C9 counting on
 * after them, where here the frame begins again when they are spent, C4 and C9
 * at 0, whatever the two hold. Where the chapter puts the HSYNC's last
 * character "within the limit of R0", it is read as falling on the next line
 * where the sync runs over, which nothing grades; and the reading there is
 * taken before a write made during that character, which is ours, the sync
 * refusing such a write besides as silicon does (crtc.c), so it reaches
 * neither. A head an HSYNC begins on or runs over lifts no border on a type 2
 * either: "during HSYNC, this C0=0 test is not performed" (ch. 15.5.2), and a
 * line keeps the border until "a next C0=0" (ch. 15.6). Whether C0=R1 is
 * likewise skipped the chapter leaves open, and here it borders as on every
 * line. The sixth is the border R6 asks for. Where R6 is 0 a frame's first
 * line is a conflict on types 0 and 2 and comes out an alternation of bordered
 * and displayed bytes, cancellable until C0 meets R1 and definitive after it
 * (ch. 18.3.2); a type 1 borders outright on an R6 of 0 "without the condition
 * C4=R6 being required" and gives the border up with the register, except where
 * the write was made while C4 stood at 0, which keeps it for the frame (ch.
 * 18.2.3, 18.3.3); and types 3 and 4 have no conflict and test R6 where a line
 * begins rather than through it (ch. 18.2.4, 18.3.4). No line the disc grades
 * moves on any of the three — what grades them is those chapters and tests of
 * our own, and Shaker says its piece there in pictures. The seventh is where
 * types 1 and 2 read their offset. A type 1's R12/R13 reach VMA itself, and
 * reach it at the head of every line the frame's first character row spends
 * with C4 at 0, where types 0, 3 and 4 load both pointers once and only where
 * C4, C9 and C0 stand together at 0 (ch. 17.4.1, 17.4.2, 20.3.1, 20.3.2,
 * 20.3.4). C4 at 0 is a type 1's plainest case and not its rule. Ch. 11.2.4
 * keeps the same update through the C4 of 1 a run of additional lines gives it,
 * where the run opened with C4 at 0 — "if C4=0 before the additional
 * management" — and that is here, along with the exception it ends on: an R4
 * moved at C0=R0 above 0 takes the carry away. Ch. 11.6 keeps the update past
 * C4 altogether after the chip's RFD, "Rupture for Dummies", which an R5
 * raised from 0 where C0 names R0 sets off, or an R0 widened on that character
 * of a frame's last line whose last-line condition an R4 or R9 write then
 * cancels (ch. 13.7.1.2): from the line's end VMA takes R12/R13 at every C0=0
 * until the C9=R9 test made where C0 meets R1 passes, and that test takes in
 * the frame's parity, so that on one frame of two it never passes and the row
 * repeats (ch. 11.6.1), save after an R5 of #10 on a CRTC 1-B (ch. 11.6.2).
 * That the failing frame is the one the RFD found even, whatever the parity
 * does after, is ours. Shakerland photographs Shaker's D (9) on a type 1
 * drawing what this does on all nine of its pages, its B (COPY) on four of
 * five, and its B (O) answering 1-B, as one of the two photographs of that
 * group does. The disc grades none of a type 1's part in words and says it in
 * pictures: its E (2), "CRTC 1 VMA TRT C4=R4=0 ON ADJ LINE C4=1 ON NO-EXTENT
 * FRAME", settles on "YOU'VE WON THIS STAGE" with the carry and on "IF YOU CAN
 * READ THIS...YOUR EMULATOR HAS A PROBLEM" without it, and nothing else on the
 * disc moves either way.
 *
 * A type 2 takes no load for a frame's head being one. VMA' takes R12/R13 where
 * C0 meets R1 on a line its last-line state names, and VMA takes VMA' at the
 * head of every line (ch. 17.4.3) — and at a C0 come round to 0 by overflowing,
 * which is ours; ch. 20.3.3 opens on a type 0's rule, "when counters C4, C9 and
 * C0 change to 0, pointers (VMA' & VMS) are initialized with R12/R13", and goes
 * on to give a type 2 its own, which is the one taken. So an offset written
 * once the R1 character has passed on the last line is too late for the next
 * frame (ch. 13.4.1) — one written during that character is still in time, as
 * ch. 20.3.3's figure draws it — a line made last only by a write on the R1
 * character or later hands the next frame whatever the latch then holds (ch.
 * 12.4.2), and where R1 stands beyond R0 no line takes the offset at all. With
 * R1 at 0 the comparison is made at a head, before the head's own last-line
 * evaluation and before VMA takes VMA', so the line after the last line is
 * where the offset is taken, and the load made at its head performs the AND of
 * its two operations alone (ch. 17.4.3). At any other head VMA' takes VMA
 * as the head hands it to the line, the line's own start, rather than
 * where the line before ran to: the chapter has VMA' hold "the value that
 * VMA had on position C0=0 of the last line of the previous frame", so
 * that "the first line of the new frame is identical to the last line of
 * the previous frame", and Shaker's A (O) G and C (T) C and D
 * photographed on a real type 2 draw what that does; the reading is ours,
 * the order the chapter gives the head's two assignments handing on where
 * the line before ran to instead. While R1 stands at 0 the latch is
 * therefore only ever cut by the AND, and a frame after one R1 held at 0
 * throughout begins from what the AND has left, which is ours; such a
 * screen is border meanwhile. The chapter tells the AND of an offset
 * written after the last line's head, and ch. 20.3.3 has the head take R12/R13
 * there whole; that every load made at a head is cut short is ours, and so is
 * such a load being closed to an offset written during its own character. Where
 * padding follows the last line, the padding's last line is the frame's last —
 * "the last line of a frame ... can be one of the adjustment lines displayed
 * via R5", and on this type the interlace line holds the "Last Line" condition
 * (ch. 11.9) — and takes the offset in the last row's stead, that row and the
 * padding's handing VMA on as any row does (ch. 11.2.1, 11.2.3): Shaker's B
 * (RETURN) photographed on a real type 2 draws its frame from an R12 written
 * too late for the last line itself, after twenty-four lines of padding, and
 * its A (P) J draws the last row taking nothing. That R5 is read as it stands
 * where C0 meets R1, an interlace line asked for with R5 at 0 leaving the last
 * row its load, is ours, and so is the load reading the padding's last line's
 * state where its C9 has not reached R9, which ch. 12.1's "(when C9=R9)" leaves
 * open and the photograph cannot show; so is a padding made to end only by
 * a write on the R1 character or later handing the next frame whatever the
 * latch then holds, and reading the line ch. 19.6.3 turns into an additional
 * line as the interlace line, which takes the offset save where a padding
 * already holds it, the mode given up on the R1 character or later leaving that
 * offset to the line after it. No graded line moves on any of it; Shaker's A
 * (O), "R1 STORIES", moves R1 past R0 on row 24's last line and repeats on
 * every row the label it keeps at the latch's address there,
 * "CRTC-VMA'=#C000+(80x24)", where a type 0 repeats the screen's first row, and
 * a real type 2 is photographed drawing the same.
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
 * OUT's write in the microsecond ch. 4.4.4 names, holding it to the next
 * character clock by its own choice (cpc.c), where it lands after that clock's
 * tick and past the window, a microsecond after a Gate Array's OUT: ch. 13.6.3
 * has the last OUT in time on a line of width #3F start at #3C where ch.
 * 13.6.1 has a type 0's start at #3D, and C0 leave a width of nothing on the
 * OUT's fifth microsecond rather than its fourth, as ch. 13.5 warns, "the I/O
 * takes place with 1 μs delay, which implies placing the OUT 1 μsec before
 * those that would be done on a CRTC 1"; Shaker's B (P) starts its OUTs a
 * microsecond early on these two to make up for it. The OUTI reaches the
 * window as behind a Gate Array, so on these two its last placement in time
 * stands level with the OUT's, where the chapter draws it a microsecond
 * sooner. A width an OUT lowers onto or below the character C0 stands on is
 * handed to the chip ahead of that clock's comparison instead: Shaker's B (R)
 * writes R0=1 on C0=1 on every type, its build for these two a microsecond
 * shorter to make up for their later interrupt, and silicon ends that line
 * there on both, "R0=1/R3=1/R52 CALC=#34 (Exp:#34)(REF #0169)" photographed,
 * which a write landing after the tick would end a character late. That a
 * lowering write is taken a microsecond sooner than a raising one is ours,
 * from those two groups. An R2 or R3 an OUT writes is handed over ahead of the
 * tick too, the line sync's comparisons taking it on the character it lands on:
 * ch. 14.9's CRTC 4 schematic draws "R2-JIT / OUT(C),r8" with the sync starting
 * on C0=R2, ch. 14.7.2 has an "R2.JIT ... does not cause the HSYNC black zone
 * to appear later ..." on these two, and ch. 14.5.3's "OUT CRTC-R3,5", landing
 * on the character C3l reaches 5 on, ends a sync five characters long;
 * Shakerland photographs Shaker's A (2), A (3), A (R), A (Y), B (4) and B (I)
 * on both drawing their syncs so. An R3 handed over so brings its VSYNC width
 * with it, which a write held to a line's end during a VSYNC then has counted a
 * line sooner; nothing grades that. An OUTI's write lands after the tick still,
 * where B (I) is photographed drawing its OUTI pages as its OUT pages: not yet.
 * The window itself only takes an ending back, so an OUTI landing on the clock
 * with a width that names the character just left does not end that line there
 * on any type, and nothing here grades it. These two have ch. 13.6.3's "The C0
 * counter never exceeds the value of R0 when R0 is updated according to the
 * timing described in the previous diagrams", diagrams that only widen R0; that
 * a width written under C0 ends the line on the next character, where the other
 * three run the counter the long way round, is ours, and so is the window
 * taking no ending back for a width written under the character the ending was
 * taken on, the line staying ended. The other readers of C0=R0 — status 1's bit
 * 0 and status 2's, the interlace question ch. 11.9 puts at C0=R0, an R4 moved
 * at a line's end — keep the equality, so a counter left past a lowered width
 * answers them as a line not at its end, and nothing grades it. Shaker's D (U),
 * photographed on both, reads status 1 under widths of 1 and 2 written so as
 * #FC or #F8, a counter come home, and Shaker's A (5) and A (6) draw their 4
 * and 2 microsecond screens as photographed only with it; one of D (U)'s status
 * plots, status 1's bit 7 under the interlace video mode, is photographed with
 * eight of its dashes a line sooner than this draws them, by a fit of the
 * lens's distortion and at moderate confidence. The *Not yet* list below
 * carries what the shift leaves unsettled. A character was drawn in that second
 * microsecond and it counts: the counter goes on from what was drawn and not
 * from the ending, or the line comes out a microsecond too long. On a line of
 * one character that second microsecond is the next line's end, and a type 1's
 * write reaches past it to the end before, which is why the chip keeps two. Ch.
 * 13.6.2 draws the "Previous R0=0" line with a type 1's OUTI going on a
 * character further than ch. 13.6.1 draws a type 0's or 2's at both placements
 * they give, and B (6)'s "OUTI ON C0=0,R0=0" asks the same of the type 1
 * record.
 *
 * The same character-clock edge reaches another comparison, the C4/R7 equality
 * that starts a VSYNC, which types 1 and 2 read on the character a write lands
 * on where the rest read it on the one after. Ch. 16.4.2 times it for a type 1
 * — a PPI read answers such a write "at the earliest 5 μsec after" where
 * ch. 16.4.1.1 has a type 0 read "6 µsec later" — and ch. 16.4.3 gives a
 * type 2 only "triggered immediately", which Shaker's B (6) settles on that
 * character. Ch. 16.4 gives every type its last chance, "For all CRTCs, the
 * update of R7 with C4 can take place up to the last µsecond preceding
 * C4=R7"; that types 1 and 2 reach a microsecond further than the other three
 * is ours, from Shaker's B (6) "R7 LAST CHANCE 4TH uSec": C4 has already
 * walked onto the value being written, and the other three do not read that
 * equality until the character after, where the block of the second exception
 * above has already spent it. The board says which edge an access
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
 * character drawn since is the next line's head, decided as any head is. A
 * type 0's VSYNC authorization is not treated as the C9 management is, and
 * that is an asymmetry rather than a finding: it is raised at C0=2 by a step
 * of exactly the same kind (ch. 13.2.2), a character that never asks for it
 * costs the following line its VSYNC, and on a line of two characters — a
 * width a program building a screen out of invisible lines reaches for — that
 * loses a pulse the chip raises. Nothing we have read or measured says which
 * of the two the silicon does. The border R1 asks for is the second cost,
 * and the plainest of them. Where a line ends on the character before R1, the
 * character the counter goes on to is the one a wider line would have met R1
 * on — and that comparison is among the lost, so the rest of the line stands
 * displayed where a chip that had been that wide all along borders it.
 * Shaker's B (6) grades the window at both its widths and neither cost: its
 * "4TH uSec ON C0=0" grades the first character on the type 0, type 1 and
 * type 2 records, its "5TH uSec ON C0=0" the second on a type 1, and its
 * "R7 LAST CHANCE 4TH uSec" the other comparison on the type 1 and type 2
 * records.
 *
 * The ninth is the display delay R8 carries in its high nibble. Ch. 19.1's
 * table marks those bits x on types 1 and 2, and ch. 19.2 gives the
 * SKEW-DISPTMG functions to "CRTC's 0, 3 and 4" only, so those two keep R8's
 * interlace bits alone and a delay written to them does nothing. Shakerland's
 * photographs of Shaker's A (2) show it on both: the border an R8 of #10 bends
 * on the other three stands straight there. Nor do the two ASICs give a line R1
 * never reached a character of border under a delay, as a type 0 does, for
 * they send it none: "On CRTC's 1, 3 and 4 ... the CRTC does not send BORDER
 * ON signal to GATE ARRAY" (ch. 19.2.4), and Shakerland photographs Shaker's A
 * (2) on both with its skewed bars unbroken.
 *
 * The tenth is a line sync that ends on a character where C0 meets R2 again. A
 * type 0 refuses the second sync there unless R3 is written on that character
 * (below); "On the CRTC's 1, 2, 3 and 4, there is a bug if C0=R2 on C0=R2+R3"
 * (ch. 15.3.1), and there "the HSYNC does not end and C3 will overflow" (ch.
 * 15.3.2), counting on round to R3l as ch. 15.3.4 and 15.3.5 draw it for an R2
 * moved onto the sync's end, an R3l written during the run "considered
 * immediately to bypass the absence of a C3l zeroing" (ch. 15.3.4). So R0=0,
 * R2=0 and an R3l of 1 make a sync that never ends, and the Gate Array, which
 * counts its interrupts on the ends, counts none: "the re-entrance prevention
 * mechanism is bugged on CRTC's from 1 to 4, which allows the creation of an
 * infinite HSYNC" (ch. 27.1). Shaker's B (R) reads that on all four records,
 * "R0=0/R3=1/R52 CALC=#FF", and Shakerland photographs it on all four. Its D
 * (H) PG 2 is photographed on both ASICs drawing a run-on: on lines of two
 * characters with R2=0 a six-wide sync ends on C0=0 and runs on, the R3 of 1
 * written on that character arriving with C3l already past it, so the sync
 * counts round to it, seventeen characters in all — in the page's first form
 * the last of it black at the band's visible left edge, in its second, with two
 * syncs a line, the whole of it a block of black. The page is photographed on
 * the other three types too: on a type 0 the second form's block is six
 * characters, the refusal, and on a type 2 the first form's tail stands at the
 * band's edge as on the ASICs, a character further from the text, the ASICs
 * handing their syncs on a character late (cpc.c); on a type 1 the band is
 * drawn slanting, which this machine does not draw. Not here: the "invisible"
 * end ch. 15.3.4 gives a type 1 where the sync runs on, after which the Gate
 * Array restarts its own count and sends a second monitor sync, a pin reported
 * once a character having no instant inside one to fall and rise in (Shaker's B
 * (R) reads no interrupt counted there on silicon).
 *
 * The eleventh is a line too narrow for C0 to reach 1 or 2, which a type 0
 * spends on the rules ch. 13.2 gives it alone (below). The other four count
 * such a line as any other: "On CRTC's 1, 2, 3 and 4, this value (R0=0) is
 * not a problem. Neither blocking C9 nor additional line C4=1, but it is more
 * subtle for CRTC 2" (ch. 13.2.7), "if R0 is 0, then C9 and R4 continue to be
 * managed normally" (ch. 13.3, 13.5), and a type 2 "doesn't have any problem
 * for the counting of C9 and C4 when R0=0" (ch. 13.4.1). Nor do they need C0
 * to have reached 2 on the line before a VSYNC: ch. 16.4.1.2 stands under
 * "16.4.1 CRTC 0", and the chapters for the others, which have a type 1's
 * VSYNC start "when C4=R7" (ch. 16.4.2), a type 2's "considered on all values
 * of C0 and C9 when C4=R7" (ch. 16.4.3) and an ASIC's start "when C4=R7 and
 * C9=C0=0" (ch. 16.4.4), ask nothing of the line before. Shaker's B (P)
 * measures the counting: the R4 its search finds counts the rows a
 * 74-microsecond stretch of R0=0 leaves behind, and Shakerland photographs
 * #20 and #41 on a type 0, the standstill, #2B and #79 on a type 1 and #48 and
 * #7A on the ASICs, a line every microsecond, as here. On a type 2 the search
 * finds nothing whatever the counting, R2 at 0 starting a HSYNC on every C0=0
 * so that no last line is ever set (ch. 12.4.1), and it prints "UNABLE TO
 * FIX" on silicon as here. Shaker's A (7) and B (CAPS), drawn under R0=0,
 * come to their photographs on a type 1 by this and the twelfth, and B (CAPS)
 * on a type 2 by this alone, save its two end columns; Shaker's A (I) reads
 * the VSYNC a line of one character leaves a type 2 as photographed. What the
 * chapters leave open a type 1 answers by a reading of ours: its VSYNC's line
 * count holds on such a line while C9 and C4 go on, A (I) photographed still
 * in the pulse at every read after R0=0 is written on the line before C4
 * meets R7, where a type 2 spends the count a line a microsecond. On the ASICs
 * six of that page's sixteen reads differ from the photographs: at four,
 * silicon is in the pulse and this is not, and at two the other way round;
 * their count is spent as a type 2's here, a reading of ours the page leaves
 * unsettled. A line of one character that brings C4 and C9 home at every end
 * is a frame of one microsecond to types 1, 3 and 4, whose ParityFrame then
 * turns at every character (ch. 19.5.3, 19.5.5).
 *
 * The twelfth is the byte of border at C0=R0 on a line R1 never reached,
 * which ch. 17.6.2 gives to "CRTC 0, 2" and denies the rest: "CRTC 1, 3, 4 ...
 * On these CRTC's, no BORDER bytes are generated between the 'frames'". So
 * screens one, two or four microseconds wide run unbroken on those three, and
 * so does a display R1 never ends where it runs on into the next line.
 * Shakerland photographs no such byte in Shaker's A (2), A (5), A (6), A (7)
 * and B (CAPS) on a type 1 and its A (5), A (6), A (7) and A (O) on both ASICs,
 * and the alternation the chapter gives a type 2 under R0=0 in its B (CAPS).
 *
 * The thirteenth is R1 written during a character, which is compared with C0
 * there as well as where C0 next meets it — "the condition C0=R1 is considered
 * immediately on a line" (ch. 17.3) — each type reading the character's two
 * comparisons, the old R1's and the new one's, its own way. On types 0, 1 and
 * 2 an equality the write makes borders the rest of the line and the character
 * the write lands on with it, as ch. 17.5.1 draws an R1 of 0 written on C0=0
 * "just in time"; that character's pins having left the chip at its tick, a
 * host borders it (display_shut_since_the_tick), save under a delay, which
 * hands out the latch as it stood a character or two before (ch. 19.2.3). The
 * two ASICs take no equality from the write, ch. 17.5.2 drawing that same R1 of
 * 0 "too late". Where VMA' is left, on a row's last line, a type 1 reads the
 * new comparison alone — "if the R1 update takes place exactly when C0=R1
 * (R1.JIT), then the condition C0=R1 is no longer true and VMA' is not updated"
 * (ch. 17.4.2) — a type 2 the old one alone, "an update of R1 on position
 * C0=R1 arrives too late" (ch. 17.4.3), the two ASICs the same, which is ours,
 * that chapter being a type 2's and the photographs below the evidence, and a
 * type 0 both, which
 * the chapters do not say. Shaker's A (O), "R1 STORIES", writes all of these,
 * and Shakerland photographs it drawing what this does on every type: its pages
 * G and H, an R1 of 0 written on C0=#3E, #3F, 0 and 1; its I, an R1 of #3F
 * written on C0=#3F, which on a type 0 draws the rows the pointer left there
 * gives; and on a type 1 its B, R1 moved off C0 on that character. Its I on a
 * type 2 is the exception, a fine pattern repeating at the row's pitch on
 * silicon where this draws black, which is not yet understood.
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
 * made at C0=2 by a write that lands there, a row's end taken for C4 where the
 * line's last character found it or a write made there leaves it, and for C9 as
 * that write leaves it (ch. 10.3.1.2, 11.2.2), the vertical adjustment a type 0
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
 * with it (ch. 19.2). No line the suite grades covers them, and what stands
 * behind their placement is a reading of ch. 19.2's diagrams. Shakerland's
 * photographs of Shaker's A (2) show the delay on this type and the two ASICs
 * and none on types 1 and 2 (the ninth exception); set against the photograph
 * of its "R8 STORIES 1" page on this type, where R8=#10 is written from
 * C0=#37 to #3E a line at a time, the border the delay cuts falls here on the
 * lines written from #39 to #3C and on silicon on those from #3A to #3D, a
 * microsecond of writing later, which is not yet understood. The interlace
 * line is asked for on the deadline of its own that ch. 11.9 gives it, later
 * than a type 0's R5 deadline and read on the last line a frame has — which
 * may be one of R5's own, so a program may add the line or take it back from
 * inside an adjustment, and a frame R5 asked nothing of is held open for it.
 * Shaker's C (P) grades that reach into an adjustment on a type 2: were the
 * deadline read only on the last line of a frame's last row,
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
 * stands, unless a block holds that value already (ch. 16.4.1.1). A line of
 * one character never reaches C0=1, so "C9 processing management" is never
 * enabled again and C4, C9 and the VSYNC's own line count freeze where they
 * stand, which is why a pulse begun there never ends; the arming the last
 * managed line made still lands, and a C4 increment is all of it that can,
 * once — and where that increment falls on a last line it is ch. 13.2.6's
 * worked table beginning rather than a row ending, so the adjustment outlives
 * the widening and C9 is measured against R5 for the rest of the frame
 * (ch. 13.2.1, 13.2.4, 13.2.6, 16.4.1.2). Shaker's C (P) prints its graded
 * line on this only at some boot phases, with the same right answer whenever
 * it does, and is weak evidence either way. A type 0 standing
 * still so on a frame's first character enters nothing: ParityFrame does not
 * turn, R12/R13 are not loaded — "R12/R13 cannot be considered until C4 and C9
 * both go back to 0" (ch. 13.8.3) — and an R6 border thrown there stands for
 * the frame, which "BORDER R6 returns to false because for a new frame
 * (C4=C9=0)" (ch. 18.3.2) and no new frame has begun. Shaker's B (U) E to K,
 * photographed on a type 0, draw both; ch. 18.3.2's last note, "possible to
 * achieve a BORDER/CHARACTER alternation on CRTC 0 with R0=0", is read as that
 * head itself. On such a line the head opens the display before R0's stand-in
 * for R1 borders it, the border of C0=R0 coming "0.5 µsec after the condition
 * C0=R0 is satisfied" (ch. 17.6.2, 19.2.4), so a line delayed by SKEW-DISPTMG
 * is border, as B (U) F to K are photographed. That order is ours, and it
 * reaches a type 0 alone, the one type with both that border and a delay. An
 * R0 of 1 widened on the character C0 names 1 is compared with the old value
 * and counts C0 on with the new: where C9=R9 "this causes an overflow of C4
 * without C0 and C9 being reset to 0, even if C4 is not equal to R4", C4
 * moving from C0=2, and on a frame's last line C9 then runs on to R5 (ch.
 * 13.7.2), which Shaker's B (7) is built on and its A (4) reads, both
 * photographed on a type 0. The HSYNC's width is counted per character
 * rather than per line and goes on counting. R4 and R9 written under a
 * frozen chip are read here, where ch. 13.2.1 says they are no longer
 * considered and ch. 13.2.4 then wants them for the last line it
 * assesses at C0=0; the chapter is in two minds and this is our reading. The
 * adjustment is armed as the chip arms it: ch. 12.1 gives the window, "this
 * management of additional line(s) is managed when C0<2", ch. 13.2.5 has the
 * chip "assess whether it is on the last line, and if so, arm an internal flag
 * by default" there, and leaves C0=2 to "assess the conditions for disarming ...
 * in particular by testing the value of R5". That assessment takes an arm back
 * on R5 alone, so a last line unmade at C0=0 unmakes the arming with it (ch.
 * 12.2) — the interlace line is left out, its question being put "on the last
 * line of a frame" (ch. 11.9) and this one no longer being one, which is our
 * reading and ungraded; an R5 above 0 still admits a line whose C4 has gone
 * past R4, which the assessment cannot see, on every type but a type 2 — ch.
 * 12.2's note gives it to a type 0, "if C4 exceeds R4 on at least one of
 * these events, it will return to 0 once the additional line handling is
 * complete", nothing gives it to types 1, 3 and 4, and Shaker's C (P) says a
 * type 2 does not have it (above); and an adjustment already begun is past
 * both (ch. 13.2.6), which keeps ch. 10.3.1.2's exception alive while the
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
 * sync out of the endless HSYNC of the tenth exception (ch. 15.3.1, 15.3.2).
 * A write to R3 there is the exception, taken, by a reading of ours, as the
 * modification whatever value it carries, ch. 15.3 not saying whether one
 * carrying the value R3l holds counts, and the sync it lets through carries the
 * old count on rather than starting from nothing, which ch. 15.3.3 calls the
 * start of an R2.JIT HSYNC. That chapter's own example moves R2 as well and comes
 * out right without the exception being reached at all, the new R3l carrying
 * the old sync past its ending comparison on its own. R3l updated during a
 * HSYNC is here as C3l counts it: a value C3l has yet to reach ends the sync
 * where C3l meets it, and one it has reached or passed overflows it and runs
 * the sync round to it (ch. 14.5, and the diagrams of ch. 14.5.1 to 14.5.3).
 * R3.JIT is not: ch. 14.5.4 has the value C3l stands at, written by an
 * OUT on its own character, stop the sync on types 0, 1 and 2, where
 * here it overflows. On the two ASICs, on which "this technique does not
 * work" (ch. 14.5.4), the R3 an OUT writes is compared from the
 * character it lands on (the eighth above), so a value C3l reaches there
 * ends the sync and one it has already passed overflows it, as ch.
 * 14.5.3 draws both. Not yet: the gap the R2.JIT leaves, the chip
 * beginning the second sync "around 3.5 Pixel-M2 after the one that has just
 * ended" where a pin reported once per character can only stay high, so the
 * Gate Array is given one sync here where the chip gives it two; a write that
 * changes R3l on that character, seen here by the comparison that ends the sync
 * as well, where the chip's is not; an R2 written onto the character C0 stands
 * on, whose sync the chapters raise on that character and this chip puts off
 * for the line after: by an OUT, in its third microsecond on types 0 to 2, "the
 * CRTC sends the HSYNC signal later to the GATE ARRAY" (ch. 14.7.1), one an
 * OUTI writes "behaves as if R2 had been programmed before that C0=R2" on types
 * 0 to 2 (ch. 14.7.1), and one an OUTI writes on the ASICs, whose sync ch.
 * 14.9's type 4 schematic draws with an OUT's ("R2-JIT / OUTI"), an OUT's
 * being here (the eighth above); whatever would place this chip's line endings
 * where ch. 13.6 draws them — ch. 13.6.1's last OUTI row, ch. 13.6.3's third
 * and ch. 13.6.2's fifth all draw wrapping a placement this chip lets run on,
 * and Shaker's B (6) says it runs on, so the disc is followed and every
 * placement here sits one character late against all three chronograms, a shift
 * the window above compensates and nothing yet explains, and the reason the two
 * microseconds ch. 13.3 sets "in principle" between an OUT(C),R8 and an OUTI
 * stand at one here behind a Gate Array, the OUT never being rescued there
 * where the OUTI is; what the chronograms are about, a type 1's OUTI one
 * placement beyond the rest, survives it; the first of ch. 16.3's two
 * protections against one C4/R7 equality raising two pulses, "it is not
 * possible to trigger or inhibit a VSYNC during a VSYNC. Thus, modifying the
 * value of R7 with a value of C4 reached during the VSYNC does not cause a new
 * VSYNC", where an R7 written here during a pulse makes its equality as at any
 * other time, so one made, past C0vs=1 on a type 0 and anywhere on a type 1 or
 * 2, on C4's value once the block has been lifted during the pulse — by C4
 * moving on, or by a line C9 begins at 0 finding R7 away from C4 — runs a
 * second pulse straight on from the first if C4 still stands there when it
 * ends, and the second protection — the equality having to change — is kept on
 * those three alone; and the rest of what ch. 13.2.1 gives a line's first three
 * microseconds — the counter updates those characters schedule for a later one.
 * R1's equality is taken where it is written as well as where it stands (ch.
 * 17.3), each type reading the two its own way (crtc.c); every other comparison
 * is made where it stands. Two of the border's rules move DISPLAY ENABLE inside
 * a character — the byte of border at C0=R0 on a line where R1 was never
 * reached and no skew stands ready to defer it to a whole character of its own
 * (ch. 17.6.2, 19.2.4), and the byte-by-byte alternation an R6 of 0 makes on a
 * frame's first line (ch. 18.3.2) — and both are here, on a type 0 and a type 2
 * alone (the twelfth exception for the first, the sixth for the second), which
 * is why a tick reports that pin for each of the two bytes a character is drawn
 * from rather than once.
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
 * rules a type 0 and a type 2 keep move it half a character — one of them
 * only while no skew stands ready to defer that border to a whole character
 * of its own — and the machine fetches two bytes for every character the chip
 * names, so what a tick owes the machine is the pin as each byte finds it
 * (ch. 17.6.2, 18.3.2, 19.2.4). The datasheet's name is kept for the first,
 * which is where the pin stands when the character begins; the second wears a
 * name of its own because the datasheet has none for it. */
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
     the one part, and this chip answers as a 1-B in its RFD (crtc.c). */
  uint8_t type;

  /* R0-R17, selected through the address register. Writes are masked to the
     documented type-0 widths (Compendium ch. 4.3), save that types 1 and 2
     keep only R8's two interlace bits (ch. 4.3, 19.1); R16/R17 ignore
     writes. */
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
     only the paths a type 2 takes maintain it, which the others share in
     part: a type 1 counts its frames' additional lines on C5 as a type 2
     does and carries the field on through them, zeroing it where a run it
     holds open brings C4 round to R4, and types 0, 3 and 4, which count those
     lines on C9, leave it standing there. */
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
     made at C0=1 lands (ch. 10.3.1.2, 12.2). A type 1 and the two ASICs
     decide it again where the line ends, and that is the answer the frame
     ends on (ch. 10.3.2.2, 12.3, 12.5). A type 2 decides it at the head and
     wherever R4 or R9 is written, by rules of its own (ch. 12.4.1). */
  bool last_line;
  /* The two states a type 2 keeps beside it (ch. 12.4.1). "Previous Last
     Line" is the comparison the last character of a line's HSYNC found, and
     it refuses the next line's head; "Last Line Management" lets an R4 or R9
     write make a last line where the head did not. The same character takes
     it away where it finds the comparison true and gives it back where it
     finds it false, and a frame's first line waits for the second. */
  bool previous_last_line;
  bool last_line_management;
  /* And the state the line before ended on, which a type 2 reads where C0
     meets an R1 of 0 at a line's head, before the head's own evaluation
     (ch. 17.4.3). */
  bool line_before_was_last;
  /* And whether the line before was a padding's last, the frame beginning
     after it, which a type 2 reads there likewise; the line ch. 19.6.3
     begins a frame again after counts as one. */
  bool line_before_ended_its_padding;
  /* Whether a type 2 took R12/R13 into VMA' where C0 met R1 on the
     character just drawn, a line's head being no such place, which an R12
     or R13 written during that same character still reaches (ch.
     20.3.3). */
  bool offset_taken_where_c0_met_r1;
  /* R9 as a type 2's line head found it, which an R4 written during the
     head's own character has the head compare C9 with again: "the
     comparison uses the updated value of R4, but the previous value of R9"
     (ch. 12.4.1). */
  uint8_t r9_at_the_line_head;
  /* And whether that head was refused, the line before having been last or
     an HSYNC standing on it, which the comparison made again keeps. */
  bool line_head_refused;

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
  /* A type 1's RFD, "Rupture for Dummies" (ch. 11.6): an R5 raised from 0
     where C0 names R0, or R0 widened on that character of a frame's last
     line whose last-line condition an R4 or R9 write then cancels (ch.
     13.7.1.2), sets two states at the line's end. VMA then takes R12/R13 at
     every C0=0 until the C9=R9 test made where C0 meets R1 passes; and that
     test takes in the frame's parity as the RFD found it, so that after an
     RFD on an even frame it never passes again until the frame ends, and the
     row it stands on repeats. */
  bool r0_widened_on_the_last_line;
  bool rfd_at_the_line_end;
  bool rfd_takes_the_parity;
  bool vma_takes_the_offset;
  bool row_end_test_is_faulty;
  /* The last R4 write of this frame, and whether it was the one the
     exception names: above 0 — the seven bits R4 keeps, so a written &80 is
     not — and made where C0 stood on R0. Written afresh by every R4 write,
     so a later one anywhere takes an earlier one's answer away. */
  bool r4_moved_at_a_lines_end;
  /* An R4 or R9 write made while C0 names R0 lands as the line ends, and
     what the line's last character compared before it is kept for the
     comparisons that write cannot take back: on a type 0 the two that move
     C4, "when R9 is modified exactly at the position C0 equals R0, while C9
     was equal to R9 before C0 reached R0. In this case, C4 is still
     incremented" (ch. 10.3.1.2); on a type 1 the last line an adjustment
     stands on, "if C4==R4 and C9==R9 in position C0==R0, and R4 or R9 is
     modified in this position, then the additional management remains true"
     (ch. 11.2.4). Taken by the first such write of the character and cleared
     after the tick that reads it. */
  bool r4_or_r9_written_on_the_last_character;
  bool row_ended_before_that_write;
  bool c4_stood_on_r4_before_that_write;

  /* Whether the chip stood on a frame's first character last time it was
     asked. ParityFrame turns, R12/R13 are loaded and the R6 border is lifted as
     that character is entered (ch. 19.5.2, 20.3.1, 18.3.2), and a type 0
     frozen on it enters nothing: without this the parity would turn under a
     still picture every microsecond. The other four enter a frame's first
     character at every end of a line of one character that brings C4 and C9
     home. */
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
     them as well. Types 0 and 2 are answered by parity_c9(): a type 0 from
     R9, C4 and ParityFrame, a type 2 from ParityFrame alone, that chip
     respecting its parity "whatever the values of R9 and C4" (ch. 19.5.4).
     No line on any record reaches the two ASICs' share of
     this: what stands behind it is ch. 19.8.4's algorithm, the two frames it
     works by hand, two of the twenty-two counting cases it draws beside
     them, and tests of ours. A row of theirs whose counter is sent past its
     limit comes home rather than walking to 31, which is ch. 10.3.4.1's
     "more complex" comparison and ch. 19.8.4's "C9 >= R9", graded by the
     disc's A (U), and C (O) silent on it. The line an interlaced frame adds
     holds their C4 on R4, as ch. 19.6.4 has it, but takes its C9 and parity here as any
     additional line would, where that chapter says "the additional line
     generated does not consider parity states as on other CRTC's. C9 will
     always be 0, even if the other lines are odd on C4=R4"; no record
     grades that. */
  bool parity_c9_held;
  /* What R8 answered at C0=R0, which is where ch. 11.9 asks it and a
     microsecond before the line it decides could begin. */
  bool interlace_line_owed;
  /* The one line interlace adds after the R5 lines, and one to a frame
     however the adjustment carrying it ends (ch. 11.9, 19.6.1) — save on a
     type 2 that gives interlace up on it, which makes it "no longer
     considered as an interlace line" (ch. 11.9). */
  bool interlace_line_given;
  /* The interlace video mode as the counters see it, which is not quite as
     R8 holds it: the mode a write asks for is taken up at the next C0=0,
     after that line's own R9 test (ch. 19.8.1). A type 2's address takes it
     from R8 at once (ch. 19.8.3). */
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
     the second the one before it. A host that copies the chip together with
     its rooms points the copy at its own by this field, which keeps what they
     keep; crtc_give_line_end_rooms is for rooms that hold nothing of the
     chip's. */
  struct crtc_t *line_end_rooms;
  bool a_line_end_is_kept;
  bool the_line_end_before_is_kept;
  /* And whether a character has been drawn since it ended, which a type 1
     lets happen: its window is two characters wide, and the counter goes on
     from what was drawn rather than from the ending (ch. 13.3, note 3). */
  bool a_character_was_drawn_since;
  /* One C4/R7 equality raises one VSYNC: the comparison must change before
     it raises another, by C4 moving or by a line C9 begins at 0 finding R7
     away from C4, so an R7 moved off C4 and back between two lines C9 begins
     at 0 changes nothing (ch. 16.3, 16.4.1.1). The two ASICs lift the block
     at every line C9 begins at 0, changed or not (ch. 16.4.4), and at every
     R7 write that does not set it, a reading of ours (crtc.c). */
  bool vsync_blocked;
  /* The state ch. 13.2.2 has a type 0 raise at C0=2 so that the next C0=0
     may read C4 against R7; the other four read it without one (crtc.c). No
     line has run at power-on and nothing has raised it, so this chip wakes
     holding it, which is the state a chip that has been running is in. */
  bool vsync_armed;
  /* "C9 processing management", which ch. 13.2.4 has the chip disable at
     C0=0 and enable again at C0=1, so that a line of one character leaves
     it disabled and a type 0's C4, C9 and VSYNC line count frozen where they
     stand, a type 1's VSYNC line count alone, and nothing on the other three
     (crtc.c). It wakes standing for the same reason the one above does. */
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
     turns over at a line's head with C9 at 0, on the tick the border is
     thrown or lifted there, and nowhere else: an R6 written onto C4 inside a
     line throws the border and leaves the bit. Ch. 21.3.3's sentence has the
     bit "updated when C0=R0", a character sooner; its two diagrams have the
     change seen first by the read that lands on C0=0, and those are
     followed. The two are kept apart so that reading the port cannot move
     the picture. */
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
  /* Whether VMA' captured VMA where C0 met R1 on the character just drawn,
     and what it and the taking of R12/R13 an RFD leaves held before, which a
     type 1 goes back to where R1 is written away from C0 during that same
     character (ch. 17.4.2). */
  bool pointer_left_on_this_character;
  uint16_t latch_before_the_pointer_was_left;
  bool offset_taken_before_the_pointer_was_left;
  /* Whether a write since the tick borders this character, whose pins have
     already left the chip: on types 0, 1 and 2 an R1 written onto C0 borders
     the character it lands on (ch. 17.5.1). A host reads it after each write
     it makes. */
  bool display_shut_since_the_tick;

  bool hsync;
  /* Whether the character being drawn is the one an HSYNC ended on: "two
     HSYNC's cannot be contiguous if position C0=R2 is encountered when C3l
     reaches R3l, and R3l has not been modified on this position" on a type 0,
     where on the other four "there is a bug if C0=R2 on C0=R2+R3" and the
     sync runs on (ch. 15.3.1). */
  bool hsync_ended_here;
  /* Whether R3 was written in time to be read on that character, which is
     the modification that same sentence exempts on a type 0. */
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
