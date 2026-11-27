/*
 * ula.c — the frame, the fetch and the drawing, and the slot the chip keeps
 * for itself.
 */
#include "ula.h"

#include <string.h>

/* Each type's frame, by the sources in ula.h. The 5C and 6C's line and
   frame are libspectrum's, and their first contended T-state Rak's 14336.
   The 7K010's line and frame are Rak's and Alford's, its 63 lines above the
   picture Farrow's and the FAQ's, and its line's split libspectrum's. An
   early chip's first contended T-state is Rak's 14362, which the FAQ counts
   as 14361; a late chip's is a T-state later. */
static const ula_frame frames[] = {
    [ULA_5C_6C_EARLY] =
        {
            .ticks_per_line = 224,
            .lines_per_frame = 312,
            .ticks_per_frame = 224 * 312,
            .retrace_ticks = 48,
            .first_display_line = 64,
            .first_contended_tick = 14336,
            .interrupt_ticks = 32,
        },
    [ULA_7K010_LATE] =
        {
            .ticks_per_line = 228,
            .lines_per_frame = 311,
            .ticks_per_frame = 228 * 311,
            .retrace_ticks = 52,
            .first_display_line = 63,
            .first_contended_tick = 14363,
            .interrupt_ticks = 36,
        },
};

/* Four T-states to a character: eight pixels at two a T-state. */
#define TICKS_PER_BYTE 4

/* A slot is two characters, and begins on a T-state owing the processor
   six. The chip fetches on its fourth to seventh T-states and is idle on
   its eighth and the next slot's first three ("Floating bus", "Memory
   Contention Revised and Snow"). */
#define TICKS_PER_SLOT 8
#define FIRST_FETCH_IN_SLOT 3
#define FETCHES_PER_SLOT 4

/* The chip begins drawing a slot's first character on the slot's seventh
   T-state. Rak's table has Bobrowski's btime put the border change at the
   picture's left edge for an OUT whose machine cycle begins on any of the
   four T-states from a slot's first, a line above; the processor here
   (z80.c) writes on that cycle's third T-state, and spectrum.c draws a
   T-state before running the processor on it, so the border is taken on the
   T-state after a write at the soonest. Taken every four, the first that
   all four of those writes have reached falls on the slot's seventh. */
#define FIRST_DRAWN_IN_SLOT 6

static uint16_t display_column(const ula_frame *frame) {
  return (uint16_t)(frame->retrace_ticks + ULA_LEFT_BORDER_TICKS);
}

/* A raster line begins at its retrace; the frame counter begins at the
   interrupt. They are this far apart, which puts the T-state the chip first
   draws the picture on at the picture's left edge, and T-state 0 on line 0,
   inside the vertical sync. */
static uint32_t line_origin(const ula_frame *frame) {
  return frame->first_display_line * frame->ticks_per_line + display_column(frame) -
         (frame->first_contended_tick + FIRST_DRAWN_IN_SLOT);
}

static bool in_display(const ula_t *ula) {
  const ula_frame *frame = &ula->frame;
  uint16_t left = display_column(frame);
  return ula->line >= frame->first_display_line &&
         ula->line < frame->first_display_line + ULA_DISPLAY_LINES && ula->column >= left &&
         ula->column < left + ULA_DISPLAY_TICKS;
}

/* T-states since the line's first fetch, or -1 on a line with none. */
static int since_first_fetch(const ula_t *ula) {
  const ula_frame *frame = &ula->frame;
  if (ula->line < frame->first_display_line ||
      ula->line >= frame->first_display_line + ULA_DISPLAY_LINES) {
    return -1;
  }
  return ula->column - (display_column(frame) - FIRST_DRAWN_IN_SLOT + FIRST_FETCH_IN_SLOT);
}

static bool fetching(int since) {
  return since >= 0 && since < ULA_DISPLAY_TICKS && since % TICKS_PER_SLOT < FETCHES_PER_SLOT;
}

/* A15..A0 = 0 1 0 R4 R3 P2 P1 P0 R2 R1 R0 C4 C3 C2 C1 C0. The pixel row
   splits into the character row R above it and the row P within it: the
   screen is divided into thirds by R4 R3, then into pixel rows by P, then
   into character rows by R2 R1 R0 ("Spectrum Video Modes"). */
uint16_t ula_display_address(int row, int column) {
  return (uint16_t)(0x4000 | ((row & 0xC0) << 5) | ((row & 0x07) << 8) | ((row & 0x38) << 2) |
                    (column & 0x1F));
}

uint16_t ula_attribute_address(int row, int column) {
  return (uint16_t)(0x5800 | ((row & 0xF8) << 2) | (column & 0x1F));
}

void ula_seek(ula_t *ula, uint32_t frame_tick) {
  const ula_frame *frame = &ula->frame;
  ula->frame_tick = frame_tick % frame->ticks_per_frame;
  uint32_t raster = (ula->frame_tick + line_origin(frame)) % frame->ticks_per_frame;
  ula->line = (uint16_t)(raster / frame->ticks_per_line);
  ula->column = (uint16_t)(raster % frame->ticks_per_line);
}

