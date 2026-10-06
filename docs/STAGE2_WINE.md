# Stage 2: Wine on iOS

Windows games run on Wine. On iOS, Wine has to live inside the app's single
process, with no Linux kernel underneath, and execute only from memory the
debugger prepared. [Madeira](https://github.com/willfaust/Madeira) solved this
first; MYIOSDECK builds Madeira's port **reproducibly in CI** and runs it behind
its own front end.

## What runs where

| Part | Native ARM64 (iOS) | Windows side (PE, ARM64EC) |
|---|---|---|
| ntdll | `libntdll_unix.a`: loader, virtual memory (copies PE code into the JIT pool), signals via Mach exceptions | `ntdll.dll` |
| win32u / windowing | `libwin32u_unix.a` + `winios` display driver (UIKit/Metal windows, touch, keys) | `user32.dll`, `win32u.dll`, … |
| wineserver | `libwineserver.a`, run as a **thread** (no processes on iOS) | — |
| x86-64 code | — | `xtajit64.dll` = FEX for ARM64EC, translating the game's x86-64 code |
| Crypto / media / fonts | GnuTLS, FFmpeg (LGPL config), FreeType unix sides | `bcrypt`, `winegstreamer`, `dwrite` |

Everything in the right column is prebuilt in Madeira's repository (168 ARM64EC
DLLs) and bundled as-is; the left column is what the CI builds.

## The pipeline

1. **`Stage 2 - Wine unix side`** workflow (`engine/wine/build-unix.sh`):
   - fetch Madeira at the pinned commit + its Wine fork, llvm-mingw, FreeType
   - configure Wine natively on macOS (`build-macos`): `config.h`, host tools
     (`winebuild`, `widl`, `wrc`) and all generated IDL headers
   - build GnuTLS/Nettle/GMP, FFmpeg and FreeType for iOS
   - compile Wine's unix side for iOS with Madeira's `*_ios.c` replacements:
     `libntdll_unix.a`, `libwin32u_unix.a`, and `libwineserver.a` — the latter
     rebuilt entirely from source (Madeira's script patches a prebuilt archive
     that is not in its repository)
   - upload the libraries as the `wine-unix-libs` artifact
2. **`Build IPA`** workflow (`engine/wine/stage-app.sh`): takes the newest
   successful `wine-unix-libs`, compiles Madeira's app-side files in place
   (Wine bridge, `winios` driver, input), bundles the DLL farm, NLS tables and
   the prefix template, and links everything.

## How MYIOSDECK hands Wine its memory

- At image load, `jit_core.c` reserves `[0x140000000, +128 MB)` for fixed-base
  x64 executables and a placeholder directly above it for the JIT pool.
- When JIT is enabled, the placeholder is released and the debugger allocates
  the pool there (first fit), so translated code stays near the images.
- Starting Wine gives it the rest of the pool through `WINE_IOS_JIT_RX/RW/SIZE`,
  the window through `WINE_IOS_EXE_WINDOW`, and the RW alias offset to FEX
  inside Wine through `MADEIRA_JIT_WRITE_OFFSET`.

## First test

Library › **Windows programs** › *Windows Hello (x64)* runs Madeira's
`hello-x64.exe` through Wine + FEX ARM64EC. Its output appears in the Logs tab
as `[stdio]` lines. Use a **1 GB** JIT pool (Settings › JIT) for Wine.
