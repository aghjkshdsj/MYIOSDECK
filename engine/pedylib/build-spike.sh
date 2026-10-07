#!/bin/bash
# No-JIT spike, part 1 (Linux CI host): build the spike DLL the way Wine's PE DLLs are
# built (llvm-mingw), with 16 KB section alignment, for ARM64EC (what Wine uses) and
# plain ARM64, and convert each to <name>.img + <name>.S for the app build to link
# as signed dylibs (engine/pedylib/link-dylibs.sh, macOS).
#   engine/pedylib/build-spike.sh <out-dir> [toolchain-cache-dir]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:?out dir}"
CACHE="${2:-$HERE/toolchain}"
# Same llvm-mingw release as Madeira's Wine build (engine/wine/PIN), Linux x86-64 host.
LLVM_MINGW=llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64
LLVM_MINGW_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20260421/$LLVM_MINGW.tar.xz
LLVM_MINGW_SHA256=f8b8cce779affeab47bcaec6ce6e9e768b166094a366123ef64b2d0b389cc121

TC="$CACHE/$LLVM_MINGW/bin"
if [ ! -x "$TC/clang" ]; then
    mkdir -p "$CACHE"
    curl -fsSL -o "$CACHE/llvm-mingw.tar.xz" "$LLVM_MINGW_URL"
    echo "$LLVM_MINGW_SHA256  $CACHE/llvm-mingw.tar.xz" | sha256sum -c -
    tar -C "$CACHE" -xf "$CACHE/llvm-mingw.tar.xz"
    rm "$CACHE/llvm-mingw.tar.xz"
fi
export PATH="$TC:$PATH"
clang --version | head -1

mkdir -p "$OUT"
B="$OUT/obj"
mkdir -p "$B"
build_one() {
    local arch="$1"
    triple="$arch-w64-mingw32"
    m=$([ "$arch" = arm64ec ] && echo arm64ec || echo arm64)
    llvm-dlltool -m "$m" -d "$HERE/spike/myiosdeck.def" -l "$B/libmyiosdeck-$arch.a"
    # -O2 and no FMA contraction: the same code generation as the app's native kernels.
    # -nostdlib: like Wine's DLLs, no CRT; the app is the only import.
    # --section-alignment 16 KB: code and data never share an iOS page.
    LINK=(-shared -nostdlib -Wl,--section-alignment=0x4000 -Wl,--file-alignment=0x200
          -Wl,--image-base=0x6f0000000 "$B/libmyiosdeck-$arch.a")
    CFLAGS=(-target "$triple" -O2 -ffp-contract=off -fno-strict-aliasing -fno-stack-protector
            -I"$ROOT/engine/guest")
    # arm64ec_crt.c: the CHPE metadata + load config Wine DLLs get from winecrt0/winebuild.
    "$triple-clang" "${CFLAGS[@]}" -o "$B/spike-$arch.dll" "$HERE/spike/spike.c" "$HERE/spike/arm64ec_crt.c" "${LINK[@]}"
    llvm-readobj --file-headers --sections --coff-imports --coff-exports "$B/spike-$arch.dll" |
        grep -E 'Machine|SectionAlignment|Name:|VirtualAddress|Characteristics:|Symbol' | head -80 || true
    [ "$arch" = arm64ec ] && { llvm-readobj --coff-load-config "$B/spike-$arch.dll" | grep -iE 'CHPE|Arm64EC|dispatch|icall' | head -40 || true; }
    python3 "$HERE/pe2dylib.py" convert --strict "$B/spike-$arch.dll" "$OUT"
}

# Each variant on its own: plain ARM64 first (needs the least), then ARM64EC (what Wine uses).
fail=0
for arch in aarch64 arm64ec; do
    ( set -e; build_one "$arch" ) || { echo "::warning::spike DLL for $arch failed"; fail=1; }
done

# Step B (Wine without JIT): the stub x64 emulator Wine loads as xtajit64.dll, and an
# ARM64EC Windows Hello. PE files go to $OUT/winepe (bundled as is: Wine opens the files),
# their signed-dylib sources to $OUT/wine (named by full file name, as Wine looks them up).
build_wine_extras() {
    local EC=(-target arm64ec-w64-mingw32 -O2 -fno-stack-protector -Wl,--section-alignment=0x4000)
    mkdir -p "$OUT/winepe" "$OUT/wine"
    arm64ec-w64-mingw32-clang "${EC[@]}" -shared -nostdlib -o "$OUT/winepe/xtajit64.dll" \
        "$HERE/emu/xtajit64_stub.c" "$HERE/spike/arm64ec_crt.c" -lntdll
    arm64ec-w64-mingw32-clang "${EC[@]}" -o "$OUT/winepe/hello-arm64ec.exe" "$HERE/hello/hello-arm64ec.c"
    llvm-readobj --coff-imports "$OUT/winepe/hello-arm64ec.exe" | grep -E 'Name:' | head -20 || true
    for f in "$OUT/winepe/xtajit64.dll" "$OUT/winepe/hello-arm64ec.exe"; do
        python3 "$HERE/pe2dylib.py" convert --strict --keep-ext "$f" "$OUT/wine" | tail -3
    done
    # The x64 test suite for FXI inside Wine (step D): plain x86-64, its code is interpreted.
    x86_64-w64-mingw32-clang -O2 -c -o "$B/suite-x64.o" "$HERE/hello/suite-x64.c"
    x86_64-w64-mingw32-clang++ -O2 -c -o "$B/suite-x64-cxx.o" "$HERE/hello/suite-x64-cxx.cpp"
    x86_64-w64-mingw32-clang++ -static -o "$OUT/winepe/suite-x64.exe" "$B/suite-x64.o" "$B/suite-x64-cxx.o"
    llvm-readobj --coff-imports "$OUT/winepe/suite-x64.exe" | grep -E 'Name:' | sort -u | head -20 || true
}
( set -e; build_wine_extras ) || { echo "::warning::Wine no-JIT extras failed"; fail=1; }
ls -la "$OUT"
exit $fail
