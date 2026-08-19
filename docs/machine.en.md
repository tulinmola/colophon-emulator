---
title: The machine
description: What the emulated CPC does today, chip by chip, and what it does not do yet.
order: 1
---

A machine assembled from parts can be honest part by part, and this one is. A chip whose account of itself is incomplete says so at the head of its own header, because a part that quietly does nothing is worse than a part that is missing: the missing part is a question, and the quiet one is an answer that happens to be wrong. This page collects those declarations in one place, for a reader who is not going to open the headers. A line leaves it the day it stops being true.

## The machines

Three names answer, and a name is listed only once the machine behind it boots its own firmware to the Ready prompt.

| Name | Machine |
| --- | --- |
| `cpc6128` | Amstrad CPC 6128, 128K, BASIC 1.1 |
| `cpc664` | Amstrad CPC 664, 64K, BASIC 1.1 |
| `cpc464` | Amstrad CPC 464, 64K, BASIC 1.0 |

They differ in the firmware they are given and the memory they are built with, which is very nearly how they differed on a desk. A 64K machine has no PAL fitted, so a banking command written to it falls into an empty socket and nothing moves — which is what happened on the hardware, and what a program written for 128K discovers there.

## The processor

The Z80 is complete and cycle-stepped: one call advances it by a single clock cycle, and every instruction the machine knows is implemented, the undocumented ones included. The register model is the full one — the shadow set, the index registers as halves that act as registers in their own right under a prefix, and WZ, the internal address latch the datasheet never admitted to.

The NMOS quirks are there, because the CPC's part is NMOS and programs can see them. `OUT (C),0` puts zero on the bus. `SCF` and `CCF` take their two undocumented flag bits from the accumulator or from the old flags depending on whether the instruction before them wrote flags at all. An interrupt landing on `LD A,I` or `LD A,R` leaves the parity flag reporting interrupts disabled when they were enabled — a bug Zilog acknowledged in 1989 and fixed on CMOS.

Interrupt modes 1 and 2 are implemented. Mode 0 executes whatever a device puts on the data bus, and on a CPC nothing drives it, so the processor reads &FF: an `RST &38`, which is mode 1's behaviour. That case is implemented and the general one is not, because no CPC carries the hardware to reach it.

## Memory

The memory map is the board's: sixteen kilobytes of lower ROM over the bottom of the address space, an upper ROM over the top, and the eight RAM configurations the 6128's PAL selects between. The upper ROM is chosen by a number latched at &DFxx, and a number with no ROM in its socket resolves to ROM 0, which is BASIC — as it does on the hardware, and which is why an unfitted expansion answers with something rather than nothing.

I/O is decoded the way the board decodes it, by single address bits rather than by whole port numbers. One access can therefore reach several devices at once, and a program that relies on it is not doing anything unusual.

## Video

The 6845 is emulated as **type 0**, the HD6845S/UM6845 — its register widths, its readable set, the counters a program can leave above their limits and the widths that bring them home, the frame construction, the two video pointers and their reload rules, and the line that ends the frame being decided while the character counter is still at 0 or 1, so that a register written afterwards cannot take it back.

Five CRTC types shipped in real machines and they diverge observably; demos probe for the type at runtime and branch. Only type 0 is here. The others arrive when there is something that can grade them, which is Shaker.

Not implemented in the 6845: interlace and skew (R8 is stored and never read), the cursor, the light pen, and the first three microseconds of a line. Two of the border's rules need a finer instrument than this one — the chip is stepped once per character, and both of those toggle display enable *inside* a character.

The Gate Array holds the colour registers, the ROM enables, the video mode, and the interrupt generator: the six-bit counter of line syncs that raises the interrupt every 52 lines, the request line held until the processor acknowledges, bit 5 killed at the acknowledge, and the rule that, two line syncs after a frame sync begins, the counter is cleared — raising an interrupt on the way only if bit 5 was set. It turns each pair of fetched bytes into sixteen colour samples in whichever of the four modes is in force, and a mode change takes effect after the next line sync rather than at once. Its palette is the one measured on the outputs of a real 40010, so the colours are the ones a machine produced rather than the ones its logic implies.

Not implemented: the 40010's habit of starting mode 2 one pixel early. It waits for Shaker.

The monitor is a cathode ray tube and knows nothing about computers. It receives colour samples and one composite sync line, and separates line retrace from frame retrace by how long the sync is held — which is what a tube does, and the reason a program that trims a sync pulse short moves the picture sideways: a line is timed from the middle of its pulse, so a shorter pulse walks that middle earlier and the picture half a character to the right.

Not modelled: the flywheel a real tube runs its horizontal oscillator on. Every sync edge retraces here, and a frame that outruns the tube clamps at the bottom of the screen where a real monitor would lose vertical hold and roll.

## Timing

The machine runs at the right speed, and the reason is the Gate Array rather than the processor. It holds the CPU off the memory for three cycles in four so that the video fetch always wins, which rounds every machine cycle up to a whole microsecond and costs the processor a quarter of its nominal 4MHz. That tax is what makes a CPC a CPC: it is why a program that counts instructions to reach a raster line reaches it.

The chip knows nothing about which cycle the processor is in. It generates the same pattern continually, and the CPU meets it wherever its own sampling happens to fall — which is how instructions whose lengths do not divide by four end up linearised onto the microsecond anyway.

## Sound and the keyboard

The keyboard is the real matrix: ten lines of eight switches, read the long way round — the processor asks the 8255, which asks the sound chip, which reads the grid. Both joysticks are there, including the one that shares its line with the letters, which is why its directions can be played from the keyboard.

Not modelled: keyboard clash. On hardware, three keys held at the corners of a rectangle in the matrix conjure the fourth, because the switches are a grid of wires with nothing to stop a current going the long way round.

The 8255 implements mode 0 — plain input and output — which is the only mode any program on these machines selects. A port turned to input presents &FF to whatever is wired to it, which is not a detail: it is what the device on the other side reads while the processor is reading, and getting it wrong stays invisible until something depends on it.

The AY-3-8912 keeps its registers and its one port, and **makes no sound**. The tone channels, the noise generator, the mixer and the envelope are stored and not sounded. The keyboard is read through the chip regardless, which is why typing at the prompt works with no audio anywhere in the machine.

## Snapshots

A machine writes itself out as an SNA snapshot and another reads it back and carries on. Versions 1, 2 and 3 are read, taking the fields they share; version 1 is written, because every emulator can read it.

What version 1 cannot carry is the CRTC's internal counters, so a machine resumed from one restarts its frame instead of continuing mid-raster. Version 3 has room for them, along with the CRTC type and the drive's motor and head — worth reading the day there is something here that can act on them.

## Discs

There is a disc and nothing yet to read it with. Both DSK layouts are read: the original, which gives every track one length and every sector the same allotment, and the extended one, which gives each track a length of its own and each sector the length it truly occupies. That second one is what lets an image describe a disc that lies — a sector announcing a size it does not hold, an identity with nothing recorded behind it, a data field that reads differently on each revolution.

An image becomes a medium: cylinders, sides, and the sectors in the order they pass under the head, each carrying the four identity bytes it announces itself with. None of the disagreements between what a sector claims and what is true are corrected, because protected discs are built out of exactly those disagreements.

What is missing is the controller. Until a µPD765 turns a head across this medium, a snapshot is the only way into a game.

## The firmware

The firmware images are Amstrad's, and they are fetched rather than committed — distributable with emulators under the permission Amstrad gave in 1999, which is the ground every emulator in this field stands on. The machine is built without them and cannot boot without them.
