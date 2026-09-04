/*
 * upd765.h — the NEC µPD765A floppy disc controller.
 *
 * A command is three phases: the processor writes the command bytes, the
 * chip executes, and the processor reads the result bytes. Every byte
 * crosses one data register under the handshake the main status register
 * shows — RQM says the chip wants a byte moved, DIO says which way, EXM
 * that an execution phase is under way in non-DMA mode, CB that a command
 * is in progress — and the boards this chip was fitted to wired neither
 * its interrupt nor its DMA lines, so the handshake is all there is.
 *
 * Time is the disc's. The chip finds a sector when its identity passes
 * under the head and moves each byte of the data field as it goes by, one
 * every 32µs; a processor that has not collected the last byte by then has
 * overrun, and the chip says so and stops. Seeks step at the rate Specify
 * set, and the drives are polled for a change of READY between commands.
 * A tick is one microsecond, which at the 4MHz clock these machines gave
 * the chip is four of its own; the datasheet's timings are all given for
 * an 8MHz clock and double at 4MHz, and that is what is implemented — a
 * check on it is that the CPC's operating system programs a step rate of
 * 6ms in the datasheet's units and documents it as 12.
 *
 * Every field is judged by its bytes: the chip runs its check over the
 * marks, the identity or data, and the two check bytes as they pass, and
 * reads the data mark to know a deleted field from a plain one — so a
 * sector a formatter wrote at one length under an identity announcing
 * another fails its check here as it does on the disc.
 *
 * Terminal count is a pin here because it is a pin on the chip, and a
 * machine that never raises it gets the behaviour its board has: a read
 * that reaches the last sector it was asked for steps past it, finds the
 * end of the cylinder, and ends abnormally with EN set. That is not a
 * fault; it is what every read on such a board looks like, and the
 * operating system tests for it.
 *
 * What is not modelled: DMA mode, which the boards cannot use, is taken
 * as non-DMA mode except that EXM stays low; the µs the chip takes between
 * a command byte and the next RQM, and between execution and result; the
 * Version command, which this revision of the part does not have; the
 * Scans' result table, which is approximated; and DTL on a write, which
 * always takes the whole field from the processor. A format the medium has
 * no room for is reported as not writeable, the nearest thing the chip can
 * say.
 *
 * Sources:
 * - µPD765A/µPD765B datasheet (NEC), mirrored at
 *   https://cpctech.cpcwiki.de/docs/upd765a/necfdc.htm — everything a
 *   name here comes from: the command set and its byte counts, the phases,
 *   the main status register, ST0-ST3 bit by bit, the meaning of MT, MF and
 *   SK, N and DTL, EOT, the 77-step limit of Recalibrate, the polling of
 *   READY and the interrupt it raises, and the doubling of every timing at
 *   4MHz.
 * - "Floppy disc controller and Floppy disc drives" (Kevin Thacker's
 *   cpctech), https://cpctech.cpcwiki.de/docs/fdc.html — INT, DRQ, /DACK
 *   and TC left unconnected on the Amstrad interface, and the 4MHz clock.
 * - "I/O port allocation" (Mark Rison & Kevin Thacker),
 *   https://cpctech.cpcwiki.de/docs/iopord.html — a write with A0 low,
 *   which the datasheet calls illegal, reaches the data register.
 * - AMSDOS ROM disassembly (Kevin Thacker's cpctech),
 *   https://cpctech.cpcwiki.de/docs/amsdos.asm — the operating system as
 *   the judge of this chip: one sector per read with EOT set to R, success
 *   tested as a normal end or an abnormal one carrying EN, interrupts
 *   drained by Sense Interrupt Status until one comes back invalid, and
 *   the step rate it programs.
 * - MAME, src/devices/machine/upd765.cpp, and Arnold, src/cpc/fdc.c — two
 *   independent implementations agreeing that a read ending at EOT without
 *   terminal count leaves C, H, R and N as they were, and that a change of
 *   READY reports as interrupt code 11 with the unit and NR.
 */
#ifndef COLOPHON_UPD765_H
#define COLOPHON_UPD765_H

#include <stdbool.h>
#include <stdint.h>

