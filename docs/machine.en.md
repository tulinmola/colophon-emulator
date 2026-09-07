---
title: The machine
description: What the emulated machines do today, chip by chip, and what they do not do yet.
order: 1
---

A machine assembled from parts can be honest part by part, and these are. A chip whose account of itself is incomplete says so at the head of its own header, because a part that quietly does nothing is worse than a part that is missing: the missing part is a question, and the quiet one is an answer that happens to be wrong. This page collects those declarations in one place, for a reader who is not going to open the headers. A line leaves it the day it stops being true.

## The machines

Four names answer, and a name is listed only once the machine behind it boots its own firmware to its own prompt.

| Name | Machine |
| --- | --- |
| `cpc6128` | Amstrad CPC 6128, 128K, BASIC 1.1 |
| `cpc664` | Amstrad CPC 664, 64K, BASIC 1.1 |
| `cpc464` | Amstrad CPC 464, 64K, BASIC 1.0 |
| `spectrum48` | Sinclair ZX Spectrum 48K |

The three Amstrads differ in the firmware they are given and the memory they are built with, which is very nearly how they differed on a desk. A 64K machine has no PAL fitted, so a banking command written to it falls into an empty socket and nothing moves — which is what happened on the hardware, and what a program written for 128K discovers there.

## The processor

The Z80 is complete and cycle-stepped: one call advances it by a single clock cycle, and every instruction the machine knows is implemented, the undocumented ones included. The register model is the full one — the shadow set, the index registers as halves that act as registers in their own right under a prefix, and WZ, the internal address latch the datasheet never admitted to.

The NMOS quirks are there, because both machines were given an NMOS part and programs can see them. `OUT (C),0` puts zero on the bus. `SCF` and `CCF` take their two undocumented flag bits from the accumulator or from the old flags depending on whether the instruction before them wrote flags at all. An interrupt landing on `LD A,I` or `LD A,R` leaves the parity flag reporting interrupts disabled when they were enabled — a bug Zilog acknowledged in 1989 and fixed on CMOS.

Interrupt modes 1 and 2 are implemented. Mode 0 executes whatever a device puts on the data bus, and on neither of these machines does anything drive it, so the processor reads &FF: an `RST &38`, which is mode 1's behaviour. That case is implemented and the general one is not, because neither machine carries the hardware to reach it.

## The Amstrad CPC

Six chips and a PAL, and the accounting below is of them.

### Memory

The memory map is the board's: sixteen kilobytes of lower ROM over the bottom of the address space, an upper ROM over the top, and the eight RAM configurations the 6128's PAL selects between. The upper ROM is chosen by a number latched at &DFxx, and a number with no ROM in its socket resolves to ROM 0, which is BASIC — as it does on the hardware, and which is why an unfitted expansion answers with something rather than nothing.

I/O is decoded the way the board decodes it, by single address bits rather than by whole port numbers. One access can therefore reach several devices at once, and a program that relies on it is not doing anything unusual. An access the Gate Array's wait line holds over several ticks reaches each device once.

### Video

The 6845 is emulated as **type 0**, the HD6845S/UM6845 — its register widths, its readable set, the counters a program can leave above their limits and the widths that bring them home, the frame construction, the two video pointers and their reload rules, and the line that ends the frame being decided while the character counter is still at 0 or 1, so that a register written afterwards cannot take it back.

Five CRTC types shipped in real machines and they diverge observably; demos probe for the type at runtime and branch. Only type 0 is here. The others arrive when there is something that can grade them, which is Shaker.

Not implemented in the 6845: interlace and skew (R8 is stored and never read), the cursor, the light pen, and the first three microseconds of a line. Two of the border's rules need a finer instrument than this one — the chip is stepped once per character, and both of those toggle display enable *inside* a character.

The Gate Array holds the colour registers, the ROM enables, the video mode, and the interrupt generator: the six-bit counter of line syncs that raises the interrupt every 52 lines, the request line held until the processor acknowledges, bit 5 killed at the acknowledge, and the rule that, two line syncs after a frame sync begins, the counter is cleared — raising an interrupt on the way only if bit 5 was set. It turns each pair of fetched bytes into sixteen colour samples in whichever of the four modes is in force, and a mode change takes effect after the next line sync rather than at once. Its palette is the one measured on the outputs of a real 40010, so the colours are the ones a machine produced rather than the ones its logic implies.

