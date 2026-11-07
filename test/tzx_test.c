/*
 * tzx_test — the two formats, read into pulses.
 *
 * Every length here is written out as the specification writes it, not taken
 * from the reader's own constants: a test that reads tzx.h grades tzx.c
 * against its own arithmetic and would agree with any mistake in it.
 *
 * Sources:
 * - "TZX format" v1.13 (Tomaz Kac, maintained by Martijn van der Heide),
 *   https://worldofspectrum.net/TZXformat.html — the signature, each block's
 *   layout, and the standard ROM timings tabulated in block ID 11: pilot
 *   pulses of 2168 T-states, 8063 of them before a header and 3223 before
 *   data, syncs of 667 and 735, bits of 855 and 1710, a second of pause; and
 *   the loop blocks' own words, quoted where each is tested.
 * - libspectrum's tape tests (Philip Kendall and others, Fuse),
 *   https://sourceforge.net/p/fuse-emulator/libspectrum/ci/master/tree/test/
 *   — loop.tzx, loop2.tzx and loopend.tzx, and the bugs `tape.c` there names
 *   them for, #113 and #118. Copied byte for byte; what we kept is that the
 *   first two play to their end, and what we changed is the third, which is
 *   refused here.
 */
#include <string.h>

#include "test.h"
#include "tzx.h"

/* A Spectrum's, so a pause in milliseconds lands where a Spectrum puts it. */
#define TICKS_PER_MILLISECOND 3500
#define MOST_PILOT_PULSES 20000

/* The cassette's leader tape, restated here rather than read from tzx.c:
   350mm by GOST 20492-87's "(350±100) мм", less the 79mm between the
   take-up hub's clamp and the head, at the compact cassette's 4.76cm a
   second. */
#define LEADER_MILLISECONDS 5700

static tzx_t reader;

static bool opened(const uint8_t *image, uint32_t length, uint32_t clock, tzx_machine machine) {
  const char *problem = NULL;
  if (!tzx_open(&reader, image, length, clock, machine, &problem)) {
    TEST_FAIL("refused: %s", problem);
    memset(&reader, 0, sizeof reader); /* so the rest of the test reads an empty tape */
    return false;
  }
  return true;
}

/* A cassette opens with its leader tape, which the reader plays before the
   image wherever the image has not kept that much silence of its own. The
   tests of the leader grade it; every other test steps over it, low and no
   longer than the leader is, to the image behind. */
static void open_image_for(const uint8_t *image, uint32_t length, uint32_t clock,
                           tzx_machine machine) {
  if (!opened(image, length, clock, machine)) {
    return;
  }
  tape_pulse_t leader = {0, true};
  TEST_CHECK(tzx_next_pulse(&reader, &leader));
  TEST_CHECK(!leader.level);
  TEST_CHECK(leader.ticks <= LEADER_MILLISECONDS * clock);
}

static void open_image(const uint8_t *image, uint32_t length) {
  open_image_for(image, length, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
}

static const char *refusal_of(const uint8_t *image, uint32_t length) {
  const char *problem = NULL;
  if (tzx_open(&reader, image, length, TICKS_PER_MILLISECOND, TZX_SPECTRUM, &problem)) {
    return NULL;
  }
  return problem;
}

/* Which refusal a block earns, by the words that tell the two apart. */
static bool refused_for(const uint8_t *image, uint32_t length, const char *words) {
  const char *problem = refusal_of(image, length);
  return problem != NULL && strstr(problem, words) != NULL;
}

/* The next pulse's length, or 0 when the tape has run out. */
static uint32_t next(void) {
  tape_pulse_t pulse = {0, false};
  return tzx_next_pulse(&reader, &pulse) ? pulse.ticks : 0;
}

/* A block leaves the level where its last pulse put it, so the pause that
   follows spends a millisecond finishing that edge before the silence. */
static void the_pause_reads(uint32_t milliseconds) {
  tape_pulse_t lead = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &lead));
  TEST_EQUAL(lead.ticks, 1 * TICKS_PER_MILLISECOND);
  TEST_CHECK(lead.level); /* the level the block left, so its edge is finished */

  tape_pulse_t silence = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &silence));
  TEST_EQUAL(silence.ticks, (milliseconds - 1) * TICKS_PER_MILLISECOND);
  TEST_CHECK(!silence.level); /* and then the silence, which is low */
}

/* A tape that stops or runs out on a low pulse holds the other level for the
   same millisecond, so that pulse has the edge that ends it, and only then
   falls quiet. */
static void the_last_edge_is_finished(void) {
  tape_pulse_t edge = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &edge));
  TEST_EQUAL(edge.ticks, 1 * TICKS_PER_MILLISECOND);
  TEST_CHECK(edge.level);
}

static bool next_level(void) {
  tape_pulse_t pulse = {0, false};
  return tzx_next_pulse(&reader, &pulse) && pulse.level;
}

/* A block's pilot, its two syncs, and its bytes bit by bit, most significant
   first. Leaves the reader on the pause that follows. */
static void the_block_reads(int want_pilot, uint32_t pilot, uint32_t first, uint32_t second,
                            uint32_t zero, uint32_t one, const uint8_t *bytes, size_t count) {
  int pilots = 0;
  uint32_t length = next();
  while (length == pilot && pilots < MOST_PILOT_PULSES) {
    pilots++;
    length = next();
  }
  TEST_EQUAL(pilots, want_pilot);
  TEST_EQUAL(length, first);
  TEST_EQUAL(next(), second);
  for (size_t index = 0; index < count; index++) {
    for (int bit = 7; bit >= 0; bit--) {
      uint32_t want = (bytes[index] >> bit) & 1 ? one : zero;
      TEST_EQUAL(next(), want);
      TEST_EQUAL(next(), want);
    }
  }
}

/* --- a .tap, which has no signature and no timings of its own --- */

static const uint8_t two_blocks[] = {0x02, 0x00, 0x00, 0xAA, 0x02, 0x00, 0xFF, 0x55};

static void a_tap_plays_at_the_roms_own_timings(void) {
  open_image(two_blocks, sizeof two_blocks);
  const uint8_t header[] = {0x00, 0xAA};
  the_block_reads(8063, 2168, 667, 735, 855, 1710, header, sizeof header);
  the_pause_reads(1000);
  const uint8_t data[] = {0xFF, 0x55};
  the_block_reads(3223, 2168, 667, 735, 855, 1710, data, sizeof data);
  the_pause_reads(1000);
  TEST_EQUAL(next(), 0); /* and the tape has run out */
}

