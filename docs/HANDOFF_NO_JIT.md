# Hand-off: Windows games without JIT (App Store path)

Self-contained notes for continuing this work in a new chat. Read this file, then
`docs/NO_JIT_WINDOWS.md` (plan + per-build findings), `docs/APP_STORE.md`,
`docs/FAST_INTERPRETER.md`, `docs/ROADMAP.md`.

## Goal
Windows games on iPhone **without JIT**, as fast as possible, shippable on the **App Store**
(guideline 4.7 allows PC emulators since July 2024; UTM SE is the precedent and also runs
without JIT). The owner has **written permission from Madeira's author** for App Store
distribution of Madeira's GPL code. Wine's LGPL still needs a relink-compliant setup before
submission. Without JIT only code inside the signed app bundle runs natively. Wine, D3D -> Metal
and everything else in the bundle stay native; the game's own x64 code runs in **FXI**, our
interpreter (8.1% of native on the A17 Pro, build 39; target 15-25% after phase 4). A JIT
sideload edition (FEX, ~90% of native) stays the full-speed option, from the same code.

## Environment and workflow
- Windows 11, PowerShell, no local compiler or Python: **everything builds in GitHub Actions**.
- git/gh are not on the tool shell's PATH: prepend `C:\Program Files\Git\cmd;C:\Program Files\GitHub CLI`
  (bash: `export PATH="/c/Program Files/Git/cmd:/c/Program Files/GitHub CLI:$PATH"`).
  gh is logged in as `aghjkshdsj` (repo + workflow scopes).
- The working copy is CRLF (autocrlf): edit existing files with the Edit tool, not sed/perl.
- Loop: edit -> commit -> push `main` -> watch CI -> release `build-N` holds the IPA.
  - `wine-unix.yml`: Madeira's Wine unix side + our patches (`engine/wine/patches/*.py`, applied in
    `engine/wine/build-unix.sh` step_ntdll after `git checkout` of the patched files). ~3 min; on
    success it dispatches `build-ipa.yml`, which uses the newest successful Wine libs.
  - `build-ipa.yml`: guests job (Linux: x86 guests, llvm-mingw spike/emulator/hello DLLs) +
    iOS job (FEX, Blink, Wine staging, PE->dylib conversion of the whole DLL farm, Xcode). ~5 min.
    Uploads artifacts `guests`, `dsym`, `build-logs` (incl. `pe-audit.json`).
  - `fxi.yml`: FXI on Linux, every guest natively and under FXI, checksums must match.
  - `symbolicate.yml` (workflow_dispatch, inputs `run_id`, `offsets`): turns `.ips` imageOffsets
    of the MYIOSDECK image into function names with that run's dSYM (Wine libs have no line info).
- Device: iPhone 15 Pro Max (A17 Pro), iOS 27.0.1, sideloaded with iloader, paid dev account.
  The owner tests every build and sends `Documents/myiosdeck-log.txt` (shared from the app), the
  crash log the app offers after a crash (`myiosdeck-crash-log.txt`, ends with the crash-proof
  `Wine no-JIT trace` section), and `.ips` reports (Settings > Privacy & Security > Analytics &
  Improvements > Analytics Data). **Give a release link for every build and say exactly what to
  test and which log to send.** Before sending a build, verify the artifact (e.g. `unzip -l` the
  IPA, read exports/bytes of a DLL with PowerShell): that caught builds that would have crashed.
- Madeira (github.com/willfaust/Madeira) pinned in `engine/wine/PIN`; a full checkout lives at
  `C:\Users\Danial\AppData\Local\Temp\claude\md`, its Wine fork (sparse) at `...\claude\mw`,
  Madeira's FEX fork at `...\claude\fex` (ARM64EC frontend: `Source/Windows/ARM64EC/Module.S`,
  `Module.cpp` = the reference for Wine's emulator interface).

