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

I/O is decoded the way the board decodes it, by single address bits rather than by whole port numbers. One access can therefore reach several devices at once, and a program that relies on it is not doing anything unusual. An access the Gate Array's wait line holds over several ticks reaches each device once.

## Video

The 6845 is emulated as **type 0**, the HD6845S/UM6845 — its register widths, its readable set, the counters a program can leave above their limits and the widths that bring them home, the frame construction, the two video pointers and their reload rules, and the line that ends the frame being decided while the character counter is still at 0 or 1, so that a register written afterwards cannot take it back.

Interlace is here, all but one deadline. An interlaced picture is two fields drawn half a line apart, and the machinery that offsets them is present: the chip keeps the two frame parities the documentation gives it, adds to every even frame the one line that makes the fields differ in length, and holds that frame's frame sync back to the middle of its line. In the interlaced video mode it counts differently as well — the raster address becomes the scanline counter doubled, with a parity filling the bit that leaves, so a character row covers twice the ground and the two frames address alternate lines; the register that sets a row's height is read up to that same parity. Which parity is the frame's own, except where a row is an odd number of lines, when it turns about with the row's number as well, so that rows come out alternately long and short and a pair of them still holds what a single row held before.

Writing that mode moves two things at different moments: the doubling waits for the head of the next line, while the parity joins the limit at once. That is what makes a row entered or left off its parity overrun — it counts the whole way round five bits before it finds its end again — and a program uses exactly that to discover which of the two frames it is on. Shaker times a frame in that mode at 20032 microseconds, which is what a real machine times.

The parity the frames alternate in has no defined start: silicon wakes holding whatever it holds, and this chip wakes even. Every frame after inherits that choice, so a program that reads the parity rather than setting it can be a frame out here and right on a real machine.

Five CRTC types shipped in real machines and they diverge observably; demos probe for the type at runtime and branch. Only type 0 is here. The others arrive when there is something that can grade them, which is Shaker.

Not implemented in the 6845: the skew bits, the cursor, the light pen, the later deadline the interlace line is asked for by — it is taken on the same schedule as the adjustment lines, so a program that asks for one after the third character of the last line does not get it — and most of the states this type settles at the head of a line and acts on later. One is here: whether the frame's last line is followed by extra ones is settled where the chip settles it, at the third character of the line rather than the first, so that a program is obeyed whether it asks for those lines in time or takes them back. A register moved at the second character can also turn that last line into an extra one itself, which is the chip's own way of adding a line without asking for any. Every other comparison is made afresh at each character, which agrees wherever no register moves mid-line and parts company the moment one does. Two of the border's rules need a finer instrument than this one — the chip is stepped once per character, and both of those toggle display enable *inside* a character.

The Gate Array holds the colour registers, the ROM enables, the video mode, and the interrupt generator: the six-bit counter of line syncs that raises the interrupt every 52 lines, a microsecond after the end of the line sync that asked for it — and the same microsecond on the interrupt the frame's own check raises, which the documentation does not say either way — the request line held until the processor acknowledges, bit 5 killed at the acknowledge, and the rule that, two line syncs after a frame sync begins, the counter is cleared — raising an interrupt on the way only if bit 5 was set. It turns each pair of fetched bytes into sixteen colour samples in whichever of the four modes is in force, and a mode change takes effect after the next line sync rather than at once. Its palette is the one measured on the outputs of a real 40010, so the colours are the ones a machine produced rather than the ones its logic implies.

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

A disc is three things here, and each is its own part: the medium, the drive that turns it, and the controller that reads it.

