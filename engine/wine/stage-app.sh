#!/bin/bash
# Stage Wine into the app build (run by the IPA workflow before xcodegen):
#   - static libraries from the "Stage 2 - Wine unix side" workflow (engine/wine/out)
#   - Madeira's app-side Wine sources (bridge, iOS display driver, input) compiled in place
#   - the prebuilt Windows side: ARM64EC DLL farm, NLS tables, prefix template
# Writes Config/Wine.xcconfig and App/Generated/wine_version.h.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/engine/wine"
# shellcheck disable=SC1091
source "$HERE/PIN"
M="$HERE/madeira"
LIBS="$HERE/out"

for l in libntdll_unix libwin32u_unix libwineserver; do
    [ -s "$LIBS/$l.a" ] || { echo "missing $LIBS/$l.a (run the Wine unix workflow first)"; exit 1; }
done

# App files and resources only: no submodules needed here.
if [ ! -d "$M/.git" ] || [ "$(git -C "$M" rev-parse HEAD)" != "$MADEIRA_SHA" ]; then
    rm -rf "$M"
    git init -q "$M"
    git -C "$M" remote add origin "$MADEIRA_REPO"
    git -C "$M" fetch -q --depth 1 origin "$MADEIRA_SHA"
    git -C "$M" checkout -q FETCH_HEAD
fi
A="$M/app/Madeira"
ls "$A/arm64ec-windows/ntdll.dll" "$A/arm64ec-windows/xtajit64.dll" "$A/prefix-template.tar.gz" > /dev/null

# No-JIT step B: with MYIOSDECK_NOJIT_EMULATOR set, system32\xtajit64.dll points at the stub
# emulator (engine/pedylib/emu) instead of FEX, which needs JIT.
python3 - "$A/WineProcessBridge.m" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
mark = "MYIOSDECK_NOJIT_EMULATOR"
anchor = 'dprintf(STDERR_FILENO, "[WineProc] Symlinked %d DLLs from %s -> sys32\\n", linked, bundle_subdir);\n'
if mark not in s:
    assert s.count(anchor) == 1, "WineProcessBridge.m anchor"
    s = s.replace(anchor, anchor + r'''            {
                const char *emu = getenv("MYIOSDECK_NOJIT_EMULATOR");
                if (emu && *emu) {
                    NSString *dst = [sys32Dir stringByAppendingPathComponent:@"xtajit64.dll"];
                    [fm removeItemAtPath:dst error:nil];
                    BOOL ok = [fm createSymbolicLinkAtPath:dst withDestinationPath:[NSString stringWithUTF8String:emu] error:nil];
                    dprintf(STDERR_FILENO, "[WineProc] no-JIT: xtajit64.dll -> %s (%s)\n", emu, ok ? "ok" : "FAILED");
                }
            }
''')
    open(p, "w").write(s)
PY

# 32-bit games without JIT (docs/NO_JIT_WOW64.md): with MYIOSDECK_NOJIT_WOW_CPU set, wow64.dll's
# CPU module (system32\xtajit.dll, FEX with JIT) is our FXI32 front end (engine/pedylib/emu).
python3 - "$A/WineProcessBridge.m" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
mark = "MYIOSDECK_NOJIT_WOW_CPU"
anchor = 'dprintf(STDERR_FILENO, "[WineProc] Symlinked %d DLLs from %s -> sys32\\n", linked, bundle_subdir);\n'
if mark not in s:
    assert s.count(anchor) == 1, "WineProcessBridge.m anchor"
    s = s.replace(anchor, anchor + r'''            {
                const char *wow = getenv("MYIOSDECK_NOJIT_WOW_CPU");
                if (wow && *wow) {
                    NSString *dst = [sys32Dir stringByAppendingPathComponent:@"xtajit.dll"];
                    [fm removeItemAtPath:dst error:nil];
                    BOOL ok = [fm createSymbolicLinkAtPath:dst withDestinationPath:[NSString stringWithUTF8String:wow] error:nil];
                    dprintf(STDERR_FILENO, "[WineProc] no-JIT: xtajit.dll -> %s (%s)\n", wow, ok ? "ok" : "FAILED");
                }
            }
''')
    open(p, "w").write(s)
PY

# Game-mode GDI windows (a launcher's dialog) are drawn in Winios's overlay view, added to the
# app window when the first one appears. The app's game view (GameHostView, black, under a
# SwiftUI full-screen cover) then went above it: the launcher was drawn but hidden behind black
# (Steins;Gate). winios_game_overlay_show(shown, window) puts the overlay in the game view's
# window above it (touches still fall through to the game view: it takes none), or hides it.
python3 - "$A/Winios/Winios.m" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
mark = "winios_game_overlay_show"
if mark not in s:
    assert "static UIView *g_compositor_view;" in s and "static BOOL g_comp_game;" in s, "Winios.m statics"
    s += r'''

/* MYIOSDECK (engine/wine/stage-app.sh): the game-mode window overlay above the app's game view
 * while it is shown (moved into `window` when given), hidden while it is not. Main thread. */
void winios_game_overlay_show(int shown, void *window) {
    if (!g_compositor_view || !g_comp_game) return;
    UIWindow *w = (__bridge UIWindow *)window;
    if (shown && w && g_compositor_view.superview != w) [w addSubview:g_compositor_view];
    g_compositor_view.hidden = shown ? NO : YES;
    if (shown && g_compositor_view.superview) [g_compositor_view.superview bringSubviewToFront:g_compositor_view];
}
'''
    open(p, "w").write(s)
PY

