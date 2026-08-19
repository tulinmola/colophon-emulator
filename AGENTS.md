# Colophon — agent instructions

Colophon is an Amstrad CPC emulator built for game archaeology: it instruments games from the inside and writes the note that was never written. Understanding, evidence, and a record that outlasts us — not "the best emulator in the world". Read `README.md` for the full prologue.

## Documentation

Documentation lives in `docs/` and is the emulator's own: a page is a markdown file named `<name>.<language>.md`, and it becomes a folder holding `index.<language>.md` the day it carries artifacts, so that what a page cites sits beside the prose citing it. A page's title lives in its front matter and the layout that publishes it sets the heading, so a document has exactly one `<h1>` and the markdown starts at `##`; `order` places a page among its siblings, and two siblings may not claim the same one. Where this documentation sits among the other projects' is not ours to say and is not written here — a repository cannot know what it is being read beside — so `docs/index.en.md` carries no order at all. A link between pages names the markdown file it points at, which is what makes it work in an editor, on GitHub, and once published — so a link to a sibling repository is a URL, never a relative path into a tree that is not there.

The Colophon Project gathers `docs/` at build time and publishes it. Nothing about that site is written down here: it owns its own addresses, and a page that hardcodes one is wrong twice over.

A page describes what the emulator does today. What it is built toward belongs in `README.md`, which is where a reader arriving at the repository starts.

The headers are the reference for the interface, and a page never restates one: a signature, a field, a register layout belongs where it is declared and nowhere else. What a page owes a reader instead is the machine's limits in plain words — what is missing, and what its absence costs them. `docs/machine.en.md` is where those are collected, and it is knowingly a second telling of what the headers declare, so a header that changes what it leaves out is not finished until that page agrees with it.

`notes/` is the quarry, not the record. A note earns a page the day its argument is settled, and the page is written afresh from it rather than promoted wholesale.

## Voice

- The register is a scribe's: plain, declarative, a little antique. Take the manuscript metaphor completely seriously and never wink at it — pointing at the bit kills it.
- Like an illuminated manuscript: mood at the openings, discipline in the middles. A section's first sentence may sing; commands, specs, and rules stay dry.
- Humor only as a byproduct of honesty. None in code comments or error messages: comments pay rent in facts, and error messages are read on bad days.
- Write for 2036. Mood is timeless, jokes are timestamps; the prose meets the same bar as the code.

## Writing style

- One paragraph, one line: markdown is never hard-wrapped. Soft wrap does the work.
- Sources are cited at their point of use, in the code or doc that uses them: link, what we learned, what we changed. No link dumps.
- The README's `Sources` section is a view over those citations, never a collection of its own: a source is promoted there the day code starts citing it, one entry saying what it is and what it gives us.

## Build and test

- `make` builds. `make test` is the fast tier: hermetic, no network, run it on every change. The heavier tiers fetch what they need into `test/data/` or `roms/`, both git-ignored, on first use; `make test-all` runs everything. The Makefile is the list of targets and this file does not repeat it — that list has gone stale here twice.
- C99 with `-Wall -Wextra -Werror`. No external dependencies: we write what we need ourselves.
- Never commit, never push. The human reviews; the human commits.

## Code conventions

- Formatting is never a discussion: `make format` before handing work back, `make format-check` to verify, `make lint` for clang-tidy. Configs in `.clang-format` and `.clang-tidy`; the style is LLVM with 2-space indentation.
- Every comment is a lost battle. The survivors: hardware facts, cited sources, and constraints the code cannot express. Never narrate what the code already says — rename or restructure instead.
- Public identifiers wear their module's name (`z80_tick`, `json_parse_file`); macros wear it in uppercase (`Z80_MREQ`); file-local statics go bare.
- Documented names for documented things: hardware pins, registers, and counters keep their datasheet and Compendium names (`Z80_MREQ`, `wz`, the CRTC's `c4`), glossed once where declared with the fact and its source. Every deviation from the documentation — active levels, prime marks — is declared where it is defined.
- Invented names explain themselves: anything without a page in the primary documentation gets a name that needs no gloss. Abbreviations count as cryptic (`err`, `msg`, `buf`, `cur`): write the word. Abbreviations that are documented names (`RFSH`, `WZ`), the language's own (`int`, `bool`, `argv`, make's `CC` and `SRC`), or the name of a real thing in the repo (the `src/` folder) stay. Single letters only in scopes a few lines long.
- The emulation core (`src/`) does no allocation and no I/O: hosts and tests hand it buffers. Test and tool code may use libc freely.
- Chip modules know nothing about machines: no machine in a chip's code, and no dependency on one. A comment may name a machine to justify a decision — why `src/z80.c` implements the NMOS parity bug, why it leaves interrupt mode 0 alone. Machine wiring gets its own file.
- Bus pins are touched only through the accessors in the chip headers; call sites never handle raw bit positions.
- Nothing exists until something checks it: an opcode when its corpus file passes, a chip's behaviour when a test grades it against the documentation or a suite from outside. Prefer evidence we did not write — a test built from our own understanding agrees with our own mistakes.