static void the_pilot_turns_on_the_flag_bytes_top_bit(void) {
  const uint8_t just_under[] = {0x02, 0x00, 0x7F, 0x00};
  open_image(just_under, sizeof just_under);
  int pilots = 0;
  while (next() == 2168 && pilots < MOST_PILOT_PULSES) {
    pilots++;
  }
  TEST_EQUAL(pilots, 8063);

  const uint8_t just_over[] = {0x02, 0x00, 0x80, 0x00};
  open_image(just_over, sizeof just_over);
  pilots = 0;
  while (next() == 2168 && pilots < MOST_PILOT_PULSES) {
    pilots++;
  }
  TEST_EQUAL(pilots, 3223);
}

static void bytes_that_are_not_blocks_are_refused(void) {
  const uint8_t truncated[] = {0x08, 0x00, 0x00, 0xAA};
  TEST_CHECK(refusal_of(truncated, sizeof truncated) != NULL);
  const uint8_t empty_block[] = {0x00, 0x00};
  TEST_CHECK(refusal_of(empty_block, sizeof empty_block) != NULL);
  const uint8_t trailing[] = {0x02, 0x00, 0xFF, 0x55, 0x00};
  TEST_CHECK(refusal_of(trailing, sizeof trailing) != NULL);
  TEST_CHECK(refusal_of(two_blocks, 0) != NULL);
}

/* --- a .tzx, which names each block and may say its own timings --- */

#define TZX_HEAD 'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20

/* A standard block takes the ROM's timings and reads its pilot off the flag
   byte, exactly as a .tap does. */
static void a_standard_blocks_pilot_turns_on_its_flag(void) {
  const uint8_t header[] = {TZX_HEAD, 0x10, 0x00, 0x00, 0x01, 0x00, 0x7F};
  open_image(header, sizeof header);
  int pilots = 0;
  while (next() == 2168 && pilots < MOST_PILOT_PULSES) {
    pilots++;
  }
  TEST_EQUAL(pilots, 8063);

  const uint8_t data[] = {TZX_HEAD, 0x10, 0x00, 0x00, 0x01, 0x00, 0x80};
  open_image(data, sizeof data);
  pilots = 0;
  while (next() == 2168 && pilots < MOST_PILOT_PULSES) {
    pilots++;
  }
  TEST_EQUAL(pilots, 3223);
}

static void a_tzx_is_known_by_its_signature(void) {
  /* ID 10: a standard block, pause 1000ms, two bytes. */
  const uint8_t image[] = {TZX_HEAD, 0x10, 0xE8, 0x03, 0x02, 0x00, 0xFF, 0x55};
  open_image(image, sizeof image);
  TEST_CHECK(reader.has_block_ids);
  const uint8_t data[] = {0xFF, 0x55};
  the_block_reads(3223, 2168, 667, 735, 855, 1710, data, sizeof data);
  the_pause_reads(1000);
  TEST_EQUAL(next(), 0);

  /* The letters alone are not the signature: the end-of-text byte closes it,
     and without it the bytes are read as a .tap and refused as one. */
  const uint8_t no_end_of_text[] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x00, 0x01, 0x14};
  TEST_CHECK(refused_for(no_end_of_text, sizeof no_end_of_text, "does not divide into blocks"));
}

/* A turbo block says every timing itself, which is how a tape carries a
   loader of its own. */
static void a_turbo_block_says_its_own_timings(void) {
  const uint8_t image[] = {TZX_HEAD, 0x11, 0x00, 0x08, /* pilot 2048 */
                           0x40,     0x01,             /* first sync 320 */
                           0x50,     0x01,             /* second sync 336 */
                           0x00,     0x02,             /* zero bit 512 */
                           0x00,     0x04,             /* one bit 1024 */
                           0x0A,     0x00,             /* ten pilot pulses */
                           0x08,                       /* eight bits in the last byte */
                           0x00,     0x00,             /* no pause */
                           0x01,     0x00, 0x00,       /* one byte of data */
                           0xA5};
  open_image(image, sizeof image);
  const uint8_t data[] = {0xA5};
  the_block_reads(10, 2048, 320, 336, 512, 1024, data, sizeof data);
  TEST_EQUAL(next(), 0); /* no pause, and nothing behind it */
}

static void a_tone_is_one_length_repeated(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x05, 0x00};
  open_image(image, sizeof image);
  for (int pulse = 0; pulse < 5; pulse++) {
    TEST_EQUAL(next(), 1000);
  }
  the_last_edge_is_finished(); /* the fifth pulse was low */
  TEST_EQUAL(next(), 0);
}

static void a_sequence_gives_every_pulse_its_own_length(void) {
  const uint8_t image[] = {TZX_HEAD, 0x13, 0x03, 0x0A, 0x00, 0x14, 0x00, 0x1E, 0x00};
  open_image(image, sizeof image);
  TEST_EQUAL(next(), 10);
  TEST_EQUAL(next(), 20);
  TEST_EQUAL(next(), 30);
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);
}

/* Pure data has no pilot and no sync in front of it, and its last byte may
   carry fewer than eight bits. */
static void pure_data_is_bits_and_nothing_else(void) {
  const uint8_t image[] = {TZX_HEAD, 0x14, 0x00, 0x02, /* zero bit 512 */
                           0x00,     0x04,             /* one bit 1024 */
                           0x03,                       /* three bits of the last byte */
                           0x00,     0x00,             /* no pause */
                           0x01,     0x00, 0x00,       /* one byte */
                           0xA0};
  open_image(image, sizeof image);
  /* &A0 is 101 in its top three bits, and the other five never go out. */
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 512);
  TEST_EQUAL(next(), 512);
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 0);

  /* Only the last byte is partial: the ones in front of it go out whole, so a
     block of two spends eight bits on the first and three on the second. */
  const uint8_t two[] = {TZX_HEAD, 0x14, 0x00, 0x02, 0x00, 0x04, 0x03,
                         0x00,     0x00, 0x02, 0x00, 0x00, 0xFF, 0xA0};
  open_image(two, sizeof two);
  for (int bit = 0; bit < 8; bit++) {
    TEST_EQUAL(next(), 1024); /* &FF, every bit of it */
    TEST_EQUAL(next(), 1024);
  }
  TEST_EQUAL(next(), 1024); /* then &A0's three */
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 512);
  TEST_EQUAL(next(), 512);
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 1024);
  TEST_EQUAL(next(), 0);
}

