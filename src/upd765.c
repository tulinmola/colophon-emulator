/*
 * upd765.c — the three phases, and the execution phase on the track.
 */
#include <stddef.h>

#include "upd765.h"

/* Command codes, in the low five bits of the first byte. */
#define READ_TRACK 0x02
#define SPECIFY 0x03
#define SENSE_DRIVE_STATUS 0x04
#define WRITE_DATA 0x05
#define READ_DATA 0x06
#define RECALIBRATE 0x07
#define SENSE_INTERRUPT_STATUS 0x08
#define WRITE_DELETED_DATA 0x09
#define READ_ID 0x0A
#define READ_DELETED_DATA 0x0C
#define FORMAT_TRACK 0x0D
#define SEEK 0x0F
#define SCAN_EQUAL 0x11
#define SCAN_LOW_OR_EQUAL 0x19
#define SCAN_HIGH_OR_EQUAL 0x1D
#define CODE_MASK 0x1F

/* The three flags above the code. */
#define MULTI_TRACK 0x80
#define MFM 0x40
#define SKIP 0x20

/* Where the parameters sit in a read, write or scan command. The unit
   byte lays HD, US1 and US0 out as ST0 and ST3 do, so their masks serve. */
#define AT_UNIT 1
#define AT_C 2
#define AT_H 3
#define AT_R 4
#define AT_N 5
#define AT_EOT 6
#define AT_DTL 8
#define AT_STP 8
/* And in Format a Track. */
#define AT_FORMAT_N 2
#define AT_FORMAT_SC 3
#define AT_FORMAT_GPL 4
#define AT_FORMAT_D 5

#define UNIT_OF(byte) ((byte) & 0x03)
#define HEAD_OF(byte) (((byte) >> 2) & 0x01)

/* At 4MHz: the datasheet's 1.024ms poll interval doubled, its 1ms step
   unit doubled, its 16ms head unload unit doubled, its 2ms head load unit
   doubled. The datasheet gives HUT and HLT no value for zero; it is taken
   as the count past the largest, where their tables would continue. */
#define POLL_INTERVAL 2048
#define STEP_UNIT 2000
#define HEAD_UNLOAD_UNIT 32000
#define HEAD_LOAD_UNIT 4000

#define RECALIBRATE_STEPS 77
#define SCAN_WILDCARD 0xFF /* from the processor, it matches anything */

static uint8_t command_length(uint8_t code) {
  switch (code) {
    case READ_TRACK:
    case WRITE_DATA:
    case READ_DATA:
    case WRITE_DELETED_DATA:
    case READ_DELETED_DATA:
    case SCAN_EQUAL:
    case SCAN_LOW_OR_EQUAL:
    case SCAN_HIGH_OR_EQUAL:
      return 9;
    case FORMAT_TRACK:
      return 6;
    case SPECIFY:
    case SEEK:
      return 3;
    case SENSE_DRIVE_STATUS:
    case RECALIBRATE:
    case READ_ID:
      return 2;
    default:
      return 1;
  }
}

void upd765_init(upd765_t *fdc) {
  *fdc = (upd765_t){0};
  /* The timings Specify would set from zeros. */
  fdc->step_time = 16 * STEP_UNIT;
  fdc->head_unload_time = 16 * HEAD_UNLOAD_UNIT;
  fdc->head_load_time = 128 * HEAD_LOAD_UNIT;
  fdc->until_poll = POLL_INTERVAL;
}

void upd765_attach(upd765_t *fdc, uint8_t unit, drive_t *drive) {
  fdc->drives[unit & UPD765_ST0_US] = drive;
}

void upd765_set_terminal_count(upd765_t *fdc, bool high) { fdc->terminal_count = high; }

static uint8_t code(const upd765_t *fdc) { return fdc->command[0] & CODE_MASK; }

static bool is_read(const upd765_t *fdc) {
  return code(fdc) == READ_DATA || code(fdc) == READ_DELETED_DATA;
}

static bool is_write(const upd765_t *fdc) {
  return code(fdc) == WRITE_DATA || code(fdc) == WRITE_DELETED_DATA;
}

static bool is_scan(const upd765_t *fdc) {
  return code(fdc) == SCAN_EQUAL || code(fdc) == SCAN_LOW_OR_EQUAL ||
         code(fdc) == SCAN_HIGH_OR_EQUAL;
}

/* The result phase: seven bytes for the commands that read or write the
   disc, and the head begins its unload delay. */
