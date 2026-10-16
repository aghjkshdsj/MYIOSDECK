// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part C: starting the Windows program on its own thread. The numbered comments are
// the steps of "The launch thread, in this order".

#include "wb_internal.h"
#include "wb_log.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define TAG "wb-launch"

#define WINEDEBUG_VERBOSE "err+all,fixme+all,warn+module,trace+loaddll,warn+seh"
#define WINEDEBUG_QUIET   "err+all,err-virtual,fixme-d3dcompiler"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_running;

int wb_process_is_running(void)
{
    return atomic_load(&g_running);
}

/* ---- step helpers ---- */

static void set_debug_channels(void)
{
    const char *channels = wb_debug_verbose(getenv("MADEIRA_DEBUG_VERBOSE")) ? WINEDEBUG_VERBOSE : WINEDEBUG_QUIET;

    setenv("WINEDEBUG", channels, 1);
    wb_log(TAG, "WINEDEBUG=%s", channels);
}

static void set_steam_identity(void)
{
    static const char *const vars[] = { "SteamAppPath", "SteamGameId", "SteamAppId" };

    if (wb_env_is("MADEIRA_DOCK_SESSION", "1")) {
        /* Valve's own client starts the game and sets them itself. */
        for (int i = 0; i < 3; i++) unsetenv(vars[i]);
        wb_log(TAG, "steam: dock session, the Steam client sets the game identity");
    } else {
        const char *id_env = getenv("MADEIRA_STEAM_APPID");
        const char *path_env = getenv("MADEIRA_STEAM_APPPATH");
        char *id = id_env ? strdup(id_env) : NULL;
        char *path = path_env ? strdup(path_env) : NULL;

        if (id && path && wb_steam_identity_valid(id, path)) {
            setenv("SteamAppPath", path, 1);
            setenv("SteamGameId", id, 1);
            setenv("SteamAppId", id, 1);
            wb_log(TAG, "steam: app %s at %s", id, path);
        } else {
            /* Never a fixed or default identity. */
            for (int i = 0; i < 3; i++) unsetenv(vars[i]);
            if (id_env || path_env) wb_log(TAG, "steam: the identity given is not valid, none set");
        }
        free(id);
        free(path);
    }
    unsetenv("MADEIRA_STEAM_APPID");
    unsetenv("MADEIRA_STEAM_APPPATH");
}

static void set_jit_offset(void)
{
    int64_t offset = fex_get_jit_write_offset();
    char buf[32];

    if (offset != 0) {
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)offset);
        setenv("MADEIRA_JIT_WRITE_OFFSET", buf, 1);
        wb_log(TAG, "JIT write offset %s", buf);
    } else if (!wb_env_is("WINE_IOS_NOJIT", "1")) {
        wb_log(TAG, "warning: no JIT write offset, and WINE_IOS_NOJIT is not 1");
    }
}

static void set_ca_bundle(const char *bundle)
{
    char path[PATH_MAX];

    if (wb_join(path, sizeof(path), "%s/cacert.pem", bundle) == 0 && wb_exists(path)) {
        setenv("MADEIRA_CA_BUNDLE", path, 1);
    } else {
        wb_log(TAG, "no cacert.pem in the bundle: HTTPS certificate checks in Windows programs will fail");
    }
}

static int sysctl_int(const char *name, int *value)
{
    int v = 0;
    size_t len = sizeof(v);

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return 0;
    *value = v;
    return 1;
}

static const char *probe_text(const struct wb_target *t)
{
    switch (t->probe) {
    case WB_PROBE_NO_32BIT_SET: return "no 32-bit set in the bundle, not probed";
    case WB_PROBE_BUNDLE64:     return "found in a 64-bit set, not probed";
    case WB_PROBE_FILE:         return "probed";
    case WB_PROBE_OTHER_FORM:   return "path form not probed";
    }
    return "?";
}

