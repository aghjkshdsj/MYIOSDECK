// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part A: the log file <Documents>/wine-log.txt.

#include "wb_log.h"
#include "wb_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t g_path_once = PTHREAD_ONCE_INIT;
static char g_path[1024];

static void init_path(void)
{
    const char *docs = wb_documents_dir();
    if (!docs || !*docs) docs = ".";
    snprintf(g_path, sizeof(g_path), "%s/wine-log.txt", docs);
}

const char *wb_log_path(void)
{
    pthread_once(&g_path_once, init_path);
    return g_path;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;

static pthread_once_t g_oslog_once = PTHREAD_ONCE_INIT;
static os_log_t g_oslog;

static void init_oslog(void)
{
    g_oslog = os_log_create("com.myiosdeck.wine", "bridge");
}

static int open_log(void)
{
    return open(wb_log_path(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
}

/* With g_lock held. The app deletes the file before each boot, so a descriptor whose file
 * has no name any more is replaced by a fresh one. */
static void ensure_fd_locked(void)
{
    struct stat st;

    if (g_fd >= 0 && fstat(g_fd, &st) == 0 && st.st_nlink > 0) return;
    if (g_fd >= 0) close(g_fd);
    g_fd = open_log();
}

static void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

void wb_logv(const char *tag, const char *fmt, va_list ap)
{
    int saved_errno = errno;
    char msg[2048];
    char line[2200];
    char ui[2100];
    struct timeval tv;
    struct tm tm;
    time_t secs;
    int n;

    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (!tag) tag = "wb";

    gettimeofday(&tv, NULL);
    secs = tv.tv_sec;
    localtime_r(&secs, &tm);
    n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\n", tm.tm_hour, tm.tm_min,
                 tm.tm_sec, (int)(tv.tv_usec / 1000), tag, msg);
    if (n < 0) {
        errno = saved_errno;
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
        line[n - 1] = '\n';
    }

    pthread_mutex_lock(&g_lock);
    ensure_fd_locked();
    if (g_fd >= 0) write_all(g_fd, line, (size_t)n);
    pthread_mutex_unlock(&g_lock);

    /* Outside the lock: neither may call back into this file while it is held. */
    pthread_once(&g_oslog_once, init_oslog);
    os_log_with_type(g_oslog, OS_LOG_TYPE_DEFAULT, "[%{public}s] %{public}s", tag, msg);
    snprintf(ui, sizeof(ui), "[%s] %s", tag, msg);
    wine_ui_log(ui);

    errno = saved_errno;
}

void wb_log(const char *tag, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    wb_logv(tag, fmt, ap);
    va_end(ap);
}

int wb_log_redirect_fd(int fd)
{
    int tmp;

    if (fd == STDOUT_FILENO) fflush(stdout);
    if (fd == STDERR_FILENO) fflush(stderr);
    tmp = open_log();
    if (tmp < 0) return -1;
    if (dup2(tmp, fd) < 0) {
        close(tmp);
        return -1;
    }
    close(tmp);
    if (fd == STDOUT_FILENO) setvbuf(stdout, NULL, _IOLBF, 0);
    return 0;
}