static void finish(upd765_t *fdc) {
  fdc->result[0] = fdc->st0;
  fdc->result[1] = fdc->st1;
  fdc->result[2] = fdc->st2;
  fdc->result[3] = fdc->c;
  fdc->result[4] = fdc->h;
  fdc->result[5] = fdc->r;
  fdc->result[6] = fdc->n;
  fdc->result_length = UPD765_RESULT_BYTES;
  fdc->result_sent = 0;
  fdc->phase = UPD765_PHASE_RESULT;
  fdc->stage = UPD765_STAGE_NONE;
  fdc->request = false;
  fdc->until_head_unload = fdc->head_unload_time;
}

static void fail(upd765_t *fdc) {
  fdc->st0 |= UPD765_ST0_ABNORMAL;
  finish(fdc);
}

static void invalid(upd765_t *fdc) {
  fdc->result[0] = UPD765_ST0_INVALID;
  fdc->result_length = 1;
  fdc->result_sent = 0;
  fdc->phase = UPD765_PHASE_RESULT;
}

static void raise_interrupt(upd765_t *fdc, uint8_t unit, uint8_t st0) {
  fdc->units[unit].interrupt = true;
  fdc->units[unit].interrupt_st0 = st0;
}

/* A search for an identity begins from wherever the disc has turned to
   and gets two passes of the index to find it; an identity that is not
   the one continues the same search. */
static void begin_search(upd765_t *fdc) {
  fdc->stage = UPD765_STAGE_FINDING_IDENTITY;
  fdc->index_pulses = 0;
}

static void keep_searching(upd765_t *fdc) { fdc->stage = UPD765_STAGE_FINDING_IDENTITY; }

static bool begins_at_index(const upd765_t *fdc) {
  return code(fdc) == FORMAT_TRACK || code(fdc) == READ_TRACK;
}

/* Every command that touches the disc: the unit and head it names, the
 * drive's readiness and protection, and the head load. */
static void begin_execution(upd765_t *fdc) {
  fdc->unit = UNIT_OF(fdc->command[AT_UNIT]);
  fdc->head = HEAD_OF(fdc->command[AT_UNIT]);
  fdc->drive = fdc->drives[fdc->unit];
  fdc->st0 = fdc->command[AT_UNIT] & (UPD765_ST0_HD | UPD765_ST0_US);
  fdc->st1 = 0;
  fdc->st2 = 0;
  /* Read ID and Format leave the identity registers to what they find and
     what they are handed; the rest name a sector. */
  if (is_read(fdc) || is_write(fdc) || is_scan(fdc) || code(fdc) == READ_TRACK) {
    fdc->c = fdc->command[AT_C];
    fdc->h = fdc->command[AT_H];
    fdc->r = fdc->command[AT_R];
    fdc->n = fdc->command[AT_N];
  }
  if (fdc->drive == NULL || !drive_ready(fdc->drive)) {
    fdc->st0 |= UPD765_ST0_NR;
    fail(fdc);
    return;
  }
  drive_select_side(fdc->drive, fdc->head);
  if ((is_write(fdc) || code(fdc) == FORMAT_TRACK) && drive_write_protected(fdc->drive)) {
    fdc->st1 |= UPD765_ST1_NW;
    fail(fdc);
    return;
  }
  fdc->phase = UPD765_PHASE_EXECUTION;
  fdc->request = false;
  fdc->to_processor = false;
  fdc->stage = UPD765_STAGE_LOADING_HEAD;
  fdc->wait = fdc->head_loaded ? 0 : fdc->head_load_time;
  fdc->identity_seen = false;
  fdc->control_mark = false;
  fdc->sectors_done = 0;
  fdc->identity_bytes = 0;
}

static void sense_drive_status(upd765_t *fdc) {
  uint8_t unit = UNIT_OF(fdc->command[AT_UNIT]);
  const drive_t *drive = fdc->drives[unit];
  uint8_t st3 = fdc->command[AT_UNIT] & (UPD765_ST3_HD | UPD765_ST3_US);
  if (drive != NULL) {
    if (drive_track_zero(drive)) {
      st3 |= UPD765_ST3_T0;
    }
    if (drive_two_sided(drive)) {
      st3 |= UPD765_ST3_TS;
    }
    if (drive_ready(drive)) {
      st3 |= UPD765_ST3_RY;
    }
    if (drive_write_protected(drive)) {
      st3 |= UPD765_ST3_WP;
    }
  }
  fdc->result[0] = st3;
  fdc->result_length = 1;
  fdc->result_sent = 0;
  fdc->phase = UPD765_PHASE_RESULT;
}