## State (2026-10-07, build 71)
- **FXI**: no-JIT x86-64 interpreter (`engine/fxi`), 8.1% of native on device. Since build 57 it
  also has a Windows mode (`fxi_win.c`): FS/GS segment slots (r[17]/r[18], `gs:[0x30]` = TEB),
  a thread-safe shared block cache, cached `ec_exit` blocks for jumps into native ARM64EC code.
- **Step A (Wine DLLs as signed code) proven, builds 43-44**: `engine/pedylib/pe2dylib.py`
  converts a PE image into a dylib: `__TEXT` [x18 trampolines][PE headers + code] then `__DATA`
  [rest][TEB TSD-offset word]. Every x18 (TEB) use in ARM64 code is rewritten at build time into
  a trampoline that reads the TEB from TPIDRRO_EL0's TSD slot; 11,196/11,196 sites in Madeira's
  DLLs handled; 150 of 167 farm PE files convert (the rest: x64 test exes, 7 D3D/Metal DLLs with
  one shared code/data page each, xtajit64 = FEX).
- **Step B (Wine maps from the signed dylibs) works for ARM64EC programs, builds 52-54**:
  `engine/wine/patches/nojit_dylib.py` patches Madeira's `virtual_ios.c` (active only with
  `WINE_IOS_NOJIT=1`): `virtual_map_image` maps from `<PE dir>/lib<name>.dylib`; `mprotect_exec`
  makes code-page requests no-ops and serves exec requests outside signed images without
  PROT_EXEC (logged); every `[nojit]` line also goes to `MYIOSDECK_NOJIT_TRACE` (crash-proof).
  `hello-arm64ec.exe` (llvm-mingw ARM64EC, normal CRT) ran start to finish with JIT off,
  native-speed loop, exit code 42 (`[program]` lines: it writes `C:\myiosdeck-output.txt`, which
  `WineController.watch` copies into the log).
  `engine/wine/patches/putenv_lifetime.py` fixes Madeira bugs that crashed iOS after a Windows
  program exited: `putenv` with stack buffers and `setprogname` with a pointer into the Wine
  thread's stack (iOS has no exec).
