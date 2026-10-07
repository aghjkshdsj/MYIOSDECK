#!/bin/bash
# No-JIT, part 2 (macOS, IPA build): link each <name>.S from pe2dylib.py into lib<name>.dylib
# in <dst> (default App/Generated/PE, bundled as PE/; the IPA step signs every dylib).
# Always leaves <dst> in place (XcodeGen needs the folder).
#   engine/pedylib/link-dylibs.sh <dir with .S + .img> [<dst>]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IN="${1:?dir}"
DST="${2:-$ROOT/App/Generated/PE}"
mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"   # the links run from inside $IN
shopt -s nullglob
n=0
for s in "$IN"/*.S; do
    stem="$(basename "$s" .S)"
    (cd "$IN" && xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -dynamiclib \
        -install_name "@rpath/lib$stem.dylib" -o "$DST/lib$stem.dylib" "$stem.S")
    # The PE layout only holds if __DATA's payload starts exactly where __TEXT's ends.
    syms=$(xcrun nm "$DST/lib$stem.dylib")
    img=$(awk '$3=="_myiosdeck_pe_image"{print $1}' <<<"$syms")
    dat=$(awk '$3=="_myiosdeck_pe_data"{print $1}' <<<"$syms")
    want=$(stat -f %z "$IN/$stem.text.img")
    got=$(( 0x$dat - 0x$img ))
    [ "$got" = "$want" ] || { echo "error: $stem: __DATA at +$got, want +$want (not contiguous with __TEXT)"; exit 1; }
    n=$((n + 1))
done
echo "linked $n PE dylibs into $DST ($(du -sh "$DST" | cut -f1))"