void ula_init(ula_t *ula, ula_type type) {
  memset(ula, 0, sizeof *ula);
  ula->frame = frames[type];
  ula_seek(ula, 0);
}

/* A slot's fetches go bitmap, attribute, bitmap, attribute: two characters
   ("Floating bus"). */
bool ula_fetch(const ula_t *ula, uint16_t *address) {
  int since = since_first_fetch(ula);
  if (!fetching(since)) {
    return false;
  }
  int row = ula->line - ula->frame.first_display_line;
  int in_slot = since % TICKS_PER_SLOT;
  int character = since / TICKS_PER_SLOT * 2 + in_slot / 2;
  *address = in_slot % 2 == 0 ? ula_display_address(row, character)
                              : ula_attribute_address(row, character);
  return true;
}

void ula_latch(ula_t *ula, uint8_t data) {
  int since = since_first_fetch(ula);
  if (!fetching(since)) {
    return;
  }
  int in_slot = since % TICKS_PER_SLOT;
  if (in_slot % 2 == 0) {
    ula->fetched_bitmap[in_slot / 2] = data;
  } else {
    ula->fetched_attribute[in_slot / 2] = data;
  }
}

void ula_video(ula_t *ula, uint8_t samples[ULA_SAMPLES_PER_TICK]) {
  uint16_t left = display_column(&ula->frame);
  /* The column past the picture's left edge. Left of it the subtraction
     wraps, unsigned, and since 65536 is a multiple of four the position
     within a character still comes out right. */
  uint16_t from_left = (uint16_t)(ula->column - left);
  if (from_left % TICKS_PER_BYTE == 0) {
    if (in_display(ula)) {
      int character = from_left / TICKS_PER_BYTE;
      ula->drawn_bitmap = ula->fetched_bitmap[character % 2];
      ula->drawn_attribute = ula->fetched_attribute[character % 2];
    } else {
      ula->drawn_bitmap = 0;
      ula->drawn_attribute = (uint8_t)(ula->border << 3);
    }
  }

  if (ula_csync(ula)) {
    samples[0] = samples[1] = 0; /* black: the beam carries no picture here */
    return;
  }

  /* The attribute byte is FLASH, BRIGHT, then paper and ink as three bits
     each, green then red then blue — the order a sample keeps, so the
     colour passes through untouched ("Spectrum Video Modes"). FLASH swaps
     ink and paper every sixteenth frame (comp.sys.sinclair FAQ). */
  uint8_t attribute = ula->drawn_attribute;
  uint8_t ink = attribute & 0x07;
  uint8_t paper = (attribute >> 3) & 0x07;
  if ((attribute & 0x80) && (ula->frame_count & 0x10)) {
    uint8_t swapped = ink;
    ink = paper;
    paper = swapped;
  }
  uint8_t bright = (attribute & 0x40) ? 0x08 : 0x00;

  int pixel = (from_left % TICKS_PER_BYTE) * ULA_SAMPLES_PER_TICK;
  for (int index = 0; index < ULA_SAMPLES_PER_TICK; index++) {
    bool lit = (ula->drawn_bitmap >> (7 - (pixel + index))) & 1;
    samples[index] = (uint8_t)((lit ? ink : paper) | bright);
  }
}

uint8_t ula_contention(const ula_t *ula, uint32_t frame_tick) {
  /* Six T-states owed at the head of the slot, falling to none for the two
     the chip leaves the processor ("Contended memory"), on a 7K010 as on a
     5C or 6C (the 128K reference). */
  static const uint8_t owed[TICKS_PER_SLOT] = {6, 5, 4, 3, 2, 1, 0, 0};
  const ula_frame *frame = &ula->frame;
  uint32_t tick = frame_tick % frame->ticks_per_frame;
  uint32_t first = frame->first_contended_tick;
  if (tick < first) {
    return 0;
  }
  uint32_t since_first = tick - first;
  uint32_t tick_in_line = since_first % frame->ticks_per_line;
  if (since_first >= ULA_DISPLAY_LINES * (uint32_t)frame->ticks_per_line ||
      tick_in_line >= ULA_DISPLAY_TICKS) {
    return 0;
  }
  return owed[tick_in_line % TICKS_PER_SLOT];
}

/* The lowest three bits are the border, bit 3 the MIC socket and bit 4 the
   sound, the loudspeaker on a 48K; the top three reach nothing
   (comp.sys.sinclair FAQ). */
void ula_write(ula_t *ula, uint8_t data) {
  ula->border = data & 0x07;
  ula->microphone = (data & 0x08) != 0;
  ula->speaker = (data & 0x10) != 0;
}

uint32_t ula_rgb(uint8_t sample) {
  uint8_t level = (sample & 0x08) ? 0xFF : 0xD7;
  uint32_t red = (sample & 0x02) ? level : 0;
  uint32_t green = (sample & 0x04) ? level : 0;
  uint32_t blue = (sample & 0x01) ? level : 0;
  return (red << 16) | (green << 8) | blue;
}
