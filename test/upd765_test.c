/*
 * upd765_test — the controller against its datasheet, on a fabricated disc.
 *
 * A processor is played here by a loop that reads the main status register
 * and moves a byte whenever RQM says to, one microsecond of the disc at a
 * time, so that every result below was reached the way software reaches
 * it: through the handshake, at the disc's speed. Where a test waits, it
 * waits for the disc to turn, and the times it checks are the datasheet's.
 *
 * The AMSDOS ROM's own sequences are the ones that matter most: one sector
 * per read with EOT set to R, and success read off an abnormal end that
 * carries EN.
 */
#include <string.h>

#include "dsk.h"
#include "test.h"
#include "upd765.h"

static uint8_t image[64 * 1024];
static size_t image_length;
static floppy_t floppy;
static uint8_t two_sided_image[64 * 1024];
static floppy_t two_sided;
static drive_t drive_a;
static drive_t drive_b;
static upd765_t fdc;
static const char *problem;

/* An image under construction, the same way floppy_test builds one. */
static size_t track_at;
static size_t track_count;

static void begin_image(uint8_t cylinders) {
  static const char tag[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";
  memset(image, 0, sizeof image);
  memcpy(image, tag, sizeof tag - 1);
  image[0x30] = cylinders;
  image[0x31] = 1;
  image_length = 256;
  track_count = 0;
}

static void begin_track(uint8_t cylinder) {
  static const char tag[] = "Track-Info\r\n";
  track_at = image_length;
  memcpy(image + track_at, tag, sizeof tag - 1);
  image[track_at + 0x10] = cylinder;
  image[track_at + 0x14] = 2;
  image[track_at + 0x16] = 0x52;
  image[track_at + 0x17] = 0xE5;
  image_length = track_at + 256;
}

static void add_sector(uint8_t c, uint8_t r, uint8_t n, uint8_t status1, uint8_t status2,
                       size_t stored, uint8_t fill) {
  uint8_t index = image[track_at + 0x15]++;
  uint8_t *entry = image + track_at + 0x18 + (size_t)index * 8;
  entry[0] = c;
  entry[2] = r;
  entry[3] = n;
  entry[4] = status1;
  entry[5] = status2;
  entry[6] = (uint8_t)(stored & 0xFF);
  entry[7] = (uint8_t)(stored >> 8);
  memset(image + image_length, fill, stored);
  image_length += stored;
}

static void end_track(void) {
  size_t length = ((image_length - track_at) + 255) / 256 * 256;
  image_length = track_at + length;
  image[0x34 + track_count++] = (uint8_t)(length / 256);
}

/* Four cylinders. The first is plain, interleaved the way the DATA format
   is; the second holds the sectors protections are made of; the third an
   8K sector and one behind it; the fourth is empty of sectors, and there
   is no fifth. */
static void build_disc(void) {
  begin_image(4);
  begin_track(0);
  add_sector(0, 0xC1, 2, 0, 0, 512, 0x11);
  add_sector(0, 0xC3, 2, 0, 0, 512, 0x33);
  add_sector(0, 0xC2, 2, 0, 0, 512, 0x22);
  add_sector(0, 0xC4, 2, 0, 0, 512, 0x44);
  end_track();
  begin_track(1);
  add_sector(1, 0xD1, 2, 0x00, 0x40, 512, 0xD1);  /* deleted */
  add_sector(1, 0xD2, 2, 0x20, 0x20, 512, 0xD2);  /* data check failed */
  add_sector(1, 0xD3, 2, 0x00, 0x00, 1024, 0xA0); /* unstable: two readings */
  memset(image + image_length - 512, 0xA1, 512);
  add_sector(1, 0xD4, 2, 0x20, 0x00, 512, 0xD4); /* identity check failed */
  add_sector(1, 0xD6, 2, 0x00, 0x00, 512, 0xD6);
  end_track();
  begin_track(2);
  add_sector(2, 0xD5, 6, 0x20, 0x20, 0x1800, 0xD5); /* 8K announced, 6K kept */
  add_sector(2, 0xD6, 2, 0x00, 0x00, 512, 0xD6);
  end_track();
  begin_track(3);
  end_track();
}

/* A two-sided disc for the two-headed drive: one cylinder, two sectors a
   side, numbered from 1 as the datasheet's multi-track example is. */
static void build_two_sided_disc(void) {
  build_disc();
  memcpy(two_sided_image, image, image_length);
  begin_image(1);
  image[0x31] = 2;
  for (uint8_t side = 0; side < 2; side++) {
    begin_track(0);
    image[track_at + 0x11] = side;
    add_sector(0, 1, 2, 0, 0, 512, (uint8_t)(0xA1 + side * 0x10));
    add_sector(0, 2, 2, 0, 0, 512, (uint8_t)(0xA2 + side * 0x10));
    image[track_at + 0x18 + 1] = side; /* the identities name their side */
    image[track_at + 0x18 + 8 + 1] = side;
    end_track();
  }
  uint8_t swap[64 * 1024];
  memcpy(swap, image, image_length);
  size_t swap_length = image_length;
  memcpy(image, two_sided_image, sizeof image);
  memcpy(two_sided_image, swap, swap_length);
  TEST_CHECK(dsk_read(&two_sided, two_sided_image, swap_length, &problem));
}

/* The chip turns the drives wired to it. */
static void tick(uint32_t microseconds) {
  while (microseconds-- > 0) {
    upd765_tick(&fdc);
  }
}

static uint8_t status(void) { return upd765_read(&fdc, UPD765_STATUS); }

static void power_on(void) {
  build_disc();
  TEST_CHECK(dsk_read(&floppy, image, image_length, &problem));
  drive_init(&drive_a, 1);
  drive_init(&drive_b, 2);
  drive_insert(&drive_a, &floppy);
  upd765_init(&fdc);
  upd765_attach(&fdc, 0, &drive_a);
  upd765_attach(&fdc, 1, &drive_b);
  drive_set_motor(&drive_a, true);
  drive_set_motor(&drive_b, true);
}

/* What AMSDOS does before touching the disc: let the motor's change of
   READY be noticed, then Sense Interrupt Status until one comes back
   invalid. */
static void drain_interrupts(void);

/* The AMSDOS Specify: 12ms steps, 32ms unload, 4ms load, no DMA. */
static const uint8_t amsdos_specify[] = {0x03, 0xA1, 0x03};

/* Write command bytes as the ROM does: each once RQM is up and DIO says
   the chip is listening. Ticks while it waits, and gives up loudly. */
static void send(const uint8_t *bytes, size_t count) {
  for (size_t index = 0; index < count; index++) {
    uint32_t patience = 1000;
    while ((status() & (UPD765_MSR_RQM | UPD765_MSR_DIO)) != UPD765_MSR_RQM && patience-- > 0) {
      tick(1);
    }
    if (patience == 0) {
      TEST_FAIL("the chip would not take command byte %zu", index);
      return;
    }
    upd765_write(&fdc, UPD765_DATA, bytes[index]);
  }
}

/* The execution phase as software runs it: a byte moved on every RQM, in
   the direction DIO gives, until EXM drops. `supply` feeds writes. Returns
   the bytes moved and the microseconds it took. */
/* Where the disc stood when each byte was handed to the chip. */
static uint32_t supplied_at[64];

static uint32_t execute(uint8_t *received, uint32_t capacity, const uint8_t *supply,
                        uint32_t supplied, uint32_t *microseconds, uint32_t dawdle) {
  uint32_t moved = 0;
  uint32_t elapsed = 0;
  uint32_t patience = 5000000; /* five seconds of disc */
  for (;;) {
    uint8_t now = status();
    if ((now & UPD765_MSR_CB) == 0 || ((now & UPD765_MSR_RQM) && (now & UPD765_MSR_EXM) == 0)) {
      break; /* the result phase, or nothing at all */
    }
    if (now & UPD765_MSR_RQM) {
      if (dawdle > 0) {
        tick(dawdle);
        elapsed += dawdle;
        now = status();
        if ((now & UPD765_MSR_EXM) == 0) {
          continue; /* too late: the chip has moved on */
        }
      }
      if (now & UPD765_MSR_DIO) {
        uint8_t byte = upd765_read(&fdc, UPD765_DATA);
        if (moved < capacity) {
          received[moved] = byte;
        }
      } else {
        if (moved < 64) {
          supplied_at[moved] = drive_a.position;
        }
        upd765_write(&fdc, UPD765_DATA, moved < supplied ? supply[moved] : 0x00);
      }
      moved++;
      continue;
    }
    if (patience-- == 0) {
      TEST_FAIL("the execution phase never ended");
      break;
    }
    tick(1);
    elapsed++;
  }
  if (microseconds != NULL) {
    *microseconds = elapsed;
  }
  return moved;
}

/* Read the result phase as the ROM does: a byte on each RQM while CB
   holds, then CB must drop. */
static uint8_t collect(uint8_t *result, size_t capacity) {
  uint8_t count = 0;
  memset(result, 0, capacity);
  while (status() & UPD765_MSR_CB) {
    if ((status() & (UPD765_MSR_RQM | UPD765_MSR_DIO)) != (UPD765_MSR_RQM | UPD765_MSR_DIO)) {
      TEST_FAIL("a result byte was not offered");
      break;
    }
    uint8_t byte = upd765_read(&fdc, UPD765_DATA);
    if (count < capacity) {
      result[count] = byte;
    }
    count++;
  }
  return count;
}

static void drain_interrupts(void) {
  const uint8_t sense[1] = {0x08};
  tick(2100);
  for (int attempts = 0; attempts < 8; attempts++) {
    uint8_t result[2];
    send(sense, 1);
    if (collect(result, sizeof result) == 1 && result[0] == UPD765_ST0_INVALID) {
      return;
    }
  }
  TEST_FAIL("the interrupts would not drain");
}

static const uint8_t *read_data_command(uint8_t r, uint8_t eot) {
  static uint8_t command[9];
  const uint8_t template[9] = {0x66, 0x00, 0x00, 0x00, r, 0x02, eot, 0x2A, 0xFF};
  memcpy(command, template, sizeof command);
  return command;
}

static void the_handshake_begins_idle(void) {
  power_on();
  TEST_EQUAL(status(), UPD765_MSR_RQM);
  upd765_write(&fdc, UPD765_DATA, 0x66);
  TEST_EQUAL(status(), UPD765_MSR_RQM | UPD765_MSR_CB);
}

/* Read Data as AMSDOS issues it: MFM, skip, one sector with EOT set to R.
   Without terminal count the chip steps past the last sector asked for,
   which is the end of the cylinder — an abnormal end carrying EN, and the
   ROM's own test for success. */
static void a_sector_reads_as_amsdos_expects(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  send(read_data_command(0xC2, 0xC2), 9);
  uint8_t data[1024] = {0};
  uint32_t took = 0;
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, &took, 0), 512);
  TEST_EQUAL(data[0], 0x22);
  TEST_EQUAL(data[511], 0x22);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_EQUAL(result[2], 0);
  TEST_EQUAL(result[3], 0);
  TEST_EQUAL(result[4], 0);
  TEST_EQUAL(result[5], 0xC2);
  TEST_EQUAL(result[6], 2);
  TEST_EQUAL(status(), UPD765_MSR_RQM); /* idle again: CB down, DIO down */
  /* C2 is the third sector past the index: the head load, then three
     identities and two whole sectors go by before its data does. */
  uint32_t data_at = 146 + 2 * (62 + 512 + 0x52) + 60;
  TEST_CHECK(took >= 4000 + data_at * 32);
  TEST_CHECK(took < 4000 + (data_at + 520) * 32);
}

