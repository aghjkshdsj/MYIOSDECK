# Another game without restarting MYIOSDECK

Question: after one Windows game ends, can a second one start without the user closing and
reopening MYIOSDECK? Until now the answer was no: `WineController.run()` refused every start once
a session had finished ("MYIOSDECK runs one Windows game per launch"), and the game menu's only
way out was "Quit game and close MYIOSDECK" (`CrashReporter.quitApp`, `exit(0)`). Madeira
(`app/Madeira/Library.swift`, `LibraryModel.sessionsThisRun`) has the same rule.

**Finding: feasible, by keeping the Wine session and starting the next game inside it** (design
B below). Tearing the session down and starting a fresh one in the same process (design A) is
not. Design B is implemented behind a setting, Settings › Without JIT › **Play another game
without restarting (experimental)**, off by default. With it off nothing changes.

Pins: Madeira 65e6fe8f, its Wine fork 257f271c (`engine/wine/PIN`).

## 1. What a second `__wine_main` in the same process would hit

Madeira runs Wine in the app process: the wineserver is a thread (`WineServerBridge.m`), and
every Windows process is a "pseudo-process", a set of threads (`WineProcessBridge.m` runs
`__wine_main` for the first one; `process_ios.c` `spawn_process` starts a thread running
`wine_ios_child_main` for each child). Today a session ends with `wineserver_stop()`.

**wineserver.** A second `wineserver_start()` cannot work:
- `g_wineserver_should_stop` (WineServerBridge.m) is set by `wineserver_stop()` and never cleared:
  a second server thread would leave its loop on the first iteration (`fd_ios.c`).
- `wineserver_main` (`main_ios.c`) runs `init_directories`, `init_registry` and friends again.
  The first server's object tree is still there (the registry root, `\KernelObjects`, the
  directories are permanent objects), so `init_registry` aborts on "\Registry", which is what
  Madeira's comment reports. `fatal_error` ends only the server thread (`pthread_exit`), and the
  half-made server stays in memory.
- The server's statics (handle tables, the desktop and window station, atoms, the HID pad of
  `hidpad_ios.c`, `user_shared_data`) have no teardown at all: upstream Wine ends them with
  `exit()`.

**ntdll's unix side** (`libntdll_unix.a`, about 70,000 lines of Madeira's `*_ios.c`):
- `virtual_init` (`virtual_ios.c`, 26,000 lines) reserves the address space once: the guest
  window band and its placeholders, the FEX arena, the jumbo holdback, the `0x140000000` exe
  window, the view tree. Running it again forgets every view of the first session while their
  memory stays mapped, and the reservations collide with themselves.
- `start_main_thread` (`loader_ios.c`) makes the first TEB and PEB, creates the TEB TLS key and
  finds its raw TSD slot (`ios_teb_tls_slot_offset`, which every x18 trampoline reads), installs
  the signal and Mach exception handlers, connects to the server (`server_init_process` reads
  `WINESERVERSOCKET` and sets the global `fd_socket`), maps the user shared data, publishes the
  hardware registry keys and the DOS drive objects ("first process of a session only"), and
  loads ntdll, the WoW64 ntdll and the apiset into that session.
- Session-wide globals: `peb`, `main_image_info` and `current_machine` (which make the session
  ARM64EC or aarch64), `ios_main_image_i386`, `wow_peb`, `user_space_wow_limit`, the no-JIT
  image table (`nojit_dylib.py`: `WINE_IOS_NOJIT` is read once), the process socket table
  (`server_ios.c`), the child slots (`process_ios.c`), the WoW window registry.
- The first game's threads that never ended (an interpreted thread busy in x64 code, a thread
  in an in-process wait the server cannot wake) still use all of the above.

**MYIOSDECK's own state.** FXI's x64 block cache is one per app (`engine/fxi/fxi_win.c`
`g_win_vm`) and the no-JIT path never invalidates it (`xtajit64_stub.c`'s notifications are
empty). FXI32 keeps one instance per guest window (`engine/fxi32/fxi_wow.c` `g_procs`), found
by the window's base. The x64 CPU choice (`fxi_win_host.c` `g_cpu`) and the exception
dispatcher are globals set by the newest process.

**DXMT and Winios.** DXMT gets the app's `CAMetalLayer` once (`GameHostView.register`), and a
new swapchain can present into it. Winios already expects later sessions:
`winios_session_reset()` drops the game overlay and the Metal window set at every start; the
client rects `stage-app.sh` adds are now dropped with them.

Resetting all of this by hand, without a debugger on the device and without upstream support
(Wine never runs `__wine_main` twice in one process), is not realistic: every missed static is a
crash in a later session. Hence design B.