/* Interrupts are answered one at a time, in unit order; none pending
   makes the command an invalid one. */
static void sense_interrupt_status(upd765_t *fdc) {
  for (uint8_t unit = 0; unit < UPD765_UNITS; unit++) {
    upd765_unit_t *state = &fdc->units[unit];
    if (state->interrupt) {
      state->interrupt = false;
      if (state->interrupt_st0 & UPD765_ST0_SE) {
        state->busy = false;
      }
      fdc->result[0] = state->interrupt_st0;
      fdc->result[1] = state->present;
      fdc->result_length = 2;
      fdc->result_sent = 0;
      fdc->phase = UPD765_PHASE_RESULT;
      return;
    }
  }
  invalid(fdc);
}

/* SRT in the high nibble of the first byte counts down from 16, HUT in
   its low nibble counts up; HLT is the high seven bits of the second and
   ND its lowest. */
static void specify(upd765_t *fdc) {
  uint8_t srt = (uint8_t)(fdc->command[1] >> 4);
  uint8_t hut = fdc->command[1] & 0x0F;
  uint8_t hlt = (uint8_t)(fdc->command[2] >> 1);
  uint8_t nd = fdc->command[2] & 0x01;
  fdc->step_time = (uint32_t)(16 - srt) * STEP_UNIT;
  fdc->head_unload_time = (hut == 0 ? 16u : hut) * HEAD_UNLOAD_UNIT;
  fdc->head_load_time = (hlt == 0 ? 128u : hlt) * HEAD_LOAD_UNIT;
  fdc->non_dma = nd != 0;
  fdc->phase = UPD765_PHASE_IDLE;
}

/* Seek and Recalibrate return the chip to the processor at once and step
   the drive on their own time; the end is an interrupt. A drive that is
   not ready cannot be stepped, and says so through the same interrupt. */
static void begin_seek(upd765_t *fdc, bool recalibrate) {
  uint8_t unit = UNIT_OF(fdc->command[AT_UNIT]);
  upd765_unit_t *state = &fdc->units[unit];
  const drive_t *drive = fdc->drives[unit];
  state->head = HEAD_OF(fdc->command[AT_UNIT]);
  uint8_t st0 = (uint8_t)(unit | (state->head ? UPD765_ST0_HD : 0));
  fdc->phase = UPD765_PHASE_IDLE;
  if (drive == NULL || !drive_ready(drive)) {
    state->busy = true;
    raise_interrupt(fdc, unit, st0 | UPD765_ST0_SE | UPD765_ST0_NR | UPD765_ST0_ABNORMAL);
    return;
  }
  if (!state->seeking) {
    fdc->seeking_units++;
  }
  state->seeking = true;
  state->busy = true;
  state->recalibrating = recalibrate;
  state->target = recalibrate ? 0 : fdc->command[2];
  state->steps = 0;
  state->until_step = 0;
}

static void execute(upd765_t *fdc) {
  switch (code(fdc)) {
    case READ_DATA:
    case READ_DELETED_DATA:
    case WRITE_DATA:
    case WRITE_DELETED_DATA:
    case READ_TRACK:
    case READ_ID:
    case FORMAT_TRACK:
    case SCAN_EQUAL:
    case SCAN_LOW_OR_EQUAL:
    case SCAN_HIGH_OR_EQUAL:
      begin_execution(fdc);
      break;
    case SPECIFY:
      specify(fdc);
      break;
    case SEEK:
      begin_seek(fdc, false);
      break;
    case RECALIBRATE:
      begin_seek(fdc, true);
      break;
    case SENSE_DRIVE_STATUS:
      sense_drive_status(fdc);
      break;
    case SENSE_INTERRUPT_STATUS:
      sense_interrupt_status(fdc);
      break;
    default:
      invalid(fdc);
      break;
  }
}

