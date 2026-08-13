CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -g
BUILD = build

SRC_C = src/z80.c test/z80_test.c test/json.c
SRC_ALL = $(SRC_C) src/z80.h test/json.h

TEST_DATA = test/data/v1/00.json

CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || echo xcrun clang-format)
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/clang-tidy 2>/dev/null || echo clang-tidy)

all: $(BUILD)/z80_test

$(BUILD)/z80_test: $(SRC_ALL)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(SRC_C) -o $@

test: $(BUILD)/z80_test $(TEST_DATA)
	$(BUILD)/z80_test $(TEST_DATA)

$(TEST_DATA):
	sh tools/fetch-tests.sh

format:
	$(CLANG_FORMAT) -i $(SRC_ALL)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SRC_ALL)

lint:
	$(CLANG_TIDY) $(SRC_C) -- $(CFLAGS) -Isrc

clean:
	rm -rf $(BUILD)

.PHONY: all test format format-check lint clean
