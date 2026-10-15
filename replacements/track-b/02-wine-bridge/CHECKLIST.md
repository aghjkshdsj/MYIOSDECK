<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

## Design

`b_runtime.m` owns lifecycle state, environment preparation and logging. `b_logic.c/.h` contains host-testable argument, path, PE, configuration and picture-choice logic. `b_archive.c` streams tar.gz without loading the archive into memory. `b_prefix.m` handles Foundation filesystem operations and DLL farms. `b_display.m` confines all view/window records to the main queue and supplies the DXMT table. `b_input.c` owns the fixed event ring. `b_pads.c` owns independent controller stores. `b_stubs.m` supplies explicit unsupported hooks. `b_internal.h` declares only track B-prefixed cross-file helpers and local Wine ABI types before including the two contracts.

Server start/stop uses lifecycle mutexes/conditions; process start uses an atomic reservation. All exported helper functions use wb_b_ (clarification3). Lifecycle locks are released before external calls or joins. Logging formats before taking its file mutex and calls os_log/UI after releasing it. Ring, display-size, layer and controller locks protect only copies of POD or retained handles; no component or UI calls under those locks. Window state and Core Animation run on main. The server is joinable and the launch thread detached with interactive QoS attributes. Pure C helpers and stores have plain-C test programs.

