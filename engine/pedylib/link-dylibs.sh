#!/bin/bash
# No-JIT spike, part 2 (macOS, IPA build): link each <name>.S from build-spike.sh into
# lib<name>.dylib in App/Generated/PE, which the app bundles as PE/ and the IPA step
# signs. Always leaves App/Generated/PE in place (XcodeGen needs the folder).
#   engine/pedylib/link-dylibs.sh <dir with .S + .img>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IN="${1:?dir}"
DST="$ROOT/App/Generated/PE"
mkdir -p "$DST"
shopt -s nullglob
for s in "$IN"/*.S; do
    stem="$(basename "$s" .S)"
    (cd "$IN" && xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -dynamiclib \
        -install_name "@rpath/lib$stem.dylib" -o "$DST/lib$stem.dylib" "$stem.S")
    xcrun otool -l "$DST/lib$stem.dylib" | grep -B3 -A8 -E 'sectname __pe_(image|data)' | grep -E 'sectname|segname|addr|size'
    # The PE layout only holds if __DATA's payload starts exactly where __TEXT's ends.
    img=$(xcrun nm "$DST/lib$stem.dylib" | awk '$3=="_myiosdeck_pe_image"{print $1}')
    dat=$(xcrun nm "$DST/lib$stem.dylib" | awk '$3=="_myiosdeck_pe_data"{print $1}')
    want=$(stat -f %z "$IN/$stem.text.img")
    got=$(( 0x$dat - 0x$img ))
    echo "$stem: image 0x$img, data 0x$dat: offset $got, want $want"
    [ "$got" = "$want" ] || { echo "error: $stem: __DATA is not contiguous with __TEXT"; exit 1; }
    ls -la "$DST/lib$stem.dylib"
done
