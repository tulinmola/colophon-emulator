#!/bin/sh
# Downloads Frank Cringle's Z80 instruction set exerciser into
# test/data/ZEXALL/, the path mirroring the upstream coordinates so the folder
# names its source: https://github.com/agn453/ZEXALL
#
# The exercisers are GPL-2.0 and stay out of this MIT repository: they are
# fetched, git-ignored test material, exactly like the SingleStepTests corpus.
#
# Pinned to the commit validated on 2026-08-14. Bump deliberately.
set -e

commit="8f71d418bae69a476a5a0e5c6e122c8801b8d9f4"
base_url="https://raw.githubusercontent.com/agn453/ZEXALL"
destination="$(cd "$(dirname "$0")/../test" && pwd)/data/ZEXALL"

if [ -f "$destination/zexdoc.com" ] && [ -f "$destination/zexall.com" ]; then
    exit 0
fi

mkdir -p "$destination"
for program in zexdoc.com zexall.com; do
    if [ ! -f "$destination/$program" ]; then
        echo "fetching $program"
        curl -fsSL "$base_url/$commit/$program" -o "$destination/$program"
    fi
done
