/*
 * cpc.h — the Amstrad CPC, wired.
 *
 * This is the machine file: the one place that knows the chips are soldered
 * into a CPC. It owns the memory map, the I/O decode, the board's video
 * address wiring and the clock that divides between the chips; the chips it
 * wires know nothing about it.
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
 *   to here: the PSG's bus on A, VSYNC, the board's links and the cassette's
 *   play head on B, and the PSG's function lines, the keyboard line, the
 *   cassette motor and the write line on C.
 * - "Reading the keyboard and Joysticks" (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/keyboard.html — the matrix table: which
 *   line and bit each key sits on, the UK legends printed on them, and the
 *   two joystick lines.
 * - "Floppy disc controller and Floppy disc drives" (Kevin Thacker's
 *   cpctech), https://cpctech.cpcwiki.de/docs/fdc.html — the disc
 *   interface's decode: A10 and A7 low select it, A8 and A0 then choose
 *   between the drive motor and the controller's two registers; the motor
 *   port drives every drive at once from bit 0; INT, DMA and terminal
 *   count left unconnected. It prints the selecting bit as 11 where "I/O
 *   port allocation" says 10; &FB7E has bit 10 low and bit 11 high, which
 *   settles it.
 */
#ifndef COLOPHON_CPC_H
#define COLOPHON_CPC_H

#include <stdbool.h>
#include <stdint.h>

#include "crtc.h"
#include "drive.h"
#include "gate_array.h"
#include "keyboard.h"
#include "monitor.h"
#include "ppi.h"
#include "psg.h"
#include "tape.h"
#include "upd765.h"
#include "z80.h"

/* The whole raster the beam covers: 64µs of line at the Gate Array's
   16MHz pixel clock, and the 312 lines of a 50Hz frame. The picture is the
   middle of it; the rest is border, sync and blanking. */
#define CPC_FRAMEBUFFER_WIDTH 1024
#define CPC_FRAMEBUFFER_HEIGHT 312
/* A CTM monitor cannot anchor an image vertically on a sync shorter than
   11-12µs (Compendium ch. 16.2.4); 12µs is 192 pixel clocks. */
#define CPC_FRAME_SYNC_SAMPLES 192
/* Half of the Gate Array's own 4µs line sync, which is where the middle of
   that pulse lands once the beam is timed from it. A pulse the Gate Array
   cut short walks its middle left and the picture right, half a microsecond
   for each one taken off (Compendium ch. 14.3, 14.4). */
#define CPC_LINE_SYNC_CENTRE 32

/* One frame of the screen the firmware programs: 312 lines of 64 characters
   at four T-states each. Unlike the raster above, which the monitor fixes,
   this is a convention and not an invariant — the CRTC's frame is whatever
   its registers say, and a demo that reprograms them makes frames of any
   length it likes. It is here because running "about a second" is a thing
   callers want, not because the machine guarantees it. */
#define CPC_TICKS_PER_STANDARD_FRAME (312L * 64L * 4L)

/* The board's clock is 4MHz, which is what a tape's timings are read
   against: the format counts them in a Spectrum's T-states. */
#define CPC_TICKS_PER_MILLISECOND 4000

/* A key, as the line that selects it and the bit that reads it. Ten lines of
   eight, numbered as the CPC's own documentation numbers its key codes;
   lines past the tenth are not wired and read &FF.

   Two of them carry a joystick as well. Joystick 0 has line 9 to itself, all
   but its top bit, which is DEL; joystick 1 shares line 6 with the letters,
   which is why its directions can be played from the keyboard and why
   two-player games pick their keys carefully. */
#define CPC_KEY(line, bit) KEYBOARD_KEY(line, bit)
#define CPC_KEYBOARD_LINES 10

/* A matrix one line short loses a whole row of keys without a word. */
typedef char cpc_keyboard_fits_the_matrix[CPC_KEYBOARD_LINES <= KEYBOARD_MAX_LINES ? 1 : -1];

/* The keys a caller needs by name; the rest it finds through
   cpc_key_for_character, which cannot reach the last four — three of them
   print nothing at all, and the fourth repeats a character it already returns. */
#define CPC_RETURN CPC_KEY(2, 2)
#define CPC_SHIFT CPC_KEY(2, 5)
#define CPC_SPACE CPC_KEY(5, 7)
#define CPC_TAB CPC_KEY(8, 4)
#define CPC_ESCAPE CPC_KEY(8, 2)
#define CPC_DELETE CPC_KEY(9, 7)
#define CPC_CONTROL CPC_KEY(2, 7)
#define CPC_COPY CPC_KEY(1, 1)
#define CPC_CAPS_LOCK CPC_KEY(8, 6)
#define CPC_FUNCTION_0 CPC_KEY(1, 7)