static void a_pause_block_is_silence_and_a_bare_one_stops_the_tape(void) {
  const uint8_t silence[] = {TZX_HEAD, 0x20, 0x64, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(silence, sizeof silence);
  TEST_EQUAL(next(), 100 * TICKS_PER_MILLISECOND);
  TEST_EQUAL(next(), 1000); /* and the tone behind it still plays */

  const uint8_t stop[] = {TZX_HEAD, 0x20, 0x00, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(stop, sizeof stop);
  TEST_EQUAL(next(), 0); /* a pause of nothing means stop, tone or no tone */
}

/* The blocks that describe rather than record are stepped over by the length
   the specification gives them. */
static void a_block_that_only_describes_is_stepped_over(void) {
  const uint8_t image[] = {TZX_HEAD, 0x30, 0x04, 'n', 'o',  't',  'e',  0x32, 0x05, 0x00,
                           0x01,     0x00, 0x01, 'x', 0x00, 0x12, 0xE8, 0x03, 0x02, 0x00};
  open_image(image, sizeof image);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 0);

  /* One of every other shape the length table gives, walked in a row: a wrong
     length here does not refuse the tape, it reads the rest as another tape. */
  const uint8_t more[] = {TZX_HEAD, 0x5A, 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 0x01, 0x0D, 0x21,
                          0x03, 'o', 'n', 'e', 0x31, 0x05, 0x04, 'h', 'e', 'r', 'e', 0x33, 0x01,
                          0x00, 0x01, 0x00, 0x35, 'a', 'r', 'c', 'h', 'i', 'v', 'e', 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0x02, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0x22,
                          /* the two a CDT must ignore rather than refuse */
                          0x34, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x01, 0x02, 0x00, 0x00, 0xAA, 0xBB,
                          0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(more, sizeof more);
  TEST_EQUAL(next(), 1000);
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);
}

/* A tape whose blocks would be visited out of order, or whose waveform is
   recorded sample by sample, is refused rather than played wrongly. */
static void a_block_this_reader_cannot_play_is_refused(void) {
  const uint8_t jump[] = {TZX_HEAD, 0x23, 0x02, 0x00};
  TEST_CHECK(refusal_of(jump, sizeof jump) != NULL);
  const uint8_t direct[] = {TZX_HEAD, 0x15, 0x4E, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0xFF};
  TEST_CHECK(refusal_of(direct, sizeof direct) != NULL);
  const uint8_t unknown[] = {TZX_HEAD, 0x7F, 0x00};
  TEST_CHECK(refusal_of(unknown, sizeof unknown) != NULL);
  const uint8_t runs_off[] = {TZX_HEAD, 0x12, 0xE8};
  TEST_CHECK(refusal_of(runs_off, sizeof runs_off) != NULL);
}

/* A pulse holds the level and then turns it over, and the tape starts low —
   so the first pulse is low and every one after it sets an edge. */
static void the_level_starts_low_and_turns_over(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x03, 0x00};
  open_image(image, sizeof image);
  TEST_CHECK(!next_level());
  TEST_CHECK(next_level());
  TEST_CHECK(!next_level());

  const uint8_t set_high[] = {TZX_HEAD, 0x2B, 0x01, 0x00, 0x00, 0x00,
                              0x01,     0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(set_high, sizeof set_high);
  TEST_CHECK(next_level()); /* a block may say the level outright */

  /* And one that declares no body says no level: there is no byte to read,
     and the last block in a file is where reading one anyway would be found
     out. */
  const uint8_t set_low[] = {TZX_HEAD, 0x2B, 0x01, 0x00, 0x00, 0x00, 0x01, 0x2B, 0x01,
                             0x00,     0x00, 0x00, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(set_low, sizeof set_low);
  TEST_CHECK(!next_level()); /* set high, then low again, and the pulse is low */

  const uint8_t says_nothing[] = {TZX_HEAD, 0x2B, 0x00, 0x00, 0x00, 0x00};
  open_image(says_nothing, sizeof says_nothing);
  TEST_EQUAL(next(), 0);
  TEST_CHECK(!reader.level);
}

/* A pause leaves the level low, so the pulse after it makes no edge — the
   one place the format has two pulses of one level running on. */
static void a_pause_leaves_the_level_low(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x20,
                           0x64,     0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(image, sizeof image);
  TEST_CHECK(!next_level());
  the_pause_reads(100);
  TEST_CHECK(!next_level());
}

/* Every timing in the format is a Spectrum's, so a machine with another
   clock holds each pulse for proportionally longer. */
static void another_clock_holds_each_pulse_for_longer(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0x78, 0x08, 0x02, 0x00};
  open_image(image, sizeof image);
  TEST_EQUAL(next(), 2168);
  open_image_for(image, sizeof image, 4000, TZX_AMSTRAD);
  TEST_EQUAL(next(), 2477);
  TEST_EQUAL(next(), 2477);

  /* A slower board holds each for less, and a pulse short enough to come to
     nothing at that clock is passed over like any other element the block has
     not got — all 65535 of this pilot's, which counted out one at a time
     would outrun the walk and end the tape where it stands. */
  const uint8_t slow[] = {TZX_HEAD, 0x11, 0x01, 0x00, 0x9B, 0x02, 0xDF, 0x02, 0x57, 0x03, 0xAE,
                          0x06,     0xFF, 0xFF, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80};
  open_image_for(slow, sizeof slow, 1000, TZX_SPECTRUM);
  TEST_EQUAL(next(), 190); /* 667 T-states at a thousand ticks a millisecond */
  TEST_EQUAL(next(), 210); /* and 735 */
  TEST_EQUAL(next(), 488); /* then &80's top bit, 1710 */

  /* A tone is counted the same way, and passed over the same way. */
  const uint8_t slow_tone[] = {TZX_HEAD, 0x12, 0x01, 0x00, 0xFF, 0xFF,
                               0x12,     0xE8, 0x03, 0x01, 0x00};
  open_image_for(slow_tone, sizeof slow_tone, 1000, TZX_SPECTRUM);
  TEST_EQUAL(next(), 285); /* 1000 T-states, and the tone in front came to none */
}

/* The walk is bounded, and the bound has to be as wide as the widest a block
   can be: sixteen passes for every byte of data, two for each of its bits,
   when neither bit cell has a length to play. A bound any narrower ends the
   tape early and says nothing about why. */
static void the_walk_reaches_the_end_of_the_widest_block(void) {
  static uint8_t image[2100];
  static const uint8_t head[] = {TZX_HEAD};
  const uint32_t data_length = 2000;
  memcpy(image, head, sizeof head);
  uint32_t at = sizeof head;
  image[at++] = 0x14;
  image[at++] = 0x00;
  image[at++] = 0x00; /* a zero bit cell of no length */
  image[at++] = 0x00;
  image[at++] = 0x00; /* and a one of the same */
  image[at++] = 0x08;
  image[at++] = 0x00;
  image[at++] = 0x00; /* no pause */
  image[at++] = (uint8_t)data_length;
  image[at++] = (uint8_t)(data_length >> 8);
  image[at++] = (uint8_t)(data_length >> 16);
  memset(image + at, 0xA5, data_length);
  at += data_length;
  const uint8_t tone[] = {0x12, 0xE8, 0x03, 0x01, 0x00};
  memcpy(image + at, tone, sizeof tone);
  at += (uint32_t)sizeof tone;

  open_image(image, at);
  TEST_EQUAL(next(), 1000); /* the tone behind two thousand silent bytes */
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);
}

