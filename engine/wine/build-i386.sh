#!/bin/bash
# The 32-bit (i386) Windows DLL farm for WoW64 sessions (docs/NO_JIT_WOW64.md, stage 1):
# bundled as i386-windows, linked by Madeira's launch path as C:\windows\syswow64. Every file is
# x86 code, i.e. data inside a 32-bit process's guest window, decoded by the interpreter (or by
# FEX with JIT): nothing here becomes a signed dylib, so the default 4 KB sections are fine.
#
# Madeira's build/wine-i386/build.sh, with this repository's paths (the Wine stage's checkout,
# engine/wine/madeira) and DXMT built the way engine/dxmt/build-pe.sh builds the ARM64EC set:
#   wine     every module the configured i386 tree has a rule for, minus the SKIP list (a
#            32-bit program that imports one missing DLL never starts, and there is no way to
#            find the gap but running it: so the farm is everything, not a hand-made list)
#   dxmt     DXMT's i386 d3d11/dxgi/d3d10core/winemetal, the D3D9 shim (d3d9.dll, d3d9shim.dll)
#            and the emulated D3D9 frontend (d3d9-emulated.dll), against that tree
#   extras   Madeira's hello-x86.exe (tests/x86: kernel32 only, prints a line, exits 42)
#   a64      the WoW64 host side's wow64win.dll (aarch64), patched to keep atoms (step_a64)
#   collect  strip into engine/wine/out-i386/i386-windows; fails on a missing import
#
#   engine/wine/build-i386.sh <step>     fetch | wine | dxmt | extras | a64 | collect | all
# macOS host (Wine's configure and DXMT's Metal shaders, as Madeira builds it).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/engine/wine/PIN"
DXMT_PIN_SHA="$(sed -n 's/^DXMT_SHA=//p' "$ROOT/engine/dxmt/PIN")"
M="$ROOT/engine/wine/madeira"     # Madeira + its Wine fork + llvm-mingw (build-unix.sh fetch)
W="$M/wine"
B="$W/build-i386"
TC="$M/toolchains/$LLVM_MINGW/bin"
OUT="$ROOT/engine/wine/out-i386"
FARM="$OUT/i386-windows"
JOBS="$(sysctl -n hw.ncpu)"
export PATH="$TC:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"

step_fetch() {
    bash "$ROOT/engine/wine/build-unix.sh" fetch
    if [ "$(git -C "$M/dxmt" rev-parse HEAD 2>/dev/null)" != "$DXMT_PIN_SHA" ]; then
        git -C "$M" submodule update --init --depth 1 dxmt
    fi
    git -C "$M/dxmt" rev-parse HEAD
    git -C "$M/dxmt" submodule update --init --depth 1 --recursive
    "$TC/i686-w64-mingw32-clang" --version | head -1
}

# What goes in: Madeira's policy, each skipped name with its reason.
SKIP_REASON=(
  "cng.sys|fltmgr.sys|hidclass.sys|hidparse.sys|http.sys|ksecdd.sys|mouhid.sys|mountmgr.sys|ndis.sys|netio.sys|nsiproxy.sys|scsiport.sys|tdi.sys|usbd.sys|winebth.sys|winebus.sys|winehid.sys|winexinput.sys|wmilib.sys=kernel drivers: under WoW64 drivers are 64-bit only"
  "ntoskrnl.exe|winedevice.exe=the driver world (see .sys)"
  "xtajit.dll|xtajit64.dll|wow64.dll|wow64win.dll|wow64cpu.dll=64-bit side of WoW64, never i386"
  "winemac.drv=macOS display driver; the display goes through win32u + Winios"
  "wineps.drv=PostScript/CUPS; no CUPS unix side on iOS"
  "winevulkan.dll|vulkan-1.dll=tree is --without-vulkan; graphics go through DXMT"
  "opencl.dll|wpcap.dll=wrappers over host unix libraries that are not built for iOS"
  "wow32.dll|winevdm.exe|vga.dll|hal.dll|w32skrnl.dll=16-bit layer; no 16-bit modules in a WoW64 tree"
  "winemenubuilder.exe=writes host desktop menu entries; there is no host desktop"
  "wineconsole.exe=host console; conhost is the WoW64-side console"
  "ir50_32.dll=needs GStreamer's codec; the winegstreamer unix side here is FFmpeg-based"
  "aero.msstyles=7.4 MiB of theme data nothing in the prefix selects"
  "winedbg.exe=4.5 MiB debugger only the (unshown) crash dialog spawns; dbghelp.dll ships"
  "d3d11.dll|dxgi.dll|d3d10core.dll|winemetal.dll=DXMT's (step dxmt); Wine's are wined3d frontends with no backend here"
  "d3d9.dll=DXMT's: the shim and the emulated frontend (step dxmt)"
)
SKIP=()
for e in "${SKIP_REASON[@]}"; do IFS='|' read -r -a n <<< "${e%%=*}"; SKIP+=("${n[@]}"); done
is_in() { local x="$1"; shift; for y in "$@"; do [ "$x" = "$y" ] && return 0; done; return 1; }

