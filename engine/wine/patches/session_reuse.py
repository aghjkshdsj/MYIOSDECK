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
         a late thread of it cannot close a number another process got since). A process's main
         thread that the server killed ends its process through the normal exit path (KILLED_*).
         wine/dlls/ntdll/unix/sync.c, build/ntdll-unix/signal_arm64_ios.c and virtual_ios.c: the
         threads an exited process left asleep in its guest window are ended (REAP_*), so the
         window and its memory go back when the next 32-bit program starts.
  server wine/server/process.c, build/wineserver/request_ios.c and fd_ios.c: the app's Force
         quit ends every program of the session, as TerminateProcess does (FORCE_*).
  win32u wine/dlls/win32u/window.c (engine/wine/build-unix.sh): a window's client object (WND) is
         taken from the shared slot only when it is that window's (WINDOW_FIXES, below); and
         build/win32u-unix/class_ios.c: [winproc] diagnostics.

Usage: session_reuse.py <madeira checkout> app|ntdll|server|win32u. Idempotent (marker line).
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
static BOOL g_session_arm64ec;    /* the first program's farm in system32: ARM64EC (64-bit) or aarch64 */
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
    # A later program of the other kind (a 64-bit game after a 32-bit one, or the reverse) runs as
    # Madeira's cross-architecture children do: system32 stays the session's farm, whose ntdll and
    # system DLLs the running session uses (an x64 process finds its own in sysx64, a 32-bit one
    # its wow64 DLLs, which only the aarch64 farm has and which are linked into an ARM64EC
    # system32 as non-colliding names). Relinking it to the new program's farm would swap the
    # session's DLLs under it.
    ("        if (is_i386_target) use_arm64ec = NO;\n",
     "        if (is_i386_target) use_arm64ec = NO;\n"
     "        if (g_session_booted) use_arm64ec = g_session_arm64ec;   /* MYIOSDECK session reuse */\n"
     "        else g_session_arm64ec = use_arm64ec;\n"),
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
    # A killed main thread ends its process (KILLED_FIXES has the call).
    ('    wine_log_write("[Wine ntdll/server] process_exit_wrapper(%d)", status );\n'
     "    exit( status );  /* on iOS, wine_ios_exit shim longjmps back to wine_process_thread */\n"
     "}\n",
     '    wine_log_write("[Wine ntdll/server] process_exit_wrapper(%d)", status );\n'
     "    exit( status );  /* on iOS, wine_ios_exit shim longjmps back to wine_process_thread */\n"
     "}\n"
     r'''
#ifdef WINE_IOS
/* MYIOSDECK session-reuse (engine/wine/patches/session_reuse.py): the server killed this process
 * (a parent's TerminateProcess, the app's Force quit) while its main thread waited for it.
 * abort_thread() ended that thread alone (pthread_exit), so the boot thread never got back to its
 * setjmp: the process's slot, socket and guest window stayed taken and the app never learnt that
 * the program had ended. The main thread ends the process instead, as exit_process() does. Its
 * requests fail from here on instead of ending the thread (the deferred-abort state of a section,
 * above): the server has let go of it, and process_exit_wrapper() only closes and releases what
 * this side holds. */
void myiosdeck_main_thread_killed( int status )
{
    static _Thread_local int dying;
    static int logged;
    const char *reuse = getenv( "MYIOSDECK_SESSION_REUSE" );

    if (!reuse || reuse[0] != '1') return;
    if (!wine_ios_exit_initialized || !pthread_equal( pthread_self(), wine_ios_main_thread )) return;
    if (dying++) return;   /* the exit path itself was cut short: the thread just ends */
    ios_section_set_state( IOS_SECTION_ABORT | IOS_SECTION_EXITING | 1 );
    if (__sync_fetch_and_add( &logged, 1 ) < 16)
        dprintf( 2, "[session-reuse] the main thread of process %04x was killed (status %#x): the process "
                    "ends through its exit path\n", (unsigned int)GetCurrentProcessId(), (unsigned int)status );
    process_exit_wrapper( status ? status : 1 );
}
#endif
'''),
]

KILLED_FIXES = [
    ("void abort_thread( int status )\n"
     "{\n"
     "    pthread_sigmask( SIG_BLOCK, &server_block_set, NULL );\n",
     "void abort_thread( int status )\n"
     "{\n"
     "    pthread_sigmask( SIG_BLOCK, &server_block_set, NULL );\n"
     "    {   " + MARK + "\n"
     "        extern void myiosdeck_main_thread_killed( int status );\n"
     "        myiosdeck_main_thread_killed( status );   /* a pseudo-process's main thread: its process ends */\n"
     "    }\n"),
]


