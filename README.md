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
```

There is nothing to play yet. What exists today is the first chip: a cycle-stepped Z80, complete — every instruction the machine knows, undocumented ones included. The rest of the machine, Gate Array and CRTC and the CPC around them, comes next and comes the same way: documentation first, tests as proof.

For development there are also `make format` (clang-format, config in `.clang-format`), `make format-check`, and `make lint` (clang-tidy, config in `.clang-tidy`).

## Evidence

An emulator that looks right and an emulator that is right are different things, and the difference surfaces years later, in the one game nobody tried. So every claim here has a check behind it, and wherever possible the check comes from outside — written by someone else, who did not know what we believe.

The tests come in tiers, separated by what they answer and what they cost.

```sh
make test              # fast, hermetic, no network — runs on every change
make test-single-step  # the complete SingleStepTests corpus
make test-all          # both
```

`make test` covers only what no external suite can see: the reset contract, the invariants of our own machinery, that every opcode on every prefix page finishes without outgrowing its micro-program. It deliberately restates nothing the corpus already proves.

`make test-single-step` runs [SingleStepTests](https://github.com/SingleStepTests/z80): 1,604 files, one per opcode across every prefix page, a thousand randomised cases each — 1,604,000 in all. Every case fixes the CPU and memory before and after, and the state of the bus after each individual clock cycle. That last part is what earns its **1.3 GB**, because it tests timing rather than only results. It matters just as much that we did not write it: a test written from our own understanding agrees with our own mistakes, and this one disagrees. It has already caught a real error — we had concluded an undocumented DD CB behaviour did not exist, and 168 files said otherwise. The corpus is pinned to a commit, so "passes the complete suite" names something exact that cannot shift underneath us. The first run downloads it, which takes a few minutes and needs `curl` and `unzip`; after that it is local.

Today every instruction the Z80 knows passes it, per cycle.

Three more suites are still to come, each proving something the others cannot. **ZEXALL** works through exhaustive operand combinations and self-checks with CRCs: a second, unrelated proof of the same CPU. **Shaker**, Longshot's CRTC acid tests, compares against recordings made on real machines, one set per CRTC type. And a battery of demos, which break on anything less than exact — the only tests written by people trying to make the hardware do something beautiful rather than something correct.

## Sources

No scribe worked alone. Every claim in this codebase cites its source at the line that depends on it; this list is the other view — what Colophon is built on, and what each source gives us.

- [The Undocumented Z80 Documented](https://raw.githubusercontent.com/floooh/emu-info/master/z80/z80-documented.pdf) (Sean Young) — the Z80's full register model, including the internals official documentation never mentioned: WZ/MEMPTR, the X/Y flags, reset state. Our `z80_t` follows it.
- ["Decoding Z80 Opcodes"](http://www.z80.info/decoding.htm) (Cristian Dinu) — the octal structure of the opcode map: the x/y/z fields and register tables our decoder computes, the way the silicon does.
- ["MEMPTR, esoteric register of the Zilog Z80"](https://raw.githubusercontent.com/floooh/emu-info/master/z80/memptr_eng.txt) (Boo-boo et al.) — the rules of WZ, the internal register the documentation never admitted to, recovered by the community through the BIT instruction's flag leakage. Our indirect addressing goes through WZ because theirs did.
- [SingleStepTests/z80](https://github.com/SingleStepTests/z80) — a thousand randomized tests per opcode, with the expected bus state after every clock cycle, recorded. Our definition of implemented: an opcode exists when its file passes. Its simplified bus conventions (single-tick read/write pulses, refresh address on the bus during T3/T4) are our bus contract.
- ["A new cycle-stepped Z80 emulator"](https://floooh.github.io/2021/12/17/cycle-stepped-z80.html) (Andre Weissflog) — the architecture we follow: the CPU not as a controller but as an ordinary chip, ticked once per clock cycle, speaking through a mask of bus pins.
- [json.org](https://www.json.org) — the grammar behind the test harness's hand-rolled JSON reader.

A source earns a line here the day code starts using it, not before.
