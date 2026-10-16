// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part H: seeding the Wine prefix, and the DLL farms (symlinks into the app bundle)
// linked at launch step 12.

#include "wb_internal.h"
#include "wb_log.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "wb-prefix"

/* ================= seeding ================= */

const char *wb_user_name(char *buf, size_t cap)
{
    const char *user = getenv("USER");
    struct passwd pw, *res = NULL;
    char pwbuf[1024];
    const char *base;

    if ((!user || !*user) && getpwuid_r(getuid(), &pw, pwbuf, sizeof(pwbuf), &res) == 0 && res &&
        res->pw_name && *res->pw_name)
        user = res->pw_name;
    if (!user || !*user) user = "wine";

    base = user;
    for (const char *p = user; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) base = "wine";
    snprintf(buf, cap, "%s", base);
    return buf;
}

static void seed_template(const char *prefix, const char *bundle)
{
    char stamp[PATH_MAX], tpl[PATH_MAX];

    if (wb_join(stamp, sizeof(stamp), "%s/.update-timestamp", prefix) != 0 ||
        wb_join(tpl, sizeof(tpl), "%s/prefix-template.tar.gz", bundle) != 0)
        return;
    if (wb_lexists(stamp)) return;   /* Wine has set this prefix up already */
    if (!wb_exists(tpl)) {
        wb_log(TAG, "seed: no prefix-template.tar.gz in the bundle, continuing without it");
        return;
    }
    if (wb_targz_extract(tpl, prefix) == 0) wb_log(TAG, "seed: template extracted into %s", prefix);
    else wb_log(TAG, "seed: template extraction into %s FAILED", prefix);
}

static void seed_dosdevices(const char *prefix)
{
    char dd[PATH_MAX], c[PATH_MAX], drive_c[PATH_MAX];
    struct stat st;

    if (wb_join(dd, sizeof(dd), "%s/dosdevices", prefix) != 0 ||
        wb_join(c, sizeof(c), "%s/c:", dd) != 0 ||
        wb_join(drive_c, sizeof(drive_c), "%s/drive_c", prefix) != 0)
        return;
    wb_mkdirs(drive_c, 0755);
    if (wb_mkdirs(dd, 0755) != 0) {
        wb_log(TAG, "seed: cannot create %s (errno %d)", dd, errno);
        return;
    }
    if (lstat(c, &st) == 0) {
        int rc = S_ISDIR(st.st_mode) ? rmdir(c) : unlink(c);
        if (rc != 0) wb_log(TAG, "seed: cannot remove the old %s (errno %d)", c, errno);
    }
    /* Only c:. A z: link to / is wrong on iOS. */
    if (symlink("../drive_c", c) != 0) wb_log(TAG, "seed: symlink %s failed (errno %d)", c, errno);
}

static int locallow_disabled(void)
{
    const char *env = getenv("MADEIRA_PROFILE_LOCALLOW");
    char v[16];

    if (env && env[0] == '0') return 1;
    if (wb_cfg_get("env.MADEIRA_PROFILE_LOCALLOW", v, sizeof(v)) && v[0] == '0') return 1;
    return 0;
}

static void seed_locallow(const char *prefix)
{
    char user[256], path[PATH_MAX];

    if (locallow_disabled()) {
        wb_log(TAG, "seed: LocalLow skipped (MADEIRA_PROFILE_LOCALLOW)");
        return;
    }
    wb_user_name(user, sizeof(user));
    /* Only LocalLow: Wine fills the rest of the profile itself, and only when those folders
     * don't exist yet. */
    if (wb_join(path, sizeof(path), "%s/drive_c/users/%s/AppData/LocalLow", prefix, user) != 0) return;
    if (wb_mkdirs(path, 0755) != 0) wb_log(TAG, "seed: cannot create %s (errno %d)", path, errno);
}