## 2. The two designs

**A. Full teardown and re-init per session.** Stop the server, re-run `wineserver_main` with a
fresh object tree, re-run `__wine_main`. Needs a reset for every static above, in Madeira's
code and in Wine's, plus a way to stop the first game's leftover threads. Not feasible.

**B. Keep the wineserver and the unix side, start each later game as a new process.** This is
what a desktop Wine does when `wine b.exe` runs while `wine a.exe` keeps the wineserver up, and
what Madeira already does for a launcher's game:
- The server takes any number of clients. `wineserver_inject_client_fd` hands it a socketpair
  end; its loop picks it up within 1 ms and `master_socket_handle_client` (`request_ios.c`)
  calls `create_process(fd, NULL, ...)`: a process with no parent, exactly as for the first game.
  `master_socket_timeout` is infinite on iOS, so the server stays up with no client.
- The client side is `wine_ios_child_main` (`loader_ios.c`), the boot every child already
  takes: a new TEB and PEB (cloned from the session's first one), the process's own server
  socket (`server_init_process_child`, which registers it in the process socket table), a guest
  window for a 32-bit image, its own ntdll (no-JIT: a new instance of the signed dylib,
  `nojit_child.py`). For a process with no parent the server sends no startup info, so
  `unix_init_startup_info` builds it from argv and the environment (`build_initial_params`), as
  for the session's first program: the app's per-launch variables (working folder, Steam
  identity, controller mode, audio) reach the new game the usual way.
- Exiting is the child path too: `process_exit_wrapper` finds the process's socket entry and
  releases its fd cache, its JIT-pool copies, its sub-floor windows, its fixed-base exe window
  and its WoW window, which the next 32-bit process tears down and takes over (with the D3D9
  native objects inside it).

Variant B' would make every game a child of a small permanent root program (Madeira's own
model for a launcher), so that the first game gets the child exit path too. It needs a root
program built for both the ARM64EC and aarch64 farms and a way to tell it what to start, and it
changes how the first game boots. B is used instead: the first game boots exactly as before.

**32-bit and 64-bit.** A session is ARM64EC (64-bit games, `MADEIRA_USE_ARM64EC=1`, the
`arm64ec-windows` farm in `system32`) or aarch64 (32-bit games through WoW64, the `aarch64-windows`
farm). The main image sets `main_image_info`, `is_arm64ec()` and the farm the bridge links, and
nothing switches them later. Madeira has cross-architecture children (`ios_load_child_ec_ntdll`
for an x64 child of an aarch64 session), but the farm in `system32` is the session's, so a later
game must be of the session's kind; a game of the other kind needs a restart, as before.

## 3. What was implemented

