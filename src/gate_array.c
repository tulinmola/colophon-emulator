/*
 * gate_array.c — the registers and the interrupt generator.
 */
#include "gate_array.h"

void gate_array_init(gate_array_t *gate_array) {
  *gate_array = (gate_array_t){0};
  gate_array->lower_rom_enabled = true;
  gate_array->upper_rom_enabled = true;
}

void gate_array_write(gate_array_t *gate_array, uint8_t data) {
  switch (data & 0xC0) {
    case 0x00: /* PENR: select a colour register; bit 4 names the border */
      gate_array->pen = (data & 0x10) ? 16 : (data & 0x0F);
      break;
    case 0x40: /* INKR: give the selected register a hardware colour code */
      gate_array->inks[gate_array->pen] = data & 0x1F;
      break;
    case 0x80: /* RMR */
      gate_array->upper_rom_enabled = (data & 0x08) == 0;
      gate_array->lower_rom_enabled = (data & 0x04) == 0;
      gate_array->mode_pending = data & 0x03;
      if (data & 0x10) {
        /* Bit 4 resets R52 and clears the pending request with it
           ("Interrupts on the CPC/CPC+ and KC Compact"). */
        gate_array->r52 = 0;
        gate_array->interrupt_request = false;
      }
      break;
    default: /* 11: the PAL's MMR — another chip's business */
      break;
  }
}

/* R52 counts to 51 and loops; the loop raises the request (Compendium ch.
   27.3.1). */
static void count_hsync_end(gate_array_t *gate_array) {
  gate_array->r52 = (gate_array->r52 + 1) & 0x3F;
  if (gate_array->r52 == 52) {
    gate_array->r52 = 0;
    gate_array->interrupt_request = true;
  }
}

bool gate_array_tick(gate_array_t *gate_array, bool hsync, bool vsync) {
  bool hsync_ended = gate_array->hsync_previous && !hsync;
  bool vsync_started = vsync && !gate_array->vsync_previous;
  gate_array->hsync_previous = hsync;
  gate_array->vsync_previous = vsync;

  if (vsync_started) {
    gate_array->hsyncs_until_vsync_check = 2;
  }
  if (hsync_ended) {
    /* Mode changes take effect after the HSYNC ("The Gate Array"); the
       sub-microsecond placement waits for the pixel rung. The interrupt
       request likewise rises here, where on hardware it is one more
       microsecond along (Compendium ch. 27.6.1). */
    gate_array->mode = gate_array->mode_pending;
    if (gate_array->hsyncs_until_vsync_check > 0) {
      gate_array->hsyncs_until_vsync_check--;
      if (gate_array->hsyncs_until_vsync_check == 0) {
        /* Two HSYNCs after the VSYNC began: an interrupt only if bit 5 of
           R52 is set — the last one comfortably far away — and R52 returns
           to 0 unconditionally (Compendium ch. 27.3.2). */
        if (gate_array->r52 & 0x20) {
          gate_array->interrupt_request = true;
        }
        gate_array->r52 = 0;
      } else {
        count_hsync_end(gate_array);
      }
    } else {
      count_hsync_end(gate_array);
    }
  }
  return gate_array->interrupt_request;
}

void gate_array_interrupt_acknowledged(gate_array_t *gate_array) {
  gate_array->interrupt_request = false;
  gate_array->r52 &= 0x1F;
}