#include "drive.h"
#include "floppy.h"

/* The chip's A0 pin: which of its two registers the bus is talking to. */
typedef enum {
  UPD765_STATUS = 0,
  UPD765_DATA = 1,
} upd765_selection;

/* The main status register. */
#define UPD765_MSR_RQM 0x80                        /* request for master: a byte wants moving */
#define UPD765_MSR_DIO 0x40                        /* direction: set when the chip is sending */
#define UPD765_MSR_EXM 0x20                        /* execution mode: transfer under way, non-DMA */
#define UPD765_MSR_CB 0x10                         /* controller busy: a command is in progress */
#define UPD765_MSR_DRIVE_BUSY(unit) (1u << (unit)) /* that drive is seeking */

/* Status register 0. */
#define UPD765_ST0_NORMAL 0x00        /* IC: the command ended as asked */
#define UPD765_ST0_ABNORMAL 0x40      /* IC: it ended early */
#define UPD765_ST0_INVALID 0x80       /* IC: it was no command */
#define UPD765_ST0_READY_CHANGED 0xC0 /* IC: READY changed under it */
#define UPD765_ST0_SE 0x20            /* seek end */
#define UPD765_ST0_EC 0x10            /* equipment check: no track 0 in 77 steps */
#define UPD765_ST0_NR 0x08            /* not ready */
#define UPD765_ST0_HD 0x04            /* head address */
#define UPD765_ST0_US 0x03            /* unit select */

/* Status register 1. */
#define UPD765_ST1_EN 0x80 /* end of cylinder: a sector past the last */
#define UPD765_ST1_DE 0x20 /* data error: a check failed */
#define UPD765_ST1_OR 0x10 /* overrun: the processor was late */
#define UPD765_ST1_ND 0x04 /* no data: the sector was not found */
#define UPD765_ST1_NW 0x02 /* not writeable */
#define UPD765_ST1_MA 0x01 /* missing address mark */

/* Status register 2. */
#define UPD765_ST2_CM 0x40 /* control mark: the other kind of data mark */
#define UPD765_ST2_DD 0x20 /* data error in the data field */
#define UPD765_ST2_WC 0x10 /* wrong cylinder */
#define UPD765_ST2_SH 0x08 /* scan equal hit */
#define UPD765_ST2_SN 0x04 /* scan not satisfied */
#define UPD765_ST2_BC 0x02 /* bad cylinder: C read as &FF */
#define UPD765_ST2_MD 0x01 /* missing address mark in the data field */

/* Status register 3. */
#define UPD765_ST3_FT 0x80 /* fault */
#define UPD765_ST3_WP 0x40 /* write protected */
#define UPD765_ST3_RY 0x20 /* ready */
#define UPD765_ST3_T0 0x10 /* track 0 */
#define UPD765_ST3_TS 0x08 /* two side */
#define UPD765_ST3_HD 0x04 /* head address */
#define UPD765_ST3_US 0x03 /* unit select */

#define UPD765_UNITS 4
#define UPD765_COMMAND_BYTES 9
#define UPD765_RESULT_BYTES 7

typedef enum {
  UPD765_PHASE_IDLE,
  UPD765_PHASE_COMMAND,
  UPD765_PHASE_EXECUTION,
  UPD765_PHASE_RESULT,
} upd765_phase;

/* Where an execution phase has got to on the track. */
typedef enum {
  UPD765_STAGE_NONE,
  UPD765_STAGE_LOADING_HEAD,
  UPD765_STAGE_FINDING_IDENTITY,   /* watching for a sector's sync */
  UPD765_STAGE_READING_IDENTITY,   /* its identity and check passing */
  UPD765_STAGE_TO_DATA,            /* the gap, sync and mark before its data */
  UPD765_STAGE_DATA,               /* moving the field, byte by byte */
  UPD765_STAGE_SKIPPING,           /* a field passing untouched */
  UPD765_STAGE_CHECK,              /* the two check bytes after it */
  UPD765_STAGE_WAITING_INDEX,      /* a command that begins at the index */
  UPD765_STAGE_FORMAT_TO_IDENTITY, /* writing up to where the identity goes */
  UPD765_STAGE_FORMAT_IDENTITY,    /* taking C, H, R and N from the processor */
  UPD765_STAGE_FORMAT_FIELD,       /* writing the field and the gap after it */
  UPD765_STAGE_FORMAT_ENDING,      /* the rest of the revolution to the index */
} upd765_stage;

