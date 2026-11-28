#!/bin/sh
# Downloads the programs the tiers run into test/data/programs/, tapes and
# snapshots, pinned by hash the way the firmware and the discs are. Named on
# the command line, only those are fetched; named none, every one is.
#
# Each is a program for testing a Spectrum whose results on real machines
# are on record, and each is fetched rather than kept here, as Shaker's disc
# is: Ramsoft's Floating Spy, Richard and Tim Butler's 48K timing tests,
# five of the ZXTests, which are GPL — Jan Bobrowski's btime, stime and
# minfo; atime, Patrik Rak's slight modification of stime; and Rak's
# ptime — Rak's Timing Test, GPL too, and Mark Woodmass's IFF2 Bug, as
# the tape and as the .z80 snapshot it is released as. Beside them is one
# program for a machine not built here, Weiv's screen switching test for
# the +2A and +3, a snapshot a +2 must refuse.
# All of them are served from zxe.io's depot, which catalogues each release
# under its title, date and author; Floating Spy, the ZXTests and the Timing
# Test come as archives, fetched once a run however many tapes are taken
# out of them, and both the archive and each tape are pinned.
#
# Pinned by content on 2026-11-27, the last three on 2026-11-28.
set -e

programs_directory="$(cd "$(dirname "$0")/.." && pwd)/test/data/programs"
depot="https://zxe.io/depot/software/ZX%20Spectrum"

# file|sha256|archive|its sha256|where it is served|its path in the archive,
# where that is not its name
images="\
butler48.tap|ecdc983879dcd27f232c3abe69e2b6c6c27befcbacfc1d7d5a710b5058f2f2f6|||$depot/ZX%20Spectrum%20Timing%20Tests%20-%2048K%20v1.0%20%282010-04-14%29%28Butler%2C%20Richard%3B%20Butler%2C%20Tim%29%5B%21%5D.tap
floatspy.tap|dc4a3ba0b0b74396919e0a67f0984aaa5762a3bdd3e0afb4bc38ac72fa7bef34|floatspy.zip|3663cfc76b0733491c69faf088dfacda9294aa76e828600070380086138747ed|$depot/Floating%20Spy%20v0.33%20%282002-04%29%28Ramsoft%29%5B%21%5D.zip
btime.tap|153991adb0a61771ee6cf753b15b03287a92d5e823548e5bf0d245066f98748f|zxtests.zip|911bffcab0d5c424c7a1e97cb8c179445637117c3c6bf6797f790f34599bb0c0|$depot/ZXTests%20v3p%20%282014-03-20%29%28Rak,%20Patrik%29%5B%21%5D.zip
stime.tap|b4658848ab96d0fef71b998948b3d46ea4ccd595954ce99a9b77f8bec18559ca|zxtests.zip|911bffcab0d5c424c7a1e97cb8c179445637117c3c6bf6797f790f34599bb0c0|$depot/ZXTests%20v3p%20%282014-03-20%29%28Rak,%20Patrik%29%5B%21%5D.zip
atime.tap|bbba33942da839d747b24c38c6e7762974d8c0922163e386cd558e5254886b60|zxtests.zip|911bffcab0d5c424c7a1e97cb8c179445637117c3c6bf6797f790f34599bb0c0|$depot/ZXTests%20v3p%20%282014-03-20%29%28Rak,%20Patrik%29%5B%21%5D.zip
minfo.tap|7caff77f607d58e4f3df8b5e69d4cd6803b67d79f19a8298ee2fcc37630c3375|zxtests.zip|911bffcab0d5c424c7a1e97cb8c179445637117c3c6bf6797f790f34599bb0c0|$depot/ZXTests%20v3p%20%282014-03-20%29%28Rak,%20Patrik%29%5B%21%5D.zip
ptime.tap|b52a7e55ce47b01e3792d40050858918a022d3f4c66871bd6b74c988351cdce7|zxtests.zip|911bffcab0d5c424c7a1e97cb8c179445637117c3c6bf6797f790f34599bb0c0|$depot/ZXTests%20v3p%20%282014-03-20%29%28Rak,%20Patrik%29%5B%21%5D.zip
timing.tap|da62ba6438af1398b3e1d1bbd3627985373b8cc81a72628421285329f4903962|timingtest.zip|bacff01453a01c14754c167b5b02695ec29a0a5960c2f202b52b26494c2e8dff|$depot/Timing%20Test%20v0.3%20%282013-12-09%29%28Rak,%20Patrik%29%5B%21%5D.zip|timingtest-0.3/timing.tap
iff2bug.tap|b7ad26a273f70d7afcceb1519f2dfd1c9749144c2aa7c76b9d9263871ff216d7|||$depot/IFF2%20Bug%20%282023-04-30%29%28Woodmass%2C%20Mark%29%5B%21%5D.tap
iff2bug.z80|4a33c94fb79556cbfb8d4213d8c854614f8d8da3e0ce14c7c262b763b50e9c34|||$depot/IFF2%20Bug%20%282023-04-30%29%28Woodmass%2C%20Mark%29%5B%21%5D.z80
screenswitch.z80|bcb07ade1222e94da4be0e9534b95ff6cada270577426815790bad309c97527e|||$depot/%2B2A-%2B3%20Screen%20Switching%20Test%20%282024-11-30%29%28Weiv%29%5B%21%5D.z80"

