/*
 * shaker_trace — what a Shaker group does, watched from outside the machine.
 *
 * A group's screen says what it concluded; this says what it did to get
 * there. Every CRTC and Gate Array register it writes, every interrupt it
 * takes and the character quarter the acknowledge began on, set against the
 * CRTC's own counters, so a test's sequence can be read rather than inferred
 * from its verdict. It sees only what cpc_tick() hands back and what the
 * structures leave in the open, which is all a trace needs and keeps it out
 * of the machine it is watching.
 *
 * Off unless SHAKER_TRACE names where to write. Each module runs in a child
 * of its own and each gets a file of its own,
 * SHAKER_TRACE-<module>-crtc<type>.txt, so the two types can be traced side
 * by side; a module that traces nothing leaves no file.
 *
 * Frames are counted as the report counts them: frame N of a group is the
 * N frames run since its key was let go, so a line of frame N had happened
 * by the screen the report names frame N. The frames of the key press itself
 * are numbered at and below zero, and what runs before a module's first group
 * is numbered from the module's start.
 *
 * Every line gives the processor's PC as the tick leaves it: past an OUT, the
 * instruction after it; at an acknowledge, the address the interrupt came in
 * front of, which is where it will return to.
 *
 *   SHAKER_TRACE_FRAMES=from:to   only those frames of each group
 *   SHAKER_TRACE_PC=low:high      only what the processor does with its PC in
 *                                 that range, in hexadecimal; the interrupt
 *                                 requests the Gate Array makes are not the
 *                                 processor's doing and are always given. A
 *                                 PC below &4000 may be the lower ROM or the
 *                                 RAM under it, which only the Gate Array's
 *                                 lower_rom_enabled says
 *   SHAKER_TRACE_EVENTS=...       syncs, reads, or both, comma separated:
 *                                 HSYNC edges with R52's count, and port reads.
 *                                 Off by default, since a line sync every 64
 *                                 microseconds and the drive's polling would
 *                                 bury everything else
 */
#ifndef SHAKER_TRACE_H
#define SHAKER_TRACE_H

#include <stdint.h>

#include "cpc.h"

/* In the parent, once, before the modules are forked: reads the settings and
   says what is wrong with them a single time rather than five. */
void shaker_trace_configure(void);

/* In the child, once it knows which module it is running. */
void shaker_trace_begin(const char *module, uint8_t crtc_type);

/* At the start of each group, once the machine has been put back as the group
   finds it. The frames before counting starts are those the key press takes. */
void shaker_trace_group(const cpc_t *cpc, const char *key, long frames_before_counting);

/* After every tick, with the pins that tick returned. */
void shaker_trace_tick(const cpc_t *cpc, uint64_t pins);

#endif
