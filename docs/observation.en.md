---
title: Observation
description: How a debugger attaches to a machine that had nothing added to it, and the rule that keeps the apparatus outside.
order: 5
---

To instrument something is to add apparatus to the thing being measured. That is the field's word for this, and it is the wrong one here, because nothing needs adding. The tick already returns the bus. Every chip is already a plain structure with public fields. What the machine has is not instrumentation but **observability**, and what a host builds on it is observation.

The distinction is not decoration. It decides that the apparatus lives outside, and that a machine with nothing watching it is the same machine, running at the same speed, as a machine under a debugger.

## The bus is already the tap

`cpc_tick` returns the pin mask after the machine has answered, and the write path does not disturb it. Every memory write, every opcode fetch, every I/O access crosses that return value with its address and its data. A host that already calls the tick in a loop — every host does — has a complete bus trace for the cost of reading the value it was throwing away.

```c
uint64_t pins = cpc_tick(&cpc);
if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
  uint16_t address = z80_address(pins);
  size_t physical = cpc.write_page[address >> 14] + (address & 0x3FFF) - cpc.ram;
  writes[physical]++;
}
```

That is the whole tap. `write_page` is a public field, so banking cannot lie about which bank a byte truly landed in.

The chips supply the rest of the context without being asked. The monitor's beam position, the 6845's three counters, the Gate Array's mode and interrupt counter, the processor's program counter: all fields, all readable between ticks, and all exact there, because a cycle-stepped machine finishes each clock before it returns. The one value that needs care is the 6845's output pins, which the machine keeps precisely because its counters have already moved past them; [the core](core.en.md#reading-and-writing-a-machine) says why.

## What it costs

Measured over 600 frames after boot — 58,945,536 T-states — on an Apple M1 with Apple clang at `-O2`. Each variant is a separately compiled binary, so the choice is never a runtime branch.

| | against a machine with nothing watching |
| --- | --- |
| polling the returned pins, counting by address | +0.2% |
| polling, resolving the RAM bank | +0.1% |
| polling, appending to a 16 MB trace ring | +0.1% |
| a callback in the write path | +1.0% |
| a callback every tick, **with the pointer NULL** | +1.3% |
| a callback every tick, pointer set | +4.7% |

The run-to-run spread is around half a percent, and one polling variant measured *faster* than the baseline, which is impossible. So the honest reading of the first three rows is not that polling is cheap but that **it is not measurable** — it disappears into code layout. Writing 4.8 million timestamped entries to a trace ring disappears too.

The callback rows are measurable, and they say two things. A hook called every tick costs 1.3% *when it is switched off*, which is a permanent tax on every host for a feature that is almost always off. And doing identical work through a hook rather than by polling costs 4.7% against 0.2%, because an indirect call is opaque to the optimiser, which must spill the pin mask it would otherwise keep in a register.

Which means the design question was never performance. It is where the code should live.

## Breakpoints are two things, and they stay apart

**Stopping** needs nothing from the core. The core is a step function, so stopping is the host declining to call it again, and `cpc_finish_instruction` supplies an instruction boundary when one is wanted. There is no feature to build.

**Deciding** is a predicate over state that is already public. A flag table — one byte per address, consulted only on the bus cycle that could possibly match — costs 2.2%, where walking eight conditions on every tick costs 5.6%. That is how [MAME's watchpoints](https://docs.mamedev.org/debugger/watchpoint.html) and [Mesen's memory tools](https://www.mesen.ca/docs/debugging/memorytools.html) keep this cheap. Compound conditions are then ordinary C in the host.

One detail a prototype turned up: latch the address of the last opcode fetch as it goes past. By the time a write lands the program counter has moved on, and what a debugger must report is the instruction that did it.

Time travel falls out of the same properties. A snapshot is 131,328 bytes and takes 3.2 µs to write, so a ring of 128 frames is 16.8 MB, holds 2.6 seconds of history, and costs 1.7% to keep filled. Rewind is a ring of a thing that already exists.

## The rule

**A machine fact belongs in the machine. Observation policy belongs in the host.**

The rule was arrived at by trying to add three things to the core and finding that two of them did not survive being questioned.

A **T-state counter** was the first casualty. How time is measured, when it resets, what reloading a snapshot does to it — all policy, none of it a fact about a CPC. A host that needs to count ticks writes the loop itself, because everything the loop needs is public.

The **physical address of a write** went the same way. It looked like reaching into internals and it is not: `write_page` and `ram` are public and the rest is pointer subtraction. Nothing was hidden, so nothing needed exposing.

The **video fetch address** is the one that survived, and it survived because it is a hardware fact with a citation rather than a policy: it encodes how *this board* routes the 6845's address lines to RAM. It was file-local, so a host had to keep a second copy of the wiring, which would diverge silently the day the wiring was corrected. It is now [`cpc_video_address`](core.en.md#reading-and-writing-a-machine), and it is the only file-local function in the core a host could ever have wanted — every other one either mutates state or is internal to the processor.

## The layering

The core stays as it is. A host-side module owns buffers, flag tables, rings and policy. Transports — a command line, a page of panels, a debug protocol — sit on that module and never touch `src/`.

This is not a new rule. It is the existing one, applied without amendment: the core allocates nothing and does no I/O, and observation *is* buffers and policy.

Two observers exist so far, and they share almost nothing. The command line's [map of writes](command-line.en.md#the-map-of-writes) is an array of counters; the [player](https://github.com/tulinmola/colophon-player) is a page of elements watching one chip each. What is common between them is one line of pin masking, which is why there is no shared observation framework: inventing one for consumers that do not exist is the mistake this whole argument is a record of not making.

## The company we keep

[QEMU's TCG plugins](https://www.qemu.org/docs/master/devel/tcg-plugins.html) are the strongest statement of the principle: instrumentation was moved *out* of the core into a versioned API that actively avoids leaking implementation details, passive-only, unable to change machine state. QEMU needs that because its core is a JIT with nothing observable from outside. Ours is a step function whose whole state is public, so we reach the outcome QEMU built an API for by not needing one.

[Unicorn Engine's hooks](https://github.com/unicorn-engine/unicorn/tree/master/docs) can cost ten times in the worst case, because installing one forces the program counter to be synchronised. A cycle-stepped core has exact state at every tick by construction, so that entire class of cost does not exist here — a dividend from an architecture chosen for other reasons.

[floooh/chips](https://github.com/floooh/chips) is the closest relative, and its debug hook is passed *into* the exec call rather than stored on the system. That is the right shape if a hook is ever wanted here, because it keeps the machine from owning a policy slot that hosts would have to fight over.

In this field, [WinAPE](http://www.winape.net/features.jsp) carries the debugger everyone knows, and [Caprice Forever](https://www.insertmorecoins.es/en/caprice-forever-amstrad-cpc-emulator/) adopted WinAPE-compatible breakpoints specifically so that CRTC-dependent code could be analysed with the conditions people already write. If a condition syntax is ever typed at runtime here, it should be that one.

## What the measurements do not prove

One machine, one compiler, one workload. The firmware's idle loop writes hard into the stack and lightly into the screen; a game blitting a frame has a different density and would move the polling rows, though not their ordering. Nothing here was measured under WebAssembly, where an indirect call through a table costs more than it does on arm64 — where the conclusion would, if anything, be stronger.