/* The two blocks the CDT specification gives an Amstrad its own rules for. */
static void an_amstrad_reads_two_blocks_differently(void) {
  const uint8_t bare_pause[] = {TZX_HEAD, 0x20, 0x00, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(bare_pause, sizeof bare_pause);
  TEST_EQUAL(next(), 0); /* a pause of none stops a Spectrum */
  open_image_for(bare_pause, sizeof bare_pause, 4000, TZX_AMSTRAD);
  TEST_EQUAL(next(), 1142); /* and is no pause at all on a CPC */

  const uint8_t stop_48k[] = {TZX_HEAD, 0x2A, 0, 0, 0, 0, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(stop_48k, sizeof stop_48k);
  TEST_EQUAL(next(), 0);
  open_image_for(stop_48k, sizeof stop_48k, 4000, TZX_AMSTRAD);
  TEST_EQUAL(next(), 1142);

  /* Both were added after the format's version 1.10, so both are stepped over
     by the length they carry rather than by the one their row gives. */
  const uint8_t padded[] = {TZX_HEAD, 0x2A, 2, 0, 0, 0, 0xDE, 0xAD, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image_for(padded, sizeof padded, 4000, TZX_AMSTRAD);
  TEST_EQUAL(next(), 1142);
}

/* A .tap has no signature, and a real one is longer than a .tzx header, so
   length alone cannot tell them apart. */
static void a_tap_as_long_as_a_header_is_still_a_tap(void) {
  const uint8_t tap[] = {0x0C, 0x00, 0xFF, 'Z',  'X',  'T',  'a',
                         'p',  'e',  '!',  0x1A, 0x01, 0x14, 0x00};
  open_image(tap, sizeof tap);
  TEST_CHECK(!reader.has_block_ids);
  int pilots = 0;
  while (next() == 2168 && pilots < MOST_PILOT_PULSES) {
    pilots++;
  }
  TEST_EQUAL(pilots, 3223); /* flag &FF earns a data block's pilot */

  /* A standard block asking for no pause runs straight into the next, as the
     block with no pilot does. */
  const uint8_t no_pause[] = {TZX_HEAD, 0x10, 0x00, 0x00, 0x01, 0x00,
                              0xFF,     0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(no_pause, sizeof no_pause);
  int pulses = 0;
  /* the pilot, the syncs and the byte, none of them a thousand */
  while (next() != 1000 && pulses < MOST_PILOT_PULSES) {
    pulses++;
  }
  TEST_CHECK(pulses < MOST_PILOT_PULSES);

  /* And it is a Spectrum's, so an Amstrad is not handed one: replaying the
     ROM's timings at 4MHz gives a tape its firmware could never read. */
  const char *problem = NULL;
  TEST_CHECK(!tzx_open(&reader, tap, sizeof tap, 4000, TZX_AMSTRAD, &problem));
  TEST_CHECK(problem != NULL && strstr(problem, ".tap") != NULL);
}

/* A block the reader knows, cut short by the end of the file, is a truncated
   file rather than an unknown block — and the two guards that say so are the
   only thing standing between a hostile image and a read past its last byte. */
static void a_block_cut_short_is_told_from_one_this_reader_does_not_know(void) {
  const uint8_t turbo_cut[] = {TZX_HEAD, 0x11, 0x78, 0x08};
  TEST_CHECK(refused_for(turbo_cut, sizeof turbo_cut, "runs off the end"));
  const uint8_t pure_data_cut[] = {TZX_HEAD, 0x14, 0x57, 0x03};
  TEST_CHECK(refused_for(pure_data_cut, sizeof pure_data_cut, "runs off the end"));
  const uint8_t custom_cut[] = {TZX_HEAD, 0x35, 'w', 'h', 'o'};
  TEST_CHECK(refused_for(custom_cut, sizeof custom_cut, "runs off the end"));
  /* And one cut off exactly where its four-byte length would have begun. */
  const uint8_t custom_no_length[] = {TZX_HEAD, 0x35, 'i', 'd', 0, 0, 0, 0, 0,
                                      0,        0,    0,   0,   0, 0, 0, 0, 0};
  TEST_CHECK(refused_for(custom_no_length, sizeof custom_no_length, "runs off the end"));

  /* And a block whose own declared length reaches past the last byte. */
  const uint8_t standard_over[] = {TZX_HEAD, 0x10, 0x00, 0x00, 0x04, 0x00, 0xAA};
  TEST_CHECK(refused_for(standard_over, sizeof standard_over, "runs off the end"));

  /* Including one that would only reach past it after wrapping: the custom
     block is the one the format gives a 32-bit length. */
  const uint8_t custom_over[] = {TZX_HEAD, 0x35, 'i',  'd',  0,    0,    0,    0,   0,
                                 0,        0,    0,    0,    0,    0,    0,    0,   0,
                                 0xFF,     0xFF, 0xFF, 0xFF, 0xAA, 0xBB, 0xCC, 0xDD};
  TEST_CHECK(refused_for(custom_over, sizeof custom_over, "runs off the end"));

  const uint8_t unknown[] = {TZX_HEAD, 0x7F, 0x00};
  TEST_CHECK(refused_for(unknown, sizeof unknown, "does not know"));
}

/* A data block's last byte carries between one and eight bits. More than eight
   is a bit number this reader would shift by and never come back from. */
static void the_bits_in_a_last_byte_are_one_to_eight(void) {
  uint8_t turbo[] = {TZX_HEAD, 0x11, 0x78, 0x08, 0x9B, 0x02, 0xDF, 0x02, 0x57, 0x03, 0xAE,
                     0x06,     0x14, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0xAA};
  const size_t at_used_bits = 10 + 1 + 0x0C;
  TEST_CHECK(refusal_of(turbo, sizeof turbo) == NULL);
  turbo[at_used_bits] = 9;
  TEST_CHECK(refusal_of(turbo, sizeof turbo) != NULL);
  turbo[at_used_bits] = 0;
  TEST_CHECK(refusal_of(turbo, sizeof turbo) != NULL);

  /* A block declaring no data at all has no last byte, and its field says
     nothing about one. */
  const uint8_t empty[] = {TZX_HEAD, 0x11, 0x78, 0x08, 0x9B, 0x02, 0xDF, 0x02, 0x57, 0x03,
                           0xAE,     0x06, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  TEST_CHECK(refusal_of(empty, sizeof empty) == NULL);

  /* And the block with no pilot in front of it says the same field. */
  uint8_t pure[] = {TZX_HEAD, 0x14, 0x57, 0x03, 0xAE, 0x06, 0x08,
                    0x00,     0x00, 0x01, 0x00, 0x00, 0x80};
  const size_t at_pure_bits = 10 + 1 + 0x04;
  TEST_CHECK(refusal_of(pure, sizeof pure) == NULL);
  pure[at_pure_bits] = 9;
  TEST_CHECK(refusal_of(pure, sizeof pure) != NULL);
  pure[at_pure_bits] = 0;
  TEST_CHECK(refusal_of(pure, sizeof pure) != NULL);
}

/* An element of no T-states is one the block has not got: it is passed over,
   the level does not turn over for it, and the tape plays on. Rippers leave
   these behind and distributed tapes carry them — a tape refused for one is a
   game that cannot be loaded at all. */
static void an_element_of_no_length_is_passed_over(void) {
  /* A tone whose pulses are no length is no tone, and the block behind it
     still plays. */
  const uint8_t tone[] = {TZX_HEAD, 0x12, 0x00, 0x00, 0xFF, 0xFF, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(tone, sizeof tone);
  TEST_EQUAL(next(), 1000);
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);

  /* And a pilot of the same, which is counted the same way. */
  const uint8_t no_pilot[] = {TZX_HEAD, 0x11, 0x00, 0x00, 0x9B, 0x02, 0xDF, 0x02, 0x57, 0x03, 0xAE,
                              0x06,     0xFF, 0xFF, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80};
  open_image(no_pilot, sizeof no_pilot);
  TEST_EQUAL(next(), 667); /* straight to the first sync */
  TEST_EQUAL(next(), 735);
  TEST_EQUAL(next(), 1710);

  /* One entry of a sequence, passed over without an edge: the pulse behind it
     holds the level the pulse in front of it left. */
  const uint8_t sequence[] = {TZX_HEAD, 0x13, 0x03, 0xE8, 0x03, 0x00, 0x00, 0xD0, 0x07};
  open_image(sequence, sizeof sequence);
  tape_pulse_t first = {0, false};
  tape_pulse_t second = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &first));
  TEST_EQUAL(first.ticks, 1000);
  TEST_CHECK(!first.level);
  TEST_CHECK(tzx_next_pulse(&reader, &second));
  TEST_EQUAL(second.ticks, 2000);
  TEST_CHECK(second.level); /* turned over once, not twice */
  TEST_EQUAL(next(), 0);

  /* Both syncs at no length, which is the shape a homebrew loader ships in:
     the pilot runs and the data follows it with nothing in between. */
  const uint8_t no_syncs[] = {TZX_HEAD, 0x11, 0x78, 0x08, 0x00, 0x00, 0x00, 0x00, 0x57, 0x03, 0xAE,
                              0x06,     0x03, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x80};
  open_image(no_syncs, sizeof no_syncs);
  TEST_CHECK(!next_level());
  TEST_CHECK(next_level());
  TEST_CHECK(!next_level()); /* three pilot pulses, turning over each time */
  tape_pulse_t after_the_syncs = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &after_the_syncs));
  TEST_EQUAL(after_the_syncs.ticks, 1710); /* &80's top bit, straight after */
  TEST_CHECK(after_the_syncs.level);       /* and the two skipped syncs left no edge */

  /* Both bit lengths at no length, which is the shape a ripper's leftover
     tail ships in: the pilot and syncs play and the data is silent. */
  const uint8_t no_bits[] = {TZX_HEAD, 0x11, 0x78, 0x08, 0x9B, 0x02, 0xDF, 0x02, 0x00,
                             0x00,     0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x01,
                             0x00,     0x00, 0x80, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(no_bits, sizeof no_bits);
  TEST_EQUAL(next(), 2168);
  TEST_EQUAL(next(), 2168);
  TEST_EQUAL(next(), 2168);
  TEST_EQUAL(next(), 667);
  TEST_EQUAL(next(), 735);
  TEST_EQUAL(next(), 1000); /* and the block behind it, with no bits between */
}

/* A stop is not an end. A multiload marks the point between its parts, and a
   tape that could not be started again would never reach the second. */
static void a_stopped_tape_carries_on_when_it_is_played_again(void) {
  const uint8_t stop_48k[] = {TZX_HEAD, 0x2A, 0, 0, 0, 0, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(stop_48k, sizeof stop_48k);
  TEST_EQUAL(next(), 0);    /* the tape stops where the block says */
  TEST_EQUAL(next(), 1000); /* and plays on from there when it is played again */

  const uint8_t bare_pause[] = {TZX_HEAD, 0x20, 0x00, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(bare_pause, sizeof bare_pause);
  TEST_EQUAL(next(), 0);
  TEST_EQUAL(next(), 1000);

  /* The end of the tape is an end, and stays one. */
  const uint8_t tone[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(tone, sizeof tone);
  TEST_EQUAL(next(), 1000);
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);
  TEST_EQUAL(next(), 0);
}

/* The deck lets the line fall low when there is no next pulse, so a low last
   pulse would never see the edge that ends it, and a loader timing edges
   would never read its last bit. The millisecond a pause begins with is held
   here too: "to ensure that the last edge produced is properly finished".
   Some images of Speedlock 4 tapes — the first part of Freddy Hardest's among
   them — end on exactly this, a block asking for no pause and then a stop. */
static void a_stop_finishes_the_last_edge_first(void) {
  const uint8_t low_then_stop[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x20,
                                   0x00,     0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(low_then_stop, sizeof low_then_stop);
  TEST_CHECK(!next_level()); /* one pulse, low */
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0); /* and then the stop */
  /* Played again, it starts low: "An emulator should put the 'current pulse
     level' to 'low' when starting to play a TZX file, either from the start or
     from a certain position". */
  TEST_CHECK(!next_level());

  /* A high last pulse is ended by the deck's own fall, and the tape stops at
     once. */
  const uint8_t high_then_stop[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x02, 0x00, 0x20,
                                    0x00,     0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(high_then_stop, sizeof high_then_stop);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 0);
  TEST_EQUAL(next(), 1000);

  /* An Amstrad's tape ends the same way, at its own clock. */
  const uint8_t low_then_end[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image_for(low_then_end, sizeof low_then_end, 4000, TZX_AMSTRAD);
  TEST_EQUAL(next(), 1142);
  TEST_EQUAL(next(), 4000);
  TEST_EQUAL(next(), 0);
}

/* --- loops --- */

/* "This block is the same as the FOR statement in BASIC", and its end will
   "jump back to the start of the loop if it hasn't been run for the
   specified number of times". */
static void a_loop_plays_its_blocks_as_many_times_as_it_asks(void) {
  const uint8_t three[] = {TZX_HEAD, 0x24, 0x03, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x13,
                           0x01,     0xD0, 0x07, 0x25, 0x12, 0xB8, 0x0B, 0x01, 0x00};
  open_image(three, sizeof three);
  for (int pass = 0; pass < 3; pass++) {
    TEST_EQUAL(next(), 1000);
    TEST_EQUAL(next(), 2000);
  }
  TEST_EQUAL(next(), 3000); /* and the block behind the loop, once */
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);

  /* The format asks for more than one pass; one is played once. */
  const uint8_t one[] = {TZX_HEAD, 0x24, 0x01, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x25};
  open_image(one, sizeof one);
  TEST_EQUAL(next(), 1000);
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);
}

/* "For simplicity reasons don't nest loop blocks!", and a loop asks for a
   "Number of repetitions (greater than 1)". A loop the format gives no
   meaning is refused rather than given one here. */
static void a_loop_the_format_forbids_is_refused(void) {
  const uint8_t nested[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x24, 0x02, 0x00,
                            0x12,     0xE8, 0x03, 0x01, 0x00, 0x25, 0x25};
  TEST_CHECK(refused_for(nested, sizeof nested, "inside another"));
  const uint8_t no_passes[] = {TZX_HEAD, 0x24, 0x00, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x25};
  TEST_CHECK(refused_for(no_passes, sizeof no_passes, "no passes"));
  const uint8_t never_ended[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  TEST_CHECK(refused_for(never_ended, sizeof never_ended, "never ends"));
  const uint8_t never_begun[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x25};
  TEST_CHECK(refused_for(never_begun, sizeof never_begun, "never began"));
  const uint8_t cut[] = {TZX_HEAD, 0x24, 0x02};
  TEST_CHECK(refused_for(cut, sizeof cut, "runs off the end"));

  /* Two loops one after the other are two loops, not one inside the other. */
  const uint8_t in_a_row[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x25, 0x24, 0x02, 0x00, 0x25};
  TEST_CHECK(refusal_of(in_a_row, sizeof in_a_row) == NULL);

  /* And the blocks that send the tape elsewhere are still refused. */
  const uint8_t jump[] = {TZX_HEAD, 0x23, 0x02, 0x00};
  TEST_CHECK(refused_for(jump, sizeof jump, "elsewhere"));
}

/* A pass that gives the deck nothing would give it nothing every time, so the
   passes behind it are not walked. Walked, 65535 passes over a block of text
   would spend the walk's whole bound and end the tape where it stands, with
   the tone behind the loop never heard. */
static void a_loop_that_gives_nothing_is_passed_over(void) {
  const uint8_t silent[] = {TZX_HEAD, 0x24, 0xFF, 0xFF, 0x30, 0x04, 'n',  'o',
                            't',      'e',  0x25, 0x12, 0xE8, 0x03, 0x02, 0x00};
  open_image(silent, sizeof silent);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 0);

  /* Each loop is judged on its own passes, not on the pulse in front of it:
     counted as a pass that sounded, the first silent pass here would be walked
     again, and a body of two thousand silent bytes walked twice is more than
     the walk allows. */
  static uint8_t after_a_pulse[2100];
  static const uint8_t head[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x24, 0xFF, 0xFF};
  const uint32_t data_length = 2000;
  memcpy(after_a_pulse, head, sizeof head);
  uint32_t at = sizeof head;
  const uint8_t silent_data[] = {0x14,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x00,
                                 0x08,
                                 0x00,
                                 0x00,
                                 (uint8_t)data_length,
                                 (uint8_t)(data_length >> 8),
                                 0x00};
  memcpy(after_a_pulse + at, silent_data, sizeof silent_data);
  at += (uint32_t)sizeof silent_data;
  memset(after_a_pulse + at, 0xA5, data_length);
  at += data_length;
  const uint8_t end_and_tone[] = {0x25, 0x12, 0xB8, 0x0B, 0x02, 0x00};
  memcpy(after_a_pulse + at, end_and_tone, sizeof end_and_tone);
  at += (uint32_t)sizeof end_and_tone;
  open_image(after_a_pulse, at);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 3000); /* the tone behind the loop, with no stop before it */
  TEST_EQUAL(next(), 3000);
  the_last_edge_is_finished();
  TEST_EQUAL(next(), 0);
}

/* A stop inside a loop stands where it is, and the loop carries on from there
   when the tape is played again: the head has not moved. */
static void a_stop_inside_a_loop_carries_on_inside_it(void) {
  const uint8_t image[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x12, 0xE8, 0x03, 0x02, 0x00,
                           0x20,     0x00, 0x00, 0x25, 0x12, 0xB8, 0x0B, 0x02, 0x00};
  open_image(image, sizeof image);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 0); /* the first pass stops */
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 0); /* and so does the second */
  TEST_EQUAL(next(), 3000);
  TEST_EQUAL(next(), 3000);
  TEST_EQUAL(next(), 0);

  /* A pass that only stops has still given the deck something, and the next
     pass stops it again. */
  const uint8_t only_stops[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x20, 0x00, 0x00,
                                0x25,     0x12, 0xB8, 0x0B, 0x02, 0x00};
  open_image(only_stops, sizeof only_stops);
  TEST_EQUAL(next(), 0);
  TEST_EQUAL(next(), 0);
  TEST_EQUAL(next(), 3000);
}

