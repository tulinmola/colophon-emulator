#!/bin/sh
# Downloads the firmware ROM images into roms/. A CPC's is 32K, the operating
# system at offset 0x0000 and BASIC at 0x4000, beside the 16K AMSDOS ROM that
# comes with the disc interface; a Spectrum's is one 16K image.
#
# Amstrad's permission is what they are fetched under. Cliff Lawson answered
# for the Spectrum ROMs on comp.sys.sinclair in August 1999 and cross-posted
# the reply to comp.sys.amstrad.8bit "because it applies equally well to all
# the CPC stuff". His terms: the copyright messages stay intact, Amstrad's
# copyright is acknowledged, and nobody charges for the images. It does not
# reach everything here. The same answer says the CPC firmware is also
# Locomotive Software's, which its own message names, and that theirs is a
# separate permission to seek; and AMSDOS carries no copyright message for
# the first term to keep intact.
# https://worldofspectrum.net/app/themes/wosc-classic/static/legacy/amstrad-roms.txt
# https://groups.google.com/g/comp.sys.amstrad.8bit/c/HtpBU2Bzv_U/m/HhNDSU3MksAJ
#
# We fetch rather than vendor for two reasons. This repository is MIT and
# these images are not: they come with conditions, and a fork that sold them
# would breach Amstrad's terms without noticing. And the pin below is a more
# durable record than the bytes would be — a hash identifies the right file
# from any source, while a committed copy rots with one repository.
#
# So: pinned by content, validated on 2026-09-05, bumped deliberately and
# never implicitly. The images are served by the Caprice32 and Fuse projects,
# both GPL — these are Amstrad's images, not their work. When a URL dies, any
# copy matching the hash will do.
set -e

caprice32_url="https://raw.githubusercontent.com/ColinPitrat/caprice32/master/rom"
fuse_url="https://sourceforge.net/p/fuse-emulator/fuse/ci/master/tree/roms"
roms_directory="$(cd "$(dirname "$0")/.." && pwd)/roms"

# The names on the left are ours: upstream calls the Spectrum image 48.rom,
# which says nothing standing next to cpc464.rom.
images="\
cpc464.rom     00960d9bf75b2b90856c970f1aa078e1e2aa028b2c104f1dded0262f5d37b15e $caprice32_url/cpc464.rom
cpc664.rom     1fcb20cf169f170774bf94954db9372c95edd038a5cb8e5199774552b93f8747 $caprice32_url/cpc664.rom
cpc6128.rom    31c3668c67bea027dab698ece233c9434d9324f9ba7dac84db58f400b6689562 $caprice32_url/cpc6128.rom
amsdos.rom     ea65e0fb44ee93ede4b6c507509b7e5ddf497fb7155023bea91ef229469fa04d $caprice32_url/amsdos.rom
spectrum48.rom d55daa439b673b0e3f5897f99ac37ecb45f974d1862b4dadb85dec34af99cb42 $fuse_url/48.rom?format=raw"

if command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
else
    echo "need shasum or sha256sum to check the images against their pins" >&2
    exit 1
fi

mkdir -p "$roms_directory"

# Staged beside the destination so the last step is a rename, which cannot
# half-happen.
staged=""
trap 'rm -f "$staged"' EXIT

while read -r file sha256 url; do
    destination="$roms_directory/$file"
    if [ -f "$destination" ] && [ "$(sha256_of "$destination")" = "$sha256" ]; then
        continue
    fi

    echo "fetching $file"
    staged="$destination.incomplete"
    curl -fsSL "$url" -o "$staged"

    actual="$(sha256_of "$staged")"
    if [ "$actual" != "$sha256" ]; then
        echo "$file hashes to $actual, expected $sha256" >&2
        exit 1
    fi
    mv "$staged" "$destination"
    staged=""
done <<IMAGES
$images
IMAGES

echo "roms are in $roms_directory"