step_wine() {
    mkdir -p "$B"
    cd "$B"
    if [ ! -f config.status ]; then
        # Madeira's flags. --enable-winegstreamer: the PE half never touches GStreamer, but
        # configure drops the module without GStreamer's development files.
        ../configure --enable-archs=i386 --without-x --without-vulkan --without-freetype --without-gnutls \
            --disable-tests --enable-winegstreamer > configure.log 2>&1 || { tail -60 configure.log; return 1; }
    fi
    make -j"$JOBS" tools/all tools/winebuild/all tools/widl/all tools/wrc/all \
        > make-tools.log 2>&1 || { tail -40 make-tools.log; return 1; }
    local ext='\.(dll|exe|drv|cpl|acm|ax|ocx|tlb|msstyles|com)$' t b targets=()
    # 16-bit modules (.dll16/.exe16/...) fall out of the pattern: WoW64 has no NE loader.
    while IFS= read -r t; do
        b="$(basename "$t")"
        is_in "$b" "${SKIP[@]}" && continue
        targets+=("$t")
    done < <(grep -oE '^(dlls|programs)/[^/]+/i386-windows/[^/ :]+' Makefile | grep -E "$ext" | sort -u)
    [ ${#targets[@]} -gt 0 ] || { echo "no i386 targets in the Makefile" >&2; return 2; }
    echo "== ${#targets[@]} i386 modules (skipped by policy: ${#SKIP[@]} names) =="
    printf '%s\n' "${targets[@]}" > targets.txt
    make -k -j"$JOBS" "${targets[@]}" > make-i386.log 2>&1 || true
    local failed=()
    for t in "${targets[@]}"; do
        [ -f "$t" ] && continue
        if ! make -j1 "$t" >> make-i386.log 2>&1 || [ ! -f "$t" ]; then failed+=("$t"); fi
    done
    if [ ${#failed[@]} -gt 0 ]; then
        printf 'FAIL %s\n' "${failed[@]}"
        grep -E 'error:|Error [0-9]' make-i386.log | head -40
        echo "== ${#failed[@]} modules failed (log: $B/make-i386.log) ==" >&2
        return 1
    fi
    echo "== all ${#targets[@]} i386 modules built =="
}

# Bump when the options change: the build directory may be cached and meson keeps its options.
DXMT_BUILD="build-pe-i386-v1"

step_dxmt() {
    local D="$M/dxmt" X="$M/dxmt/build-i386-win.txt"
    cat > "$X" <<EOF
[binaries]
c = '$TC/i686-w64-mingw32-clang'
cpp = '$TC/i686-w64-mingw32-clang++'
ar = '$TC/i686-w64-mingw32-ar'
strip = '$TC/i686-w64-mingw32-strip'
windres = '$TC/i686-w64-mingw32-windres'
dlltool = '$TC/i686-w64-mingw32-dlltool'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
    if [ ! -f "$D/$DXMT_BUILD/build.ninja" ]; then
        meson setup "$D/$DXMT_BUILD" "$D" --cross-file "$X" -Dbuildtype=release \
            -Dwine_build_path="$B" -Dwine_builtin_dll=true
    fi
    ninja -C "$D/$DXMT_BUILD" src/winemetal/winemetal.dll src/d3d11/d3d11.dll src/d3d10/d3d10core.dll \
        src/dxgi/dxgi.dll src/d3d9/d3d9.dll src/d3d9shim/d3d9shim.dll
}

step_extras() {
    mkdir -p "$OUT/extras"
    # Madeira tests/x86/build.sh hello-x86: no CRT, kernel32 only.
    i686-w64-mingw32-clang -O2 -nostdlib -Wl,--entry=_start -o "$OUT/extras/hello-x86.exe" \
        "$M/tests/x86/hello-x86.c" -lkernel32
    llvm-readobj --file-headers --coff-imports "$OUT/extras/hello-x86.exe" | grep -E 'Machine|Name:' || true
}

# The WoW64 host side's wow64win.dll (aarch64), rebuilt from the same Wine fork with
# engine/wine/patches/wow64win_atoms.py: atoms and resource IDs pass its thunks unchanged
# (Madeira's prebuilt copy turns the dialog class 0x8002 into a pointer, and win32u faults on
# it). 16 KB sections, so pe2dylib converts it like the rest of the aarch64 farm; build-ipa.yml
# puts it over Madeira's copy. Its exports and imports must equal the prebuilt one's.
A="$W/build-a64"
A64_OUT="$OUT/aarch64-windows"
step_a64() {
    python3 "$ROOT/engine/wine/patches/wow64win_atoms.py" "$W"
    mkdir -p "$A"
    cd "$A"
    if [ ! -f config.status ]; then
        ../configure --enable-archs=aarch64 --with-wine-tools="$B" --without-x --without-vulkan --without-freetype \
            --without-gnutls --disable-tests aarch64_LDFLAGS="-Wl,--section-alignment=0x4000" \
            > configure.log 2>&1 || { tail -60 configure.log; return 1; }
    fi
    local t=dlls/wow64win/aarch64-windows/wow64win.dll
    make -j"$JOBS" "$t" > make-a64.log 2>&1 || { grep -E 'error:|Error [0-9]' make-a64.log | head -40; return 1; }
    mkdir -p "$A64_OUT"
    aarch64-w64-mingw32-strip --strip-debug -o "$A64_OUT/wow64win.dll" "$t"
    local ref="$M/app/Madeira/aarch64-windows/wow64win.dll" diffs=0 what
    [ -f "$ref" ] || { echo "no prebuilt wow64win.dll at $ref to compare with" >&2; return 1; }
    llvm-readobj --file-headers "$A64_OUT/wow64win.dll" | grep -E 'Machine|SectionAlignment'
    for what in coff-exports coff-imports; do
        if ! diff <(llvm-readobj --"$what" "$ref" | grep -E '^ *Name:' | sort) \
                  <(llvm-readobj --"$what" "$A64_OUT/wow64win.dll" | grep -E '^ *Name:' | sort); then
            echo "wow64win.dll: $what differ from Madeira's prebuilt copy" >&2
            diffs=1
        fi
    done
    [ "$diffs" = 0 ]
    echo "== wow64win.dll (aarch64, atoms kept): $(wc -c < "$A64_OUT/wow64win.dll" | tr -d ' ') bytes, exports and imports as Madeira's =="
}

step_collect() {
    local t b f imp l missing=0 n=0
    rm -rf "$FARM"
    mkdir -p "$FARM"
    while IFS= read -r t; do
        b="$(basename "$t")"
        case "$b" in
            *.tlb) cp -f "$B/$t" "$FARM/$b" ;;
            *) i686-w64-mingw32-strip --strip-debug -o "$FARM/$b" "$B/$t" ;;
        esac
        n=$((n + 1))
    done < "$B/targets.txt"
    echo "== $n Wine modules =="
    local DB="$M/dxmt/$DXMT_BUILD/src"
    for f in winemetal/winemetal.dll d3d11/d3d11.dll d3d10/d3d10core.dll dxgi/dxgi.dll; do
        i686-w64-mingw32-strip -o "$FARM/$(basename "$f")" "$DB/$f"
    done
    # The shim is what programs load as d3d9.dll; the emulated frontend (meson's d3d9.dll)
    # ships beside it as d3d9-emulated.dll (Madeira docs/WOW64.md, Direct3D 9).
    i686-w64-mingw32-strip -o "$FARM/d3d9.dll" "$DB/d3d9shim/d3d9shim.dll"
    i686-w64-mingw32-strip -o "$FARM/d3d9shim.dll" "$DB/d3d9shim/d3d9shim.dll"
    i686-w64-mingw32-strip -o "$FARM/d3d9-emulated.dll" "$DB/d3d9/d3d9.dll"
    [ -f "$OUT/extras/hello-x86.exe" ] && cp "$OUT/extras/hello-x86.exe" "$FARM/"
    # Every DLL an installed module imports must be in the farm (api-ms-win-* resolve through
    # apisetschema.dll).
    local present
    present="$(ls "$FARM" | tr '[:upper:]' '[:lower:]')"
    for f in "$FARM"/*; do
        case "$f" in *.tlb|*.msstyles) continue ;; esac
        while IFS= read -r imp; do
            l="$(tr '[:upper:]' '[:lower:]' <<< "$imp")"
            case "$l" in api-ms-win-*|ext-ms-win-*) continue ;; esac
            grep -qxF "$l" <<< "$present" && continue
            echo "missing import: $(basename "$f") -> $imp"; missing=$((missing + 1))
        done < <(llvm-objdump -p "$f" 2>/dev/null | sed -n 's/^ *DLL Name: //p')
    done
    echo "== $(ls "$FARM" | wc -l | tr -d ' ') files, $(du -sh "$FARM" | cut -f1), $missing missing imports =="
    llvm-readobj --file-headers "$FARM/ntdll.dll" "$FARM/kernel32.dll" | grep -E 'File:|Machine' || true
    [ "$missing" = 0 ]
}

case "${1:-all}" in
    fetch) step_fetch ;;
    wine) step_wine ;;
    dxmt) step_dxmt ;;
    extras) step_extras ;;
    a64) step_a64 ;;
    collect) step_collect ;;
    all) step_fetch; step_wine; step_dxmt; step_extras; step_a64; step_collect ;;
    *) echo "unknown step $1" >&2; exit 2 ;;
esac
