// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK's embedding of Blink (github.com/jart/blink, ISC): runs a static
// x86-64 Linux ELF in Blink's interpreter, inside the app, without JIT.
//
// Blink is built as a command-line program. This replaces its main()
// (blink/blink.c) with a driver that:
//   - asks Blink to trap guest exit_group() instead of exiting the process,
//   - turns Blink's fatal paths (exit(), TerminateSignal) into a longjmp back
//     here, so a crashing guest can never take the app down with it,
//   - uses software page tables (no linear memory), so Blink installs no
//     SIGSEGV handlers that would fight FEX or the JIT trap handler,
//   - captures the guest's stdout/stderr for the UI.

#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blink/bus.h"
#include "blink/flag.h"
#include "blink/loader.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/map.h"
#include "blink/signal.h"
#include "blink/syscall.h"

#include "interp_engine.h"

#define BAIL_EXIT 0x10000
#define BAIL_SIGNAL 0x20000

static sigjmp_buf g_bail;
static volatile int g_armed;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_inited;

// Blink's own exit() calls (see ios_overrides.h).
void mid_blink_exit(int status) {
    if (g_armed) siglongjmp(g_bail, BAIL_EXIT | (status & 0xff));
    abort();
}

// Normally in blink.c: a guest died of a signal it did not handle.
void TerminateSignal(struct Machine *m, int sig, int code) {
    (void)m;
    (void)code;
    if (g_armed) siglongjmp(g_bail, BAIL_SIGNAL | (sig & 0xff));
    abort();
}

bool mid_interp_linked(void) { return true; }
const char *mid_interp_version(void) { return MYIOSDECK_BLINK_SHA; }

struct RunArgs {
    const char *path;
    int argc;
    const char *const *argv;
    mid_run_result *out;
};

static void *RunThread(void *p) {
    struct RunArgs *a = p;
    mid_run_result *out = a->out;
    char *argv[33];
    int argc = a->argc < 32 ? a->argc : 32;
    for (int i = 0; i < argc; i++) argv[i] = strdup(a->argv[i]);
    argv[argc] = NULL;
    char *envp[] = { "PATH=/bin", "HOME=/", NULL };

    struct Machine *m = NewMachine(NewSystem(XED_MACHINE_MODE_LONG), 0);
    g_machine = m;
    m->system->trapexit = true;

    uint64_t t0 = mach_absolute_time();
    g_armed = 1;
    int bail = sigsetjmp(g_bail, 1);
    if (!bail) {
        LoadProgram(m, (char *)a->path, (char *)a->path, argv, envp, NULL);
        for (int i = 0; i < 3; i++) AddStdFd(&m->system->fds, i);
        for (;;) {
            int rc = sigsetjmp(m->onhalt, 1);
            if (!rc) {
                m->canhalt = true;
                Actor(m);
            }
            // Same bookkeeping as Blink(): a halt is a guest exception boundary.
            m->sysdepth = 0;
            m->sigdepth = 0;
            m->canhalt = false;
            m->nofault = false;
            m->insyscall = false;
            CollectPageLocks(m);
            CollectGarbage(m, 0);
            if (IsMakingPath(m)) AbandonPath(m);
            if (rc == kMachineExitTrap) {
                out->exit_code = m->system->exitcode;
                out->ok = true;
                break;
            }
        }
    } else if (bail & BAIL_SIGNAL) {
        snprintf(out->error, sizeof out->error, "guest terminated by Linux signal %d", bail & 0xff);
    } else {
        snprintf(out->error, sizeof out->error, "interpreter stopped (status %d)", bail & 0xff);
    }
    g_armed = 0;
    uint64_t t1 = mach_absolute_time();

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    out->seconds = (double)(t1 - t0) * tb.numer / tb.denom / 1e9;
    // A machine that bailed out mid-instruction may hold locks; leak it rather
    // than risk freeing inconsistent state.
    if (out->ok) FreeMachine(m);
    g_machine = NULL;
    for (int i = 0; i < argc; i++) free(argv[i]);
    return NULL;
}

bool mid_interp_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_lock);
    if (!g_inited) {
        WriteErrorInit();
        InitMap();
        InitBus();
        FLAG_nolinear = true;
        g_inited = true;
    }

    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    char path[1024], outpath[1024];
    snprintf(path, sizeof path, "%s/myiosdeck-guest.elf", tmp);
    snprintf(outpath, sizeof outpath, "%s/myiosdeck-guest.out", tmp);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0 || write(fd, elf, len) != (ssize_t)len) {
        snprintf(out->error, sizeof out->error, "could not stage the program: %s", strerror(errno));
        if (fd >= 0) close(fd);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    close(fd);

    // The guest's fds 1/2 are the host's: point them at a capture file for the run.
    int cap = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    int saved1 = dup(1), saved2 = dup(2);
    fflush(stdout);
    fflush(stderr);
    if (cap >= 0) {
        dup2(cap, 1);
        dup2(cap, 2);
    }

    struct RunArgs args = { path, argc, argv, out };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t th;
    if (pthread_create(&th, &attr, RunThread, &args) == 0) {
        pthread_join(th, NULL);
    } else {
        snprintf(out->error, sizeof out->error, "pthread_create failed");
    }
    pthread_attr_destroy(&attr);

    fflush(stdout);
    fflush(stderr);
    dup2(saved1, 1);
    dup2(saved2, 2);
    close(saved1);
    close(saved2);
    if (cap >= 0) {
        lseek(cap, 0, SEEK_SET);
        ssize_t n = read(cap, out->output, sizeof out->output - 1);
        out->output_len = n > 0 ? (size_t)n : 0;
        out->output[out->output_len] = 0;
        close(cap);
    }
    pthread_mutex_unlock(&g_lock);
    return out->ok;
}