Not implemented: the 40010's habit of starting mode 2 one pixel early. It waits for Shaker.

The monitor is a cathode ray tube and knows nothing about computers. It receives colour samples and one composite sync line, and separates line retrace from frame retrace by how long the sync is held — which is what a tube does, and the reason a program that trims a sync pulse short moves the picture sideways: a line is timed from the middle of its pulse, so a shorter pulse walks that middle earlier and the picture half a character to the right.

Not modelled: the flywheel a real tube runs its horizontal oscillator on. Every sync edge retraces here, and a frame that outruns the tube clamps at the bottom of the screen where a real monitor would lose vertical hold and roll.

### Timing

The machine runs at the right speed, and the reason is the Gate Array rather than the processor. It holds the CPU off the memory for three cycles in four so that the video fetch always wins, which rounds every machine cycle up to a whole microsecond and costs the processor a quarter of its nominal 4MHz. That tax is what makes a CPC a CPC: it is why a program that counts instructions to reach a raster line reaches it.

The chip knows nothing about which cycle the processor is in. It generates the same pattern continually, and the CPU meets it wherever its own sampling happens to fall — which is how instructions whose lengths do not divide by four end up linearised onto the microsecond anyway.

### Sound and the keyboard

The keyboard is the real matrix: ten lines of eight switches, read the long way round — the processor asks the 8255, which asks the sound chip, which reads the grid. Both joysticks are there, including the one that shares its line with the letters, which is why its directions can be played from the keyboard.

Not modelled: keyboard clash. On hardware, three keys held at the corners of a rectangle in the matrix conjure the fourth, because the switches are a grid of wires with nothing to stop a current going the long way round.

The 8255 implements mode 0 — plain input and output — which is the only mode any program on these machines selects. A port turned to input presents &FF to whatever is wired to it, which is not a detail: it is what the device on the other side reads while the processor is reading, and getting it wrong stays invisible until something depends on it.

The AY-3-8912 keeps its registers and its one port, and **makes no sound**. The tone channels, the noise generator, the mixer and the envelope are stored and not sounded. The keyboard is read through the chip regardless, which is why typing at the prompt works with no audio anywhere in the machine.

### The cassette

The deck is the same one a Spectrum has, wired to the pins a CPC brings it to: bit 7 of the 8255's port B is what is at the play head, and bit 4 of port C is the motor. That motor is the difference between the two machines. A CPC starts and stops the tape itself, so a program may take as long as it likes between blocks; a Spectrum has no such line and must keep up with a reel that never waits.

What the machine writes to tape — port C's bit 5 — goes nowhere. Nothing here records.

The format is the `.cdt`, which is a `.tzx` under another name, so one reader serves both machines and refuses the same blocks. A `.tap` is not one of them: it is bytes at the Spectrum ROM's own timings, which no CPC firmware can read, so it is refused here rather than played into a machine that will never make sense of it. No CPC tape has been loaded through the firmware here: the wiring is graded, the loading is not.

### Snapshots

A machine writes itself out as an SNA snapshot and another reads it back and carries on. Versions 1, 2 and 3 are read, taking the fields they share; version 1 is written, because every emulator can read it.

What version 1 cannot carry is the CRTC's internal counters, so a machine resumed from one restarts its frame instead of continuing mid-raster. Version 3 has room for them, along with the CRTC type and the drive's motor and head — worth reading the day there is something here that can act on them.

### Discs

A disc is three things here, and each is its own part: the medium, the drive that turns it, and the controller that reads it.

