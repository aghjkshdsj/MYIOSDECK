// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part C, the rules of a launch that don't touch global state, kept apart so they
// can be tested on any host: argument splitting, the PE machine probe, the target and DLL
// set, the working folder, the Steam identity check and the CPU probe string.
// PE layout from Microsoft's "PE Format" documentation (MS-DOS stub e_lfanew at 0x3C,
// "PE\0\0" signature, then IMAGE_FILE_HEADER.Machine).

#include "wb_internal.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- arguments ---- */

int wb_split_args(const char *s, char **argv, int max, int *dropped)
{
    char *buf;
    int n = 0, extra = 0;

    if (dropped) *dropped = 0;
    if (!s) return 0;
    buf = malloc(strlen(s) + 1);
    if (!buf) return 0;

    while (*s) {
        size_t o = 0;
        int quoted = 0;

        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        while (*s && (quoted || (*s != ' ' && *s != '\t'))) {
            if (*s == '"') {
                quoted = !quoted;
                s++;
                continue;
            }
            buf[o++] = *s++;
        }
        buf[o] = 0;
        if (n < max) {
            char *a = strdup(buf);
            if (!a) break;
            argv[n++] = a;
        } else {
            extra++;
        }
    }
    free(buf);
    if (dropped) *dropped = extra;
    return n;
}

void wb_free_args(char **argv, int n)
{
    for (int i = 0; i < n; i++) free(argv[i]);
}

/* ---- PE probe ---- */

int wb_pe_machine(const char *path)
{
    unsigned char dos[64];
    unsigned char pe[6];
    uint32_t lfanew;
    int machine = -1;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (pread(fd, dos, sizeof(dos), 0) == (ssize_t)sizeof(dos) && dos[0] == 'M' && dos[1] == 'Z') {
        lfanew = (uint32_t)dos[0x3C] | (uint32_t)dos[0x3D] << 8 | (uint32_t)dos[0x3E] << 16 |
                 (uint32_t)dos[0x3F] << 24;
        if (lfanew <= 16u * 1024 * 1024 &&
            pread(fd, pe, sizeof(pe), (off_t)lfanew) == (ssize_t)sizeof(pe) && pe[0] == 'P' &&
            pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)
            machine = pe[4] | pe[5] << 8;
    }
    close(fd);
    return machine;
}

/* ---- target and DLL set ---- */

const char *wb_set_dir(int set)
{
    return set == WB_SET_ARM64EC ? "arm64ec-windows" : "aarch64-windows";
}

static int has_drive(const char *s)
{
    return ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':';
}

static void backslashes_to_slashes(char *s)
{
    for (; *s; s++)
        if (*s == '\\') *s = '/';
}

int wb_choose_target(const char *exe, const char *bundle, const char *prefix,
                     int force_arm64ec, struct wb_target *t)
{
    char path[PATH_MAX];
    int backslash = strchr(exe, '\\') != NULL;
    int drive = has_drive(exe);

    memset(t, 0, sizeof(*t));
    t->machine = -1;
    t->probe = WB_PROBE_NO_32BIT_SET;
    t->set = (force_arm64ec || strstr(exe, "x64") || backslash) ? WB_SET_ARM64EC : WB_SET_AARCH64;

    if (wb_join(path, sizeof(path), "%s/i386-windows/ntdll.dll", bundle) != 0) return -1;
    if (wb_exists(path)) {
        if (wb_c_drive(exe)) {
            size_t base;
            if (wb_join(t->probed, sizeof(t->probed), "%s/drive_c/", prefix) != 0) return -1;
            base = strlen(t->probed);
            if (wb_join(t->probed + base, sizeof(t->probed) - base, "%s", exe + 3) != 0) return -1;
            backslashes_to_slashes(t->probed + base);
            t->probe = WB_PROBE_FILE;
            t->machine = wb_pe_machine(t->probed);
        } else if (!backslash && !drive) {
            char a[PATH_MAX], b[PATH_MAX];
            if (wb_join(a, sizeof(a), "%s/aarch64-windows/%s", bundle, exe) != 0 ||
                wb_join(b, sizeof(b), "%s/arm64ec-windows/%s", bundle, exe) != 0)
                return -1;
            if (wb_exists(a) || wb_exists(b)) {
                t->probe = WB_PROBE_BUNDLE64;
            } else {
                if (wb_join(t->probed, sizeof(t->probed), "%s/i386-windows/%s", bundle, exe) != 0)
                    return -1;
                t->probe = WB_PROBE_FILE;
                t->machine = wb_pe_machine(t->probed);
            }
        } else {
            t->probe = WB_PROBE_OTHER_FORM;
        }
    }

    if (t->machine == WB_MACHINE_I386) {
        t->is32 = 1;
        t->set = WB_SET_AARCH64;
    }

    if (backslash || drive) {
        if (wb_join(t->winpath, sizeof(t->winpath), "%s", exe) != 0) return -1;
    } else if (wb_join(t->winpath, sizeof(t->winpath), "C:\\windows\\%s\\%s",
                       t->is32 ? "syswow64" : "system32", exe) != 0) {
        return -1;
    }
    return 0;
}