| Spec ref | Requirement | File | Function | Status |
|---|---|---|---|---|
| A/log path | Once-computed Documents/wine-log.txt | b_runtime.m | wb_log_path | done |
| A/log helper | Timestamp ms, file mutex/flush, os_log/UI outside lock | b_runtime.m | wb_b_log | done |
| A/unix logs | ntdll/server paths and append stdout/stderr redirection | b_runtime.m | wb_b_redirect_log | done |
| A/tagging | Part tags, counts/failures only | all | logging | done |
| B/start 1 | already running return0 | b_runtime.m | wb_server_start | done |
| B/start 2 | copy prefix | b_runtime.m | wb_server_start | done |
| B/start 3 | seed before create | b_runtime.m | wb_server_start | done |
| B/start 4 | release-ready reset | b_runtime.m | wb_server_start | done |
| B/start 5 | interactive QoS attr, joinable, creation failure | b_runtime.m | wb_server_start | done |
| B/thread 1 | stderr and server log | b_runtime.m | wb_b_server_main | done |
| B/thread 2 | WINEPREFIX/HOME and mkdir0755 | b_runtime.m | wb_b_server_main | done |
| B/thread 3 | bundle nls | b_runtime.m | wb_b_server_main | done |
| B/thread 4 | foreground/server argv | b_runtime.m | wb_b_server_main | done |
| B/thread 5 | result and running reset | b_runtime.m | wb_b_server_main | done |
| B/ready | Acquire ready flag | b_runtime.m | wb_server_is_ready | done |
| B/stop | Stop signal, join once, running reset, before/after logs | b_runtime.m | wb_server_stop | done |
| B/fatal | Format/log FATAL, pthread_exit only | b_runtime.m | fatal_error | done |
| C/start 1 | Running guard, copy prefix, mark running | b_runtime.m | wb_process_start | done |
| C/start 2 | socketpair/inject/decimal client fd | b_runtime.m | wb_process_start | question |
| C/start 3 | interactive attr, detached, failure closes fds | b_runtime.m | wb_process_start | question |
| C/launch 1 | seed | b_runtime.m | wb_b_launch_main | done |
| C/launch 2 | base Wine environment | b_runtime.m | wb_b_launch_main | done |
| C/launch 3 | verbose/default WINEDEBUG and log | b_runtime.m | wb_b_launch_main | done |
| C/launch 4 | fixed/non-overwriting FNA/Mono environment | b_runtime.m | wb_b_launch_main | done |
| C/launch 5 | AVAudioSession Playback/activation diagnostics | b_runtime.m | wb_b_launch_main | done |
| C/launch 6 | Steam validate/dock/unset/one-launch cleanup | b_runtime.m | wb_b_launch_main | done |
| C/launch 7 | JIT hex or warning | b_runtime.m | wb_b_launch_main | done |
| C/launch 8 | Wine log, session reset, Documents | b_runtime.m | wb_b_launch_main | done |
| C/launch 9 | settings exports then sync | b_runtime.m | wb_b_launch_main | done |
| C/launch 10 | CA bundle or failure diagnostic | b_runtime.m | wb_b_launch_main | done |
| C/launch 11 | append stdout/stderr | b_runtime.m | wb_b_launch_main | done |
| C/launch 12 | target/farms/argv | b_runtime.m | wb_b_launch_main | done |
| C/launch 13 | working folder | b_runtime.m | wb_b_launch_main | done |
| C/launch 14 | TLS/main image/CPU probe | b_runtime.m | wb_b_launch_main | done |
| C/launch 15 | setjmp/main/exit logging | b_runtime.m | wb_b_launch_main | done |
| C/launch 16 | child detection/wait200ms/log60s | b_runtime.m | wb_b_launch_main | done |
| C/launch 17 | running and per-launch cleanup | b_runtime.m | wb_b_launch_main | question |
| C/launch 18 | server stop | b_runtime.m | wb_b_launch_main | done |
| C/launch 19 | clear ARM64 TSD275 | b_runtime.m | wb_b_launch_main | done |
| C/target selection | Required exe, arm64ec heuristic, PE32 override | b_logic.c, b_runtime.m | wb_b_target | done |
| C/PE probe | MZ/e_lfanew<=16MB/PE/machine; permitted probe paths | b_logic.c, b_runtime.m | wb_b_pe_machine | done |
| C/windows path | Preserve full path or system32/syswow64 bare name | b_logic.c | wb_b_windows_path | done |
| C/arguments | Spaces/tabs, quotes removed, max64 args, wine/path/NULL, logs | b_logic.c | wb_b_split_args | question |
| C/workdir | Validate override, unset, exe fallback, trim, POSIX chdir/PWD/INITIAL_CWD log | b_logic.c, b_runtime.m | wb_b_workdir | done |
| C/CPU probe | 11 ordered sysctl 32-bit feature values, question marks | b_runtime.m | wb_b_cpu_probe | done |
| C/exit status | Release packed crash/acquire getters/reset, only NTSTATUS>=C0000000 | b_runtime.m | exit status hooks | done |
| C/phase | Thread-safe monotonic first-call origin, null name | b_runtime.m | winios_phase | done |
| D/table | Ten pointers exact order, four-pointer win_data, dummy device/noops | b_display.m | macdrv_functions | done |
| D/Metal create | Mutex layer snapshot, no-layer log, mark HWND, retain/release | b_display.m | macdrv_view_create_metal_view | done |
| D/dispatch | Now on main else async | b_display.m | wb_b_on_main | done |
| D/exports | Five plain fallbacks and table used/default visibility; export test | b_display.m | DXMT exports | done |
| D/screen | Published lock size else current positive env/default | b_display.m | wb_screen_size | done |
| D/mode change | Ignore invalid, store lock, changed-only main notification and relayout | b_display.m | winios_display_mode_changed | done |
| E/driver | Create1/show~0/noop pos and cursor/destroy main | b_input.c, b_display.m | driver hooks | done |
| E/ring | 256 fixed mutex, drop newest, no callout under lock | b_input.c | wb_b_enqueue | done |
| E/key | VK and KEYUP0x2 | b_input.c | wb_input_key | done |
| E/pointer | Copy all fields including unsigned data | b_input.c | wb_input_pointer | done |
| E/touch | Phase0 MOVE/LEFTDOWN/ABS,1 MOVE/ABS,2 LEFTUP/ABS | b_input.c | wb_input_touch | done |
| E/drain | FIFO drain per-event unlock before unix delivery, BOOL result | b_input.c | winios_pProcessEvents | done |
| E/overlay off | Read GAME_WINDOWS0 each time | b_display.m | wb_b_windows_enabled | done |
| E/create | Main lazy key/first window transparent clipped touch-through overlay | b_display.m | wb_b_overlay | done |
| E/geometry | Frame/window bounds, aspect fit or stretched desktop rect, main disabled actions, ignore repeats | b_display.m | wb_b_layout | done |
| E/layer | Per-HWND top-level layer anchor0 nearest opaque visible frame/hidden | b_display.m | wb_b_update | done |
| E/frame | Copy Wine-thread values to main, visible/client storage | b_display.m | winios_window_frame | done |
| E/surface | Checked malloc copy stride*h, provider owns free, BGRX CGImage, crop, pending position | b_display.m | winios_surface_present | done |
| E/draw tests | Exclude Metal/unknown/full desktop, remove layer, once-per-window reason log | b_display.m | wb_b_update | done |
| E/mark Metal | Main set first-time log/recheck | b_display.m | wb_b_mark_metal | done |
| E/reset | Main clear Metal/overlay/layers/rects | b_display.m | winios_session_reset | question |
| E/show | Noop without view, move UIWindow/show/hide/front | b_display.m | wb_overlay_show | done |
| E/picture1 | Largest visible Metal client | b_logic.c, b_display.m | wb_b_choose_picture | done |
| E/picture2 | Otherwise exact drawable size and >=quarter desktop | b_logic.c, b_display.m | wb_b_choose_picture | done |
| E/picture3 | No candidate return0 | b_logic.c | wb_b_choose_picture | done |
| F/XInput | Range guard normalized state, packet compare/increment, disconnected read allzero | b_pads.c | gamepad store | done |
| F/HID | Separate mutex, reserved zero, packet preserved on disconnected read | b_pads.c | hidpad store | done |
| F/HID output | NULL ignore, reserved zero, serial compare/increment, getter | b_pads.c | hid output store | done |
| F/vibration | Per-slot first/change serial increment skipping0, getter | b_pads.c | vibration store | done |
| F/rumble | Return0, no physical application | b_pads.c | winios_gamepad_rumble_caps | done |
| F/cursor | Wanted0 and all noops | b_stubs.m | cursor hooks | done |
| F/build/power | Exact wine_build and three NULL IOPowerSources functions | b_stubs.m | stubs | done |
| F/TLS | Four exact _Thread_local definitions | b_runtime.m | TLS | done |
| G/parser | UTF8/BOM/key=value/trim/comments/last wins, get/int/bool | b_logic.c | wb_b_cfg | done |
| G/exports | env lines, prefile launch ownership snapshot, duplicates override, logs | b_runtime.m | wb_b_export_config | done |
| G/per-launch | Eleven exact keys and cleanup | b_runtime.m | wb_b_launch_keys | done |
| G/sync | Four specified precedence branches and unset auto default | b_logic.c, b_runtime.m | wb_b_sync_default | done |
| G/constructor | Early DOCS=$HOME/Documents only if unset | b_runtime.m | wb_b_early_docs | done |
| H/seed 1 | create prefix parents | b_prefix.m | wb_seed_prefix | done |
| H/seed 2 | template only absent update timestamp, missing continue | b_prefix.m | wb_seed_prefix | done |
| H/seed 3 | only c: self-healing link | b_prefix.m | wb_seed_prefix | done |
| H/seed 4 | LocalLow username/fallback/sanitize/disable; no other profile dirs | b_prefix.m | wb_seed_prefix | done |
| H/seed 5 | six shell folders missing/dangling repair, preserve working | b_prefix.m | wb_seed_prefix | done |
| H/tar headers | gzip512 blocks, zero end, short error, ustar prefix and octal strict | b_archive.c | wb_b_extract_tar_gz | done |
| H/tar safety | Strip template prefix component; absolute and dotdot reject/skip data | b_archive.c | wb_b_extract_tar_gz | done |
| H/tar types | Directories5/NUL ending slash, files0/NUL0644 parents wholeblocks, skip metadata/links | b_archive.c | wb_b_extract_tar_gz | done |
| H/tar counts | File/directory counts, return0/-1 | b_archive.c | wb_b_extract_tar_gz | done |
| H/farms 1 | Chosen-set system32 all files, skip dots, unlink/relink | b_prefix.m | wb_b_link_farms | done |
| H/farms 2 | No-JIT overrides and ok/failure logs | b_prefix.m | wb_b_link_farms | done |
| H/farms 3 | Chosen WMI five present | b_prefix.m | wb_b_link_farms | done |
| H/farms 4 | Other-set nonresolving files fill system32 | b_prefix.m | wb_b_link_farms | done |
| H/farms 5 | Full sysx64/sysaa64 farms | b_prefix.m | wb_b_link_farms | done |
| H/farms 6 | 32-bit every session, WMI, SxS | b_prefix.m | wb_b_link_farms | done |
| H/farms 7 | Bundled/fallback Mono, preserve real dir, remove stale symlink | b_prefix.m | wb_b_link_farms | done |
| H/farms 8 | arm64ec VC runtime DLL override with two exclusions | b_prefix.m | wb_b_link_farms | done |
| H/SxS metadata | Wine11.4 manifest identities/files/classes for all named assemblies | b_prefix.m | wb_b_seed_sxs | done |
| H/SxS layout | Lowercase folder, token/version/deadbeef, file links, LF UTF8 manifest matching files | b_prefix.m | wb_b_seed_sxs | done |
| H/SxS skipping | First file absent skip, rewrite every session, counts | b_prefix.m | wb_b_seed_sxs | done |
| Done/tests | All eight pure C areas, five device cases, exports check | tests/, TESTS.md | test main | done |
| Process | Headers, full checklist, provenance, questions, review and commit | documentation | review | done |
| ABI/wb_server_start | Exact declaration/layout and defined symbol | b_runtime.m | wb_server_start | done |
| ABI/wb_server_is_running | Exact declaration/layout and defined symbol | b_runtime.m | wb_server_is_running | done |
| ABI/wb_server_is_ready | Exact declaration/layout and defined symbol | b_runtime.m | wb_server_is_ready | done |
| ABI/wb_server_stop | Exact declaration/layout and defined symbol | b_runtime.m | wb_server_stop | done |
| ABI/wb_process_start | Exact declaration/layout and defined symbol | b_runtime.m | wb_process_start | done |
| ABI/wb_process_is_running | Exact declaration/layout and defined symbol | b_runtime.m | wb_process_is_running | done |
| ABI/wb_crash_exit_status | Exact declaration/layout and defined symbol | b_runtime.m | wb_crash_exit_status | done |
| ABI/wb_exit_status_reset | Exact declaration/layout and defined symbol | b_runtime.m | wb_exit_status_reset | done |
| ABI/wine_crash_exit_status | Exact declaration/layout and defined symbol | b_runtime.m | wine_crash_exit_status | done |
| ABI/wb_seed_prefix | Exact declaration/layout and defined symbol | b_prefix.m | wb_seed_prefix | done |
| ABI/wb_log_path | Exact declaration/layout and defined symbol | b_runtime.m | wb_log_path | done |
| ABI/wb_display_set_layer | Exact declaration/layout and defined symbol | b_display.m | wb_display_set_layer | done |
| ABI/wb_screen_size | Exact declaration/layout and defined symbol | b_display.m | wb_screen_size | done |
| ABI/wb_overlay_set_frame | Exact declaration/layout and defined symbol | b_display.m | wb_overlay_set_frame | done |
| ABI/wb_overlay_set_desktop_rect | Exact declaration/layout and defined symbol | b_display.m | wb_overlay_set_desktop_rect | done |
| ABI/wb_overlay_show | Exact declaration/layout and defined symbol | b_display.m | wb_overlay_show | done |
| ABI/wb_game_picture_rect | Exact declaration/layout and defined symbol | b_display.m | wb_game_picture_rect | done |
| ABI/wb_input_key | Exact declaration/layout and defined symbol | b_input.c | wb_input_key | done |
| ABI/wb_input_pointer | Exact declaration/layout and defined symbol | b_input.c | wb_input_pointer | done |
| ABI/wb_input_touch | Exact declaration/layout and defined symbol | b_input.c | wb_input_touch | done |
| ABI/winios_gamepad_set_state | Exact declaration/layout and defined symbol | b_pads.c | winios_gamepad_set_state | done |
| ABI/winios_hidpad_set_state | Exact declaration/layout and defined symbol | b_pads.c | winios_hidpad_set_state | done |
| ABI/wine_ios_exit_jmpbuf | Exact declaration/layout and defined symbol | b_runtime.m | wine_ios_exit_jmpbuf | done |
| ABI/wine_ios_exit_code | Exact declaration/layout and defined symbol | b_runtime.m | wine_ios_exit_code | done |
| ABI/wine_ios_main_thread | Exact declaration/layout and defined symbol | b_runtime.m | wine_ios_main_thread | done |
| ABI/wine_ios_exit_initialized | Exact declaration/layout and defined symbol | b_runtime.m | wine_ios_exit_initialized | done |
| ABI/wine_launched_process_did_exit | Exact declaration/layout and defined symbol | b_runtime.m | wine_launched_process_did_exit | done |
| ABI/winios_phase | Exact declaration/layout and defined symbol | b_runtime.m | winios_phase | done |
| ABI/fatal_error | Exact declaration/layout and defined symbol | b_runtime.m | fatal_error | done |
| ABI/g_wineserver_should_stop | Exact declaration/layout and defined symbol | b_runtime.m | g_wineserver_should_stop | done |
| ABI/wine_build | Exact declaration/layout and defined symbol | b_stubs.m | wine_build | done |
| ABI/IOPSCopyPowerSourcesInfo | Exact declaration/layout and defined symbol | b_stubs.m | IOPSCopyPowerSourcesInfo | done |
| ABI/IOPSCopyPowerSourcesList | Exact declaration/layout and defined symbol | b_stubs.m | IOPSCopyPowerSourcesList | done |
| ABI/IOPSGetPowerSourceDescription | Exact declaration/layout and defined symbol | b_stubs.m | IOPSGetPowerSourceDescription | done |
| ABI/winios_cursor_set | Exact declaration/layout and defined symbol | b_stubs.m | winios_cursor_set | done |
| ABI/winios_cursor_show | Exact declaration/layout and defined symbol | b_stubs.m | winios_cursor_show | done |
| ABI/winios_direct_cursor_wanted | Exact declaration/layout and defined symbol | b_stubs.m | winios_direct_cursor_wanted | done |
| ABI/winios_direct_cursor_set | Exact declaration/layout and defined symbol | b_stubs.m | winios_direct_cursor_set | done |
| ABI/winios_direct_cursor_show | Exact declaration/layout and defined symbol | b_stubs.m | winios_direct_cursor_show | done |
| ABI/winios_direct_cursor_pos | Exact declaration/layout and defined symbol | b_stubs.m | winios_direct_cursor_pos | done |
| ABI/winios_pCreateWindow | Exact declaration/layout and defined symbol | b_input.c | winios_pCreateWindow | done |
| ABI/winios_pShowWindow | Exact declaration/layout and defined symbol | b_input.c | winios_pShowWindow | done |
| ABI/winios_pWindowPosChanged | Exact declaration/layout and defined symbol | b_input.c | winios_pWindowPosChanged | done |
| ABI/winios_pProcessEvents | Exact declaration/layout and defined symbol | b_input.c | winios_pProcessEvents | done |
| ABI/winios_pSetCursor | Exact declaration/layout and defined symbol | b_input.c | winios_pSetCursor | done |
| ABI/winios_pDestroyCursorIcon | Exact declaration/layout and defined symbol | b_input.c | winios_pDestroyCursorIcon | done |
| ABI/winios_pDestroyWindow | Exact declaration/layout and defined symbol | b_display.m | winios_pDestroyWindow | done |
| ABI/winios_surface_present | Exact declaration/layout and defined symbol | b_display.m | winios_surface_present | done |
| ABI/winios_window_frame | Exact declaration/layout and defined symbol | b_display.m | winios_window_frame | done |
| ABI/winios_display_mode_changed | Exact declaration/layout and defined symbol | b_display.m | winios_display_mode_changed | done |
| ABI/winios_session_reset | Exact declaration/layout and defined symbol | b_display.m | winios_session_reset | done |
| ABI/macdrv_functions | Exact declaration/layout and defined symbol | b_display.m | macdrv_functions | done |
| ABI/get_win_data | Exact declaration/layout and defined symbol | b_display.m | get_win_data | done |
| ABI/release_win_data | Exact declaration/layout and defined symbol | b_display.m | release_win_data | done |
| ABI/macdrv_view_create_metal_view | Exact declaration/layout and defined symbol | b_display.m | macdrv_view_create_metal_view | done |
| ABI/macdrv_view_get_metal_layer | Exact declaration/layout and defined symbol | b_display.m | macdrv_view_get_metal_layer | done |
| ABI/macdrv_view_release_metal_view | Exact declaration/layout and defined symbol | b_display.m | macdrv_view_release_metal_view | done |
| ABI/struct win_data | Exact declaration/layout and defined symbol | b_display.m | struct win_data | done |
| ABI/winios_gamepad_get_state | Exact declaration/layout and defined symbol | b_pads.c | winios_gamepad_get_state | done |
| ABI/winios_gamepad_set_vibration | Exact declaration/layout and defined symbol | b_pads.c | winios_gamepad_set_vibration | done |
| ABI/winios_gamepad_rumble_caps | Exact declaration/layout and defined symbol | b_pads.c | winios_gamepad_rumble_caps | done |
| ABI/winios_hidpad_get_state | Exact declaration/layout and defined symbol | b_pads.c | winios_hidpad_get_state | done |
| ABI/winios_hidpad_set_output | Exact declaration/layout and defined symbol | b_pads.c | winios_hidpad_set_output | done |
| ABI/wb_b_get_vibration | Exact declaration/layout and defined symbol | b_pads.c | wb_b_get_vibration | done |
| ABI/wb_b_get_hid_output | Exact declaration/layout and defined symbol | b_pads.c | wb_b_get_hid_output | done |
| ABI/struct winios_gamepad | Exact declaration/layout and defined symbol | b_internal.h, b_pads.c | struct winios_gamepad | done |
| ABI/struct winios_hidpad | Exact declaration/layout and defined symbol | b_internal.h, b_pads.c | struct winios_hidpad | done |
| ABI/struct winios_hidpad_output | Exact declaration/layout and defined symbol | b_internal.h, b_pads.c | struct winios_hidpad_output | done |
| ABI/struct winios_gamepad_vibration | Exact declaration/layout and defined symbol | b_internal.h, b_pads.c | struct winios_gamepad_vibration | done |
| Clarification 1 | Running drops only after joined server | bridge sources | corresponding hooks | done |
| Clarification 2 | Reset stop flag for each server | bridge sources | corresponding hooks | done |
| Clarification 3 | Non-interface exported helpers use wb_ prefix | bridge sources | corresponding hooks | done |
| Clarification 4 | Only C-rooted paths map drive_c | bridge sources | corresponding hooks | done |
| Clarification 5 | INITIAL_CWD lifetime; PWD only after successful chdir | bridge sources | corresponding hooks | done |
| Clarification 6 | Reset published display size per session | bridge sources | corresponding hooks | done |
| Clarification 7 | Disabled overlay still records frames/picture; surfaces handled without copy | bridge sources | corresponding hooks | done |
| Clarification 8 | Case-sensitive x64 test | bridge sources | corresponding hooks | done |
| Clarification 9 | Remove stale x86_64 runtime symlinks before aarch64 farms | bridge sources | corresponding hooks | done |
| Clarification 10 | Leave other dosdevices entries alone | bridge sources | corresponding hooks | done |
| Clarification 11 | Tar drops ./ then prefix/ | bridge sources | corresponding hooks | done |
| Clarification 12 | Tar write errors skip/continue and return-1; structural errors stop | bridge sources | corresponding hooks | done |
| Clarification 13 | Skipped SxS removes manifest and folder | bridge sources | corresponding hooks | done |
| Clarification 14 | Either environment or config0 disables LocalLow | bridge sources | corresponding hooks | done |
| Clarification 15 | Seed cache valid only while c: exists | bridge sources | corresponding hooks | done |
| Clarification 16 | Known-position empty layers; excluded windows retain images | bridge sources | corresponding hooks | done |
| Clarification 17 | Verbose requires nonempty and first byte not0 | bridge sources | corresponding hooks | done |
| Clarification 18 | Nonconst macdrv_functions | bridge sources | corresponding hooks | done |
