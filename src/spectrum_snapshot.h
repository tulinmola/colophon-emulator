/*
 * spectrum_snapshot.h — the Spectrum's snapshots: the SNA formats, the
 * 48K's and the 128K's, read and written, and the .z80 format, read.
 *
 * A snapshot is a machine caught mid-flight: its registers and then its
 * RAM. It carries no ROM, so the caller fits that first and loads the
 * snapshot into a machine already of the right shape. Each model takes only
 * the snapshots that describe it, and a snapshot refused leaves the machine
 * as it was. Nothing may sniff a file to decide which reader to use; the
 * caller knows which machine it is building and which format it was given,
 * and reaches for that reader.
 *
 * There is no file handling here: bytes in, bytes out, like everything else
 * in this directory.
 *
 * The SNA formats. Twenty-seven bytes of registers, then the RAM. A 48K's
 * is the whole of its 48K; a +2's, in the 128K format, is banks 5 and 2 and
 * the one paged at 0xC000, laid out where a 48K's RAM is, then the program
 * counter, the bank register, a byte for TR-DOS, and the remaining banks in
 * ascending order. Each model reads and writes its own format and refuses
 * the other's.
 *
 * The formats share their extension with the CPC's and nothing else. The
 * CPC's announces itself — eight bytes of "MV - SNA" and a version — while
 * these have neither signature nor version, and are told from anything else
 * only by their lengths: 49179 bytes for a 48K, and for a 128K 131103, or
 * 147487 when the bank paged at 0xC000 is 5 or 2, which the format then
 * holds twice.
 *
 * The 48K format has one defect and it is not repairable, only declared.
 * There is nowhere in the header for the program counter: it goes where a
 * PUSH would have put it, so the two bytes below SP are spent and reading
 * one pops them back. A machine whose stack was full at the moment it was
 * caught loses two bytes of whatever lay beneath, and the format's own
 * description says so. The 128K revision fixed this by giving PC a field of
 * its own; the 48K one is what it is.
 *
 * Those bytes are lost in the snapshot and not in the machine. Writing one
 * does not damage what it describes, which is why a machine can be written
 * down as often as a caller likes.
 *
 * A 128K snapshot is refused when it contradicts itself — a length that does
 * not fit the bank it pages, or the bank it holds twice held two ways — or
 * asks for TR-DOS, a ROM that its byte names and a +2 has not got. And the
 * format does not say which 128K machine it was taken from. It is read as a
 * +2's, and one taken on a Sinclair 128 while inside the first ROM resumes in
 * the +2's revision of that ROM, whose "ROM routine entry addresses were
 * shifted" (Farrow, below).
 *
 * Neither SNA format carries where the beam stood, nor how many frames it had
 * drawn, which FLASH keeps time by. A machine restored from one picks its
 * program up exactly and begins a frame as one just switched on does: the beam
 * at the top, the monitor waiting for it there, and the flashing characters at
 * the start of their count, whatever the machine was doing before the load. So
 * it cannot be set running beside the machine it was taken from and expected to
 * keep step. There is no room for the ULA's position, and nothing here invents
 * one.
 *
 * Nor does either carry the rest of the last write to the ULA beside the
 * border. Bits 3 and 4, which set the sound, come back clear, so the sound
 * rests until the program next writes to the port. Nor, in the 128K format,
 * any register of the sound chip: a +2 comes back with the chip as reset
 * leaves it, silent until the program writes to it again.
 *
 * Interrupts come back by way of a RETN that is never run. The header the two
 * formats share holds IFF2 alone, because the 48K format's restart is meant
 * to be a RETN and RETN copies IFF2 into IFF1. Reading one sets both flags
 * from that single bit, which is where a RETN would have left them, without
 * spending the instruction.
 *
 * What that loses is a machine that took a non-maskable interrupt with
 * interrupts enabled and has not yet run its RETN, where IFF1 is clear and
 * IFF2 is not. The header has one bit for the pair, so such a machine comes
 * back with interrupts already enabled instead of still masked. That is the
 * header's, not this reader's, and there is nowhere to put the difference.
 *
 * One bit is written that neither format defines. MAME reads IFF1 from
 * bit 0 of the interrupt byte, so a snapshot carrying bit 2 alone resumes
 * there with interrupts disabled. Bit 0 is written with the same flag as
 * bit 2, which is what the RETN would have put in IFF1, so that MAME and
 * this reader resume the same machine; reading ignores it.
 *
 * Neither format has a bit for a halted processor. A machine caught halted is
 * written with its program counter on the HALT, as Fuse writes one (z80.h), so
 * it comes back running the HALT again and halts where it was.
 *
 * A byte holds the interrupt mode, room for modes no instruction sets, and a
 * snapshot naming one above 2 is refused.
 *
 * The .z80 format. Gerton Lunter's emulator Z80 wrote it, and World of
 * Spectrum's archive keeps its snapshots in it; here it is read and never
 * written. It opens with thirty bytes of registers — the SNA's and the program
 * counter, in another order, with IFF1 and IFF2 a byte each — and comes in
 * three versions. The first, from Z80 1.45 and before, is a 48K's alone: its
 * RAM follows the header whole, or compressed and closed by the end marker 00
 * ED ED 00. The second and third leave the header's program counter at zero and
 * follow it with an additional header whose length says which they are: 23
 * bytes for the second, or 21, which Lunter's own z80dump also takes for the
 * second, with fourteen of the sound chip's sixteen registers; 54 for the
 * third, or 55 with a byte for the +3's paging port. The additional header
 * names the hardware and holds the bank register, the sound chip's registers
 * and, in the third version, a count of where the beam stood; and the memory
 * follows in a block for each 16K page, numbered, in any order. Each page the
 * machine is written with must be there once — a 16K needs only its page 8 —
 * and a page it is not written with — a ROM's, the Multiface's, a 128K's bank
 * on a 48K — is refused, though WinZ80's loader takes any from 2 to 10 and
 * Fuse's refuses 0 and passes over 1, 2 and 11; none of 773 files drawn from
 * World of Spectrum's archive holds such a page.
 *
 * Compression replaces a run of one byte with ED ED, the count and the byte,
 * and every other byte stands for itself. A block must expand to exactly one
 * page, using every byte its length gives it, or the file is refused. Two
 * lengths are special. 0xFFFF marks a page stored raw, which Lunter's z80dump
 * knows and WinZ80's loader does not. And 16384 is read two ways: WinZ80 stores
 * a page raw when compressing it would not make it shorter, and gives it that
 * length, while at least one other writer leaves a page compressed even when
 * that makes it longer. A block of 16384 bytes is read compressed when it
 * expands to exactly one page and raw when it does not, which reads both right
 * unless a raw page's bytes, read as runs, happen to come to exactly a page, as
 * they do, for one, when each ED ED in it is followed by 04 and a byte. Of the
 * 24 blocks of that length among the 773, 23 hold no ED ED and read alike
 * either way; one does not, both its readings consistent, and the rule reads it
 * compressed. It is a second-version file, which WinZ80 never wrote.
 *
 * The hardware byte's values 3 to 6 name different machines in the two
 * versions, and the values other emulators added from 7 up turn up in either. A
 * 48K takes a first version's file, and a later one's 48K, 48K with the
 * Interface 1, or 48K with an MGT disc interface; a +2 takes a 128K, a 128K
 * with either interface, or a +2. Bit 7 of byte 37, Spectaculator's addition,
 * makes a 48K a 16K, which only a 48K with 16K fitted takes, and a 128K a +2. A
 * 48K's file is refused by a +2 and a 128K's by a 48K, and the machines no
 * model here is — the SamRam; the +3 and the +2A, which page their memory as a
 * +2 cannot; the Pentagon, the Scorpion, the Didaktik and the Timex machines —
 * by both. A file taken with an interface's ROM paged in was running a ROM no
 * machine here has, and is refused; one taken with it out runs as on the bare
 * machine, since neither interface is built here. A 128K's file resumes in the
 * +2's revisions of the 128's ROMs, as a 128K SNA does: of the 244 128K files
 * among the 773, 6 reset, having run into what Amstrad changed, and all 6 run
 * on when the same machine is given a 128's ROMs.
 *
 * The rest of the header holds the settings of the emulator that wrote it,
 * which change nothing here — issue 2, the interrupt frequency, the video
 * synchronisation, the joystick and its keys, R and LDIR emulation, a sound
 * chip on a 48K and the Fuller box, and the µSpeech — and bytes no machine here
 * could use: 58, Spectator's; 60, the Multiface's; 61 and 62, which say whether
 * ROM or RAM lies below 0x4000 and follow from the rest; and 86, the +3's
 * paging port. Byte 59 is read only where it says whether an MGT interface's
 * ROM is paged in. Nor does bit 4 of byte 12, which in the first version says a
 * SamRam's ROM was switched in, change anything: 16 of the 271 first-version
 * files among the 773 set it, and all 16 run on in Sinclair's ROM, 14 of them
 * picked up inside it. A byte 12 of 255 is taken for 1, as the manual says,
 * border and all, where WinZ80's loader takes the border from it first and
 * gives 7; none of the 773 holds one. Bytes 55 to 62 hold junk in about ten of
 * the 773. A 48K has no sound chip to give the registers to. A +2's takes them
 * through its own bus, so its masks apply; the format keeps no record of how
 * far its generators had run, so they start afresh, the envelope's shape put
 * where it rests if it ends or holds (ay38910.h). Both interrupt flip-flops
 * come back, each from its own byte, and a file whose mode bits name mode 3,
 * which no instruction sets, is refused: one of the 773 does, with interrupts
 * disabled.
 *
 * Version 3's counter puts the beam back where it stood. Byte 57 counts the
 * frame's quarters, from 3 just after the interrupt, and the word before it
 * counts down through each quarter from 17471 on a 48K and 17726 on a 128K;
 * read so, as Fuse reads it, the two name a T-state counted from the interrupt
 * as Fuse counts, which is one less than the frame tick here (ula.h). A count
 * outside that range begins the frame at its top, as the first two versions and
 * the SNA formats do: 89 of the 297 third-version files among the 773 hold one,
 * 10 of them among the files with junk beside it and 79 for no reason found.
 * Many of those in range record the beam only to about a line, their low counts
 * falling on a line's end. WinZ80's loader decodes the counter and leaves it
 * unused, and its writer puts the low count one higher than World of Spectrum's
 * page has it, so its files resume a T-state early, and one taken on a
 * quarter's first T-state at the top. Wherever the beam starts, the monitor
 * waits for the next frame's sync, and FLASH begins its count.
 *
 * Sources:
 * - "Emulator file formats" (World of Spectrum FAQ),
 *   https://worldofspectrum.org/faq/reference/formats.htm — the header field
 *   by field, the total length, the program counter on the stack and the two
 *   bytes it costs, and that "when the registers have been loaded, a RETN
 *   command is required to start the program". And the 128K version: "The
 *   first 49179 bytes of the snapshot are otherwise exactly as described
 *   above", then "PC", "port 0x7ffd setting", "TR-DOS rom paged (1) or not
 *   (0)" and "remaining RAM banks in ascending order", "Total: 131103 or
 *   147487 bytes"; and "The third RAM bank saved is always the one currently
 *   paged, even if this is page 5 or 2 - in this case, the bank is actually
 *   included twice".
 * - Paul Farrow's page on the +2's ROMs,
 *   http://www.fruitcake.plus.com/Sinclair/Spectrum128/ROMDisassembly/Spectrum128ROMDisassembly2.htm
 *   — "ROM routine entry addresses were shifted due to the above changes".
 * - MAME's snapshot reader,
 *   https://github.com/mamedev/mame/blob/d66c1c7/src/mame/sinclair/spec_snqk.cpp
 *   — its own note on the header, "bit 0 contains IFF1, bit 2 contains
 *   IFF2", which is how it reads one.
 * - Gerton Lunter's manual for Z80 3.02, z80.doc §5.5 "File formats",
 *   https://files.mpoli.fi/unpacked/software/misc/misc/z80-302.zip/z80.doc —
 *   the first header and the additional one to byte 85; the compression,
 *   "Only sequences of length at least 5 are coded", "even two ED's are
 *   encoded into ED ED 02 ED", "every byte directly following a single ED
 *   is not taken into a block, for example ED 6*00 is not encoded into ED
 *   ED ED 06 00 but into ED 00 ED ED 05 00", and "The block is terminated
 *   by an end marker, 00 ED ED 00"; "if byte 12 is 255, it has to be
 *   regarded as being 1"; and the pages, "In 48K mode, pages 4,5 and 8 are
 *   saved ... In '128 mode, all pages from 3 to 10 are saved".
 * - Lunter's source for Z80 and WinZ80 4.00, 1999, released under the MIT
 *   licence with his leave,
 *   https://github.com/ZXSpectrumVault/z80-spectrum-emulator/tree/8490cb50567ab03686e3d5aabe10016904cb1221/source-4.00
 *   — winsrc/spectrum.h's "flg2_uspeech 8 // micro-speech emulation",
 *   byte 37's bit 3; winsrc/emulator.c: LoadZ80File, which decodes the
 *   counter and leaves its call to setz80time commented out, "// not yet.
 *   difficult to implement."; pack, which
 *   stores a page raw once its compressed form reaches 16384 bytes, and
 *   unpack, which copies a block of that length; SaveZ80File's counter,
 *   "tstatelo=(tframe/4) - time % (tframe/4)". And utilsrc/z80dump.c,
 *   which takes a 21-byte additional header as the second version's with
 *   fourteen of the sound chip's registers.
 * - "Z80 File Format" (World of Spectrum FAQ, $Id 1.12, 2004),
 *   https://worldofspectrum.org/faq/reference/z80format.htm — the 55-byte
 *   header and byte 86; byte 37's bits 2, 6 and 7, and "If bit 7 of byte
 *   37 is set ... any 48K machine becomes a 16K machine, any 128K machines
 *   becomes a +2"; the machines other emulators numbered from 7, which
 *   "can be seen in either version 2 or version 3 files"; "If
 *   length=0xffff, data is 16384 bytes long and not compressed"; the
 *   counter, which "counts down from 17471 to 0 (17726 in 128K modes)";
 *   and the corrections to Lunter's documentation of the third version's
 *   hardware numbers 2 and 3 and of bytes 61 and 62.
 * - libspectrum's z80.c, Fuse's reader,
 *   https://sourceforge.net/p/fuse-emulator/libspectrum/ci/7a7ed3e12709f5cb8643c7b593866746570422fa/tree/z80.c
 *   — its reading of the counter, "( ( ( extra_header[25] + 1 ) % 4 ) + 1 )
 *   * quarter_tstates - ( extra_header[23] + extra_header[24] * 0x100 + 1
 *   )", "correct, even if it does disagree with Z80 v3.05's `z80dump'",
 *   which is the reading here; and that bit 7 is Spectaculator's.
 * - World of Spectrum's archive, its June 2017 mirror,
 *   https://archive.org/details/World_of_Spectrum_June_2017_Mirror — the
 *   773 files of 400 games and 100 utilities drawn at random and of every
 *   demo, one of them a macOS fork and no snapshot, for what the format's
 *   writers put in it.
 */