uint8_t upd765_read(upd765_t *fdc, upd765_selection selection) {
  if (selection == UPD765_STATUS) {
    uint8_t status = 0;
    for (uint8_t unit = 0; unit < UPD765_UNITS; unit++) {
      if (fdc->units[unit].busy) {
        status |= (uint8_t)UPD765_MSR_DRIVE_BUSY(unit);
      }
    }
    switch (fdc->phase) {
      case UPD765_PHASE_IDLE:
        status |= UPD765_MSR_RQM;
        break;
      case UPD765_PHASE_COMMAND:
        status |= UPD765_MSR_RQM | UPD765_MSR_CB;
        break;
      case UPD765_PHASE_EXECUTION:
        status |= UPD765_MSR_CB;
        if (fdc->non_dma) {
          status |= UPD765_MSR_EXM;
        }
        if (fdc->request) {
          status |= UPD765_MSR_RQM;
        }
        if (fdc->to_processor) {
          status |= UPD765_MSR_DIO;
        }
        break;
      case UPD765_PHASE_RESULT:
        status |= UPD765_MSR_RQM | UPD765_MSR_DIO | UPD765_MSR_CB;
        break;
    }
    return status;
  }
  if (fdc->phase == UPD765_PHASE_RESULT) {
    uint8_t byte = fdc->result[fdc->result_sent++];
    if (fdc->result_sent >= fdc->result_length) {
      fdc->phase = UPD765_PHASE_IDLE;
    }
    return byte;
  }
  if (fdc->phase == UPD765_PHASE_EXECUTION && fdc->request && fdc->to_processor) {
    fdc->request = false;
  }
  return fdc->data;
}

void upd765_write(upd765_t *fdc, upd765_selection selection, uint8_t byte) {
  (void)selection;
  switch (fdc->phase) {
    case UPD765_PHASE_IDLE:
      fdc->command[0] = byte;
      fdc->command_length = command_length(byte & CODE_MASK);
      fdc->command_received = 1;
      if (fdc->command_received == fdc->command_length) {
        execute(fdc);
      } else {
        fdc->phase = UPD765_PHASE_COMMAND;
      }
      break;
    case UPD765_PHASE_COMMAND:
      fdc->command[fdc->command_received++] = byte;
      if (fdc->command_received == fdc->command_length) {
        execute(fdc);
      }
      break;
    case UPD765_PHASE_EXECUTION:
      if (fdc->request && !fdc->to_processor) {
        fdc->data = byte;
        fdc->request = false;
      }
      break;
    case UPD765_PHASE_RESULT:
      break;
  }
}

/* Polling continues between commands: a change of READY on any unit
   leaves an interrupt saying so, with NR telling which way it went. */
static void poll_drives(upd765_t *fdc) {
  if (--fdc->until_poll > 0) {
    return;
  }
  fdc->until_poll = POLL_INTERVAL;
  if (fdc->phase != UPD765_PHASE_IDLE) {
    return;
  }
  for (uint8_t unit = 0; unit < UPD765_UNITS; unit++) {
    upd765_unit_t *state = &fdc->units[unit];
    const drive_t *drive = fdc->drives[unit];
    bool ready = drive != NULL && drive_ready(drive);
    /* A change found while an earlier interrupt still waits is found again
       at the next poll, once that one has been collected. */
    if (ready != state->ready_seen && !state->interrupt) {
      state->ready_seen = ready;
      raise_interrupt(fdc, unit,
                      (uint8_t)(UPD765_ST0_READY_CHANGED | unit | (ready ? 0 : UPD765_ST0_NR)));
    }
  }
}

static void end_seek(upd765_t *fdc, uint8_t unit, uint8_t st0) {
  upd765_unit_t *state = &fdc->units[unit];
  state->seeking = false;
  fdc->seeking_units--;
  raise_interrupt(fdc, unit, (uint8_t)(st0 | unit | (state->head ? UPD765_ST0_HD : 0)));
}

static void run_seeks(upd765_t *fdc) {
  if (fdc->seeking_units == 0) {
    return;
  }
  for (uint8_t unit = 0; unit < UPD765_UNITS; unit++) {
    upd765_unit_t *state = &fdc->units[unit];
    if (!state->seeking) {
      continue;
    }
    drive_t *drive = fdc->drives[unit];
    if (drive == NULL || !drive_ready(drive)) {
      end_seek(fdc, unit, UPD765_ST0_SE | UPD765_ST0_NR | UPD765_ST0_ABNORMAL);
      continue;
    }
    if (state->until_step > 0) {
      state->until_step--;
      continue;
    }
    if (state->recalibrating) {
      if (drive_track_zero(drive)) {
        state->present = 0;
        end_seek(fdc, unit, UPD765_ST0_SE);
        continue;
      }
      if (state->steps >= RECALIBRATE_STEPS) {
        end_seek(fdc, unit, UPD765_ST0_SE | UPD765_ST0_EC);
        continue;
      }
      drive_step(drive, false);
      state->steps++;
      if (state->present > 0) {
        state->present--;
      }
    } else {
      if (state->present == state->target) {
        end_seek(fdc, unit, UPD765_ST0_SE);
        continue;
      }
      bool inward = state->target > state->present;
      drive_step(drive, inward);
      state->present = (uint8_t)(state->present + (inward ? 1 : -1));
    }
    state->until_step = fdc->step_time;
  }
}

