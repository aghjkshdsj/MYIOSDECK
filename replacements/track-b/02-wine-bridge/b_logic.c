// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_logic.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>

static char *wb_b_trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = 0;
    return s;
}

void wb_b_cfg_free(struct wb_b_config *cfg) {
    for (size_t i = 0; i < cfg->count; ++i) { free(cfg->items[i].key); free(cfg->items[i].value); }
    free(cfg->items);
    *cfg = (struct wb_b_config){0};
}

int wb_b_cfg_read(FILE *file, struct wb_b_config *cfg) {
    *cfg = (struct wb_b_config){0};
    if (!file) return -1;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int first = 1;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *start = line;
        if (first && length >= 3 && memcmp(start, "\xef\xbb\xbf", 3) == 0) start += 3;
        first = 0;
        start = wb_b_trim(start);
        if (!*start || *start == '#') continue;
        char *equals = strchr(start, '=');
        if (!equals) continue;
        *equals = 0;
        char *key = wb_b_trim(start), *value = wb_b_trim(equals + 1);
        if (!*key) continue;
        size_t i;
        for (i = 0; i < cfg->count; ++i) if (!strcmp(key, cfg->items[i].key)) break;
        char *copy = strdup(value);
        if (!copy) goto failure;
        if (i < cfg->count) { free(cfg->items[i].value); cfg->items[i].value = copy; continue; }
        char *key_copy = strdup(key);
        if (!key_copy) { free(copy); goto failure; }
        struct wb_b_setting *grown = realloc(cfg->items, (cfg->count + 1) * sizeof(*grown));
        if (!grown) { free(copy); free(key_copy); goto failure; }
        cfg->items = grown;
        cfg->items[cfg->count++] = (struct wb_b_setting){key_copy, copy};
    }
    free(line);
    if (ferror(file)) { wb_b_cfg_free(cfg); return -1; }
    return 0;
failure:
    free(line);
    wb_b_cfg_free(cfg);
    return -1;
}

const char *wb_b_cfg_value(const struct wb_b_config *cfg, const char *key) {
    for (size_t i = 0; i < cfg->count; ++i) if (!strcmp(cfg->items[i].key, key)) return cfg->items[i].value;
    return NULL;
}
int wb_b_cfg_get(const struct wb_b_config *cfg, const char *key, char *out, size_t cap) {
    const char *value = wb_b_cfg_value(cfg, key);
    if (cap && out) snprintf(out, cap, "%s", value ? value : "");
    return value != NULL;
}
int wb_b_cfg_int(const struct wb_b_config *cfg, const char *key, int fallback) {
    const char *v = wb_b_cfg_value(cfg, key);
    if (!v || !*v) return fallback;
    errno = 0;
    char *end;
    long n = strtol(v, &end, 10);
    return errno || *end || n < INT_MIN || n > INT_MAX ? fallback : (int)n;
}
int wb_b_cfg_bool(const struct wb_b_config *cfg, const char *key) {
    const char *v = wb_b_cfg_value(cfg, key);
    return v && (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcasecmp(v, "yes"));
}
int wb_b_sync_default(const struct wb_b_config *cfg, int already) {
    if (wb_b_cfg_bool(cfg, "inproc-sync")) return 0;
    if (wb_b_cfg_value(cfg, "env.MADEIRA_FASTSYNC")) return 0;
    if (wb_b_cfg_value(cfg, "inproc-sync")) return 0;
    return !already;
}

void wb_b_free_args(char **args, int count) { for (int i = 0; i < count; ++i) { free(args[i]); args[i] = NULL; } }
int wb_b_split_args(const char *text, char **out, size_t cap) {
    if (!text) return 0;
    const char *p = text;
    int count = 0;
    while (*p && (size_t)count < cap) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char *token = malloc(strlen(p) + 1);
        if (!token) { wb_b_free_args(out, count); return -1; }
        size_t used = 0;
        int quoted = 0;
        while (*p) {
            if (*p == '"') { quoted = !quoted; ++p; continue; }
            if (!quoted && (*p == ' ' || *p == '\t')) break;
            token[used++] = *p++;
        }
        token[used] = 0;
        out[count++] = token;
    }
    return count;
}