#ifndef COLOPHON_SPECTRUM_SNAPSHOT_H
#define COLOPHON_SPECTRUM_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectrum.h"

#define SPECTRUM_SNAPSHOT_SNA_HEADER_SIZE 27
/* The only length a 48K SNA snapshot has: the header and the whole of the
   RAM. */
#define SPECTRUM_SNAPSHOT_SNA_48K_SIZE (SPECTRUM_SNAPSHOT_SNA_HEADER_SIZE + SPECTRUM_RAM_48K)
/* A 128K SNA snapshot's two: the header, three banks, four bytes of its
   own and the five banks left, or six when the bank paged is 5 or 2 and so
   repeats one of the first two. */
#define SPECTRUM_SNAPSHOT_SNA_128K_SIZE                                                            \
  (SPECTRUM_SNAPSHOT_SNA_HEADER_SIZE + 3 * SPECTRUM_BANK_SIZE + 4 + 5 * SPECTRUM_BANK_SIZE)
#define SPECTRUM_SNAPSHOT_SNA_128K_REPEATING_SIZE                                                  \
  (SPECTRUM_SNAPSHOT_SNA_128K_SIZE + SPECTRUM_BANK_SIZE)

/* How many bytes writing this machine will need, which for a +2 turns on the
 * bank it has paged. Zero for a machine no SNA snapshot can describe: a 48K
 * without 48K fitted, or a +2 without 128K. */
size_t spectrum_snapshot_sna_size(const spectrum_t *spectrum);

/* Restore a machine from an SNA snapshot. Returns false having pointed
 * `problem` at a sentence saying what is wrong with the bytes, or why this
 * machine cannot take them, and left the machine as it was. */
bool spectrum_snapshot_load_sna(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                                const char **problem);

/* Restore a machine from a .z80 file. As the SNA reader does, returns false
 * having pointed `problem` at a sentence saying why, and left the machine as
 * it was. */
bool spectrum_snapshot_load_z80(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                                const char **problem);

/* Write a machine as an SNA snapshot in its model's format; `capacity` must
 * be at least the size above. The processor must be between instructions.
 * The machine is not modified: a 48K's two bytes below SP are spent in the
 * snapshot alone. */
bool spectrum_snapshot_save_sna(const spectrum_t *spectrum, uint8_t *bytes, size_t capacity,
                                const char **problem);

#endif