static void run_head(upd765_t *fdc) {
  if (!fdc->head_loaded || fdc->phase == UPD765_PHASE_EXECUTION || fdc->until_head_unload == 0) {
    return;
  }
  fdc->until_head_unload--;
  if (fdc->until_head_unload == 0) {
    fdc->head_loaded = false;
  }
}

/* The sector whose identity the search settled on, or NULL when the head
   no longer stands over that track: a seek in progress moves it under a
   command. */
static const floppy_sector_t *current_sector(const upd765_t *fdc) {
  const drive_t *drive = fdc->drive;
  if (drive->cylinder != fdc->sector_cylinder || drive->side != fdc->sector_side) {
    return NULL;
  }
  return floppy_sector(drive->floppy, drive->cylinder, drive->side, (uint8_t)fdc->sector);
}

static bool field_sound(const upd765_t *fdc) { return fdc->crc == 0; }

/* The identity under the head has been read whole. */
static void identity_known(upd765_t *fdc) {
  const floppy_sector_t *sector = current_sector(fdc);
  if (sector == NULL) {
    begin_search(fdc);
    return;
  }
  fdc->identity_seen = true;
  if (code(fdc) == READ_ID) {
    /* The first identity to pass is the answer, sound or not. */
    fdc->c = sector->c;
    fdc->h = sector->h;
    fdc->r = sector->r;
    fdc->n = sector->n;
    if (!field_sound(fdc)) {
      fdc->st1 |= UPD765_ST1_DE;
      fail(fdc);
      return;
    }
    finish(fdc);
    return;
  }
  bool matches =
      sector->c == fdc->c && sector->h == fdc->h && sector->r == fdc->r && sector->n == fdc->n;
  if (code(fdc) == READ_TRACK) {
    /* Reads on regardless, noting what was wrong. */
    if (!matches) {
      fdc->st1 |= UPD765_ST1_ND;
    }
    if (!field_sound(fdc)) {
      fdc->st1 |= UPD765_ST1_DE;
    }
  } else if (!matches) {
    if (sector->c != fdc->c) {
      fdc->st2 |= UPD765_ST2_WC;
      if (sector->c == 0xFF) {
        fdc->st2 |= UPD765_ST2_BC;
      }
    }
    keep_searching(fdc);
    return;
  } else if (!field_sound(fdc)) {
    fdc->st1 |= UPD765_ST1_DE;
    fail(fdc);
    return;
  } else {
    fdc->st2 &= (uint8_t)~(UPD765_ST2_WC | UPD765_ST2_BC);
  }
  /* The field's length on the track is what N counts; what crosses the
     data register is that, or DTL when N is zero. Writes fill the field. */
  uint8_t n = code(fdc) == READ_TRACK ? fdc->command[AT_N] : sector->n;
  fdc->field_length = floppy_sector_length(n);
  fdc->transfer_length = fdc->field_length;
  if (n == 0 && !is_write(fdc) && fdc->command[AT_DTL] < fdc->field_length) {
    fdc->transfer_length = fdc->command[AT_DTL];
  }
  fdc->transferred = 0;
  fdc->stage = UPD765_STAGE_TO_DATA;
  fdc->countdown = FLOPPY_DATA_FIELD - FLOPPY_ID_KNOWN;
}

/* Two passes of the index and no sector: what was missing decides the
   flags. No identity at all is a missing address mark; identities but not
   the one asked for is no data. */
static void search_failed(upd765_t *fdc) {
  fdc->st1 |= fdc->identity_seen ? UPD765_ST1_ND : UPD765_ST1_MA;
  fail(fdc);
}

/* Terminal count ends the command with the sector in hand; the identity
   registers then name the sector that would have come next, as the
   datasheet's table has them. */
static void terminal_count_ends(upd765_t *fdc) {
  if (fdc->r == fdc->command[AT_EOT]) {
    fdc->r = 1;
    if ((fdc->command[0] & MULTI_TRACK) && fdc->head == 0) {
      fdc->h ^= 1;
    } else {
      fdc->c++;
      if (fdc->command[0] & MULTI_TRACK) {
        fdc->h ^= 1;
      }
    }
  } else {
    fdc->r++;
  }
  finish(fdc);
}

/* After a sector: the next one, the other side, or the end. Without
   terminal count the last sector asked for is followed by a step past it,
   which is the end of the cylinder. */
