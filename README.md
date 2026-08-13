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
make test   # run the test suites (first run downloads test data, needs curl)
```

There is nothing to play yet. What exists today is the first chip: a cycle-stepped Z80 being built opcode by opcode, where each opcode counts as done only when its thousand-test [SingleStepTests](https://github.com/SingleStepTests/z80) file passes, per cycle. The machine around it — Gate Array, CRTC, the Amstrad CPC itself — comes next, the same way: documentation first, tests as proof.

For development there are also `make format` (clang-format, config in `.clang-format`), `make format-check`, and `make lint` (clang-tidy, config in `.clang-tidy`).

## Sources

No scribe worked alone. Every claim in this codebase cites its source at the line that depends on it; this list is the other view — what Colophon is built on, and what each source gives us.

- [The Undocumented Z80 Documented](https://raw.githubusercontent.com/floooh/emu-info/master/z80/z80-documented.pdf) (Sean Young) — the Z80's full register model, including the internals official documentation never mentioned: WZ/MEMPTR, the X/Y flags, reset state. Our `z80_t` follows it.
- [SingleStepTests/z80](https://github.com/SingleStepTests/z80) — a thousand randomized tests per opcode, with the expected bus state after every clock cycle, recorded. Our definition of implemented: an opcode exists when its file passes. Its simplified bus conventions (single-tick read/write pulses, refresh address on the bus during T3/T4) are our bus contract.
- ["A new cycle-stepped Z80 emulator"](https://floooh.github.io/2021/12/17/cycle-stepped-z80.html) (Andre Weissflog) — the architecture we follow: the CPU not as a controller but as an ordinary chip, ticked once per clock cycle, speaking through a mask of bus pins.
- [json.org](https://www.json.org) — the grammar behind the test harness's hand-rolled JSON reader.

A source earns a line here the day code starts using it, not before.