/* The bytes cross one every 32µs, and one not collected in time is lost:
   overrun, and the command ends. */
static void a_late_processor_overruns(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  send(read_data_command(0xC1, 0xC1), 9);
  uint8_t data[8] = {0};
  TEST_CHECK(execute(data, sizeof data, NULL, 0, NULL, 40) < 512);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_OR);
  /* Just in time is in time. */
  send(read_data_command(0xC1, 0xC1), 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 31), 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
}

static void sectors_follow_one_another_until_eot(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  send(read_data_command(0xC1, 0xC3), 9);
  uint8_t data[1536] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 1536);
  TEST_EQUAL(data[0], 0x11);
  TEST_EQUAL(data[512], 0x22);
  TEST_EQUAL(data[1024], 0x33);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_EQUAL(result[5], 0xC3); /* R stays at EOT: MAME and Arnold agree */
}

/* Terminal count held high ends the transfer normally, with the sector in
   hand, which is what a board that wires it gets. */
static void terminal_count_ends_a_transfer_normally(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  upd765_set_terminal_count(&fdc, true);
  send(read_data_command(0xC1, 0xC9), 9);
  uint8_t data[512] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);
  TEST_EQUAL(result[1], 0);
}

static void a_sector_that_is_not_there_is_no_data(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  send(read_data_command(0xC9, 0xC9), 9);
  uint8_t data[8] = {0};
  uint32_t took = 0;
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, &took, 0), 0);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_ND);
  TEST_EQUAL(result[2], 0);
  /* Two passes of the index: between one and two revolutions of looking. */
  TEST_CHECK(took > 200000);
  TEST_CHECK(took < 404000 + 4000);
}

