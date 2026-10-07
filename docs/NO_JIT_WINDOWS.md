# Windows games without JIT

Goal: Windows games on iPhone with JIT off (no StikDebug), as fast as possible.
First milestone: **Windows Hello (x64) runs with JIT off**.

## Why it needs JIT today

Without a debugger iOS executes only code that is signed and mapped from a file. Madeira's
Wine copies every PE image (168 ARM64EC DLLs, 158 MB) into the debugger-prepared JIT pool,
patches it there (x18 → TEB trampolines, `RtlPcToFileHeader`, hand-written TEB reads),
writes runtime thunks into the pool, and runs the game's x86-64 code through FEX
(`xtajit64.dll`), a JIT.

## The plan

| | Step | What it takes |
|---|---|---|
| A | Wine's ARM64EC DLLs run from **signed dylibs** | Rebuild the PE side with 16 KB section alignment; each image wrapped by `engine/pedylib/pe2dylib.py` as a dylib: headers + code in signed `__TEXT` (r-x), data in `__DATA` (rw) right behind it, so RVAs hold and code is never written (iOS refuses mmap over dyld-mapped pages, build 42). Everything Madeira patches in `.text` at load has to be done at build time instead (x18 sites → trampolines inside the image, syscall-stub literal pools → data). |
| B | ntdll's unix loader maps those images | `loader_ios.c` / `virtual_ios.c`: when JIT is off, take the image from the dylib instead of copying it into the pool. |
| C | No runtime-generated code | Every pool trampoline/thunk becomes fixed code compiled into the app. |
| D | **FXI** as the x86-64 CPU | Replaces FEX/xtajit64 behind the ARM64EC emulator interface (`__os_arm64x_*` dispatch, `BTCpu*` exports): GS → TEB, x64 ⇄ ARM64EC calling convention transitions, exceptions/unwinding, threads. |
| E | FXI speed | Phase 4 of docs/FAST_INTERPRETER.md: NEON SSE, superinstructions, `preserve_none` dispatch, SSE4/AVX coverage driven by real game code. |

Only the game's own x86-64 code is interpreted. Wine, the D3D → Metal path (DXMT) and
everything else stay native ARM64, so a game's speed sits between FXI's (8% of native on
the A17 Pro, build 39) and native, depending on how much time it spends in its own code.

## What the audit of Madeira's real DLLs found (build 40)

149 ARM64EC DLLs, 40 MB of code, sections mostly 4 KB aligned:
- only **7** 16 KB pages anywhere hold code together with something written at load, and
  **no** base relocation lands in code: the images are almost mappable as they are, and a
  16 KB-aligned rebuild removes the rest;
- **11,364 x18 instructions** (ntdll 745, user32 450, msvcr* ~220 each, …): Windows keeps
  the TEB in x18 and iOS clears it, so each must be rewritten at build time into a branch
  to a trampoline appended to the image that reads the TEB from TPIDRRO_EL0 (Madeira found
  the TSD slot fixed at 275, offset 0x898). This is the main Step A job;
- 6 hand-written TEB reads, all in `xtajit64.dll` (FEX), which FXI replaces anyway.

**Build 43, iPhone 15 Pro Max, JIT off:** both spike DLLs (ARM64 and ARM64EC) loaded from
signed dylibs and passed every check; their benchmark kernels ran at native speed (99–101%,
checksums match). Build 42 showed that iOS refuses `mmap(MAP_FIXED)` over pages dyld mapped,
hence the `__TEXT`/`__DATA` split.

**x18 at build time (build 44):** `pe2dylib.py convert` rewrites every x18 use in ARM64 code
(the ARM64EC code map excludes x64 thunks) into `b` to a signed trampoline placed just before
the image: push two scratch registers, `mrs TPIDRRO_EL0`, load the TEB from the TSD slot whose
offset the loader stores in the dylib, run the instruction on the scratch register, restore,
branch back. All 11,196 sites in Madeira's DLLs are handled; 141 of 149 DLLs are then ready
(7 D3D/Metal DLLs have one shared code/data page each, xtajit64 has FEX's TSD reads). Build 44
also loads Wine's real `ntdll.dll` this way (743 sites rewritten) and calls into it.

ARM64EC exports point at x64 "fast-forward" thunks in `.hexpthk`; a native caller
follows their `jmp` (as Wine's `arm64x_check_call` does). The spike loader does the same.

## Step A spike (build 42)

`engine/pedylib`:
- `spike/spike.c` is a small DLL built like Wine's (llvm-mingw, `-nostdlib`, 16 KB
  sections), for ARM64EC and ARM64. It reaches `.data`/`.rdata`/`.bss` with ADRP, has
  relocated pointer tables, imports from the app through the IAT, makes ARM64EC indirect
  calls through `__os_arm64x_check_icall`, and contains the Performance tab's benchmark
  kernels.
- `build-spike.sh` (Linux CI) builds it and converts each DLL with `pe2dylib.py convert --strict`.
- `link-dylibs.sh` (macOS CI) links the dylibs; the IPA step signs them into `PE/`.
- `App/Sources/Native/pe_dylib.c` loads them (see its header comment) and runs the checks
  and the kernels; Performance → *Windows code without JIT* → **Run DLL test**.
- `pe2dylib.py audit` runs on Madeira's real DLL farm in every IPA build and puts the
  blockers in the job summary (code/data page conflicts, relocations inside code, x18
  sites, TSD reads): the to-do list for rebuilding the real DLLs.

What the spike does not cover yet: x18/TEB (the spike DLL never touches the TEB), x64 code
(no emulator), and Wine itself.
