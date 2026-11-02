#!/bin/sh
# Downloads the disc images the tiers read into test/data/discs/,
# pinned by hash the way the firmware is. Named on the command line, only
# those images are fetched, so a tier waits on the images it reads and no
# other; named none, every one is.
#
# Shaker is Longshot's suite of CRTC acid tests, published at
# https://shaker.logonsystem.eu/ for anyone building an emulator to grade
# it against real machines; the site offers the image for download with no
# terms beside it, and the Compendium it accompanies is CC BY-NC-ND. The
# image is used here as a real disc: written by a real tool in the DATA
# format, catalogued and loaded through the real AMSDOS ROM — and as the
# suite it is, run module by module by the tier of its own name. Two versions
# are run: 2.7, and 2.6 before it, the version the site's photographs of real
# machines show wherever their text tells the two apart, which Logon System's
# older site still serves. That one answers over plain HTTP, and the hash is
# what stands between it and the tier.
#
# Batman Forever is Batman Group's 2011 demo, released at the Forever party
# and archived at scene.org for anyone to download. The software tier plays it
# from end to end and sets the frames against a record; the release is
# fetched here rather than kept in the repository, as Shaker's disc is. Its
# one-disc release is double-sided, which is why the tier starts it from
# drive B.
#
# Pinned by content: Shaker 2.7 and 2.6 validated on 2026-10-04, the demo on
# 2026-09-23. The 2.7 image is the one the site has served since 2026-09-18;
# the one it served before, validated on 2026-09-02 and pinned by
# 65eb43e1f99ea232a6cc1494e799880488130ba1876bfe9d67b347a924e7721b, loads the
# same but for SHAKE27E.BIN, where six entries of one table were 3 and are 1.
# The rest that differs between the two images — slack past that file's end,
# two erased catalogue entries and sectors no file owns — is never loaded. The
# 2.6 image the site once served at /Shaker_CSL/shaker26.dsk answers with the
# portal page instead.
set -e

discs_directory="$(cd "$(dirname "$0")/.." && pwd)/test/data/discs"

asked="$*"
for name in $asked; do
    case "$name" in
    shaker27.dsk | shaker26.dsk | batman-forever.dsk) ;;
    *)
        echo "no disc image here is named $name" >&2
        exit 1
        ;;
    esac
done

wanted() {
    [ -z "$asked" ] && return 0
    for name in $asked; do
        [ "$name" = "$1" ] && return 0
    done
    return 1
}

# file|sha256|where it is served
images="\
shaker27.dsk|81592dc2f676097b33182f8e3ad12a60a1c8f0a51d47a9d2a0592fb4b4b087ba|https://shaker.logonsystem.eu/Shaker_CSL/shaker27.dsk
shaker26.dsk|f7082f8eab521d632c343a288f54038af6df090c59b372e0d2866269c2cc4d08|http://logon.system.free.fr/down/shaker26.dsk"

hash_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

# An image that cannot be had is reported and the others asked for are
# fetched all the same; the script fails at its end, naming every one it
# could not have.
unfetched=""

mkdir -p "$discs_directory"
for image in $images; do
    file="$(echo "$image" | cut -d'|' -f1)"
    expected="$(echo "$image" | cut -d'|' -f2)"
    url="$(echo "$image" | cut -d'|' -f3)"
    destination="$discs_directory/$file"
    wanted "$file" || continue

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
    if ! curl -fsSL "$url" -o "$staged"; then
        rm -f "$staged"
        echo "$file could not be fetched from $url" >&2
        unfetched="$unfetched $file"
        continue
    fi

    actual="$(hash_of "$staged")"
    if [ "$actual" != "$expected" ]; then
        rm -f "$staged"
        echo "$file hashes to $actual, expected $expected" >&2
        unfetched="$unfetched $file"
        continue
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

# Every step answers for its own failure, the function being called where a
# failure is counted rather than fatal.
fetch_the_demo() {
    archive="$discs_directory/batman_forever.zip.$$.incomplete"
    staged="$demo_destination.$$.incomplete"
    trap 'rm -f "$archive" "$staged"' EXIT
    if ! curl -fsSL "$demo_url" -o "$archive"; then
        rm -f "$archive"
        echo "batman_forever.zip could not be fetched from $demo_url" >&2
        return 1
    fi

    actual="$(hash_of "$archive")"
    if [ "$actual" != "$demo_archive_hash" ]; then
        rm -f "$archive"
        echo "batman_forever.zip hashes to $actual, expected $demo_archive_hash" >&2
        return 1
    fi

    if command -v unzip >/dev/null 2>&1; then
        if ! unzip -p "$archive" "$demo_member" > "$staged"; then
            rm -f "$archive" "$staged"
            echo "batman_forever.zip would not open" >&2
            return 1
        fi
    elif command -v python3 >/dev/null 2>&1; then
        if ! python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' \
            "$archive" "$demo_member" > "$staged"; then
            rm -f "$archive" "$staged"
            echo "batman_forever.zip would not open" >&2
            return 1
        fi
    else
        rm -f "$archive" "$staged"
        echo "neither unzip nor python3 is here to open batman_forever.zip" >&2
        return 1
    fi

    actual="$(hash_of "$staged")"
    if [ "$actual" != "$demo_hash" ]; then
        rm -f "$archive" "$staged"
        echo "$demo_member hashes to $actual, expected $demo_hash" >&2
        return 1
    fi
    # Called on the left of ||, where set -e stands down.
    mv "$staged" "$demo_destination" || return 1
    rm -f "$archive"
}

if wanted "$demo_file" &&
    { [ ! -f "$demo_destination" ] || [ "$(hash_of "$demo_destination")" != "$demo_hash" ]; }; then
    echo "fetching $demo_file"
    fetch_the_demo || unfetched="$unfetched $demo_file"
fi

if [ -n "$unfetched" ]; then
    echo "not fetched:$unfetched" >&2
    exit 1
fi
echo "discs are in $discs_directory"
