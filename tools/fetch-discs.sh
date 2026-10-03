#!/bin/sh
# Downloads the disc images the machine tier reads into test/data/discs/,
# pinned by hash the way the firmware is.
#
# Shaker is Longshot's suite of CRTC acid tests, published at
# https://shaker.logonsystem.eu/ for anyone building an emulator to grade
# it against real machines; the site offers the image for download with no
# terms beside it, and the Compendium it accompanies is CC BY-NC-ND. The
# image is used here as a real disc: written by a real tool in the DATA
# format, catalogued and loaded through the real AMSDOS ROM — and as the
# suite it is, run module by module by the tier of its own name.
#
# Batman Forever is Batman Group's 2011 demo, released at the Forever party
# and archived at scene.org for anyone to download. The demo tier plays it
# from end to end and sets the frames against a record; the release is
# fetched here rather than kept in the repository, as Shaker's disc is. Its
# one-disc release is double-sided, which is why the tier starts it from
# drive B.
#
# Pinned by content, validated on 2026-09-02, and the demo on 2026-09-23.
# The 2.6 image the site once served at /Shaker_CSL/shaker26.dsk now answers
# with the portal page.
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
    # Named for this process, so that two tiers fetching at once cannot
    # delete each other's half-written file.
    staged="$destination.$$.incomplete"
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

# The demo travels as an archive of four images, one of them the double-sided
# release the tier plays. Both the archive and the image it yields are pinned.
demo_url="https://files.scene.org/get/parties/2011/forever11/cpc/demo/batman_forever.zip"
demo_archive_hash="7bd7de65685416c3782790315f31d7cfa73e7ffde59bbe83493c020d0d18268e"
demo_member="Batman Forever (One disk version).dsk"
demo_file="batman-forever.dsk"
demo_hash="915e116b3b8ef8258e9948e845800ec83b6c04d44cdc2e71c86b9d3169e6c3ad"

demo_destination="$discs_directory/$demo_file"
if [ ! -f "$demo_destination" ] || [ "$(hash_of "$demo_destination")" != "$demo_hash" ]; then
    echo "fetching $demo_file"
    archive="$discs_directory/batman_forever.zip.$$.incomplete"
    staged="$demo_destination.$$.incomplete"
    trap 'rm -f "$archive" "$staged"' EXIT
    curl -fsSL "$demo_url" -o "$archive"

    actual="$(hash_of "$archive")"
    if [ "$actual" != "$demo_archive_hash" ]; then
        rm -f "$archive"
        echo "batman_forever.zip hashes to $actual, expected $demo_archive_hash" >&2
        exit 1
    fi

    if command -v unzip >/dev/null 2>&1; then
        unzip -p "$archive" "$demo_member" > "$staged"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' \
            "$archive" "$demo_member" > "$staged"
    else
        rm -f "$archive" "$staged"
        echo "neither unzip nor python3 is here to open batman_forever.zip" >&2
        exit 1
    fi

    actual="$(hash_of "$staged")"
    if [ "$actual" != "$demo_hash" ]; then
        rm -f "$archive" "$staged"
        echo "$demo_member hashes to $actual, expected $demo_hash" >&2
        exit 1
    fi
    mv "$staged" "$demo_destination"
    rm -f "$archive"
fi

echo "discs are in $discs_directory"
