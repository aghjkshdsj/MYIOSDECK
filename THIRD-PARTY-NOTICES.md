# Third-party notices

| Component | Use in MYIOSDECK | License |
|---|---|---|
| [FEX-Emu](https://github.com/FEX-Emu/FEX) via [Madeira's iOS fork](https://github.com/willfaust/FEX) (`engine/fex/PIN`) | x86/x86-64 → ARM64 JIT, linked statically | Upstream MIT; Madeira's changes GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | Design of the JIT allocator, FEX bridge and StikDebug script (`App/Sources/Native/jit_core.c`, `fex_engine.mm`, `App/Resources/myiosdeck-jit.js`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | x18-instruction classifier ported to Python for the no-JIT DLL audit (`engine/pedylib/pe2dylib.py`, from `build/ntdll-unix/virtual_ios.c`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) SwiftSteam | Native Steam client (sign-in, library, depot downloads), vendored unchanged in `App/Sources/Steam/SwiftSteam/` | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira Dock](https://github.com/willfaust/madeira-dock) (commit e0007841) | `dockhost.exe`, the headless host for Valve's own Steam client, built from source in CI (`engine/pedylib/build-spike.sh`) and shipped with its notices (`dock-notices.txt`) | GPL-3.0-or-later with the Madeira Converter Exception, Copyright 2026 125hz |
| [Madeira](https://github.com/willfaust/Madeira) Dock app side | `App/Sources/Steam/MadeiraDock.swift` (launch contract, sign-in transfer, report) and `SteamRuntime.swift` (Valve client download and verification), vendored from Madeira 65e6fe8f with small adaptations noted in each file | GPL-3.0-or-later with the Madeira Converter Exception 
| [innoextract](https://github.com/dscharrer/innoextract) (commit 6e9e34ed, `engine/innoextract/PIN`) | Unpacks GOG offline installers (Inno Setup) on the phone; fetched and built from source in CI with a small patch (`engine/innoextract/myiosdeck.patch`: log and progress hooks, no `system()` on iOS), linked statically through `engine/innoextract/myiosdeck_inno.cpp` | zlib license, Copyright (C) 2011-2020 Daniel Scharrer. Altered version: see the patch |
| [Boost](https://www.boost.org) 1.83.0 (`engine/innoextract/PIN`) | Headers, Boost.Filesystem and Boost.Iostreams' zlib/bzip2 filters, compiled into the unpacker | BSL-1.0 |
| [xz / liblzma](https://tukaani.org/xz/) 5.4.7 headers (`engine/innoextract/PIN`) | `lzma.h` for building the unpacker; the library itself is iOS's own liblzma | Public domain (liblzma 5.4) |
| zlib, bzip2, libiconv | Used by the unpacker from the iOS SDK (system libraries, not bundled) | zlib / bzip2 license / LGPL-2.0 |
| [StikDebug / StikJIT](https://github.com/StikDebug/StikJIT) | JIT protocol (BRK #0xf00d) and URL scheme; StikDebug is a separate app | MPL-2.0 (not bundled) |
| [DroidDeck](https://github.com/Droid-Deck/DroidDeck) | Product design blueprint (no code copied) | GPL-3.0 |
| [SteamOS ARM Port](https://github.com/hashtagbasit/SteamOS-ARM-Port) | Preset naming (compat / fast / fastest) | GPL-2.0 (no code copied) |
| FEX submodules: fmt, xxHash, range-v3, unordered_dense, SoftFloat-3e, cephes | Linked through FEXCore | MIT / BSD-2-Clause / BSL-1.0 / BSD-3-Clause / MIT-style |

Steam is a trademark of Valve Corporation. iPhone, iOS and Metal are trademarks of Apple Inc.
MYIOSDECK is not affiliated with Valve or Apple.
