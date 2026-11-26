/*
 * ula.c — the frame, the serialiser, and the slot the chip keeps for itself.
 */
#include "ula.h"

#include <string.h>

/* Each type's frame, by the sources in ula.h. The 5C and 6C's is
   libspectrum's, and its 14336 divides exactly by a line, so the interrupt
   falls at the same offset into a line as the first displayed byte, 64
   lines earlier ("ZX Spectrum Compatible Interrupts"). The 7K010's line and
   frame are Rak's and Alford's, its 63 lines above the picture Farrow's and
   the FAQ's, and its line's split libspectrum's. On an early chip its first
   contended T-state is the FAQ's 14361 and its first displayed byte
   libspectrum's 14362; a late chip has both a T-state later. */
static const ula_frame frames[] = {
    [ULA_5C_6C_EARLY] =
        {
            .ticks_per_line = 224,
            .lines_per_frame = 312,
            .ticks_per_frame = 224 * 312,
            .retrace_ticks = 48,
            .first_display_line = 64,
            .first_display_tick = 14336,
            .interrupt_ticks = 32,
        },
    [ULA_7K010_LATE] =
        {
            .ticks_per_line = 228,
            .lines_per_frame = 311,
            .ticks_per_frame = 228 * 311,
            .retrace_ticks = 52,
            .first_display_line = 63,
            .first_display_tick = 14363,
            .interrupt_ticks = 36,
        },
};

/* Four T-states to a display byte: eight pixels at two a T-state. */
#define TICKS_PER_BYTE 4

static uint16_t display_column(const ula_frame *frame) {
  return (uint16_t)(frame->retrace_ticks + ULA_LEFT_BORDER_TICKS);
}

/* A raster line begins at its retrace; the frame counter begins at the
   interrupt. They are this far apart, which puts T-state 0 on line 0, inside
   the vertical sync: on a 5C or 6C directly above the first pixel the chip
   will ever paint, and on a late 7K010 one T-state to the right of it. */
static uint32_t line_origin(const ula_frame *frame) {
  return frame->first_display_line * frame->ticks_per_line + display_column(frame) -
         frame->first_display_tick;
}

/* The chip wants the bus one T-state before the first byte it displays, and
   keeps wanting it for as long as it is reading the screen: on a 5C or 6C
   the first contended T-state of a frame is 14335, "just one cycle before
   the top left corner is reached" (the 48K reference), and an early 7K010's
   two stand as far apart, the 128K reference's 14361 against libspectrum's
   14362. The pattern runs the 192 lines of the picture out. */
static uint32_t first_contended_tick(const ula_frame *frame) {
  return frame->first_display_tick - 1;
}

static bool in_display(const ula_t *ula) {
  const ula_frame *frame = &ula->frame;
  uint16_t left = display_column(frame);
  return ula->line >= frame->first_display_line &&
         ula->line < frame->first_display_line + ULA_DISPLAY_LINES && ula->column >= left &&
         ula->column < left + ULA_DISPLAY_TICKS;
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

bool ula_fetch(const ula_t *ula, uint16_t *display, uint16_t *attribute) {
  if (!in_display(ula)) {
    return false;
  }
  int row = ula->line - ula->frame.first_display_line;
  int character = (ula->column - display_column(&ula->frame)) / TICKS_PER_BYTE;
  *display = ula_display_address(row, character);
  *attribute = ula_attribute_address(row, character);
  return true;
}

void ula_video(const ula_t *ula, uint8_t display, uint8_t attribute,
               uint8_t samples[ULA_SAMPLES_PER_TICK]) {
  if (ula_csync(ula)) {
    samples[0] = samples[1] = 0; /* black: the beam carries no picture here */
    return;
  }
  if (!in_display(ula)) {
    samples[0] = samples[1] = ula->border;
    return;
  }

  /* The attribute byte is FLASH, BRIGHT, then paper and ink as three bits
     each, green then red then blue — the order a sample keeps, so the
     colour passes through untouched ("Spectrum Video Modes"). FLASH swaps
     ink and paper every sixteenth frame (comp.sys.sinclair FAQ). */
  uint8_t ink = attribute & 0x07;
  uint8_t paper = (attribute >> 3) & 0x07;
  if ((attribute & 0x80) && (ula->frame_count & 0x10)) {
    uint8_t swapped = ink;
    ink = paper;
    paper = swapped;
  }
  uint8_t bright = (attribute & 0x40) ? 0x08 : 0x00;

  int pixel = ((ula->column - display_column(&ula->frame)) % TICKS_PER_BYTE) * ULA_SAMPLES_PER_TICK;
  for (int index = 0; index < ULA_SAMPLES_PER_TICK; index++) {
    bool lit = (display >> (7 - (pixel + index))) & 1;
    samples[index] = (uint8_t)((lit ? ink : paper) | bright);
  }
}

uint8_t ula_contention(const ula_t *ula, uint32_t frame_tick) {
  /* Six T-states owed at the head of the slot, falling to none for the two
     the chip leaves the processor ("Contended memory"), on a 7K010 as on a
     5C or 6C (the 128K reference). */
  static const uint8_t owed[8] = {6, 5, 4, 3, 2, 1, 0, 0};
  const ula_frame *frame = &ula->frame;
  uint32_t tick = frame_tick % frame->ticks_per_frame;
  uint32_t first = first_contended_tick(frame);
  if (tick < first) {
    return 0;
  }
  uint32_t since_first = tick - first;
  uint32_t tick_in_line = since_first % frame->ticks_per_line;
  if (since_first >= ULA_DISPLAY_LINES * (uint32_t)frame->ticks_per_line ||
      tick_in_line >= ULA_DISPLAY_TICKS) {
    return 0;
  }
  return owed[tick_in_line % 8];
}

/* The lowest three bits are the border, bit 3 the MIC socket and bit 4 the
   loudspeaker; the top three reach nothing (comp.sys.sinclair FAQ). */
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
