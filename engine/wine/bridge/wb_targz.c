// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part H "The tar.gz reader": extracts the bundled prefix template. Written from the
// POSIX ustar header layout (IEEE Std 1003.1, pax "ustar Interchange Format") and zlib's
// gzopen/gzread documentation. Only directories and regular files are created; every other
// entry type is skipped with its data.

#include "wb_internal.h"
#include "wb_log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define TAG "wb-prefix"
#define TAR_BLOCK 512

/* ustar header fields (offset, length). */
#define H_NAME      0
#define H_NAME_LEN  100
#define H_SIZE      124
#define H_SIZE_LEN  12
#define H_TYPE      156
#define H_MAGIC     257
#define H_PREFIX    345
#define H_PREFIX_LEN 155

static int read_block(gzFile gz, unsigned char *block)
{
    return gzread(gz, block, TAR_BLOCK) == TAR_BLOCK ? 0 : -1;
}

static int all_zero(const unsigned char *block)
{
    for (int i = 0; i < TAR_BLOCK; i++)
        if (block[i]) return 0;
    return 1;
}

/* Octal number; spaces and NULs are ignored, any other non-octal byte is an error. */
static int parse_octal(const unsigned char *field, size_t len, uint64_t *out)
{
    uint64_t v = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = field[i];
        if (c == ' ' || c == 0) continue;
        if (c < '0' || c > '7') return -1;
        if (v > (UINT64_MAX >> 3)) return -1;
        v = (v << 3) | (uint64_t)(c - '0');
    }
    *out = v;
    return 0;
}

/* The entry name: prefix "/" name for POSIX ustar headers with a prefix, else the name. */
static void entry_name(const unsigned char *h, char *out /* >= 257 bytes */)
{
    size_t nl = strnlen((const char *)h + H_NAME, H_NAME_LEN);

    if (memcmp(h + H_MAGIC, "ustar\0", 6) == 0 && h[H_PREFIX]) {
        size_t pl = strnlen((const char *)h + H_PREFIX, H_PREFIX_LEN);
        memcpy(out, h + H_PREFIX, pl);
        out[pl] = '/';
        memcpy(out + pl + 1, h + H_NAME, nl);
        out[pl + 1 + nl] = 0;
    } else {
        memcpy(out, h + H_NAME, nl);
        out[nl] = 0;
    }
}

static int has_dotdot_component(const char *s)
{
    const char *p = s;

    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* The name relative to the template's top folder ("prefix/..."). */
static const char *strip_top(const char *name)
{
    while (name[0] == '.' && name[1] == '/') name += 2;
    if (strncmp(name, "prefix/", 7) == 0) return name + 7;
    if (strcmp(name, "prefix") == 0) return name + 6;
    return name;
}

static int skip_blocks(gzFile gz, uint64_t size)
{
    unsigned char block[TAR_BLOCK];
    uint64_t blocks = (size + TAR_BLOCK - 1) / TAR_BLOCK;

    while (blocks-- > 0)
        if (read_block(gz, block) != 0) return -1;
    return 0;
}

static int make_parents(const char *path)
{
    char dir[PATH_MAX];
    char *slash;

    if (wb_join(dir, sizeof(dir), "%s", path) != 0) return -1;
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) return 0;
    *slash = 0;
    return wb_mkdirs(dir, 0755);
}

/* Write `size` bytes of entry data to `path`. Returns -1 only for archive read errors;
 * *write_failed reports a file that could not be written (its data is still consumed). */
