CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -g
BUILD = build

Z80_C = src/z80.c
MONITOR_C = src/monitor.c
KEYBOARD_C = src/keyboard.c
TAPE_C = src/tape.c
SHARED_C = $(Z80_C) $(MONITOR_C) $(KEYBOARD_C) $(TAPE_C)

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
SPECTRUM_SNAPSHOT_TEST_C = test/spectrum_snapshot_test.c
SPECTRUM_FIRMWARE_TEST_C = test/spectrum_firmware_test.c
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

CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || echo xcrun clang-format)
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || echo clang-tidy)

all: $(BUILD)/emulator $(BUILD)/z80_test $(BUILD)/tape_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/ula_test $(BUILD)/spectrum_test $(BUILD)/spectrum_snapshot_test $(BUILD)/cpc_test $(BUILD)/cpc_timing_test $(BUILD)/cpc_snapshot_test $(BUILD)/floppy_test $(BUILD)/drive_test $(BUILD)/upd765_test $(BUILD)/png_test $(BUILD)/z80_single_step_test $(BUILD)/z80_exerciser_test

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
test: $(BUILD)/z80_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/tape_test $(BUILD)/ula_test $(BUILD)/spectrum_test $(BUILD)/spectrum_snapshot_test $(BUILD)/cpc_test $(BUILD)/cpc_timing_test $(BUILD)/cpc_snapshot_test $(BUILD)/floppy_test $(BUILD)/drive_test $(BUILD)/upd765_test $(BUILD)/png_test
	@$(BUILD)/z80_test
	@$(BUILD)/crtc_test
	@$(BUILD)/gate_array_test
	@$(BUILD)/monitor_test
	@$(BUILD)/ppi_test
	@$(BUILD)/psg_test
	@$(BUILD)/keyboard_test
	@$(BUILD)/tape_test
	@$(BUILD)/ula_test
	@$(BUILD)/spectrum_test
	@$(BUILD)/spectrum_snapshot_test
	@$(BUILD)/cpc_test
	@$(BUILD)/cpc_timing_test
	@$(BUILD)/cpc_snapshot_test
	@$(BUILD)/floppy_test
	@$(BUILD)/drive_test
	@$(BUILD)/upd765_test
	@$(BUILD)/png_test

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

test-all: test test-firmware test-single-step test-exerciser

format:
	$(CLANG_FORMAT) -i $(SOURCES) $(HEADERS)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES) $(HEADERS)

lint:
	$(CLANG_TIDY) $(SOURCES) -- $(CFLAGS) -Isrc -Icli -Itest

clean:
	rm -rf $(BUILD)

.PHONY: all roms discs test test-firmware test-single-step test-exerciser test-all format format-check lint clean
