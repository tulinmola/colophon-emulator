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
build/emulator boot --screenshot ready.png
build/emulator boot --machine cpc464 --screenshot ready.png
build/emulator boot --type 'PRINT 2+2\n' --screenshot sum.png
build/emulator boot --type 'MODE 0\n' --save state.sna
build/emulator run state.sna --type 'BORDER 6\n' --screenshot resumed.png
build/emulator boot --full-raster --screenshot raster.png
```

`boot` starts a machine from reset and `run` picks one up from a snapshot; both then run for a fixed number of frames, type whatever `--type` was given, and write what they are asked for — a PNG of the screen, an SNA snapshot of the machine, or both. Three machines answer to `--machine` — `cpc6128`, `cpc664`, `cpc464` — and a name is only listed once the machine behind it boots to its prompt. `--full-raster` gives the whole beam path instead of the picture: sync, blanking, the border in its entirety, and the corner the flyback never sweeps. Nothing consults a clock, so the same command writes the same bytes every time.

The firmware images are Amstrad's. `make roms` fetches them, pinned by hash, under the permission Amstrad granted in 1999 to distribute them with emulators; they are never committed here. The images the PNG writer produces are uncompressed — the format allows it, and it saves us a compressor to get wrong.

There is nothing to play yet, but there is something to see. The Z80 came first — cycle-stepped, complete, every instruction the machine knows, undocumented ones included — and the CPC has been built around it a chip at a time: the memory map with its RAM banking and ROM paging, a 6845 CRTC counting out the frame at one character per microsecond, a Gate Array raising the 300Hz heartbeat and turning bytes into colour, and a monitor that takes the one composite sync wire and separates it the way a tube does. Given the firmware, the machine now boots it, and the Ready prompt arrives on the screen in the right colours, in the right place.

It can hear you, too. The keyboard is a grid of switches read the long way round — the CPU asks the 8255, which asks the sound chip, which reads the grid — and with that path in place you can type at the prompt and BASIC will answer.

And it runs at the right speed. The Gate Array keeps the CPU off the memory for three cycles in four so the video always wins, which stretches every instruction onto a whole microsecond and costs the processor a quarter of its nominal 4MHz — the tax that makes a CPC a CPC.

It can also be stopped and picked up again: a machine writes itself out as an SNA snapshot, and another reads it back and carries on. What comes next is the disc controller, which is what stands between here and the games.

For development there are also `make format` (clang-format, config in `.clang-format`), `make format-check`, and `make lint` (clang-tidy, config in `.clang-tidy`).

## Evidence

An emulator that looks right and an emulator that is right are different things, and the difference surfaces years later, in the one game nobody tried. So every claim here has a check behind it, and wherever possible the check comes from outside — written by someone else, who did not know what we believe.

The tests come in tiers, separated by what they answer and what they cost.

```sh
make test              # fast, hermetic, no network — runs on every change
make test-firmware     # boots the real firmware and types at it
make test-single-step  # the complete SingleStepTests corpus
make test-exerciser    # the Z80 instruction set exerciser
make test-all          # all four
```

`make test` covers only what no external suite can see: the reset contract, the invariants of our own machinery, that every opcode on every prefix page finishes without outgrowing its micro-program. Since the machine began it also proves the memory map against its documentation — the eight banking configurations, the ROM paging, the I/O decode — each exercised through the bus by a program running from a fabricated ROM, the CRTC against the Compendium's frame — 312 scanlines of 64 microseconds, the syncs where the registers put them, the video pointer walking the documented rows — the Gate Array against chapter 27 and its own documentation — the interrupt counter looping at 52, bit 5 dying at the acknowledge, the two-HSYNC rule after VSYNC, a byte becoming pixels in each of the four modes — the video path end to end — a screen of pixels through the whole machine, landing 640 by 200 exactly where the syncs put it — and the duration of seventy-odd instructions in microseconds, against two tables of measurements made independently of each other and of us. It deliberately restates nothing the corpus already proves, and the corpus proves nothing about wait states: it runs the CPU with the pin released throughout.

`make test-firmware` boots the real thing. It runs each of the three machines from reset, reads the screen back as text and checks it says what Amstrad and Locomotive Software wrote, then types `PRINT 2+2` at the prompt and insists BASIC answers `4`. That one line is the strictest test here: the keyboard matrix, the 8255's direction flipping, the PSG, the fifty-times-a-second scan and the interrupt that drives it must all be right at once, and none of it is graded by us. The letters are identified by looking each glyph up in the character table the ROM itself carries — a test that recognised letters by our own table would only prove we agree with ourselves. It needs the firmware, so it fetches it first.

`make test-single-step` runs [SingleStepTests](https://github.com/SingleStepTests/z80): 1,604 files, one per opcode across every prefix page, a thousand randomised cases each — 1,604,000 in all. Every case fixes the CPU and memory before and after, and the state of the bus after each individual clock cycle. That last part is what earns its **1.3 GB**, because it tests timing rather than only results. It matters just as much that we did not write it: a test written from our own understanding agrees with our own mistakes, and this one disagrees. It has already caught a real error — we had concluded an undocumented DD CB behaviour did not exist, and 168 files said otherwise. The corpus is pinned to a commit, so "passes the complete suite" names something exact that cannot shift underneath us. The first run downloads it, which takes a few minutes and needs `curl` and `unzip`; after that it is local.

Today every instruction the Z80 knows passes it, per cycle.

`make test-exerciser` runs [Frank Cringle's Z80 instruction set exerciser](https://github.com/agn453/ZEXALL) from 1994, in both its forms: ZEXDOC checks the documented flags, ZEXALL all eight bits including the undocumented two. It sweeps each instruction across long runs of operands and flags and checks the result against a CRC recorded from real hardware. Its method is independent of the corpus, but the deeper difference is that it is a *program*: millions of instructions in sequence, each inheriting whatever the last one left behind, where the corpus tests each instruction alone from a clean state. That is the failure the corpus cannot see, and the one that decides whether real software runs. It is slow — hours of 4 MHz machine time, minutes of ours — so it runs by the group: `EXERCISER_GROUPS=0` for all sixty-seven.

Two suites are still to come, each proving something the others cannot. **Shaker**, Longshot's CRTC acid tests, compares against recordings made on real machines, one set per CRTC type. And a battery of demos, which break on anything less than exact — the only tests written by people trying to make the hardware do something beautiful rather than something correct.

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
- ["Snapshot file format"](https://cpctech.cpcwiki.de/docs/snapshot.html) (Kevin Thacker) — the SNA header of all three versions, and the notes on the two fields easiest to get backwards: the PPI stores inputs for ports A and B but outputs for C, and the flip-flops it names IFF0 and IFF1 are what everyone else calls IFF1 and IFF2.
- ["Timings"](https://cpctech.cpcwiki.de/docs/instrtim.html) (Kevin Thacker) — how long every instruction takes on a CPC, in microseconds, measured. Together with the Compendium's chapter 26, which was measured separately, it is what judges our wait states; the two agree on all but one row.
- ["Reading the keyboard and Joysticks"](https://cpctech.cpcwiki.de/docs/keyboard.html), ["8255 PPI"](https://cpctech.cpcwiki.de/docs/8255cpc.html) and ["AY-3-8912 PSG"](https://cpctech.cpcwiki.de/docs/psg.html) (Kevin Thacker) — the key matrix position by position, the six-step dance that reads one line of it, what each port of the 8255 is wired to, and the rule that a port turned to input presents &FF to whatever is on the other side. Our keyboard, `ppi.c` and `psg.c` are built on them.
- ["Interrupts on the CPC/CPC+ and KC Compact"](https://cpctech.cpcwiki.de/docs/ints.html) (Kevin Thacker) — the interrupt counter's behaviour from the programmer's side; RMR bit 4 clearing the pending request along with the counter.
- ["Amstrad CPC Ram Paging"](https://cpctech.cpcwiki.de/docs/rampage.html), ["I/O port allocation"](https://cpctech.cpcwiki.de/docs/iopord.html) (Mark Rison & Kevin Thacker) and ["Expansion ROM Selection"](https://cpctech.cpcwiki.de/docs/exprom.html), from Kevin Thacker's cpctech — the 6128 PAL's partial decode of its register, the address-bit I/O decoding that lets one access reach several devices at once, and the upper ROM latch with its fallback to BASIC. The machine's I/O decode follows them.
- [json.org](https://www.json.org) — the grammar behind the test harness's hand-rolled JSON reader.
- [The PNG Specification](https://www.w3.org/TR/png-3/), with [RFC 1950](https://www.rfc-editor.org/rfc/rfc1950) and [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951) — chunks, CRC-32, the zlib wrapper and its Adler-32, and deflate's stored block, which is the only one we emit. Enough to write a screenshot without a dependency.
- Amstrad's permission to distribute the firmware ROMs, given by [Cliff Lawson in 1999](https://groups.google.com/g/comp.sys.amstrad.8bit/c/HtpBU2Bzv_U/m/HhNDSU3MksAJ) on comp.sys.amstrad.8bit: keep the copyright messages intact, acknowledge Amstrad's copyright, charge nobody for them. Never rescinded, and the ground every emulator in the field stands on. `tools/fetch-roms.sh` relies on it.

A source earns a line here the day code starts using it, not before.