/* A wrong cylinder is reported as such; a track with nothing on it is a
   missing address mark, not a missing sector. */
static void what_is_missing_decides_the_flags(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t wrong_cylinder[9] = {0x66, 0x00, 0x05, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF};
  send(wrong_cylinder, 9);
  uint8_t data[8] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_ND);
  TEST_EQUAL(result[2], UPD765_ST2_WC);

  drive_a.cylinder = 3; /* formatted, empty */
  send(read_data_command(0xC1, 0xC1), 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_MA);

  drive_a.cylinder = 4; /* past the disc */
  send(read_data_command(0xC1, 0xC1), 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_MA);
}

static void read_id_returns_the_next_identity_to_pass(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  const uint8_t read_id[2] = {0x4A, 0x00};
  send(read_id, 2);
  uint8_t data[8] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);
  TEST_EQUAL(result[5], 0xC1); /* the first past the index */
  send(read_id, 2);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[5], 0xC3); /* the next one along */
  TEST_EQUAL(result[3], 0);
  TEST_EQUAL(result[6], 2);
}

/* A deleted mark under Read Data is a control mark: the data is read and
   the command ends there, normally; with SK the sector is passed over. */
static void a_deleted_mark_is_a_control_mark(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 1;
  uint8_t plain[9] = {0x46, 0x00, 0x01, 0x00, 0xD1, 0x02, 0xD6, 0x2A, 0xFF};
  send(plain, 9);
  uint8_t data[512] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(data[0], 0xD1);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);
  TEST_EQUAL(result[2], UPD765_ST2_CM);

  uint8_t skipping[9] = {0x66, 0x00, 0x01, 0x00, 0xD1, 0x02, 0xD1, 0x2A, 0xFF};
  send(skipping, 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_EQUAL(result[2], UPD765_ST2_CM);

  /* Read Deleted Data is the mirror: the deleted one is plain to it. */
  uint8_t read_deleted[9] = {0x4C, 0x00, 0x01, 0x00, 0xD1, 0x02, 0xD1, 0x2A, 0xFF};
  send(read_deleted, 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_EQUAL(result[2], 0);
}

static void a_failed_check_is_reported_after_the_data(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 1;
  uint8_t command[9] = {0x66, 0x00, 0x01, 0x00, 0xD2, 0x02, 0xD2, 0x2A, 0xFF};
  send(command, 9);
  uint8_t data[512] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_DE);
  TEST_EQUAL(result[2], UPD765_ST2_DD);

  uint8_t bad_identity[9] = {0x66, 0x00, 0x01, 0x00, 0xD4, 0x02, 0xD4, 0x2A, 0xFF};
  send(bad_identity, 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_DE);
  TEST_EQUAL(result[2], 0);
}