static void next_sector(upd765_t *fdc) {
  if (fdc->terminal_count) {
    terminal_count_ends(fdc);
    return;
  }
  if (code(fdc) == READ_TRACK) {
    /* "The command terminates when the number of sectors read is equal to
       EOT" (Read a Track); what it ends with when no terminal count came
       the datasheet leaves unsaid, and MAME's controller ends as a read
       past EOT does. */
    if (++fdc->sectors_done >= fdc->command[AT_EOT]) {
      fdc->st1 |= UPD765_ST1_EN;
      fail(fdc);
    } else {
      begin_search(fdc);
    }
    return;
  }
  if (fdc->r == fdc->command[AT_EOT]) {
    if ((fdc->command[0] & MULTI_TRACK) && fdc->head == 0) {
      fdc->head = 1;
      fdc->st0 |= UPD765_ST0_HD;
      fdc->h ^= 1;
      fdc->r = 1;
      drive_select_side(fdc->drive, true);
      begin_search(fdc);
      return;
    }
    fdc->st1 |= UPD765_ST1_EN;
    if (is_scan(fdc)) {
      fdc->st2 |= UPD765_ST2_SN;
    }
    fail(fdc);
    return;
  }
  fdc->r = (uint8_t)(fdc->r + (is_scan(fdc) ? fdc->command[AT_STP] : 1));
  begin_search(fdc);
}

/* The field and its check have passed. */
static void sector_done(upd765_t *fdc) {
  if (is_write(fdc)) {
    if (current_sector(fdc) != NULL) {
      floppy_data_written(fdc->drive->floppy, fdc->drive->cylinder, fdc->drive->side,
                          (uint8_t)fdc->sector, code(fdc) == WRITE_DELETED_DATA);
    }
  } else if (!field_sound(fdc)) {
    fdc->st1 |= UPD765_ST1_DE;
    fdc->st2 |= UPD765_ST2_DD;
    if (code(fdc) != READ_TRACK) {
      fail(fdc);
      return;
    }
  }
  if (is_scan(fdc) && !fdc->scan_failed) {
    if (fdc->scan_satisfied) {
      fdc->st2 |= UPD765_ST2_SH;
    }
    finish(fdc);
    return;
  }
  if (is_read(fdc) && fdc->control_mark) {
    finish(fdc);
    return;
  }
  next_sector(fdc);
}

static void overrun(upd765_t *fdc) {
  fdc->st1 |= UPD765_ST1_OR;
  fail(fdc);
}

/* A byte the chip will write is asked for one byte's time before it is
   due. */
static void ask_for_byte(upd765_t *fdc) {
  fdc->request = true;
  fdc->to_processor = false;
}

/* One byte of the data field is under the head. */
static void data_byte(upd765_t *fdc, uint32_t position) {
  drive_t *drive = fdc->drive;
  uint8_t on_disc =
      floppy_byte(drive->floppy, drive->cylinder, drive->side, position, drive->revolutions);
  fdc->crc = floppy_crc(fdc->crc, on_disc);
  if (is_write(fdc) || is_scan(fdc)) {
    if (fdc->request) {
      overrun(fdc);
      return;
    }
    if (is_write(fdc)) {
      floppy_write_byte(drive->floppy, drive->cylinder, drive->side, position, fdc->data);
    } else if (fdc->data != SCAN_WILDCARD) {
      if (on_disc != fdc->data) {
        fdc->scan_satisfied = false;
      }
      if ((code(fdc) == SCAN_EQUAL && on_disc != fdc->data) ||
          (code(fdc) == SCAN_LOW_OR_EQUAL && on_disc > fdc->data) ||
          (code(fdc) == SCAN_HIGH_OR_EQUAL && on_disc < fdc->data)) {
        fdc->scan_failed = true;
      }
    }
    fdc->transferred++;
    if (fdc->transferred < fdc->field_length) {
      fdc->request = true;
      return;
    }
  } else {
    if (fdc->transferred > 0 && fdc->request) {
      overrun(fdc);
      return;
    }
    if (fdc->transferred < fdc->transfer_length) {
      fdc->data = on_disc;
      fdc->request = true;
      fdc->to_processor = true;
    }
    fdc->transferred++;
    if (fdc->transferred < fdc->field_length) {
      return;
    }
  }
  fdc->stage = UPD765_STAGE_CHECK;
  fdc->countdown = FLOPPY_CHECK_BYTES;
}

/* The first byte of the data field is under the head: the mark before it
   has just passed, and decides whether the field is read at all. */