static void seed_shell_folders(const char *prefix)
{
    static const char *const names[] = { "Documents", "Desktop", "Downloads", "Music", "Pictures", "Videos" };
    char users[PATH_MAX], profile[PATH_MAX], path[PATH_MAX], old[PATH_MAX];
    struct dirent *de;
    int created = 0, repaired = 0;
    DIR *dir;

    if (wb_join(users, sizeof(users), "%s/drive_c/users", prefix) != 0) return;
    dir = opendir(users);
    if (!dir) return;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') continue;
        if (wb_join(profile, sizeof(profile), "%s/%s", users, de->d_name) != 0 || !wb_is_dir(profile))
            continue;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            struct stat st;
            if (wb_join(path, sizeof(path), "%s/%s", profile, names[i]) != 0) continue;
            if (lstat(path, &st) != 0) {
                if (mkdir(path, 0755) == 0) created++;
                else wb_log(TAG, "seed: cannot create %s (errno %d)", path, errno);
            } else if (S_ISLNK(st.st_mode) && !wb_exists(path)) {
                /* Templates built on a Mac can hold links to the build machine's home. */
                ssize_t n = readlink(path, old, sizeof(old) - 1);
                old[n > 0 ? n : 0] = 0;
                if (unlink(path) == 0 && mkdir(path, 0755) == 0) {
                    repaired++;
                    wb_log(TAG, "seed: %s was a dangling link to %s, now a folder", path, old);
                } else {
                    wb_log(TAG, "seed: cannot replace the dangling link %s (errno %d)", path, errno);
                }
            }
        }
    }
    closedir(dir);
    if (created || repaired)
        wb_log(TAG, "seed: shell folders: %d created, %d dangling links replaced", created, repaired);
}

static pthread_mutex_t g_seed_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_seeded;   /* the prefix seeded last */

void wb_seed_prefix(const char *prefix)
{
    char cdrive[PATH_MAX];

    if (!prefix || !*prefix) return;
    pthread_mutex_lock(&g_seed_lock);
    /* Cheap repeat: the same prefix, and it still has its c: link (it wasn't deleted). */
    if (g_seeded && strcmp(g_seeded, prefix) == 0 &&
        wb_join(cdrive, sizeof(cdrive), "%s/dosdevices/c:", prefix) == 0 && wb_lexists(cdrive)) {
        pthread_mutex_unlock(&g_seed_lock);
        return;
    }

    if (wb_mkdirs(prefix, 0755) != 0) wb_log(TAG, "seed: cannot create %s (errno %d)", prefix, errno);
    seed_template(prefix, wb_bundle_dir());
    seed_dosdevices(prefix);
    seed_locallow(prefix);
    seed_shell_folders(prefix);

    free(g_seeded);
    g_seeded = strdup(prefix);
    pthread_mutex_unlock(&g_seed_lock);
    wb_log(TAG, "seed: prefix %s ready", prefix);
}

/* ================= DLL farms ================= */

enum farm_mode {
    FARM_ALL,          /* every file */
    FARM_IF_ABSENT,    /* only names that don't resolve in the destination */
    FARM_WMI,          /* only the five WMI files */
    FARM_VCRUNTIME,    /* DLLs except vcruntime140.dll and msvcp140.dll */
};

static int is_wmi_file(const char *name)
{
    static const char *const wmi[] = { "wbemprox.dll", "wbemdisp.dll", "wmiutils.dll", "wmic.exe", "mofcomp.exe" };
    for (size_t i = 0; i < sizeof(wmi) / sizeof(wmi[0]); i++)
        if (strcasecmp(name, wmi[i]) == 0) return 1;
    return 0;
}

static int ends_with_dll(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".dll") == 0;
}

/* Link the files of `src` into `dst` (created if needed), one symlink per file, replacing
 * what is there. Names starting with '.' and sub-folders are skipped. */
