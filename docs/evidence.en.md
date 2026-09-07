---
title: The evidence
description: The tiers of tests behind the machine, what each one proves, and the things none of them can see.
order: 2
---

An emulator that looks right and an emulator that is right are different things, and the difference surfaces years later, in the one game nobody tried. So every claim here has a check behind it, and wherever possible the check comes from outside — written by someone who did not know what we believe.

That last part is the whole of it. A test written from our own understanding agrees with our own mistakes. The suites that matter are the ones that can disagree.

The tests come in tiers, separated by what they answer and what they cost. The fast tier is hermetic and runs on every change; the others fetch what they need on first use.

## The fast tier

It covers only what no external suite can see: the reset contract, the invariants of our own machinery, and the proof that every opcode on every prefix page finishes without outgrowing its micro-program.

Since the machine began it also grades the parts against their documentation.

The **memory map** is proved against the board's: the eight banking configurations, the ROM paging and the I/O decode, each exercised through the bus by a program running from a fabricated ROM rather than by reaching into the machine's fields.

The **6845** is held against the Compendium's frame — 312 scanlines of 64 microseconds, the syncs where the registers put them, the video pointer walking the documented rows, the last line decided while the character counter is still 0 or 1, the counters running to their own tops when a program writes a limit beneath them, the vertical adjustment that brings a runaway row counter home, the display and the border as the latches the equalities throw rather than comparisons standing, and the sync width of zero that leaves this type with no line sync at all. The **monitor** is timed from the middle of that sync, which is what turns a trimmed pulse into half a character of scroll.

The **Gate Array** is held against chapter 27 and its own documentation: the interrupt counter looping at 52, bit 5 dying at the acknowledge, the two-line rule after a frame sync, and a byte becoming pixels in each of the four modes. Grimware states that last rule inverted; the Compendium is the one tested on silicon, and it is the one we follow.

The **video path** is proved end to end — a screen of pixels through the whole machine, landing 640 by 200 exactly where the syncs put it.

And the **duration of seventy-odd instructions** in microseconds, against two tables of measurements made independently of each other and of us.

The **tape reader** is held to the same tables: the pilot's 8063 pulses before a header and 3223 before data, the two syncs, the two bit lengths, and the second of silence between blocks — each transcribed from the TZX specification rather than worked out from the code that plays them. The **deck** is graded apart from it, on pulses written out by hand, so what is proved there is the timing and nothing else. And the CPC's own wiring is graded: the motor line turning the reel and stopping it, and a pulse arriving at bit 7 of port B held for the longer count a 4MHz board owes it.

The **ULA's charge for the bus** is held against the tables published for it: the delay owed at each T-state of a Spectrum's frame, and the length of instructions whose operands lie in the screen's memory — which fixes where each charge falls inside an instruction, including the internal T-states these ULAs charge for and the four rows by which a port is charged instead. That is not a handful of positions but a walk: twenty-seven shapes of instruction — one for every place a cycle can fall, each of the four rows an I/O access is charged by, a conditional taken and not taken, and the internal T-states that hold the interrupt register rather than an address the instruction chose — each begun at every T-state of the first line to carry picture, a line in the middle, the last, and the border either side. 23,004 durations, every one worked out from the published rule and none of them asked of the chip being graded.

The **interrupt** is graded as the machine delivers it rather than as the processor takes it: what an acceptance costs in each mode, where it sends the processor, what it pushes and what it leaves in the refresh counter and the two flip-flops — and the window, walked T-state by T-state with instructions of two lengths, so the last one that catches a frame's interrupt and the first one that misses it are both pinned. A halted processor is woken by it, and a repeating instruction is interrupted between its iterations and pushed at its own address.

The **disc** is graded in three parts. The medium is proved to lay a track out where a formatter would, to answer every position on it — the identity checks it computes were checked against a CRC-16 computed outside this code — and to read back as the same disc after being written out. The **µPD765** is driven through its own handshake by a loop that plays the processor, one microsecond of disc at a time, and judged against its datasheet: the bytes crossing every 32µs and the overrun when one is late, the sector found only when its identity comes round, the end of the cylinder a read without terminal count runs into, the flags for a sector that is not there and a track that has nothing on it, the seek stepping at the specified rate and the interrupt it leaves, and the change of READY that polling reports. What the datasheet leaves open was settled by the AMSDOS ROM's own listing: a read is one sector with EOT set to R, and success is an abnormal end carrying EN.