asked="$*"
for name in $asked; do
    if ! echo "$images" | cut -d'|' -f1 | grep -qxF "$name"; then
        echo "no program here is named $name" >&2
        exit 1
    fi
done

wanted() {
    [ -z "$asked" ] && return 0
    for name in $asked; do
        [ "$name" = "$1" ] && return 0
    done
    return 1
}

hash_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        sha256sum "$1" | cut -d' ' -f1
    fi
}

# The one member of an archive, written to standard output.
member_of() {
    if command -v unzip >/dev/null 2>&1; then
        unzip -p "$1" "$2"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))' "$1" "$2"
    else
        echo "neither unzip nor python3 is here to open $1" >&2
        return 1
    fi
}

# Every step answers for its own failure, the function being called where a
# failure is counted rather than fatal. Staged beside the destination and
# named for this process, so that the last step is a rename, which cannot
# half-happen, and two tiers fetching at once cannot delete each other's
# half-written file.
fetch() {
    file="$1" expected="$2" archive="$3" archive_hash="$4" url="$5" member="${6:-$1}"
    staged="$programs_directory/$file.$$.incomplete"
    if [ -z "$archive" ]; then
        if ! curl -fsSL "$url" -o "$staged"; then
            rm -f "$staged"
            echo "$file could not be fetched from $url" >&2
            return 1
        fi
    else
        # An archive already fetched and found sound this run is used again.
        download="$programs_directory/$archive.$$.incomplete"
        if [ ! -f "$download" ]; then
            if ! curl -fsSL "$url" -o "$download"; then
                rm -f "$download"
                echo "$file could not be fetched from $url" >&2
                return 1
            fi
            actual="$(hash_of "$download")"
            if [ "$actual" != "$archive_hash" ]; then
                rm -f "$download"
                echo "$archive hashes to $actual, expected $archive_hash" >&2
                return 1
            fi
        fi
        if ! member_of "$download" "$member" > "$staged"; then
            rm -f "$staged"
            echo "$file would not come out of $archive" >&2
            return 1
        fi
    fi
    actual="$(hash_of "$staged")"
    if [ "$actual" != "$expected" ]; then
        rm -f "$staged"
        echo "$file hashes to $actual, expected $expected" >&2
        return 1
    fi
    # Called on the left of ||, where set -e stands down.
    mv "$staged" "$programs_directory/$file" || return 1
}

# An image that cannot be had is reported and the others asked for are
# fetched all the same; the script fails at its end, naming every one it
# could not have.
unfetched=""

# Whatever this run staged and did not keep, archives included, goes at
# its end.
trap 'rm -f "$programs_directory"/*."$$".incomplete' EXIT

mkdir -p "$programs_directory"
for image in $images; do
    file="$(echo "$image" | cut -d'|' -f1)"
    expected="$(echo "$image" | cut -d'|' -f2)"
    wanted "$file" || continue
    destination="$programs_directory/$file"
    if [ -f "$destination" ] && [ "$(hash_of "$destination")" = "$expected" ]; then
        continue
    fi
    echo "fetching $file"
    fetch "$file" "$expected" "$(echo "$image" | cut -d'|' -f3)" \
        "$(echo "$image" | cut -d'|' -f4)" "$(echo "$image" | cut -d'|' -f5)" \
        "$(echo "$image" | cut -d'|' -f6)" ||
        unfetched="$unfetched $file"
done

if [ -n "$unfetched" ]; then
    echo "not fetched:$unfetched" >&2
    exit 1
fi
echo "programs are in $programs_directory"