/* The walk's bound holds across a loop's return: from the last pulse of one
   pass to the first of the next is the end of the pass and its start, never
   the same block twice. Here that is two thousand silent bytes between them. */
static void the_walk_reaches_round_a_loop_to_its_next_pulse(void) {
  static uint8_t image[2100];
  static const uint8_t head[] = {TZX_HEAD, 0x24, 0x02, 0x00};
  const uint32_t data_length = 2000;
  memcpy(image, head, sizeof head);
  uint32_t at = sizeof head;
  const uint8_t tone[] = {0x12, 0xE8, 0x03, 0x01, 0x00};
  memcpy(image + at, tone, sizeof tone);
  at += (uint32_t)sizeof tone;
  image[at++] = 0x14;
  image[at++] = 0x00;
  image[at++] = 0x00; /* a zero bit cell of no length */
  image[at++] = 0x00;
  image[at++] = 0x00; /* and a one of the same */
  image[at++] = 0x08;
  image[at++] = 0x00;
  image[at++] = 0x00; /* no pause */
  image[at++] = (uint8_t)data_length;
  image[at++] = (uint8_t)(data_length >> 8);
  image[at++] = (uint8_t)(data_length >> 16);
  memset(image + at, 0xA5, data_length);
  at += data_length;
  image[at++] = 0x25;

  open_image(image, at);
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000); /* the second pass, round the silent bytes */
  TEST_EQUAL(next(), 0);
}