static void farm(const char *src, const char *dst, enum farm_mode mode, const char *label)
{
    char s[PATH_MAX], d[PATH_MAX];
    struct dirent *de;
    int linked = 0, failed = 0;
    DIR *dir;

    dir = opendir(src);
    if (!dir) {
        wb_log(TAG, "farm %s: no source folder %s", label, src);
        return;
    }
    if (wb_mkdirs(dst, 0755) != 0) {
        wb_log(TAG, "farm %s: cannot create %s (errno %d)", label, dst, errno);
        closedir(dir);
        return;
    }
    while ((de = readdir(dir))) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
        if (mode == FARM_WMI && !is_wmi_file(name)) continue;
        if (mode == FARM_VCRUNTIME && (!ends_with_dll(name) || strcasecmp(name, "vcruntime140.dll") == 0 ||
                                       strcasecmp(name, "msvcp140.dll") == 0))
            continue;
        if (wb_join(s, sizeof(s), "%s/%s", src, name) != 0 || wb_join(d, sizeof(d), "%s/%s", dst, name) != 0) {
            failed++;
            continue;
        }
        if (wb_is_dir(s)) continue;
        if (mode == FARM_IF_ABSENT && wb_exists(d)) continue;   /* a dangling link counts as absent */
        if (wb_symlink_replace(s, d) == 0) linked++;
        else failed++;
    }
    closedir(dir);
    if (failed) wb_log(TAG, "farm %s: %d linked, %d FAILED", label, linked, failed);
    else wb_log(TAG, "farm %s: %d linked", label, linked);
}

static void link_override(const char *env, const char *sys32, const char *dll)
{
    const char *target = getenv(env);
    char d[PATH_MAX];
    int ok;

    if (!target || !*target) return;
    if (wb_mkdirs(sys32, 0755) != 0 || wb_join(d, sizeof(d), "%s/%s", sys32, dll) != 0) ok = 0;
    else ok = wb_symlink_replace(target, d) == 0;
    wb_log(TAG, "override %s -> %s: %s", dll, target, ok ? "ok" : "FAILED");
}

/* x64 C runtime links an earlier arm64ec session left in system32: links whose target is in
 * the bundle's x86_64-vcruntime/ (clarification 9). */
