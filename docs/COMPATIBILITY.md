# Game compatibility record

What runs, what does not, and why, without JIT (FXI/FXR for 64-bit, FXI32 for 32-bit, DXMT, Wine
from signed dylibs). One row per game the owner has tried; the build is the last one tested.
Statuses: **works**, **partly** (starts, with a named problem), **fails** (with the cause), and
**fix pending** (a cause found and fixed on a branch, not yet confirmed on the phone).

Every launch logs `[compat] <program>: engine <engine>` (and, for an Adobe AIR game, where its
runtime is), from the game's file names (App/Sources/Engine/GameCompatCore.swift). Use it for the
engine column of a new row.

## Games

| game | engine | bits | start | status | build | notes |
|---|---|---|---|---|---|---|
| Steins;Gate | own (Bink, DirectInput) | 32 | added game, launcher + Game.exe | works | 197 | the WoW64 path's reference game |
| Forager | GameMaker | 32 | added game | works; froze once | 190 | froze when the on-screen pad was unplugged/replugged behind the game menu: DirectInput's input thread looped in a corrupted acquired-device list (fix below, `dinput_ios_unplug.py`); build 197 already keeps the pad connected behind the menu |
| Stick Fight: The Game | Unity (Mono) | 64 | Steam | works | 197 | FXR |
| Dokimon | Unity | 64 | Steam, With and Without Steam | works | 197 | With Steam through Madeira Dock |
| Deponia | — | — | — | works | 197 | |
| Hollow Knight | Unity (Mono) | 64 | added game | works; app crashed opening the game menu | 187 | crash in the 32-bit thread report, see "Game menu crash" |
| Billie's Wheelie | Unity | 64 | Steam (Dock) | works; app crashed opening the game menu | 187 | same crash as Hollow Knight |
| 911 Operator | Unity | 32 | Steam | With Steam: works. Without Steam: fix pending | 190 | Without Steam the game asked Steam to restart it (`SteamAPI_RestartAppIfNecessary`), started `steam.exe steam://run/503560` and quit (exit 53); Valve's 32-bit steam.exe then stopped on MMX. Now: `steam_appid.txt` and MMX |
| Cuphead | Unity (Mono) | 64 | added game | works | 120 | 24-56 fps in the log |
| Vampire Survivors | Unity (IL2CPP) | 64 | Steam | fix pending | 120 | "Failed to load il2cpp": GameAssembly.dll imports api-ms-win-core-winrt-robuffer-l1-1-0 = wintypes.dll, which the 64-bit DLL set lacked (c0000135). Now built (`build-pe-extras.sh`) |
| Hades II | own (The Forge, D3D12) | 64 | added game | fix pending, more likely needed | 120 | the game's own dbghelp.dll (0x1f3000) was mapped as Wine's signed dbghelp (0xf0000) and refused (c000007b), so the exe never loaded (c0000135). Now the game's copy loads as its code. What comes after (D3D12 through madeira-d3d12, FMOD, SDL2) is untested |
| Capcom Arcade Stadium | own | 64 | Steam | fix pending | 120 | FXI stopped on `65 48 a1 <imm64>` (`mov rax, gs:[0x8]`, the moffs64 form): the game ended with 0xE0F0F001. FXI and FXR now decode A0-A3 |
| Transformice | Adobe AIR | 32 | Steam | fails, cause not in a log | ≤197 | "This application requires a version of Adobe AIR which cannot be found" at ~3 fps. No log of the run; the next one has `[compat] ... Adobe AIR: ...` saying whether the captive runtime is there. MMX (now in FXI32) is a likely cause if the runtime's DLL failed to initialise |

## Fixes that apply to games nobody has tried

Each changes behaviour only where something failed before.

| fix | where | what failed before | check |
|---|---|---|---|
| Wine builtins the 64-bit set lacked: wintypes, twinapi.appcore, msvcp120, msvcr/msvcp 80/90/100, vcomp*, xaudio2_8, xaudio2_9, d3d10, d3d10_1, d3dx10_43, d3dx11_42, d3dx9_36-42, d3dcompiler_46, d2d1, shfolder, msvfw32, avifil32, gameux | `engine/wine/build-pe-extras.sh` (dxmt-pe.yml) | a 64-bit game importing one ended with c0000135 at start (the 32-bit set already had them all) | new names only: the script never rebuilds a module Madeira ships; the step summary lists what was built and any import still missing |
| A game's own DLL larger than Wine's signed DLL of the same name loads as the game's code | `engine/wine/patches/nojit_dylib.py` | c000007b, then c0000135 (Hades II's dbghelp.dll) | only the size check that used to fail |
| `mov al/eax/rax <-> [moffs64]` (A0-A3) in 64-bit code | FXI and FXR decoders | STOP "unimplemented instruction" | difftest `gs_moffs` (fxi.yml, fxr.yml) |
| MMX in 32-bit code: every MMX instruction (arithmetic, compare, pack/unpack, shifts, moves, pshufw/pinsrw/pextrw/pmovmskb/maskmovq/movntq, the MMX<->XMM conversions, emms) | FXI32 (`fxi_sse.c` m_*, `fxi_decode.c` decode_mmx) | STOP on the first MMX instruction, although CPUID reports MMX | difftest32 `mmx` (fxi.yml) |
| An acquired DirectInput pad that disappears leaves the acquired list | `engine/wine/patches/dinput_ios_unplug.py` (32-bit farm) | the game froze after a pad unplug + re-Acquire | anchors checked; 64-bit games keep Madeira's prebuilt dinput (follow-up) |
| `steam_appid.txt` for a Steam game started without Steam, when it ships steam_api and has none | `GameCompatCore.swift`, GameLauncher | the game restarted itself through steam.exe and quit | tests/host `game_compat` |
| Game menu crash: the 32-bit thread report only reads threads with its own magic and TEB | `App/Sources/Native/fxi_wow_host.c` | the app crashed opening the game menu in some 64-bit games | — |
| A 32-bit game after a 64-bit one in a kept session is refused with a restart message | `WineController.nextGameRefusal` | the 32-bit game crashed at start (its 64-bit half loaded the ARM64EC win32u.dll) | 64-bit after 32-bit is unchanged |

## Follow-ups

- **32-bit after 64-bit in one session**: the aarch64 ntdll needs the mirror of the ARM64EC
  loader's `sysx64` retry (`sysaa64`), or the WoW64 side's system32 lookups pointed at the aarch64
  farm. Until then the app asks for a restart.
- **64-bit DirectInput pad unplug**: the same list fix for the ARM64EC dinput/dinput8 would replace
  Madeira's prebuilt binaries; not done blind.
- **Adobe AIR**: read the `[compat]` line of the next Transformice log.
- **SSE3/SSSE3/SSE4 in CPUID**: FXI reports none (ECX = 0) although it implements SSE3; games
  that require SSE4.1 refuse to start. Needs the remaining SSSE3/SSE4.1 forms first.
