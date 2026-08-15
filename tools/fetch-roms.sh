#!/bin/sh
# Downloads the Amstrad firmware ROM images into roms/, one file per machine,
# each 32K: the operating system at 0x0000 and BASIC at 0x4000.
#
# Amstrad gave blanket permission to distribute these images with emulators —
# Cliff Lawson, 1999, on comp.sys.amstrad.8bit — on the terms that the
# copyright messages stay intact, Amstrad's copyright is acknowledged, and
# nobody charges for the ROMs. The permission has never been rescinded, and
# every emulator in the field relies on it:
# https://groups.google.com/g/comp.sys.amstrad.8bit/c/HtpBU2Bzv_U/m/HhNDSU3MksAJ
#
# We fetch rather than vendor for two reasons. This repository is MIT and
# these images are not: they come with conditions, and a fork that sold them
# would breach Amstrad's terms without noticing. And the pin below is a more
# durable record than the bytes would be — a hash identifies the right file
# from any source, while a committed copy rots with one repository.
#
# So: pinned by content, validated on 2026-08-15, bumped deliberately and
# never implicitly. The files are served by the Caprice32 project, whose own
# code is GPL — these are Amstrad's images, not Caprice32's work. When that
# URL eventually dies, any copy that matches the hash will do; CPCWiki's ROM
# List is where the community keeps its index of them.
set -e

base_url="https://raw.githubusercontent.com/ColinPitrat/caprice32/master/rom"
roms_directory="$(cd "$(dirname "$0")/.." && pwd)/roms"

# machine:file:sha256
images="\
cpc464:cpc464.rom:00960d9bf75b2b90856c970f1aa078e1e2aa028b2c104f1dded0262f5d37b15e
cpc664:cpc664.rom:1fcb20cf169f170774bf94954db9372c95edd038a5cb8e5199774552b93f8747
cpc6128:cpc6128.rom:31c3668c67bea027dab698ece233c9434d9324f9ba7dac84db58f400b6689562"

hash_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$roms_directory"
for image in $images; do
    file="$(echo "$image" | cut -d: -f2)"
    expected="$(echo "$image" | cut -d: -f3)"
    destination="$roms_directory/$file"

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

echo "roms are in $roms_directory"
