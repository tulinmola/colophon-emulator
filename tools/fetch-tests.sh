#!/bin/sh
# Downloads the SingleStepTests Z80 test data we currently consume into
# test/data/v1/ (git-ignored). https://github.com/SingleStepTests/z80 (MIT).
# The list grows as opcodes are implemented; existing files are not re-fetched.
set -e

base_url="https://raw.githubusercontent.com/SingleStepTests/z80/main/v1"
destination="$(dirname "$0")/../test/data/v1"
mkdir -p "$destination"

files="
00.json
"

for file in $files; do
    if [ ! -f "$destination/$file" ]; then
        echo "fetching $file"
        curl -fsSL "$base_url/$file" -o "$destination/$file"
    fi
done
