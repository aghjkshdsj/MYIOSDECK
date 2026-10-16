// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part B: the wineserver as a thread of the app.

#include "wb_internal.h"
#include "wb_log.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "wb-server"

volatile int g_wineserver_should_stop = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_have_thread;      /* a joinable thread not yet joined */
static atomic_int g_running;
static char *g_prefix;         /* the server thread's copy; replaced only while not running */

static void *server_main(void *arg)
{
    const char *prefix = arg;
    char nls[PATH_MAX];
    static char arg0[] = "wineserver";
    static char arg1[] = "--foreground";
    char *argv[] = { arg0, arg1, NULL };
    int rc;

    /* 1. The log, for this thread's stderr and the server's own C code. */
    if (wb_log_redirect_fd(STDERR_FILENO) != 0)
        wb_log(TAG, "cannot point stderr at the log (errno %d)", errno);
    wineserver_log_set_file(wb_log_path());

    /* 2. Environment and the prefix directory. */
    setenv("WINEPREFIX", prefix, 1);
    setenv("HOME", prefix, 1);
    if (mkdir(prefix, 0755) != 0 && errno != EEXIST)
        wb_log(TAG, "mkdir %s failed (errno %d)", prefix, errno);

    /* 3. NLS tables. */
    if (wb_join(nls, sizeof(nls), "%s/nls", wb_bundle_dir()) == 0) wineserver_set_nls_dir(nls);
    else wb_log(TAG, "bundle path too long for the NLS folder");

    /* 4. The server loop. */
    foreground = 1;
    wb_log(TAG, "wineserver starting (prefix %s)", prefix);
    rc = wineserver_main(2, argv);

    /* 5. */
    wb_log(TAG, "wineserver_main returned %d", rc);
    atomic_store(&g_running, 0);
    return NULL;
}

int wb_server_start(const char *prefix)
{
    pthread_attr_t attr;
    pthread_t thread;
    char *copy;
    int rc;

    if (!prefix || !*prefix) {
        wb_log(TAG, "start: no prefix");
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    if (atomic_load(&g_running)) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    /* A server that ended on its own (or through fatal_error) is still joinable. */
    if (g_have_thread) {
        g_have_thread = 0;
        pthread_join(g_thread, NULL);
    }

    copy = strdup(prefix);
    if (!copy) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    free(g_prefix);
    g_prefix = copy;

    /* Before the thread: at start-up the server loads system.reg, and without it builds an
     * empty registry that it later saves over the template's. */
    wb_seed_prefix(g_prefix);

    g_wineserver_should_stop = 0;   /* a previous wb_server_stop left it at 1 */
    __atomic_store_n(&wineserver_ready, 0, __ATOMIC_RELEASE);

    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    atomic_store(&g_running, 1);
    rc = pthread_create(&thread, &attr, server_main, g_prefix);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_store(&g_running, 0);
        pthread_mutex_unlock(&g_lock);
        wb_log(TAG, "start: pthread_create failed (%d)", rc);
        return -1;
    }
    g_thread = thread;
    g_have_thread = 1;
    pthread_mutex_unlock(&g_lock);
    wb_log(TAG, "server thread started");
    return 0;
}

int wb_server_is_running(void)
{
    return atomic_load(&g_running);
}

int wb_server_is_ready(void)
{
    return __atomic_load_n(&wineserver_ready, __ATOMIC_ACQUIRE);
}

void wb_server_stop(void)
{
    pthread_t thread;
    int have;

    pthread_mutex_lock(&g_lock);
    have = g_have_thread;
    thread = g_thread;
    if (have && pthread_equal(thread, pthread_self())) have = 0;   /* never join ourselves */
    else g_have_thread = 0;
    pthread_mutex_unlock(&g_lock);

    g_wineserver_should_stop = 1;
    wb_log(TAG, "stopping the wineserver%s", have ? ", joining its thread" : "");
    if (have) {
        pthread_join(thread, NULL);
        wb_log(TAG, "wineserver thread joined");
    }
    atomic_store(&g_running, 0);
}

void fatal_error(const char *err, ...)
{
    char msg[1024];
    va_list ap;
    size_t n;

    va_start(ap, err);
    vsnprintf(msg, sizeof(msg), err ? err : "(null)", ap);
    va_end(ap);
    n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = 0;
    wb_log(TAG, "FATAL: %s", msg);

    pthread_mutex_lock(&g_lock);
    if (g_have_thread && pthread_equal(g_thread, pthread_self())) atomic_store(&g_running, 0);
    pthread_mutex_unlock(&g_lock);

    /* End only this thread: exit() would end the whole app. */
    pthread_exit(NULL);
}
