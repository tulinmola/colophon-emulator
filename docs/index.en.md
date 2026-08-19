---
title: The emulator
description: The instrument — an Amstrad CPC built a chip at a time in C, stepped one clock at a time, and readable from outside without disturbing it.
---

A colophon is an argument, and an argument is worth no more than the instrument that made it. This is the instrument: an Amstrad CPC built one chip at a time, in C, with nothing beneath it but a compiler.

It is a machine before it is a program. The processor is not asked to run an instruction and report back — it is ticked once per clock cycle and drives its pins, and the wiring around it decides what those pins mean. The 6845 counts out the frame one character at a time. The Gate Array turns bytes into colour, holds the processor off the memory three cycles in four, and raises the interrupt three hundred times a second. A monitor is handed one composite sync wire and separates both locks out of it the way a tube does. Not one of the chips knows it is in a CPC; a single file knows they are soldered into one.

That shape is not a matter of taste. A machine assembled from parts that each answer to their own datasheet can be graded part by part against those datasheets, and a machine whose every value is a plain structure can be read while it runs by something that was never built into it. Both properties are the difference between an emulator that looks right and one that can be used as evidence.

## The pages

- [The machine](machine.en.md) — what it does today, chip by chip, and what it does not.
- [The evidence](evidence.en.md) — the tiers of tests, what each proves, and what each cannot see.
- [The command line](command-line.en.md) — booting a machine, typing at it, and carrying away a picture or a snapshot.
- [The core](core.en.md) — the interface a host builds on: the tick, the pin masks, and the two things the core refuses to do.
- [Observation](observation.en.md) — how a debugger attaches to a machine that had nothing added to it.

## Where it stands

There is nothing to play yet, and there is something to see. A CPC boots its own firmware to the Ready prompt, takes what is typed at it, runs at the speed the hardware ran, and can be stopped and picked up again. What stands between here and the games is the disc controller. [The machine](machine.en.md) is the full accounting, and it is honest about the sound that is silent and the four CRTC types that are not there.

The [player](https://github.com/tulinmola/colophon-player) carries this machine into a page, where it can be watched panel by panel while it runs.
