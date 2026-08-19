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

It deliberately restates nothing an external suite already proves. The one place it cannot defer is timing: the corpus below runs the processor with its wait pin released throughout, so it proves nothing whatever about wait states.

## The machine tier

This one boots the real thing. It runs each of the three machines from reset, reads the screen back as text and checks it says what Amstrad and Locomotive Software wrote, then types `PRINT 2+2` at the prompt and insists BASIC answers `4`.

That one line is the strictest test here. The key matrix, the 8255's direction flipping, the sound chip, the fifty-times-a-second scan and the interrupt that drives it must all be right at once, and none of it is graded by us — the firmware is the judge, and it was written in 1984 by people who had the hardware.

The letters are identified by looking each glyph up in the character table the ROM itself carries. A test that recognised letters by our own table would only prove we agree with ourselves.

It needs the firmware images, so it fetches them first.

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

**Shaker**, Longshot's CRTC acid tests, compares against recordings made on real machines, one set per CRTC type. It is what the four unimplemented CRTC types are waiting for, and what will decide the border rules that need a finer instrument than one sample per character.

And a battery of demos, which break on anything less than exact — the only tests written by people trying to make the hardware do something beautiful rather than something correct.
