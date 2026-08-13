#!/bin/sh
# Downloads the SingleStepTests Z80 test data listed in test/manifest.txt into
# test/data/v1/ (git-ignored). https://github.com/SingleStepTests/z80 (MIT).
# The manifest is the passing set: one test file per implemented opcode.
set -e

base_url="https://raw.githubusercontent.com/SingleStepTests/z80/main/v1"
test_directory="$(dirname "$0")/../test"
destination="$test_directory/data/v1"
mkdir -p "$destination"

# Upstream names prefixed files with a space ("cb 00.json"); locally they are
# space-free ("cb00.json") so the manifest, make and this loop stay simple.
for file in $(cat "$test_directory/manifest.txt"); do
    if [ ! -f "$destination/$file" ]; then
        case "$file" in
            ????.json) remote="$(printf %s "$file" | cut -c1-2)%20$(printf %s "$file" | cut -c3-)" ;;
            *) remote="$file" ;;
        esac
        echo "fetching $file"
        curl -fsSL "$base_url/$remote" -o "$destination/$file"
    fi
done
