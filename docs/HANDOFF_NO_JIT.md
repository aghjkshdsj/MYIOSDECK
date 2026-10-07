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

## State (2026-10-07, build 61)
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

## Next steps (in order)
1. FXI instructions for real Windows x64 code, driven by the STOP lines and `fxi --scan-pe`.
   Atomics (build 60), x87 + shld/shrd + 0F AE (build 61) done. Known FXI gaps: no 32-bit
   addressing, few SSE3+/SSSE3/SSE4, no AVX, no self-modifying-code invalidation, no x64
   exceptions (int3, guest faults -> SEH).
   Rebuild loop for FXI alone: `fxi.yml` (Linux checksums) before the IPA.
2. Until hello-x64 prints: x64 SEH/unwinding and guest faults inside FXI (a host SIGSEGV while
   interpreting must become a Windows exception for the x64 code), FPCR/MXCSR, `syscall` never
   expected (x64 ntdll stubs jump to EC code).
3. Threads (ThreadInit per thread exists; check the inline-cache race in fxi_ops.c INDIRECT:
   imm/link written non-atomically), NotifyMemoryProtect/Flush for code invalidation.
4. Child processes: one signed ntdll per pseudo-process (ship several separately signed ntdll
   dylibs; today a second mapping of a DLL returns "mapped again -- not supported").
5. 16 KB-aligned rebuild of the 7 D3D/Metal DLLs (or of the whole PE side) so D3D games load.
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
