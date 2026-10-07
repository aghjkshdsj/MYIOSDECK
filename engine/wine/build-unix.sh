#!/bin/bash
# Stage 2: build Wine's unix side for iOS arm64 as static libraries.
#   engine/wine/build-unix.sh <step>
# Steps (run in order; each is resumable):
#   fetch      Madeira checkout + its Wine fork (shallow, pinned)
#   configure  wine/build-macos: config.h, generated IDL headers, host tools
#   deps       GnuTLS/Nettle/GMP, FFmpeg (LGPL config), FreeType for iOS
#   ntdll      libntdll_unix.a   (Madeira build/ntdll-unix)
#   win32u     libwin32u_unix.a  (Madeira build/win32u-unix)
#   wineserver libwineserver.a   (reproducible rebuild, see build_wineserver)
#   collect    copy the libraries to engine/wine/out
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/engine/wine"
# shellcheck disable=SC1091
source "$HERE/PIN"
M="$HERE/madeira"          # Madeira checkout; its scripts expect this layout
W="$M/wine"
OUT="$HERE/out"
JOBS="$(sysctl -n hw.ncpu)"
TC="$M/toolchains/$LLVM_MINGW/bin"
export PATH="$TC:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$(brew --prefix llvm)/bin:$PATH"

step_fetch() {
    if [ ! -d "$M/.git" ] || [ "$(git -C "$M" rev-parse HEAD)" != "$MADEIRA_SHA" ]; then
        rm -rf "$M"
        git init -q "$M"
        git -C "$M" remote add origin "$MADEIRA_REPO"
        git -C "$M" fetch -q --depth 1 origin "$MADEIRA_SHA"
        git -C "$M" checkout -q FETCH_HEAD
    fi
    if [ ! -d "$W/.git" ] && [ ! -f "$W/.git" ] || [ "$(git -C "$W" rev-parse HEAD 2>/dev/null)" != "$WINE_SHA" ]; then
        git -C "$M" submodule update --init --depth 1 wine
    fi
    git -C "$W" rev-parse HEAD
    if [ ! -x "$TC/aarch64-w64-mingw32-clang" ]; then
        mkdir -p "$M/toolchains"
        curl -fsSL -o "$M/toolchains/llvm-mingw.tar.xz" "$LLVM_MINGW_URL"
        echo "$LLVM_MINGW_SHA256  $M/toolchains/llvm-mingw.tar.xz" | shasum -a 256 -c -
        tar -C "$M/toolchains" -xf "$M/toolchains/llvm-mingw.tar.xz"
        rm "$M/toolchains/llvm-mingw.tar.xz"
    fi
    "$TC/aarch64-w64-mingw32-clang" --version | head -1
    if [ ! -d "$M/research/freetype" ]; then
        git clone -q --depth 1 --branch "$FREETYPE_TAG" https://github.com/freetype/freetype.git "$M/research/freetype"
    fi
}

step_configure() {
    mkdir -p "$W/build-macos"
    cd "$W/build-macos"
    if [ ! -f config.status ]; then
        # Native (host) tree: config.h, generated headers and host tools. The PE
        # cross compiler only has to be present for configure on aarch64.
        ../configure --without-x --disable-tests --without-freetype --without-gnutls \
            --without-gstreamer --without-sdl --without-cups --without-sane --without-pcap --without-krb5 \
            --without-opencl --without-vulkan --without-usb --without-v4l2 --without-netapi --without-capi \
            --without-gphoto --without-pcsclite --without-inotify --without-dbus --without-ffmpeg \
            > configure.log 2>&1 || { tail -60 configure.log; return 1; }
    fi
    # Madeira's tree was configured with GnuTLS present: bcrypt, secur32 and
    # crypt32 compile their GnuTLS backends only then. On iOS the library is
    # linked statically and Madeira's ios_gnutls_shim.h routes this dlopen to it.
    if ! grep -q 'MYIOSDECK gnutls' include/config.h; then
        printf '
/* MYIOSDECK gnutls */
#define HAVE_GNUTLS_CIPHER_INIT 1
#define SONAME_LIBGNUTLS "libgnutls.30.dylib"
' >> include/config.h
    fi
    grep -c '#define' include/config.h
    make -j"$JOBS" tools/all tools/winebuild/all tools/widl/all tools/wrc/all > make-tools.log 2>&1 ||
        { tail -40 make-tools.log; return 1; }
    make -j"$JOBS" include/all > make-include.log 2>&1 || { tail -40 make-include.log; return 1; }
    ls tools/winebuild/winebuild tools/widl/widl
}

step_deps() {
    [ -f "$M/toolchains/gnutls-ios/include/gnutls/gnutls.h" ] || bash "$M/build/gnutls-ios/build.sh"
    [ -f "$M/toolchains/ffmpeg-ios/include/libavcodec/avcodec.h" ] || bash "$M/build/ffmpeg/build.sh"
    [ -f "$M/build/freetype-ios/build/libfreetype.a" ] || bash "$M/build/freetype-ios/build.sh"
}

