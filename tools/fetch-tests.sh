#!/bin/sh
# Downloads the SingleStepTests Z80 test data listed in test/manifest.txt into
# test/data/v1/ (git-ignored). https://github.com/SingleStepTests/z80 (MIT).
# The manifest is the passing set: one test file per implemented opcode.
set -e

base_url="https://raw.githubusercontent.com/SingleStepTests/z80/main/v1"
test_directory="$(dirname "$0")/../test"
destination="$test_directory/data/v1"
mkdir -p "$destination"

for file in $(cat "$test_directory/manifest.txt"); do
    if [ ! -f "$destination/$file" ]; then
        echo "fetching $file"
        curl -fsSL "$base_url/$file" -o "$destination/$file"
    fi
done
