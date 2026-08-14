# Colophon — agent instructions

Colophon is an Amstrad CPC emulator built for game archaeology: it instruments games from the inside and writes the note that was never written. Understanding, evidence, and a record that outlasts us — not "the best emulator in the world". Read `README.md` for the full prologue.

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

- `make` builds. `make test` is the fast tier: hermetic, no network, run it on every change. `make test-single-step` runs the instruction corpus and `make test-exerciser` the acceptance suite; both fetch their material into `test/data/` (git-ignored) on first use. `make test-all` runs everything.
- C99 with `-Wall -Wextra -Werror`. No external dependencies: we write what we need ourselves.
- Never commit, never push. The human reviews; the human commits.

## Code conventions

- Formatting is never a discussion: `make format` before handing work back, `make format-check` to verify, `make lint` for clang-tidy. Configs in `.clang-format` and `.clang-tidy`; the style is LLVM with 2-space indentation.
- Every comment is a lost battle. The survivors: hardware facts, cited sources, and constraints the code cannot express. Never narrate what the code already says — rename or restructure instead.
- Public identifiers wear their module's name (`z80_tick`, `json_parse_file`); macros wear it in uppercase (`Z80_MREQ`); file-local statics go bare.
- Documented names for documented things: hardware pins, registers, and counters keep their datasheet and Compendium names (`Z80_MREQ`, `wz`, a future `c4`), glossed once where declared with the fact and its source. Every deviation from the documentation — active levels, prime marks — is declared where it is defined.
- Invented names explain themselves: anything without a page in the primary documentation gets a name that needs no gloss. Abbreviations count as cryptic (`err`, `msg`, `buf`, `cur`): write the word. Abbreviations that are documented names (`RFSH`, `WZ`), the language's own (`int`, `bool`, `argv`, make's `CC` and `SRC`), or the name of a real thing in the repo (the `src/` folder) stay. Single letters only in scopes a few lines long.
- The emulation core (`src/`) does no allocation and no I/O: hosts and tests hand it buffers. Test and tool code may use libc freely.
- Chip modules know nothing about machines: `src/z80.c` must never say "CPC". Machine wiring gets its own file.
- Bus pins are touched only through the accessors in the chip headers; call sites never handle raw bit positions.
- An opcode exists when its SingleStepTests file passes — not before. New opcodes bring their test file into `tools/fetch-tests.sh` and the Makefile's test run.