/* ---- working folder ---- */

static void strip_trailing_backslashes(char *s)
{
    size_t n = strlen(s);
    while (n > 3 && s[n - 1] == '\\') s[--n] = 0;
}

int wb_workdir_choose(const char *env_workdir, const char *exe, char *out, size_t cap)
{
    if (env_workdir) {
        size_t n = strlen(env_workdir);
        if (wb_c_drive(env_workdir) && n > 3 && n < 510 && !strstr(env_workdir, "..")) {
            if (wb_join(out, cap, "%s", env_workdir) != 0) return 0;
            strip_trailing_backslashes(out);
            return 1;
        }
    }
    if (exe && wb_c_drive(exe)) {
        const char *last = strrchr(exe, '\\');   /* at index 2 or later */
        size_t len = (size_t)(last - exe);
        if (len < 3) len = 3;                     /* keep "C:\" */
        if (len >= cap) return 0;
        memcpy(out, exe, len);
        out[len] = 0;
        strip_trailing_backslashes(out);
        return 1;
    }
    return 0;
}

int wb_workdir_unix(const char *prefix, const char *win, char *out, size_t cap)
{
    size_t base;

    if (!wb_c_drive(win)) return -1;
    if (!win[3]) return wb_join(out, cap, "%s/drive_c", prefix);
    if (wb_join(out, cap, "%s/drive_c/", prefix) != 0) return -1;
    base = strlen(out);
    if (wb_join(out + base, cap - base, "%s", win + 3) != 0) return -1;
    backslashes_to_slashes(out + base);
    return 0;
}

int wb_workdir_initial_cwd(const char *win, char *out, size_t cap)
{
    if (wb_join(out, cap, "%s", win) != 0) return -1;
    strip_trailing_backslashes(out);
    if (strlen(out) == 3 && out[2] == '\\') return 0;   /* "C:\" */
    return wb_join(out + strlen(out), cap - strlen(out), "\\");
}

/* ---- Steam identity ---- */

int wb_steam_identity_valid(const char *appid, const char *path)
{
    size_t n;

    if (!appid || !path) return 0;
    n = strlen(appid);
    if (n < 1 || n > 10) return 0;
    for (size_t i = 0; i < n; i++)
        if (appid[i] < '0' || appid[i] > '9') return 0;
    if (!wb_c_drive(path) || strlen(path) >= 1024 || strstr(path, "..")) return 0;
    return 1;
}

/* ---- WINEDEBUG choice ---- */

int wb_debug_verbose(const char *value)
{
    /* Clarification 17: non-empty and not starting with '0'. */
    return value && value[0] && value[0] != '0';
}

/* ---- CPU probe ---- */

static const struct { const char *key; const char *sysctl; } g_probes[] = {
    { "AFP",     "hw.optional.arm.FEAT_AFP" },
    { "FLAGM",   "hw.optional.arm.FEAT_FlagM" },
    { "FLAGM2",  "hw.optional.arm.FEAT_FlagM2" },
    { "FCMA",    "hw.optional.arm.FEAT_FCMA" },
    { "RCPC",    "hw.optional.arm.FEAT_LRCPC" },
    { "LRCPC2",  "hw.optional.arm.FEAT_LRCPC2" },
    { "AES",     "hw.optional.arm.FEAT_AES" },
    { "PMULL",   "hw.optional.arm.FEAT_PMULL" },
    { "SHA",     "hw.optional.arm.FEAT_SHA256" },
    { "CRC",     "hw.optional.armv8_crc32" },
    { "ATOMICS", "hw.optional.arm.FEAT_LSE" },
};

void wb_hostprobe_format(char *out, size_t cap, wb_sysctl_query query)
{
    size_t o = 0;

    if (!cap) return;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(g_probes) / sizeof(g_probes[0]); i++) {
        int value = 0;
        char v = query(g_probes[i].sysctl, &value) ? (value ? '1' : '0') : '?';
        int n = snprintf(out + o, cap - o, "%s%s=%c", i ? "," : "", g_probes[i].key, v);
        if (n < 0 || (size_t)n >= cap - o) break;
        o += (size_t)n;
    }
}