| where | what |
|---|---|
| `engine/wine/patches/session_reuse.py app` (from `stage-app.sh`) | `WineProcessBridge.m`: with `MYIOSDECK_SESSION_REUSE=1` the end of a session leaves the wineserver running. The next `wine_process_start` gives the new program a socketpair of its own, injects the server end, and its thread calls `wine_ios_child_main` instead of `__wine_main`; `ios_main_image_i386` keeps the session's value. After the program ends (it longjmps back as before), the thread does what `ios_child_thread_entry` does for a child (boot lock, WoW window of a failed boot, TEB key) and reports the exit status, which ntdll reports only for the first program. Children of earlier games that never released their slot do not hold the session's `MADEIRA_WAIT_CHILDREN` wait. The running flag drops after the thread's own clean-up, so the next game's per-launch variables are not unset by the last one. `wine_session_reusable()` says whether a program can start now. |
| same, `app` | When a kept session's first program ends, its 32-bit guest window is retired (`ios_wow_window_retire_current`): it keeps its memory and matches nothing. `ios_wow_slot_current()` falls back to a window's boot thread, and a later boot thread can get the same `pthread_t`: on the phone (build 156) Forager booted into Steins;Gate's dead launcher's window. |
| `engine/wine/patches/session_reuse.py ntdll` (from `build-unix.sh`) | `server_ios.c`: the process socket table (64 entries, never reused, and readers walked past its end once a 65th process registered) reuses an exited process's entry once full; the first program's socket number is forgotten when it closes. |
| `engine/wine/patches/session_reuse.py win32u` (from `build-unix.sh`) | Wine's `dlls/win32u/window.c`: win32u's client objects (`client_objects[]`, the WND behind each window handle index) are per process in Wine and shared by every pseudo-process here. A window the wineserver made for this process itself (its desktop and message windows) has no WND, and its index was usually an exited process's window, whose WND, freed or not, was still in the slot. The second Forager (build 158) dispatched WM_PARENTNOTIFY to its message window through such a WND and called its window procedure, read as 0x3c. A WND is now the window's only when its own handle is the window's full handle; window enumeration skips the others. Also `[winproc]` diagnostics in `class_ios.c` (window procedures allocated, and any dispatched below 64 KB). |
| same, `ntdll` | **Threads an exited process leaves asleep.** A process ends by the server killing its threads, and a thread learns it from its next server request. DXMT's encode, finish and other worker threads sleep in address waits (`NtWaitForAlertByThreadId`) that never ask the server, and its shared-event listener sits in a CFRunLoop: six threads per Direct3D game stayed (build 159, logs 93/94), their TEBs inside the guest window, and Madeira keeps a window while a thread of it lives, so the first Steins;Gate's window and its memory never came back ("teardown did not complete", the second game took another window). When a window's owner has exited, the reclaim (`virtual_ios.c`) now asks `myiosdeck_reap_range` (`signal_arm64_ios.c`) first: every thread of the window is woken (`myiosdeck_reap_alert`, `sync.c`), and a woken thread that sees a new reap generation asks the server about itself (`get_thread_info`); a killed thread's request finds its pipe closed and the thread exits through Wine's own path. What still sleeps half a second later where no alert reaches it (the run loop), suspended inside a system call and so holding no lock of this side, has its server pipes closed and is terminated. Then the window is torn down as Madeira does for a launcher. |
| same, `ntdll` | **A killed main thread ends its process.** `abort_thread` ended a killed thread alone; for a pseudo-process's main thread that meant its boot thread never got back to its `setjmp`, so the process's slot, socket and window stayed taken and the app never heard that the program ended. `myiosdeck_main_thread_killed` (`server_ios.c`, called from `thread_ios.c`) ends the process through `process_exit_wrapper` instead; its requests fail from then on (the deferred-abort state of a section) rather than ending the thread halfway. |
| `engine/wine/patches/session_reuse.py server` (from `build-unix.sh`) | **Force quit.** `wineserver_kill_programs` (`request_ios.c`) sets a flag the server loop reads (`fd_ios.c`), and `myiosdeck_kill_programs` (`process.c`) terminates every program of the session (not a system process) as `TerminateProcess` does, on the server's own thread, then wakes their threads so one asleep in an address wait finds out. |
| `engine/wine/patches/nojit_dylib.py` | The no-JIT image table, one entry per DLL per process, grows from 512 to 4096 entries. 512 lasted only a few processes. |
| `engine/fxi/fxi_win.c` | `fxi_win_invalidate_all()`: the app drops FXI's x64 blocks before a later game starts (an earlier game's freed code ranges may hold the new game's code). `fxi_invalidate` frees nothing, so a thread still inside a dropped block translates again. |
| `engine/fxi32/fxi_wow.c`, `App/Sources/Native/fxi_wow_host.c` | `fx32_wow_process_start()` at `BTCpuProcessInit`: a window an exited process left gets a fresh FXI32 instance. The old one held that process's blocks and its BOP page address (the new process allocates its own), and its BOP exit blocks are never invalidated. Tested in `wow_test.c` (check 9). |
| `App/Sources/Native/wine_host.c` | `mid_wine_boot` for a later program: no JIT slice, no server start, no log truncation; resets the exit status, flushes FXI, starts the program in the session. The log follower keeps reading while a session is kept. |
| `App/Sources/Engine/WineController.swift` | `SessionKind` (no JIT, ARM64EC or aarch64, HID pad, FXR) recorded at the first program; `nextGameRefusal` says why a later game cannot join (setting off, session not kept, kind differs, FXR on for a 64-bit game, 8 programs already, less than 1.5 GB free) or nil. The Dock/added-game flags are now per launch (they stayed set for the whole app run). |
| `App/Sources/UI/LibraryView.swift`, `SettingsView.swift` | The setting; Play is enabled again after a game ends when another can join; the game menu gets **Quit game**, which asks the game to close with Alt+F4 through its normal input (as Madeira's own Quit game does, `Library.swift` `requestQuit`), keeps its view up with "Closing…" until it exits, and leaves MYIOSDECK open. If the game still runs 10 s later the log gets a `[quit-watch]` report (`mid_wow_thread_report`, `fxi_wow_host.c`: each 32-bit thread's system call in progress or x86 code, its native function, the return addresses on its stack) and the menu offers **Force quit** (`WineController.forceQuit`, the server's Force quit above); the library then says the game "was force-quit". **Quit game and close MYIOSDECK** stays for a game even Force quit does not end. |
| `engine/wine/stage-app.sh` | Winios's per-window client rects are dropped at a new session's start, with its Metal windows. |

## 4. Limits

Phone results so far (Forager twice, Steins;Gate then Forager; builds 155-158): the second game
boots in the kept session every time, and died about 4.5 s in on the stale WND above until
that fix. Other per-process state that pseudo-processes share the same way is the main risk left
(win32u's menus and icons share `client_objects[]` too, but they always have a client object).

Build 159 (logs 93, 94): the second Forager and the second Steins;Gate run, but Quit game did not
end them. Steins;Gate started its exit (the same nine threads ended as the first time) and its
main thread then waited, in a server wait, on something that never came; the first time its
DXMT encode thread had ended next. Forager showed no reaction at all. The first game's six DXMT
threads were still asleep inside its window, which therefore kept its memory (the "much higher
RAM"), and the frozen picture is the shared game layer's last frame while nothing presents. The
`[quit-watch]` report names the wait the next time; Force quit ends such a game meanwhile.

- **The first game is never reclaimed.** It is the session's initial process: no process socket
  entry, so none of the child exit clean-up. For a 32-bit first game its guest window also holds
  the PEB every later process is cloned from (`ios_initial_peb`), so it must stay mapped. A
  32-bit game started through a launcher (Steins;Gate: `Launcher.exe` starts `Game.exe`) loses
  little: the launcher is the first process, the game is a child and is reclaimed.
- **64-bit games' memory is not given back.** Wine's views are not tagged by process, so only
  JIT-pool copies are released when a 64-bit pseudo-process exits. The game's heap and its
  Metal objects stay until MYIOSDECK closes. The app refuses another game with less than
  1.5 GB free (`os_proc_available_memory`).
- **Quit game asks; Force quit kills.** Quit game asks with Alt+F4, so a game can save. Force
  quit ends a game that ignores it or hangs on its way out, as Task Manager would: unsaved
  progress is lost. A game thread that runs x86 code without ever calling the system is not
  stopped by it (the server cannot interrupt a thread on iOS); Quit game and close MYIOSDECK is
  left for that.
- **Memory of a 32-bit game comes back when the next 32-bit game starts**, not when it ends: the
  reclaim runs on the next 32-bit process's boot thread, as in Madeira (the dying thread stands
  on its own TEB inside the window). The first game's window is the exception above. Threads a
  game leaves running (not asleep) still keep its window, as before.
- **Metal objects of an ended 32-bit game are not released**: winemetal keeps no list per
  process, so the textures and buffers behind an exited game's handles stay with the window gone.
- **Same kind only**: no JIT, the same 32-bit/64-bit class, the same HID pad mode (DirectInput
  and XInput are per process and can differ), and FXI rather than FXR for 64-bit games (FXR has
  no way to drop code it translated). At most 8 programs per app run (the process tables
  hold that many games with their helpers).
- **Idle server**: between games the wineserver thread wakes every millisecond, as it does
  during a game.

## 5. Testing on the phone

1. Settings › Without JIT › Play another game without restarting: on.
2. Start a 64-bit game (Crab Game or Stick Fight). In the game menu choose **Quit game**; confirm
   the game's exit dialog if it shows one. The library comes back, and the Windows card says
   another program can start.
3. Start the other 64-bit game. Log lines to look for: `[session-reuse] ... starts in the running
   Wine session as a new top-level process`, `[Wine child] ... PE loaded`, `x64 blocks dropped`.
4. Quit it, then try a 32-bit game: MYIOSDECK asks for a restart (the session is 64-bit).
5. After a restart: Steins;Gate, Quit game, Steins;Gate again (a 32-bit session): look for
   `[wow-window] ... released` and, at the second start, `FXI32 instance ... dropped`.
6. The game's own quit (from its menu) ends the session the same way.
7. A game that does not close within 10 s of Quit game: the status says so, the log has
   `[quit-watch]` lines, and the game menu offers **Force quit**. After it: `[wineserver] force
   quit: N program process(es) terminated`, `[session-reuse] the main thread of process ... was
   killed`, and the library says the game "was force-quit".
8. Steins;Gate twice: at the second start, `[session-reuse] window B=0x72...: 6 thread(s) the
   exited process left were woken ...` and no `REJECTED: the slot is still held by an exited
   process` line; the second Game.exe gets the first one's window back.

If a later game fails to start, the log has `[Wine child] BOOT FAILED at stage ...`, and the
crash report offers the log as for any game that ends with an error.
