---
title: The command line
description: Booting a machine, typing at it, and carrying away a picture, a snapshot, or a map of everything it wrote.
order: 3
---

The command line is the plainest host there is: it builds a machine, runs it for a fixed number of frames, types whatever it was told to, and writes out what it was asked for. Nothing consults a clock, so the same command writes the same bytes every time.

A C compiler and `make` are the whole toolchain. `make` builds, `make roms` fetches the firmware once, and the binary lands in `build/emulator`.

## The two commands

```sh
build/emulator boot [options]
build/emulator run SNAPSHOT.sna [options]
```

`boot` starts a machine from reset. `run` picks one up from a snapshot, fitting the ROMs first so that the snapshot lands in a machine of the right shape. Both then run their frames, type, and write.

```sh
build/emulator boot --screenshot ready.png
build/emulator boot --machine cpc464 --screenshot ready.png
build/emulator boot --type 'PRINT 2+2\n' --screenshot sum.png
build/emulator boot --type 'MODE 0\n' --save state.sna
build/emulator run state.sna --type 'BORDER 6\n' --screenshot resumed.png
build/emulator boot --full-raster --screenshot raster.png
build/emulator boot --type 'PRINT 2+2\n' --writes heat.png
```

## The options

| Option | Effect |
| --- | --- |
| `--machine NAME` | Which machine to build: `cpc6128`, `cpc664` or `cpc464`. The default is `cpc6128`. |
| `--roms DIRECTORY` | Where the ROM images are. The default is `roms`. |
| `--frames N` | Frames to run before typing. The default is 78. |
| `--type TEXT` | Type this once the machine has booted. |
| `--sixty-hz` | Wire the refresh link for 60Hz. The firmware reads it and programs the 6845 from a different table. |
| `--screenshot PATH` | Write the screen here as a PNG. |
| `--writes PATH` | Write a map of memory writes here as a PNG. |
| `--save PATH` | Write the machine here as an SNA snapshot. |
| `--full-raster` | The whole beam path instead of the picture: sync, blanking, the border in its entirety, and the corner the flyback never sweeps. |
| `--no-double` | One image line per raster line, squashed. |

`emulator --help` prints the same list, and is the copy that cannot fall behind the code.

`--type` takes five escapes: `\n` for Return, `\t` for Tab, `\e` for Escape, `\b` for Del, and `\\` for a backslash itself. A character the UK keyboard cannot produce is refused rather than dropped.

Typing is done by holding keys down, not by injecting characters. The firmware scans the keyboard once a frame off the 50Hz tick, so a key must be held for at least one scan to be seen and released for at least one more to be seen let go — which works out at nine characters a second of emulated time, and means what reaches BASIC went through the matrix, the 8255 and the sound chip exactly as a typist's keystroke would.

## Why 78 frames

The 6128's boot screen stops changing at frame 42, measured by counting the text's pixels frame by frame; the other two machines settle sooner. The default is twice that, which costs a fraction of a second and leaves room for a machine that dawdles.

Wait states moved that number only from 39. The firmware's boot waits on the 300Hz ticker far more than it computes, so a processor a quarter slower barely shows — which is worth knowing before treating a successful boot as evidence about timing. It is not.

## The picture

By default the screenshot is the window a monitor shows: 768 by 544, the picture with a border around it and the frame flyback left out, each raster line drawn twice so the image stands at the proportions a screen had. `--no-double` gives the raster its true height instead, 768 by 272. `--full-raster` crops nothing at all: the whole beam path, 1024 samples by 312 lines — doubled to 624 like everything else, unless `--no-double` says otherwise — with the sync, the blanking, and the corner the flyback never sweeps.

The PNGs are uncompressed. The format allows it, and it saves us a compressor to get wrong.

## The map of writes

`--writes` draws every write the machine made, on the screen where the beam put it — the same crop and the same line doubling the screenshot uses, so that the two images lie over one another exactly.

Where a byte lands on the picture is recorded rather than calculated. Each character clock the host reads the address the 6845 actually emitted and attributes it to the samples the beam has just painted, honouring the microsecond between fetch and display. Inverting the address formula instead would need the CRTC's configuration to hold still for a whole frame, and every program worth reading breaks that assumption.

The scale is logarithmic and taken over displayed bytes alone. The firmware's stack is written a hundred thousand times harder than any pixel, and letting it set the top of the range flattens everything the picture exists to show.

What it shows that a screenshot cannot is history. The first run of it turned up two hot bands above the visible text where nothing is drawn at all: writes the boot made and then scrolled away. The screenshot shows the end state; the map shows what happened.

The machine is told nothing about any of this. See [observation](observation.en.md) for why there is nothing to switch on.