static void data_field_begins(upd765_t *fdc, uint32_t position) {
  if (current_sector(fdc) == NULL) {
    begin_search(fdc);
    return;
  }
  bool deleted = fdc->mark == FLOPPY_DELETED_DATA_MARK;
  if (!deleted && fdc->mark != FLOPPY_DATA_MARK) {
    fdc->st1 |= UPD765_ST1_MA;
    fdc->st2 |= UPD765_ST2_MD;
    fail(fdc);
    return;
  }
  if (is_read(fdc)) {
    fdc->control_mark = deleted != (code(fdc) == READ_DELETED_DATA);
    if (fdc->control_mark) {
      fdc->st2 |= UPD765_ST2_CM;
      if (fdc->command[0] & SKIP) {
        /* The field and its check pass untouched; this byte is the first
           of them. */
        fdc->stage = UPD765_STAGE_SKIPPING;
        fdc->countdown = fdc->field_length + FLOPPY_CHECK_BYTES - 1;
        return;
      }
    }
  }
  fdc->scan_satisfied = true;
  fdc->scan_failed = false;
  fdc->stage = UPD765_STAGE_DATA;
  data_byte(fdc, position);
}

/* One of the four identity bytes of a sector being formatted is due. */
static void format_byte(upd765_t *fdc) {
  if (fdc->request) {
    overrun(fdc);
    return;
  }
  fdc->identities[fdc->sectors_done][fdc->identity_bytes++] = fdc->data;
  if (fdc->identity_bytes < FLOPPY_IDENTITY_BYTES) {
    fdc->request = true;
    return;
  }
  fdc->c = fdc->identities[fdc->sectors_done][0];
  fdc->h = fdc->identities[fdc->sectors_done][1];
  fdc->r = fdc->identities[fdc->sectors_done][2];
  fdc->n = fdc->identities[fdc->sectors_done][3];
  /* From N to where the next sector begins: the rest of this one and the
     gap after it. */
  fdc->stage = UPD765_STAGE_FORMAT_FIELD;
  fdc->countdown = FLOPPY_SECTOR_OVERHEAD - (FLOPPY_ID_FIELD + FLOPPY_IDENTITY_BYTES - 1) +
                   floppy_sector_length(fdc->command[AT_FORMAT_N]) + fdc->command[AT_FORMAT_GPL];
}

/* The next sector to format begins `after` bytes on, and its identity
   sixteen bytes into it. */
static void format_next(upd765_t *fdc, uint32_t after) {
  if (fdc->sectors_done < fdc->command[AT_FORMAT_SC] && fdc->sectors_done < FLOPPY_MAX_SECTORS) {
    fdc->stage = UPD765_STAGE_FORMAT_TO_IDENTITY;
    fdc->countdown = after + FLOPPY_ID_FIELD;
    fdc->identity_bytes = 0;
  } else {
    fdc->stage = UPD765_STAGE_FORMAT_ENDING;
  }
}

/* A medium with no room left for the format, or asked for more sectors
   than a track holds, refuses; not writeable is what the chip can say. */
static void format_ends(upd765_t *fdc) {
  drive_t *drive = fdc->drive;
  if (!floppy_format_track(drive->floppy, drive->cylinder, drive->side, fdc->identities,
                           fdc->command[AT_FORMAT_SC], fdc->command[AT_FORMAT_N],
                           fdc->command[AT_FORMAT_GPL], fdc->command[AT_FORMAT_D])) {
    fdc->st1 |= UPD765_ST1_NW;
    fail(fdc);
    return;
  }
  finish(fdc);
}