static void an_unstable_sector_reads_differently_each_revolution(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 1;
  uint8_t command[9] = {0x66, 0x00, 0x01, 0x00, 0xD3, 0x02, 0xD3, 0x2A, 0xFF};
  uint8_t data[512] = {0};
  uint8_t result[7];
  send(command, 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  uint8_t first = data[0];
  send(command, 9);
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_CHECK(data[0] != first);
  TEST_CHECK((first == 0xA0 && data[0] == 0xA1) || (first == 0xA1 && data[0] == 0xA0));
}

/* An 8K sector read whole: the six thousand bytes the disc holds, then
   its failed check and the next sector's own sync, marks and identity —
   what a protection reading past the end is looking for. Such a sector
   fills a revolution on its own, so the layout leaves it no gap. */
static void reading_an_8k_sector_runs_into_the_next(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 2;
  uint8_t command[9] = {0x66, 0x00, 0x02, 0x00, 0xD5, 0x06, 0xD5, 0x2A, 0xFF};
  send(command, 9);
  static uint8_t data[8192] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 8192);
  TEST_EQUAL(data[0], 0xD5);
  TEST_EQUAL(data[0x17FF], 0xD5);
  uint32_t next = 0x1800 + 2; /* past the check, the next sector */
  TEST_EQUAL(data[next], 0x00);
  TEST_EQUAL(data[next + 12], 0xA1);
  TEST_EQUAL(data[next + 15], 0xFE);
  TEST_EQUAL(data[next + 16], 0x02);
  TEST_EQUAL(data[next + 18], 0xD6);
  TEST_EQUAL(data[next + 60], 0xD6);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_DE);
  TEST_EQUAL(result[2], UPD765_ST2_DD);
}

static void a_write_lands_on_the_disc(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t command[9] = {0x45, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF};
  static uint8_t supply[512];
  for (int index = 0; index < 512; index++) {
    supply[index] = (uint8_t)index;
  }
  send(command, 9);
  uint8_t none[1];
  TEST_EQUAL(execute(none, 0, supply, sizeof supply, NULL, 0), 512);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_CHECK(floppy.modified);

  send(read_data_command(0xC3, 0xC3), 9);
  uint8_t data[512] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_CHECK(memcmp(data, supply, 512) == 0);

  /* Late with a byte is an overrun on a write too. */
  send(command, 9);
  TEST_CHECK(execute(none, 0, supply, sizeof supply, NULL, 40) < 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_OR);
}

