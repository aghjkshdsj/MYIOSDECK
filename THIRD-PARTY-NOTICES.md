# Third-party notices

| Component | Use in MYIOSDECK | License |
|---|---|---|
| [FEX-Emu](https://github.com/FEX-Emu/FEX) via [Madeira's iOS fork](https://github.com/willfaust/FEX) (`engine/fex/PIN`) | x86/x86-64 → ARM64 JIT, linked statically | Upstream MIT; Madeira's changes GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | Design of the JIT allocator, FEX bridge and StikDebug script (`App/Sources/Native/jit_core.c`, `fex_engine.mm`, `App/Resources/myiosdeck-jit.js`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) | x18-instruction classifier ported to Python for the no-JIT DLL audit (`engine/pedylib/pe2dylib.py`, from `build/ntdll-unix/virtual_ios.c`) | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira](https://github.com/willfaust/Madeira) SwiftSteam | Native Steam client (sign-in, library, depot downloads), vendored unchanged in `App/Sources/Steam/SwiftSteam/` | GPL-3.0-or-later with the Madeira Converter Exception |
| [Madeira, Connor Gow's fork](https://github.com/c-gow/Madeira) (release opengl-test-7, `engine/wine/PIN`) | Wine's unix side for iOS built in CI, including the winios OpenGL driver (`build/win32u-unix/opengl_ios.c`, `app/Madeira/Winios/WiniosGL.m`), opengl32 changes (`patches/wine-opengl-winios.patch`), the Mesa / MoltenVK build scripts and Mesa patches (`build/mesa-ios`, `build/moltenvk-ios`) and the GC64 LuaJIT substitution (`build/wineserver/luajit_compat.c`); OpenGL work by Connor Gow on Will Faust's Madeira | Wine parts LGPL-2.1-or-later; Madeira parts GPL-3.0-or-later with the Madeira Converter Exception; Mesa patches MIT |
| [Mesa](https://mesa3d.org) 25.0.7 (`engine/gl/PIN`) | OSMesa + Zink (OpenGL on Vulkan), `gl/libOSMesa.dylib`; built without LLVM | MIT (some files under other permissive licences, see `gl/licenses/Mesa-license.rst` in the app) |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) v1.4.2 (`engine/gl/PIN`) | Vulkan on Metal for Zink, `gl/libMoltenVK.dylib` | Apache-2.0 |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Linked into MoltenVK (SPIR-V to Metal Shading Language) | Apache-2.0 (Vulkan-Headers: Apache-2.0 or MIT) |
| [cereal](https://github.com/USCiLab/cereal) | Serialization inside MoltenVK | BSD-3-Clause |
| [StikDebug / StikJIT](https://github.com/StikDebug/StikJIT) | JIT protocol (BRK #0xf00d) and URL scheme; StikDebug is a separate app | MPL-2.0 (not bundled) |
| [DroidDeck](https://github.com/Droid-Deck/DroidDeck) | Product design blueprint (no code copied) | GPL-3.0 |
| [SteamOS ARM Port](https://github.com/hashtagbasit/SteamOS-ARM-Port) | Preset naming (compat / fast / fastest) | GPL-2.0 (no code copied) |
| FEX submodules: fmt, xxHash, range-v3, unordered_dense, SoftFloat-3e, cephes | Linked through FEXCore | MIT / BSD-2-Clause / BSL-1.0 / BSD-3-Clause / MIT-style |

Steam is a trademark of Valve Corporation. iPhone, iOS and Metal are trademarks of Apple Inc.
MYIOSDECK is not affiliated with Valve or Apple.
