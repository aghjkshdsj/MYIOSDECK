#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Session reuse (docs/SESSION_REUSE.md): another game starts in the running Wine session.

Madeira runs one Wine session per app run: its wineserver cannot start a second time in one
process (init_registry aborts on "\\Registry"), and __wine_main's session-wide setup (virtual_init,
the signal and Mach handlers, the first TEB) cannot run twice either. A Wine session can still
hold more than one top-level program, as on a desktop where `wine b.exe` joins the wineserver
`wine a.exe` started: the server takes any number of clients (request_ios.c,
master_socket_handle_client is create_process with no parent), and Madeira already brings
processes up inside a running session for a program's children (loader_ios.c,
wine_ios_child_main). A program with no parent gets no startup info from the server, so the
loader builds its process from argv and the environment, as for the session's first program
(env_ios.c, build_initial_params).

  app    app/Madeira/WineProcessBridge.m (engine/wine/stage-app.sh): with
         MYIOSDECK_SESSION_REUSE=1 a session's end leaves the wineserver running, and the next
         wine_process_start boots its program through wine_ios_child_main on a new socketpair
         instead of __wine_main. Without the variable nothing changes.
  ntdll  build/ntdll-unix/server_ios.c (engine/wine/build-unix.sh): the process socket table
         (64 entries, never reused) reuses the entries of exited processes once it is full, and
         the session's first program closes its socket for good when it exits (fd_socket = -1, so
         a late thread of it cannot close a number another process got since).
  win32u build/win32u-unix/class_ios.c (engine/wine/build-unix.sh): [winproc] diagnostics, below.