static void run_execution(upd765_t *fdc) {
  drive_t *drive = fdc->drive;
  if (!drive_ready(drive)) {
    fdc->st0 = (uint8_t)((fdc->st0 & (UPD765_ST0_HD | UPD765_ST0_US)) | UPD765_ST0_READY_CHANGED |
                         UPD765_ST0_NR);
    finish(fdc);
    return;
  }
  if (fdc->stage == UPD765_STAGE_LOADING_HEAD) {
    if (fdc->wait > 0) {
      fdc->wait--;
      return;
    }
    fdc->head_loaded = true;
    if (begins_at_index(fdc)) {
      fdc->stage = UPD765_STAGE_WAITING_INDEX;
    } else {
      begin_search(fdc);
    }
    return;
  }
  if (!drive->byte_passed) {
    return;
  }
  if (drive_index(drive)) {
    fdc->index_pulses++;
  }
  uint32_t position = drive->position;
  uint8_t under_head =
      floppy_byte(drive->floppy, drive->cylinder, drive->side, position, drive->revolutions);
  switch (fdc->stage) {
    case UPD765_STAGE_FINDING_IDENTITY: {
      if (fdc->index_pulses >= 2) {
        search_failed(fdc);
        return;
      }
      /* An FM read finds no identity on an MFM track, which is the only
         kind the medium records. */
      int found =
          (fdc->command[0] & MFM)
              ? floppy_sector_beginning_at(drive->floppy, drive->cylinder, drive->side, position)
              : -1;
      if (found >= 0) {
        fdc->sector = found;
        fdc->sector_cylinder = drive->cylinder;
        fdc->sector_side = drive->side;
        fdc->stage = UPD765_STAGE_READING_IDENTITY;
        fdc->countdown = FLOPPY_ID_KNOWN;
        fdc->crc = FLOPPY_CRC_INITIAL;
      }
      return;
    }
    case UPD765_STAGE_READING_IDENTITY: {
      /* The check runs from the mark bytes through the identity and its
         own two check bytes, and is zero at the end when all is sound. */
      fdc->countdown--;
      uint32_t offset = FLOPPY_ID_KNOWN - fdc->countdown;
      if (offset >= FLOPPY_ID_FIELD - FLOPPY_MARK_BYTES && offset < FLOPPY_ID_KNOWN) {
        fdc->crc = floppy_crc(fdc->crc, under_head);
      }
      if (fdc->countdown == 0) {
        identity_known(fdc);
      }
      return;
    }
    case UPD765_STAGE_TO_DATA: {
      fdc->countdown--;
      uint32_t offset = FLOPPY_DATA_FIELD - fdc->countdown;
      bool at_marks = offset >= FLOPPY_DATA_FIELD - FLOPPY_MARK_BYTES && offset < FLOPPY_DATA_FIELD;
      if (offset == FLOPPY_DATA_FIELD - FLOPPY_MARK_BYTES) {
        fdc->crc = FLOPPY_CRC_INITIAL;
      }
      if (at_marks) {
        fdc->crc = floppy_crc(fdc->crc, under_head);
      }
      if (fdc->countdown == 1) {
        fdc->mark = under_head;
        if (is_write(fdc) || is_scan(fdc)) {
          ask_for_byte(fdc);
        }
      }
      if (fdc->countdown == 0) {
        data_field_begins(fdc, position);
      }
      return;
    }
    case UPD765_STAGE_DATA:
      data_byte(fdc, position);
      return;
    case UPD765_STAGE_SKIPPING:
      if (--fdc->countdown == 0) {
        next_sector(fdc);
      }
      return;
    case UPD765_STAGE_CHECK:
      if (fdc->countdown == FLOPPY_CHECK_BYTES && !is_write(fdc) && !is_scan(fdc) && fdc->request) {
        overrun(fdc);
        return;
      }
      fdc->crc = floppy_crc(fdc->crc, under_head);
      if (--fdc->countdown == 0) {
        sector_done(fdc);
      }
      return;
    case UPD765_STAGE_WAITING_INDEX:
      if (!drive_index(drive)) {
        return;
      }
      if (code(fdc) == FORMAT_TRACK) {
        format_next(fdc, FLOPPY_TRACK_PREAMBLE);
      } else {
        begin_search(fdc);
      }
      return;
    case UPD765_STAGE_FORMAT_TO_IDENTITY:
      fdc->countdown--;
      if (fdc->countdown == 1) {
        ask_for_byte(fdc);
      }
      if (fdc->countdown == 0) {
        fdc->stage = UPD765_STAGE_FORMAT_IDENTITY;
        format_byte(fdc);
      }
      return;
    case UPD765_STAGE_FORMAT_IDENTITY:
      format_byte(fdc);
      return;
    case UPD765_STAGE_FORMAT_FIELD:
      if (--fdc->countdown == 0) {
        fdc->sectors_done++;
        format_next(fdc, 0);
      }
      return;
    case UPD765_STAGE_FORMAT_ENDING:
      if (drive_index(drive)) {
        format_ends(fdc);
      }
      return;
    case UPD765_STAGE_NONE:
    case UPD765_STAGE_LOADING_HEAD:
      return;
  }
}

void upd765_tick(upd765_t *fdc) {
  for (uint8_t unit = 0; unit < UPD765_UNITS; unit++) {
    if (fdc->drives[unit] != NULL && fdc->drives[unit]->motor) {
      drive_tick(fdc->drives[unit]);
    }
  }
  poll_drives(fdc);
  run_seeks(fdc);
  run_head(fdc);
  if (fdc->phase == UPD765_PHASE_EXECUTION) {
    run_execution(fdc);
  }
}