# The game view shows the client area of the window a game presents in, which is not the
# desktop's corner when that window has a frame: Crab Game (Unity windowed, 1566x752 at 0,0)
# draws from 3,29, and Steins;Gate's 1286x752 window holds a 1280x720 picture the view stretches
# to the desktop's width, so taps and the drawn cursor missed what they pointed at. Winios keeps
# game-mode windows' frames but not their client rects; this keeps the visible ones, and
# winios_game_picture_rect() answers the presenting window's for GameInput.swift (main thread):
# a window presenting through Metal (64-bit DXMT marks it), else a visible window as large as the
# game view's drawable (DXMT sizes it to the back buffer; the 32-bit path marks no window) and at
# least a quarter of the desktop, so a launcher's dialog never counts.
python3 - "$A/Winios/Winios.m" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
mark = "winios_game_picture_rect"
decl = "static NSMutableSet<NSNumber *> *g_game_metal;"
frame = ("        if (g_comp_game) {   /* game-mode windows, see winios_game_window_shown */\n"
         "            g_px_rects[key] = [NSValue valueWithCGRect:CGRectMake(x, y, w, h)];\n")
if mark not in s:
    assert s.count(decl) == 1 and s.count(frame) == 1, "Winios.m game-window anchors"
    i = s.index("\n", s.index(decl)) + 1
    s = s[:i] + r'''
/* MYIOSDECK (engine/wine/stage-app.sh): game mode, visible top-level hwnd -> client rect in
 * desktop pixels. */
static NSMutableDictionary<NSNumber *, NSValue *> *g_game_clients;
static void winios_note_game_client(NSNumber *key, int visible, int cx, int cy, int cw, int ch) {
    if (!g_game_clients) g_game_clients = [NSMutableDictionary new];
    if (visible && cw > 0 && ch > 0) g_game_clients[key] = [NSValue valueWithCGRect:CGRectMake(cx, cy, cw, ch)];
    else [g_game_clients removeObjectForKey:key];
}
static CGRect winios_largest(CGRect a, CGRect b) {
    if (CGRectIsNull(a)) return b;
    return b.size.width * b.size.height > a.size.width * a.size.height ? b : a;
}
/* The presenting window's client rect, 0 when there is none. dw x dh: the game view's drawable. */
int winios_game_picture_rect(int dw, int dh, int *x, int *y, int *w, int *h) {
    CGRect metal = CGRectNull, sized = CGRectNull;
    int sw = 0, sh = 0;
    winios_screen_size(&sw, &sh);
    for (NSNumber *key in g_game_clients) {
        CGRect r = g_game_clients[key].CGRectValue;
        if ([g_game_metal containsObject:key]) metal = winios_largest(metal, r);
        else if ((int)r.size.width == dw && (int)r.size.height == dh && 4.0 * dw * dh >= (double)sw * sh)
            sized = winios_largest(sized, r);
    }
    CGRect best = CGRectIsNull(metal) ? sized : metal;
    if (CGRectIsNull(best)) return 0;
    *x = (int)best.origin.x; *y = (int)best.origin.y;
    *w = (int)best.size.width; *h = (int)best.size.height;
    return 1;
}
''' + s[i:]
    s = s.replace(frame, frame + "            winios_note_game_client(key, visible, cx, cy, cw, ch);   /* MYIOSDECK: winios_game_picture_rect */\n")
    open(p, "w").write(s)
PY

LINK=""
for f in "$LIBS"/*.a; do LINK="$LINK \$(SRCROOT)/${f#"$ROOT/"}"; done
mkdir -p "$ROOT/Config" "$ROOT/App/Generated"
cat > "$ROOT/Config/Wine.xcconfig" <<X
// Generated by engine/wine/stage-app.sh — do not edit.
MYIOSDECK_WINE_HEADERS = \$(SRCROOT)/engine/wine/madeira/app/Madeira
MYIOSDECK_WINE_LIBS = $LINK -lz -framework IOKit -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework AudioToolbox -framework AVFoundation -framework CoreHaptics -framework ImageIO -framework GameController -framework Metal -framework QuartzCore
X
cat > "$ROOT/App/Generated/wine_version.h" <<H
// Generated by engine/wine/stage-app.sh
#define MYIOSDECK_WITH_WINE 1
#define MYIOSDECK_MADEIRA_SHA "${MADEIRA_SHA}"
H
# Add the sources and resources to the XcodeGen spec (CI working copy only).
python3 - "$ROOT/project.yml" <<'PY'
import sys
p = sys.argv[1]
base = "engine/wine/madeira/app/Madeira/"
srcs = ["WineProcessBridge.m", "WineServerBridge.m", "IOSDisplayShim.m", "PadOutput.m",
        "PrefixExtractor.c", "wine_stubs.c", "Winios/Winios.m", "Winios/WiniosGamepad.c",
        "Winios/WiniosCursor.c"]
lines = [f"      - path: {base}{f}" for f in srcs]
# aarch64-windows and i386-windows: 32-bit games (WoW64, docs/NO_JIT_WOW64.md). Madeira's launch
# path takes a 32-bit target's session from the aarch64 farm and links syswow64 to the i386 farm
# (filled by build-ipa.yml from wine-i386.yml; without it, i386-windows holds only .gitkeep and
# no target is treated as 32-bit).
for d in ["arm64ec-windows", "aarch64-windows", "i386-windows", "nls"]:
    lines += [f"      - path: {base}{d}", "        type: folder", "        buildPhase: resources"]
lines += [f"      - path: {base}prefix-template.tar.gz", "        buildPhase: resources"]
s = open(p).read()
assert "# @WINE_SOURCES@" in s
s = s.replace("      # @WINE_SOURCES@", "\n".join(lines))
open(p, "w").write(s)
PY
cat "$ROOT/Config/Wine.xcconfig"
mkdir -p "$A/i386-windows"
du -sh "$A/arm64ec-windows" "$A/aarch64-windows" "$A/i386-windows" "$A/nls"