/* The three loop images libspectrum keeps for the bugs its own reader once
   had, byte for byte. Two play here as they do there; the third is a loop end
   with no start, which libspectrum steps over and this reader refuses. */
static void libspectrums_loop_images(void) {
  /* loop.tzx, bug #113: a loop of two passes around nothing. */
  const uint8_t empty[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x25};
  open_image(empty, sizeof empty);
  TEST_EQUAL(next(), 0);

  /* loop2.tzx, bug #118: two passes of a tone of 185 pulses of 837 T-states. */
  const uint8_t tone[] = {TZX_HEAD, 0x24, 0x02, 0x00, 0x12, 0x45, 0x03, 0xB9, 0x00, 0x25};
  open_image(tone, sizeof tone);
  int pulses = 0;
  while (next() == 837 && pulses < MOST_PILOT_PULSES) {
    pulses++;
  }
  TEST_EQUAL(pulses, 370);

  /* loopend.tzx, bug #113. */
  const uint8_t end_alone[] = {TZX_HEAD, 0x25};
  TEST_CHECK(refused_for(end_alone, sizeof end_alone, "never began"));
}

/* The millisecond in front of a pause is there to finish the block's last
   edge, so a pause of exactly one millisecond is that millisecond. Spending it
   on silence instead would leave the edge with no time on its far side, and
   the loader would never see it. */