The medium is a sectored floppy as an IBM System 34 controller finds it. Both DSK layouts are read: the original, which gives every track one length and every sector the same allotment, and the extended one, which gives each track a length of its own and each sector the length it truly occupies. That second one is what lets an image describe a disc that lies — a sector announcing a size it does not hold, an identity with nothing recorded behind it, a data field that reads differently on each revolution — and none of the disagreements between what a sector claims and what is true are corrected, because protected discs are built out of exactly those disagreements. A size code of eight or more announces 32K, which is how the chip itself counts; the image definition's older rule that only three bits counted was retracted by its own author, and Arnold's table measured on a chip agrees. An image that records where its sectors lay, in the Offset-Info block SAMdisk writes, is believed; one that does not has its sectors laid out where a formatter would have put them, gaps and all, with the gaps shortened evenly when the revolution has no room for them. Every byte of a track then has a position, so that a controller told to read past a data field finds the check, the gap and the next sector's identity — which is what some protections read past the end to look for. A disc can be written in place: a data field rewritten, a track formatted again into the room its image already gives it. An image written back is written in the extended layout, with the sector positions, and reads back as the same disc.

A track formatted larger than it was takes fresh room past the image's end, as far as the host said its buffer reaches; the command line gives every disc room to format each track once more. Not modelled in the medium: a track with more than the 29 sectors a header describes, a data field written where the image recorded none, and a format with no room left anywhere — it is reported as a write-protected disc, which is the nearest thing the software can show. A track recorded at any rate or in any mode but the one these machines read holds nothing a head can find, and keeps its place and its bytes so that an image written back still carries it.

The drive owns the motor, the head, the side and the turning. A revolution is 200ms at 300 rpm and a byte passes every 32µs, so a sector is found when its identity comes round and not before. Drive A is the machine's own one-headed 3" drive and B is the connector for a two-headed second; the motor port turns both, as the board's does. Not modelled: how long a motor takes to reach speed, which no source measures — the operating system waits a full second — so READY comes with the motor and the host may set a delay the day someone measures one; and the mechanical stop a head hits past its last cylinder, so a head steps as far as the medium is wide and finds unformatted tracks there.

The controller is a µPD765A, ticked in microseconds. Every command in its set is implemented — Read Data and Read Deleted Data, Write Data and Write Deleted Data, Read a Track, Read ID, Format a Track, the three Scans, Seek, Recalibrate with its limit of 77 steps, Sense Interrupt Status, Sense Drive Status and Specify — and every one of them is graded by a test except Scan Low or Equal and Scan High or Equal, which share Scan Equal's path and differ only in the comparison. With them come the three phases and the RQM, DIO, EXM and CB handshake the operating system polls, since neither the interrupt nor the DMA lines are wired on these boards. The data moves one byte every 32µs and a processor that has not collected one in time has overrun. Every field is judged by the bytes that pass: the chip runs its check over the marks, the identity or the data and the two check bytes, and reads the data mark for itself, so a sector a formatter wrote at one length under an identity announcing another fails its check here as it does on the disc. A command in FM finds nothing on these discs, which are MFM. The drives are polled for a change of READY between commands, at the datasheet's interval doubled for the 4MHz clock, as every other timing is; a check on that doubling is that AMSDOS programs a 6ms step rate in the datasheet's units and documents it as 12. Terminal count is a pin the machine never raises, so a read that reaches the last sector it was asked for steps past it and ends with the end-of-cylinder flag set — which is what every read on a CPC looks like, and what AMSDOS tests for.

Not modelled in the controller: DMA mode, which the boards cannot use, is taken as non-DMA mode except that EXM stays low; the microseconds the chip takes between one command byte and the next RQM, and between the end of execution and the first result byte; and the Version command, which this revision of the part does not have. The Scans compare sector by sector but their result is an approximation of the datasheet's table.

On the CPC the interface answers at A10 and A7 low — the motor at A8 low, the controller's two registers at A8 high — and is built into the 664 and 6128; a 464 gets it plugged in, with the AMSDOS ROM as upper ROM 7, the day a disc is given to it. That an access held by the wait line reaches a device once, as the memory section says, matters here more than anywhere: this chip hands over its next byte on every read.

## The firmware

The firmware images are Amstrad's, and they are fetched rather than committed — distributable with emulators under the permission Amstrad gave in 1999, which is the ground every emulator in this field stands on. The machine is built without them and cannot boot without them.