static void remove_vcruntime_links(const char *sys32, const char *vc)
{
    char d[PATH_MAX], target[PATH_MAX];
    struct dirent *de;
    int removed = 0;
    size_t vclen = strlen(vc);
    DIR *dir = opendir(sys32);

    if (!dir) return;
    while ((de = readdir(dir))) {
        ssize_t n;
        if (de->d_name[0] == '.') continue;
        if (wb_join(d, sizeof(d), "%s/%s", sys32, de->d_name) != 0) continue;
        n = readlink(d, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (strncmp(target, vc, vclen) == 0 && target[vclen] == '/' && unlink(d) == 0) removed++;
    }
    closedir(dir);
    if (removed) wb_log(TAG, "farm system32: %d x64 C runtime links of an earlier session removed", removed);
}

static void link_mono(const char *win, const char *bundle)
{
    char a[PATH_MAX], af[PATH_MAX], b[PATH_MAX], bf[PATH_MAX], monodir[PATH_MAX], dst[PATH_MAX];
    const char *support = wb_app_support_dir();
    const char *src = NULL;

    if (wb_join(a, sizeof(a), "%s/wine-mono", bundle) == 0 &&
        wb_join(af, sizeof(af), "%s/bin/libmono-2.0-x86.dll", a) == 0 && wb_exists(af))
        src = a;
    else if (support && wb_join(b, sizeof(b), "%s/WineMono/wine-mono", support) == 0 &&
             wb_join(bf, sizeof(bf), "%s/bin/libmono-2.0-x86.dll", b) == 0 && wb_exists(bf))
        src = b;

    if (wb_join(monodir, sizeof(monodir), "%s/mono", win) != 0 ||
        wb_join(dst, sizeof(dst), "%s/mono-2.0", monodir) != 0)
        return;

    if (src) {
        if (wb_is_real_dir(dst)) {
            wb_log(TAG, "mono: a real mono-2.0 folder is installed, kept");
        } else if (wb_mkdirs(monodir, 0755) == 0 && wb_symlink_replace(src, dst) == 0) {
            wb_log(TAG, "mono: mono-2.0 -> %s", src);
        } else {
            wb_log(TAG, "mono: linking mono-2.0 -> %s FAILED (errno %d)", src, errno);
        }
    } else if (wb_is_symlink(dst)) {
        unlink(dst);
        wb_log(TAG, "mono: no Wine Mono in this build, old mono-2.0 link removed");
    }
}

void wb_link_dll_farms(const char *prefix, const char *bundle, int set)
{
    char win[PATH_MAX], sys32[PATH_MAX], sub[PATH_MAX];
    char chosen[PATH_MAX], other[PATH_MAX], ec[PATH_MAX], aa[PATH_MAX], x86dir[PATH_MAX];
    char x86_ntdll[PATH_MAX], vc[PATH_MAX];

    if (wb_join(win, sizeof(win), "%s/drive_c/windows", prefix) != 0 ||
        wb_join(sys32, sizeof(sys32), "%s/system32", win) != 0 ||
        wb_join(chosen, sizeof(chosen), "%s/%s", bundle, wb_set_dir(set)) != 0 ||
        wb_join(other, sizeof(other), "%s/%s", bundle,
                wb_set_dir(set == WB_SET_ARM64EC ? WB_SET_AARCH64 : WB_SET_ARM64EC)) != 0 ||
        wb_join(ec, sizeof(ec), "%s/arm64ec-windows", bundle) != 0 ||
        wb_join(aa, sizeof(aa), "%s/aarch64-windows", bundle) != 0 ||
        wb_join(x86dir, sizeof(x86dir), "%s/i386-windows", bundle) != 0 ||
        wb_join(x86_ntdll, sizeof(x86_ntdll), "%s/ntdll.dll", x86dir) != 0 ||
        wb_join(vc, sizeof(vc), "%s/x86_64-vcruntime", bundle) != 0) {
        wb_log(TAG, "farms: paths too long, nothing linked");
        return;
    }

    if (set != WB_SET_ARM64EC) remove_vcruntime_links(sys32, vc);

    /* 1. The chosen set. */
    farm(chosen, sys32, FARM_ALL, "system32");
    /* 2. Interpreter overrides of the builds without JIT. */
    link_override("MYIOSDECK_NOJIT_EMULATOR", sys32, "xtajit64.dll");
    link_override("MYIOSDECK_NOJIT_WOW_CPU", sys32, "xtajit.dll");
    /* 3. WMI registers its files in system32\wbem. */
    if (wb_join(sub, sizeof(sub), "%s/wbem", sys32) == 0) farm(chosen, sub, FARM_WMI, "system32/wbem");
    /* 4. The other 64-bit set, for names system32 doesn't have yet. */
    farm(other, sys32, FARM_IF_ABSENT, "system32 (other 64-bit set)");
    /* 5. Each 64-bit kind's own copies. */
    if (wb_join(sub, sizeof(sub), "%s/sysx64", win) == 0) farm(ec, sub, FARM_ALL, "sysx64");
    if (wb_join(sub, sizeof(sub), "%s/sysaa64", win) == 0) farm(aa, sub, FARM_ALL, "sysaa64");
    /* 6. The 32-bit set, every session: a 64-bit launcher can start a 32-bit game. */
    if (wb_exists(x86_ntdll)) {
        if (wb_join(sub, sizeof(sub), "%s/syswow64", win) == 0) farm(x86dir, sub, FARM_ALL, "syswow64");
        if (wb_join(sub, sizeof(sub), "%s/syswow64/wbem", win) == 0) farm(x86dir, sub, FARM_WMI, "syswow64/wbem");
        wb_build_winsxs(prefix, bundle);
    }
    /* 7. Wine Mono. */
    link_mono(win, bundle);
    /* 8. The x64 C runtime over the built-in one, except the two that must stay built-in
     *    (C++ exceptions crossing into translated x86 code corrupt the stack). */
    if (set == WB_SET_ARM64EC && wb_is_dir(vc)) farm(vc, sys32, FARM_VCRUNTIME, "system32 (x64 C runtime)");
}
