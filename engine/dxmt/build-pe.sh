#!/bin/bash
# No-JIT step 5 (docs/NO_JIT_WINDOWS.md): the Direct3D/Metal DLLs rebuilt with 16 KB section
# alignment, so engine/pedylib/pe2dylib.py can wrap them as signed dylibs.
#
# Madeira ships these PE DLLs prebuilt with 4 KB sections: the tail of .text and the start of
# .rdata (where lld puts the import address table, which Wine writes at load) share a 16 KB
# iOS page, and a signed code page cannot be written. Built here from the same sources:
#   DXMT (Madeira's fork, engine/dxmt/PIN): winemetal.dll, d3d11.dll, d3d10core.dll, dxgi.dll
#   madeira-d3d12 (Madeira): madeira_d3d12.dll (also shipped as d3d12.dll), d3d12core.dll
# winemetal links Wine's winecrt0 and ntdll/dbghelp import libraries and is marked builtin by
# winebuild, so a Wine tree configured for ARM64EC provides those (only those targets are built).
#
#   engine/dxmt/build-pe.sh <step>     fetch | wine | dxmt | d3d12 | collect | all
# Output: engine/dxmt/out-pe/*.dll (macOS host: DXMT compiles its Metal shaders with xcrun).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/engine/wine/PIN"
DXMT_PIN_SHA="$(sed -n 's/^DXMT_SHA=//p' "$ROOT/engine/dxmt/PIN")"
M="$ROOT/engine/wine/madeira"     # the Wine stage's checkout (Madeira + its Wine fork + llvm-mingw)
W="$M/wine"
TC="$M/toolchains/$LLVM_MINGW/bin"
OUT="$ROOT/engine/dxmt/out-pe"
JOBS="$(sysctl -n hw.ncpu)"
export PATH="$TC:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
ALIGN="-Wl,--section-alignment=0x4000"

step_fetch() {
    bash "$ROOT/engine/wine/build-unix.sh" fetch
    if [ "$(git -C "$M/dxmt" rev-parse HEAD 2>/dev/null)" != "$DXMT_PIN_SHA" ]; then
        git -C "$M" submodule update --init --depth 1 dxmt
    fi
    git -C "$M/dxmt" rev-parse HEAD
    git -C "$M/dxmt" submodule update --init --depth 1 --recursive
}

step_wine() {
    mkdir -p "$W/build-arm64ec"
    cd "$W/build-arm64ec"
    if [ ! -f config.status ]; then
        ../configure --enable-archs=arm64ec --without-x --disable-tests --without-freetype --without-gnutls \
            --without-gstreamer --without-sdl --without-cups --without-sane --without-pcap --without-krb5 \
            --without-opencl --without-vulkan --without-usb --without-v4l2 --without-netapi --without-capi \
            --without-gphoto --without-pcsclite --without-inotify --without-dbus --without-ffmpeg \
            > configure.log 2>&1 || { tail -60 configure.log; return 1; }
    fi
    make -j"$JOBS" tools/all tools/winebuild/all tools/widl/all > make-tools.log 2>&1 || { tail -40 make-tools.log; return 1; }
    # Per-directory makes, as Madeira's build/wine-pe scripts do (winecrt0 is in libs/ in this
    # Wine; DXMT's meson looks in libs/winecrt0 and dlls/winecrt0).
    local crt=libs/winecrt0
    [ -d ../libs/winecrt0 ] || crt=dlls/winecrt0
    { make -j"$JOBS" -C "$crt" && make -j"$JOBS" -C dlls/ntdll && make -j"$JOBS" -C dlls/dbghelp; } > make-libs.log 2>&1 ||
        { tail -60 make-libs.log; return 1; }
    ls -la tools/winebuild/winebuild
    find "$crt" dlls/ntdll dlls/dbghelp -name '*.a' | xargs ls -la
}

# Bump when the link options change: the build directory is cached and meson keeps the options
# it was set up with.
DXMT_BUILD="build-arm64ec-16k-v2"

