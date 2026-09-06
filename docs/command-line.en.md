---
title: The command line
description: Booting a machine, giving it a disc, typing at it, and carrying away a picture, a snapshot, or a map of everything it wrote.
order: 3
---

The command line is the plainest host there is: it builds a machine, runs it for a fixed number of frames, types whatever it was told to, and writes out what it was asked for. Nothing consults a clock, so the same command writes the same bytes every time.

A C compiler and `make` are the whole toolchain. `make` builds, `make roms` fetches the firmware once — the AMSDOS ROM among it, which the disc interface brings — and the binary lands in `build/emulator`.

## The two commands

```sh
build/emulator boot --machine NAME [options]
build/emulator run SNAPSHOT.sna --machine NAME [options]
```

`boot` starts a machine from reset. `run` picks one up from a snapshot, fitting the ROMs first so that the snapshot lands in a machine of the right shape. Both then run their frames, type, and write.

Which machine must always be said. There is no default and there is not going to be one: the emulator holds several machines and none of them is the ordinary case, so a command that does not name one is a question rather than an instruction. Asked without it, the tool lists what it has.

```sh
build/emulator boot --machine cpc6128 --screenshot ready.png
build/emulator boot --machine cpc464 --type 'PRINT 2+2\n' --screenshot sum.png
build/emulator boot --machine cpc6128 --type 'MODE 0\n' --save state.sna
build/emulator run state.sna --machine cpc6128 --type 'BORDER 6\n' --screenshot resumed.png
build/emulator boot --machine cpc6128 --full-raster --screenshot raster.png
build/emulator boot --machine cpc6128 --type 'PRINT 2+2\n' --writes heat.png
build/emulator boot --machine cpc6128 --disc shaker27.dsk --type 'CAT\n' --wait 150 --screenshot catalogue.png
build/emulator boot --machine spectrum48 --screenshot sinclair.png
build/emulator boot --machine spectrum48 --type 'p2+2\n' --wait 10 --screenshot sum.png
build/emulator boot --machine spectrum48 --save state.sna
build/emulator run state.sna --machine spectrum48 --screenshot resumed.png
```

That last pair is a Spectrum, and the `p` is not a typo. Its forty keys carry upwards of two hundred meanings between them, and at the start of a line a letter is a keyword — so `p` is PRINT. `--type` holds keys down; what they mean is the firmware's business.

## The options

| Option | Effect |
| --- | --- |
| `--machine NAME` | Which machine to build: `cpc6128`, `cpc664`, `cpc464` or `spectrum48`. Required. |
| `--roms DIRECTORY` | Where the ROM images are. The default is `roms`. |
| `--frames N` | Frames to run before typing. The default is each machine's own: 78 on a CPC, 128 on a Spectrum. |
| `--type TEXT` | Type this once the machine has booted. Keys are held down, not characters injected, so what arrives is whatever the firmware makes of the keypress — on a Spectrum, where forty keys carry two hundred meanings, `p` at the start of a line is the PRINT keyword and not a letter. |
| `--wait N` | Frames to run after typing, for a machine that has been given something to do. The default is 0. |
| `--sixty-hz` | A CPC's. Wire the refresh link for 60Hz. The firmware reads it and programs the 6845 from a different table. |
| `--screenshot PATH` | Write the screen here as a PNG. |
| `--writes PATH` | A CPC's. Write a map of memory writes here as a PNG. |
| `--save PATH` | Write the machine here as an SNA snapshot. Each machine writes its own format: they share the name `.sna` and nothing else, and `--machine` decides which is meant rather than the file being sniffed. |
| `--disc PATH` | A CPC's. Put this DSK image in drive A. A 464 gets the disc interface plugged in to take it. |
| `--disc-b PATH` | A CPC's. And this one in drive B. |
| `--save-disc PATH` | A CPC's. Write drive A's disc here when the run is done, in the extended layout. Nothing is ever written back to the image that was given. |
| `--full-raster` | The whole beam path instead of the picture: sync, blanking, the border in its entirety, and the corner the flyback never sweeps. |
| `--no-double` | One image line per raster line, squashed. |

`emulator --help` prints the same list, and is the copy that cannot fall behind the code.

The five options marked a CPC's are refused on a Spectrum rather than ignored, and the machine says which one it will not do.

`--type` takes five escapes on a CPC: `\n` for Return, `\t` for Tab, `\e` for Escape, `\b` for Del, and `\\` for a backslash itself. A Spectrum has keys for none of the middle three and takes `\n` and `\\` alone. A character the machine's keyboard cannot produce is refused rather than dropped.

A disc is read whole into memory, with room after it for every track to be formatted once more, and the medium borrows the buffer for the run. What the machine writes lands in that copy, and reaches a file only through `--save-disc`; the image named by `--disc` is never touched.

Typing is done by holding keys down, not by injecting characters. The firmware scans the keyboard once a frame off the 50Hz tick, so a key must be held for at least one scan to be seen and released for at least one more to be seen let go — which works out at nine characters a second of emulated time, and means what reaches BASIC went through the matrix, the 8255 and the sound chip exactly as a typist's keystroke would.

## Why those frame counts

The 6128's boot screen stops changing at frame 42, measured by counting the text's pixels frame by frame; the other two CPCs settle sooner. The default is twice that, which costs a fraction of a second and leaves room for a machine that dawdles.

Wait states moved that number only from 39. The firmware's boot waits on the 300Hz ticker far more than it computes, so a processor a quarter slower barely shows — which is worth knowing before treating a successful boot as evidence about timing. It is not.

A Spectrum shows its copyright message by frame 50 and its cursor by 65, but takes no keystroke until 85 — measured by typing `p2+2` one frame later each time and reading the answer back off the screen. Before that the first key is dropped and the rest arrive as nonsense, which is the failure a screenshot alone will not show. Its default of 128 is half again as long as the number that works.

## The picture

By default the screenshot is the window a monitor shows, cropped to each machine's own: 768 by 544 on a CPC, 352 by 264 on a Spectrum. The picture with a border around it, and the frame flyback left out. A CPC's raster lines are drawn twice so the image stands at the proportions a screen had; a Spectrum's are not, because at two samples to a T-state they already do. `--no-double` gives a CPC's raster its true height instead, 768 by 272. `--full-raster` crops nothing at all — the whole beam path, sync and blanking and the corner the flyback never sweeps: 1024 samples by 312 lines on a CPC, doubled to 624 unless `--no-double` says otherwise, and 448 by 312 on a Spectrum.

The PNGs are uncompressed. The format allows it, and it saves us a compressor to get wrong.

## The map of writes

This is a CPC's alone, because what it records is read off the 6845.

`--writes` draws every write the machine made, on the screen where the beam put it — the same crop and the same line doubling the screenshot uses, so that the two images lie over one another exactly.

Where a byte lands on the picture is recorded rather than calculated. Each character clock the host reads the address the 6845 actually emitted and attributes it to the samples the beam has just painted, honouring the microsecond between fetch and display. Inverting the address formula instead would need the CRTC's configuration to hold still for a whole frame, and every program worth reading breaks that assumption.

The scale is logarithmic and taken over displayed bytes alone. The firmware's stack is written a hundred thousand times harder than any pixel, and letting it set the top of the range flattens everything the picture exists to show.

What it shows that a screenshot cannot is history. The first run of it turned up two hot bands above the visible text where nothing is drawn at all: writes the boot made and then scrolled away. The screenshot shows the end state; the map shows what happened.

The machine is told nothing about any of this. See [observation](observation.en.md) for why there is nothing to switch on.
