#!/bin/bash
# Stage 3: build DXMT's iOS side (Direct3D -> Metal) as one static library.
#   engine/dxmt/build-unix.sh <step>
#   fetch   Madeira + its DXMT fork (+ DXMT submodules) + LLVM 15 sources
#   llvm    LLVM 15 libraries for iOS arm64 (host llvm-tblgen first)
#   metal   Metal toolchain component (Xcode 26 ships it separately)
#   dxmt    Madeira build/dxmt-ios/build.sh -> combined with LLVM -> libdxmt_combined.a
#   tests   d3d12-cube-x64.exe with colour writes enabled (llvm-mingw)
#   collect engine/dxmt/out
# The Windows-side DXMT DLLs (d3d11, dxgi, winemetal, d3d12) are prebuilt in
# Madeira's ARM64EC DLL farm and match this commit.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/engine/dxmt"
# shellcheck disable=SC1091
source "$HERE/PIN"
M="$HERE/madeira"
T="$M/toolchains"
OUT="$HERE/out"
JOBS="$(sysctl -n hw.ncpu)"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"

step_fetch() {
    if [ ! -d "$M/.git" ] || [ "$(git -C "$M" rev-parse HEAD)" != "$MADEIRA_SHA" ]; then
        rm -rf "$M"
        git init -q "$M"
        git -C "$M" remote add origin "$MADEIRA_REPO"
        git -C "$M" fetch -q --depth 1 origin "$MADEIRA_SHA"
        git -C "$M" checkout -q FETCH_HEAD
    fi
    if [ "$(git -C "$M/dxmt" rev-parse HEAD 2>/dev/null)" != "$DXMT_SHA" ]; then
        git -C "$M" submodule update --init --depth 1 dxmt
    fi
    git -C "$M/dxmt" submodule update --init --depth 1 --recursive
    if [ ! -d "$T/llvm-project/llvm" ]; then
        mkdir -p "$T"
        curl -fsSL "$LLVM_URL" | tar -xJ -C "$T"
        mv "$T/llvm-project-$LLVM_VERSION.src" "$T/llvm-project"
        # Apple's linker has no --gc-sections; LLVM only knows Darwin uses -dead_strip.
        sed -i '' 's/MATCHES "Darwin"/MATCHES "Darwin|iOS"/' "$T/llvm-project/llvm/cmake/modules/AddLLVM.cmake"
    fi
}

