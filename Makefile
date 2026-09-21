CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -g
BUILD = build

Z80_C = src/z80.c
MONITOR_C = src/monitor.c
KEYBOARD_C = src/keyboard.c
TAPE_C = src/tape.c
TZX_C = src/tzx.c
SHARED_C = $(Z80_C) $(MONITOR_C) $(KEYBOARD_C) $(TAPE_C) $(TZX_C)

CRTC_C = src/crtc.c
GATE_ARRAY_C = src/gate_array.c
PPI_C = src/ppi.c
PSG_C = src/psg.c
UPD765_C = src/upd765.c
DRIVE_C = src/drive.c
FLOPPY_C = src/floppy.c src/dsk.c
CPC_C = src/cpc.c
CPC_SNAPSHOT_C = src/cpc_snapshot.c
CPC_OWN_C = $(CRTC_C) $(GATE_ARRAY_C) $(PPI_C) $(PSG_C) $(UPD765_C) $(DRIVE_C) \
            $(FLOPPY_C) $(CPC_C) $(CPC_SNAPSHOT_C)
CPC_CORE_C = $(SHARED_C) $(CPC_OWN_C)

ULA_C = src/ula.c
SPECTRUM_C = src/spectrum.c
SPECTRUM_SNAPSHOT_C = src/spectrum_snapshot.c
SPECTRUM_OWN_C = $(ULA_C) $(SPECTRUM_C) $(SPECTRUM_SNAPSHOT_C)
SPECTRUM_CORE_C = $(SHARED_C) $(SPECTRUM_OWN_C)

# A host that offers a choice of machines links every one of them.
ALL_CORES_C = $(SHARED_C) $(CPC_OWN_C) $(SPECTRUM_OWN_C)

PNG_C = cli/png.c
CLI_C = cli/main.c

Z80_TEST_C = test/z80_test.c
CRTC_TEST_C = test/crtc_test.c
GATE_ARRAY_TEST_C = test/gate_array_test.c
MONITOR_TEST_C = test/monitor_test.c
PPI_TEST_C = test/ppi_test.c
PSG_TEST_C = test/psg_test.c
KEYBOARD_TEST_C = test/keyboard_test.c
TAPE_TEST_C = test/tape_test.c
TZX_TEST_C = test/tzx_test.c
ULA_TEST_C = test/ula_test.c
FLOPPY_TEST_C = test/floppy_test.c
DRIVE_TEST_C = test/drive_test.c
UPD765_TEST_C = test/upd765_test.c
PNG_TEST_C = test/png_test.c
CPC_TEST_C = test/cpc_test.c
CPC_SNAPSHOT_TEST_C = test/cpc_snapshot_test.c
CPC_TIMING_TEST_C = test/cpc_timing_test.c
CPC_FIRMWARE_TEST_C = test/cpc_firmware_test.c
SPECTRUM_TEST_C = test/spectrum_test.c
SPECTRUM_TIMING_TEST_C = test/spectrum_timing_test.c
SPECTRUM_INTERRUPT_TEST_C = test/spectrum_interrupt_test.c
SPECTRUM_SNAPSHOT_TEST_C = test/spectrum_snapshot_test.c
SPECTRUM_FIRMWARE_TEST_C = test/spectrum_firmware_test.c
SHAKER_TEST_C = test/shaker_test.c test/shaker_trace.c
Z80_SINGLE_STEP_C = test/z80_single_step_test.c test/json.c
Z80_EXERCISER_C = test/z80_exerciser_test.c

