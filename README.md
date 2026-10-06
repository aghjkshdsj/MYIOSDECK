<p align="center"><img src="App/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png" width="160" alt="MYIOSDECK"></p>

<h1 align="center">MYIOSDECK</h1>

<p align="center"><b>Steam PC games on iPhone.</b> FEX x86-64 → ARM64 JIT, Wine ARM64EC, Direct3D on Metal, with a Steam Deck–style front end.</p>

> [!NOTE]
> Early research project. Stage 1 (JIT + FEX translation, a no-JIT interpreter, benchmarks) is in
> the app and verified on an iPhone 15 Pro Max with iOS 27.0.1. Stage 2 (Wine) is being built in CI.
> Steam games need stages 2–4 (Wine, Direct3D on Metal, Steam); see the [roadmap](docs/ROADMAP.md).

## What it is

[DroidDeck](https://github.com/Droid-Deck/DroidDeck) brings SteamOS to Android by running a Linux
userspace (proot), Valve's ARM64 Steam client and Proton. **iOS cannot run that stack:** it has no
Linux kernel, an app cannot start other processes, and iPhones expose no hypervisor. So MYIOSDECK
rebuilds the *same compatibility stack Valve's ARM64 Proton uses*, natively for iOS, in one process:

| Layer | Android (DroidDeck) | iPhone (MYIOSDECK) |
|---|---|---|
| x86 → ARM64 | FEX under proot | **FEXCore embedded**, JIT pool from StikDebug |
| Windows API | Proton ARM64 (Wine ARM64EC) | **Wine ARM64EC in-process**, wineserver as a thread |
| Graphics | DXVK / VKD3D → Vulkan (Turnip) | **Direct3D → Metal directly** (DXMT, D3D12 path), no Vulkan layer |
| Steam | Linux ARM64 Steam client, Big Picture | Native Steam sign-in/library/downloads + Valve's Windows client to launch |
| Display | gamescope | CAMetalLayer at 120 Hz ProMotion, paced presents |

The iOS ports of FEX, Wine and DXMT come from [Madeira](https://github.com/willfaust/Madeira), the
project that first ran Windows games on an iPhone. MYIOSDECK builds them in GitHub Actions.

## Install (iPhone 15 Pro Max, iOS 26/27)

1. Download the newest `MYIOSDECK-…-fex.ipa` from [Releases](../../releases).
2. Sideload it with **[iloader](https://github.com/nab138/iloader)** (or SideStore / AltStore) using your Apple ID.
3. Install **[StikDebug](https://github.com/StikDebug/StikDebug)**, import your pairing file (iloader can place it), and connect **LocalDevVPN**.
4. Open MYIOSDECK → **Enable JIT**. StikDebug opens, attaches, and hands control back.
5. Home shows **Ready to translate**. Tap **Run x86-64 test**, then try **Performance**.

Full guide and troubleshooting: [docs/INSTALL.md](docs/INSTALL.md).

## Performance

Measured on an iPhone 15 Pro Max (A17 Pro), iOS 27.0.1, build 2, Fast preset: x86-64 code through
FEX ran at **~80–100% of native ARM64 speed** (integer 103%, memory 100%, SIMD 100%, float 81%,
branch-heavy 53%), with every checksum matching native.


- FEXCore is tuned from the kernel's own CPU feature list (LSE, LRCPC2, FlagM2, AFP, …), not a hardcoded guess.
- Presets match the SteamOS ARM Port's: **Compat**, **Fast** (default), **Fastest** (TSO off).
- The Performance tab runs the same C kernels natively and as x86-64 through FEX and reports FEX's efficiency on *your* phone.
- Without JIT, MYIOSDECK can still run x86-64 programs in an interpreter (Blink) after asking first;
  see [docs/NO_JIT.md](docs/NO_JIT.md). Results are logged as a shareable report.
- The Metal stage measures frame time and GPU time at 30/40/60/120 Hz with even ProMotion pacing.

## App Store

Not possible for this design today: iOS only allows JIT while a debugger is attached, and the App
Store forbids it. Without JIT, FEX would have to interpret x86 code, which is roughly 10–50× slower.
See [docs/APP_STORE.md](docs/APP_STORE.md) for the details and what would have to change.

## Build

GitHub Actions builds everything ([build-ipa.yml](.github/workflows/build-ipa.yml)): x86-64 guest
programs on Linux, FEXCore for iOS on macOS, then the app with XcodeGen and `xcodebuild`, packaged
as an IPA and published to Releases on every push to `main`.

Locally on a Mac: `bash engine/fex/build-ios.sh && xcodegen generate && open MYIOSDECK.xcodeproj`.

## License

GPL-3.0-or-later ([LICENSE](LICENSE)), with the Madeira Converter Exception for the parts derived
from Madeira ([LICENSE-EXCEPTION.md](LICENSE-EXCEPTION.md)). See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
Steam is a trademark of Valve Corporation; this project is not affiliated with Valve or Apple.