static void a_protected_disc_is_not_writeable(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  floppy.write_protected = true;
  uint8_t command[9] = {0x45, 0x00, 0x00, 0x00, 0xC3, 0x02, 0xC3, 0x2A, 0xFF};
  send(command, 9);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_NW);
  TEST_CHECK(!floppy.modified);
}

static void a_track_formats_from_the_index(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t command[6] = {0x4D, 0x00, 0x02, 0x04, 0x52, 0xE5};
  const uint8_t identities[16] = {0, 0, 0x41, 2, 0, 0, 0x42, 2, 0, 0, 0x43, 2, 0, 0, 0x44, 2};
  send(command, 6);
  uint8_t none[1];
  uint32_t took = 0;
  TEST_EQUAL(execute(none, 0, identities, sizeof identities, &took, 0), 16);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);
  TEST_EQUAL(result[5], 0x44);
  /* From the index round to the index again: a revolution or so. Each
     identity is asked for a byte before its C is due, sixteen bytes into
     its sector, the sectors 62 + 512 + &52 bytes apart from the preamble
     on. */
  TEST_CHECK(took >= 200000);
  TEST_CHECK(took < 400000 + 4000);
  TEST_EQUAL(supplied_at[0], FLOPPY_TRACK_PREAMBLE + FLOPPY_ID_FIELD - 1);
  TEST_EQUAL(supplied_at[4], supplied_at[0] + 62 + 512 + 0x52);
  TEST_EQUAL(supplied_at[12], supplied_at[0] + 3 * (62 + 512 + 0x52));
  TEST_EQUAL(floppy_sector_count(&floppy, 0, 0), 4);
  TEST_EQUAL(floppy_sector(&floppy, 0, 0, 2)->r, 0x43);

  uint8_t read[9] = {0x66, 0x00, 0x00, 0x00, 0x42, 0x02, 0x42, 0x2A, 0xFF};
  send(read, 9);
  uint8_t data[512] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(data[0], 0xE5);
  TEST_EQUAL(data[511], 0xE5);
}

/* Seeks step the drive on the chip's own time, at the rate Specify set,
   and end in an interrupt that Sense Interrupt Status collects. Asked too
   soon, it has nothing to say and is an invalid command. */
static void a_seek_steps_at_the_specified_rate(void) {
  power_on();
  drain_interrupts();
  send(amsdos_specify, sizeof amsdos_specify);
  const uint8_t seek[3] = {0x0F, 0x00, 5};
  send(seek, 3);
  TEST_EQUAL(status(), UPD765_MSR_RQM | UPD765_MSR_DRIVE_BUSY(0));
  /* The first step at once and one every 12ms after; the end is seen one
     step time after the last. */
  tick(3 * 12000 + 100);
  TEST_EQUAL(drive_a.cylinder, 4);
  const uint8_t sense[1] = {0x08};
  send(sense, 1);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST0_INVALID);
  tick(2 * 12000);
  TEST_EQUAL(drive_a.cylinder, 5);
  /* The drive stays in seek mode until its end is heard. */
  TEST_EQUAL(status(), UPD765_MSR_RQM | UPD765_MSR_DRIVE_BUSY(0));
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_SE);
  TEST_EQUAL(result[1], 5);
  TEST_EQUAL(status(), UPD765_MSR_RQM);

  /* Recalibrate steps back to track 0 and reports it. */
  const uint8_t recalibrate[2] = {0x07, 0x00};
  send(recalibrate, 2);
  tick(6 * 12000 + 100);
  TEST_EQUAL(drive_a.cylinder, 0);
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_SE);
  TEST_EQUAL(result[1], 0);

  /* Seeking a drive with nothing in it ends in not ready. */
  const uint8_t seek_b[3] = {0x0F, 0x01, 3};
  send(seek_b, 3);
  tick(1);
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL | UPD765_ST0_SE | UPD765_ST0_NR | 1);
}

static void recalibrate_gives_up_after_77_steps(void) {
  power_on();
  drain_interrupts();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 90;
  const uint8_t recalibrate[2] = {0x07, 0x00};
  send(recalibrate, 2);
  tick(79 * 12000);
  TEST_EQUAL(drive_a.cylinder, 13);
  const uint8_t sense[1] = {0x08};
  send(sense, 1);
  uint8_t result[2];
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_SE | UPD765_ST0_EC);
}