# The threads an exited process leaves asleep. A process ends (ExitProcess, TerminateProcess) by
# the server killing its threads; a thread learns it from its next server request (the pipe is
# closed) and exits there. DXMT's encode, finish and other worker threads sleep in address waits
# (NtWaitForAlertByThreadId) or delays that never ask the server, so they stayed (6 per D3D game,
# build 159), their TEBs inside the guest window, and Madeira keeps a window while a thread of it
# lives (ios_thread_registry_range_busy): its gigabytes never came back. myiosdeck_reap_alert()
# wakes such a thread and moves a generation; a thread that sees a new generation when it wakes
# or enters a wait asks the server about itself, and a killed one exits right there (abort_thread)
# as at any request.
REAP_HELPERS = MARK + r'''
/* MYIOSDECK session-reuse (engine/wine/patches/session_reuse.py): see myiosdeck_reap_range()
 * (signal_arm64_ios.c) and myiosdeck_kill_programs() (the wineserver's process.c). */
volatile LONG myiosdeck_reap_gen;
static __thread LONG myiosdeck_reap_seen;

void myiosdeck_reap_alert( unsigned int tid )
{
    InterlockedIncrement( &myiosdeck_reap_gen );
    NtAlertThreadByThreadId( ULongToHandle( tid ) );
}

/* A new generation since this thread last looked: is it still alive? A killed thread's request
 * finds its pipe closed and the thread exits in send_request (abort_thread). */
static void myiosdeck_reap_check(void)
{
    LONG gen = myiosdeck_reap_gen;

    if (gen == myiosdeck_reap_seen) return;
    myiosdeck_reap_seen = gen;
    SERVER_START_REQ( get_thread_info )
    {
        req->handle = wine_server_obj_handle( GetCurrentThread() );
        req->access = THREAD_QUERY_LIMITED_INFORMATION;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

'''

REAP_SYNC_FIXES = [
    ("/******************************************************************\n"
     " *\t\tNtDelayExecution (NTDLL.@)\n"
     " */\n"
     "NTSTATUS WINAPI NtDelayExecution( BOOLEAN alertable, const LARGE_INTEGER *timeout )\n"
     "{\n"
     "    unsigned int status = STATUS_SUCCESS;\n"
     "\n"
     "    IOS_ECO_POLL();   /* ml1133 */\n",
     REAP_HELPERS +
     "/******************************************************************\n"
     " *\t\tNtDelayExecution (NTDLL.@)\n"
     " */\n"
     "NTSTATUS WINAPI NtDelayExecution( BOOLEAN alertable, const LARGE_INTEGER *timeout )\n"
     "{\n"
     "    unsigned int status = STATUS_SUCCESS;\n"
     "\n"
     "    IOS_ECO_POLL();   /* ml1133 */\n"
     "    myiosdeck_reap_check();   /* MYIOSDECK session-reuse */\n"),
    ("    IOS_ECO_POLL();   /* ml1133 */\n"
     "    __sync_fetch_and_add( &ios_alert_waits, 1 );   /* ml1115 */\n",
     "    IOS_ECO_POLL();   /* ml1133 */\n"
     "    __sync_fetch_and_add( &ios_alert_waits, 1 );   /* ml1115 */\n"
     "    myiosdeck_reap_check();   /* MYIOSDECK session-reuse */\n"),
    ("        if (ios_wslot >= 0) ios_alert_waiters[ios_wslot].addr = NULL;\n"
     "        if (ios_marked && ios_alert_unix_n < 120) { InterlockedIncrement( &ios_alert_unix_n );\n"
     "            ERR( \"[alert-unix] tid=%04x -> ALERTED\\n\",\n",
     "        if (ios_wslot >= 0) ios_alert_waiters[ios_wslot].addr = NULL;\n"
     "        myiosdeck_reap_check();   /* MYIOSDECK session-reuse: woken to end? */\n"
     "        if (ios_marked && ios_alert_unix_n < 120) { InterlockedIncrement( &ios_alert_unix_n );\n"
     "            ERR( \"[alert-unix] tid=%04x -> ALERTED\\n\",\n"),
]

