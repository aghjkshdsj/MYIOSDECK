#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage 4: desktop OpenGL (3.3-4.1) for Windows games, as two iOS dylibs the winios GL
# driver (Madeira build/win32u-unix/opengl_ios.c, "zink" backend) dlopens from the
# app's gl/ folder:
#   libMoltenVK.dylib  Vulkan on Metal (MoltenVK + SPIRV-Cross, SPIRV-Tools, cereal)
#   libOSMesa.dylib    Mesa OSMesa + Zink (GL -> Vulkan), softpipe built but unused
# Built with Connor Gow's (c-gow) scripts and Mesa patches from the pinned Madeira fork.
#   engine/gl/build.sh <step>
#   fetch     Madeira (build/mesa-ios, build/moltenvk-ios only), Mesa, MoltenVK, Python venv
#   moltenvk  MoltenVK for iOS (fetchDependencies + make ios; cached)
#   mesa      Mesa OSMesa + Zink for iOS
#   audit     no CPU JIT: no LLVM, no code-generation imports, PROT_EXEC users reported
#   collect   engine/gl/out (dylibs, versions, licence texts, audit)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/engine/gl"
# shellcheck disable=SC1091
source "$ROOT/engine/wine/PIN"
# shellcheck disable=SC1091
source "$HERE/PIN"
M="$HERE/madeira"                  # sparse Madeira checkout: the build scripts write into its app/Madeira/gl
T="$HERE/work"                     # sources and build trees (cached)
MESA_SRC="$T/mesa-$MESA_VERSION"
MVK_SRC="$T/MoltenVK"
VENV="$T/mesa-venv"
GL="$M/app/Madeira/gl"
OUT="$HERE/out"

