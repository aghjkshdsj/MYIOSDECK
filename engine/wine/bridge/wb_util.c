// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Small file-system helpers shared by the spec 02 sources.

#include "wb_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int wb_join(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int wb_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int wb_lexists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int wb_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int wb_is_real_dir(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int wb_is_symlink(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

int wb_mkdirs(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    size_t len = strlen(path);
    char *p;

    if (len == 0 || len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;

    for (p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, mode) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
    if (!wb_is_dir(buf)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int wb_symlink_replace(const char *target, const char *linkpath)
{
    struct stat st;
    char cur[PATH_MAX];
    ssize_t n;

    if (lstat(linkpath, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            errno = EISDIR;
            return -1;
        }
        if (S_ISLNK(st.st_mode)) {
            /* Already the right link: nothing to heal. */
            n = readlink(linkpath, cur, sizeof(cur) - 1);
            if (n >= 0) {
                cur[n] = 0;
                if (strcmp(cur, target) == 0) return 0;
            }
        }
        if (unlink(linkpath) != 0) return -1;
    }
    return symlink(target, linkpath);
}

int wb_c_drive(const char *s)
{
    return s && (s[0] == 'C' || s[0] == 'c') && s[1] == ':' && s[2] == '\\';
}

int wb_env_is(const char *name, const char *value)
{
    const char *v = getenv(name);
    return v && strcmp(v, value) == 0;
}

int wb_write_file(const char *path, const void *data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    const char *p = data;
    int fd;

    if (wb_join(tmp, sizeof(tmp), "%s.tmp", path) != 0) return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    fchmod(fd, mode);
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