step_ntdll() {
    # No-JIT step B: map PE images from signed dylibs when WINE_IOS_NOJIT=1
    # (engine/wine/patches/nojit_dylib.py). Start from the pristine file: the checkout is cached.
    git -C "$M" checkout -- build/ntdll-unix/virtual_ios.c build/ntdll-unix/loader_ios.c build/ntdll-unix/process_ios.c \
        build/ntdll-unix/env_ios.c
    python3 "$HERE/patches/nojit_dylib.py" "$M"
    # putenv() must not keep stack/freed buffers: iOS has no exec (engine/wine/patches/putenv_lifetime.py).
    python3 "$HERE/patches/putenv_lifetime.py" "$M"
    # dwrite's unix side wants generated headers from the PE tree; the native
    # tree generates the same headers (include/all), so use that instead.
    sed -i '' 's#wine/build-arm64ec/include#wine/build-macos/include#g' "$M/build/ntdll-unix/build.sh"
    # ri_page_wait_time_mach is newer than the CI's iOS SDK; it only feeds a
    # diagnostic [xp] log line, so report 0 there.
    python3 - "$M/build/ntdll-unix/server_ios.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("XP_MS( ru.ri_page_wait_time_mach - pru.ri_page_wait_time_mach )", "0.0")
open(p, "w").write(s)
PY
    bash "$M/build/ntdll-unix/build.sh"
    test -s "$M/app/Madeira/libntdll_unix.a"
}

step_win32u() {
    bash "$M/build/win32u-unix/build.sh"
    test -s "$M/app/Madeira/libwin32u_unix.a"
}

# Madeira's build/wineserver/build.sh patches objects into a prebuilt archive
# that is not in its repository. Rebuild the whole archive from source with the
# same flags: every wine/server/*.c, Madeira's *_ios.c replacements where they
# exist, its extra objects, then the same symbol renames.
build_wineserver() {
    local B="$M/build/wineserver" O="$HERE/obj-wineserver"
    local SDK; SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
    rm -rf "$O" && mkdir -p "$O"
    local FLAGS=(-arch arm64 -isysroot "$SDK" -miphoneos-version-min=17.0 -O2
        -I"$W/include" -I"$W/include/wine" -I"$W/build-macos/include"
        -I"$B" -I"$W/server" -I"$M/build/ntdll-unix/shims"
        -I"$M/build/madsync" -DHAVE_LINUX_NTSYNC_H=1
        -include "$B/config_ios.h" -include stdarg.h -include "$B/unicode_fix.h"
        -include "$B/wineserver_ios_kill.h"
        -DBINDIR=\"/usr/local/bin\" -DDATADIR=\"/usr/local/share\"
        -D__WINESRC__ -DWINE_IOS=1 -Dmain=wineserver_main
        -Wno-implicit-function-declaration -Wno-int-conversion)
    local fail=0 src name repl
    for src in "$W"/server/*.c; do
        name="$(basename "$src" .c)"
        repl="$B/${name}_ios.c"
        # Madeira compiles these from the submodule even though *_ios.c exists for some.
        case "$name" in process) repl="" ;; esac
        [ -n "$repl" ] && [ -f "$repl" ] && src="$repl"
        xcrun -sdk iphoneos clang "${FLAGS[@]}" -c "$src" -o "$O/$name.o" 2> "$O/$name.err" ||
            { echo "FAILED $src"; head -30 "$O/$name.err"; fail=1; }
    done
    for extra in wine_log_ios hidpad_ios; do
        xcrun -sdk iphoneos clang "${FLAGS[@]}" -c "$B/$extra.c" -o "$O/$extra.o" 2> "$O/$extra.err" ||
            { echo "FAILED $extra"; head -30 "$O/$extra.err"; fail=1; }
    done
    xcrun -sdk iphoneos clang "${FLAGS[@]}" -c "$M/build/hidpad/hidparse_ios.c" -o "$O/hidparse_ios.o" 2> "$O/hidparse_ios.err" ||
        { echo "FAILED hidparse_ios"; head -30 "$O/hidparse_ios.err"; fail=1; }
    # The kill wrapper is compiled WITHOUT the kill macro header.
    xcrun -sdk iphoneos clang -arch arm64 -isysroot "$SDK" -miphoneos-version-min=17.0 -O2 -I"$B" -DWINE_IOS=1 \
        -Wno-implicit-function-declaration -c "$B/wineserver_ios_kill.c" -o "$O/wineserver_ios_kill.o" || fail=1
    [ "$fail" = 0 ] || return 1

    # Symbols that collide with win32u-unix / ntdll-unix in one process (see Madeira's script).
    local syms=(alloc_user_handle free_user_handle get_virtual_screen_rect destroy_thread_windows
        get_window_thread is_desktop_class is_message_class is_window_visible mirror_region
        send_notify_message shared_session user_shared_data)
    local args=() s f
    for s in "${syms[@]}"; do args+=(--redefine-sym "_${s}=_ws_${s}"); done
    for f in "$O"/*.o; do llvm-objcopy "${args[@]}" "$f"; done
    rm -f "$M/app/Madeira/libwineserver.a"
    xcrun --sdk iphoneos ar rcs "$M/app/Madeira/libwineserver.a" "$O"/*.o
    ls -la "$M/app/Madeira/libwineserver.a"
}

step_collect() {
    mkdir -p "$OUT"
    for l in libntdll_unix libwin32u_unix libwineserver; do
        if [ -f "$M/app/Madeira/$l.a" ]; then cp "$M/app/Madeira/$l.a" "$OUT/"; fi
    done
    for l in libgmp libgnutls libhogweed libnettle libavformat libavcodec libswresample libavutil; do
        if [ -f "$M/app/Madeira/$l.a" ]; then cp "$M/app/Madeira/$l.a" "$OUT/"; fi
    done
    git -C "$M" rev-parse HEAD > "$OUT/MADEIRA_SHA"
    ls -la "$OUT"
}

case "${1:-all}" in
    fetch) step_fetch ;;
    configure) step_configure ;;
    deps) step_deps ;;
    ntdll) step_ntdll ;;
    win32u) step_win32u ;;
    wineserver) build_wineserver ;;
    collect) step_collect ;;
    all) step_fetch; step_configure; step_deps; step_ntdll; step_win32u; build_wineserver; step_collect ;;
    *) echo "unknown step $1"; exit 2 ;;
esac
