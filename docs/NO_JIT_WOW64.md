# 32-bit Windows games without JIT (WoW64)

Goal: 32-bit (i386, PE machine 0x14c) Windows games run with JIT off, as 64-bit games do
(docs/NO_JIT_WINDOWS.md). Test game: Forager (GameMaker, 32-bit, Direct3D 11). Today the app
refuses them (App/Sources/Engine/WineController.swift, "This is a 32-bit game...").

Sources this design is taken from, at the pins in engine/wine/PIN:
- Madeira docs/WOW64.md (65e6fe8f): the guest window, the launch path, unix calls, contexts.
- Wine fork (257f271c) dlls/wow64/syscall.c: how wow64.dll loads and calls the CPU module.
- Madeira's FEX fork (3bec2ac4) Source/Windows/WOW64/Module.cpp: the reference CPU module
  (`libwow64fex.dll`, shipped as `xtajit.dll`).

## 1. How it runs

Like Windows on ARM and like Madeira with JIT: a 32-bit process is a WoW64 process. Its
64-bit half is plain aarch64 Windows code (not ARM64EC); only x86 code is emulated.

| piece | what it is | without JIT |
|---|---|---|
| the game and Wine's 32-bit DLLs (`C:\windows\syswow64` = bundle `i386-windows`) | i386 code | **data** inside the guest window, interpreted by **FXI32** (FXI's i386 build) |
| 64-bit `ntdll.dll`, `wow64.dll`, `wow64win.dll`, every other DLL of the session (`system32` = bundle `aarch64-windows`) | native aarch64 | signed dylibs (`PE/wine-a64`), like the ARM64EC farm (`PE/wine`) |
| `xtajit.dll`, the WoW64 CPU module | FEX with JIT | **ours**: `engine/pedylib/emu/xtajit_wow.c`, an aarch64 PE from a signed dylib that forwards into the app |
| app side | | `App/Sources/Native/fxi_wow_host.c` + glue: per-thread FXI32 CPU, system calls, exceptions, contexts |
| unix side | Madeira's ntdll-unix (guest windows, `ios_wow.h`) | as today, plus the no-JIT patches (`engine/wine/patches`) |
| Direct3D 11 | DXMT's i386 `d3d11/dxgi/d3d10core/winemetal` (x86 code) + winemetal's 32-bit unix table | the i386 DLLs are interpreted; the Metal renderer behind the unix table is native |

