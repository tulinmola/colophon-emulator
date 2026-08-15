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
MACHINE_C = src/cpc.c
Z80_TEST_C = test/z80_test.c
CRTC_TEST_C = test/crtc_test.c
GATE_ARRAY_TEST_C = test/gate_array_test.c
MONITOR_TEST_C = test/monitor_test.c
PPI_TEST_C = test/ppi_test.c
PSG_TEST_C = test/psg_test.c
KEYBOARD_TEST_C = test/keyboard_test.c
CPC_TEST_C = test/cpc_test.c
PNG_TEST_C = test/png_test.c
TIMING_TEST_C = test/timing_test.c
FIRMWARE_TEST_C = test/firmware_test.c
SINGLE_STEP_C = test/z80_single_step_test.c test/json.c
EXERCISER_C = test/z80_exerciser_test.c
CORE_C = $(SRC_C) $(CRTC_C) $(GATE_ARRAY_C) $(MONITOR_C) $(PPI_C) $(PSG_C) $(KEYBOARD_C) $(MACHINE_C)
PNG_C = cli/png.c
CLI_C = cli/main.c
SRC_ALL = $(CORE_C) src/z80.h src/crtc.h src/gate_array.h src/monitor.h src/ppi.h src/psg.h src/keyboard.h src/cpc.h $(PNG_C) $(CLI_C) cli/png.h $(Z80_TEST_C) $(CRTC_TEST_C) $(GATE_ARRAY_TEST_C) $(MONITOR_TEST_C) $(PPI_TEST_C) $(PSG_TEST_C) $(KEYBOARD_TEST_C) $(CPC_TEST_C) $(TIMING_TEST_C) $(PNG_TEST_C) $(FIRMWARE_TEST_C) $(SINGLE_STEP_C) $(EXERCISER_C) test/json.h test/test.h

SINGLE_STEP_DATA = test/data/SingleStepTests/z80/v1
EXERCISER_DATA = test/data/ZEXALL
# Groups of the exerciser to run by default; 0 runs all 67, which takes a while.
EXERCISER_GROUPS ?= 12

CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || echo xcrun clang-format)
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || echo clang-tidy)

all: $(BUILD)/emulator $(BUILD)/z80_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/cpc_test $(BUILD)/timing_test $(BUILD)/png_test $(BUILD)/z80_single_step_test $(BUILD)/z80_exerciser_test

# The command line. The core allocates nothing and does no I/O; everything
# that does lives in cli/.
$(BUILD)/emulator: $(CORE_C) $(PNG_C) $(CLI_C) src/cpc.h cli/png.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Icli $(CORE_C) $(PNG_C) $(CLI_C) -o $@

$(BUILD)/z80_test: $(SRC_C) src/z80.h $(Z80_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(Z80_TEST_C) -o $@

$(BUILD)/crtc_test: $(CRTC_C) src/crtc.h $(CRTC_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CRTC_C) $(CRTC_TEST_C) -o $@

$(BUILD)/gate_array_test: $(GATE_ARRAY_C) src/gate_array.h $(GATE_ARRAY_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(GATE_ARRAY_C) $(GATE_ARRAY_TEST_C) -o $@

$(BUILD)/monitor_test: $(MONITOR_C) src/monitor.h $(MONITOR_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(MONITOR_C) $(MONITOR_TEST_C) -o $@

$(BUILD)/ppi_test: $(PPI_C) src/ppi.h $(PPI_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(PPI_C) $(PPI_TEST_C) -o $@

$(BUILD)/psg_test: $(PSG_C) src/psg.h $(PSG_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(PSG_C) $(PSG_TEST_C) -o $@

$(BUILD)/keyboard_test: $(KEYBOARD_C) src/keyboard.h $(KEYBOARD_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(KEYBOARD_C) $(KEYBOARD_TEST_C) -o $@

$(BUILD)/cpc_test: $(CORE_C) src/z80.h src/crtc.h src/gate_array.h src/monitor.h src/cpc.h $(CPC_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(CPC_TEST_C) -o $@

$(BUILD)/firmware_test: $(CORE_C) src/cpc.h $(FIRMWARE_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(FIRMWARE_TEST_C) -o $@

$(BUILD)/timing_test: $(CORE_C) src/cpc.h $(TIMING_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(CORE_C) $(TIMING_TEST_C) -o $@

$(BUILD)/png_test: $(PNG_C) cli/png.h $(PNG_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icli -Itest $(PNG_C) $(PNG_TEST_C) -o $@

$(BUILD)/z80_single_step_test: $(SRC_C) src/z80.h $(SINGLE_STEP_C) test/json.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(SINGLE_STEP_C) -o $@

$(BUILD)/z80_exerciser_test: $(SRC_C) src/z80.h $(EXERCISER_C)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(EXERCISER_C) -o $@

# The fast tier: hermetic, no network, runs on every change.
test: $(BUILD)/z80_test $(BUILD)/crtc_test $(BUILD)/gate_array_test $(BUILD)/monitor_test $(BUILD)/ppi_test $(BUILD)/psg_test $(BUILD)/keyboard_test $(BUILD)/cpc_test $(BUILD)/timing_test $(BUILD)/png_test
	@$(BUILD)/z80_test
	@$(BUILD)/crtc_test
	@$(BUILD)/gate_array_test
	@$(BUILD)/monitor_test
	@$(BUILD)/ppi_test
	@$(BUILD)/psg_test
	@$(BUILD)/keyboard_test
	@$(BUILD)/cpc_test
	@$(BUILD)/timing_test
	@$(BUILD)/png_test

# The firmware images, fetched and pinned by hash. Needed to run a machine,
# not to build one or to test the parts.
roms:
	@sh tools/fetch-roms.sh

# The machine tier: the real firmware, booted and typed at. Needs the ROM
# images, so it fetches them first.
test-firmware: $(BUILD)/firmware_test
	@sh tools/fetch-roms.sh
	@$(BUILD)/firmware_test roms

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
	$(CLANG_TIDY) $(CORE_C) $(PNG_C) $(CLI_C) $(Z80_TEST_C) $(CRTC_TEST_C) $(GATE_ARRAY_TEST_C) $(MONITOR_TEST_C) $(PPI_TEST_C) $(PSG_TEST_C) $(KEYBOARD_TEST_C) $(CPC_TEST_C) $(TIMING_TEST_C) $(PNG_TEST_C) $(FIRMWARE_TEST_C) $(SINGLE_STEP_C) $(EXERCISER_C) -- $(CFLAGS) -Isrc -Icli -Itest

clean:
	rm -rf $(BUILD)

.PHONY: all roms test test-firmware test-single-step test-exerciser test-all format format-check lint clean
