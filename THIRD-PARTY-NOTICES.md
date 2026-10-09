# Third-party notices

| Component | Use in MYIOSDECK | License |
|---|---|---|
| [FEX-Emu](https://github.com/FEX-Emu/FEX) via [Madeira's iOS fork](https://github.com/willfaust/FEX) (`engine/fex/PIN`) | x86/x86-64 → ARM64 JIT, linked statically | Upstream MIT; Madeira's changes GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | Design of the JIT allocator, FEX bridge and StikDebug script (`App/Sources/Native/jit_core.c`, `fex_engine.mm`, `App/Resources/myiosdeck-jit.js`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | x18-instruction classifier ported to Python for the no-JIT DLL audit (`engine/pedylib/pe2dylib.py`, from `build/ntdll-unix/virtual_ios.c`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) SwiftSteam | Native Steam client (sign-in, library, depot downloads), vendored unchanged in `App/Sources/Steam/SwiftSteam/` | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira Dock](https://github.com/willfaust/madeira-dock) (commit e0007841) | `dockhost.exe`, the headless host for Valve's own Steam client, built from source in CI (`engine/pedylib/build-spike.sh`) and shipped with its notices (`dock-notices.txt`) | GPL-3.0-or-later with the Madeira Converter Exception, Copyright 2026 125hz |
| [Madeira](https://github.com/willfaust/Madeira) Dock app side | `App/Sources/Steam/MadeiraDock.swift` (launch contract, sign-in transfer, report) and `SteamRuntime.swift` (Valve client download and verification), vendored from Madeira 65e6fe8f with small adaptations noted in each file | GPL-3.0-or-later with the Madeira Converter Exception 
| [StikDebug / StikJIT](https://github.com/StikDebug/StikJIT) | JIT protocol (BRK #0xf00d) and URL scheme; StikDebug is a separate app | MPL-2.0 (not bundled) |
| [DroidDeck](https://github.com/Droid-Deck/DroidDeck) | Product design blueprint (no code copied) | GPL-3.0 |
| [SteamOS ARM Port](https://github.com/hashtagbasit/SteamOS-ARM-Port) | Preset naming (compat / fast / fastest) | GPL-2.0 (no code copied) |
| FEX submodules: fmt, xxHash, range-v3, unordered_dense, SoftFloat-3e, cephes | Linked through FEXCore | MIT / BSD-2-Clause / BSL-1.0 / BSD-3-Clause / MIT-style |
| [Wine](https://www.winehq.org) via [Madeira's fork](https://github.com/willfaust/wine) (`engine/wine/PIN`) | Windows DLL farms: ARM64EC and aarch64 (Madeira's prebuilt PE files, also wrapped as signed dylibs), i386 (built from source in CI, `engine/wine/build-i386.sh`, for 32-bit games); unix side linked statically | LGPL-2.1-or-later |
| [DXMT](https://github.com/3Shain/dxmt) via [Madeira's fork](https://github.com/willfaust/dxmt) (`engine/dxmt/PIN`) | Direct3D 9/10/11 → Metal: ARM64EC and i386 DLLs built from source in CI, unix side linked statically | MIT (upstream); D3D9 import LGPL-2.1-or-later; Madeira's changes GPL-3.0-or-later (with the Madeira Converter Exception for 125hz's) |
| [Madeira](https://github.com/willfaust/Madeira) `tests/x86/hello-x86.c` | `hello-x86.exe`, the 32-bit (WoW64) smoke test, built in CI | GPL-3.0-or-later with the Madeira Converter Exception |

Steam is a trademark of Valve Corporation. iPhone, iOS and Metal are trademarks of Apple Inc.
MYIOSDECK is not affiliated with Valve or Apple.
