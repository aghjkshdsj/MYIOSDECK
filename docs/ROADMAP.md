# Roadmap and architecture

## Why not SteamOS itself

SteamOS ARM (the Steam Frame image, and the SteamOS ARM Port for Snapdragon handhelds) is a whole
operating system: a Linux kernel, systemd, gamescope, the Linux ARM64 Steam client with its
multi-process Chromium UI, and Proton launched as separate processes in containers. On iPhone:

- **No Linux kernel.** Apple's bootloader runs only Apple-signed kernels, so Linux binaries' system
  calls have nothing to talk to.
- **No processes.** An app cannot `fork`/`exec`; Steam alone is a dozen processes.
- **No hypervisor.** iPhones expose no virtualization to apps; a full-system emulator (QEMU TCG) has
  no GPU acceleration and runs far too slowly for games.

So MYIOSDECK keeps the parts of that stack that are user-space libraries and runs them in one
process, the way Madeira proved works.

```
 Steam game (x86-64 Windows .exe)
        │  x86-64 code ───────────────► FEXCore JIT ──► ARM64 code in the JIT pool
        │  Win32 / NT API calls ──────► Wine ARM64EC (native ARM64), wineserver thread
        │  Direct3D 9/10/11/12 ───────► DXMT / D3D12 path ──► Metal ──► CAMetalLayer 120 Hz
        │  XInput ────────────────────► GameController framework
        ▼
 MYIOSDECK.app (one iOS process, JIT from StikDebug, Memory+)
```

## Stages

| # | Stage | Status |
|---|---|---|
| 1 | JIT pool via StikDebug (iOS 26/27 TXM), FEXCore built in CI, host-feature tuning, presets, x86-64 test + native-vs-FEX benchmarks, Metal 120 Hz stage + HUD | Done: verified on iPhone 15 Pro Max / iOS 27 (FEX ~93% of native) |
| 2 | Wine ARM64EC: build `ntdll_unix`, `win32u_unix`, `wineserver` for iOS and the ARM64EC PE modules + `xtajit64.dll` (FEX ARM64EC) in CI; boot a prefix and run a console `.exe` | Done: hello-x64.exe runs on device (build 20) |
| 3 | DXMT (D3D9–11 → Metal) and the D3D12 path; present into the Metal stage; first 3D game | Done: D3D11 (DXMT) and D3D12 (runtime DXIL conversion) cubes render at 60 fps on iPhone 15 Pro Max (builds 21-22); first real game moves to stage 4 |
| 4 | Steam: native sign-in (QR / password + Steam Guard), owned library with artwork, depot downloads, cloud saves; launch through Valve's Windows client | Next |
| 5 | Deck UX: controller-driven Big Picture-style UI, per-game presets, touch controls, keyboard/mouse | Started: touch as mouse, hardware keyboard and mouse, the on-screen keyboard and the right-stick cursor (`App/Sources/Engine/GameInput.swift`, build 131); the rest planned |
| 6 | Performance: AOT code cache (persist FEX translations between runs), Metal shader cache, efficiency-core management, 40 Hz / 30 Hz caps | Planned |

Stages 2–4 port Madeira's working components (their build scripts are documented in Madeira's
`docs/BUILDING.md`) into this repository's CI, so every IPA is reproducible from a clean checkout.

## Speed: where it comes from

1. **FEX JIT** with the A17 Pro's ARMv8.6 features: LSE atomics for `LOCK` instructions, LRCPC/LRCPC2
   for x86 memory ordering, FlagM/FlagM2 for EFLAGS, AFP for exact SSE min/max.
2. **Wine ARM64EC**: Windows itself runs as native ARM64; only the game's own x86 code is translated.
3. **Direct3D → Metal directly**: no Vulkan in between (DXVK → MoltenVK would add a second
   translation layer and lacks features games need).
4. **Presets**: Fastest turns TSO emulation off for the largest CPU win in single-threaded-heavy games.
5. **Even pacing**: presents through `presentDrawable(afterMinimumDuration:)` at 30/40/60/120 Hz.
