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
`g_win_vm`, FXR's likewise in `engine/fxr/fxi_win.c`) and the no-JIT path never invalidates
it (`xtajit64_stub.c`'s notifications are empty). FXI32 keeps one instance per guest window
(`engine/fxi32/fxi_wow.c` `g_procs`), found by the window's base. The x64 CPU choice (`fxi_win_host.c` `g_cpu`) and the exception
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
nothing switches them later. A later game of the other kind runs the way Madeira's
cross-architecture children do, and the bridge keeps the session's farm in `system32` for it
(it relinked `system32` for every launch, which would swap the running session's DLLs):
- a 64-bit game in a 32-bit (aarch64) session gets its own ARM64EC ntdll
  (`ios_load_child_ec_ntdll`, which every no-JIT process takes anyway, `nojit_child.py`) and
  finds the ARM64EC system DLLs in `sysx64` when the `system32` name is the aarch64 one;
- a 32-bit game in a 64-bit (ARM64EC) session gets the aarch64 ntdll (`nojit_child.py`) and
  `wow64.dll`, `wow64win.dll` and `xtajit.dll`, which only the aarch64 farm has and which the
  bridge links into an ARM64EC `system32` as non-colliding names; its guest window is reserved
  when it starts (a 64-bit session arms the WoW64 windows lazily).
The no-JIT map hook picks each DLL's signed dylib by that DLL's machine, not by the session. This
is new ground for the 32-bit-after-64-bit order (Madeira calls that direction "future" for its
JIT path): the phone decides.

The phone decided (build 197, Forager after Dokimon): the 32-bit game's aarch64 half loaded
`system32\win32u.dll`, which in an ARM64EC session is the ARM64EC build, ran its x64 bytes as
ARM64 code and faulted. Only the ARM64EC ntdll retries the other farm (`sysx64`), and WoW64 accepts
an image of any machine. `nextGameRefusal` now refuses a 32-bit game in a session whose first game
was 64-bit, with a restart message, until the aarch64 side retries `sysaa64`
(docs/COMPATIBILITY.md, follow-ups). 64-bit after 32-bit is unchanged.

## 3. What was implemented

| where | what |
|---|---|
| `engine/wine/patches/session_reuse.py app` (from `stage-app.sh`) | `WineProcessBridge.m`: with `MYIOSDECK_SESSION_REUSE=1` the end of a session leaves the wineserver running. The next `wine_process_start` gives the new program a socketpair of its own, injects the server end, and its thread calls `wine_ios_child_main` instead of `__wine_main`; `ios_main_image_i386` keeps the session's value. After the program ends (it longjmps back as before), the thread does what `ios_child_thread_entry` does for a child (boot lock, WoW window of a failed boot, TEB key) and reports the exit status, which ntdll reports only for the first program. Children of earlier games that never released their slot do not hold the session's `MADEIRA_WAIT_CHILDREN` wait. The running flag drops after the thread's own clean-up, so the next game's per-launch variables are not unset by the last one. `wine_session_reusable()` says whether a program can start now. |
| same, `app` | When a kept session's first program ends, its 32-bit guest window is retired (`ios_wow_window_retire_current`): it keeps its memory and matches nothing. `ios_wow_slot_current()` falls back to a window's boot thread, and a later boot thread can get the same `pthread_t`: on the phone (build 156) Forager booted into Steins;Gate's dead launcher's window. |
| `engine/wine/patches/session_reuse.py ntdll` (from `build-unix.sh`) | `server_ios.c`: the process socket table (64 entries, never reused, and readers walked past its end once a 65th process registered) reuses an exited process's entry once full; the first program's socket number is forgotten when it closes. |
| `engine/wine/patches/session_reuse.py win32u` (from `build-unix.sh`) | Wine's `dlls/win32u/window.c`: win32u's client objects (`client_objects[]`, the WND behind each window handle index) are per process in Wine and shared by every pseudo-process here. A window the wineserver made for this process itself (its desktop and message windows) has no WND, and its index was usually an exited process's window, whose WND, freed or not, was still in the slot. The second Forager (build 158) dispatched WM_PARENTNOTIFY to its message window through such a WND and called its window procedure, read as 0x3c. A WND is now the window's only when its own handle is the window's full handle; window enumeration skips the others. Also `[winproc]` diagnostics in `class_ios.c` (window procedures allocated, and any dispatched below 64 KB). |
| same, `ntdll` | **Threads an exited process leaves asleep.** A process ends by the server killing its threads, and a thread learns it from its next server request. DXMT's encode, finish and other worker threads sleep in address waits (`NtWaitForAlertByThreadId`) that never ask the server, and its shared-event listener sits in a CFRunLoop: six threads per Direct3D game stayed (build 159, logs 93/94), their TEBs inside the guest window, and Madeira keeps a window while a thread of it lives, so the first Steins;Gate's window and its memory never came back ("teardown did not complete", the second game took another window). When a window's owner has exited, the reclaim (`virtual_ios.c`) now asks `myiosdeck_reap_range` (`signal_arm64_ios.c`) first: every thread of the window is woken (`myiosdeck_reap_alert`, `sync.c`), and a woken thread that sees a new reap generation asks the server about itself (`get_thread_info`); a killed thread's request finds its pipe closed and the thread exits through Wine's own path. What still sleeps half a second later where no alert reaches it (the run loop), suspended inside a system call and so holding no lock of this side, has its server pipes closed and is terminated. Then the window is torn down as Madeira does for a launcher. |
| same, `ntdll` | **A killed main thread ends its process.** `abort_thread` ended a killed thread alone; for a pseudo-process's main thread that meant its boot thread never got back to its `setjmp`, so the process's slot, socket and window stayed taken and the app never heard that the program ended. `myiosdeck_main_thread_killed` (`server_ios.c`, called from `thread_ios.c`) ends the process through `process_exit_wrapper` instead; its requests fail from then on (the deferred-abort state of a section) rather than ending the thread halfway. |
| same, `ntdll` | **Locks after the first program.** Wine's `mutex_lock`/`mutex_unlock` (`unix_private.h`) do nothing once the global `process_exiting` is set, which `NtTerminateProcess` does for the exiting process. On iOS that global is the exit flag of the session's first program only (every other process has its own, `ios_proc_sockets[].exiting`), so once the first program ended (Steins;Gate's launcher a few seconds in, or the first game), every later program ran without any unix-side lock (`fd_cache_mutex`, `virtual_mutex`, …). Build 173: two Stick Fight threads received each other's fds (`[fd-recv] HANDLE MISMATCH: asked 0x46c got 0x468`) and the assert in `server_get_unix_fd` ended the game. With session reuse the first program gets an exit flag of its own (`ios_process_exiting_ptr`, `server_ios.c`). |
| `engine/wine/patches/session_reuse.py server` (from `build-unix.sh`) | **Force quit.** `wineserver_kill_programs` (`request_ios.c`) sets a flag the server loop reads (`fd_ios.c`), and `myiosdeck_kill_programs` (`process.c`) terminates every program of the session (not a system process) as `TerminateProcess` does, on the server's own thread, then wakes their threads so one asleep in an address wait finds out. It also counts the Force quits (`g_myiosdeck_force_quits`) for the bridge below. |
| same, `server` | **A thread that ended itself.** Wine's server treats a thread as dead once its request pipe closes; `NtTerminateThread` on itself only records the exit code. From the second program of a kept session on, threads that ended this way stayed alive in the server (build 171): their handles never became signaled and the game waited for them. `terminate_thread` (`wine/server/thread.c`) now arms a 250 ms timer for such a thread; if the server still has it then, it is ended as the pipe's close would have ended it (`kill_thread`), and a `[session-reuse] thread … ended itself 0.25 s ago, but its request pipe (server fd N: …) never closed` line says what had become of the pipe (still open at the thread's end, closed and missed by the server's poll, or no longer open). |
| same, `app` | **Force quit and a child that never comes back.** The bridge waits for a launcher's game (a child) by its slot, which only the child's main thread gives back when it returns to its start. 3 s after a Force quit (the count above changed since the program started) it stops waiting: `[session-reuse] Force quit 3 s ago and N child process(es) (…) still hold their slot`, and the session ends; the next program counts that slot as an earlier program's. |
| `engine/wine/patches/nojit_dylib.py` | The no-JIT image table, one entry per DLL per process, grows from 512 to 4096 entries. 512 lasted only a few processes. |
| `engine/fxi/fxi_win.c` | `fxi_win_invalidate_all()`: the app drops FXI's x64 blocks before a later game starts (an earlier game's freed code ranges may hold the new game's code). `fxi_invalidate` frees nothing, so a thread still inside a dropped block translates again. |
| `engine/fxr/fxi_core.c`, `fxi_win.c`, `fxr_pin.c` | `fxr_win_invalidate_all()` (build 178), FXR's equivalent: an empty block table, and every old block's first uop becomes a re-lookup (`p_relookup`), where links, loop traces and the threads' indirect-branch caches lead (each thread empties its cache when the block generation changed, `fxr_ibtc_sync`). Nothing is freed. CI: `engine/fxr-probe/invalidate_test.c` (fxr.yml) runs program A, then B at the same addresses after the flush, 400 times, with another thread looping inside a dropped block, on FXR and FXI. |
| `engine/fxi32/fxi_wow.c`, `App/Sources/Native/fxi_wow_host.c` | `fx32_wow_process_start()` at `BTCpuProcessInit`: a window an exited process left gets a fresh FXI32 instance. The old one held that process's blocks and its BOP page address (the new process allocates its own), and its BOP exit blocks are never invalidated. Tested in `wow_test.c` (check 9). |
| `App/Sources/Native/wine_host.c` | `mid_wine_boot` for a later program: no JIT slice, no server start, no log truncation; resets the exit status, flushes FXI and FXR (`mid_fxr_win_invalidate_all`, `fxr_win_shim.c`), starts the program in the session. The log follower keeps reading while a session is kept. |
| `App/Sources/Engine/WineController.swift` | `SessionKind` (no JIT, ARM64EC or aarch64, HID pad, FXR) recorded at the first program; `nextGameRefusal` says why a later game cannot join (setting off, session not kept, kind differs, less than 1.5 GB free) or nil. Every 64-bit game of an app run uses the x64 CPU the first one used (`fxrThisRun`: FXR or FXI, Settings › Without JIT › Windows games: FXR, on by default since build 178): the CPU is one global of the session (`fxi_win_host.c` `g_cpu`) and threads an ended game left keep theirs, so the toggle applies after a restart then; a 32-bit game keeps `MYIOSDECK_WIN_CPU` as it is. Until build 178 FXR could not drop its blocks and ran only the first 64-bit game (later ones took FXI; build 172 refused Stick Fight after Steins;Gate because FXR was on). There is no limit on the number of games. The Dock/added-game flags are now per launch (they stayed set for the whole app run). |
| `App/Sources/UI/LibraryView.swift`, `SettingsView.swift` | The setting; Play is enabled again after a game ends when another can join; the game menu gets **Quit game**, which asks the game to close with Alt+F4 through its normal input (as Madeira's own Quit game does, `Library.swift` `requestQuit`), keeps its view up with "Closing…" until it exits, and leaves MYIOSDECK open. If the game still runs 6 s later the log gets a `[quit-watch]` report (`mid_wow_thread_report`, `fxi_wow_host.c`: each 32-bit thread's system call in progress or x86 code, its native function, the return addresses on its stack), the status says "force quitting it in 4 s", and 4 s later the game is force quit with no menu step (`WineController.forceQuit`, the server's Force quit above). Quit game chosen again while one is pending force quits at once, and the menu keeps a **Force quit** button once the watch has fired. The library then says the game "was force-quit". **Quit game and close MYIOSDECK** stays for a game even Force quit does not end. |
| `App/Sources/Engine/RecentlyPlayed.swift`, `SteamGamesView.swift`, `CustomGamesCard.swift` | Not session reuse, asked for with it: the library lists the recently played games first (owned Steam games, installed Steam games, My games); each start is recorded per game in UserDefaults. The Steam card lists the Steam games **installed on this iPhone** (every library on C: with Steam install records, downloaded here or by Valve's client; shown signed in or not), each with its size and **Delete…**, which removes its folder under `common`, its `appmanifest_<id>.acf` and its resume journal (`SteamInstallFiles.delete`) after a confirmation; not while a Windows game runs. The game sheet's Uninstall now asks first too. |
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

Build 161 (log 95, Steins;Gate twice): at the second start the reaper woke the first Game.exe's
six threads and five ended themselves; the sixth, DXMT's event-listener run loop (mach_msg), was
left because the check asked for its state after suspending it (a suspended thread reports
STOPPED, not WAITING), so the window was again not reclaimed. Fixed: the state is read first. The
second game hung on Quit exactly as in build 159, and the log ended before the report (Quit was
chosen twice, which restarted the 10 s watch): the report now comes 6 s after the first Quit, or
at once when Quit is chosen again. The old game's frame behind the new launcher was the shared
layer's last picture: the layer is cleared to black when a later game starts.

Build 162 (logs 96, 97): the run-loop thread passed the check and was still left: iOS does not
terminate a pthread from another thread. Such a thread now stays suspended for good (it sleeps
inside a system call, holds nothing of this side, and never runs again) and is dropped from the
thread registry, so the window can go. The second Forager has never got past its start (build
159's log shows the same): about 6 s in, after the thread that ends with 503 system calls, every
thread waits and nothing is drawn; the report did not run, since Quit game was not chosen. The
report now also runs each time the game menu opens. The second Steins;Gate's Game.exe died 0.4 s
in: its mmdevapi thread returned from Sleep (NtDelayExecution) to a heap address, as if its x86
stack had changed under it (build 161 started the same game fine). Wine's iOS audio driver
(`audio_null_ios.c`) is one table for the session: an exited game's stream stays registered and
mixed (16 slots), and every process attach re-initializes its lock; not yet changed.

Build 163 (log 98, crash log 6): the reclaim works: the first Game.exe's window was torn down
(166 views) and its memory came back (footprint 482 MB at the second start, against 900+ before),
with the run-loop thread kept suspended (thread_terminate refused, error 53). The second
Forager's report names the hang: its main thread waits in WaitForSingleObject for its audio
thread, which waits for its first WASAPI buffer event, which the audio driver's timer gives only
while the RemoteIO endpoint consumes. The endpoint is shared by the app and is stopped when the
last client stops (the first game's exit) and started again for the next. Steins;Gate's second
start ended the app with no log line while the second Game.exe brought its threads up, the moment
its audio starts; Madeira's own notes record Apple's caulk trapping on RemoteIO churn. In a kept
session the endpoint now stays running once started, and a started client whose endpoint is not
running gets the 10 ms null-mode beat rather than no event at all.

Build 164 (log 99, crash log 7): neither changed. The second Forager hangs the same way (no
driver line about its stream reached the log), and the app ended 0.4 s after the second Game.exe
started, again with no crash report on the phone (none from that day in Analytics Data, which
is what an exit() rather than a crash leaves). The game-menu report now includes the audio
driver's state (endpoint, every registered and mixed stream with its positions and event), and
an exit() while Wine runs logs its caller.

Build 165 (log 100, crash log 8): the second Forager's audio is not the hang: its stream plays
(written 708086, played 703286 frames, the endpoint running), so the thread waiting at
Forager.exe+0x14d609f waits for something else; the report now prints a waiting call's first
arguments (the handles). The app's end at the second Game.exe's start called no exit(). Wine's
messages reach the app's log through a follower of Documents/madeira-log.txt whose last lines die
with the app; the crash log now appends that file's tail (it is only cleared by the next Wine
start). Madeira's one silent end is a fault redelivered 2000 times (task_terminate, no report).

Build 166 (crash log 9 with Wine's log tail): the cause of the second Steins;Gate's end. Wine's
exit_thread() joins and frees the TEB of the thread that exited before it, kept in one static for
the whole session; the second Game.exe's first exiting thread got the first Game.exe's last TEB,
inside the window just torn down, and faulted reading it (exit_thread+0x88, address
0x72002f03a0) until Madeira's redelivery guard ended the app (task_terminate, no report). In
build 162, before the window was torn down, the same TEB was freed into the new process: the
corrupted x86 stack of log 96. The last TEB is now a global that a window teardown clears, and
exit_thread joins and frees only a TEB of its own process. The second Forager waits on handle
0xc4 in its main thread; its audio thread waits normally on its stream event 0xac.

Build 167 (logs of 2026-10-13): four Steins;Gate starts in one run; each window was torn down
and reused. Three issues left. (1) The footprint grew about 330 MB a game (970 MB, 1.3 GB,
1.65 GB, 1.97 GB): DXMT's census counts 409 MB of Metal objects per game, which an ended game
never gives back. `engine/dxmt/patches/metal_owner.py` records each texture and buffer a 32-bit
process makes and counts its references; the window teardown gives back what the dead process
still held (`winemetal_release_window`, called from the reclaim). (2) From the second game on,
Quit game stops in Bink's BinkWaitStopAsyncThreadsMulti: its worker threads end as in the first
game, but the main thread then waits on handle 0xa4 for good (Force quit ends it). (3) The second
Forager's main thread waits on handle 0xc0 at start. The report now names the call that made a
waited handle (NtCreateEvent, NtCreateThreadEx, ... and three return addresses above it).

Build 171 (logs of 2026-10-14: six Steins;Gate starts in one run; Steins;Gate then Forager). RAM no
longer climbs (the Metal records above give back about 290 objects a game). Three hangs were left:
- From the second Steins;Gate on, Quit game stopped in BinkWaitStopAsyncThreadsMulti every time.
  The main thread waited on handle 0xa4, a thread `_BinkOpenXAudio28` made. Game 1 and game 2 end
  the same way: the six Bink async threads (waiting on semaphores from `CreateSemaphoreA` at
  bink2w32.dll+0x2c76e) wake, run `RtlExitUserThread` and `NtTerminateThread` on themselves and
  free the previous exiting thread's stacks (three `[vfree]` lines each). In game 1 each is then
  followed by `read_request EOF tid=… -> kill_thread`; in game 2 by nothing, and Force quit later
  found all six still alive in the server (`kill_thread tid=015c … violent=1`). Short-lived
  threads of the same game 2 got their EOF, so it is not every exit. Where the pipe's end went is
  still open: the new timer above ends such a thread anyway and its line names the pipe's state.
- Forager after Steins;Gate (and after Forager): its main thread waits on handle 0xc0, the thread
  it made at Forager.exe+0x134f69a, which ended (`thread exit: 2013 system calls`) and is listed no
  more. The same shape, the same timer.
- The last Steins;Gate: Force quit killed both processes and Game.exe's main thread ran its exit
  path (`the main thread of process 0540 was killed`, its window and fd cache released), but it
  never got back to its start: it faulted in `_MetalLayer_getProps32` writing to the untranslated
  guest address 0x350fd70 (its window was already released, so the 32-bit thunk's pointer
  conversion added nothing), and the bridge kept waiting for the child's slot ("still running 8 s
  after Force quit"). The bridge now stops waiting 3 s after a Force quit (above). The mouse report
  for that game: 194 mouse events reached Wine (`[winios] drain`), touch was set to Off, then to
  Trackpad during the game; no input fault was found in the log.

- **The first game is never reclaimed.** It is the session's initial process: no process socket
  entry, so none of the child exit clean-up. For a 32-bit first game its guest window also holds
  the PEB every later process is cloned from (`ios_initial_peb`), so it must stay mapped. A
  32-bit game started through a launcher (Steins;Gate: `Launcher.exe` starts `Game.exe`) loses
  little: the launcher is the first process, the game is a child and is reclaimed.
- **64-bit games' memory is not given back.** Wine's views are not tagged by process, so only
  JIT-pool copies are released when a 64-bit pseudo-process exits. The game's heap and its
  Metal objects stay until MYIOSDECK closes. The app refuses another game with less than
  1.5 GB free (`os_proc_available_memory`).
- **Quit game asks; Force quit kills.** Quit game asks with Alt+F4, so a game can save; a game
  that has not closed 10 s later is force quit (a game whose exit dialog waits longer than that
  for an answer is force quit too). Force quit ends a game that ignores it or hangs on its way
  out, as Task Manager would: unsaved progress is lost. A game thread that runs x86 code without ever calling the system is not
  stopped by it (the server cannot interrupt a thread on iOS); Quit game and close MYIOSDECK is
  left for that.
- **Memory of a 32-bit game comes back when the next 32-bit game starts**, not when it ends: the
  reclaim runs on the next 32-bit process's boot thread, as in Madeira (the dying thread stands
  on its own TEB inside the window). The first game's window is the exception above. Threads a
  game leaves running (not asleep) still keep its window, as before.
- **Metal objects of an ended 32-bit game are not released**: winemetal keeps no list per
  process, so the textures and buffers behind an exited game's handles stay with the window gone.
- **What a later game must share**: no JIT, the same HID pad mode (DirectInput and XInput are
  per process and can differ). 64-bit games keep the first one's x64 CPU (FXR by default)
  until MYIOSDECK closes. 32-bit and 64-bit games may follow each other (section 2), and there is
  no limit on the number of games: what stops another game is memory (a 64-bit game's stays in
  use, and less than 1.5 GB free asks for a restart). Concurrent 32-bit windows are limited to 8
  (Madeira's map): a window an ended game cannot give back (a thread still running x86 code in
  it) uses one up.
- **Idle server**: between games the wineserver thread wakes every millisecond, as it does
  during a game.

## 5. Testing on the phone

1. Settings › Without JIT › Play another game without restarting: on.
2. Start a 64-bit game (Crab Game or Stick Fight). In the game menu choose **Quit game**; confirm
   the game's exit dialog if it shows one. The library comes back, and the Windows card says
   another program can start.
3. Start the other 64-bit game. Log lines to look for: `[session-reuse] ... starts in the running
   Wine session as a new top-level process`, `[Wine child] ... PE loaded`, `x64 blocks dropped`.
4. Quit it, then start a 32-bit game (Forager) in the same session: look for `[session-reuse]
   ... starts in the running Wine session as a new top-level process (32-bit` and `[wow-window]
   reserved`. Then a 64-bit game again.
5. After a restart: Steins;Gate, Quit game, Steins;Gate again (a 32-bit session): look for
   `[wow-window] ... released` and, at the second start, `FXI32 instance ... dropped`.
6. The game's own quit (from its menu) ends the session the same way.
7. A game that does not close within 6 s of Quit game: the status says "force quitting it in 4 s",
   the log has `[quit-watch]` lines, and 4 s later the game is force quit by itself (Quit game
   chosen twice force quits at once). After it: `[wineserver] force quit: N program process(es)
   terminated`, maybe `[session-reuse] the main thread of process ... was killed` or `Force quit 3 s
   ago and N child process(es) ... still hold their slot`, and the library says the game "was
   force-quit" within about 4 s.
8. Steins;Gate twice: at the second start, `[session-reuse] window B=0x72...: 6 thread(s) the
   exited process left were woken ...` and no `REJECTED: the slot is still held by an exited
   process` line; the second Game.exe gets the first one's window back.
9. Steins;Gate three times in a row, each ended with Quit game: from the second on it closes by
   itself (no Force quit). The log has `[session-reuse] thread … ended itself 0.25 s ago, but its
   request pipe (server fd N: …) never closed: the server ends it` for the Bink threads; send the
   log, the part after the colon tells where their pipes went.
10. Steins;Gate, Quit game, then Forager: Forager gets past its black screen (the same line may
   appear for its start thread).
11. Library: the game just played is first in Steam's owned games, in "Installed on this iPhone" and
   in My games. Installed on this iPhone › Delete… on a game removes it (its size disappears from
   Settings › iPhone Storage for MYIOSDECK) and from the list; Install brings it back.
12. Steins;Gate or Forager, Quit game, then Stick Fight (a 64-bit game after 32-bit ones, the order
   Madeira calls "future"): it starts as `[session-reuse] … starts in the running Wine session as a
   new top-level process (64-bit`, with FXR if that is on (`[fxi-win] x64 CPU: FXR`). A second
   64-bit game after it runs with FXR too (build 178; before it took FXI).
13. FXR for every 64-bit game (build 178; Settings › Without JIT › Windows games: FXR is on by
   default): Steins;Gate, Quit game, Stick Fight, Quit game, Stick Fight again. Both Stick Fight
   starts log `[fxi-win] x64 CPU: FXR`, and the second start's `[session-reuse] started … (program
   N of this app run; M x64 blocks dropped)` has M > 0 (the first Stick Fight's FXR blocks). The
   second Stick Fight plays as the first did.
   Build 173 got this far and Stick Fight ran 42 s, then ended on `HANDLE MISMATCH` (the locks above); with build 174 it should keep running.

If a later game fails to start, the log has `[Wine child] BOOT FAILED at stage ...`, and the
crash report offers the log as for any game that ends with an error.