The medium is a sectored floppy as an IBM System 34 controller finds it. Both DSK layouts are read: the original, which gives every track one length and every sector the same allotment, and the extended one, which gives each track a length of its own and each sector the length it truly occupies. That second one is what lets an image describe a disc that lies — a sector announcing a size it does not hold, an identity with nothing recorded behind it, a data field that reads differently on each revolution — and none of the disagreements between what a sector claims and what is true are corrected, because protected discs are built out of exactly those disagreements. A size code of eight or more announces 32K, which is how the chip itself counts; the image definition's older rule that only three bits counted was retracted by its own author, and Arnold's table measured on a chip agrees. An image that records where its sectors lay, in the Offset-Info block SAMdisk writes, is believed; one that does not has its sectors laid out where a formatter would have put them, gaps and all, with the gaps shortened evenly when the revolution has no room for them. Every byte of a track then has a position, so that a controller told to read past a data field finds the check, the gap and the next sector's identity — which is what some protections read past the end to look for. A disc can be written in place: a data field rewritten, a track formatted again into the room its image already gives it. An image written back is written in the extended layout, with the sector positions, and reads back as the same disc.

A track formatted larger than it was takes fresh room past the image's end, as far as the host said its buffer reaches; the command line gives every disc room to format each track once more. Not modelled in the medium: a track with more than the 29 sectors a header describes, a data field written where the image recorded none, and a format with no room left anywhere — it is reported as a write-protected disc, which is the nearest thing the software can show. A track recorded at any rate or in any mode but the one these machines read holds nothing a head can find, and keeps its place and its bytes so that an image written back still carries it.

The drive owns the motor, the head, the side and the turning. A revolution is 200ms at 300 rpm and a byte passes every 32µs, so a sector is found when its identity comes round and not before. Drive A is the machine's own one-headed 3" drive and B is the connector for a two-headed second; the motor port turns both, as the board's does. Not modelled: how long a motor takes to reach speed, which no source measures — the operating system waits a full second — so READY comes with the motor and the host may set a delay the day someone measures one; and the mechanical stop a head hits past its last cylinder, so a head steps as far as the medium is wide and finds unformatted tracks there.

The controller is a µPD765A, ticked in microseconds. Every command in its set is implemented — Read Data and Read Deleted Data, Write Data and Write Deleted Data, Read a Track, Read ID, Format a Track, the three Scans, Seek, Recalibrate with its limit of 77 steps, Sense Interrupt Status, Sense Drive Status and Specify — and every one of them is graded by a test except Scan Low or Equal and Scan High or Equal, which share Scan Equal's path and differ only in the comparison. With them come the three phases and the RQM, DIO, EXM and CB handshake the operating system polls, since neither the interrupt nor the DMA lines are wired on these boards. The data moves one byte every 32µs and a processor that has not collected one in time has overrun. Every field is judged by the bytes that pass: the chip runs its check over the marks, the identity or the data and the two check bytes, and reads the data mark for itself, so a sector a formatter wrote at one length under an identity announcing another fails its check here as it does on the disc. A command in FM finds nothing on these discs, which are MFM. The drives are polled for a change of READY between commands, at the datasheet's interval doubled for the 4MHz clock, as every other timing is; a check on that doubling is that AMSDOS programs a 6ms step rate in the datasheet's units and documents it as 12. Terminal count is a pin the machine never raises, so a read that reaches the last sector it was asked for steps past it and ends with the end-of-cylinder flag set — which is what every read on a CPC looks like, and what AMSDOS tests for.

Not modelled in the controller: DMA mode, which the boards cannot use, is taken as non-DMA mode except that EXM stays low; the microseconds the chip takes between one command byte and the next RQM, and between the end of execution and the first result byte; and the Version command, which this revision of the part does not have. The Scans compare sector by sector but their result is an approximation of the datasheet's table.

On the CPC the interface answers at A10 and A7 low — the motor at A8 low, the controller's two registers at A8 high — and is built into the 664 and 6128; a 464 gets it plugged in, with the AMSDOS ROM as upper ROM 7, the day a disc is given to it. That an access held by the wait line reaches a device once, as the memory section says, matters here more than anywhere: this chip hands over its next byte on every read.

## The ZX Spectrum

A Spectrum is one chip where a CPC is two. The Ferranti ULA counts out the frame, reads the screen, turns bytes into pixels, raises the interrupt, holds the border colour and gates the keyboard, and the board around it is little more than sixteen kilobytes of ROM, some RAM and eight address lines running to forty keys. What that buys is a machine with almost no wiring to get wrong; what it costs is that everything interesting happens inside the one part.

