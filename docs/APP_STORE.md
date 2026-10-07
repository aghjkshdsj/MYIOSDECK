# MYIOSDECK on the App Store

Short version: an App Store build is possible as a **no-JIT edition**. Wine, Direct3D → Metal and
everything else in the bundle run as native signed code. The game's own x86-64 code is
interpreted by FXI. The sideload edition (StikDebug JIT, FEX at ~90% of native) stays the
full-speed option. Both are built from one codebase.

## 1. JIT: never on the App Store

iOS lets a process run code it generated only while a debugger is attached (what StikDebug
provides). App Store apps cannot get that. The `dynamic-codesigning` entitlement is reserved for
Apple and, in the EU, for alternative browser engines. This holds in the EU marketplaces too.

The consequence: **only code inside the signed bundle runs natively.** Code the user brings in (a
game's .exe and DLLs, Steam's client) must be interpreted. That applies even to ARM64 Windows
games, and to code translated ahead of time on a PC, since guideline 2.5.2 forbids downloading
executable code.

What runs natively without JIT is proven on device (docs/NO_JIT_WINDOWS.md, builds 43–44):
- Wine's ARM64EC DLLs, as signed dylibs at native speed;
- the D3D → Metal path (DXMT) and everything else in the app.

Only the game's own code goes through FXI. FXI runs at 8.1% of native on the A17 Pro (build 39),
with a target of 15–25% after phase 4 (docs/FAST_INTERPRETER.md). A game's speed lands between
that and native, depending on how much time it spends in its own code rather than in Wine, the
driver and the GPU.

## 2. Executable content: allowed for emulators since 2024

Apple rejected UTM SE and iDOS 3 in June 2024 under guideline 4.7 ("not a retro game console").
It approved UTM SE in July 2024, then updated 4.7 to let PC emulator apps offer to download
games. UTM SE ships without JIT (an interpreter), which is the same model as the no-JIT edition.
Approval is still App Review's call, case by case.

## 3. Licensing: the blocker that is not code

The App Store's terms add restrictions the GPL forbids; GPL apps (VLC, 2011) were removed for
this. What MYIOSDECK contains:
- **Madeira** (GPL-3.0-or-later with the Madeira Converter Exception): Wine's iOS unix side
  (`*_ios.c`), the app-side Wine bridge, `winios`, SwiftSteam, the DXMT/D3D12 changes. Needs
  Madeira's author's written permission for App Store distribution, or a rewrite.
- **Wine** (LGPL-2.1+): allowed in principle if users can relink against modified Wine
  libraries. That means shipping Wine's code as replaceable libraries and providing the object
  files. It needs a careful look before submission.
- **FEX** (MIT; Madeira's changes GPL): not in the App Store edition, since it is the JIT.
- **Blink** (ISC): fine. **FXI, pe2dylib, the app's own code**: ours; the repository license
  (GPL-3.0-or-later) is the owner's to change for code we wrote.

## 4. Steam

Valve's Windows Steam client (needed for license checks in many games) is x86 code. Without JIT
it runs interpreted, which will be very slow (it embeds Chromium). Native sign-in, library and
downloads (SwiftSteam) are unaffected. DRM-free and Steamworks-light games are the realistic
first targets.

## Plan for the App Store edition

1. Finish the no-JIT path, steps B–D in docs/NO_JIT_WINDOWS.md: Wine loads its signed dylibs, no
   runtime-generated code, FXI as the x86-64 CPU.
2. FXI speed (phase 4): `preserve_none` dispatch, superinstructions, NEON SSE, guest register
   caching.
3. Licensing (owner's action): ask Madeira's author for an App Store exception, and plan the
   Wine LGPL relinking setup.
4. Build flavours: a `nojit` IPA without FEX or the StikDebug flow, `get-task-allow` off, signed
   for distribution.
5. Submit under guideline 4.7 as a PC emulator. The user supplies games; no game is bundled.
