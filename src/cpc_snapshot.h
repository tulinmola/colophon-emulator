/*
 * cpc_snapshot.h — the CPC's SNA snapshot format, read and written.
 *
 * A snapshot is a machine caught mid-flight: a 256-byte header of every
 * register the CPC has, then a dump of its RAM. It carries no ROMs — only
 * the number of the upper one — so the caller fits those first and loads
 * the snapshot into a machine that is already the right shape.
 *
 * Reading accepts versions 1, 2 and 3, taking the fields they share; the
 * later versions place those at the same offsets and add their own after.
 * Writing produces version 1, which every emulator can read.
 *
 * What version 1 cannot carry is the CRTC's internal counters, so a machine
 * resumed from one restarts its frame rather than continuing mid-raster.
 * Version 3 has room for them at offsets &A9 and &AB, along with the CRTC
 * type, the drive's motor and head — worth reading the day we can act on
 * them. Nor can any version carry where the sound chip's generators stand:
 * a load starts them as reset does, and an envelope whose shape ends or
 * holds is put where it rests.
 *
 * One field departs from the format's notes. Note 17 asks for the PSG's
 * selected register at &5A to be 0 to 15, "For CPCEMU compatibility", but a
 * program can leave the chip unselected, with no register at all, by
 * giving it an address above 15. Such a chip is written as the register it
 * last selected, plus 16, so that a reader that keeps the low four bits
 * finds that register, and any byte above 15 is read back as a chip
 * unselected. CPCEC and Caprice32 write the whole latched byte there
 * instead (Caprice32's src/slotshandler.cpp at a0d78f0,
 * https://github.com/ColinPitrat/caprice32).
 *
 * There is no file handling here: bytes in, bytes out, like everything else
 * in this directory.
 *
 * Sources:
 * - "Snapshot file format" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/snapshot.html — the header layout of
 *   all three versions and the notes on what each field means, including
 *   the two that are easy to get backwards: the PPI's A and B hold their
 *   inputs while C holds its outputs, and the flip-flops the format calls
 *   IFF0 and IFF1 are what everyone else calls IFF1 and IFF2.
 * - CPCEC (Cesar Nicolas-Gonzalez), http://cngsoft.no-ip.org/cpcec.htm,
 *   CPCEC.C in cpcec-20260303 — beside its write of the latched PSG
 *   address at &5A: "INDEX can be invalid, it's the emulator's duty to
 *   behave accordingly".
 */
#ifndef COLOPHON_CPC_SNAPSHOT_H
#define COLOPHON_CPC_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpc.h"

#define CPC_SNAPSHOT_HEADER_SIZE 256

/* How many bytes writing this machine will need. */
size_t cpc_snapshot_size(const cpc_t *cpc);

/* Restore a machine from a snapshot. Returns false having pointed `problem`
 * at a sentence saying what is wrong with the bytes, or why this machine
 * cannot take them: a dump larger than its RAM, or, with no PAL fitted, an
 * MMR other than &C0, the only value it can then hold. */
bool cpc_snapshot_load(cpc_t *cpc, const uint8_t *bytes, size_t length, const char **problem);

/* Write a machine as a version 1 snapshot; `capacity` must be at least
 * cpc_snapshot_size(). */
bool cpc_snapshot_save(const cpc_t *cpc, uint8_t *bytes, size_t capacity, const char **problem);

#endif