static int extract_file(gzFile gz, const char *path, uint64_t size, int *write_failed)
{
    unsigned char block[TAR_BLOCK];
    uint64_t remaining = size;
    struct stat st;
    int fd = -1;

    *write_failed = 0;
    if (make_parents(path) != 0) {
        wb_log(TAG, "tar: cannot create the folders of %s (errno %d)", path, errno);
        *write_failed = 1;
    } else {
        if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) {
            wb_log(TAG, "tar: cannot write %s (errno %d)", path, errno);
            *write_failed = 1;
        }
    }

    while (remaining > 0) {
        size_t chunk = remaining < TAR_BLOCK ? (size_t)remaining : TAR_BLOCK;
        const unsigned char *p = block;
        size_t left = chunk;

        if (read_block(gz, block) != 0) {
            if (fd >= 0) close(fd);
            return -1;
        }
        while (fd >= 0 && left > 0) {
            ssize_t w = write(fd, p, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                wb_log(TAG, "tar: write to %s failed (errno %d)", path, errno);
                close(fd);
                fd = -1;
                *write_failed = 1;
                break;
            }
            p += w;
            left -= (size_t)w;
        }
        remaining -= chunk;
    }
    if (fd >= 0) {
        fchmod(fd, 0644);
        if (close(fd) != 0) *write_failed = 1;
    }
    return 0;
}

int wb_targz_extract(const char *archive, const char *dest)
{
    unsigned char h[TAR_BLOCK];
    char name[H_PREFIX_LEN + 1 + H_NAME_LEN + 1];
    char path[PATH_MAX];
    int files = 0, dirs = 0, skipped = 0, failures = 0;
    int result = 0;
    gzFile gz;

    gz = gzopen(archive, "rb");
    if (!gz) {
        wb_log(TAG, "tar: cannot open %s (errno %d)", archive, errno);
        return -1;
    }
    if (wb_mkdirs(dest, 0755) != 0) {
        wb_log(TAG, "tar: cannot create %s (errno %d)", dest, errno);
        gzclose(gz);
        return -1;
    }

    for (;;) {
        uint64_t size;
        const char *rel;
        char type;

        if (read_block(gz, h) != 0) {
            wb_log(TAG, "tar: short read in %s", archive);
            result = -1;
            break;
        }
        if (all_zero(h)) break;   /* end of archive */

        entry_name(h, name);
        if (parse_octal(h + H_SIZE, H_SIZE_LEN, &size) != 0) {
            wb_log(TAG, "tar: bad size field for \"%s\"", name);
            result = -1;
            break;
        }
        type = (char)h[H_TYPE];

        rel = strip_top(name);
        if (name[0] == '/' || rel[0] == '/' || has_dotdot_component(name)) {
            wb_log(TAG, "tar: refused unsafe name \"%s\"", name);
            skipped++;
            if (skip_blocks(gz, size) != 0) { result = -1; break; }
            continue;
        }

        size_t rl = strlen(rel);
        int is_dir = type == '5' || (type == 0 && rl > 0 && rel[rl - 1] == '/');
        int is_file = !is_dir && (type == '0' || type == 0);

        if (rl == 0) {
            /* The top folder itself: dest already exists. */
            if (skip_blocks(gz, size) != 0) { result = -1; break; }
            continue;
        }
        if (wb_join(path, sizeof(path), "%s/%s", dest, rel) != 0) {
            wb_log(TAG, "tar: name too long: \"%s\"", name);
            failures++;
            if (skip_blocks(gz, size) != 0) { result = -1; break; }
            continue;
        }

        if (is_dir) {
            if (wb_mkdirs(path, 0755) == 0) dirs++;
            else {
                wb_log(TAG, "tar: cannot create folder %s (errno %d)", path, errno);
                failures++;
            }
            if (skip_blocks(gz, size) != 0) { result = -1; break; }
        } else if (is_file) {
            int write_failed;
            if (extract_file(gz, path, size, &write_failed) != 0) {
                wb_log(TAG, "tar: short read in the data of \"%s\"", name);
                result = -1;
                break;
            }
            if (write_failed) failures++;
            else files++;
        } else {
            /* x, g, L, K, links and everything else: data skipped. */
            skipped++;
            if (skip_blocks(gz, size) != 0) { result = -1; break; }
        }
    }

    gzclose(gz);
    wb_log(TAG, "tar: %s: %d files, %d folders, %d entries skipped, %d failures%s", archive, files,
           dirs, skipped, failures, result ? " (archive error)" : "");
    if (failures) result = -1;
    return result;
}