static void a_pause_of_one_millisecond_is_the_edge_it_finishes(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00, 0x20,
                           0x01,     0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(image, sizeof image);
  tape_pulse_t pulse = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &pulse));
  TEST_CHECK(!pulse.level);

  TEST_CHECK(tzx_next_pulse(&reader, &pulse));
  TEST_EQUAL(pulse.ticks, 1 * TICKS_PER_MILLISECOND);
  TEST_CHECK(pulse.level);

  TEST_CHECK(tzx_next_pulse(&reader, &pulse));
  TEST_EQUAL(pulse.ticks, 1000);
  TEST_CHECK(!pulse.level);
}

/* A line already low needs no such millisecond: the silence carries the same
   level on, and there is no edge to finish. */
static void a_pause_on_a_low_line_is_silence_and_nothing_else(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x02, 0x00, 0x20, 0x64, 0x00};
  open_image(image, sizeof image);
  TEST_CHECK(!next_level());
  TEST_CHECK(next_level()); /* two pulses, so the line is low again */

  tape_pulse_t silence = {0, false};
  TEST_CHECK(tzx_next_pulse(&reader, &silence));
  TEST_EQUAL(silence.ticks, 100 * TICKS_PER_MILLISECOND);
  TEST_CHECK(!silence.level);
}

/* A data block asking for no pause is followed by no pause: the tape runs
   straight on into the block behind it. */
static void a_data_block_asking_for_no_pause_runs_straight_on(void) {
  /* The level is set high first, so the block ends high and a pause that
     should not be there would show as the millisecond that finishes an edge. */
  const uint8_t image[] = {TZX_HEAD, 0x2B, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14,
                           0x57,     0x03, 0xAE, 0x06, 0x08, 0x00, 0x00, 0x01,
                           0x00,     0x00, 0x80, 0x12, 0xE8, 0x03, 0x01, 0x00};
  open_image(image, sizeof image);
  for (int bit = 0; bit < 8; bit++) {
    next();
    next();
  }
  TEST_EQUAL(next(), 1000); /* the tone behind it, with no silence between */
}

/* --- the cassette's leader tape --- */

/* The pulse the reader gives first, which a test of the leader reads itself
   rather than stepping over. */
static tape_pulse_t first_pulse(const uint8_t *image, uint32_t length, uint32_t clock,
                                tzx_machine machine) {
  tape_pulse_t pulse = {0, true};
  if (opened(image, length, clock, machine)) {
    TEST_CHECK(tzx_next_pulse(&reader, &pulse));
  }
  return pulse;
}

/* A tape that opens with sound has the whole leader in front of it. Its
   first pulse is low, as every first pulse is, and low before the first edge
   is silence whatever the image calls it — so the leader that comes first is
   that much shorter, and the first edge falls where the leader ends. */
static void the_leader_passes_before_a_tape_that_opens_with_sound(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x03, 0x00};
  const tape_pulse_t leader = first_pulse(image, sizeof image, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
  TEST_CHECK(!leader.level);
  TEST_EQUAL(leader.ticks, LEADER_MILLISECONDS * TICKS_PER_MILLISECOND - 1000);
  TEST_EQUAL(next(), 1000); /* and the tone behind it, all three pulses, as written */
  TEST_EQUAL(next(), 1000);
  TEST_EQUAL(next(), 1000);
}

/* Silence the image kept counts against the leader: what is left of the
   leader goes first, and the image's own silence after it as written. */