step_fetch() {
    if [ ! -d "$M/.git" ] || [ "$(git -C "$M" rev-parse HEAD)" != "$MADEIRA_SHA" ]; then
        rm -rf "$M"
        git init -q "$M"
        git -C "$M" remote add origin "$MADEIRA_REPO"
        git -C "$M" sparse-checkout set build/mesa-ios build/moltenvk-ios app/Madeira/gl app/Madeira/licenses
        git -C "$M" fetch -q --depth 1 --filter=blob:none origin "$MADEIRA_SHA"
        git -C "$M" checkout -q FETCH_HEAD
    fi
    ls "$M/build/mesa-ios/build.sh" "$M/build/moltenvk-ios/build.sh" "$M"/build/mesa-ios/patches/*.patch
    mkdir -p "$T"
    if [ ! -f "$MESA_SRC/meson.build" ]; then
        curl -fsSL -o "$T/mesa.tar.xz" "$MESA_URL"
        echo "$MESA_SHA256  $T/mesa.tar.xz" | shasum -a 256 -c -
        tar -C "$T" -xf "$T/mesa.tar.xz"
        rm "$T/mesa.tar.xz"
    fi
    if [ ! -f "$MVK_SRC/fetchDependencies" ]; then
        git clone -q --depth 1 -b "$MOLTENVK_TAG" "$MOLTENVK_REPO" "$MVK_SRC"
    fi
    git -C "$MVK_SRC" describe --tags --always
    if [ ! -x "$VENV/bin/meson" ]; then
        python3 -m venv "$VENV"
        # shellcheck disable=SC2086
        "$VENV/bin/pip" install -q $MESA_PY_DEPS
    fi
    "$VENV/bin/meson" --version
}

step_moltenvk() {
    MOLTENVK_SRC="$MVK_SRC" bash "$M/build/moltenvk-ios/build.sh"
    test -s "$GL/libMoltenVK.dylib"
}

step_mesa() {
    MESA_SRC="$MESA_SRC" MOLTENVK_SRC="$MVK_SRC" MESA_VENV="$VENV" bash "$M/build/mesa-ios/build.sh"
    test -s "$GL/libOSMesa.dylib"
    # Zink loads Vulkan by path (ZINK_VULKAN_LIBRARY, Mesa patch 0001). If the link also
    # recorded MoltenVK, point it next to libOSMesa: both live in the app's gl/ folder.
    local dep
    for dep in $(otool -L "$GL/libOSMesa.dylib" | awk 'NR > 1 { print $1 }' | grep -i moltenvk || true); do
        install_name_tool -change "$dep" @loader_path/libMoltenVK.dylib "$GL/libOSMesa.dylib"
    done
    otool -L "$GL/libOSMesa.dylib"
}

# Without JIT nothing may generate CPU code: Mesa is built without LLVM (no gallivm, no
# llvmpipe), and neither library may use the JIT APIs. Sources that ask for executable
# memory are listed with whether they were compiled, and their entry points with whether
# the dylib has them; on iOS without the JIT entitlement such a request fails anyway.
step_audit() {
    local A="$T/nojit-audit.txt" fail=0 f base sym
    {
        echo "# No-JIT audit of the OpenGL dylibs"
        echo
        echo "Mesa configuration:"
        grep -E "^ *(llvm|gallium-drivers|shader-cache)" "$M/build/mesa-ios/obj/meson-logs/meson-log.txt" 2>/dev/null | head -5 || true
        "$VENV/bin/meson" introspect --buildoptions "$M/build/mesa-ios/obj" 2>/dev/null |
            python3 -c 'import json,sys; o={x["name"]:x["value"] for x in json.load(sys.stdin)}; print("  llvm =", o.get("llvm"), "| gallium-drivers =", o.get("gallium-drivers"))' || true
        for f in "$GL/libOSMesa.dylib" "$GL/libMoltenVK.dylib"; do
            echo
            echo "$(basename "$f"): $(du -h "$f" | cut -f1)"
            otool -L "$f" | sed 's/^/  /'
            for sym in pthread_jit_write_protect_np pthread_jit_write_with_callback_np sys_icache_invalidate \
                       sys_dcache_flush mprotect vm_protect mach_vm_protect mmap; do
                if nm -u "$f" 2>/dev/null | grep -qx "_$sym"; then echo "  imports $sym"; fi
            done
            if nm "$f" 2>/dev/null | grep -qiE ' _?(LLVM|lp_build_|gallivm_)'; then echo "  LLVM/gallivm symbols: YES"; fi
        done
        echo
        echo "Mesa sources that request PROT_EXEC (compiled? -> object):"
        for f in $(grep -rl 'PROT_EXEC' "$MESA_SRC/src" --include='*.c' --include='*.cpp' --include='*.h' | sort); do
            base="$(basename "$f")"
            if find "$M/build/mesa-ios/obj" -name "*${base}.o" | grep -q .; then echo "  COMPILED  ${f#"$MESA_SRC/"}"; else echo "  not built ${f#"$MESA_SRC/"}"; fi
        done
        echo "Exec-memory entry points linked into libOSMesa (unstripped build, local symbols too):"
        nm "$M/build/mesa-ios/obj/src/gallium/targets/osmesa/libOSMesa.8.dylib" 2>/dev/null |
            grep -iE ' _(u_execmem_alloc|rtasm_exec_malloc|entry_generate|x86_init_func|translate_sse2_create|lp_build_[a-z_]*|gallivm_[a-z_]*)$' |
            sed 's/^/  /' || echo "  none"
        echo
        echo "MoltenVK sources that request PROT_EXEC:"
        grep -rl 'PROT_EXEC' "$MVK_SRC/MoltenVK" "$MVK_SRC/Common" --include='*.c' --include='*.cpp' --include='*.mm' --include='*.m' --include='*.h' 2>/dev/null |
            sed "s|^$MVK_SRC/|  |" || true
        echo "  (end)"
    } > "$A"
    cat "$A"
    for f in "$GL/libOSMesa.dylib" "$GL/libMoltenVK.dylib"; do
        for sym in pthread_jit_write_protect_np pthread_jit_write_with_callback_np sys_icache_invalidate; do
            if nm -u "$f" 2>/dev/null | grep -qx "_$sym"; then echo "::error::$(basename "$f") imports $sym (CPU code generation)"; fail=1; fi
        done
        if nm "$f" 2>/dev/null | grep -qE ' _(LLVMCreateExecutionEngineForModule|LLVMOrcCreateLLJIT|gallivm_create)'; then
            echo "::error::$(basename "$f") contains LLVM's JIT"; fail=1
        fi
    done
    return $fail
}

step_collect() {
    rm -rf "$OUT" && mkdir -p "$OUT/licenses"
    cp "$GL/libOSMesa.dylib" "$GL/libMoltenVK.dylib" "$OUT/"
    cp "$GL"/*.version "$OUT/" 2>/dev/null || true
    local L="$M/app/Madeira/licenses" f
    for f in Mesa-license.rst MoltenVK-Apache-2.0.txt SPIRV-Cross-Apache-2.0.txt SPIRV-Tools-Apache-2.0.txt cereal-BSD-3-Clause.txt; do
        cp "$L/$f" "$OUT/licenses/"
    done
    cp "$MVK_SRC/External/Vulkan-Headers/LICENSE.md" "$OUT/licenses/Vulkan-Headers-LICENSE.md" 2>/dev/null || true
    cp "$T/nojit-audit.txt" "$OUT/" 2>/dev/null || true
    git -C "$M" rev-parse HEAD > "$OUT/MADEIRA_SHA"
    ls -la "$OUT" "$OUT/licenses"
}

case "${1:-all}" in
    fetch) step_fetch ;;
    moltenvk) step_moltenvk ;;
    mesa) step_mesa ;;
    audit) step_audit ;;
    collect) step_collect ;;
    all) step_fetch; step_moltenvk; step_mesa; step_audit; step_collect ;;
    *) echo "unknown step $1"; exit 2 ;;
esac