static void sense_drive_status_reads_the_lines(void) {
  power_on();
  const uint8_t sense_a[2] = {0x04, 0x00};
  send(sense_a, 2);
  uint8_t result[1];
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST3_RY | UPD765_ST3_T0);
  drive_a.cylinder = 7;
  floppy.write_protected = true;
  send(sense_a, 2);
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST3_RY | UPD765_ST3_WP);
  const uint8_t sense_b[2] = {0x04, 0x05}; /* head 1, unit 1: two-sided, empty */
  send(sense_b, 2);
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST3_TS | UPD765_ST3_T0 | UPD765_ST3_HD | 1);
}

/* Between commands the drives are polled, and a change of READY leaves an
   interrupt saying which way it went. */
static void a_change_of_ready_is_an_interrupt(void) {
  power_on();
  const uint8_t sense[1] = {0x08};
  uint8_t result[2];
  tick(2100);
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_READY_CHANGED); /* unit 0 became ready */
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST0_INVALID); /* unit 1 never was */
  drive_set_motor(&drive_a, false);
  tick(2100);
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_READY_CHANGED | UPD765_ST0_NR);
}

static void a_drive_that_is_not_ready_ends_a_command_at_once(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_set_motor(&drive_a, false);
  send(read_data_command(0xC1, 0xC1), 9);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL | UPD765_ST0_NR);
  TEST_EQUAL(result[5], 0xC1);

  /* And one that stops being ready under a command ends it with the
     code for that. */
  drive_set_motor(&drive_a, true);
  drain_interrupts();
  send(read_data_command(0xC4, 0xC4), 9);
  tick(1000);
  drive_set_motor(&drive_a, false);
  tick(10);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_READY_CHANGED | UPD765_ST0_NR);
}

static void an_unknown_command_is_invalid(void) {
  power_on();
  const uint8_t version[1] = {0x10};
  send(version, 1);
  uint8_t result[1];
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST0_INVALID);
  TEST_EQUAL(status(), UPD765_MSR_RQM);
}

/* The board writes the data register whichever way A0 lies. */
static void a_write_with_a0_low_reaches_the_data_register(void) {
  power_on();
  upd765_write(&fdc, UPD765_STATUS, 0x04);
  TEST_EQUAL(status(), UPD765_MSR_RQM | UPD765_MSR_CB);
  upd765_write(&fdc, UPD765_STATUS, 0x00);
  uint8_t result[1];
  TEST_EQUAL(collect(result, sizeof result), 1);
  TEST_EQUAL(result[0], UPD765_ST3_RY | UPD765_ST3_T0);
}

/* Write Deleted Data leaves the other kind of mark, which Read Data then
   reports as a control mark. */
static void write_deleted_data_leaves_a_deleted_mark(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t command[9] = {0x49, 0x00, 0x00, 0x00, 0xC4, 0x02, 0xC4, 0x2A, 0xFF};
  static uint8_t supply[512];
  memset(supply, 0x99, sizeof supply);
  send(command, 9);
  uint8_t none[1];
  TEST_EQUAL(execute(none, 0, supply, sizeof supply, NULL, 0), 512);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_CHECK(floppy_sector(&floppy, 0, 0, 3)->deleted);

  uint8_t plain_read[9] = {0x46, 0x00, 0x00, 0x00, 0xC4, 0x02, 0xC4, 0x2A, 0xFF}; /* no skip */
  send(plain_read, 9);
  uint8_t data[512] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 512);
  TEST_EQUAL(data[0], 0x99);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);
  TEST_EQUAL(result[2], UPD765_ST2_CM);
}

/* Read a Track begins at the index and takes the sectors as they come,
   whatever they are numbered, for as many as EOT counts; an identity that
   is not the one asked for is noted and read anyway. */
static void read_track_takes_sectors_as_they_pass(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t command[9] = {0x42, 0x00, 0x00, 0x00, 0xC1, 0x02, 0x04, 0x2A, 0xFF};
  send(command, 9);
  static uint8_t data[2048];
  uint32_t took = 0;
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, &took, 0), 2048);
  TEST_EQUAL(data[0], 0x11); /* C1, C3, C2, C4: the order on the disc */
  TEST_EQUAL(data[512], 0x33);
  TEST_EQUAL(data[1024], 0x22);
  TEST_EQUAL(data[1536], 0x44);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN | UPD765_ST1_ND);
  TEST_EQUAL(result[5], 0xC1);
  /* It waited for the index first: most of a revolution before the first
     sector, on a disc that had just started turning. */
  TEST_CHECK(took > 200000);
}

/* Scan Equal compares what the processor supplies with what the disc
   holds, sector after sector, and stops at the first that matches; &FF
   from the processor matches anything. */