static void an_image_that_kept_some_silence_gets_the_rest(void) {
  const uint8_t image[] = {TZX_HEAD, 0x20, 0xE8, 0x03, 0x12, 0xE8, 0x03, 0x01, 0x00};
  const tape_pulse_t leader = first_pulse(image, sizeof image, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
  TEST_CHECK(!leader.level);
  TEST_EQUAL(leader.ticks, (LEADER_MILLISECONDS - 1000) * TICKS_PER_MILLISECOND - 1000);
  tape_pulse_t silence = {0, true};
  TEST_CHECK(tzx_next_pulse(&reader, &silence));
  TEST_CHECK(!silence.level);
  TEST_EQUAL(silence.ticks, 1000 * TICKS_PER_MILLISECOND);
  TEST_EQUAL(next(), 1000);
}

/* And an image that kept as much as the leader, or more, plays as it is. */
static void an_image_that_kept_its_silence_plays_as_it_is(void) {
  const uint32_t kept = LEADER_MILLISECONDS + 1000;
  const uint8_t image[] = {TZX_HEAD, 0x20, (uint8_t)kept, (uint8_t)(kept >> 8), 0x12, 0xE8,
                           0x03,     0x01, 0x00};
  const tape_pulse_t silence =
      first_pulse(image, sizeof image, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
  TEST_CHECK(!silence.level);
  TEST_EQUAL(silence.ticks, kept * TICKS_PER_MILLISECOND);
  TEST_EQUAL(next(), 1000);
}

/* The leader tape is a length of tape, so it lasts as long on any machine:
   its milliseconds are the machine's, like a pause's. */
static void the_leader_lasts_as_long_on_a_cpc(void) {
  const uint8_t image[] = {TZX_HEAD, 0x12, 0xE8, 0x03, 0x01, 0x00};
  const tape_pulse_t leader = first_pulse(image, sizeof image, 4000, TZX_AMSTRAD);
  TEST_CHECK(!leader.level);
  TEST_EQUAL(leader.ticks, LEADER_MILLISECONDS * 4000 - 1142); /* 1000 of the format's T-states */
  TEST_EQUAL(next(), 1142);
}

/* A .tap was a cassette too. */
static void a_tap_has_the_leader_in_front_of_it(void) {
  const uint8_t tap[] = {0x02, 0x00, 0x00, 0xAA};
  const tape_pulse_t leader = first_pulse(tap, sizeof tap, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
  TEST_CHECK(!leader.level);
  TEST_EQUAL(leader.ticks, LEADER_MILLISECONDS * TICKS_PER_MILLISECOND - 2168);
  TEST_EQUAL(next(), 2168); /* the pilot's first pulse, whole */
}

/* The leader tape passes once, at the start of the tape. A Spectrum's tape
   that stops before it has sounded at all still has it in front of the
   stop, and played on it does not have it again. */
static void the_leader_passes_once(void) {
  const uint8_t image[] = {TZX_HEAD, 0x20, 0x00, 0x00, 0x12, 0xE8, 0x03, 0x01, 0x00};
  const tape_pulse_t leader = first_pulse(image, sizeof image, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
  TEST_CHECK(!leader.level);
  TEST_EQUAL(leader.ticks, LEADER_MILLISECONDS * TICKS_PER_MILLISECOND);
  TEST_EQUAL(next(), 0);    /* the stop */
  TEST_EQUAL(next(), 1000); /* and played again, the tone, with nothing in front */

  /* A tape with no sound on it at all is a leader and then its end. */
  const uint8_t empty[] = {TZX_HEAD};
  const tape_pulse_t only = first_pulse(empty, sizeof empty, TICKS_PER_MILLISECOND, TZX_SPECTRUM);
  TEST_CHECK(!only.level);
  TEST_EQUAL(only.ticks, LEADER_MILLISECONDS * TICKS_PER_MILLISECOND);
  TEST_EQUAL(next(), 0);
}

int main(void) {
  TEST_RUN(a_tap_plays_at_the_roms_own_timings);
  TEST_RUN(the_pilot_turns_on_the_flag_bytes_top_bit);
  TEST_RUN(bytes_that_are_not_blocks_are_refused);
  TEST_RUN(a_tzx_is_known_by_its_signature);
  TEST_RUN(a_standard_blocks_pilot_turns_on_its_flag);
  TEST_RUN(a_turbo_block_says_its_own_timings);
  TEST_RUN(a_tone_is_one_length_repeated);
  TEST_RUN(a_sequence_gives_every_pulse_its_own_length);
  TEST_RUN(pure_data_is_bits_and_nothing_else);
  TEST_RUN(a_pause_block_is_silence_and_a_bare_one_stops_the_tape);
  TEST_RUN(a_block_that_only_describes_is_stepped_over);
  TEST_RUN(a_block_this_reader_cannot_play_is_refused);
  TEST_RUN(the_level_starts_low_and_turns_over);
  TEST_RUN(a_pause_leaves_the_level_low);
  TEST_RUN(another_clock_holds_each_pulse_for_longer);
  TEST_RUN(the_walk_reaches_the_end_of_the_widest_block);
  TEST_RUN(an_amstrad_reads_two_blocks_differently);
  TEST_RUN(a_tap_as_long_as_a_header_is_still_a_tap);
  TEST_RUN(a_block_cut_short_is_told_from_one_this_reader_does_not_know);
  TEST_RUN(the_bits_in_a_last_byte_are_one_to_eight);
  TEST_RUN(an_element_of_no_length_is_passed_over);
  TEST_RUN(a_stopped_tape_carries_on_when_it_is_played_again);
  TEST_RUN(a_stop_finishes_the_last_edge_first);
  TEST_RUN(a_loop_plays_its_blocks_as_many_times_as_it_asks);
  TEST_RUN(a_loop_the_format_forbids_is_refused);
  TEST_RUN(a_loop_that_gives_nothing_is_passed_over);
  TEST_RUN(a_stop_inside_a_loop_carries_on_inside_it);
  TEST_RUN(the_walk_reaches_round_a_loop_to_its_next_pulse);
  TEST_RUN(libspectrums_loop_images);
  TEST_RUN(a_pause_of_one_millisecond_is_the_edge_it_finishes);
  TEST_RUN(a_pause_on_a_low_line_is_silence_and_nothing_else);
  TEST_RUN(a_data_block_asking_for_no_pause_runs_straight_on);
  TEST_RUN(the_leader_passes_before_a_tape_that_opens_with_sound);
  TEST_RUN(an_image_that_kept_some_silence_gets_the_rest);
  TEST_RUN(an_image_that_kept_its_silence_plays_as_it_is);
  TEST_RUN(the_leader_lasts_as_long_on_a_cpc);
  TEST_RUN(a_tap_has_the_leader_in_front_of_it);
  TEST_RUN(the_leader_passes_once);
  return TEST_REPORT("tzx");
}
