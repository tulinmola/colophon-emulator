/*
 * snapshot.h — the SNA snapshot format, read and written.
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
 * them.
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
 */
#ifndef COLOPHON_SNAPSHOT_H
#define COLOPHON_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpc.h"

#define SNAPSHOT_HEADER_SIZE 256

/* How many bytes writing this machine will need. */
size_t snapshot_size(const cpc_t *cpc);

/* Restore a machine from a snapshot. Returns false having pointed `problem`
 * at a sentence saying what is wrong with the bytes. */
bool snapshot_load(cpc_t *cpc, const uint8_t *bytes, size_t length, const char **problem);

/* Write a machine as a version 1 snapshot; `capacity` must be at least
 * snapshot_size(). */
bool snapshot_save(const cpc_t *cpc, uint8_t *bytes, size_t capacity, const char **problem);

#endif
