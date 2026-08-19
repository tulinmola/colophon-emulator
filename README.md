# Colophon

## Prologue

For centuries, a scribe finishing a manuscript would add a colophon at the end: who copied it, where, when, and how. The games of the 8-bit era never got one. They shipped as sealed boxes of code, and most of what their authors knew about how they worked was never written down.

A lot of it is already gone.

Colophon opens those boxes. It runs Amstrad CPC games and watches them from the inside, revealing how the screen was drawn, how the levels were packed, how the music was squeezed out of a tiny sound chip. Then it writes the note that was never written, and puts it in a public archive where anyone can read it.

Preserving these games means more than keeping the files alive. It means understanding them, while there's still someone around to check we got it right.

## Building

A C compiler and `make` are the whole toolchain. No configure step, no dependencies: the CPC is from 1984, and its workshop may as well be.

```sh
make        # build
make test   # the fast tests
make roms   # fetch the firmware, once
```

Then run a machine and look at it:

```sh
build/emulator boot --type 'PRINT 2+2\n' --screenshot sum.png
```

`boot` starts a machine from reset and `run` picks one up from a snapshot; both then run for a fixed number of frames, type whatever `--type` was given, and write what they are asked for — a PNG of the screen, an SNA snapshot of the machine, a map of every write it made, or all three. Nothing consults a clock, so the same command writes the same bytes every time. [The command line](docs/command-line.en.md) sets out the rest.

The firmware images are Amstrad's. `make roms` fetches them, pinned by hash, under the permission Amstrad granted in 1999 to distribute them with emulators; they are never committed here.

For development there are also `make format` (clang-format, config in `.clang-format`), `make format-check`, and `make lint` (clang-tidy, config in `.clang-tidy`).

## Where it stands

There is nothing to play yet, but there is something to see. The Z80 came first — cycle-stepped, complete, every instruction the machine knows, undocumented ones included — and the CPC has been built around it a chip at a time: the memory map with its RAM banking and ROM paging, a 6845 CRTC counting out the frame at one character per microsecond, a Gate Array raising the 300Hz heartbeat and turning bytes into colour, and a monitor that takes the one composite sync wire and separates it the way a tube does. Given the firmware, the machine now boots it, and the Ready prompt arrives on the screen in the right colours, in the right place.

It can hear you, too. The keyboard is a grid of switches read the long way round — the CPU asks the 8255, which asks the sound chip, which reads the grid — and with that path in place you can type at the prompt and BASIC will answer. And it runs at the right speed: the Gate Array keeps the CPU off the memory for three cycles in four so the video always wins, which stretches every instruction onto a whole microsecond and costs the processor a quarter of its nominal 4MHz — the tax that makes a CPC a CPC.

It can also be stopped and picked up again: a machine writes itself out as an SNA snapshot, and another reads it back and carries on.

And there is a disc, though nothing yet to read it with. A disc image becomes a medium — cylinders, sides, and the sectors lying under the head with the identities they announce, the wrong lengths some of them claim, and the several readings a protected one keeps. What comes next is the controller that turns a head across it, which is what stands between here and the games.

[The machine](docs/machine.en.md) is the full accounting, chip by chip, of what is there and what is not.

## Documentation

The emulator's documentation lives in `docs/`, beside the code it describes, and is gathered and published by [The Colophon Project](https://github.com/tulinmola/colophon-project).

- [The machine](docs/machine.en.md) — what it does today, chip by chip, and what it does not.
- [The evidence](docs/evidence.en.md) — the tiers of tests, what each proves, and what each cannot see.
- [The command line](docs/command-line.en.md) — booting a machine, typing at it, and carrying away a picture or a snapshot.
- [The core](docs/core.en.md) — the interface a host builds on.
- [Observation](docs/observation.en.md) — how a debugger attaches to a machine that had nothing added to it.

## Evidence

An emulator that looks right and an emulator that is right are different things, and the difference surfaces years later, in the one game nobody tried. So every claim here has a check behind it, and wherever possible the check comes from outside — written by someone else, who did not know what we believe. A test built from our own understanding agrees with our own mistakes.

```sh
make test              # fast, hermetic, no network — runs on every change
make test-firmware     # boots the real firmware and types at it
make test-single-step  # the complete SingleStepTests corpus
make test-exerciser    # the Z80 instruction set exerciser
make test-all          # all four
```

