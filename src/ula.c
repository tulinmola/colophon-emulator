/*
 * ula.c — the frame, the serialiser, and the slot the chip keeps for itself.
 */
#include "ula.h"

#include <string.h>

/* A raster line begins at its retrace; the frame counter begins at the
   interrupt. The two origins are this far apart because 14336 divides
   exactly by a line, so the interrupt falls at the same offset into a line
   as the first displayed byte does, 64 lines earlier ("ZX Spectrum
   Compatible Interrupts"). T-state 0 is therefore line 0 column
   DISPLAY_COLUMN — inside the vertical sync, directly above the first pixel
   the chip will ever paint. */
#define LINE_ORIGIN (ULA_RETRACE_TICKS + ULA_LEFT_BORDER_TICKS)
#define DISPLAY_COLUMN LINE_ORIGIN
#define RIGHT_BORDER_COLUMN (DISPLAY_COLUMN + ULA_DISPLAY_TICKS)
#define LAST_DISPLAY_LINE (ULA_FIRST_DISPLAY_LINE + ULA_DISPLAY_LINES)

/* The chip wants the bus one T-state before the first byte it displays, and
   keeps wanting it for as long as it is reading the screen: the first
   contended T-state of a frame is 14335, one before the first displayed one
   ("Contended memory"). */
#define CONTENTION_COLUMN (DISPLAY_COLUMN - 1)

/* Four T-states to a display byte: eight pixels at two a T-state. */
#define TICKS_PER_BYTE 4

static bool in_display(const ula_t *ula) {
  return ula->line >= ULA_FIRST_DISPLAY_LINE && ula->line < LAST_DISPLAY_LINE &&
         ula->column >= DISPLAY_COLUMN && ula->column < RIGHT_BORDER_COLUMN;
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
  uint32_t raster = (frame_tick + LINE_ORIGIN) % ULA_TICKS_PER_FRAME;
  ula->frame_tick = frame_tick % ULA_TICKS_PER_FRAME;
  ula->line = (uint16_t)(raster / ULA_TICKS_PER_LINE);
  ula->column = (uint16_t)(raster % ULA_TICKS_PER_LINE);
}

void ula_init(ula_t *ula) {
  memset(ula, 0, sizeof *ula);
  ula_seek(ula, 0);
}

bool ula_fetch(const ula_t *ula, uint16_t *display, uint16_t *attribute) {
  if (!in_display(ula)) {
    return false;
  }
  int row = ula->line - ULA_FIRST_DISPLAY_LINE;
  int character = (ula->column - DISPLAY_COLUMN) / TICKS_PER_BYTE;
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

  int pixel = ((ula->column - DISPLAY_COLUMN) % TICKS_PER_BYTE) * ULA_SAMPLES_PER_TICK;
  for (int index = 0; index < ULA_SAMPLES_PER_TICK; index++) {
    bool lit = (display >> (7 - (pixel + index))) & 1;
    samples[index] = (uint8_t)((lit ? ink : paper) | bright);
  }
}

bool ula_csync(const ula_t *ula) {
  return ula->line < ULA_VSYNC_LINES || ula->column < ULA_RETRACE_TICKS;
}

bool ula_interrupt(const ula_t *ula) { return ula->frame_tick < ULA_INTERRUPT_TICKS; }

uint8_t ula_contention(const ula_t *ula) {
  /* Six T-states owed at the head of the slot, falling to none for the two
     the chip leaves the CPU ("Contended memory"). */
  static const uint8_t owed[8] = {6, 5, 4, 3, 2, 1, 0, 0};
  if (ula->line < ULA_FIRST_DISPLAY_LINE || ula->line >= LAST_DISPLAY_LINE) {
    return 0;
  }
  if (ula->column < CONTENTION_COLUMN || ula->column >= RIGHT_BORDER_COLUMN - 1) {
    return 0;
  }
  return owed[(ula->column - CONTENTION_COLUMN) % 8];
}

/* The lowest three bits are the border, bit 3 the MIC socket and bit 4 the
   loudspeaker; the top three reach nothing (comp.sys.sinclair FAQ). */
void ula_write(ula_t *ula, uint8_t data) {
  ula->border = data & 0x07;
  ula->microphone = (data & 0x08) != 0;
  ula->speaker = (data & 0x10) != 0;
}

void ula_tick(ula_t *ula) {
  if (++ula->frame_tick == ULA_TICKS_PER_FRAME) {
    ula->frame_tick = 0;
    ula->frame_count++;
  }
  if (++ula->column == ULA_TICKS_PER_LINE) {
    ula->column = 0;
    if (++ula->line == ULA_LINES_PER_FRAME) {
      ula->line = 0;
    }
  }
}