# The reaper (build/ntdll-unix/signal_arm64_ios.c, next to the thread registry it walks).
REAP_SIGNAL_FIXES = [
    ("static int ios_lookup_thread(thread_t mach_thread, uintptr_t *teb_out, void **tramp_out)\n",
     MARK + r'''
/* MYIOSDECK session-reuse (engine/wine/patches/session_reuse.py): end the threads an exited
 * process left asleep in its guest window, so the window can be torn down. With
 * MYIOSDECK_SESSION_REUSE=1 only; called by the window reclaim (virtual_ios.c) for a window whose
 * owner has exited.
 *
 * First every such thread is woken (myiosdeck_reap_alert, sync.c): one asleep in an address wait
 * or a delay then asks the server about itself and, killed with its process, exits through
 * Wine's own path. What is left after half a second sleeps where no alert reaches it (DXMT's
 * shared-event listener runs a CFRunLoop in mach_msg): a thread suspended inside a system call
 * holds no lock of this side, so its server pipes are closed and the thread is terminated.
 * A thread running anything else is left alone, and keeps the window as before. */
static int myiosdeck_thread_alive( thread_t port )
{
    struct thread_basic_info info;
    mach_msg_type_number_t length = THREAD_BASIC_INFO_COUNT;
    kern_return_t kr = thread_info( port, THREAD_BASIC_INFO, (thread_info_t)&info, &length );

    return kr != KERN_INVALID_ARGUMENT && kr != MACH_SEND_INVALID_DEST && kr != KERN_TERMINATED;
}

int myiosdeck_reap_range( uintptr_t base, uintptr_t size )
{
    extern void myiosdeck_reap_alert( unsigned int tid );
    extern void ios_thread_died( unsigned tid );
    const char *reuse = getenv( "MYIOSDECK_SESSION_REUSE" );
    int count = ios_thread_registry_count(), i, wait, woken = 0, stopped = 0, parked = 0, left = 0;
    mach_port_t self;

    if (!reuse || reuse[0] != '1') return 0;
    self = mach_thread_self();
    for (i = 0; i < count; i++)
    {
        uintptr_t teb = ios_thread_registry[i].teb;
        thread_t port = ios_thread_registry[i].mach_thread;

        if (!port || port == self || !teb || teb < base || teb - base >= size) continue;
        if (!myiosdeck_thread_alive( port )) continue;
        myiosdeck_reap_alert( (unsigned int)(ULONG_PTR)((TEB *)teb)->ClientId.UniqueThread );
        woken++;
    }
    for (wait = 0; woken && wait < 50 && ios_thread_registry_range_busy( base, size ); wait++) usleep( 10000 );
    for (i = 0; woken && i < count; i++)
    {
        uintptr_t teb = ios_thread_registry[i].teb;
        thread_t port = ios_thread_registry[i].mach_thread;
        struct ntdll_thread_data *td;
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        struct thread_basic_info info;
        mach_msg_type_number_t length = THREAD_BASIC_INFO_COUNT;
        uint64_t pc = 0;
        uint32_t insn = 0;
        mach_vm_size_t got = 0;
        unsigned int tid;
        kern_return_t kr;

        if (!port || port == self || !teb || teb < base || teb - base >= size) continue;
        if (!myiosdeck_thread_alive( port )) continue;
        /* asleep before it is stopped: a suspended thread reports TH_STATE_STOPPED (build 161: the
         * run-loop thread was left running for that reason) */
        if (thread_info( port, THREAD_BASIC_INFO, (thread_info_t)&info, &length ) != KERN_SUCCESS ||
            info.run_state != TH_STATE_WAITING) { left++; continue; }
        if (thread_suspend( port ) != KERN_SUCCESS) { left++; continue; }
        if (thread_get_state( port, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt ) != KERN_SUCCESS ||
            !(pc = arm_thread_state64_get_pc( st )) ||
            mach_vm_read_overwrite( mach_task_self(), pc - 4, 4, (mach_vm_address_t)&insn, &got ) != KERN_SUCCESS ||
            got != 4 || insn != 0xd4001001 /* svc #0x80: inside a system call */)
        {
            thread_resume( port );
            dprintf( 2, "[session-reuse] a thread of an exited process (TEB %p) is not inside a system call "
                        "(pc %#llx, instruction before it %08x): left running\n", (void *)teb,
                     (unsigned long long)pc, insn );
            left++;
            continue;
        }
        td = (struct ntdll_thread_data *)&((TEB *)teb)->GdiTebBatch;
        tid = (unsigned int)(ULONG_PTR)((TEB *)teb)->ClientId.UniqueThread;
        if (td->request_fd > 2) close( td->request_fd );
        if (td->reply_fd > 2) close( td->reply_fd );
        if (td->wait_fd[0] > 2) close( td->wait_fd[0] );
        if (td->wait_fd[1] > 2) close( td->wait_fd[1] );
        if (td->alert_fd > 2) close( td->alert_fd );
        td->request_fd = td->reply_fd = td->wait_fd[0] = td->wait_fd[1] = td->alert_fd = -1;
        if ((kr = thread_terminate( port )) != KERN_SUCCESS)
        {
            /* iOS does not terminate a pthread from another thread (build 162: the run-loop thread
             * was refused). It stays suspended for good instead: asleep inside a system call it
             * holds nothing of this side, and suspended it never runs again, so it no longer counts
             * as the window's (ios_thread_registry_range_busy). */
            ios_thread_registry[i].mach_thread = 0;
            __sync_synchronize();
            ios_thread_registry[i].teb = 0;
            ios_thread_registry[i].trampoline = NULL;
            dprintf( 2, "[session-reuse] thread %04x of an exited process: not terminated (%d), kept suspended\n",
                     tid, (int)kr );
            parked++;
        }
        else stopped++;
        ios_thread_died( tid );
    }
    /* a woken thread may still be on its way out (pthread_exit) */
    for (wait = 0; woken && wait < 30 && ios_thread_registry_range_busy( base, size ); wait++) usleep( 10000 );
    mach_port_deallocate( mach_task_self(), self );
    if (woken)
        dprintf( 2, "[session-reuse] window B=%p: %d thread(s) the exited process left were woken to end "
                    "themselves, %d asleep for good were stopped, %d kept suspended, %d still run there\n",
                 (void *)base, woken, stopped, parked, left );
    return woken;
}

static int ios_lookup_thread(thread_t mach_thread, uintptr_t *teb_out, void **tramp_out)
'''),
]