It deliberately restates nothing an external suite already proves. The one place it cannot defer is timing: the corpus below runs the processor with its wait pin released throughout, so it proves nothing whatever about wait states.

## The sanitized tier

The fast tier again, compiled under the address and undefined-behaviour sanitizers. It is hermetic like the fast tier but slow enough to be its own target, and it exists for one class of fault the other tiers cannot see: a read that strays outside the bytes it was given. Tape images, disc images, snapshots and the firmware all arrive from outside and are none of them ours, and the checks that keep a truncated or hostile one inside its own buffer are exactly the checks a passing test agrees with — without a sanitizer, a read past the end of an image returns a number like any other. This is where those are graded.

## The machine tier

This one boots the real thing. It runs each of the four machines from reset, reads the screen back as text and checks it says what Amstrad, Locomotive Software and Sinclair wrote, then types `PRINT 2+2` at the prompt and insists BASIC answers `4`. The Spectrum's screen is read twice over, once out of the display file and once out of the framebuffer the beam painted, so the serialiser and the sync separator are in the path that is checked and not merely the memory beneath them.

That one line is the strictest test here. The key matrix, the 8255's direction flipping, the sound chip, the fifty-times-a-second scan and the interrupt that drives it must all be right at once, and none of it is graded by us — the firmware is the judge, and it was written in 1984 by people who had the hardware.

The letters are identified by looking each glyph up in the character table the ROM itself carries. A test that recognised letters by our own table would only prove we agree with ourselves.

A tape is judged the same way, and more sharply. The deck plays a block and the Spectrum's own `LD-BYTES` reads it — the routine that measures the time between edges on the ear line and has no other way of knowing what it has been handed. It returns with the carry set only if every bit arrived and the checksum agreed, so the sixteen bytes that turn up in memory were put there by Sinclair's arithmetic and not by ours.

Then it gives the 6128 a disc — Shaker's, written by somebody else's tool — and types `CAT`. The names and sizes AMSDOS prints are set against what a reader written in the test, which knows nothing of floppies or controllers and walks the image's own directory, finds in the same bytes; a file is then loaded through the controller, the ROM and BASIC and compared byte for byte with that reader's copy of it. Two routes to the same 27,004 bytes, only one of them through the chip. Then `RUN"SHAKE27A` must leave the machine in mode 2 with its program counter inside the module, which is Shaker's menu drawn in a font the ROM's reader cannot spell. With no disc at all, the ROM must find the drive not ready and say so.

It needs the firmware and disc images, so it fetches them first.

## The conformance tier

[SingleStepTests](https://github.com/SingleStepTests/z80): 1,604 files, one per opcode across every prefix page, a thousand randomised cases each — 1,604,000 in all. Every case fixes the processor and memory before and after, **and the state of the bus after each individual clock cycle**. That last part is what earns its 1.3 GB, because it tests timing rather than only results.

It matters just as much that we did not write it. It has already caught a real error: we had concluded that an undocumented behaviour under the `DD CB` prefix did not exist, and 168 files said otherwise. The corpus is pinned to a commit, so "passes the complete suite" names something exact that cannot shift underneath us.

Today every instruction the Z80 knows passes it, per cycle.

The first run downloads the corpus, which takes a few minutes; after that it is local.

## The acceptance tier

[Frank Cringle's Z80 instruction set exerciser](https://github.com/agn453/ZEXALL), from 1994, in both its forms: ZEXDOC checks the documented flags, ZEXALL all eight bits including the undocumented two. It sweeps each instruction across long runs of operands and flags and checks the result against a CRC recorded from real hardware.

Its method is independent of the corpus, but the deeper difference is that it is a *program*. Millions of instructions in sequence, each inheriting whatever the last one left behind, where the corpus tests each instruction alone from a clean state. That is the failure the corpus cannot see, and the one that decides whether real software runs.

It is slow — hours of 4MHz machine time, minutes of ours — so it runs by the group, and running all sixty-seven is a deliberate act rather than the default: `EXERCISER_GROUPS=0`.

## What is still to come

Two suites, each proving something the others cannot.

**Shaker**, Longshot's CRTC acid tests, compares against recordings made on real machines, one set per CRTC type. Its disc now boots and its modules run; what their screens show against the recordings is the question the four unimplemented CRTC types are waiting on, and what will decide the border rules that need a finer instrument than one sample per character.

And a battery of demos, which break on anything less than exact — the only tests written by people trying to make the hardware do something beautiful rather than something correct.
