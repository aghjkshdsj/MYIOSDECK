#!/bin/bash
# Wine builtin modules that Madeira's ARM64EC farm (app/Madeira/arm64ec-windows, the 64-bit side
# and every 64-bit game's system32) does not ship, built from the same Wine fork with the tree
# engine/dxmt/build-pe.sh configured (its `wine` step), the way Madeira's own
# build/wine-pe/build-modules.sh adds a builtin. Wine links builtins with 64 KB sections, so each
# one becomes a signed dylib like the rest of the farm (build-ipa.yml converts the whole farm).
#
# Only NEW names: a module the farm already has is never rebuilt or replaced (the games that run
# today keep Madeira's binaries), and a game that needs none of these loads nothing new. A game that
# imports one of them failed before with c0000135 at load, or (a delay load / LoadLibrary) took a
# fallback path, so adding the file only changes what used to fail.
#
#   engine/wine/build-pe-extras.sh [out dir]      (default engine/dxmt/out-pe, the d3d-pe-dlls
#                                                  artifact build-ipa.yml copies into the farm)
# A module that does not build is reported and skipped; the others still ship.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1091
source "$ROOT/engine/wine/PIN"
M="$ROOT/engine/wine/madeira"
W="$M/wine"
B="$W/build-arm64ec"
TC="$M/toolchains/$LLVM_MINGW/bin"
FARM="$M/app/Madeira/arm64ec-windows"
OUT="${1:-$ROOT/engine/dxmt/out-pe}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
export PATH="$TC:$(brew --prefix bison 2>/dev/null)/bin:$(brew --prefix flex 2>/dev/null)/bin:$PATH"
STRIP="$TC/arm64ec-w64-mingw32-strip"

# name=why. Each is a stock Wine module (dlls/<name>) the i386 farm already has (wine-i386.yml
# builds every module), so 32-bit games had it and 64-bit games did not.
EXTRAS=(
  "wintypes=IL2CPP Unity games: GameAssembly.dll imports api-ms-win-core-winrt-robuffer-l1-1-0 (RoGetBufferMarshaler), which the apiset maps here; without it 'Failed to load il2cpp' (Vampire Survivors, build 120)"
  "twinapi.appcore=WinRT app-core classes some Unity/IL2CPP and UWP-ported games import"
  "msvcp120=Visual C++ 2013 runtime (msvcr120 was already there, msvcp120 was not); Steam's VC redist installer never runs here"
  "msvcr100=Visual C++ 2010 runtime"
  "msvcp100=Visual C++ 2010 runtime"
  "msvcr90=Visual C++ 2008 runtime"
  "msvcp90=Visual C++ 2008 runtime"
  "msvcr80=Visual C++ 2005 runtime"
  "msvcp80=Visual C++ 2005 runtime"
  "vcomp=OpenMP runtime"
  "vcomp100=OpenMP runtime (VC 2010)"
  "vcomp110=OpenMP runtime (VC 2012)"
  "vcomp120=OpenMP runtime (VC 2013)"
  "vcomp140=OpenMP runtime (VC 2015-2022)"
  "xaudio2_8=XAudio 2.8 (Windows 8 inbox; FAudio inside Wine): UE4 and FNA/MonoGame games import it"
  "xaudio2_9=XAudio 2.9 (Windows 10 inbox): UE4/UE5, FNA/MonoGame and many recent games import it"
  "d3d10=Direct3D 10 front end (devices come from DXMT's d3d10core)"
  "d3d10_1=Direct3D 10.1 front end (DXMT's d3d10core)"
  "d3dx10_43=D3DX 10 helpers"
  "d3dx11_42=D3DX 11 helpers (d3dx11_43 was there)"
  "d3dx9_36=D3DX 9 helpers (d3dx9_43 was there; older SDKs link these names)"
  "d3dx9_38=D3DX 9 helpers"
  "d3dx9_39=D3DX 9 helpers"
  "d3dx9_40=D3DX 9 helpers"
  "d3dx9_41=D3DX 9 helpers"
  "d3dx9_42=D3DX 9 helpers"
  "d3dcompiler_46=HLSL compiler (Windows 8 SDK name)"
  "d2d1=Direct2D (on Direct3D 10.1 and DirectWrite); UI layers and some engines import it"
  "shfolder=SHGetFolderPath forwarder older games import"
  "msvfw32=Video for Windows (older games' video playback)"
  "avifil32=AVI file API (older games' video playback)"
  "gameux=Games Explorer API some Windows Vista-era games import"
)

mkdir -p "$OUT"
[ -x "$STRIP" ] || { echo "llvm-mingw not found at $TC" >&2; exit 1; }
[ -f "$B/config.status" ] || { echo "no configured ARM64EC tree at $B (run engine/dxmt/build-pe.sh wine first)" >&2; exit 1; }
# widl looks for stdole2.tlb under aarch64-windows/ when it builds an ARM64EC typelib import
# (Madeira's build-modules.sh does the same); this tree builds it into arm64ec-windows/.
mkdir -p "$B/dlls/stdole2.tlb"
[ -e "$B/dlls/stdole2.tlb/aarch64-windows" ] || ln -s arm64ec-windows "$B/dlls/stdole2.tlb/aarch64-windows"

built=() failed=() skipped=()
for e in "${EXTRAS[@]}"; do
    m="${e%%=*}"
    case "$m" in *.drv|*.acm|*.cpl|*.exe|*.ocx|*.tlb) f="$m" ;; *) f="$m.dll" ;; esac   # twinapi.appcore -> .dll
    if [ -n "$(find "$FARM" -maxdepth 1 -iname "$f" -print -quit 2>/dev/null)" ]; then
        skipped+=("$f"); continue                             # Madeira ships it: never replaced
    fi
    if [ ! -f "$W/dlls/$m/Makefile.in" ]; then
        echo "::warning::$m: no dlls/$m in this Wine"; failed+=("$f"); continue
    fi
    t="dlls/$m/arm64ec-windows/$f"
    if make -C "$B" -j"$JOBS" "$t" > "$B/extras-$m.log" 2>&1; then
        cp "$B/$t" "$OUT/$f.tmp" && "$STRIP" --strip-debug "$OUT/$f.tmp" && mv "$OUT/$f.tmp" "$OUT/$f"
        built+=("$f")
    else
        echo "::warning::$m did not build (tail of $B/extras-$m.log):"; tail -15 "$B/extras-$m.log"
        failed+=("$f")
    fi
done

echo "## Wine modules added to the ARM64EC farm"
echo "- built (${#built[@]}): ${built[*]:-none}"
echo "- already in Madeira's farm, kept (${#skipped[@]}): ${skipped[*]:-none}"
echo "- not built (${#failed[@]}): ${failed[*]:-none}"
# Imports none of the farm, the extras or the apiset provide: such a module would still fail to load
# (no worse than before), and the name is the next one to add.
for f in "${built[@]}"; do
    for imp in $(llvm-readobj --coff-imports "$OUT/$f" 2>/dev/null | sed -n 's/^ *Name: \(.*\.[dD][lL][lL]\)$/\1/p' | sort -u); do
        case "$(echo "$imp" | tr 'A-Z' 'a-z')" in api-ms-*|ext-ms-*) continue ;; esac
        if [ -z "$(find "$FARM" "$OUT" -maxdepth 1 -iname "$imp" -print -quit)" ]; then
            echo "- note: $f imports $imp, which neither the farm nor the extras have"
        fi
    done
done
exit 0