static void set_working_folder(const char *prefix, const char *exe)
{
    const char *wd_env = getenv("MADEIRA_WORKDIR");
    char *wd = wd_env ? strdup(wd_env) : NULL;
    char win[1024], unix_path[PATH_MAX], initial[1040];
    int have;

    unsetenv("MADEIRA_WORKDIR");   /* one launch only */
    have = wd && wb_workdir_choose(wd, NULL, win, sizeof(win));
    if (wd && !have) wb_log(TAG, "workdir: MADEIRA_WORKDIR \"%s\" not usable, ignored", wd);
    if (!have) have = wb_workdir_choose(NULL, exe, win, sizeof(win));
    free(wd);
    if (!have) {
        /* A bare system program: the unix folder stays, and MADEIRA_INITIAL_CWD belongs to
         * one launch (clarification 5). */
        unsetenv("MADEIRA_INITIAL_CWD");
        return;
    }

    if (wb_workdir_unix(prefix, win, unix_path, sizeof(unix_path)) != 0 ||
        wb_workdir_initial_cwd(win, initial, sizeof(initial)) != 0) {
        wb_log(TAG, "workdir: %s too long", win);
        unsetenv("MADEIRA_INITIAL_CWD");
        return;
    }
    if (chdir(unix_path) == 0) {
        setenv("PWD", unix_path, 1);
        wb_log(TAG, "workdir: %s (%s)", initial, unix_path);
    } else {
        wb_log(TAG, "workdir: chdir %s failed (errno %d)", unix_path, errno);
    }
    setenv("MADEIRA_INITIAL_CWD", initial, 1);
}

static double seconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void wait_for_children(void)
{
    char names[256];
    int n;

    names[0] = 0;
    n = madeira_live_game_children(names, (int)sizeof(names), 60.0);
    names[sizeof(names) - 1] = 0;
    if (n <= 0) return;

    if (wb_env_is("MADEIRA_WAIT_CHILDREN", "1")) {
        struct timespec start;
        double next_report = 60.0;

        clock_gettime(CLOCK_MONOTONIC, &start);
        wb_log(TAG, "program ended, waiting for %d child process(es): %s", n, names);
        for (;;) {
            n = madeira_live_game_children(names, (int)sizeof(names), -1.0);
            names[sizeof(names) - 1] = 0;
            if (n <= 0) break;
            usleep(200 * 1000);
            if (seconds_since(&start) >= next_report) {
                wb_log(TAG, "still waiting after %.0f s for %d child process(es): %s",
                       seconds_since(&start), n, names);
                next_report += 60.0;
            }
        }
        wb_log(TAG, "child processes ended after %.1f s", seconds_since(&start));
    } else {
        wb_log(TAG, "%d child process(es) still running (%s); the session ends anyway", n, names);
    }
}

/* Step 15, in its own frame so nothing it uses is live across the longjmp. */
static __attribute__((noinline)) void run_wine(int argc, char **argv)
{
    volatile int returned = 0;

    if (setjmp(wine_ios_exit_jmpbuf) == 0) {
        __wine_main(argc, argv);
        returned = 1;
    }
    wine_ios_exit_initialized = 0;   /* the jump buffer's frame ends here */
    if (returned) wb_log(TAG, "__wine_main returned");
    else wb_log(TAG, "program exited with code %d (0x%08x)", wine_ios_exit_code, (unsigned int)wine_ios_exit_code);
}

/* Step 19: Wine and the x86 translator keep the Windows thread block in thread-specific-data
 * slot 275 without a pthread key; pthread would otherwise call a destructor on it. */
static void clear_tsd_slot_275(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    uintptr_t base;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(base));
    base &= ~(uintptr_t)7;
    ((void *volatile *)base)[275] = NULL;
#endif
}

/* argv of the current launch. Other Wine threads may still read it for a moment after the
 * program ends, so it lives until the next launch replaces it. */
