/*
 * spectrum_snapshot.h — the Spectrum's SNA snapshot formats, the 48K's and
 * the 128K's, read and written.
 *
 * A snapshot is a machine caught mid-flight: twenty-seven bytes of registers
 * and then the RAM. It carries no ROM, so the caller fits that first and
 * loads the snapshot into a machine already of the right shape. A 48K's is
 * the whole of its 48K; a +2's, in the 128K format, is banks 5 and 2 and the
 * one paged at 0xC000, laid out where a 48K's RAM is, then the program
 * counter, the bank register, a byte for TR-DOS, and the remaining banks in
 * ascending order. Each model reads and writes its own format and refuses
 * the other's.
 *
 * The formats share their extension with the CPC's and nothing else. The
 * CPC's announces itself — eight bytes of "MV - SNA" and a version — while
 * these have neither signature nor version, and are told from anything else
 * only by their lengths: 49179 bytes for a 48K, and for a 128K 131103, or
 * 147487 when the bank paged at 0xC000 is 5 or 2, which the format then
 * holds twice. Nothing may sniff a file to decide which reader to use; the
 * caller knows which machine it is building and reaches for that machine's
 * reader.
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
 * Neither format carries where the beam stood, nor how many frames it had
 * drawn, which FLASH keeps time by. A machine restored from one picks its
 * program up exactly and begins a frame as one just switched on does: the
 * beam at the top, the monitor waiting for it there, and the flashing
 * characters at the start of their count, whatever the machine was doing
 * before the load. So it cannot be set running beside the machine it was
 * taken from and expected to keep step. There is no room for the ULA's
 * position, and nothing here invents one.
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
 * There is no file handling here: bytes in, bytes out, like everything else
 * in this directory.
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
 */
#ifndef COLOPHON_SPECTRUM_SNAPSHOT_H
#define COLOPHON_SPECTRUM_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectrum.h"

#define SPECTRUM_SNAPSHOT_HEADER_SIZE 27
/* The only length a 48K snapshot has: the header and the whole of the RAM. */
#define SPECTRUM_SNAPSHOT_48K_SIZE (SPECTRUM_SNAPSHOT_HEADER_SIZE + SPECTRUM_RAM_48K)
/* A 128K snapshot's two: the header, three banks, four bytes of its own and
   the five banks left, or six when the bank paged is 5 or 2 and so repeats
   one of the first two. */
#define SPECTRUM_SNAPSHOT_128K_SIZE                                                                \
  (SPECTRUM_SNAPSHOT_HEADER_SIZE + 3 * SPECTRUM_BANK_SIZE + 4 + 5 * SPECTRUM_BANK_SIZE)
#define SPECTRUM_SNAPSHOT_128K_REPEATING_SIZE (SPECTRUM_SNAPSHOT_128K_SIZE + SPECTRUM_BANK_SIZE)

/* How many bytes writing this machine will need, which for a +2 turns on the
 * bank it has paged. Zero for a machine no snapshot can describe: a 48K
 * without 48K fitted, or a +2 without 128K. */
size_t spectrum_snapshot_size(const spectrum_t *spectrum);

/* Restore a machine from a snapshot. Returns false having pointed `problem`
 * at a sentence saying what is wrong with the bytes, or why this machine
 * cannot take them. */
bool spectrum_snapshot_load(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                            const char **problem);

/* Write a machine as a snapshot in its model's format; `capacity` must be at
 * least the size above. The processor must be between instructions. The
 * machine is not modified: a 48K's two bytes below SP are spent in the
 * snapshot alone. */
bool spectrum_snapshot_save(const spectrum_t *spectrum, uint8_t *bytes, size_t capacity,
                            const char **problem);

#endif
