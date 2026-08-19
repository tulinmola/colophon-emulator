---
title: The core
description: The interface a host builds on — the tick, the pin masks, the buffers it must hand over, and the two things the core refuses to do.
order: 4
---

The core is a machine, not an application. It has no `main`, it never asks the operating system for anything, and it cannot print. What it offers a host is a step function and a set of plain structures, and everything else — files, windows, sound, time — belongs to whoever is calling.

Two rules produce all of that, and they are worth stating before anything else.

**The core allocates nothing.** Every buffer it uses is handed to it: the RAM, the ROM images, the framebuffer. It holds them by pointer and never copies them, so they must outlive the machine and must not move.

**The core does no I/O.** Nothing in `src/` opens a file or writes to a stream. That is what lets the same C run behind a command line and inside a browser without either host inheriting the other's assumptions.

## The tick

```c
uint64_t pins = cpc_tick(&cpc);
```

One call advances the machine by one T-state. The processor runs every time; the chips run on the character clock, which is every fourth. The return value is the bus after the machine has answered.

The processor is not a controller. `z80_tick` takes the current pins and returns the new ones, and the wiring around it decides what those pins mean — which is why the same CPU file has nothing about the CPC in it, and why the machine's 4T alignment is produced by the Gate Array holding a wait line rather than by the processor knowing anything about a CPC.

A machine stopped mid-instruction has a program counter that belongs to no instruction anyone can name, so anything that wants an instruction boundary asks for one:

```c
cpc_finish_instruction(&cpc);
```

A snapshot has nowhere to record a half-executed instruction, so anything about to take one owes the machine that call first.

## Pin masks

A bus is a 64-bit mask. Address lines sit at the bottom, the data lanes above them, and the control pins above those. **A set bit means the pin is asserted, not that a wire is high** — most of these are active-low on silicon, and the mask models assertion so that reading the code does not require holding the inversion in your head.

Pins are touched through the accessors and never by their bit positions:

```c
uint16_t address = z80_address(pins);
uint8_t data = z80_data(pins);
if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) { … }
```

The 6845 has its own mask with its own accessors — `crtc_ma`, `crtc_ra`, `crtc_data` — sharing the data lanes with the processor's so that a byte crossing between them needs no shifting.

The names are the datasheets'. A pin, a register or a counter that has a page in the primary documentation carries the name that page gives it: `Z80_MREQ`, `wz`, the CRTC's `c0`, `c4` and `c9` as the Compendium asks emulator authors to call them. Where we deviate — an active level, a prime mark spelled as a trailing underscore — the deviation is declared where the thing is defined.

## Building a machine

```c
cpc_t cpc;
cpc_init(&cpc, ram, ram_size, lower_rom);
cpc_set_upper_rom(&cpc, 0, basic_rom);
cpc_connect_monitor(&cpc, framebuffer);
cpc_set_links(&cpc, true, CPC_MANUFACTURER_AMSTRAD);
```

The RAM's size is the machine's identity as far as the board is concerned: 64K means no PAL is fitted and banking commands die on the empty socket, 128K makes it a 6128. An upper ROM socket left empty resolves to ROM 0 when something selects it, as it does on the hardware.

The framebuffer is `CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT` bytes of hardware colour codes — the whole raster, not the picture. Left unplugged, the machine runs on and draws into the void, as it would with the cable out.

The links are the ones soldered on the board: the refresh rate, which decides which of two tables in the firmware the machine programs the 6845 from, and the manufacturer, which software reads and cannot change.

## Reading and writing a machine

Every field of every chip is public. This is deliberate and not an oversight: there is no encapsulation to work around anywhere in the core, which is the property [observation](observation.en.md) is built on.

Three functions exist anyway, because each encodes something a host would otherwise have to know for itself:

`cpc_peek` and `cpc_poke` resolve through the same mapping the processor's own memory cycles use, so a peek under an enabled ROM sees the ROM and a poke lands in the RAM beneath it.

`cpc_video_address` applies the board's rewiring of the 6845's address lines — which is what scatters a character row across eight blocks two kilobytes apart — to the pins the chip put out on its last character clock. It takes the machine and not the chip, and that is not an accident: by the time a caller holds the pins, the chip's counters have already moved past the values that went out on the wires, so a function reading the counters would name an address the machine never fetched from.

A host that sets the registers behind the memory map from outside — restoring a snapshot, say — owes the machine a `cpc_remap` afterwards.

## What is a chip and what is a machine

A chip module knows nothing about any machine. No chip's code names one and no chip depends on one: `crtc.c`, `ppi.c`, `psg.c`, `keyboard.c` and `monitor.c` do not contain the word. A comment may name a machine to justify a decision, and two in `z80.c` do — why the processor implements the NMOS parity bug, and why it leaves the general case of interrupt mode 0 alone — but that is the comment explaining a choice, not the code making one.

The machine wiring gets its own file, and it is the only one that knows these chips are soldered into a CPC: the memory map, the I/O decode, the board's video address wiring, and the clock that divides between the chips. The monitor is not even that — it is a cathode ray tube, and a tube will take composite sync from anything that emits it.

The practical result is that a second machine built around the same parts inherits them unchanged.

## What this interface promises

It is young, and it is shaped by its two hosts: the command line in this repository, and the [player](https://github.com/tulinmola/colophon-player), which compiles the same C to WebAssembly. It will change as the machine grows, and no version of it has been declared stable.

A host that needs to reach into a structure from another language should generate what it needs from these headers rather than write it down. The player does exactly that: the field offsets its panels read are produced by a generator at build time, so renaming a field breaks a build instead of leaving a table quietly reporting a number that is no longer true.