It boots its firmware, shows the copyright message Sinclair put in it, and answers arithmetic typed at the keyboard. The picture is read back off the beam and not out of the display file, so the serialiser, the composite sync and the tube are all in the path that is checked.

The screen and the processor share one bank of memory, and the ULA settles that argument by stopping the processor's clock rather than by asserting a wait line — so a held T-state here is one the processor does not run, while the beam runs on without it. What it costs depends entirely on when: an instruction that reads and writes the screen can take twice as long as the book says, and the same instruction begun a few T-states later costs nothing at all. That is why a Spectrum program's speed is a property of where in the frame it began, and why one that needs its timing to hold waits for the interrupt before it begins.

A memory access is charged once, at the T-state it begins. An internal T-state that only holds an address is charged like an access of its own, because these ULAs weigh the address and not the request. A port is charged by a rule of its own, at up to four of the T-states of the one access, turning on the port's low bit as well as its address. An interrupt acknowledge is charged nothing, and nothing here can settle whether the chip would: the acknowledge always falls in the top border, where nothing is owed under any rule.

Three things are missing.

**The EAR socket hears nothing the machine itself writes.** On hardware the EAR and MIC sockets and the speaker share one ULA pin, so a program that writes to bit 4 of port &FE changes what bit 6 reads back, by a path whose exact behaviour is what separates an issue 2 board from an issue 3. Here the socket hears the deck and nothing else. Loading is unaffected — the tape drives the pin — but the handful of programs that measure the machine by writing and reading their own bit will not find what they wrote.

**The floating bus is not modelled.** The chip fetches ahead of the beam through a pipeline that this collapses into a single T-state, and that pipeline is exactly what a read of an unattached port observes. Software that steers by it — waiting for the beam to reach a particular place by watching what the bus happens to be carrying — will not find what it is looking for.

**Nothing sounds.** The speaker bit is stored and never heard.

A tape plays, in both the formats a Spectrum's came in. A `.tap` is bytes at the ROM's own timings; a `.tzx` records the timings themselves, block by block, which is what a tape that brought a loader of its own needs — and most commercial releases brought one, for speed and to be hard to copy. Either way the deck plays the edges and the firmware measures the time between them, exactly as it did on hardware. A Spectrum has no motor line, so the tape turns from the moment it starts and the machine cannot stop it, which is why loading was always a race the program had to win.

What a `.tzx` can still ask for and not get is a waveform recorded sample by sample, data built from a table of symbols, or blocks visited out of order by a jump, a loop, a call or a menu. So is a block whose identifying byte the reader does not know, because the format gives a block's length nowhere but its own table and an unknown one cannot be stepped over. All of those are refused when the tape goes in rather than played wrongly, and a refusal is the whole tape and not one block. So is an image that does not hold together — one whose blocks run off its end, or whose last byte of data claims to carry more than eight bits — because a tape read past its own bytes is worse than a tape not read.

What is not refused is an element of no length: a pulse of no T-states, or a run of none. Rippers leave those in the tails of blocks and distributed tapes carry them, so they are passed over in silence, as they sound.

What the machine writes to tape goes nowhere here, as on the CPC: bit 3 of a write to port &FE is the MIC socket, and it is stored and goes no further. Nothing here records.

A tape can also stop before it ends: a multiload marks the point between its parts, and there the reel stops where a real one would have. Playing it again carries on from the next block — but nothing here presses PLAY a second time, so from the command line a multiload loads its first part and waits.

A snapshot it keeps too: the 48K `.sna`, read and written. What that format has no room for is where the beam stood, so a machine picked up from one begins its frame again from the top — the same limitation the CPC's snapshots have, for the same reason.

One smaller declaration. The colours are chosen rather than measured: the shape of the answer is fixed by the hardware, one line per gun and one more for brightness, but no measurement of what those lines reach was found, so the levels are the ones the field has settled on and a real reading would replace them.

## The firmware

The firmware images are Amstrad's — the CPC's are Locomotive Software's as well — and they are fetched rather than committed — distributable with emulators under the permission Amstrad gave in 1999, which is the ground every emulator in this field stands on. The machine is built without them and cannot boot without them.
