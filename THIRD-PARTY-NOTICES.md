# Third-party notices

| Component | Use in MYIOSDECK | License |
|---|---|---|
| [FEX-Emu](https://github.com/FEX-Emu/FEX) via [Madeira's iOS fork](https://github.com/willfaust/FEX) (`engine/fex/PIN`) | x86/x86-64 → ARM64 JIT, linked statically | Upstream MIT; Madeira's changes GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | Design of the JIT allocator, FEX bridge and StikDebug script (`App/Sources/Native/jit_core.c`, `fex_engine.mm`, `App/Resources/myiosdeck-jit.js`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [StikDebug / StikJIT](https://github.com/StikDebug/StikJIT) | JIT protocol (BRK #0xf00d) and URL scheme; StikDebug is a separate app | MPL-2.0 (not bundled) |
| [DroidDeck](https://github.com/Droid-Deck/DroidDeck) | Product design blueprint (no code copied) | GPL-3.0 |
| [SteamOS ARM Port](https://github.com/hashtagbasit/SteamOS-ARM-Port) | Preset naming (compat / fast / fastest) | GPL-2.0 (no code copied) |
| FEX submodules: fmt, xxHash, range-v3, unordered_dense, SoftFloat-3e, cephes | Linked through FEXCore | MIT / BSD-2-Clause / BSL-1.0 / BSD-3-Clause / MIT-style |

Steam is a trademark of Valve Corporation. iPhone, iOS and Metal are trademarks of Apple Inc.
MYIOSDECK is not affiliated with Valve or Apple.
