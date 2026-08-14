CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -g
BUILD = build

SRC_C = src/z80.c
Z80_TEST_C = test/z80_test.c
SINGLE_STEP_C = test/z80_single_step_test.c test/json.c
EXERCISER_C = test/z80_exerciser_test.c
SRC_ALL = $(SRC_C) src/z80.h $(Z80_TEST_C) $(SINGLE_STEP_C) $(EXERCISER_C) test/json.h test/test.h

SINGLE_STEP_DATA = test/data/SingleStepTests/z80/v1
EXERCISER_DATA = test/data/ZEXALL
# Groups of the exerciser to run by default; empty runs all 67, which takes a while.
EXERCISER_GROUPS ?= 12

CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || echo xcrun clang-format)
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || echo clang-tidy)

all: $(BUILD)/z80_test $(BUILD)/z80_single_step_test $(BUILD)/z80_exerciser_test

$(BUILD)/z80_test: $(SRC_C) src/z80.h $(Z80_TEST_C) test/test.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(Z80_TEST_C) -o $@

$(BUILD)/z80_single_step_test: $(SRC_C) src/z80.h $(SINGLE_STEP_C) test/json.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(SINGLE_STEP_C) -o $@

$(BUILD)/z80_exerciser_test: $(SRC_C) src/z80.h $(EXERCISER_C)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -Itest $(SRC_C) $(EXERCISER_C) -o $@

# The fast tier: hermetic, no network, runs on every change.
test: $(BUILD)/z80_test
	@$(BUILD)/z80_test

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

test-all: test test-single-step test-exerciser

format:
	$(CLANG_FORMAT) -i $(SRC_ALL)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SRC_ALL)

lint:
	$(CLANG_TIDY) $(SRC_C) $(Z80_TEST_C) $(SINGLE_STEP_C) $(EXERCISER_C) -- $(CFLAGS) -Isrc -Itest

clean:
	rm -rf $(BUILD)

.PHONY: all test test-single-step test-exerciser test-all format format-check lint clean