Usage: session_reuse.py <madeira checkout> app|ntdll|win32u. Idempotent (marker line).
"""
import sys

MARK = "/* MYIOSDECK session-reuse */"

APP_HELPERS = MARK + r'''
/* MYIOSDECK session reuse (engine/wine/patches/session_reuse.py, docs/SESSION_REUSE.md).
 *
 * With MYIOSDECK_SESSION_REUSE=1 the end of a session leaves the wineserver running, and the
 * next wine_process_start boots its program as a new top-level process of that session: a new
 * client of the running server (its own socketpair, as for the first program), brought up by
 * wine_ios_child_main, the path a program's children take here. The server gives a process with
 * no parent no startup info, so the loader builds it from argv and the environment as for the
 * first program. __wine_main runs once per app run. */
static int g_session_booted;      /* __wine_main has run in this app run */
static int g_next_fd = -1;        /* a later program's end of its server socketpair */
static int g_stale_children;      /* children of earlier programs still listed when this one started */
extern void wine_ios_child_main(int argc, char *argv[], int child_fd_socket);   /* loader_ios.c */
extern _Thread_local uint16_t ios_child_main_machine;   /* process_ios.c (WORD) */
extern void ios_child_boot_unlock(void);                 /* loader_ios.c */
extern void ios_wow_window_release_current(void);        /* virtual_ios.c */
extern void ios_wow_window_retire_current(void);         /* virtual_ios.c */
extern pthread_key_t ios_teb_tls_key;
extern int madeira_live_game_children(char *buf, int len, double max_age);   /* process_ios.c */
static int myiosdeck_session_reuse(void)
{
    const char *v = getenv("MYIOSDECK_SESSION_REUSE");
    return v && v[0] == '1';
}
/* 1 when another program can start in this session now: the first one has run, nothing runs
 * and the wineserver was kept. */
int wine_session_reusable(void)
{
    return g_session_booted && !g_wine_running && wineserver_is_running();
}
'''

APP_FIXES = [
    # The helpers, after the last static they use (g_wine_running is above it).
    ("static char *g_prefix_path = NULL;\n",
     "static char *g_prefix_path = NULL;\n" + APP_HELPERS),
    # A later program's server socket is not the first program's WINESERVERSOCKET (only
    # server_init_process reads that); wine_process_thread hands it to wine_ios_child_main.
    ('    setenv("WINESERVERSOCKET", fd_str, 1);\n',
     '    if (g_session_booted) g_next_fd = pair[1];   /* MYIOSDECK session reuse: a later program */\n'
     '    else setenv("WINESERVERSOCKET", fd_str, 1);\n'),
    # argv and the image path stay readable after the boot (main_argv points at them).
    ("        char exe_path[512];\n", "        static char exe_path[512];   /* MYIOSDECK session reuse: main_argv[1] */\n"),
    ("        char *argv[72];\n", "        static char *argv[72];   /* MYIOSDECK session reuse: main_argv */\n"),
    # An earlier program's working folder is not this one's.
    ('        const char *launch_workdir = getenv("MADEIRA_WORKDIR");',
     '        if (g_session_booted) unsetenv("MADEIRA_INITIAL_CWD");   /* MYIOSDECK session reuse */\n'
     '        const char *launch_workdir = getenv("MADEIRA_WORKDIR");'),
    # The boot: __wine_main once, then wine_ios_child_main for every later program.
    ("        ios_main_image_i386 = is_i386_target ? 1 : 0;\n"
     "        madeira_publish_host_probe();   /* both FEX modules read it (A12/A13: FlagM, FlagM2) */\n"
     "\n"
     "        if (setjmp(wine_ios_exit_jmpbuf) == 0) {\n"
     "            __wine_main(argc, argv);\n"
     '            dprintf(STDERR_FILENO, "[WineProc] __wine_main returned normally\\n");\n'
     "        } else {\n"
     '            dprintf(STDERR_FILENO, "[WineProc] Wine exited with code %d (caught by longjmp)\\n", wine_ios_exit_code);\n'
     "        }\n",
     "        /* MYIOSDECK session reuse: a later program keeps the session's main-image machine\n"
     "         * (the unix side read it when the session started) and boots like a child. */\n"
     "        volatile int next_program = g_session_booted;\n"
     "        if (!next_program) ios_main_image_i386 = is_i386_target ? 1 : 0;\n"
     "        madeira_publish_host_probe();   /* both FEX modules read it (A12/A13: FlagM, FlagM2) */\n"
     "        g_stale_children = next_program ? madeira_live_game_children(NULL, 0, -1.0) : 0;\n"
     "\n"
     "        if (setjmp(wine_ios_exit_jmpbuf) == 0) {\n"
     "            if (next_program) {\n"
     "                ios_child_main_machine = is_i386_target ? 0x014c : 0x8664;   /* i386: a guest window first */\n"
     '                dprintf(STDERR_FILENO, "[session-reuse] %s starts in the running Wine session as a new "\n'
     '                        "top-level process (%s, server fd %d, %d child process(es) of earlier programs listed)\\n",\n'
     '                        exe_path, is_i386_target ? "32-bit" : "64-bit", g_next_fd, g_stale_children);\n'
     "                wine_ios_child_main(argc, argv, g_next_fd);\n"
     "                /* it returns only when the boot failed (it named the stage) */\n"
     '                dprintf(STDERR_FILENO, "[session-reuse] %s did not start: its boot failed\\n", exe_path);\n'
     "                wine_launched_process_did_exit((int)0xC0000142);   /* STATUS_DLL_INIT_FAILED for the app */\n"
     "            } else {\n"
     "                g_session_booted = 1;\n"
     "                __wine_main(argc, argv);\n"
     '                dprintf(STDERR_FILENO, "[WineProc] __wine_main returned normally\\n");\n'
     "            }\n"
     "        } else {\n"
     '            dprintf(STDERR_FILENO, "[WineProc] Wine exited with code %d (caught by longjmp)\\n", wine_ios_exit_code);\n'
     "            /* The first program's status comes from ntdll (process_exit_wrapper); a later one has a\n"
     "             * process slot, like a child, whose exit reports nothing: report it here. */\n"
     "            if (next_program) wine_launched_process_did_exit(wine_ios_exit_code);\n"
     "        }\n"
     "        if (next_program) {\n"
     "            /* what ios_child_thread_entry does when a child's boot thread is done */\n"
     "            ios_child_boot_unlock();\n"
     "            ios_wow_window_release_current();   /* a boot that failed before binding its window */\n"
     "            pthread_setspecific(ios_teb_tls_key, NULL);\n"
     "        }\n"),
    # Children of earlier programs that never released their slot do not hold this session up.
    ("                while ((n = madeira_live_game_children(names, sizeof names, -1.0)) > 0) {\n",
     "                while ((n = madeira_live_game_children(names, sizeof names, -1.0)) > g_stale_children) {   /* MYIOSDECK */\n"),
    # The running flag drops once this thread's own clean-up is done: the next program's
    # environment (set by the app before it starts) must not lose the per-launch variables.
    ("        g_wine_running = 0;\n"
     "        /* ml1184: these belong to the launch that just ended; a later session in this app\n",
     "        const int keep_session = myiosdeck_session_reuse();   /* MYIOSDECK session reuse */\n"
     "        if (!keep_session) g_wine_running = 0;\n"
     "        /* ml1184: these belong to the launch that just ended; a later session in this app\n"),
    ('        // Stop wineserver to prevent CPU spin (iOS kills for excessive CPU)\n'
     '        dprintf(STDERR_FILENO, "[WineProc] stopping wineserver...\\n");\n'
     '        wineserver_stop();\n',
     '        if (keep_session) {\n'
     '            /* The first program\'s guest window is never handed on (it holds the PEB every later\n'
     '             * process is cloned from), but it must stop answering for this thread\'s pthread_t:\n'
     '             * ios_wow_slot_current() falls back to the slot\'s boot thread, and a later boot thread\n'
     '             * can get the same pthread_t (build 156: Forager booted into Steins;Gate\'s launcher\'s\n'
     '             * window). Retired, the slot keeps its memory and matches nothing. */\n'
     '            if (!next_program) ios_wow_window_retire_current();\n'
     '            dprintf(STDERR_FILENO, "[session-reuse] the wineserver keeps running: another program can start "\n'
     '                    "in this session\\n");\n'
     '        } else {\n'
     '        // Stop wineserver to prevent CPU spin (iOS kills for excessive CPU)\n'
     '        dprintf(STDERR_FILENO, "[WineProc] stopping wineserver...\\n");\n'
     '        wineserver_stop();\n'
     '        }\n'),
    ('        dprintf(STDERR_FILENO, "[WineProc] Wine process thread finished cleanly\\n");\n',
     '        dprintf(STDERR_FILENO, "[WineProc] Wine process thread finished cleanly\\n");\n'
     '        if (keep_session) g_wine_running = 0;   /* MYIOSDECK session reuse */\n'),
]

NTDLL_FIXES = [
    # Reuse the entry of an exited process once the table is full. The count never passes the
    # table (it did: readers then walked past its end), and the first 64 processes of an app run
    # take the same entries as before.
    ("static void ios_register_proc_socket(void *peb_id, int fd)\n"
     "{\n"
     "    int idx = __sync_fetch_and_add(&ios_proc_socket_count, 1);\n"
     "    if (idx >= IOS_MAX_PROC_SOCKETS)\n"
     "    {\n"
     '        wine_log_write("[Wine child] proc-socket table FULL (%d)!", idx);\n'
     "        return;\n"
     "    }\n"
     "    ios_fdt_reg( fd, FDT_MASTER, peb_id );\n"
     "    ios_proc_sockets[idx].fd = fd;\n"
     "    ios_proc_sockets[idx].exiting = FALSE;\n"
     "    __sync_synchronize();\n"
     "    ios_proc_sockets[idx].peb = peb_id;\n"
     "}\n",
     "static void ios_register_proc_socket(void *peb_id, int fd)\n"
     "{\n"
     "    " + MARK + "\n"
     "    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;\n"
     "    int idx = -1, i;\n"
     "\n"
     "    pthread_mutex_lock( &lock );\n"
     "    if (ios_proc_socket_count < IOS_MAX_PROC_SOCKETS) idx = ios_proc_socket_count;\n"
     "    else for (i = 0; i < IOS_MAX_PROC_SOCKETS; i++) if (!ios_proc_sockets[i].peb) { idx = i; break; }\n"
     "    if (idx < 0)\n"
     "    {\n"
     "        pthread_mutex_unlock( &lock );\n"
     '        wine_log_write("[Wine child] proc-socket table FULL (%d live processes)!", IOS_MAX_PROC_SOCKETS);\n'
     "        return;\n"
     "    }\n"
     "    if (idx < ios_proc_socket_count)\n"
     '        wine_log_write("[Wine child] proc-socket entry %d reused (its process exited)", idx);\n'
     "    ios_fdt_reg( fd, FDT_MASTER, peb_id );\n"
     "    ios_proc_sockets[idx].fd = fd;\n"
     "    ios_proc_sockets[idx].exiting = FALSE;\n"
     "    __sync_synchronize();\n"
     "    ios_proc_sockets[idx].peb = peb_id;\n"
     "    if (idx == ios_proc_socket_count) __atomic_store_n( &ios_proc_socket_count, idx + 1, __ATOMIC_RELEASE );\n"
     "    pthread_mutex_unlock( &lock );\n"
     "}\n"),
    # The session's first program: its socket number is free once closed, and may be another
    # process's socket by the time a late thread of this one calls the exit path again.
    ("        if (wine_launched_process_did_exit) wine_launched_process_did_exit( status );\n"
     "        close( fd_socket );\n",
     "        if (wine_launched_process_did_exit) wine_launched_process_did_exit( status );\n"
     "        if (fd_socket >= 0) close( fd_socket );\n"
     "        fd_socket = -1;   " + MARK + "\n"),
]


# Diagnostics (win32u's build/win32u-unix/class_ios.c, from build-unix.sh): Forager started as the
# second program of a kept session (build 156) dispatched a message to a window procedure that is
# 0x3c in its guest window. Every window procedure allocated in win32u's session-wide table (index,
# value, process, caller), and every dispatch whose procedure converts to a guest address below
# 64 KB (never code), are logged as [winproc] lines, 300 per run.
WIN32U_HELPER = MARK + r'''
extern ULONG_PTR ios_wow_base(void);
extern void mid_log( const char *fmt, ... );   /* the app's log (App/Sources/Native/jit_core.c): not through madeira-log.txt */
static unsigned int myiosdeck_winproc_lines;
static void myiosdeck_winproc_note( const WINDOWPROC *proc, WNDPROC func, BOOL ansi, void *caller )
{
    if (__atomic_fetch_add( &myiosdeck_winproc_lines, 1, __ATOMIC_RELAXED ) >= 300) return;
    mid_log( "[winproc] new %#x %c %p (peb %p, B %#lx, caller %p)",
             (unsigned int)(proc - winproc_array), ansi ? 'A' : 'W', func, NtCurrentTeb()->Peb,
             (unsigned long)ios_wow_base(), caller );
}

'''

WIN32U_FIXES = [
    ("static inline WINDOWPROC *alloc_winproc_ptr( WNDPROC func, BOOL ansi )\n",
     WIN32U_HELPER + "static inline WINDOWPROC *alloc_winproc_ptr( WNDPROC func, BOOL ansi )\n"),
    ("            if (ansi) proc->procA = func;\n"
     "            else proc->procW = func;\n"
     "            TRACE_(win)( \"allocated %p for %c %p (%d/%d used)\\n\",\n",
     "            if (ansi) proc->procA = func;\n"
     "            else proc->procW = func;\n"
     "            myiosdeck_winproc_note( proc, func, ansi, __builtin_return_address( 0 ) );\n"
     "            TRACE_(win)( \"allocated %p for %c %p (%d/%d used)\\n\",\n"),
    ("    if (!params->procA) params->procA = params->func;\n"
     "    if (!params->procW) params->procW = params->func;\n"
     "}\n",
     "    if (!params->procA) params->procA = params->func;\n"
     "    if (!params->procW) params->procW = params->func;\n"
     "    {   /* MYIOSDECK diagnostics: a 32-bit program's procedure below 64 KB is never code */\n"
     "        ULONG_PTR b = ios_wow_base();\n"
     "        ULONG a = (ULONG)(ULONG_PTR)params->procA, w = (ULONG)(ULONG_PTR)params->procW;\n"
     "        if (b && ((params->procA && a < 0x10000) || (params->procW && w < 0x10000)) &&\n"
     "            __atomic_fetch_add( &myiosdeck_winproc_lines, 1, __ATOMIC_RELAXED ) < 300)\n"
     "        {\n"
     "            WINDOWPROC *p = get_winproc_ptr( params->func );\n"
     "            int idx = (p && p != WINPROC_PROC16) ? (int)(p - winproc_array) : -1;\n"
     "            mid_log( \"[winproc] BAD hwnd %p msg %#x func %p -> A %p W %p ansi %d ansi_dst %d \"\n"
     "                     \"entry %d (A %p W %p) used %u peb %p own-builtins %d B %#lx\",\n"
     "                     params->hwnd, params->msg, params->func, params->procA, params->procW,\n"
     "                     params->ansi, params->ansi_dst, idx, idx >= 0 ? p->procA : NULL,\n"
     "                     idx >= 0 ? p->procW : NULL, winproc_used, NtCurrentTeb()->Peb,\n"
     "                     ios_client_procs_for_peb() != NULL, (unsigned long)b );\n"
     "        }\n"
     "    }\n"
     "}\n"),
]


def apply(path, fixes, what):
    s = open(path).read()
    if MARK in s:
        print("session_reuse (%s): already applied" % what)
        return
    for old, new in fixes:
        if s.count(old) != 1:
            sys.exit("session_reuse.py (%s): anchor found %d times: %r" % (what, s.count(old), old[:70]))
        s = s.replace(old, new)
    open(path, "w").write(s)
    print("session_reuse (%s): applied to %s" % (what, path))


def main():
    root, what = sys.argv[1], sys.argv[2]
    if what == "app":
        apply(root + "/app/Madeira/WineProcessBridge.m", APP_FIXES, what)
    elif what == "ntdll":
        apply(root + "/build/ntdll-unix/server_ios.c", NTDLL_FIXES, what)
    elif what == "win32u":
        apply(root + "/build/win32u-unix/class_ios.c", WIN32U_FIXES, what)
    else:
        sys.exit("usage: session_reuse.py <madeira checkout> app|ntdll|win32u")


if __name__ == "__main__":
    main()
