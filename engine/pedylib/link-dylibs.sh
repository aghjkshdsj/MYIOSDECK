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
    xcrun otool -l "$DST/lib$stem.dylib" | grep -A10 -E 'sectname __pe_image|segname __TEXT$' | head -24
    ls -la "$DST/lib$stem.dylib"
done
