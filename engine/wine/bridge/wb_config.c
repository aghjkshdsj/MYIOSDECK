// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 02 Part G: the optional settings file <Documents>/madeira.cfg ("key = value" lines),
// its env. exports at launch step 9, the sync engine default, and the per-launch keys.

#include "wb_internal.h"
#include "wb_log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "wb-config"
#define CFG_MAX_BYTES (1024 * 1024)

const char *const wb_per_launch_keys[] = {
    "MADEIRA_FASTSYNC",
    "MADEIRA_FASTSYNC_SEM",
    "MADEIRA_CPU_COUNT",
    "DXMT_D9_ANISO_LIMIT",
    "FEX_X87REDUCEDPRECISION",
    "MADEIRA_DINPUT_PAD",
    "MADEIRA_FEX_AVX",
    "MADEIRA_FRAMEGEN",
    "DXMT_ENABLE_NVEXT",
    "DXMT_WSI_MONITOR_IDENTITY",
    "DXMT_WSI_MODE_TABLE",
};
const int wb_per_launch_key_count = (int)(sizeof(wb_per_launch_keys) / sizeof(wb_per_launch_keys[0]));

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

void wb_cfg_init(struct wb_cfg *cfg)
{
    cfg->entries = NULL;
    cfg->count = 0;
    cfg->cap = 0;
}

void wb_cfg_free(struct wb_cfg *cfg)
{
    for (int i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].key);
        free(cfg->entries[i].value);
    }
    free(cfg->entries);
    wb_cfg_init(cfg);
}

static char *dup_range(const char *s, const char *e)
{
    size_t n = (size_t)(e - s);
    char *r = malloc(n + 1);

    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static int add_entry(struct wb_cfg *cfg, const char *ks, const char *ke, const char *vs, const char *ve)
{
    struct wb_cfg_entry *e;

    if (cfg->count == cfg->cap) {
        int ncap = cfg->cap ? cfg->cap * 2 : 16;
        struct wb_cfg_entry *n = realloc(cfg->entries, (size_t)ncap * sizeof(*n));
        if (!n) return -1;
        cfg->entries = n;
        cfg->cap = ncap;
    }
    e = &cfg->entries[cfg->count];
    e->key = dup_range(ks, ke);
    e->value = dup_range(vs, ve);
    if (!e->key || !e->value) {
        free(e->key);
        free(e->value);
        return -1;
    }
    cfg->count++;
    return 0;
}

/* One line, without its '\n'. */
static int parse_line(struct wb_cfg *cfg, const char *s, const char *e)
{
    const char *eq, *ke, *vs, *ve;

    while (s < e && is_ws(*s)) s++;
    if (s == e || *s == '#') return 0;
    eq = memchr(s, '=', (size_t)(e - s));
    if (!eq) return 0;

    ke = eq;
    while (ke > s && is_ws(ke[-1])) ke--;
    if (ke == s) return 0;   /* no key */

    vs = eq + 1;
    ve = e;
    while (vs < ve && is_ws(*vs)) vs++;
    while (ve > vs && is_ws(ve[-1])) ve--;
    return add_entry(cfg, s, ke, vs, ve);
}

int wb_cfg_parse(struct wb_cfg *cfg, const char *buf, size_t len)
{
    const char *p, *end;

    wb_cfg_init(cfg);
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }
    p = buf;
    end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        if (parse_line(cfg, p, le) != 0) return -1;
        p = nl ? nl + 1 : end;
    }
    return 0;
}

int wb_cfg_load(struct wb_cfg *cfg)
{
    char path[PATH_MAX];
    struct stat st;
    char *buf;
    size_t got = 0;
    int fd, rc;

    wb_cfg_init(cfg);
    if (wb_join(path, sizeof(path), "%s/madeira.cfg", wb_documents_dir()) != 0) return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > CFG_MAX_BYTES) {
        close(fd);
        return -1;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    rc = wb_cfg_parse(cfg, buf, got);
    free(buf);
    if (rc != 0) {
        wb_cfg_free(cfg);
        return -1;
    }
    return 1;
}

