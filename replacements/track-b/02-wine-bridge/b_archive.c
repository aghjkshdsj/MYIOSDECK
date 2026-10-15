// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_logic.h"
#include <zlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

extern void wb_b_log(const char *format, ...);

static int wb_b_tar_size(const unsigned char *p, uint64_t *out) {
    uint64_t n = 0;
    for (int i = 0; i < 12; ++i) {
        if (!p[i] || p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7' || n > (UINT64_MAX - 7) / 8) return -1;
        n = n * 8 + (p[i] - '0');
    }
    *out = n;
    return 0;
}

static int wb_b_safe_tar_name(char *name) {
    if (!*name || name[0] == '/' || name[0] == '\\') return 0;
    for (char *p = name; *p; ++p) if (*p == '\\' || *p == ':') return 0;
    const char *p = name;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!end) break;
        p = end + 1;
    }
    return 1;
}

// Open every parent relative to the verified destination descriptor. Existing
// symlinks cannot turn a safe archive name into a write outside the prefix.
static int wb_b_tar_parent(int root, const char *name, int directory, int *file_fd) {
    char *copy = strdup(name);
    if (!copy) return -1;
    size_t n = strlen(copy);
    while (n && copy[n - 1] == '/') copy[--n] = 0;
    int parent = dup(root), result = -1;
    if (parent < 0) { free(copy); return -1; }
    char *save = NULL;
    char *component = strtok_r(copy, "/", &save);
    while (component) {
        char *next = strtok_r(NULL, "/", &save);
        if (!strcmp(component, ".")) { component = next; continue; }
        if (!next && !directory) {
            *file_fd = openat(parent, component, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
            if (*file_fd >= 0) {
                struct stat st;
                if (fstat(*file_fd, &st) || !S_ISREG(st.st_mode) || fchmod(*file_fd, 0644)) {
                    close(*file_fd); *file_fd = -1;
                }
            }
            result = *file_fd < 0 ? -1 : 0;
            goto end;
        }
        if (mkdirat(parent, component, 0755) && errno != EEXIST) goto end;
        int child = openat(parent, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (child < 0) goto end;
        close(parent); parent = child;
        component = next;
    }
    result = directory ? 0 : -1;
end:
    close(parent); free(copy); return result;
}

int wb_b_extract_tar_gz(const char *archive, const char *destination, size_t *files, size_t *dirs) {
    size_t nf = 0, nd = 0;
    int result = -1, output = -1, entryErrors = 0;
    int root = open(destination, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0) return -1;
    gzFile gz = gzopen(archive, "rb");
    if (!gz) { close(root); return -1; }
    unsigned char block[512];
    for (;;) {
        if (gzread(gz, block, 512) != 512) { wb_b_log("[wb-prefix] short tar header"); break; }
        int nonzero = 0;
        for (size_t i = 0; i < sizeof block; ++i) nonzero |= block[i];
        if (!nonzero) { result = entryErrors ? -1 : 0; break; }
        char name[258] = {0};
        size_t nn = strnlen((const char *)block, 100);
        if (!memcmp(block + 257, "ustar\0", 6) && block[345]) {
            size_t pn = strnlen((const char *)block + 345, 155);
            memcpy(name, block + 345, pn); name[pn] = '/'; memcpy(name + pn + 1, block, nn);
        } else memcpy(name, block, nn);
        uint64_t size;
        if (wb_b_tar_size(block + 124, &size)) { wb_b_log("[wb-prefix] invalid tar size"); break; }
        int safe = wb_b_safe_tar_name(name);
        char *relative = name;
        if (!strncmp(relative, "./", 2)) relative += 2;
        if (!strncmp(relative, "prefix/", 7)) relative += 7;
        safe = safe && (!*relative || wb_b_safe_tar_name(relative));
        int type = block[156];
        int directory = type == '5' || (type == 0 && *relative && relative[strlen(relative) - 1] == '/');
        int regular = !directory && (type == 0 || type == '0');
        output = -1;
        if (!safe) wb_b_log("[wb-prefix] rejected tar path: %s", name);
        else if (*relative && (directory || regular)) {
            if (wb_b_tar_parent(root, relative, directory, &output)) {
                wb_b_log("[wb-prefix] cannot create tar entry: %s", relative); entryErrors = 1;
            } else if (directory) ++nd;
        }
        uint64_t remaining = size;
        int failed = 0, fileCreated = output >= 0, writeFailed = 0;
        while (remaining) {
            if (gzread(gz, block, 512) != 512) { failed = 1; break; }
            size_t amount = remaining < 512 ? (size_t)remaining : 512;
            if (output >= 0) {
                size_t written = 0;
                while (written < amount) {
                    ssize_t n = write(output, block + written, amount - written);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { writeFailed = 1; entryErrors = 1; break; }
                    written += (size_t)n;
                }
                if (writeFailed) { close(output); output = -1; }
            }
            remaining -= amount;
            if (failed) break;
        }
        if (output >= 0) { if (close(output)) { writeFailed = 1; entryErrors = 1; } output = -1; }
        if (failed) { wb_b_log("[wb-prefix] short or failed tar payload"); break; }
        if (writeFailed) wb_b_log("[wb-prefix] tar write failed: %s", relative);
        if (fileCreated && !writeFailed) ++nf;
    }
    if (output >= 0) close(output);
    if (gzclose(gz) != Z_OK) result = -1;
    close(root);
    if (files) *files = nf;
    if (dirs) *dirs = nd;
    wb_b_log("[wb-prefix] tar files=%zu directories=%zu result=%d", nf, nd, result);
    return result;
}