**The guest window.** XNU's 4 GB `__PAGEZERO` forbids mappings below 4 GB, so every 32-bit
pseudo-process owns a reserved host range `[B, B + 4 GB)`; guest address `a` lives at host
`B + a`. Wine's unix side reserves it (`ios_wow_window_reserve`) and places TEB32/PEB32, the
32-bit stacks, the images and `KUSER_SHARED_DATA` (guest 0x7ffe0000) inside it.
`NtQueryInformationProcess(ProcessWineIosWowGuestBase = 1010)` returns `B` (0 for a 64-bit
process). Rules (Madeira's invariants, binding for us too):
1. Anything the x86 code observes (registers, EIP, the FS base, `WOW64_CONTEXT` fields, the BOP
   addresses) is a **guest** address, below 4 GB.
2. Anything native code dereferences is a **host** address. Guest to host is `+B` of the owning
   process. NULL stays NULL. Handles, sizes, flags are never offset.
3. wow64.dll converts every pointer it passes to us (`BTCpuNotify*` get host addresses).
4. Exception records keep host addresses until wow64.dll converts them once (`-B` on
   `ExceptionAddress` and an access violation's `ExceptionInformation[1]`).
5. **Nothing inside a window is executable.** x86 bytes are data the interpreter decodes.

**Performance, honestly.** On the x64 path only the game's own code is interpreted; Wine's DLLs
are native ARM64EC. Under WoW64 every 32-bit DLL is x86 code: kernel32, user32, the CRT, DXMT's
D3D11 frontend all run in the interpreter until they reach a system call. Expect a 32-bit game
to run clearly slower than a 64-bit game of the same weight at first. Stage 6 attacks this
(FXR32, native fast paths for hot 32-bit system functions, as FXR did for x64).

## 2. The interface

### 2.1 Wine -> CPU module (wow64.dll `process_init`)

wow64.dll loads `C:\windows\system32\xtajit.dll` (registry `HKLM\Software\Microsoft\Wow64\x86`,
default `xtajit.dll` on ARM64), resolves the exports below by name (`RtlFindExportedRoutineByName`;
missing ones stay NULL, wow64 checks the optional ones), calls `BTCpuProcessInit`, then per
thread `BTCpuThreadInit` and `BTCpuSimulate` in a loop (`cpu_simulate`). Without JIT the app
symlinks `system32\xtajit.dll` to our module (as it does for `xtajit64.dll` today), so the
server parses our PE's headers and the map hook maps our signed dylib.

| export | called by / when | contract | ours |
|---|---|---|---|
| `BTCpuProcessInit()` | process_init, once, after the module is loaded | set up everything; must not fail silently | query B; allocate the BOP page; `Wow64Info->CpuFlags = WOW64_CPUFLAGS_SOFTWARE` (enables the cross-process work list); resolve `Wow64SystemServiceEx`, `Wow64PassExceptionToGuest`, `Wow64RaiseException`, `Wow64ProcessPendingCrossProcessItems` (wow64.dll), 64-bit ntdll's `__wine_unix_call_dispatcher`, `LdrSystemDllInitBlock`; hand all of it to the app (`H_PROCESS_INIT`) |
| `BTCpuThreadInit()` | thread_init, every thread, before its first simulate | create the thread's CPU state | `H_THREAD_INIT(teb64)`: the app allocates an `FxiCpu` (i386) for the thread, keyed by TEB64 |
| `BTCpuGetBopCode()` | process_init, thread_init | **guest** address of the system-call entry | the BOP page's guest address `g` |
| `__wine_get_unix_opcode()` | process_init | **guest** address of the unix-call entry | `g + 2` |
| `BTCpuSimulate()` | `cpu_simulate` loop; nested from `Wow64KiUserCallbackDispatcher` and `Wow64ApcRoutine` | run x86 code from the thread's context; may return (the loop calls it again) or be left by longjmp/unwind | naked: capture the entry `CONTEXT` (RtlCaptureContext), `H_SIMULATE(entry_ctx)` |
| `BTCpuGetContext(thread, process, NULL, WOW64_CONTEXT*)` | wow64 internals only, always with `GetCurrentThread()` (exception dispatch, callbacks, APCs, the initial context); a 32-bit `NtGetContextThread` reads the CPU area directly (`RtlWow64GetThreadContext`) | current 32-bit state | `RtlWow64GetThreadContext`: every call comes while the thread is outside x86 code, when the CPU area is exact (2.3), so no call into the app (as wow64cpu) |
| `BTCpuSetContext(...)` | exception dispatch, NtContinue, callbacks, APCs, initial context | replace the 32-bit state | `RtlWow64SetThreadContext`, then `WOW64_CPURESERVED_FLAG_RESET_STATE` on the current thread's CPU area: the app reloads before the next x86 instruction (2.3) |
| `BTCpuResetToConsistentState(EXCEPTION_POINTERS*)` | `Wow64PrepareForException`, for every exception the 64-bit side dispatches | turn a fault inside the emulator into a guest exception | `H_RESET`: when the faulting pc is inside FXI32 (normally caught earlier, 2.5), rebuild the guest state and restore the entry context; else nothing |
| `BTCpuSuspendLocalThread(thread, count)` | not called by this Wine (wow64's NtSuspendThread is a plain one) | | `NtSuspendThread` |
| `BTCpuFlushInstructionCache2/Heavy(addr, size)` | NtFlushInstructionCache, cross-process work | host address | `H_INVALIDATE(addr - B, size)` (Heavy with NULL,0 = everything) |
| `BTCpuNotifyMemoryFree/Protect` (`after` = 0 then 1) | NtFreeVirtualMemory, NtProtectVirtualMemory | host address | after, on success: invalidate decoded blocks in the range |
| `BTCpuNotifyMemoryAlloc/Dirty` | NtAllocateVirtualMemory, cross-process writes | host address | Dirty: invalidate; Alloc: nothing |
| `BTCpuNotifyMapViewOfSection` / `UnmapViewOfSection` | image views | host address | Unmap: invalidate the view |
| `BTCpuNotifyReadFile(handle, addr, size, after, status)` | NtReadFile into guest memory | | after: invalidate the range (code loaded by ReadFile) |
| `BTCpuNotifyProcessExecuteFlagsChange` | DEP changes (Madeira's addition) | | nothing: FXI32 executes any readable page |
| `BTCpuIsProcessorFeaturePresent(feature)` | IsProcessorFeaturePresent from 32-bit code | what the emulated CPU has | FXI32's feature set (SSE, SSE2, SSE3 when implemented; no AVX) |
| `BTCpuUpdateProcessorInformation(SYSTEM_CPU_INFORMATION*)` | NtQuerySystemInformation | | x86 family/model the CPUID instruction reports |
| `BTCpuProcessTerm`, `BTCpuThreadTerm` | exit | | `H_THREAD_TERM` frees the thread's CPU on its own thread only (iOS cannot really suspend another thread: Madeira ml1200) |

`wow64.dll` also looks up `KiRaiseUserExceptionDispatcher`, `__wine_syscall_dispatcher`,
`__wine_unix_call_dispatcher` in the **32-bit** ntdll and stores `g` / `g + 2` there and in
`Wow64Transition` and `TEB32->WOW32Reserved`. It translates every resolved export pointer with
`MemoryWineIosJitPoolAddress` (1005); without JIT that query fails and the pointer stays as is.

### 2.2 CPU module <-> app

Same scheme as the x64 emulator (`engine/pedylib/emu/xtajit64_stub.c`): the module exports a
data slot `MyiosdeckWowHost`; the map hook (`nojit_dylib.py`) stores the app's table
`mid_fxi_wow_host_table(tsd_offset)` into it when it maps `xtajit.dll` for an ARM64 image. Calls
from the module into the app are plain `blr` with 64-bit arguments (the Apple arm64 ABI and the
Windows arm64 ABI agree on x0-x7/v0-v7 and callee-saved x19-x28; x18 is never used by either
side's interface). Slots: `H_PROCESS_INIT, H_THREAD_INIT, H_THREAD_TERM, H_SIMULATE, H_RESET,
H_INVALIDATE, H_FEATURE, H_CPU_INFO`. Contexts never need the app: the CPU area is the
hand-over (2.4), and the module reads and writes it with Wine's own functions.

Calls from the app back into Windows code (system calls, unix calls, exceptions) go through one
export of the module, `MyiosdeckWowCall(fn, a0, a1, a2)`: FEX's `SEHFrameTrampoline2Args`. Its
unwind information says "the caller's frame is the context captured at `BTCpuSimulate` entry"
(`.seh_pushframe` over the saved Sp/Pc), so Windows unwinding (wow64's `longjmp` from
`NtCallbackReturn`, `RtlUnwind` during exception dispatch) steps from Windows code straight to
`BTCpuSimulate`, over the app's frames, which have no Windows unwind information. The entry
context is per thread and per nesting level (saved and restored around each call, as FEX does).

### 2.3 x86 code -> system calls and unix calls

- **BOP page**: `NtAllocateVirtualMemory(zero_bits = 0x7fffffff, PAGE_READWRITE)` places one page
  inside the window (a sub-4 GB ceiling is a guest request, `ios_wow_translate_limits`). Bytes
  `cd 2e cd 2e` (FEX's choice, `int 0x2e` twice). Never executable: FXI32 recognises the two
  addresses before decoding (a lookup of `g` or `g + 2` returns the system-call / unix-call block).
- **System call**: 32-bit ntdll's stubs do `mov eax, N ; call [__wine_syscall_dispatcher]`.
  At `g`: `[esp]` = return address into the stub, arguments at `esp + 8`.
  The app calls `Wow64SystemServiceEx(eax, host(esp + 8))` through `MyiosdeckWowCall`; then
  `eax = status, eip = [esp], esp += 4`.
- **Unix call**: `__wine_unix_call_dispatcher(handle, code, args)` from 32-bit code. At `g + 2`:
  `[esp]` = return address, then `{ u64 handle; u32 code; u32 args }`. The app calls the 64-bit
  ntdll's `__wine_unix_call_dispatcher(handle, code, host(args))`; the unix library's
  `*_unix_call_wow64_funcs` table converts pointers inside `args` (Madeira, WOW64.md section 4).
  Then `eax = status, eip = [esp], esp += 4 + 16`.
- **Around each call**, as wow64cpu and Madeira's FEX module do: the thread's whole 32-bit state
  goes to the CPU area (2.4) with `eip = g` (or `g + 2`); after the call, if `BTCpuSetContext`
  ran or the CPU area carries `WOW64_CPURESERVED_FLAG_RESET_STATE`, the state is reloaded from
  the CPU area; if `eip` is still `g` / `g + 2` the call returns normally as above, otherwise
  (NtContinue, an exception or APC dispatched, a callback returned by longjmp) execution goes on
  at the reloaded `eip`. `Wow64ProcessPendingCrossProcessItems()` runs before each system call.

### 2.4 Thread state: the CPU area

`TEB64->TlsSlots[WOW64_TLS_CPURESERVED]` points at `WOW64_CPURESERVED { USHORT Flags; USHORT
Machine = 0x14c; }` followed by an `I386_CONTEXT` (`RtlWow64GetCurrentCpuArea`). Wine's
`RtlWow64Get/SetThreadContext` read and write it (for the current thread without a server
round trip; for another thread Madeira's server applies a Set at resume, WOW64.md section 5).
FXI32 keeps the live state in its `FxiCpu`; the CPU area is exact whenever the thread is outside
x86 code (in a system call, unix call or callback). Another thread's `GetThreadContext` on a
thread that is running x86 code sees the state of its last system call (as with Madeira's FEX
module; iOS has no real suspend). The initial context of a new thread is in the CPU area when
`BTCpuThreadInit` runs (wow64's thread_init then sets EIP to the 32-bit `LdrInitializeThunk`).
`FS` base = TEB32 guest address = `TEB64 + TEB64->WowTebOffset - B`.

TLS slots of TEB64 we may use: 14 and 15 (Madeira found 16 is ntdll's errno cell). The app keys
its per-thread CPU on TEB64 and keeps the pointer in slot 14.

### 2.5 Exceptions

- **Raised by an instruction** (int3, int 0x2d, int 0x29 fastfail, ud2, #DE, into, bound): FXI32
  stops before (or for int3 after) the instruction; the app stores the state to the CPU area and
  calls `Wow64RaiseException(int_no, &rec)` through `MyiosdeckWowCall`. wow64 builds the record
  (codes in syscall.c: 0 #DE, 3 breakpoint, 4 overflow, 5 bounds, 6 #UD, 0x29 fastfail, 0x2d
  debug service), dispatches it to the 32-bit `KiUserExceptionDispatcher` with `BTCpuSetContext`,
  and the app goes on there.
- **A host fault in guest memory** (B + ea not mapped / not writable): unlike FXI's x64 mode,
  which runs on a separate emulator stack (hence `nojit_fault_hook.py`), FXI32 runs on the
  thread's own stack inside BTCpuSimulate, so Madeira's server delivers the fault normally to
  the 64-bit KiUserExceptionDispatcher, which calls `BTCpuResetToConsistentState` first. When the
  thread was interpreting x86 code, the app puts the state at the faulting instruction (exact EIP
  from the uop that touched memory, `c->cur`) into the CPU area, sets ExceptionAddress to
  B + EIP (an execute fault: info = { 8, B + EIP }) and replaces the dispatch context with the
  one BTCpuSimulate captured. Dispatch then reaches wow64's handler on `cpu_simulate`, which
  passes the exception to the 32-bit dispatcher (`Wow64PassExceptionToGuest`) and unwinds back
  into the simulate loop: FEX's scheme exactly.
- **Exceptions in native 64-bit code** take Wine's normal path; `BTCpuResetToConsistentState`
  has nothing to do for them.

### 2.6 Callbacks and APCs

`Wow64KiUserCallbackDispatcher` (window procedures, D3D/Metal callbacks) and `Wow64ApcRoutine`
set a new 32-bit context and call `cpu_simulate` again: a **nested** `BTCpuSimulate` on the same
thread, using the same `FxiCpu`. A callback ends with `NtCallbackReturn`, a system call whose
wow64 side `longjmp`s back into `Wow64KiUserCallbackDispatcher`, which restores the original
context with `BTCpuSetContext`. So the inner run's C frames are abandoned (allowed: FXI32 holds
no locks across a system call) and the outer system call returns into a reloaded state (2.3).

### 2.7 Code invalidation

FXI32 caches decoded blocks by guest EIP. Unmapping, freeing, re-protecting, flushing or
ReadFile into a range drops the blocks that start in it (a generation per 64 KB page; a block
records the generations of the pages it covers). Self-modifying code without any of those calls
(a game patching its own code in place) is not detected yet; GameMaker's runner does not do it.

### 2.8 FXI32: the interpreter's i386 mode

engine/fxi compiled a second time with `-DFXI_I386=1` and a rename header (`fx32_*`, like
engine/fxr/fxr_rename.h): the x64 FXI and FXR objects stay byte-identical. Differences:
- **Memory**: every access goes to `B + (uint32_t)ea`, stack (push/pop/call/ret, 4 bytes),
  strings (esi/edi, ecx), code fetch (`B + eip`). `B` is a field of the CPU (per process).
- **Decoder**: operand and address size 32 by default; `66` -> 16-bit operand, `67` -> 16-bit
  addressing (ModRM 16-bit forms); no REX: `40-4F` are inc/dec r32; `mod=00 rm=101` is
  `[disp32]` (absolute, not RIP-relative); `push/pop` of segment registers, `pushad/popad`,
  `pushfd/popfd`, `enter/leave`, `daa/das/aaa/aas/aam/aad`, `bound`, `into`, `arpl` (63), far
  `call/jmp/ret` (only to stop with a clear error: Win32 code never uses them), `lds/les`.
  `fs:` adds the TEB32 guest address; other segment bases are 0.
- **Registers**: 8 GPRs; 32-bit results zero the upper half of the 64-bit slot (FXI's register
  file stays 64-bit, only the low 32 bits are architectural).
- **x87** as FXI's (values as double; 32-bit code uses x87 for all float math), SSE/SSE2 as FXI's.
- **Windows mode** like FXI's x64 mode minus the ARM64EC exits: a run ends at `g`/`g + 2`, at an
  exception, or at a fault; there is no native code to call into from x86.

### 2.9 The map hook, per machine

`engine/wine/patches/nojit_dylib.py` today finds `<MYIOSDECK_PE_DIR>/lib<name>.dylib` by file
name alone. Under WoW64 three images can share a name (`ntdll.dll`: ARM64EC, aarch64, i386), so
the lookup uses the image's machine from the server's `pe_image_info`:
- `IMAGE_FILE_MACHINE_AMD64` (ARM64EC images): `MYIOSDECK_PE_DIR` (`PE/wine`), exactly as before.
- `IMAGE_FILE_MACHINE_ARM64` (aarch64 images): `MYIOSDECK_PE_DIR_A64` (`PE/wine-a64`).
- anything else (i386): never from a dylib; Wine maps it into the window as data.
Loaded images are keyed by (machine, name). Inside a guest window, executable protection is
never requested from the host (no log line, no JIT pool).

### 2.10 Bundle

| folder | content | size (raw) |
|---|---|---|
| `arm64ec-windows`, `PE/wine` | as today | 158 MB + dylibs |
| `aarch64-windows` | Madeira's prebuilt aarch64 farm (125 of 135 files have 64 KB sections, so they convert as they are; the rest are test programs, the aarch64 D3D DLLs and FEX's `xtajit.dll`, none needed) | 116 MB |
| `PE/wine-a64` | the aarch64 farm as signed dylibs, plus our `xtajit.dll` | ~116 MB |
| `i386-windows` | Wine's i386 farm (built in CI, `wine-i386.yml`) + DXMT i386 | measured in stage 1 |

Madeira's launch path does the rest once the bundle has `i386-windows/ntdll.dll`: it reads the
target's PE machine, picks the aarch64 farm for system32, links `syswow64` to the i386 farm,
seeds the x86 side-by-side store and sets `ios_main_image_i386` so the unix side reserves the
main window early (WoW64.md section 3).

## 3. Stages

Each stage keeps all CI gates green (fxi.yml, fxr.yml difftest / Windows-mode gate / speed) and
leaves 64-bit games unchanged: every new path is keyed on an i386 process, an ARM64 image or the
new bundle folders.

### Stage 1: the WoW64 pieces in the bundle
- `engine/wine/build-i386.sh` + `.github/workflows/wine-i386.yml` (macOS): Wine's i386 farm from
  the pinned Wine fork (Madeira's build/wine-i386/build.sh: every i386 module minus the skip
  list), DXMT's i386 d3d11/dxgi/d3d10core/winemetal and the D3D9 shim, Madeira's `hello-x86.exe`;
  import closure check (fails on a missing import); artifact `wine-i386-farm`.
- build-ipa.yml: the newest i386 farm into the bundle (`i386-windows`); the aarch64 farm into the
  bundle and converted to signed dylibs (`PE/wine-a64`); the IPA size in the job summary.
- nojit_dylib.py: the per-machine lookup (2.9).
- A test program `hello-aarch64.exe` (the ARM64EC hello built for plain ARM64) and a Library
  entry "Windows Hello (ARM64, no JIT)": a plain aarch64 Wine session, the 64-bit half of WoW64,
  from signed dylibs.
- CI: wine-i386.yml (modules built, 0 missing imports), build-ipa summary (aarch64 files
  converted, IPA size).
- **Phone**: Library > Windows Hello (ARM64, no JIT): exit code 42 and `[program]` lines. Then the
  usual no-JIT checks (x64 test suite 10/10, D3D11 cube, one x64 game) to confirm nothing changed.
  The log's `[WineProc]` lines now show the syswow64/sysaa64 links.

### Stage 2: FXI32 in CI
**Status (2026-10-09, run 37992360512):** fxi32 builds (symbols renamed, checked), difftest32's
163 forms match native exactly, bench matches native in every kernel for both the SSE2 and the
x87 float build (3.3-9.3% of native on the x86-64 runner), and the i386 farm's kernel32,
kernelbase, user32, DXMT d3d11/dxgi/winemetal and hello-x86.exe decode 100%.
- engine/fxi i386 build (2.8) as a Linux host tool `fxi32` that runs static i386 ELF guests
  (`int 0x80` syscalls) inside a window at a high `B` (mmap'ed 4 GB reservation), so a missing
  `+B` faults instead of passing by accident.
- engine/guest: i386 builds (`-m32`) of hello, difftest (the 32-bit forms, plus the i386-only
  ones: inc/dec 40-4F, pushad/popad, 16-bit addressing, daa/aaa, x87 through memory), x87,
  atomics, bench_sse2. The x86-64 runners run i686 code natively: output must equal native line
  for line, as for FXI.
- fxi.yml job "FXI32", and `fxi32 --scan-pe` for PE32 files (the i386 farm's kernel32, user32,
  ntdll, ucrtbase, msvcrt, DXMT d3d11): every reachable instruction decodes.
- **Phone**: nothing new to run (optional: the Performance tab's benchmark under FXI32).

### Stage 3: the CPU module and the app host
- `engine/pedylib/emu/xtajit_wow.c` (aarch64 PE, 16 KB sections, exports of 2.1,
  `MyiosdeckWowHost`, `MyiosdeckWowCall`; imports only ntdll, through
  `xtajit_wow_ntdll.def`), converted with `--strict`; build-spike.sh fails when an export is
  missing.
- `engine/fxi32/fxi_wow.c` (API `engine/fxi32/fx32.h`): a CPU per thread, one block cache per
  process (by window), run until the BOP page / an exception / an error, I386_CONTEXT load and
  save (ExtendedRegisters = fxsave; FloatSave derived from it).
- `App/Sources/Native/fxi_wow_host.c`: 2.2-2.6. A fault while interpreting needs no Mach-hook
  branch: FXI32 runs on the thread's own stack inside BTCpuSimulate, so Wine delivers it
  normally and `BTCpuResetToConsistentState` turns it into the guest's (state at the faulting
  instruction into the CPU area, dispatch restarted from BTCpuSimulate's captured context).
- The map hook installs the table for `xtajit.dll` (ARM64); the app links `system32\xtajit.dll`
  to our module without JIT (`MYIOSDECK_NOJIT_WOW_CPU`); `hello-i386.exe` (the hello source built
  for i686) goes into the i386 farm.
- CI: `llvm-readobj --coff-exports` must list every BTCpu export (build-spike.sh);
  `engine/fxi32/wow_test.c` (fxi.yml) drives FXI32's WoW64 API as the host does, in a real
  4 GB window: the system-call and unix-call stops with Wine's stack layouts, the context round
  trip, `fs:` to the TEB32, int3, #DE, a fault at the exact EIP, ud2. **All pass (run
  37995496475).** Not covered yet: nested simulation (a callback), invalidation.
- **Phone**: Library > "Windows Hello (x86 32-bit, no JIT)" (hello-i386.exe): the log shows
  `PE probe: machine=0x14c (i386: WoW64)`, `[nojit] xtajit.dll: host table ... (FXI32 is the x86
  CPU)`, `[fxi-wow] first x86 code`, the `[program]` lines ("Hello from Windows (x86, 32-bit)")
  and exit code 42.

### Stage 4: 32-bit programs in the app
- WineController: a 32-bit game starts without JIT when the bundle has the WoW64 set (no refusal);
  Settings texts that say 32-bit needs JIT.
- `engine/pedylib/hello/suite-x86.c`: the x64 suite for i386 (callbacks through a window
  procedure, x87/SSE float, threads, RaiseException/int3/divide/fault, C++ throw, longjmp, a
  32-bit child process); Library > "x86 test suite (no JIT)".
- **Phone**: the suite n/n; an added 32-bit game gets past the refusal (log `[games] start ...
  machine=0x14c`).

### Stage 5: Forager
- Copy the game folder into Files > MYIOSDECK > Games, add it in the Library, start it without
  JIT. Iterate on STOP lines, `[wow-*]` and `[nojit]` lines: missing instructions (`fxi32
  --scan-pe` on Forager.exe locally first: the binary is never uploaded), D3D11 through DXMT's
  i386 DLLs and winemetal's 32-bit table, audio (XAudio2/DirectSound 32-bit), input.
- **Phone**: Forager's title screen, then play; FPS from the overlay.

### Stage 6: speed
- FXR32 (guest registers pinned, as FXR), native fast paths for hot 32-bit system functions
  (memcpy/memset/strlen in the CRT, critical sections, TlsGetValue), measured with the profiler
  on Forager.

## 4. Risks and open questions

1. Madeira's unix side has WoW64 paths written for the JIT pool (anonymous RWX memory in a window
   keeps a pool alias for managed runtimes, ml1279). Without JIT the map hook gives window
   memory without execute permission before those paths are reached; watch for `[jit-pool]` or
   pool-alias lines in stage 3 logs.
2. Another thread's GetThreadContext sees the last system-call state, not the live one (same as
   Madeira with FEX on iOS). Mono/.NET-style stop-the-world collectors may need more (later).
3. x87 as double: GML numbers are doubles, so results match; 80-bit extended intermediates
   differ (Madeira's FEX makes the same trade for 32-bit guests by default, ml950).
4. Everything 32-bit is interpreted (section 1): the first Forager runs will be slow.
5. The Forager path in the task (C:\Games\Forager.v4.1.9\...) is not on the build PC; the game is
   only ever copied to the phone, never into the repository or CI.