# The reclaim (build/ntdll-unix/virtual_ios.c) asks the reaper before keeping a window for its
# live threads.
REAP_VIRTUAL_FIXES = [
    ("            if ((!env || strcmp( env, \"0\" )) &&\n"
     "                ios_thread_registry_range_busy( base, IOS_WOW_WINDOW_SIZE ))\n",
     "            extern int myiosdeck_reap_range( uintptr_t base, uintptr_t size );   " + MARK + "\n"
     "            if ((!env || strcmp( env, \"0\" )) &&\n"
     "                ios_thread_registry_range_busy( base, IOS_WOW_WINDOW_SIZE ) &&\n"
     "                (!myiosdeck_reap_range( base, IOS_WOW_WINDOW_SIZE ) ||\n"
     "                 ios_thread_registry_range_busy( base, IOS_WOW_WINDOW_SIZE )))\n"),
]


# The app's Force quit (the wineserver, on its own thread): every program of the session ends as
# TerminateProcess ends it (wine/server/process.c), asked through a flag the server loop reads
# (build/wineserver/request_ios.c, fd_ios.c), as the app hands it a new client socket. The
# programs' threads that sleep where no server reply reaches them are woken to find out
# (myiosdeck_reap_alert); a killed main thread ends its process (myiosdeck_main_thread_killed).
FORCE_PROCESS_FIXES = [
    ("/* kill all processes */\n"
     "static void kill_all_processes(void)\n",
     MARK + r'''
/* MYIOSDECK session-reuse (engine/wine/patches/session_reuse.py): the app's Force quit. Every
 * program of the session (not a system process) is terminated as TerminateProcess terminates it;
 * then its threads are woken, so one asleep where no server reply reaches it learns it was killed
 * (ntdll's sync.c). Runs on the server thread, from the main loop (fd_ios.c). */
extern void myiosdeck_reap_alert( unsigned int tid );
int myiosdeck_kill_programs( int exit_code )
{
    struct process *process, *victims[32];
    struct thread *thread;
    unsigned int tids[512];
    int n = 0, nt = 0, i;

    LIST_FOR_EACH_ENTRY( process, &process_list, struct process, entry )
    {
        if (process->is_system || !process->running_threads || n >= 32) continue;
        victims[n++] = (struct process *)grab_object( process );
        LIST_FOR_EACH_ENTRY( thread, &process->thread_list, struct thread, proc_entry )
            if (nt < 512) tids[nt++] = thread->id;
    }
    for (i = 0; i < n; i++)
    {
        terminate_process( victims[i], NULL, exit_code );
        release_object( victims[i] );
    }
    for (i = 0; i < nt; i++) myiosdeck_reap_alert( tids[i] );
    return n;
}

/* kill all processes */
static void kill_all_processes(void)
'''),
]

