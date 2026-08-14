#!/bin/sh
# Downloads the SingleStepTests Z80 corpus into test/data/SingleStepTests/z80/,
# the path mirroring the upstream coordinates so the folder names its source:
# https://github.com/SingleStepTests/z80 (MIT).
#
# Pinned to the commit validated on 2026-08-14 — the complete corpus, 1604
# files. Bump the pin deliberately, never implicitly.
#
# Upstream names prefixed files with spaces ("dd cb __ 46.json"); locally they
# are space-free ("ddcb46.json") because make and shell loops split on spaces.
set -e

commit="ebe1875d48f374bcfd4b505d8eb8ee751568b5f7"
expected_files=1604
archive_url="https://codeload.github.com/SingleStepTests/z80/zip/$commit"
test_directory="$(cd "$(dirname "$0")/../test" && pwd)"
destination="$test_directory/data/SingleStepTests/z80/v1"
staged="$destination.incomplete"

count_files() {
    ls "$1" | wc -l | tr -d ' '
}

if [ -d "$destination" ]; then
    present="$(count_files "$destination")"
    if [ "$present" -eq "$expected_files" ]; then
        exit 0
    fi
    echo "corpus holds $present of $expected_files files; fetching it again" >&2
    rm -rf "$destination"
fi

working_directory="$(mktemp -d)"
trap 'rm -rf "$working_directory" "$staged"' EXIT

echo "fetching SingleStepTests/z80 @ $commit"
curl -fsSL "$archive_url" -o "$working_directory/z80.zip"
unzip -q "$working_directory/z80.zip" -d "$working_directory"

# Staged beside the destination so the last step is a rename, which cannot
# half-happen.
rm -rf "$staged"
mkdir -p "$staged"
for file in "$working_directory/z80-$commit/v1/"*.json; do
    renamed="$(basename "$file" | tr -d ' _')"
    mv "$file" "$staged/$renamed"
done

count="$(count_files "$staged")"
if [ "$count" -ne "$expected_files" ]; then
    echo "expected $expected_files corpus files, found $count" >&2
    exit 1
fi

mv "$staged" "$destination"
echo "placed $count files"