typedef struct {
  /* A seek in progress, and the interrupt a seek's end or a change of
     READY leaves behind until Sense Interrupt Status collects it. */
  bool seeking; /* stepping */
  bool busy;    /* in seek mode as the main status register shows it: from
                   the command until Sense Interrupt Status hears its end */
  bool recalibrating;
  uint8_t head;    /* HD, as the seek named it; reported with its end */
  uint8_t target;  /* NCN */
  uint8_t present; /* PCN */
  uint8_t steps;
  uint32_t until_step; /* microseconds */
  bool interrupt;
  uint8_t interrupt_st0;
  bool ready_seen; /* what polling last found */
} upd765_unit_t;

typedef struct {
  drive_t *drives[UPD765_UNITS]; /* by unit select; NULL where none is wired */
  upd765_unit_t units[UPD765_UNITS];

  upd765_phase phase;
  uint8_t command[UPD765_COMMAND_BYTES];
  uint8_t command_length;
  uint8_t command_received;
  uint8_t result[UPD765_RESULT_BYTES];
  uint8_t result_length;
  uint8_t result_sent;

  /* The data register and the handshake over it during execution. */
  uint8_t data;
  bool request;
  bool to_processor;
  bool terminal_count; /* the TC pin */

  /* The status registers of the command under way, and the identity
     registers, which keep what the last command left. */
  uint8_t st0;
  uint8_t st1;
  uint8_t st2;
  uint8_t c;
  uint8_t h;
  uint8_t r;
  uint8_t n;

  /* Specify, in microseconds at this clock. */
  uint32_t step_time;
  uint32_t head_unload_time;
  uint32_t head_load_time;
  bool non_dma;
  bool head_loaded;
  uint32_t until_head_unload;
  uint32_t until_poll;
  uint8_t seeking_units; /* how many are stepping, so that none is a quick tick */

  /* The execution phase on the track. */
  upd765_stage stage;
  uint8_t unit;
  uint8_t head; /* HD as the command named it, and the other after MT crosses */
  drive_t *drive;
  uint32_t wait;      /* microseconds to go before the stage moves */
  uint32_t countdown; /* bytes to go before it does */
  uint32_t index_pulses;
  int sector; /* the one the search settled on, on the track it stood over */
  uint8_t sector_cylinder;
  bool sector_side;
  uint32_t transferred;
  uint32_t transfer_length; /* bytes that cross the data register */
  uint32_t field_length;    /* bytes the field takes on the track */
  uint16_t crc;             /* the check running over the field under the head */
  uint8_t mark;             /* the data address mark the field began with */
  bool identity_seen;
  bool control_mark;
  bool scan_satisfied;
  bool scan_failed;
  uint8_t sectors_done;
  uint8_t identity_bytes;
  uint8_t identities[FLOPPY_MAX_SECTORS][FLOPPY_IDENTITY_BYTES];
} upd765_t;

/* Reset. Nothing is in progress and no drive is wired. */
void upd765_init(upd765_t *fdc);

/* Wire a drive to a unit select. */
void upd765_attach(upd765_t *fdc, uint8_t unit, drive_t *drive);

/* The bus. A read with A0 low is the main status register; with it high,
 * the data register. A write reaches the data register whichever way A0
 * lies: the datasheet calls a write with A0 low illegal, and on the boards
 * it was fitted to it writes the data register regardless. */
uint8_t upd765_read(upd765_t *fdc, upd765_selection selection);
void upd765_write(upd765_t *fdc, upd765_selection selection, uint8_t byte);

/* The TC pin. Held high, a transfer ends with the sector in hand. */
void upd765_set_terminal_count(upd765_t *fdc, bool high);

/* One microsecond, for the chip and for every drive wired to it whose
 * motor is on. */
void upd765_tick(upd765_t *fdc);

#endif