static struct wb_target g_target;
static char *g_args[WB_MAX_ARGS];
static int g_nargs;
static char *g_argv[2 + WB_MAX_ARGS + 1];

/* Steps 12 to 15. MADEIRA_EXE was checked by the caller. */
static void run_program(const char *prefix, const char *bundle, const char *exe)
{
    struct wb_target *const t = &g_target;
    static char arg0[] = "wine";
    char hostprobe[256];
    int dropped, argc;

    wb_free_args(g_args, g_nargs);
    g_nargs = 0;

    /* 12. Target, DLL set, farms, argv. */
    if (wb_choose_target(exe, bundle, prefix, wb_env_is("MADEIRA_USE_ARM64EC", "1"), t) != 0) {
        wb_log(TAG, "program path too long: %s", exe);
        return;   /* step 17 unsets MADEIRA_WORKDIR */
    }
    if (t->probe == WB_PROBE_FILE)
        wb_log(TAG, "target %s: machine %s0x%04x (%s %s), DLL set %s", exe,
               t->machine < 0 ? "unknown " : "", t->machine < 0 ? 0 : t->machine,
               probe_text(t), t->probed, wb_set_dir(t->set));
    else
        wb_log(TAG, "target %s: %s, DLL set %s", exe, probe_text(t), wb_set_dir(t->set));
    if (t->is32) wb_log(TAG, "target is a 32-bit program");

    wb_link_dll_farms(prefix, bundle, t->set);

    g_nargs = wb_split_args(getenv("MADEIRA_ARGS"), g_args, WB_MAX_ARGS, &dropped);
    if (dropped) wb_log(TAG, "%d argument(s) past the first %d dropped", dropped, WB_MAX_ARGS);
    argc = 0;
    g_argv[argc++] = arg0;
    g_argv[argc++] = t->winpath;
    for (int i = 0; i < g_nargs; i++) g_argv[argc++] = g_args[i];
    g_argv[argc] = NULL;
    for (int i = 0; i < argc; i++) wb_log(TAG, "argv[%d] = %s", i, g_argv[i]);

    /* 13. */
    set_working_folder(prefix, exe);

    /* 14. */
    wine_ios_main_thread = pthread_self();
    wine_ios_exit_initialized = 1;
    ios_main_image_i386 = t->is32 ? 1 : 0;
    wb_hostprobe_format(hostprobe, sizeof(hostprobe), sysctl_int);
    setenv("FEX_MADEIRA_HOSTPROBE", hostprobe, 1);
    wb_log(TAG, "FEX_MADEIRA_HOSTPROBE=%s", hostprobe);

    /* 15. */
    run_wine(argc, g_argv);
}