int wb_b_c_absolute(const char *path) {
    return path && strlen(path) >= 3 && (path[0] == 'C' || path[0] == 'c') && path[1] == ':' && path[2] == '\\';
}
int wb_b_c_path(const char *path, size_t limit, int child) {
    if (!path) return 0;
    size_t length = strlen(path);
    return length >= (child ? 4u : 3u) && length < limit && wb_b_c_absolute(path) && !strstr(path, "..");
}

char *wb_b_workdir(const char *override, const char *exe) {
    char *result = NULL;
    if (wb_b_c_path(override, 510, 1)) result = strdup(override);
    else if (wb_b_c_absolute(exe)) {
        const char *last = strrchr(exe, '\\');
        if (last) {
            size_t n = (size_t)(last - exe);
            if (n == 2) n = 3; // Keep the root separator.
            result = strndup(exe, n);
        }
    }
    if (result) {
        size_t n = strlen(result);
        while (n > 3 && result[n - 1] == '\\') result[--n] = 0;
    }
    return result;
}

char *wb_b_unix_c_path(const char *prefix, const char *windows) {
    if (!prefix || !wb_b_c_absolute(windows)) return NULL;
    size_t size = strlen(prefix) + strlen(windows) + 12;
    char *result = malloc(size);
    if (!result) return NULL;
    snprintf(result, size, "%s/drive_c/%s", prefix, windows + 3);
    for (char *p = result + strlen(prefix) + 9; *p; ++p) if (*p == '\\') *p = '/';
    return result;
}

char *wb_b_windows_path(const char *exe, int is32) {
    if (!exe) return NULL;
    if (strchr(exe, '\\') || (strlen(exe) >= 2 && exe[1] == ':')) return strdup(exe);
    size_t cap = strlen(exe) + 40;
    char *result = malloc(cap);
    if (result) snprintf(result, cap, "C:\\windows\\%s\\%s", is32 ? "syswow64" : "system32", exe);
    return result;
}

static uint32_t wb_b_le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int wb_b_pe_machine(const char *path) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return -1;
    unsigned char dos[64], pe[6];
    int result = -1;
    if (fread(dos, 1, sizeof dos, f) != sizeof dos || dos[0] != 'M' || dos[1] != 'Z') goto end;
    uint32_t offset = wb_b_le32(dos + 0x3c);
    if (offset > 16u * 1024u * 1024u || fseek(f, (long)offset, SEEK_SET)) goto end;
    if (fread(pe, 1, sizeof pe, f) != sizeof pe || memcmp(pe, "PE\0\0", 4)) goto end;
    result = pe[4] | pe[5] << 8;
end:
    fclose(f);
    return result;
}

int wb_b_steam_identity(const char *id, const char *path) {
    if (!id) return 0;
    size_t n = strlen(id);
    if (n < 1 || n > 10 || !wb_b_c_path(path, 1024, 0)) return 0;
    for (size_t i = 0; i < n; ++i) if (id[i] < '0' || id[i] > '9') return 0;
    return 1;
}

int wb_b_choose_picture(const struct wb_b_picture *items, size_t count, int dw, int dh, int sw, int sh) {
    int best = -1, metal = 0;
    int64_t area = -1;
    const int64_t desktop = (int64_t)(sw > 0 ? sw : 1) * (sh > 0 ? sh : 1);
    for (size_t i = 0; i < count; ++i) {
        const struct wb_b_picture p = items[i];
        if (p.w <= 0 || p.h <= 0) continue;
        int64_t a = (int64_t)p.w * p.h;
        if (!p.metal && (metal || p.w != dw || p.h != dh || a < (desktop + 3) / 4)) continue;
        if (p.metal && !metal) { best = (int)i; area = a; metal = 1; }
        else if (a > area) { best = (int)i; area = a; }
    }
    return best;
}