static void scan_equal_finds_the_matching_sector(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  static uint8_t supply[2048];
  memset(supply, 0x22, sizeof supply);
  supply[512 + 100] = 0xFF;
  uint8_t command[9] = {0x51, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC4, 0x2A, 0x01};
  send(command, 9);
  uint8_t none[1];
  TEST_EQUAL(execute(none, 0, supply, sizeof supply, NULL, 0), 1024); /* C1, then C2 */
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);
  TEST_EQUAL(result[2], UPD765_ST2_SH);
  TEST_EQUAL(result[5], 0xC2);

  memset(supply, 0x55, sizeof supply);
  send(command, 9);
  TEST_EQUAL(execute(none, 0, supply, sizeof supply, NULL, 0), 2048); /* all four, none of them */
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_EQUAL(result[2], UPD765_ST2_SN);
}

/* With MT set a read that reaches EOT on side 0 goes on from sector 1 of
   side 1, and ends at EOT there. */
static void multi_track_continues_on_the_other_side(void) {
  power_on();
  build_two_sided_disc();
  drive_insert(&drive_b, &two_sided);
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t command[9] = {0xC6, 0x01, 0x00, 0x00, 0x01, 0x02, 0x02, 0x2A, 0xFF};
  send(command, 9);
  static uint8_t data[2048];
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 2048);
  TEST_EQUAL(data[0], 0xA1);
  TEST_EQUAL(data[512], 0xA2);
  TEST_EQUAL(data[1024], 0xB1);
  TEST_EQUAL(data[1536], 0xB2);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL | UPD765_ST0_HD | 1);
  TEST_EQUAL(result[1], UPD765_ST1_EN);
  TEST_EQUAL(result[4], 1); /* H */
  TEST_EQUAL(result[5], 2); /* R */
  TEST_CHECK(drive_b.side);
}

/* A seek still stepping moves the head from under a read. The identity the
   search settled on is gone with the track; the chip looks again where
   the head now is, and the command ends one way or another. */
static void a_seek_under_a_read_moves_the_search(void) {
  power_on();
  drain_interrupts();
  const uint8_t slow[] = {0x03, 0x01, 0x03}; /* 32ms a step */
  send(slow, sizeof slow);
  const uint8_t seek[3] = {0x0F, 0x00, 3};
  send(seek, 3);
  uint8_t command[9] = {0x66, 0x00, 0x02, 0x00, 0xD5, 0x06, 0xD5, 0x2A, 0xFF};
  send(command, 9);
  static uint8_t data[8192] = {0};
  execute(data, sizeof data, NULL, 0, NULL, 0);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0] & 0xC0, UPD765_ST0_ABNORMAL);
  TEST_EQUAL(drive_a.cylinder, 3);
}

/* The window is one byte time exactly. */
static void the_overrun_window_is_one_byte_time(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  send(read_data_command(0xC1, 0xC1), 9);
  uint8_t data[8] = {0};
  TEST_CHECK(execute(data, sizeof data, NULL, 0, NULL, 32) < 512);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[1], UPD765_ST1_OR);
}

/* A change of READY found while an earlier interrupt waits is reported
   once that one has been collected, not lost. */
static void a_ready_change_waits_behind_a_pending_interrupt(void) {
  power_on();
  drain_interrupts();
  send(amsdos_specify, sizeof amsdos_specify);
  const uint8_t seek[3] = {0x0F, 0x00, 1};
  send(seek, 3);
  tick(2 * 12000 + 100); /* the seek has ended; nobody has asked */
  drive_set_motor(&drive_a, false);
  tick(2100);
  const uint8_t sense[1] = {0x08};
  uint8_t result[2];
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_SE);
  tick(2100);
  send(sense, 1);
  TEST_EQUAL(collect(result, sizeof result), 2);
  TEST_EQUAL(result[0], UPD765_ST0_READY_CHANGED | UPD765_ST0_NR);
}

/* Read a Track reads on past a failed check, noting it, and past an
   identity that is not the one asked for. */
static void read_track_reads_on_past_a_failed_check(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 1;
  uint8_t command[9] = {0x42, 0x00, 0x01, 0x00, 0xD1, 0x02, 0x05, 0x2A, 0xFF};
  send(command, 9);
  static uint8_t data[2560] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 2560);
  TEST_EQUAL(data[0], 0xD1);
  TEST_EQUAL(data[512], 0xD2);
  TEST_EQUAL(data[1536], 0xD4);
  TEST_EQUAL(data[2048], 0xD6);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_EN | UPD765_ST1_DE | UPD765_ST1_ND);
  TEST_EQUAL(result[2], UPD765_ST2_DD);
}

