#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks the app's unpacker (unpack_test, built from myiosdeck_inno.cpp) against upstream
# innoextract's own command line on the installers make-installers.sh built:
#   compare.sh <installers_dir> <innoextract> <unpack_test>
# <installers_dir> holds one folder per Inno Setup version. Every installer must unpack to the
# same game folder as `innoextract -e -m -I /app`; inspect must find every .bin part, report a
# missing one by name, and recognise the old RAR-in-.bin layout.
set -euo pipefail
DIR="$(cd "${1:?installers dir}" && pwd)"
CLI="${2:?innoextract}"
TEST="${3:?unpack_test}"
WORK="$(mktemp -d)"
FAIL=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAIL=1; }

# unpack <exe> <language> <dir>: both tools into <dir>/ref and <dir>/ours
unpack() {
    "$CLI" -e -m -q -I /app --default-language "$2" -d "$3/ref" "$1" ||
        { fail "innoextract $(basename "$1") ($2)"; return 1; }
    "$TEST" extract "$1" "$3/ours" "$2" > "$3/ours.json" ||
        { fail "unpack_test $(basename "$1") ($2): $(cat "$3/ours.json")"; return 1; }
}

same() { # dir label
    if diff -r "$1/ref" "$1/ours" > "$1/diff.txt"; then
        pass "$2: $(find "$1/ours/app" -type f | wc -l) files identical"
    else
        fail "$2"; head -20 "$1/diff.txt"
    fi
}

for verdir in "$DIR"/*/; do
    ver="$(basename "$verdir")"
    echo "== Inno Setup $ver"
    "$CLI" --version | head -1
    for exe in "$verdir"setup_mid_*.exe; do
        name="$(basename "$exe" .exe)"
        [ "$name" = setup_mid_dlc ] && continue
        w="$WORK/$ver/$name"
        mkdir -p "$w"
        "$CLI" -s --data-version "$exe" || true
        unpack "$exe" english "$w" && same "$w" "$ver $name"
        [ -f "$w/ours/app/lang.txt" ] && grep -q "english" "$w/ours/app/lang.txt" &&
            pass "$ver $name: English lang.txt" || fail "$ver $name: lang.txt is not the English one"
        [ ! -e "$w/ours/tmp" ] && [ ! -e "$w/ours/commonappdata" ] &&
            pass "$ver $name: only app/" || fail "$ver $name: files outside app/"
        [ -d "$w/ours/app/saves" ] && pass "$ver $name: empty [Dirs] folder" || fail "$ver $name: no app/saves"

        "$TEST" inspect "$exe" > "$w/inspect.json" || fail "$ver $name: inspect $(cat "$w/inspect.json")"
        jq -c '{app_name, default_dir_name, inno_version, embedded_data, app_files, app_size, languages, parts}' "$w/inspect.json"
        jq -e '.ok and .app_files > 0 and ([.parts[] | select(.present | not)] | length == 0)
               and (.languages == ["english", "german"])' "$w/inspect.json" > /dev/null &&
            pass "$ver $name: inspect" || fail "$ver $name: inspect"
        if [ "$name" = setup_mid_bzip ]; then
            jq -e '.embedded_data and (.parts | length == 0)' "$w/inspect.json" > /dev/null &&
                pass "$ver $name: data inside the .exe" || fail "$ver $name: expected embedded data"
        else
            jq -e '(.embedded_data | not) and (.parts | length > 1)' "$w/inspect.json" > /dev/null &&
                pass "$ver $name: $(jq '.parts | length' "$w/inspect.json") .bin parts" || fail "$ver $name: expected .bin parts"
        fi
    done

    # German: the other language's file wins the lang.txt collision, as with the command line.
    w="$WORK/$ver/german"
    mkdir -p "$w"
    unpack "$verdir/setup_mid_lzma2.exe" german "$w" && same "$w" "$ver german" &&
        { grep -q deutsch "$w/ours/app/lang.txt" && pass "$ver: German lang.txt" || fail "$ver: lang.txt is not the German one"; }

    # The add-on unpacked over the base game: same folder, one file replaced.
    w="$WORK/$ver/setup_mid_lzma2"
    unpack "$verdir/setup_mid_dlc.exe" english "$w" && same "$w" "$ver base + add-on" &&
        { grep -q "patched" "$w/ours/app/data/script.txt" && [ -f "$w/ours/app/dlc/expansion.pak" ] &&
          pass "$ver: add-on files in place" || fail "$ver: add-on files missing"; }

    # A missing part is reported by name and unpacking fails cleanly.
    w="$WORK/$ver/missing"
    mkdir -p "$w/in"
    cp "$verdir"/setup_mid_lzma2* "$w/in/"
    rm "$w/in/setup_mid_lzma2-2.bin"
    "$TEST" inspect "$w/in/setup_mid_lzma2.exe" > "$w/inspect.json" || true
    jq -e '[.parts[] | select(.present | not) | .name] == ["setup_mid_lzma2-2.bin"]' "$w/inspect.json" > /dev/null &&
        pass "$ver: missing setup_mid_lzma2-2.bin reported" || { fail "$ver: missing part"; cat "$w/inspect.json"; }
    if "$TEST" extract "$w/in/setup_mid_lzma2.exe" "$w/out" english > "$w/extract.json"; then
        fail "$ver: unpacking without a part succeeded"
    else
        pass "$ver: unpacking without a part fails: $(jq -r .error "$w/extract.json")"
    fi

    # Part names in another case (copied from a case-insensitive disk) still count.
    w="$WORK/$ver/case"
    mkdir -p "$w"
    cp "$verdir"/setup_mid_zip.exe "$w/"
    for f in "$verdir"/setup_mid_zip-*.bin; do cp "$f" "$w/$(basename "$f" | tr a-z A-Z | sed 's/\.BIN$/.bin/')"; done
    "$TEST" inspect "$w/setup_mid_zip.exe" > "$w/inspect.json" || true
    jq -e '[.parts[] | select(.present | not)] | length == 0' "$w/inspect.json" > /dev/null &&
        pass "$ver: upper-case part names found" || { fail "$ver: upper-case part names"; cat "$w/inspect.json"; }

    # Old GOG layout: data in the .exe plus a RAR archive named like a part.
    w="$WORK/$ver/rar"
    mkdir -p "$w"
    cp "$verdir/setup_mid_bzip.exe" "$w/"
    printf 'Rar!\x1a\x07\x00' > "$w/setup_mid_bzip-1.bin"
    "$TEST" inspect "$w/setup_mid_bzip.exe" > "$w/inspect.json" || true
    jq -e '.rar_data' "$w/inspect.json" > /dev/null && pass "$ver: RAR .bin recognised" || fail "$ver: RAR .bin"
done

# Not an installer at all.
head -c 4096 /dev/urandom > "$WORK/setup_noise.exe"
if "$TEST" inspect "$WORK/setup_noise.exe" > "$WORK/noise.json"; then
    fail "random bytes inspected as an installer"
else
    pass "random bytes rejected: $(jq -r .error "$WORK/noise.json")"
fi

[ "$FAIL" = 0 ] && echo "All checks passed" || { echo "Some checks failed"; exit 1; }
