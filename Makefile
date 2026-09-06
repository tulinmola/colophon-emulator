CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -g
BUILD = build

SRC_C = src/z80.c
CRTC_C = src/crtc.c
GATE_ARRAY_C = src/gate_array.c
MONITOR_C = src/monitor.c
PPI_C = src/ppi.c
PSG_C = src/psg.c
KEYBOARD_C = src/keyboard.c
ULA_C = src/ula.c
SPECTRUM_C = src/spectrum.c
SPECTRUM_SNAPSHOT_C = src/spectrum_snapshot.c
CPC_SNAPSHOT_C = src/cpc_snapshot.c
FLOPPY_C = src/floppy.c src/dsk.c
DRIVE_C = src/drive.c
UPD765_C = src/upd765.c
MACHINE_C = src/cpc.c
Z80_TEST_C = test/z80_test.c
CRTC_TEST_C = test/crtc_test.c
GATE_ARRAY_TEST_C = test/gate_array_test.c
MONITOR_TEST_C = test/monitor_test.c
PPI_TEST_C = test/ppi_test.c
PSG_TEST_C = test/psg_test.c
KEYBOARD_TEST_C = test/keyboard_test.c
ULA_TEST_C = test/ula_test.c
SPECTRUM_TEST_C = test/spectrum_test.c
SPECTRUM_SNAPSHOT_TEST_C = test/spectrum_snapshot_test.c
CPC_TEST_C = test/cpc_test.c
PNG_TEST_C = test/png_test.c
TIMING_TEST_C = test/timing_test.c
CPC_SNAPSHOT_TEST_C = test/cpc_snapshot_test.c
FLOPPY_TEST_C = test/floppy_test.c
DRIVE_TEST_C = test/drive_test.c
UPD765_TEST_C = test/upd765_test.c
FIRMWARE_TEST_C = test/firmware_test.c
SPECTRUM_FIRMWARE_TEST_C = test/spectrum_firmware_test.c
SINGLE_STEP_C = test/z80_single_step_test.c test/json.c
EXERCISER_C = test/z80_exerciser_test.c
CORE_C = $(SRC_C) $(CRTC_C) $(GATE_ARRAY_C) $(MONITOR_C) $(PPI_C) $(PSG_C) $(KEYBOARD_C) $(FLOPPY_C) $(DRIVE_C) $(UPD765_C) $(MACHINE_C) $(CPC_SNAPSHOT_C)
# Every target depends on every header. The build compiles straight from
# sources with nothing finer to hang a dependency on, and a header naming
# only its own target is not what a translation unit reads: a constant in
# keyboard.h governs whether a CPC has a Q key, and a stale object file
# hid that from a test that went looking for it.
HEADERS = $(wildcard src/*.h) $(wildcard cli/*.h) $(wildcard test/*.h)

SPECTRUM_CORE_C = $(SRC_C) $(MONITOR_C) $(KEYBOARD_C) $(ULA_C) $(SPECTRUM_C) $(SPECTRUM_SNAPSHOT_C)
PNG_C = cli/png.c
CLI_C = cli/main.c
SRC_ALL = $(CORE_C) $(ULA_C) $(SPECTRUM_C) $(SPECTRUM_SNAPSHOT_C) src/z80.h src/crtc.h src/gate_array.h src/monitor.h src/ppi.h src/psg.h src/keyboard.h src/ula.h src/spectrum.h src/spectrum_snapshot.h src/cpc.h src/cpc_snapshot.h src/floppy.h src/dsk.h src/drive.h src/upd765.h $(PNG_C) $(CLI_C) cli/png.h $(Z80_TEST_C) $(CRTC_TEST_C) $(GATE_ARRAY_TEST_C) $(MONITOR_TEST_C) $(PPI_TEST_C) $(PSG_TEST_C) $(KEYBOARD_TEST_C) $(ULA_TEST_C) $(SPECTRUM_TEST_C) $(SPECTRUM_SNAPSHOT_TEST_C) $(CPC_TEST_C) $(TIMING_TEST_C) $(CPC_SNAPSHOT_TEST_C) $(FLOPPY_TEST_C) $(DRIVE_TEST_C) $(UPD765_TEST_C) $(PNG_TEST_C) $(FIRMWARE_TEST_C) $(SPECTRUM_FIRMWARE_TEST_C) $(SINGLE_STEP_C) $(EXERCISER_C) test/json.h test/test.h

SINGLE_STEP_DATA = test/data/SingleStepTests/z80/v1
EXERCISER_DATA = test/data/ZEXALL
# Groups of the exerciser to run by default; 0 runs all 67, which takes a while.
EXERCISER_GROUPS ?= 12

CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || echo xcrun clang-format)
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || echo clang-tidy)

all: $(BUILD)/emulator $(BUILD)/z80_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/ula_test $(BUILD)/spectrum_test $(BUILD)/spectrum_snapshot_test $(BUILD)/cpc_test $(BUILD)/timing_test $(BUILD)/cpc_snapshot_test $(BUILD)/floppy_test $(BUILD)/drive_test $(BUILD)/upd765_test $(BUILD)/png_test $(BUILD)/z80_single_step_test $(BUILD)/z80_exerciser_test

# The command line. The core allocates nothing and does no I/O; everything
# that does lives in cli/.
$(BUILD)/emulator: $(CORE_C) $(ULA_C) $(SPECTRUM_C) $(SPECTRUM_SNAPSHOT_C) $(PNG_C) $(CLI_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Icli $(CORE_C) $(ULA_C) $(SPECTRUM_C) $(SPECTRUM_SNAPSHOT_C) $(PNG_C) $(CLI_C) -o $@

$(BUILD)/z80_test: $(SRC_C) $(Z80_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(Z80_TEST_C) -o $@

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

$(BUILD)/ula_test: $(ULA_C) $(ULA_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(ULA_C) $(ULA_TEST_C) -o $@

$(BUILD)/spectrum_test: $(SPECTRUM_CORE_C) $(SPECTRUM_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_TEST_C) -o $@

$(BUILD)/spectrum_snapshot_test: $(SPECTRUM_CORE_C) $(SPECTRUM_SNAPSHOT_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_SNAPSHOT_TEST_C) -o $@

$(BUILD)/cpc_test: $(CORE_C) $(CPC_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(CPC_TEST_C) -o $@

$(BUILD)/firmware_test: $(CORE_C) $(FIRMWARE_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(FIRMWARE_TEST_C) -o $@

$(BUILD)/spectrum_firmware_test: $(SPECTRUM_CORE_C) $(SPECTRUM_FIRMWARE_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SPECTRUM_CORE_C) $(SPECTRUM_FIRMWARE_TEST_C) -o $@

$(BUILD)/timing_test: $(CORE_C) $(TIMING_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(TIMING_TEST_C) -o $@

$(BUILD)/cpc_snapshot_test: $(CORE_C) $(CPC_SNAPSHOT_TEST_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(CPC_SNAPSHOT_TEST_C) -o $@

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

$(BUILD)/z80_single_step_test: $(SRC_C) $(SINGLE_STEP_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(SINGLE_STEP_C) -o $@

$(BUILD)/z80_exerciser_test: $(SRC_C) $(EXERCISER_C) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(EXERCISER_C) -o $@

# The fast tier: hermetic, no network, runs on every change.
test: $(BUILD)/z80_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/ula_test $(BUILD)/spectrum_test $(BUILD)/spectrum_snapshot_test $(BUILD)/cpc_test $(BUILD)/timing_test $(BUILD)/cpc_snapshot_test $(BUILD)/floppy_test $(BUILD)/drive_test $(BUILD)/upd765_test $(BUILD)/png_test
	@$(BUILD)/z80_test
	@$(BUILD)/crtc_test
	@$(BUILD)/gate_array_test
	@$(BUILD)/monitor_test
	@$(BUILD)/ppi_test
	@$(BUILD)/psg_test
	@$(BUILD)/keyboard_test
	@$(BUILD)/ula_test
	@$(BUILD)/spectrum_test
	@$(BUILD)/spectrum_snapshot_test
	@$(BUILD)/cpc_test
	@$(BUILD)/timing_test
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
test-firmware: $(BUILD)/firmware_test $(BUILD)/spectrum_firmware_test
	@sh tools/fetch-roms.sh
	@sh tools/fetch-discs.sh
	@$(BUILD)/firmware_test roms test/data/discs
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
	$(CLANG_FORMAT) -i $(SRC_ALL)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SRC_ALL)

lint:
	$(CLANG_TIDY) $(CORE_C) $(ULA_C) $(SPECTRUM_C) $(SPECTRUM_SNAPSHOT_C) $(PNG_C) $(CLI_C) $(Z80_TEST_C) $(CRTC_TEST_C) $(GATE_ARRAY_TEST_C) $(MONITOR_TEST_C) $(PPI_TEST_C) $(PSG_TEST_C) $(KEYBOARD_TEST_C) $(ULA_TEST_C) $(SPECTRUM_TEST_C) $(SPECTRUM_SNAPSHOT_TEST_C) $(CPC_TEST_C) $(TIMING_TEST_C) $(CPC_SNAPSHOT_TEST_C) $(FLOPPY_TEST_C) $(DRIVE_TEST_C) $(UPD765_TEST_C) $(PNG_TEST_C) $(FIRMWARE_TEST_C) $(SPECTRUM_FIRMWARE_TEST_C) $(SINGLE_STEP_C) $(EXERCISER_C) -- $(CFLAGS) -Isrc -Icli -Itest

clean:
	rm -rf $(BUILD)

.PHONY: all roms discs test test-firmware test-single-step test-exerciser test-all format format-check lint clean