/* Read ID answers with the first identity to pass, sound or not. */
static void read_id_reports_an_identity_that_fails_its_check(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  drive_a.cylinder = 1;
  /* The head loads for 4ms, 125 bytes of disc, before the search begins:
     start that much and a little before D4. */
  drive_a.position = floppy_sector(&floppy, 1, 0, 3)->position - 130;
  const uint8_t read_id[2] = {0x4A, 0x00};
  send(read_id, 2);
  uint8_t data[8] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_DE);
  TEST_EQUAL(result[5], 0xD4);
}

/* A field formatted at one length under an identity announcing another
   fails its check when read by the identity's length, as it does on the
   disc: the check found there was written for the whole field. */
static void a_field_of_the_wrong_length_fails_its_check(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t format[6] = {0x4D, 0x00, 0x02, 0x01, 0x52, 0xE5}; /* 512-byte fields */
  const uint8_t identities[4] = {0, 0, 0x41, 1};            /* announcing 256 */
  send(format, 6);
  uint8_t none[1];
  TEST_EQUAL(execute(none, 0, identities, sizeof identities, NULL, 0), 4);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_NORMAL);

  uint8_t read[9] = {0x66, 0x00, 0x00, 0x00, 0x41, 0x01, 0x41, 0x2A, 0xFF};
  send(read, 9);
  uint8_t data[256] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 256);
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_DE);
  TEST_EQUAL(result[2], UPD765_ST2_DD);
}

/* A read in FM finds nothing on an MFM track: no identity in two passes of
   the index. */
static void an_fm_read_finds_nothing_on_an_mfm_track(void) {
  power_on();
  send(amsdos_specify, sizeof amsdos_specify);
  uint8_t command[9] = {0x26, 0x00, 0x00, 0x00, 0xC1, 0x02, 0xC1, 0x2A, 0xFF}; /* MF clear */
  send(command, 9);
  uint8_t data[8] = {0};
  TEST_EQUAL(execute(data, sizeof data, NULL, 0, NULL, 0), 0);
  uint8_t result[7];
  TEST_EQUAL(collect(result, sizeof result), 7);
  TEST_EQUAL(result[0], UPD765_ST0_ABNORMAL);
  TEST_EQUAL(result[1], UPD765_ST1_MA);
}

int main(void) {
  TEST_RUN(the_handshake_begins_idle);
  TEST_RUN(a_sector_reads_as_amsdos_expects);
  TEST_RUN(a_late_processor_overruns);
  TEST_RUN(sectors_follow_one_another_until_eot);
  TEST_RUN(terminal_count_ends_a_transfer_normally);
  TEST_RUN(a_sector_that_is_not_there_is_no_data);
  TEST_RUN(what_is_missing_decides_the_flags);
  TEST_RUN(read_id_returns_the_next_identity_to_pass);
  TEST_RUN(a_deleted_mark_is_a_control_mark);
  TEST_RUN(a_failed_check_is_reported_after_the_data);
  TEST_RUN(an_unstable_sector_reads_differently_each_revolution);
  TEST_RUN(reading_an_8k_sector_runs_into_the_next);
  TEST_RUN(a_write_lands_on_the_disc);
  TEST_RUN(a_protected_disc_is_not_writeable);
  TEST_RUN(a_track_formats_from_the_index);
  TEST_RUN(a_seek_steps_at_the_specified_rate);
  TEST_RUN(recalibrate_gives_up_after_77_steps);
  TEST_RUN(sense_drive_status_reads_the_lines);
  TEST_RUN(a_change_of_ready_is_an_interrupt);
  TEST_RUN(a_drive_that_is_not_ready_ends_a_command_at_once);
  TEST_RUN(an_unknown_command_is_invalid);
  TEST_RUN(a_write_with_a0_low_reaches_the_data_register);
  TEST_RUN(write_deleted_data_leaves_a_deleted_mark);
  TEST_RUN(read_track_takes_sectors_as_they_pass);
  TEST_RUN(scan_equal_finds_the_matching_sector);
  TEST_RUN(multi_track_continues_on_the_other_side);
  TEST_RUN(a_seek_under_a_read_moves_the_search);
  TEST_RUN(the_overrun_window_is_one_byte_time);
  TEST_RUN(a_ready_change_waits_behind_a_pending_interrupt);
  TEST_RUN(read_track_reads_on_past_a_failed_check);
  TEST_RUN(read_id_reports_an_identity_that_fails_its_check);
  TEST_RUN(a_field_of_the_wrong_length_fails_its_check);
  TEST_RUN(an_fm_read_finds_nothing_on_an_mfm_track);
  return TEST_REPORT("upd765");
}