FORCE_REQUEST_FIXES = [
    ("    ws_log(\"[wineserver] inject_client_fd: fd=%d\", fd);\n"
     "    g_injected_client_fd = fd;\n"
     "}\n",
     "    ws_log(\"[wineserver] inject_client_fd: fd=%d\", fd);\n"
     "    g_injected_client_fd = fd;\n"
     "}\n"
     "\n" + MARK + "\n"
     "/* The app's Force quit (engine/wine/patches/session_reuse.py): the server loop ends every\n"
     " * program of the session (myiosdeck_kill_programs, process.c) on its own thread. */\n"
     "volatile int g_myiosdeck_kill_programs = -1;\n"
     "void wineserver_kill_programs(int exit_code)\n"
     "{\n"
     "    extern void ios_wineserver_wake(void);\n"
     "    ws_log(\"[wineserver] kill_programs: exit code %d (the app's Force quit)\", exit_code);\n"
     "    g_myiosdeck_kill_programs = exit_code;\n"
     "    ios_wineserver_wake();\n"
     "}\n"),
]

FORCE_FD_FIXES = [
    ("            /* Check for injected client fd (socketpair bypass) */\n",
     "            {   " + MARK + "   /* the app's Force quit (wineserver_kill_programs) */\n"
     "                extern volatile int g_myiosdeck_kill_programs;\n"
     "                extern int myiosdeck_kill_programs( int exit_code );\n"
     "                int code = __atomic_exchange_n( &g_myiosdeck_kill_programs, -1, __ATOMIC_SEQ_CST );\n"
     "                if (code >= 0)\n"
     "                    ws_log( \"[wineserver] force quit: %d program process(es) terminated\",\n"
     "                            myiosdeck_kill_programs( code ) );\n"
     "            }\n"
     "            /* Check for injected client fd (socketpair bypass) */\n"),
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


# The fix (Wine's dlls/win32u/window.c, from build-unix.sh): win32u keeps one client object (the
# WND) per user handle index, client_objects[], per process in Wine and shared by every
# pseudo-process here, and takes the slot as this window's once the session's shared handle table
# says the window belongs to this process. A window the wineserver itself made for this process
# (its desktop and message windows, made for a process without them) has no client object, and its
# index was usually an exited process's window: the slot still held that process's WND, freed or
# not. Forager's second start in a kept session (build 158) dispatched WM_PARENTNOTIFY to its
# message window through such a WND and called its window procedure, 0x3c. A WND is the window's
# only if its own handle is the window's full handle (generation included); otherwise the window
# has no client object here, as in Wine.
WINDOW_FIXES = [
    ("    else ptr = client_objects[index];\n",
     "    else ptr = client_objects[index];\n"
     "    " + MARK + "\n"
     "    if (ptr && ptr != OBJ_OTHER_PROCESS && type == NTUSER_OBJ_WINDOW && ((WND *)ptr)->handle != handle)\n"
     "        ptr = NULL;   /* another (exited) process's WND in this index's slot */\n"),
    ("        if (tid != -1 && entry.tid != tid) continue;\n",
     "        if (tid != -1 && entry.tid != tid) continue;\n"
     "        if (type == NTUSER_OBJ_WINDOW && client_objects[i] &&\n"
     "            ((WND *)client_objects[i])->handle != *handle) continue;   /* MYIOSDECK: another process's WND */\n"),
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
        apply(root + "/build/ntdll-unix/thread_ios.c", KILLED_FIXES, what + " thread_ios.c")
        apply(root + "/wine/dlls/ntdll/unix/sync.c", REAP_SYNC_FIXES, what + " sync.c")
        apply(root + "/build/ntdll-unix/signal_arm64_ios.c", REAP_SIGNAL_FIXES, what + " signal_arm64_ios.c")
        apply(root + "/build/ntdll-unix/virtual_ios.c", REAP_VIRTUAL_FIXES, what + " virtual_ios.c")
    elif what == "server":
        apply(root + "/wine/server/process.c", FORCE_PROCESS_FIXES, what + " process.c")
        apply(root + "/build/wineserver/request_ios.c", FORCE_REQUEST_FIXES, what + " request_ios.c")
        apply(root + "/build/wineserver/fd_ios.c", FORCE_FD_FIXES, what + " fd_ios.c")
    elif what == "win32u":
        apply(root + "/build/win32u-unix/class_ios.c", WIN32U_FIXES, what)
        apply(root + "/wine/dlls/win32u/window.c", WINDOW_FIXES, what + " window.c")
    else:
        sys.exit("usage: session_reuse.py <madeira checkout> app|ntdll|server|win32u")


if __name__ == "__main__":
    main()
