#!/bin/sh
# Downloads the disc images the machine tier reads into test/data/discs/,
# pinned by hash the way the firmware is.
#
# Shaker is Longshot's suite of CRTC acid tests, published at
# https://shaker.logonsystem.eu/ for anyone building an emulator to grade
# it against real machines; the site offers the image for download with no
# terms beside it, and the Compendium it accompanies is CC BY-NC-ND. The
# image is used here as a real disc: written by a real tool in the DATA
# format, catalogued and loaded through the real AMSDOS ROM. What the tests
# inside it show is the next question.
#
# Pinned by content, validated on 2026-09-02. The 2.6 image the site once
# served at /Shaker_CSL/shaker26.dsk now answers with the portal page.
set -e

base_url="https://shaker.logonsystem.eu/Shaker_CSL"
discs_directory="$(cd "$(dirname "$0")/.." && pwd)/test/data/discs"

# file:sha256
images="\
shaker27.dsk:65eb43e1f99ea232a6cc1494e799880488130ba1876bfe9d67b347a924e7721b"

hash_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$discs_directory"
for image in $images; do
    file="$(echo "$image" | cut -d: -f1)"
    expected="$(echo "$image" | cut -d: -f2)"
    destination="$discs_directory/$file"

    if [ -f "$destination" ] && [ "$(hash_of "$destination")" = "$expected" ]; then
        continue
    fi

    echo "fetching $file"
    # Staged beside the destination so the last step is a rename, which
    # cannot half-happen.
    staged="$destination.incomplete"
    trap 'rm -f "$staged"' EXIT
    curl -fsSL "$base_url/$file" -o "$staged"

    actual="$(hash_of "$staged")"
    if [ "$actual" != "$expected" ]; then
        rm -f "$staged"
        echo "$file hashes to $actual, expected $expected" >&2
        exit 1
    fi
    mv "$staged" "$destination"
done

echo "discs are in $discs_directory"