step_llvm() {
    local H="$T/llvm-host-build" B="$T/llvm-ios-build"
    # LLVM 15 predates libc++ dropping transitive <cstdint>/<string> includes.
    local CXXF="-include cstdint -include string -Wno-deprecated-declarations"
    if [ ! -x "$H/bin/llvm-tblgen" ]; then
        cmake -S "$T/llvm-project/llvm" -B "$H" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DLLVM_TARGETS_TO_BUILD="" -DLLVM_INCLUDE_TESTS=Off -DLLVM_INCLUDE_BENCHMARKS=Off \
            -DLLVM_ENABLE_ZSTD=Off -DLLVM_ENABLE_ZLIB=Off -DLLVM_ENABLE_TERMINFO=Off \
            -DCMAKE_CXX_FLAGS="$CXXF"
        ninja -C "$H" llvm-tblgen
    fi
    if [ ! -f "$B/build.ninja" ]; then
        cmake -S "$T/llvm-project/llvm" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphoneos \
            -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
            -DLLVM_HOST_TRIPLE=arm64-apple-ios17.0 -DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios17.0 \
            -DLLVM_TARGET_ARCH=host -DLLVM_TARGETS_TO_BUILD="" -DLLVM_ENABLE_PROJECTS="" \
            -DLLVM_TABLEGEN="$H/bin/llvm-tblgen" -DLLVM_BUILD_TOOLS=Off -DLLVM_BUILD_UTILS=Off \
            -DLLVM_INCLUDE_TOOLS=Off -DLLVM_INCLUDE_UTILS=Off -DLLVM_INCLUDE_TESTS=Off \
            -DLLVM_INCLUDE_EXAMPLES=Off -DLLVM_INCLUDE_BENCHMARKS=Off -DLLVM_INCLUDE_DOCS=Off \
            -DLLVM_ENABLE_ZLIB=Off -DLLVM_ENABLE_ZSTD=Off -DLLVM_ENABLE_TERMINFO=Off \
            -DLLVM_ENABLE_LIBXML2=Off -DLLVM_ENABLE_ASSERTIONS=Off \
            -DCMAKE_CXX_FLAGS="$CXXF"
    fi
    ninja -C "$B" -j "$JOBS" > "$T/llvm-ios-ninja.log" 2>&1 || { grep -B2 -A12 'error:' "$T/llvm-ios-ninja.log" | head -80; return 1; }
    ls "$B"/lib/*.a | wc -l
}

# airconv_context.cpp embeds three AIR modules (air_msad.h, air_samplepos.h,
# air_tessellation.h). DXMT's meson makes them with metalir_generator +
# hexdump_generator (src/airconv/meson.build); Madeira's build.sh doesn't, so
# do the same here: .metal -> .air -> xxd C array named after the file.
airconv_shader_headers() {
    local H="$1" S="$M/dxmt/src/airconv/shaders" n
    mkdir -p "$H"
    for n in air_msad air_samplepos air_tessellation; do
        xcrun -sdk macosx metal -std=metal3.1 --target=air64-apple-macos14.0 \
            -o "$H/$n.air" -c "$S/$n.metal"
        (cd "$H" && xxd -n "$n" -i "$n.air" "$n.h")
        echo "  $n.h OK"
    done
}

step_metal() {
    xcrun -sdk macosx metal --version 2>/dev/null || xcodebuild -downloadComponent MetalToolchain
    xcrun -sdk macosx metal --version
}

step_dxmt() {
    local D="$M/build/dxmt-ios"
    airconv_shader_headers "$D/shader-headers"
    bash "$D/build.sh"
    rm -f "$D/libdxmt_combined.a"
    xcrun -sdk iphoneos libtool -static -o "$D/libdxmt_combined.a" "$D"/obj/*.o "$T"/llvm-ios-build/lib/*.a
    ls -la "$D/libdxmt_combined.a"
}

# Madeira's d3d12-cube-x64.exe zeroes its pipeline desc and never sets
# RenderTargetWriteMask, so on D3D12 (and on Windows) it draws nothing but the
# clear colour. Rebuild it with colour writes enabled; stage-app.sh ships it.
step_tests() {
    # shellcheck disable=SC1091
    source "$ROOT/engine/wine/PIN"
    local TC="$T/$LLVM_MINGW/bin" W="$M/madeira-d3d12/tests/windows" B="$M/build/d3d12-tests"
    if [ ! -x "$TC/x86_64-w64-mingw32-clang" ]; then
        curl -fsSL -o "$T/llvm-mingw.tar.xz" "$LLVM_MINGW_URL"
        echo "$LLVM_MINGW_SHA256  $T/llvm-mingw.tar.xz" | shasum -a 256 -c -
        tar -C "$T" -xf "$T/llvm-mingw.tar.xz"
        rm "$T/llvm-mingw.tar.xz"
    fi
    mkdir -p "$B"
    perl -pe 's/^(\s*)(pd\.NumRenderTargets = 1;)/$1$2\n$1pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;/' \
        "$W/cube_window.c" > "$B/cube_window.c"
    grep -q 'RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL' "$B/cube_window.c"
    "$TC/x86_64-w64-mingw32-clang" -O2 -Wall -mwindows \
        -o "$B/d3d12-cube-x64.exe" "$B/cube_window.c" -I"$W" -luuid -lole32
    ls -la "$B/d3d12-cube-x64.exe"
}

step_collect() {
    mkdir -p "$OUT"
    cp "$M/build/dxmt-ios/libdxmt_combined.a" "$OUT/"
    cp "$M/build/d3d12-tests/d3d12-cube-x64.exe" "$OUT/"
    git -C "$M" rev-parse HEAD > "$OUT/MADEIRA_SHA"
    ls -la "$OUT"
}

case "${1:-all}" in
    fetch) step_fetch ;;
    llvm) step_llvm ;;
    metal) step_metal ;;
    dxmt) step_dxmt ;;
    tests) step_tests ;;
    collect) step_collect ;;
    all) step_fetch; step_llvm; step_metal; step_dxmt; step_tests; step_collect ;;
    *) echo "unknown step $1"; exit 2 ;;
esac