const char *wb_cfg_lookup(const struct wb_cfg *cfg, const char *key)
{
    for (int i = cfg->count - 1; i >= 0; i--)
        if (strcmp(cfg->entries[i].key, key) == 0) return cfg->entries[i].value;
    return NULL;
}

int wb_cfg_is_true(const char *value)
{
    return value && (strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0 ||
                     strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0);
}

int wb_cfg_get(const char *key, char *out, size_t cap)
{
    struct wb_cfg cfg;
    const char *v;
    int found = 0;

    if (wb_cfg_load(&cfg) <= 0) return 0;
    v = wb_cfg_lookup(&cfg, key);
    if (v) {
        found = 1;
        if (out && cap) snprintf(out, cap, "%s", v);
    }
    wb_cfg_free(&cfg);
    return found;
}

long wb_cfg_get_int(const char *key, long def)
{
    char buf[64];
    char *end;
    long v;

    if (!wb_cfg_get(key, buf, sizeof(buf)) || !buf[0]) return def;
    errno = 0;
    v = strtol(buf, &end, 0);
    if (errno || *end) return def;
    return v;
}

int wb_cfg_get_bool(const char *key, int def)
{
    char buf[64];

    if (!wb_cfg_get(key, buf, sizeof(buf))) return def;
    return wb_cfg_is_true(buf);
}

static int per_launch_index(const char *name)
{
    for (int i = 0; i < wb_per_launch_key_count; i++)
        if (strcmp(wb_per_launch_keys[i], name) == 0) return i;
    return -1;
}

void wb_cfg_export_env(const struct wb_cfg *cfg)
{
    int app_set[sizeof(wb_per_launch_keys) / sizeof(wb_per_launch_keys[0])];

    /* Taken before any line is applied, so a later line still replaces an earlier one. */
    for (int i = 0; i < wb_per_launch_key_count; i++) app_set[i] = getenv(wb_per_launch_keys[i]) != NULL;

    for (int i = 0; i < cfg->count; i++) {
        const char *key = cfg->entries[i].key;
        const char *name, *value = cfg->entries[i].value;
        int idx;

        if (strncmp(key, "env.", 4) != 0 || !key[4]) continue;
        name = key + 4;
        idx = per_launch_index(name);
        if (idx >= 0 && app_set[idx]) {
            wb_log(TAG, "settings: %s=%s from the file ignored, the app set it for this launch",
                   name, value);
            continue;
        }
        if (setenv(name, value, 1) == 0) wb_log(TAG, "settings: export %s=%s", name, value);
        else wb_log(TAG, "settings: cannot export %s (errno %d)", name, errno);
    }
}

int wb_sync_default_needed(const struct wb_cfg *cfg, int fastsync_env_set)
{
    const char *inproc = wb_cfg_lookup(cfg, "inproc-sync");

    if (inproc && wb_cfg_is_true(inproc)) return 0;
    if (wb_cfg_lookup(cfg, "env.MADEIRA_FASTSYNC")) return 0;
    if (inproc) return 0;
    return !fastsync_env_set;
}

void wb_cfg_apply_sync_default(const struct wb_cfg *cfg)
{
    if (!wb_sync_default_needed(cfg, getenv("MADEIRA_FASTSYNC") != NULL)) return;
    setenv("MADEIRA_FASTSYNC", "auto", 1);
    wb_log(TAG, "settings: no sync setting, MADEIRA_FASTSYNC=auto");
}

void wb_unset_per_launch_keys(void)
{
    for (int i = 0; i < wb_per_launch_key_count; i++) unsetenv(wb_per_launch_keys[i]);
}

/* Before main: the server thread later changes HOME to the prefix, and the unix side finds
 * the settings file through MADEIRA_DOCS_DIR. */
__attribute__((constructor)) static void wb_cfg_early_docs_dir(void)
{
    const char *home;
    char buf[PATH_MAX];

    if (getenv("MADEIRA_DOCS_DIR")) return;
    home = getenv("HOME");
    if (!home || !*home) return;
    if (wb_join(buf, sizeof(buf), "%s/Documents", home) == 0) setenv("MADEIRA_DOCS_DIR", buf, 1);
}
