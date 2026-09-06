---
title: The emulator
description: The instrument — old machines built a chip at a time in C, stepped one clock at a time, and readable from outside without disturbing them.
---

A colophon is an argument, and an argument is worth no more than the instrument that made it. This is the instrument: old machines built one chip at a time, in C, with nothing beneath them but a compiler. An Amstrad CPC came first and is the fullest; a ZX Spectrum stands beside it; the shape is meant to take more.

They are machines before they are programs. The processor is not asked to run an instruction and report back — it is ticked once per clock cycle and drives its pins, and the wiring around it decides what those pins mean. On one board a 6845 counts out the frame one character at a time and a Gate Array turns bytes into colour, holds the processor off the memory three cycles in four, and raises the interrupt three hundred times a second; on the other a single Ferranti array does all of it at once. A monitor is handed one composite sync wire and separates both locks out of it the way a tube does, and it does not care which board handed it over. Not one of the chips knows which machine it is in; one file for each machine knows what they are soldered into.

That shape is not a matter of taste. A machine assembled from parts that each answer to their own datasheet can be graded part by part against those datasheets, and a machine whose every value is a plain structure can be read while it runs by something that was never built into it. Both properties are the difference between an emulator that looks right and one that can be used as evidence.

## The pages

- [The machine](machine.en.md) — what it does today, chip by chip, and what it does not.
- [The evidence](evidence.en.md) — the tiers of tests, what each proves, and what each cannot see.
- [The command line](command-line.en.md) — booting a machine, typing at it, and carrying away a picture or a snapshot.
- [The core](core.en.md) — the interface a host builds on: the tick, the pin masks, and the two things the core refuses to do.
- [Observation](observation.en.md) — how a debugger attaches to a machine that had nothing added to it.

## Where it stands

There is something to play, and more of the machine to see than there is of the game. A CPC boots its own firmware to the Ready prompt, takes what is typed at it, runs at the speed the hardware ran, reads a disc through a real µPD765, and can be stopped and picked up again. A Spectrum boots its own firmware too, answers what is typed at it, and contends for its memory the way the ULA did — but has no tape, so what can be put into it is a snapshot and nothing else. [The machine](machine.en.md) is the full accounting, and it is honest about the sound that is silent, the four CRTC types that are not there, and what the Spectrum still owes.

The [player](https://github.com/tulinmola/colophon-player) carries these machines into a page and sets a debugger beside them: the processor, the memory, the screen, the drive and the track under its head, each on a panel of its own, and a few seconds of the recent past to step back through. That is where the games are played, and it is the reason the machine is built to be read while it runs rather than only after it stops.