static void *launch_main(void *arg)
{
    char *prefix = arg;
    const char *bundle = wb_bundle_dir();
    const char *exe_env;
    struct wb_cfg cfg;

    /* 1. */
    wb_seed_prefix(prefix);

    /* 2. */
    setenv("WINEPREFIX", prefix, 1);
    setenv("HOME", prefix, 1);
    setenv("WINELOADERNOEXEC", "1", 1);
    setenv("WINEDLLPATH", bundle, 1);

    /* 3. */
    set_debug_channels();

    /* 4. FNA games default to OpenGL, which doesn't exist here; Mono's default is chatty. */
    setenv("MADEIRA_WIN32U", "1", 1);
    setenv("MADEIRA_QUIET", "1", 1);
    setenv("FNA3D_FORCE_DRIVER", "D3D11", 0);
    setenv("MONO_LOG_LEVEL", "warning", 0);

    /* 5. */
    wb_audio_activate();

    /* 6. */
    set_steam_identity();

    /* 7. */
    set_jit_offset();

    /* 8. */
    wine_log_set_file(wb_log_path());
    winios_session_reset();
    setenv("MADEIRA_DOCS_DIR", wb_documents_dir(), 1);

    /* 9. */
    if (wb_cfg_load(&cfg) < 0) wb_log(TAG, "settings file could not be read");
    wb_cfg_export_env(&cfg);
    wb_cfg_apply_sync_default(&cfg);
    wb_cfg_free(&cfg);

    /* 10. */
    set_ca_bundle(bundle);

    /* 11. */
    if (wb_log_redirect_fd(STDOUT_FILENO) != 0 || wb_log_redirect_fd(STDERR_FILENO) != 0)
        wb_log(TAG, "cannot point stdout/stderr at the log (errno %d)", errno);

    /* 12-15. */
    exe_env = getenv("MADEIRA_EXE");
    if (!exe_env || !*exe_env) {
        wb_log(TAG, "error: MADEIRA_EXE is not set, nothing to run");
    } else {
        char *exe = strdup(exe_env);
        if (exe) run_program(prefix, bundle, exe);
        free(exe);
    }

    /* 16. */
    wait_for_children();

    /* 17. The per-launch settings, MADEIRA_INITIAL_CWD, and (clarification 22) the JIT offset,
     *     the server socket number and MADEIRA_WORKDIR, whatever happened above.
     *     MADEIRA_CA_BUNDLE stays: it never changes in one app run. */
    wb_unset_per_launch_keys();
    unsetenv("MADEIRA_INITIAL_CWD");
    unsetenv("MADEIRA_JIT_WRITE_OFFSET");
    unsetenv("WINESERVERSOCKET");
    unsetenv("MADEIRA_WORKDIR");

    /* 18. Stop the server, then mark not running: the flag drops only after the server
     *     thread is joined, so the next session can't start under a stopping server. */
    wb_server_stop();
    atomic_store(&g_running, 0);
    wb_log(TAG, "launch finished");
    free(prefix);

    /* 19. */
    clear_tsd_slot_275();
    return NULL;
}

int wb_process_start(const char *prefix)
{
    pthread_attr_t attr;
    pthread_t thread;
    char num[16];
    char *copy;
    int pair[2];
    int rc;

    if (!prefix || !*prefix) {
        wb_log(TAG, "start: no prefix");
        unsetenv("MADEIRA_WORKDIR");
        return -1;
    }

    /* 1. */
    pthread_mutex_lock(&g_lock);
    if (atomic_load(&g_running)) {
        pthread_mutex_unlock(&g_lock);
        wb_log(TAG, "start: a program already runs");
        return 0;
    }
    copy = strdup(prefix);
    if (!copy) {
        unsetenv("MADEIRA_WORKDIR");   /* clarification 22: on every outcome */
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    atomic_store(&g_running, 1);
    if (!wb_server_is_ready()) wb_log(TAG, "start: warning, the wineserver is not ready yet");

    /* 2. Unix-domain accept() doesn't work for the in-app server: connect directly. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        wb_log(TAG, "start: socketpair failed (errno %d)", errno);
        unsetenv("MADEIRA_WORKDIR");
        atomic_store(&g_running, 0);
        pthread_mutex_unlock(&g_lock);
        free(copy);
        return -1;
    }
    wineserver_inject_client_fd(pair[0]);
    snprintf(num, sizeof(num), "%d", pair[1]);
    setenv("WINESERVERSOCKET", num, 1);

    /* 3. The QoS goes through the attribute: a thread created with an explicit priority
     *    can't raise its own QoS later. */
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&thread, &attr, launch_main, copy);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        /* Clarification 19: the server owns pair[0] since the inject; only the client end
         * is ours to close. */
        wb_log(TAG, "start: pthread_create failed (%d)", rc);
        close(pair[1]);
        unsetenv("WINESERVERSOCKET");
        unsetenv("MADEIRA_WORKDIR");
        atomic_store(&g_running, 0);
        pthread_mutex_unlock(&g_lock);
        free(copy);
        return -1;
    }
    pthread_mutex_unlock(&g_lock);
    wb_log(TAG, "launch thread started (server socket fd %d)", pair[1]);
    return 0;
}