step_dxmt() {
    local D="$M/dxmt" B="$M/dxmt/$DXMT_BUILD"
    # The cross file names the toolchain under @GLOBAL_SOURCE_ROOT@/toolchains; add the 16 KB
    # section alignment for every link, and -static: the C++ DLLs (d3d11, dxgi) must carry libc++
    # and libunwind inside them, the Wine prefix has no libc++.dll (build 70: d3d11 failed to load).
    ln -sfn "$M/toolchains" "$D/toolchains"
    sed "s#llvm-mingw-20260421-ucrt-macos-universal#$LLVM_MINGW#g" "$D/build-arm64ec-win.txt" > "$D/build-arm64ec-16k.txt"
    printf "\n[built-in options]\nc_link_args = ['-static', '%s']\ncpp_link_args = ['-static', '%s']\n" "$ALIGN" "$ALIGN" \
        >> "$D/build-arm64ec-16k.txt"
    if [ ! -f "$B/build.ninja" ]; then
        meson setup "$B" "$D" -Dbuildtype=release -Dwine_build_path="$W/build-arm64ec" \
            --cross-file="$D/build-arm64ec-16k.txt"
    fi
    ninja -C "$B" src/winemetal/winemetal.dll src/d3d11/d3d11.dll src/d3d10/d3d10core.dll src/dxgi/dxgi.dll
    mkdir -p "$OUT"
    cp "$B/src/winemetal/winemetal.dll" "$B/src/d3d11/d3d11.dll" "$B/src/d3d10/d3d10core.dll" "$B/src/dxgi/dxgi.dll" "$OUT/"
}

step_d3d12() {
    # Madeira build/madeira-d3d12/build-pe.sh, with the 16 KB alignment and our paths.
    local S="$M/madeira-d3d12/src/pe" B="$M/dxmt/$DXMT_BUILD"
    mkdir -p "$OUT"
    python3 "$S/gen_vtables.py" "$M/toolchains/$LLVM_MINGW/generic-w64-mingw32/include/d3d12.h" \
        "$S/madeira_d3d12_stubs.h" > /dev/null
    arm64ec-w64-mingw32-clang -shared -O2 -Wall $ALIGN -o "$OUT/madeira_d3d12.dll" "$S/madeira_d3d12.c" "$S/d3d12.def" \
        -I"$S" -I"$M/madeira-d3d12/src" -I"$M/dxmt/src/winemetal" -L"$B/src/winemetal" -lwinemetal -luuid -lole32
    cp "$OUT/madeira_d3d12.dll" "$OUT/d3d12.dll"   # one binary, both names (Madeira ml849)
    arm64ec-w64-mingw32-clang -shared -O2 -Wall $ALIGN -o "$OUT/d3d12core.dll" "$S/d3d12core.c" "$S/d3d12core.def"
}

step_collect() {
    mkdir -p "$OUT"
    local bad=0
    for f in "$OUT"/*.dll; do
        llvm-readobj --file-headers "$f" | grep -E 'SectionAlignment' | sed "s#^#$(basename "$f"): #"
        # Every import must be a DLL the Wine prefix has: no toolchain runtime DLLs.
        imps=$(llvm-readobj --coff-imports "$f" | sed -n 's/^ *Name: \(.*\.dll\)$/\1/Ip' | sort -u | tr '\n' ' ')
        echo "$(basename "$f"): imports $imps"
        if grep -qiE 'libc\+\+|libunwind|libwinpthread|libgcc' <<<"$imps"; then
            echo "error: $(basename "$f") imports a toolchain runtime DLL the Wine prefix does not have" >&2
            bad=1
        fi
    done
    ls -la "$OUT"
    return $bad
}

case "${1:-all}" in
    fetch) step_fetch ;;
    wine) step_wine ;;
    dxmt) step_dxmt ;;
    d3d12) step_d3d12 ;;
    collect) step_collect ;;
    all) step_fetch; step_wine; step_dxmt; step_d3d12; step_collect ;;
    *) echo "unknown step $1" >&2; exit 2 ;;
esac
