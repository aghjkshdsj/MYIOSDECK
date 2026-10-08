#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage the OpenGL dylibs into the app build (run by the IPA workflow after Wine is staged):
# engine/gl/out from the "Stage 4 - OpenGL" workflow -> Madeira's app/Madeira/gl/, bundled
# as the app's gl/ folder, where WineProcessBridge.m finds them (MADEIRA_GL_DIR) and picks
# the Zink backend. Without this stage the GL driver uses Apple's OpenGL ES instead.
# The IPA packaging step signs gl/*.dylib like the other loose dylibs.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/engine/gl/out"
# Wine's stage-app.sh has already checked out Madeira here.
A="$ROOT/engine/wine/madeira/app/Madeira"

for f in libOSMesa.dylib libMoltenVK.dylib; do
    [ -s "$OUT/$f" ] || { echo "missing $OUT/$f (run the OpenGL workflow first)"; exit 1; }
done
[ -d "$A" ] || { echo "Wine is not staged; OpenGL needs it"; exit 1; }
WANT="$(git -C "$ROOT/engine/wine/madeira" rev-parse HEAD)"
HAVE="$(cat "$OUT/MADEIRA_SHA" 2>/dev/null || echo unknown)"
# The dylibs only have to match the OSMesa API opengl_ios.c uses, not this exact commit.
[ "$WANT" = "$HAVE" ] || echo "::warning::OpenGL dylibs built with Madeira $HAVE's scripts, Wine staged from $WANT"

mkdir -p "$A/gl"
cp "$OUT/libOSMesa.dylib" "$OUT/libMoltenVK.dylib" "$A/gl/"
cp "$OUT"/*.version "$A/gl/" 2>/dev/null || true
cp -R "$OUT/licenses" "$A/gl/licenses"
python3 - "$ROOT/project.yml" <<'PY'
import sys
p = sys.argv[1]
lines = ["      - path: engine/wine/madeira/app/Madeira/gl",
         "        type: folder",
         "        buildPhase: resources"]
s = open(p).read()
assert "# @GL_SOURCES@" in s
s = s.replace("      # @GL_SOURCES@", "\n".join(lines))
open(p, "w").write(s)
PY
ls -la "$A/gl"
