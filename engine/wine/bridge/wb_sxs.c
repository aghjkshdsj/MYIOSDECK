// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part H "x86 side-by-side store": C:\windows\winsxs with the assemblies of
// wb_sxs_data.c, as links into the bundle's i386-windows plus one manifest per assembly.
// Folder names follow the scheme the spec gives (x86_<lower-case name>_<token>_<version>_
// none_deadbeef); the manifest is the assembly manifest schema Microsoft documents
// ("Assembly Manifests", urn:schemas-microsoft-com:asm.v1).

#include "wb_internal.h"
#include "wb_log.h"
#include "wb_sxs.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TAG "wb-prefix"
#define MANIFEST_MAX 8192

static void folder_name(const struct wb_sxs_assembly *a, char *out, size_t cap)
{
    char lname[128];
    size_t o = 0;

    /* Letters, digits, '-' and '.' only, lower-cased. (Every name in the table is shorter
     * than the 40 characters past which upstream shortens names.) */
    for (const char *p = a->name; *p && o < sizeof(lname) - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') lname[o++] = (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.') lname[o++] = c;
    }
    lname[o] = 0;
    snprintf(out, cap, "x86_%s_%s_%s_none_deadbeef", lname, a->token, a->version);
}

struct buf { char data[MANIFEST_MAX]; size_t len; int overflow; };

static void put(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(struct buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(b->data + b->len, sizeof(b->data) - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(b->data) - b->len) b->overflow = 1;
    else b->len += (size_t)n;
}

/* UTF-8, LF line ends, no BOM; lists exactly the files in `linked`. */
static int write_manifest(const char *path, const struct wb_sxs_assembly *a, const int *linked)
{
    struct buf b = { .len = 0, .overflow = 0 };

    put(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    put(&b, "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n");
    put(&b, "  <assemblyIdentity type=\"win32\" name=\"%s\" version=\"%s\" processorArchitecture=\"x86\" "
            "publicKeyToken=\"%s\"/>\n", a->name, a->version, a->token);
    for (int i = 0; i < a->file_count; i++) {
        const struct wb_sxs_file *f = &a->files[i];
        if (!linked[i]) continue;
        if (!f->window_classes) {
            put(&b, "  <file name=\"%s\"/>\n", f->name);
            continue;
        }
        put(&b, "  <file name=\"%s\">\n", f->name);
        for (const char *const *c = f->window_classes; *c; c++)
            put(&b, "    <windowClass>%s</windowClass>\n", *c);
        put(&b, "  </file>\n");
    }
    put(&b, "</assembly>\n");
    if (b.overflow) return -1;
    return wb_write_file(path, b.data, b.len, 0644);
}

/* An assembly not seeded this session loses its manifest and its folder (clarification 13).
 * The folder holds only the links this file makes; sub-folders are left alone. */
static void remove_assembly(const char *mpath, const char *dir)
{
    char p[PATH_MAX];
    struct dirent *de;
    DIR *d;

    unlink(mpath);
    d = opendir(dir);
    if (!d) return;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (wb_join(p, sizeof(p), "%s/%s", dir, de->d_name) == 0 && !wb_is_real_dir(p)) unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

void wb_build_winsxs(const char *prefix, const char *bundle)
{
    char sxs[PATH_MAX], manifests[PATH_MAX], x86dir[PATH_MAX];
    int seeded = 0, skipped = 0;

    if (wb_join(sxs, sizeof(sxs), "%s/drive_c/windows/winsxs", prefix) != 0 ||
        wb_join(manifests, sizeof(manifests), "%s/manifests", sxs) != 0 ||
        wb_join(x86dir, sizeof(x86dir), "%s/i386-windows", bundle) != 0)
        return;
    if (wb_mkdirs(manifests, 0755) != 0) {
        wb_log(TAG, "winsxs: cannot create %s (errno %d)", manifests, errno);
        return;
    }

    for (int i = 0; i < wb_sxs_assembly_count; i++) {
        const struct wb_sxs_assembly *a = &wb_sxs_assemblies[i];
        char fname[256], dir[PATH_MAX], mpath[PATH_MAX], src[PATH_MAX], dst[PATH_MAX];
        int linked[16] = { 0 };

        folder_name(a, fname, sizeof(fname));
        if (a->file_count > (int)(sizeof(linked) / sizeof(linked[0])) ||
            wb_join(dir, sizeof(dir), "%s/%s", sxs, fname) != 0 ||
            wb_join(mpath, sizeof(mpath), "%s/%s.manifest", manifests, fname) != 0 ||
            wb_join(src, sizeof(src), "%s/%s", x86dir, a->files[0].source) != 0) {
            skipped++;
            continue;
        }
        if (!wb_exists(src)) {
            /* A manifest without its DLL breaks loading: drop what an earlier build wrote. */
            remove_assembly(mpath, dir);
            wb_log(TAG, "winsxs: %s %s skipped, no %s in the 32-bit set", a->name, a->version,
                   a->files[0].source);
            skipped++;
            continue;
        }
        if (wb_mkdirs(dir, 0755) != 0) {
            wb_log(TAG, "winsxs: cannot create %s (errno %d)", dir, errno);
            skipped++;
            continue;
        }
        for (int f = 0; f < a->file_count; f++) {
            if (wb_join(src, sizeof(src), "%s/%s", x86dir, a->files[f].source) != 0 ||
                wb_join(dst, sizeof(dst), "%s/%s", dir, a->files[f].name) != 0 || !wb_exists(src))
                continue;
            linked[f] = wb_symlink_replace(src, dst) == 0;
        }
        if (!linked[0] || write_manifest(mpath, a, linked) != 0) {
            int err = errno;
            remove_assembly(mpath, dir);
            wb_log(TAG, "winsxs: %s %s FAILED (errno %d)", a->name, a->version, err);
            skipped++;
            continue;
        }
        seeded++;
    }
    wb_log(TAG, "winsxs: %d assemblies seeded, %d skipped", seeded, skipped);
}