- **Step D part 1 (FXI = Wine's x64 CPU) works, build 57**: the emulator DLL Wine loads as
  xtajit64.dll (`engine/pedylib/emu/xtajit64_stub.c`) exports ExitToX64 / DispatchJump /
  RetToEntryThunk / BeginSimulation as DATA (real code addresses, jumps through
  `MyiosdeckFxiHost`, which the map hook fills with `mid_fxi_win_host_table()`), and
  ProcessInit/ThreadInit/BTCpu64IsProcessorFeaturePresent via a plain `blr` into the app.
  App side: `App/Sources/Native/fxi_win_glue.S` (transitions in the ARM64EC register mapping:
  RAX=x8 RCX=x0 RDX=x1 RBX=x27 RSP=sp RBP=x29 RSI=x25 RDI=x26 R8-R11=x2-x5 R12-R15=x19-x22,
  XMM0-15=v0-v15, x9 = target; CPU area = TEB+0x1788: +0 InSimulation, +8 EmulatorStackBase,
  +0x18 ContextAmd64, +0x30 EmulatorData[0] = our FxiCpu; entry thunk = target + [target-4];
  a return into an exit thunk is recognised by `blr x16` at [target-4]) and
  `fxi_win_host.c` (per-thread FxiCpu, errors end the Windows program via Wine's unix
  NtTerminateProcess with 0xE0F0F001, not the app).
  **Build 57 result**: Library > "Windows Hello (x64, no JIT)" ran hello-x64.exe's x64 TLS
  callback through ExitToX64 in FXI and stopped cleanly at **`lock cmpxchg [rbx], rsi`**
  (`f0 48 0f b1 33`, unimplemented). Everything around it worked.
- **Build 60 (FXI 0.2)**: atomics in `engine/fxi/fxi_atomic.c`: cmpxchg, xadd, xchg m, cmpxchg8b/16b,
  lock add/or/adc/sbb/and/sub/xor/inc/dec/not/neg on memory, bt/bts/btr/btc (reg + mem, signed bit
  offsets), all memory forms real seq-cst host atomics (misaligned: one global split lock).
  `engine/guest/atomics.c` checks results + flags against native in fxi.yml (byte-identical).
- **`fxi --scan-pe <exe/dll>`** (fxi.yml step "Scan Windows x64 test programs"): decodes every
  `.pdata` function with FXI's decoder (an independent length decoder keeps the sweep in step)
  and lists unimplemented instructions with counts. Result: hello, fib, clocktest, heap, fileio,
  calltest, fpconf, child-test, d3d12-cube decode 100%; cube-x64 needs x87 (db/d9/dd: long-double
  printf in the mingw CRT). Use it on any game exe/DLL before a device round.
- **Build 60 result**: past `lock cmpxchg`; TLS callback stopped at `fninit` (`db e3`, mingw's FPU
  reset, a leaf function without .pdata, so the first scanner missed it).
- **Build 61 (FXI 0.3)**: x87 in `engine/fxi/fxi_x87.c` (values as double, 80-bit load/store,
  rounding control, fcomi/fcmov/fxam, transcendentals, fnsave/fnstenv), 0F AE (fxsave/fxrstor,
  ldmxcsr/stmxcsr, fences), fwait, shld/shrd; the CONTEXT's FltSave now loads the x87 state. Guest
  `engine/guest/x87.c` matches native. The scanner now also follows call/jmp targets from the entry
  point (leaf functions): all 10 Madeira x64 test programs, cube-x64 included, decode 100%.
  `fxi-next` is the side branch for FXI work between device builds (`gh workflow run fxi.yml --ref fxi-next`).
- **Build 61 result: Windows Hello (x64) runs with JIT off** (2026-10-07): TLS callbacks and main
  in FXI, "Hello from x86_64 PE ..." printed, exit code 42, app stays up.
- **Build 63**: x64 exceptions. Each uop carries its instruction address; `fxi_ea` (and the stack
  and string ops, FXI_TOUCH) record the uop that touches memory, so a host fault has an exact rip
  (stack ops change RSP after the access). int3 / int n / hlt / ud2 / #DE raise Windows exceptions
  (FXI_STOP_EXCEPTION). The emulator DLL passes ntdll's native KiUserExceptionDispatcher at
  ProcessInit (export = ffwd thunk, followed) and forwards ResetToConsistentState; the app
  (`fxi_win_host.c` raise_x64) packs the x64 state into an ARM64EC context and jumps to the
  dispatcher on the guest stack (FEX's RethrowGuestException scheme). Threads: the indirect
  inline cache is one pointer checked against the block's rip (was a two-field race); links are
  release stores. Cost on CI: integer/memory kernels about -9% (64-byte uops + the cur store).
  New Library entry **"x64 test suite (no JIT)"** (`engine/pedylib/hello/suite-x64.c`, built
  in the guests job, scanned by fxi --scan-pe: 47k instructions, 100% decoded): callbacks,
  float, threads, raise, int3, divide, fault, c++, longjmp, child; `[suite]` lines in the log.

- **Build 63 result**: every Windows program died in ProcessInit (AV 0xC0000005 at
  `host_process_init+0x14`): the emulator DLL passed KiUserExceptionDispatcher's address as a
  Windows `long` (32 bits). Following the ffwd thunk itself was right (x9 held the real address,
  first word d11343ff). **Build 64** passes 64-bit values (`hostarg`) and the app rejects a
  dispatcher below 4 GB or not starting with d11343ff (x64 exceptions disabled, logged).
- **D3D DLLs (step 5) findings**: the 6 D3D/Metal DLLs fail because 4 KB sections put the tail of
  `.text` and the start of `.rdata` (lld puts the IAT there, written by Wine's loader) in one 16 KB
  page; signed code pages cannot be written, so they must be rebuilt with 16 KB alignment.
  `madeira_d3d12.dll` / `d3d12.dll` / `d3d12core.dll`: plain llvm-mingw (Madeira
  `build/madeira-d3d12/build-pe.sh`; needs a winemetal import lib, e.g. llvm-dlltool from a .def
  of the prebuilt winemetal.dll) -> easy in the guests job. `d3d11` / `d3d10core` / `winemetal`:
  DXMT's meson PE build (`-Dwine_build_path=` Wine's ARM64EC PE build tree, cross file
  `build-arm64ec-win.txt`, Madeira docs/BUILDING.md step 4) -> needs Wine's PE side in CI.

- **Build 64 result: x64 test suite 9/10 with JIT off** (callbacks, float, threads 400000,
  raise, int3, divide, fault at the exact rip via a host Mach fault, c++, longjmp). `child`
  failed: Madeira's child ntdll pool copy fails without JIT ("no parent mapping found"), the child
  shared the parent's ntdll and crashed in its loader; the parent then hung at exit.
- **Build 66 (step 4)**: `App/Sources/Native/pe_instance.c` maps another instance of a signed PE
  dylib like dyld (F_ADDFILESIGS_RETURN, __TEXT r-x from the file, fresh __DATA from the file;
  the dylibs have no dyld fixups). nojit_dylib.py: a second mapping of a DLL gets a new instance
  (`[nojit] <dll>: instance #N`). `engine/wine/patches/nojit_child.py`: without JIT every child
  takes Madeira's private-ntdll path (ios_load_child_ec_ntdll -> ios_set_proc_ntdll). Performance
  tab "Run DLL test" also runs a second instance of the spike DLL (the iOS question: may the app
  map signed code from a bundle file a second time?).

- **Build 66 result: x64 test suite 10/10 with JIT off.** The child pseudo-process loaded its own
  instances of ntdll, xtajit64, kernel32, kernelbase, ucrtbase (`[nojit] ... instance #2`,
  F_ADDFILESIGS_RETURN ok), ran and exited 33; the suite exited normally. iOS allows mapping
  signed bundle code a second time.
- **Build 70 (step 5)**: `engine/dxmt/build-pe.sh` + `.github/workflows/dxmt-pe.yml` (macOS):
  Wine ARM64EC tree (`--enable-archs=arm64ec`, `make -C libs/winecrt0 / dlls/ntdll /
  dlls/dbghelp`), DXMT meson with a cross file adding `--section-alignment=0x4000` ->
  winemetal, d3d11, d3d10core, dxgi; Madeira's madeira_d3d12 (= d3d12) and d3d12core the same way.
  All 7 have SectionAlignment 16384; build-ipa.yml copies the newest successful set into the farm
  before conversion: 156 PE files become signed dylibs (only x64 test exes + tftrace left).
  Library: "Direct3D 11 cube (x64, no JIT)" and "Direct3D 12 cube (x64, no JIT)".
- **Build 70 result**: x64 test suite 10/10 again; **Direct3D 12 cube runs with JIT off** (x64
  program code in FXI, signed D3D12/Metal DLLs). D3D11 cube: DLL_NOT_FOUND, the meson build
  linked d3d11/dxgi against `libc++.dll` / `libunwind.dll`, which the prefix lacks. Fixed:
  `-static` in the cross file's link args, fresh meson build dir (`DXMT_BUILD`, bump it when the
  link options change: the cached dir keeps old options), and `build-pe.sh collect` fails when a
  DLL imports a toolchain runtime DLL (only api-ms-win-crt-*, kernel32, ntdll, user32, gdi32,
  advapi32, winemetal, dxgi/d3d11 remain).
- **FXR in the app**: `engine/fxr` (from `claude/fxr`, another session's work) is FXI with the 16
  guest GPRs, an EA temporary and the lazy-flag words passed as `preserve_none` arguments (host
  registers) between musttail handlers; hot forms specialised per register, everything else via
  FXI's handler (`p_slow`, spills around the call). CI aarch64: 9.25% of native vs FXI 7.50%
  (x86 hosts lack the argument registers: slower there). Its FXI copy is renamed to fxr_*
  (`engine/fxr/fxr_rename.h`, included from its `fxi.h`; fxr.yml's iOS job fails on any global
  `fxi_*` symbol); its headers are excluded from the Xcode project (same names as FXI's, header
  map). Performance tab benchmark has an FXR column and the report `fxr%` + "FXR vs FXI";
  Settings > Without JIT runs x86-64 Hello with FXR. **ELF only**: pinned handlers do not set
  `c->cur` (no exact host-fault rip) and PIND writes `u->link` non-atomically, so Windows mode
  stays on FXI until those are fixed. engine/fxr duplicates engine/fxi: port FXI fixes to both,
  or fold FXR back into engine/fxi once it covers Windows mode.
- Other branches in the repo: `claude/fxr` (FXR's origin), `gpt-astra/interp`.

## Next steps (in order)
1. FXI instructions for real Windows x64 code, driven by the STOP lines and `fxi --scan-pe`.
   Atomics (build 60), x87 + shld/shrd + 0F AE (build 61) done. Known FXI gaps: no 32-bit
   addressing, few SSE3+/SSSE3/SSE4, no AVX, no self-modifying-code invalidation.
   Rebuild loop for FXI alone: `fxi.yml` (Linux checksums) before the IPA.
2. x64 exceptions (build 63): confirm on device with the suite; rep-string faults are not exact.
3. Threads (build 63 suite checks them), NotifyMemoryProtect/Flush for code invalidation.
4. Child processes: one signed ntdll per pseudo-process (ship several separately signed ntdll
   dylibs; today a second mapping of a DLL returns "mapped again -- not supported").
5. D3D DLLs rebuilt with 16 KB sections (build 70: D3D12 runs; build 71: D3D11 static libc++). Next: a real D3D11 game.
6. FXI speed (phase 4 of docs/FAST_INTERPRETER.md): preserve_none dispatch, superinstructions,
   NEON SSE, guest register caching; cache ffwd thunks (x64 -> EC calls) as direct exits.
7. App Store flavour: IPA without FEX/StikDebug flow, no get-task-allow, distribution signing,
   Wine LGPL relink setup, then submission under 4.7.

## Hard-won iOS / Wine facts
- iOS refuses `mmap(MAP_FIXED)` over pages dyld mapped (EPERM): data must be in `__DATA` from the start.
- ARM64EC exports point at x64 fast-forward thunks (`48 8b c4 48 89 58 20 55 5d e9 rel32`).
- The TEB TSD slot offset differs per run/device (0x8c8, 0x8e0 seen; Madeira saw 0x898): runtime value.
- Wine's server answers a mapping off its preferred base with STATUS_IMAGE_NOT_AT_BASE; Wine then
  relocates (load_ntdll -> virtual_relocate_module; PE loader perform_relocations). Relocating
  in the map hook too applied pointers twice (build 49).
- The read-only PE header keeps its preferred ImageBase; code computing `ptr - ImageBase` after
  relocation breaks (update_arm64ec_ranges runs before relocation, fine).
- Madeira points the console handle at a file (type 1): console output is not in `[stdio]`.
- Crash logs: Wine's stderr reaches the app through a pipe whose tail is lost on a crash; use the
  nojit trace file and `.ips` + `symbolicate.yml`.

## Rules
- Never upload game binaries; no Steam emulators or license-check bypasses.
- Keep GPL/LGPL headers; note vendored/ported code in `THIRD-PARTY-NOTICES.md`.
- Every build gets a release link + exact test steps + which log to send.
- Be honest about what iOS allows; give a recommendation, not a menu.
