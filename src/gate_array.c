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
  bool hsync_started = hsync && !gate_array->hsync_previous;
  bool hsync_ended = gate_array->hsync_previous && !hsync;
  bool vsync_started = vsync && !gate_array->vsync_previous;
  gate_array->hsync_previous = hsync;
  gate_array->vsync_previous = vsync;

  if (vsync_started) {
    gate_array->hsyncs_until_vsync_check = 2;
    gate_array->v26 = 0;
    gate_array->vsync_ga = true;
    gate_array->black_vsync = true;
  }

  /* H06 restarts on the CRTC's HSYNC and counts characters from there; the
     Gate Array's own pulse runs from the second to the sixth, so it begins
     2µs late and lasts 4µs at most (Compendium ch. 16.2.2-16.2.3). The
     counter stops where its name does. */
  if (hsync_started) {
    gate_array->h06 = 0;
    gate_array->black_hsync = true;
  } else if (gate_array->h06 < 6) {
    gate_array->h06++;
    if (gate_array->h06 == 2) {
      gate_array->sig_hsync = true;
    } else if (gate_array->h06 == 6) {
      gate_array->sig_hsync = false;
    }
  }

  if (hsync_ended) {
    /* A CRTC HSYNC shorter than the Gate Array's own pulse cuts it short. */
    gate_array->sig_hsync = false;
    gate_array->black_hsync = false;
    if (gate_array->vsync_ga) {
      gate_array->v26++;
      if (gate_array->v26 == 2) {
        gate_array->sig_vsync = true;
      } else if (gate_array->v26 == 6) {
        gate_array->sig_vsync = false;
      } else if (gate_array->v26 == 26) {
        gate_array->black_vsync = false;
        gate_array->vsync_ga = false;
      }
    }

    /* Mode changes take effect after the HSYNC ("The Gate Array"); the
       sub-microsecond placement waits for Shaker. The interrupt request
       likewise rises here, where on hardware it is one more microsecond
       along (Compendium ch. 27.6.1). */
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

/* The pens a byte carries, and how many. The bits of a pen number are
   scattered across the byte so that the same fetch feeds every mode
   ("The Gate Array", byte/pixel structure). Mode 3 uses mode 1's two
   leftmost pixels at mode 0's width and ignores four bits. */
static uint8_t decode_pens(uint8_t mode, uint8_t byte, uint8_t pens[8]) {
  switch (mode) {
    case 0:
      pens[0] = (uint8_t)(((byte & 0x80) >> 7) | ((byte & 0x08) >> 2) | ((byte & 0x20) >> 3) |
                          ((byte & 0x02) << 2));
      pens[1] = (uint8_t)(((byte & 0x40) >> 6) | ((byte & 0x04) >> 1) | ((byte & 0x10) >> 2) |
                          ((byte & 0x01) << 3));
      return 2;
    case 1:
      for (uint8_t pixel = 0; pixel < 4; pixel++) {
        pens[pixel] = (uint8_t)(((byte >> (7 - pixel)) & 1) | (((byte >> (3 - pixel)) & 1) << 1));
      }
      return 4;
    case 2:
      for (uint8_t pixel = 0; pixel < 8; pixel++) {
        pens[pixel] = (uint8_t)((byte >> (7 - pixel)) & 1);
      }
      return 8;
    default:
      pens[0] = (uint8_t)(((byte & 0x80) >> 7) | ((byte & 0x08) >> 2));
      pens[1] = (uint8_t)(((byte & 0x40) >> 6) | ((byte & 0x04) >> 1));
      return 2;
  }
}

void gate_array_video(gate_array_t *gate_array, bool display, uint8_t byte0, uint8_t byte1,
                      uint8_t samples[GATE_ARRAY_SAMPLES_PER_CHARACTER]) {
  if (gate_array->black_hsync || gate_array->black_vsync) {
    for (uint8_t index = 0; index < GATE_ARRAY_SAMPLES_PER_CHARACTER; index++) {
      samples[index] = GATE_ARRAY_BLACK;
    }
  } else if (!gate_array->latched_display) {
    for (uint8_t index = 0; index < GATE_ARRAY_SAMPLES_PER_CHARACTER; index++) {
      samples[index] = gate_array->inks[16];
    }
  } else {
    uint8_t written = 0;
    for (uint8_t half = 0; half < 2; half++) {
      uint8_t pens[8];
      uint8_t count = decode_pens(gate_array->mode, gate_array->latched_bytes[half], pens);
      uint8_t pixel_width = (uint8_t)(8 / count);
      for (uint8_t pixel = 0; pixel < count; pixel++) {
        for (uint8_t repeat = 0; repeat < pixel_width; repeat++) {
          samples[written++] = gate_array->inks[pens[pixel]];
        }
      }
    }
  }
  gate_array->latched_bytes[0] = byte0;
  gate_array->latched_bytes[1] = byte1;
  gate_array->latched_display = display;
}

uint32_t gate_array_rgb(uint8_t colour_code) {
  /* Measured on the outputs of a 40010 Gate Array and tabulated by "The
     Gate Array" (Grim), INKR color-codes, whose rows are ordered by
     luminosity; this one is indexed by the 5 bits the INKR command keeps.
     The levels are not the tidy 0/50/100% the three-state logic suggests,
     which is the point of using measured values. */
  static const uint32_t levels[32] = {
      0x6E7D6B, 0x6E7B6D, 0x00F36B, 0xF3F36D, 0x00026B, 0xF00268, 0x007868, 0xF37D6B,
      0xF30268, 0xF3F36B, 0xF3F30D, 0xFFF3F9, 0xF30506, 0xF302F4, 0xF37D0D, 0xFA80F9,
      0x000268, 0x02F36B, 0x02F001, 0x0FF3F2, 0x000201, 0x0C02F4, 0x027801, 0x0C7BF4,
      0x690268, 0x71F36B, 0x71F504, 0x71F3F4, 0x6C0201, 0x6C02F2, 0x6E7B01, 0x6E7BF6,
  };
  return levels[colour_code & 0x1F];
}
