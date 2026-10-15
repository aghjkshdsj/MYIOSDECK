// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#ifndef B_LOGIC_H
#define B_LOGIC_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct wb_b_setting { char *key; char *value; };
struct wb_b_config { struct wb_b_setting *items; size_t count; };
int wb_b_cfg_read(FILE *file, struct wb_b_config *cfg);
void wb_b_cfg_free(struct wb_b_config *cfg);
int wb_b_cfg_get(const struct wb_b_config *cfg, const char *key, char *out, size_t cap);
const char *wb_b_cfg_value(const struct wb_b_config *cfg, const char *key);
int wb_b_cfg_int(const struct wb_b_config *cfg, const char *key, int fallback);
int wb_b_cfg_bool(const struct wb_b_config *cfg, const char *key);
int wb_b_sync_default(const struct wb_b_config *cfg, int fastsync_already_set);
int wb_b_split_args(const char *text, char **out, size_t cap);
void wb_b_free_args(char **args, int count);
int wb_b_c_path(const char *path, size_t limit, int require_child);
int wb_b_c_absolute(const char *path);
char *wb_b_workdir(const char *override, const char *exe);
char *wb_b_unix_c_path(const char *prefix, const char *windows);
char *wb_b_windows_path(const char *exe, int is32);
int wb_b_pe_machine(const char *path);
int wb_b_steam_identity(const char *id, const char *path);
struct wb_b_picture { int x, y, w, h, metal; };
int wb_b_choose_picture(const struct wb_b_picture *items, size_t count, int dw, int dh,
                     int screen_w, int screen_h);
int wb_b_extract_tar_gz(const char *archive, const char *destination, size_t *files, size_t *dirs);
#endif
