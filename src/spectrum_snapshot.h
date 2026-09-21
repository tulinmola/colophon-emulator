/*
 * spectrum_snapshot.h — the Spectrum's SNA snapshot format, read and written.
 *
 * A snapshot is a machine caught mid-flight: twenty-seven bytes of registers
 * and then the whole of a 48K machine's RAM. It carries no ROM, so the
 * caller fits that first and loads the snapshot into a machine already of
 * the right shape.
 *
 * The format shares its extension with the CPC's and nothing else. The CPC's
 * announces itself — eight bytes of "MV - SNA" and a version — while this one
 * has neither signature nor version, and is told from anything else only by
 * being exactly 49179 bytes long. Nothing may sniff a file to decide which
 * reader to use; the caller knows which machine it is building and reaches
 * for that machine's reader.
 *
 * The format has one defect and it is not repairable, only declared. There
 * is nowhere in the header for the program counter: it goes where a PUSH
 * would have put it, so the two bytes below SP are spent and reading one
 * pops them back. A machine whose stack was full at the moment it was caught
 * loses two bytes of whatever lay beneath, and the format's own description
 * says so. The 128K revision fixed this by giving PC a field of its own; the
 * 48K one is what it is.
 *
 * Those bytes are lost in the snapshot and not in the machine. Writing one
 * does not damage what it describes, which is why a machine can be written
 * down as often as a caller likes.
 *
 * What the format does not carry is where the beam stood. A machine restored
 * from one picks its program up exactly and begins its frame again from the
 * top, so it cannot be set running beside the machine it was taken from and
 * expected to keep step. Nothing in the 48K format has room for the ULA's
 * position, and nothing here invents one.
 *
 * Interrupts come back the same roundabout way. The header holds IFF2 alone,
 * because the restart is meant to be a RETN and RETN copies IFF2 into IFF1.
 * Reading one sets both flags from that single bit, which is where a RETN
 * would have left them, without spending the instruction.
 *
 * What that loses is a machine caught inside its own interrupt handler,
 * where IFF1 is clear and IFF2 is not. The format has one bit for the pair,
 * so such a machine comes back with interrupts already enabled instead of
 * still masked. That is the format's, not this reader's, and there is
 * nowhere to put the difference.
 *
 * There is no file handling here: bytes in, bytes out, like everything else
 * in this directory.
 *
 * Sources:
 * - "Emulator file formats" (World of Spectrum FAQ),
 *   https://worldofspectrum.org/faq/reference/formats.htm — the header field
 *   by field, the total length, the program counter on the stack and the two
 *   bytes it costs, and that "when the registers have been loaded, a RETN
 *   command is required to start the program".
 */
#ifndef COLOPHON_SPECTRUM_SNAPSHOT_H
#define COLOPHON_SPECTRUM_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectrum.h"

#define SPECTRUM_SNAPSHOT_HEADER_SIZE 27
/* The only length a 48K snapshot has: the header and the whole of the RAM. */
#define SPECTRUM_SNAPSHOT_SIZE (SPECTRUM_SNAPSHOT_HEADER_SIZE + SPECTRUM_RAM_48K)

/* How many bytes writing this machine will need. Zero for a machine a
 * snapshot cannot describe, which is any that is not a 48K. */
size_t spectrum_snapshot_size(const spectrum_t *spectrum);

/* Restore a machine from a snapshot. Returns false having pointed `problem`
 * at a sentence saying what is wrong with the bytes. */
bool spectrum_snapshot_load(spectrum_t *spectrum, const uint8_t *bytes, size_t length,
                            const char **problem);

/* Write a machine as a snapshot; `capacity` must be at least the size above.
 * The processor must be between instructions. The machine is not modified:
 * the two bytes below SP are spent in the snapshot alone. */
bool spectrum_snapshot_save(const spectrum_t *spectrum, uint8_t *bytes, size_t capacity,
                            const char **problem);

#endif
