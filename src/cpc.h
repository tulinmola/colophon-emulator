/*
 * cpc.h — the Amstrad CPC, wired.
 *
 * This is the machine file: the one place that knows the chips are soldered
 * into a CPC. It owns the memory map, the I/O decode and the board's video
 * address wiring; the chips it wires know nothing about it. At this stage
 * the machine is a Z80, its memory, a CRTC counting out the frame, a Gate
 * Array drawing from it, a monitor to draw on, and a keyboard read the long
 * way round through two more chips. No wait states yet.
 *
 * Sources:
 * - "The Gate Array" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/gatearray — the
 *   &7Fxx command dispatch the wiring routes by, and the eight MMR banking
 *   configurations.
 * - "Amstrad CPC Ram Paging" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/rampage.html — the 6128's PAL decodes
 *   only MMR bits 2-0.
 * - "I/O port allocation" (Mark Rison & Kevin Thacker),
 *   https://cpctech.cpcwiki.de/docs/iopord.html — devices decode single
 *   address bits, so one I/O access can reach several devices at once.
 * - "Expansion ROM Selection" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/exprom.html — the &DFxx ROM number
 *   latch; selecting an absent ROM resolves to ROM 0.
 * - "The CRTC" (Grim),
 *   https://www.grimware.org/doku.php/documentations/devices/crtc — the
 *   board's wiring of the CRTC bus: A14 low selects the chip, RS is A8 and
 *   R/W is A9, giving the four ports &BC00-&BF00.
 * - "Screen memory addressess" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/scraddr.html — the board's rewiring of
 *   the CRTC's address lines on their way to RAM, which is what scatters a
 *   character row across eight blocks two kilobytes apart.
 * - "8255 PPI" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/8255cpc.html — what each port is wired
 *   to here: the PSG's bus on A, VSYNC and the board's links on B, the
 *   PSG's function lines and the keyboard line on C.
 */
#ifndef COLOPHON_CPC_H
#define COLOPHON_CPC_H

#include <stdbool.h>
#include <stdint.h>

#include "crtc.h"
#include "gate_array.h"
#include "keyboard.h"
#include "monitor.h"
#include "ppi.h"
#include "psg.h"
#include "z80.h"

/* The whole raster the beam covers: 64µs of line at the Gate Array's
   16MHz pixel clock, and the 312 lines of a 50Hz frame. The picture is the
   middle of it; the rest is border, sync and blanking. */
#define CPC_FRAMEBUFFER_WIDTH 1024
#define CPC_FRAMEBUFFER_HEIGHT 312
/* A CTM monitor cannot anchor an image vertically on a sync shorter than
   11-12µs (Compendium ch. 16.2.4); 12µs is 192 pixel clocks. */
#define CPC_FRAME_SYNC_SAMPLES 192

typedef struct {
  z80_t cpu;
  uint64_t pins; /* the bus between ticks */

  crtc_t crtc;
  uint64_t crtc_pins; /* the CRTC's outputs as of its last character clock */
  gate_array_t gate_array;
  monitor_t monitor;
  ppi_t ppi;
  psg_t psg;
  keyboard_t keyboard;

  /* Links soldered on the board, which software reads and cannot change.
     The refresh rate decides which of the two tables in the firmware's ROM
     it programs the CRTC from. */
  bool fifty_hz;
  uint8_t manufacturer; /* 0-7; seven is Amstrad, see cpc_manufacturer */

  /* Host-provided storage; the core allocates nothing. 64K means no PAL is
     fitted and banking commands die on the empty socket; 128K is a 6128,
     banks 0-3 the base 64K the video hardware will read, banks 4-7 the
     extension. */
  uint8_t *ram;
  uint32_t ram_size;
  const uint8_t *lower_rom;       /* 16K; reset fetches from it */
  const uint8_t *upper_roms[256]; /* sparse; absent numbers resolve to ROM 0 */
  uint8_t upper_rom_number;

  /* MMR, the PAL's memory-mapping register. The 6128's PAL decodes only
     bits 2-0, the configuration; bits 5-3 address 64K pages that only larger
     expansions fit. */
  uint8_t mmr;

  /* Derived from MMR and the Gate Array's ROM enables by remap(); cache,
     never the state. */
  const uint8_t *read_page[4];
  uint8_t *write_page[4];
} cpc_t;

/* Power-on. The lower ROM is readable at &0000 — it must be, or no first
 * instruction could ever be fetched. Upper ROM enabled and configuration 0
 * are conventions: the firmware writes both registers before anything could
 * observe their reset state. */
void cpc_init(cpc_t *cpc, uint8_t *ram, uint32_t ram_size, const uint8_t *lower_rom);

/* Fit a 16K ROM as upper ROM `number`; NULL empties the socket. */
void cpc_set_upper_rom(cpc_t *cpc, uint8_t number, const uint8_t *rom);

/* Plug in a monitor: CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT bytes
 * of hardware colour codes, host-owned. Unplugged, the machine runs on and
 * draws into the void, as it would with the cable out. */
void cpc_connect_monitor(cpc_t *cpc, uint8_t *framebuffer);

/* The board's links. A machine is 50Hz and made by Amstrad unless someone
 * resoldered it, and the firmware reads both before it programs anything. */
#define CPC_MANUFACTURER_AMSTRAD 7
void cpc_set_links(cpc_t *cpc, bool fifty_hz, uint8_t manufacturer);

/* Advance one T-state: tick the CPU, answer its pins from the map. No wait
 * states yet — timing is the raw Z80's until the Gate Array brings the 4T
 * grid. Returns the bus for the host to watch. */
uint64_t cpc_tick(cpc_t *cpc);

/* The CPU's view without the CPU: reads and writes resolve through the same
 * mapping the CPU's memory cycles use, so a peek under an enabled ROM sees
 * the ROM and a poke lands in the RAM beneath it. */
uint8_t cpc_peek(const cpc_t *cpc, uint16_t address);
void cpc_poke(cpc_t *cpc, uint16_t address, uint8_t value);

#endif