/* Where a character lives on a UK CPC keyboard, and whether shift is held to
 * reach it. Returns KEYBOARD_NO_KEY for a character the keyboard cannot
 * produce. */
keyboard_key cpc_key_for_character(char character, bool *shifted);

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

  /* The deck, host-owned as a disc is, and NULL when there is none. */
  tape_t *tape;

  /* The disc interface: built into the 664 and 6128, plugged into a 464
     as the DDI-1. Absent, its ports are nobody's and float. Drive A is the
     machine's own one-headed 3" drive; B is the connector for a second
     drive, given two heads here. The motor port turns both. */
  bool disc_interface;
  drive_t drives[2];
  upd765_t fdc;
  uint8_t fdc_bus; /* the controller's answer, held for the rest of a read */

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

/* Power-on with a CRTC built as the given type — only type 0's behaviour
 * is implemented, and what a program can read of the chip is what follows
 * the number given (crtc.h). The lower
 * ROM is readable at &0000 — it must be, or no first instruction could ever
 * be fetched. Upper ROM enabled and configuration 0 are conventions: the
 * firmware writes both registers before anything could observe their reset
 * state. */
void cpc_init(cpc_t *cpc, uint8_t *ram, uint32_t ram_size, const uint8_t *lower_rom,
              uint8_t crtc_type);

/* Fit a 16K ROM as upper ROM `number`; NULL empties the socket. */
void cpc_set_upper_rom(cpc_t *cpc, uint8_t number, const uint8_t *rom);

/* Fit the disc interface, or take it out. The AMSDOS ROM that comes with
 * it is a ROM like any other and goes in as upper ROM 7. */
void cpc_fit_disc_interface(cpc_t *cpc, bool fitted);

/* Put a disc in drive 0 (A) or 1 (B), or take it out with NULL. The disc
 * is borrowed: it must outlive the machine or be taken out first. */
void cpc_insert_disc(cpc_t *cpc, uint8_t drive, floppy_t *floppy);

/* Put a tape in the deck, or take it out with NULL. Bit 4 of port C is the
 * motor, so the machine starts and stops the tape itself; bit 7 of port B is
 * the play head. Bit 5 of port C is what the machine writes to tape, and it
 * goes nowhere: nothing here records. */
void cpc_insert_tape(cpc_t *cpc, tape_t *tape);

/* Plug in a monitor: CPC_FRAMEBUFFER_WIDTH * CPC_FRAMEBUFFER_HEIGHT bytes
 * of hardware colour codes, host-owned. Unplugged, the machine runs on and
 * draws into the void, as it would with the cable out. */
void cpc_connect_monitor(cpc_t *cpc, uint8_t *framebuffer);

/* The board's links. A machine is 50Hz and made by Amstrad unless someone
 * resoldered it, and the firmware reads both before it programs anything. */
#define CPC_MANUFACTURER_AMSTRAD 7
void cpc_set_links(cpc_t *cpc, bool fifty_hz, uint8_t manufacturer);

/* Advance the machine one T-state: the CPU every time, the chips on the
 * character clock every fourth. Returns the bus for the host to watch. */
uint64_t cpc_tick(cpc_t *cpc);

/* Tick until the CPU is between instructions. A snapshot has nowhere to
 * record a half-executed one, so anything about to take one owes the
 * machine this call first. */
void cpc_finish_instruction(cpc_t *cpc);

/* Recompute which memory answers where. The machine does this itself
 * whenever the CPU writes one of the registers behind the map; anything
 * that sets those registers from outside — restoring a snapshot — owes the
 * machine this call afterwards. */
void cpc_remap(cpc_t *cpc);

/* Where the video hardware last read for a character: the board's rewiring
 * of the CRTC's address lines, applied to the pins the chip put out on the
 * last character clock. The byte it names and the one beside it are the
 * pair the Gate Array puts on screen a microsecond later, which is what a
 * host must allow for when lining a byte up against the picture. */
uint16_t cpc_video_address(const cpc_t *cpc);

/* The CPU's view without the CPU: reads and writes resolve through the same
 * mapping the CPU's memory cycles use, so a peek under an enabled ROM sees
 * the ROM and a poke lands in the RAM beneath it. */
uint8_t cpc_peek(const cpc_t *cpc, uint16_t address);
void cpc_poke(cpc_t *cpc, uint16_t address, uint8_t value);

#endif
