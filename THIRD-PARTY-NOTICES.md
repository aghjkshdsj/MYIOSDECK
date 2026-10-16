# Third-party notices

| Component | Use in MYIOSDECK | License |
|---|---|---|
| [FEX-Emu](https://github.com/FEX-Emu/FEX) via [Madeira's iOS fork](https://github.com/willfaust/FEX) (`engine/fex/PIN`) | x86/x86-64 → ARM64 JIT, linked statically | Upstream MIT; Madeira's changes GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | Design of the JIT allocator, FEX bridge and StikDebug script (`App/Sources/Native/jit_core.c`, `fex_engine.mm`, `App/Resources/myiosdeck-jit.js`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | x18-instruction classifier ported to Python for the no-JIT DLL audit (`engine/pedylib/pe2dylib.py`, from `build/ntdll-unix/virtual_ios.c`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Zstandard](https://github.com/facebook/zstd) educational decoder (v1.5.6, `doc/educational_decoder`) | Decoding of Steam's Zstandard chunks, vendored unchanged in `App/Sources/Steam/Vendor/zstd/` and compiled only through `App/Sources/Steam/Client/SteamZstdGlue.c` | BSD-3-Clause (`App/Sources/Steam/Vendor/zstd/LICENSE`) |
| [StikDebug / StikJIT](https://github.com/StikDebug/StikJIT) | JIT protocol (BRK #0xf00d) and URL scheme; StikDebug is a separate app | MPL-2.0 (not bundled) |
| [DroidDeck](https://github.com/Droid-Deck/DroidDeck) | Product design blueprint (no code copied) | GPL-3.0 |
| [SteamOS ARM Port](https://github.com/hashtagbasit/SteamOS-ARM-Port) | Preset naming (compat / fast / fastest) | GPL-2.0 (no code copied) |
| FEX submodules: fmt, xxHash, range-v3, unordered_dense, SoftFloat-3e, cephes | Linked through FEXCore | MIT / BSD-2-Clause / BSL-1.0 / BSD-3-Clause / MIT-style |
| [Wine](https://www.winehq.org) via [Madeira's fork](https://github.com/willfaust/wine) (`engine/wine/PIN`) | Windows DLL farms: ARM64EC and aarch64 (Madeira's prebuilt PE files, also wrapped as signed dylibs; `wow64win.dll` rebuilt from the same source with `engine/wine/patches/wow64win_atoms.py`), i386 (built from source in CI, `engine/wine/build-i386.sh`, for 32-bit games; its `dinput.dll` gets `engine/wine/patches/dinput_ios_pad.py`); unix side linked statically | LGPL-2.1-or-later |
| [DXMT](https://github.com/3Shain/dxmt) via [Madeira's fork](https://github.com/willfaust/dxmt) (`engine/dxmt/PIN`) | Direct3D 9/10/11 → Metal: ARM64EC and i386 DLLs built from source in CI, unix side linked statically | MIT (upstream); D3D9 import LGPL-2.1-or-later; Madeira's changes GPL-3.0-or-later (with the Madeira Converter Exception for 125hz's) |
| [FEX-Emu](https://github.com/FEX-Emu/FEX) WOW64 module (`Source/Windows/WOW64/Module.cpp`, via Madeira's fork) | The BTCpu* contract, the BOP-page convention and the unwind-frame trampoline, ported to C in `engine/pedylib/emu/xtajit_wow.c` (the no-JIT WoW64 CPU module) | MIT (upstream); Madeira's guest-window changes GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) `tests/x86/hello-x86.c` | `hello-x86.exe`, the 32-bit (WoW64) smoke test, built in CI | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) prefix template (`app/Madeira/prefix-template.tar.gz`, at `engine/wine/PIN`) | The Wine prefix a new install starts from, bundled as a resource by `engine/wine/stage-app.sh` | GPL-3.0-or-later with the Madeira Converter Exception |

## Clean-room components

These parts were written for MYIOSDECK from behaviour-only specifications in a clean room, by
writers who never saw the code they replace. They carry MIT headers (Copyright 2026 the
MYIOSDECK author):

- `App/Sources/Engine/GameInput.swift`, `ControllerBridge.swift`: touch, keyboard, mouse and
  controller input;
- `engine/wine/bridge/`: the app-side Wine bridge, prefix setup and game-mode display driver;
- `App/Sources/Steam/Client/`: the Steam client (sign-in, library, downloads, Steam files). Its
  message numbers follow Valve's published protobuf definitions (used as a reference, not shipped);
- `App/Sources/Steam/SteamHost*.swift`: installs and starts Valve's own Steam client from
  Valve's public installer.

Steam is a trademark of Valve Corporation. iPhone, iOS and Metal are trademarks of Apple Inc.
MYIOSDECK is not affiliated with Valve or Apple.