Today every instruction the Z80 knows passes [SingleStepTests](https://github.com/SingleStepTests/z80) per cycle — 1,604,000 cases, each fixing the state of the bus after every clock — and all three machines boot their own firmware and answer `PRINT 2+2` correctly, with the letters read back through the character table the ROM itself carries.

[The evidence](docs/evidence.en.md) sets out what each tier proves, what it costs, and the two suites still to come.

## Sources

No scribe worked alone. Every claim in this codebase cites its source at the line that depends on it; this list is the other view — what Colophon is built on, and what each source gives us.

- [The Undocumented Z80 Documented](https://raw.githubusercontent.com/floooh/emu-info/master/z80/z80-documented.pdf) (Sean Young) — the Z80's full register model, including the internals official documentation never mentioned: WZ/MEMPTR, the X/Y flags, reset state. Our `z80_t` follows it.
- ["Decoding Z80 Opcodes"](http://www.z80.info/decoding.htm) (Cristian Dinu) — the octal structure of the opcode map: the x/y/z fields and register tables our decoder computes, the way the silicon does.
- ["MEMPTR, esoteric register of the Zilog Z80"](https://raw.githubusercontent.com/floooh/emu-info/master/z80/memptr_eng.txt) (Boo-boo et al.) — the rules of WZ, the internal register the documentation never admitted to, recovered by the community through the BIT instruction's flag leakage. Our indirect addressing goes through WZ because theirs did.
- [SingleStepTests/z80](https://github.com/SingleStepTests/z80) — a thousand randomized tests per opcode, with the expected bus state after every clock cycle, recorded. Our definition of implemented: an opcode exists when its file passes. Its simplified bus conventions (single-tick read/write pulses, refresh address on the bus during T3/T4) are our bus contract.
- ["A new cycle-stepped Z80 emulator"](https://floooh.github.io/2021/12/17/cycle-stepped-z80.html) (Andre Weissflog) — the architecture we follow: the CPU not as a controller but as an ordinary chip, ticked once per clock cycle, speaking through a mask of bus pins.
- ["Getting into way too much detail with the Z80 netlist simulation"](https://floooh.github.io/2021/12/06/z80-instruction-timing.html) (Andre Weissflog) — gate-level traces of interrupt acceptance, half-cycle by half-cycle. Our acknowledge cycles follow it, including the finding that a run of DD prefixes locks out even a non-maskable interrupt.
- ["Interrupt Behaviour of the Z80 CPU"](http://www.z80.info/interrup.htm) (Achim Flammenkamp) and ["Z80 interrupt mode 2"](http://www.z80.info/interrup2.htm) (J.G. Harston) — the machine-cycle breakdowns we time against, and the hardware experiment proving mode 2 does not force its vector even, against Zilog's own manual.
- ["The Amstrad CPC CRTC Compendium" v1.10](https://shaker.logonsystem.eu/ACCC1.10-EN.pdf) (Longshot / Logon System) — the video system as the demoscene proved it on real machines: the counters and the names it asks emulator authors to adopt, the frame construction, the two video pointers and their reload rules, the five types and their register files, and the Gate Array's interrupt generator — the R52 counter, the INT line held until acknowledged, the two-HSYNC rule after VSYNC — and the composite sync it sends the monitor, one wire carrying both locks by inverting the line pulses inside the frame pulse. Chapter 26 tabulates every instruction's duration on a CPC and chapter 4.4.4 dissects why, down to which T-state of which machine cycle the Z80 samples its WAIT pin — the fact our wait states turn on. Our `crtc.c`, `gate_array.c`, `monitor.c` and the CPU's wait sampling are built on it. Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot (CC BY-NC-ND).
- ["The CRTC"](https://www.grimware.org/doku.php/documentations/devices/crtc) (Grim) — the register overview, the five types, the board's wiring of the CRTC bus (A14 selects the chip, RS is A8, R/W is A9), and what a monitor does between frames: it holds its beam in the top-left corner, which is where our frame retrace leaves it.
- ["The Gate Array"](https://www.grimware.org/doku.php/documentations/devices/gatearray) (Grim) — the Gate Array and the PAL as their programmers knew them: the &7Fxx command dispatch, the PENR/INKR/RMR layouts, the eight RAM banking configurations, mode changes taking effect after the HSYNC, which bit of a byte becomes which pixel in each mode, and the palette as measured on the outputs of a real 40010 — so our colours are the ones a machine produced, not the ones the three-state logic implies.
- ["Screen memory addressess"](https://cpctech.cpcwiki.de/docs/scraddr.html) (Kevin Thacker) — the board's rewiring of the CRTC's address lines on the way to RAM, which is what scatters a character row across eight blocks two kilobytes apart.
- [The µPD765A/µPD765B Floppy Disc Controller](https://cpctech.cpcwiki.de/docs/upd765a/necfdc.htm) (NEC, mirrored by cpctech) — the controller's own datasheet: its commands and their three phases, the status registers it returns, and the four identity bytes C, H, R and N a sector announces and a controller matches against. It also names the shape of the disc itself, IBM System 34 double density — which is why `floppy.h` describes a medium belonging to a family of controllers rather than to a machine, and why it is the datasheet and not the image definition that settles where the control mark sits.
- ["Snapshot file format"](https://cpctech.cpcwiki.de/docs/snapshot.html) (Kevin Thacker) — the SNA header of all three versions, and the notes on the two fields easiest to get backwards: the PPI stores inputs for ports A and B but outputs for C, and the flip-flops it names IFF0 and IFF1 are what everyone else calls IFF1 and IFF2.
- ["Disk image file format"](https://cpctech.cpcwiki.de/docs/dsk.html) and ["Extended DiSK image definition"](https://cpctech.cpcwiki.de/docs/extdsk.html) (Kevin Thacker, the second with extensions by John Elliott and Simon Owen) — the two layouts `dsk.c` reads. The original gives every track one length and every sector the same allotment; the extended one gives each track a length of its own and each sector the length it truly occupies, which is what lets it describe a disc that lies. Its per-track table marks a track never formatted with a zero that costs no bytes, its size code is three bits so eight means none, and a sector whose stored length is an exact multiple of the length it announces holds that many readings of a data field that reads differently each time.
- ["Timings"](https://cpctech.cpcwiki.de/docs/instrtim.html) (Kevin Thacker) — how long every instruction takes on a CPC, in microseconds, measured. Together with the Compendium's chapter 26, which was measured separately, it is what judges our wait states; the two agree on all but one row.
- ["Reading the keyboard and Joysticks"](https://cpctech.cpcwiki.de/docs/keyboard.html), ["8255 PPI"](https://cpctech.cpcwiki.de/docs/8255cpc.html) and ["AY-3-8912 PSG"](https://cpctech.cpcwiki.de/docs/psg.html) (Kevin Thacker) — the key matrix position by position, the six-step dance that reads one line of it, what each port of the 8255 is wired to, and the rule that a port turned to input presents &FF to whatever is on the other side. Our keyboard, `ppi.c` and `psg.c` are built on them.
- ["Interrupts on the CPC/CPC+ and KC Compact"](https://cpctech.cpcwiki.de/docs/ints.html) (Kevin Thacker) — the interrupt counter's behaviour from the programmer's side; RMR bit 4 clearing the pending request along with the counter.
- ["Amstrad CPC Ram Paging"](https://cpctech.cpcwiki.de/docs/rampage.html), ["I/O port allocation"](https://cpctech.cpcwiki.de/docs/iopord.html) (Mark Rison & Kevin Thacker) and ["Expansion ROM Selection"](https://cpctech.cpcwiki.de/docs/exprom.html), from Kevin Thacker's cpctech — the 6128 PAL's partial decode of its register, the address-bit I/O decoding that lets one access reach several devices at once, and the upper ROM latch with its fallback to BASIC. The machine's I/O decode follows them.
- [json.org](https://www.json.org) — the grammar behind the test harness's hand-rolled JSON reader.
- [The PNG Specification](https://www.w3.org/TR/png-3/), with [RFC 1950](https://www.rfc-editor.org/rfc/rfc1950) and [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951) — chunks, CRC-32, the zlib wrapper and its Adler-32, and deflate's stored block, which is the only one we emit. Enough to write a screenshot without a dependency.
- Amstrad's permission to distribute the firmware ROMs, given by [Cliff Lawson in 1999](https://groups.google.com/g/comp.sys.amstrad.8bit/c/HtpBU2Bzv_U/m/HhNDSU3MksAJ) on comp.sys.amstrad.8bit: keep the copyright messages intact, acknowledge Amstrad's copyright, charge nobody for them. Never rescinded, and the ground every emulator in the field stands on. `tools/fetch-roms.sh` relies on it.

A source earns a line here the day code starts using it, not before.