# Every target depends on every header: the build compiles straight from
# sources, with nothing finer to hang a dependency on.
HEADERS = $(wildcard src/*.h) $(wildcard cli/*.h) $(wildcard test/*.h)

# Taken by wildcard so a file cannot be added and left out of the formatter
# or the linter.
SOURCES = $(wildcard src/*.c) $(wildcard cli/*.c) $(wildcard test/*.c)

SINGLE_STEP_DATA = test/data/SingleStepTests/z80/v1
EXERCISER_DATA = test/data/ZEXALL
# Groups of the exerciser to run by default; 0 runs all 67, which takes a while.
EXERCISER_GROUPS ?= 12

# Which of Shaker's modules to walk, and which group of one. MODULE=E runs
# that module alone; MODULE=E GROUP=6 runs one group of it and keeps the
# beam path of every screen it draws, which is a megabyte apiece. A group
# needs the module it belongs to. SHAKER_TRACE=prefix in the environment
# writes what each group does as it does it; test/shaker_trace.h has the rest.
MODULE ?=
GROUP ?=

# Which CRTC the machine is built with, which decides both what Shaker runs
# and the record it is set against. Only type 0 behaves as itself here;
# CRTC=1 builds a machine a program names a type 1 and runs the groups that
# belong to one. The chip answers to all five, and two have a record.
CRTC ?= 0
ifeq ($(filter $(CRTC),0 1 2 3 4),)
$(error CRTC=$(CRTC) names no CRTC; the types are 0 to 4)
endif

CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || echo xcrun clang-format)
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || echo clang-tidy)

all: $(BUILD)/emulator $(BUILD)/z80_test $(BUILD)/tape_test $(BUILD)/tzx_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/ula_test $(BUILD)/spectrum_test $(BUILD)/spectrum_snapshot_test $(BUILD)/spectrum_timing_test $(BUILD)/spectrum_interrupt_test $(BUILD)/cpc_test $(BUILD)/cpc_timing_test $(BUILD)/cpc_snapshot_test $(BUILD)/floppy_test $(BUILD)/drive_test $(BUILD)/upd765_test $(BUILD)/png_test $(BUILD)/z80_single_step_test $(BUILD)/z80_exerciser_test $(BUILD)/cpc_firmware_test $(BUILD)/spectrum_firmware_test $(BUILD)/shaker_test

# The command line. The core allocates nothing and does no I/O; everything
# that does lives in cli/.
$(BUILD)/emulator: $(ALL_CORES_C) $(PNG_C) $(CLI_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Icli $(ALL_CORES_C) $(PNG_C) $(CLI_C) -o $@

$(BUILD)/z80_test: $(Z80_C) $(Z80_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(Z80_C) $(Z80_TEST_C) -o $@

$(BUILD)/crtc_test: $(CRTC_C) $(CRTC_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CRTC_C) $(CRTC_TEST_C) -o $@

$(BUILD)/gate_array_test: $(GATE_ARRAY_C) $(GATE_ARRAY_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(GATE_ARRAY_C) $(GATE_ARRAY_TEST_C) -o $@

$(BUILD)/monitor_test: $(MONITOR_C) $(MONITOR_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(MONITOR_C) $(MONITOR_TEST_C) -o $@

$(BUILD)/ppi_test: $(PPI_C) $(PPI_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(PPI_C) $(PPI_TEST_C) -o $@

$(BUILD)/psg_test: $(PSG_C) $(PSG_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(PSG_C) $(PSG_TEST_C) -o $@

$(BUILD)/keyboard_test: $(KEYBOARD_C) $(KEYBOARD_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(KEYBOARD_C) $(KEYBOARD_TEST_C) -o $@

$(BUILD)/tape_test: $(TAPE_C) $(TAPE_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(TAPE_C) $(TAPE_TEST_C) -o $@

$(BUILD)/tzx_test: $(TZX_C) $(TZX_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(TZX_C) $(TZX_TEST_C) -o $@

$(BUILD)/shaker_test: $(CPC_CORE_C) $(PNG_C) $(SHAKER_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Icli -Itest $(CPC_CORE_C) $(PNG_C) $(SHAKER_TEST_C) -o $@

$(BUILD)/ula_test: $(ULA_C) $(ULA_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(ULA_C) $(ULA_TEST_C) -o $@

$(BUILD)/spectrum_test: $(SPECTRUM_CORE_C) $(SPECTRUM_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_TEST_C) -o $@

$(BUILD)/spectrum_snapshot_test: $(SPECTRUM_CORE_C) $(SPECTRUM_SNAPSHOT_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_SNAPSHOT_TEST_C) -o $@

$(BUILD)/cpc_test: $(CPC_CORE_C) $(CPC_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CPC_CORE_C) $(CPC_TEST_C) -o $@

$(BUILD)/cpc_firmware_test: $(CPC_CORE_C) $(CPC_FIRMWARE_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CPC_CORE_C) $(CPC_FIRMWARE_TEST_C) -o $@

$(BUILD)/spectrum_firmware_test: $(SPECTRUM_CORE_C) $(SPECTRUM_FIRMWARE_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_FIRMWARE_TEST_C) -o $@

$(BUILD)/spectrum_interrupt_test: $(SPECTRUM_CORE_C) $(SPECTRUM_INTERRUPT_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_INTERRUPT_TEST_C) -o $@

$(BUILD)/spectrum_timing_test: $(SPECTRUM_CORE_C) $(SPECTRUM_TIMING_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_TIMING_TEST_C) -o $@

$(BUILD)/cpc_timing_test: $(CPC_CORE_C) $(CPC_TIMING_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CPC_CORE_C) $(CPC_TIMING_TEST_C) -o $@

$(BUILD)/cpc_snapshot_test: $(CPC_CORE_C) $(CPC_SNAPSHOT_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CPC_CORE_C) $(CPC_SNAPSHOT_TEST_C) -o $@

$(BUILD)/floppy_test: $(FLOPPY_C) $(FLOPPY_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(FLOPPY_C) $(FLOPPY_TEST_C) -o $@

$(BUILD)/drive_test: $(FLOPPY_C) $(DRIVE_C) $(DRIVE_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(FLOPPY_C) $(DRIVE_C) $(DRIVE_TEST_C) -o $@

$(BUILD)/upd765_test: $(FLOPPY_C) $(DRIVE_C) $(UPD765_C) $(UPD765_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(FLOPPY_C) $(DRIVE_C) $(UPD765_C) $(UPD765_TEST_C) -o $@

$(BUILD)/png_test: $(PNG_C) $(PNG_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icli -Itest $(PNG_C) $(PNG_TEST_C) -o $@

$(BUILD)/z80_single_step_test: $(Z80_C) $(Z80_SINGLE_STEP_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(Z80_C) $(Z80_SINGLE_STEP_C) -o $@

$(BUILD)/z80_exerciser_test: $(Z80_C) $(Z80_EXERCISER_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(Z80_C) $(Z80_EXERCISER_C) -o $@

# The fast tier: hermetic, no network, runs on every change.
test: sources-agree $(BUILD)/z80_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/tape_test $(BUILD)/tzx_test $(BUILD)/ula_test $(BUILD)/spectrum_test $(BUILD)/spectrum_snapshot_test $(BUILD)/spectrum_timing_test $(BUILD)/spectrum_interrupt_test $(BUILD)/cpc_test $(BUILD)/cpc_timing_test $(BUILD)/cpc_snapshot_test $(BUILD)/floppy_test $(BUILD)/drive_test $(BUILD)/upd765_test $(BUILD)/png_test
	@$(BUILD)/z80_test
	@$(BUILD)/crtc_test
	@$(BUILD)/gate_array_test
	@$(BUILD)/monitor_test
	@$(BUILD)/ppi_test
	@$(BUILD)/psg_test
	@$(BUILD)/keyboard_test
	@$(BUILD)/tape_test
	@$(BUILD)/tzx_test
	@$(BUILD)/ula_test
	@$(BUILD)/spectrum_test
	@$(BUILD)/spectrum_snapshot_test
	@$(BUILD)/spectrum_timing_test
	@$(BUILD)/spectrum_interrupt_test
	@$(BUILD)/cpc_test
	@$(BUILD)/cpc_timing_test
	@$(BUILD)/cpc_snapshot_test
	@$(BUILD)/floppy_test
	@$(BUILD)/drive_test
	@$(BUILD)/upd765_test
	@$(BUILD)/png_test

# Every source the build names, against every source there is. The build's
# lists are hand-written and the formatter's are a wildcard, so without this a
# new file is formatted and linted and built into nothing, silently.
BUILT_C = $(ALL_CORES_C) $(PNG_C) $(CLI_C) \
          $(Z80_TEST_C) $(CRTC_TEST_C) $(GATE_ARRAY_TEST_C) $(MONITOR_TEST_C) $(PPI_TEST_C) \
          $(PSG_TEST_C) $(KEYBOARD_TEST_C) $(TAPE_TEST_C) $(TZX_TEST_C) $(ULA_TEST_C) \
          $(FLOPPY_TEST_C) $(DRIVE_TEST_C) $(UPD765_TEST_C) $(PNG_TEST_C) $(CPC_TEST_C) \
          $(CPC_SNAPSHOT_TEST_C) $(CPC_TIMING_TEST_C) $(CPC_FIRMWARE_TEST_C) \
          $(SPECTRUM_TEST_C) $(SPECTRUM_SNAPSHOT_TEST_C) $(SPECTRUM_TIMING_TEST_C) \
          $(SPECTRUM_INTERRUPT_TEST_C) \
          $(SPECTRUM_FIRMWARE_TEST_C) $(SHAKER_TEST_C) \
          $(Z80_SINGLE_STEP_C) $(Z80_EXERCISER_C)

sources-agree:
	@mkdir -p $(BUILD)
	@printf '%s\n' $(BUILT_C) | sort -u > $(BUILD)/.built
	@printf '%s\n' $(SOURCES) | sort -u > $(BUILD)/.present
	@diff $(BUILD)/.built $(BUILD)/.present || \
	  { echo "the build's lists and the sources on disk disagree"; exit 1; }

# The fast tier again with the sanitizers on. A read past the end of an image
# is the kind of fault a passing test cannot see, so the guards against one
# are graded here or nowhere.
SANITIZERS = -fsanitize=address,undefined -fno-sanitize-recover=all

test-sanitized:
	@$(MAKE) --no-print-directory test BUILD=$(BUILD)/sanitized \
	  CFLAGS="$(filter-out -O2,$(CFLAGS)) -O1 $(SANITIZERS)"

# The firmware images, fetched and pinned by hash. Needed to run a machine,
# not to build one or to test the parts.
roms:
	@sh tools/fetch-roms.sh

# The disc images the machine tier reads, fetched and pinned by hash like
# the firmware.
discs:
	@sh tools/fetch-discs.sh

# The machine tier: the real firmware, booted and typed at, and a real disc
# catalogued. Needs the ROM and disc images, so it fetches them first.
test-firmware: $(BUILD)/cpc_firmware_test $(BUILD)/spectrum_firmware_test
	@sh tools/fetch-roms.sh
	@sh tools/fetch-discs.sh
	@$(BUILD)/cpc_firmware_test roms test/data/discs
	@$(BUILD)/spectrum_firmware_test roms

# Shaker: Longshot's CRTC acid tests, walked module by module, and what they
# said set against the copy on record in test/. Passing means nothing moved,
# and not that the machine is right: most groups state their verdict in a
# picture, and a group is graded only once its convention has been read off
# its own output.
test-shaker: $(BUILD)/shaker_test
	@sh tools/fetch-roms.sh
	@sh tools/fetch-discs.sh
	@mkdir -p $(BUILD)/shaker/crtc$(CRTC)
	@$(BUILD)/shaker_test roms test/data/discs $(BUILD)/shaker/crtc$(CRTC) test/shaker-scoreboard-crtc$(CRTC).txt "$(MODULE)" "$(GROUP)" "$(CRTC)"

# The conformance tier: the complete SingleStepTests corpus, fetched on first
# use. Run it before committing anything that touches the CPU.
test-single-step: $(BUILD)/z80_single_step_test
	@sh tools/fetch-tests.sh
	@$(BUILD)/z80_single_step_test $(SINGLE_STEP_DATA)/*.json

# The acceptance tier: a real program exercising the CPU for hours of its own
# time. EXERCISER_GROUPS=0 runs every group.
test-exerciser: $(BUILD)/z80_exerciser_test
	@sh tools/fetch-exerciser.sh
	@$(BUILD)/z80_exerciser_test $(EXERCISER_DATA)/zexdoc.com $(EXERCISER_GROUPS)
	@$(BUILD)/z80_exerciser_test $(EXERCISER_DATA)/zexall.com $(EXERCISER_GROUPS)

# Both CRTCs, whatever the command line asked for: the point of the tier is
# every record, and a type named here would silently grade one of them twice.
test-all: override CRTC := 0
test-all: test test-sanitized test-firmware test-shaker test-single-step test-exerciser
	@$(MAKE) --no-print-directory test-shaker CRTC=1

format:
	$(CLANG_FORMAT) -i $(SOURCES) $(HEADERS)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES) $(HEADERS)

lint:
	$(CLANG_TIDY) $(SOURCES) -- $(CFLAGS) -Isrc -Icli -Itest

clean:
	rm -rf $(BUILD)

.PHONY: all roms discs sources-agree test test-sanitized test-firmware test-shaker test-single-step test-exerciser test-all format format-check lint clean
