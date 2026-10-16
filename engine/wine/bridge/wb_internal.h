// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Shared declarations of the spec 02 sources: the Wine handle types the contract needs
// (declared here, never taken from Wine's headers), the two interface headers, and the
// functions the files of this folder call in each other. Build with interfaces/ on the
// include path.

#ifndef WB_INTERNAL_H
#define WB_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Types listed at the top of interfaces/wine_unix_contract.h. */
typedef void *HWND;
typedef void *HCURSOR;
typedef unsigned int UINT;
typedef int INT;
typedef unsigned long DWORD;
typedef long WINELONG;
typedef struct { WINELONG left, top, right, bottom; } RECT;
struct window_rects;
struct window_surface;
#ifndef __OBJC__
/* The unix side's BOOL. Objective-C files keep their own; the hooks that return BOOL across
 * the boundary live in .c files (wb_drv.c) so they return exactly this type. */
typedef int BOOL;
#endif

/* File-scope tags, so the contract's prototypes (which name some of these before Section C
 * defines them) refer to the same struct types as the definitions. */
struct winios_gamepad;
struct winios_hidpad;
struct winios_hidpad_output;
struct winios_gamepad_vibration;

#include "wine_unix_contract.h"
#include "wine_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- provided by the app (not by spec 02) ---- */
extern int64_t fex_get_jit_write_offset(void);

/* ---- wb_paths.m: directories of the app, computed once ---- */
const char *wb_documents_dir(void);
const char *wb_bundle_dir(void);
const char *wb_app_support_dir(void);   /* may be NULL */

/* ---- wb_util.c ---- */
/// snprintf into `out`; 0 when it fit, -1 when it was cut short.
int  wb_join(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/// mkdir -p. 0 when the directory exists afterwards.
int  wb_mkdirs(const char *path, mode_t mode);
int  wb_exists(const char *path);       /* stat succeeds (follows links) */
int  wb_lexists(const char *path);      /* lstat succeeds */
int  wb_is_dir(const char *path);       /* a directory, following links */
int  wb_is_real_dir(const char *path);  /* a directory that is not a symlink */
int  wb_is_symlink(const char *path);
/// Make `linkpath` a symlink to `target`, removing a non-directory entry already there.
int  wb_symlink_replace(const char *target, const char *linkpath);
/// "C:\" or "c:\" at the start.
int  wb_c_drive(const char *s);
/// getenv(name) is set and equals value.
int  wb_env_is(const char *name, const char *value);
/// Write a whole file through a temporary name and rename. 0 or -1.
int  wb_write_file(const char *path, const void *data, size_t len, mode_t mode);

/* ---- wb_targz.c ---- */
int  wb_targz_extract(const char *archive, const char *dest);

/* ---- wb_config.c (Part G) ---- */
struct wb_cfg_entry { char *key; char *value; };
struct wb_cfg { struct wb_cfg_entry *entries; int count; int cap; };
void wb_cfg_init(struct wb_cfg *cfg);
/// Parse a whole settings file held in memory. 0, or -1 when out of memory.
int  wb_cfg_parse(struct wb_cfg *cfg, const char *buf, size_t len);
/// Read <Documents>/madeira.cfg into cfg. 1 when read, 0 when there is none, -1 on error.
int  wb_cfg_load(struct wb_cfg *cfg);
/// The value of the last line with this key, or NULL.
const char *wb_cfg_lookup(const struct wb_cfg *cfg, const char *key);
void wb_cfg_free(struct wb_cfg *cfg);
/// "1", "on", "true" or "yes" (any case).
int  wb_cfg_is_true(const char *value);
/// The spec's cfg_get: read the file, copy the value; 1 when the key was found.
int  wb_cfg_get(const char *key, char *out, size_t cap);
long wb_cfg_get_int(const char *key, long def);
int  wb_cfg_get_bool(const char *key, int def);
/// Launch step 9: export the env. lines, keeping per-launch keys the app already set.
void wb_cfg_export_env(const struct wb_cfg *cfg);
/// The sync engine rule: 1 when MADEIRA_FASTSYNC=auto must be set.
int  wb_sync_default_needed(const struct wb_cfg *cfg, int fastsync_env_set);
void wb_cfg_apply_sync_default(const struct wb_cfg *cfg);
extern const char *const wb_per_launch_keys[];
extern const int wb_per_launch_key_count;
void wb_unset_per_launch_keys(void);

/* ---- wb_launch_rules.c (Part C, pure rules) ---- */
#define WB_MAX_ARGS 64
/// Split on spaces and tabs; double quotes group and are removed. Fills at most `max`
/// strdup'ed strings and returns how many; *dropped (if not NULL) counts the rest.
int  wb_split_args(const char *s, char **argv, int max, int *dropped);
void wb_free_args(char **argv, int n);

enum { WB_SET_AARCH64 = 0, WB_SET_ARM64EC = 1 };
const char *wb_set_dir(int set);   /* "aarch64-windows" / "arm64ec-windows" */

#define WB_MACHINE_I386  0x014c
/// The PE machine of a file, or -1 when it can't be read as a PE image.
int  wb_pe_machine(const char *path);

enum wb_probe_kind {
    WB_PROBE_NO_32BIT_SET,   /* the bundle has no i386-windows/ntdll.dll: no detection */
    WB_PROBE_BUNDLE64,       /* bare name found in a 64-bit set: 64-bit, not probed */
    WB_PROBE_FILE,           /* `probed` was read; `machine` is its result */
    WB_PROBE_OTHER_FORM,     /* a name form the rules don't probe */
};
struct wb_target {
    int  set;                /* WB_SET_* for the 64-bit half */
    int  is32;               /* the program is an i386 PE */
    int  machine;            /* probe result, -1 for unknown */
    enum wb_probe_kind probe;
    char probed[1024];       /* the file probed, when probe == WB_PROBE_FILE */
    char winpath[1024];      /* argv[1] */
};
/// Spec 02 "Target and DLL set". 0, or -1 when a path didn't fit.
int  wb_choose_target(const char *exe, const char *bundle, const char *prefix,
                      int force_arm64ec, struct wb_target *t);

/// Spec 02 "Working folder": the Windows folder (no trailing backslash except "C:\") from
/// MADEIRA_WORKDIR's value (may be NULL) or the program path. 1 with a folder, else 0.
int  wb_workdir_choose(const char *env_workdir, const char *exe, char *out, size_t cap);
/// <prefix>/drive_c/<rest with slashes> for a "C:\..." folder. 0 or -1.
int  wb_workdir_unix(const char *prefix, const char *win, char *out, size_t cap);
/// The folder with exactly one trailing backslash. 0 or -1.
int  wb_workdir_initial_cwd(const char *win, char *out, size_t cap);

/// MADEIRA_DEBUG_VERBOSE's value (may be NULL) asks for the verbose WINEDEBUG set.
int  wb_debug_verbose(const char *value);

/// MADEIRA_STEAM_APPID / MADEIRA_STEAM_APPPATH pass the checks of launch step 6.
int  wb_steam_identity_valid(const char *appid, const char *path);

/// Query one sysctl as a 32-bit int: 1 and *value when it exists, else 0.
typedef int (*wb_sysctl_query)(const char *name, int *value);
/// FEX_MADEIRA_HOSTPROBE's value, "AFP=1,FLAGM=0,...".
void wb_hostprobe_format(char *out, size_t cap, wb_sysctl_query query);

/* ---- wb_prefix.c, wb_sxs.c (Part H) ---- */
void wb_link_dll_farms(const char *prefix, const char *bundle, int set);
void wb_build_winsxs(const char *prefix, const char *bundle);
const char *wb_user_name(char *buf, size_t cap);

/* ---- wb_picture.c (Part E "Game picture", pure rule) ---- */
struct wb_client_rect { int x, y, w, h; int metal; };
int  wb_pick_picture_rect(const struct wb_client_rect *rects, int n, int dw, int dh,
                          int desk_w, int desk_h, int *x, int *y, int *w, int *h);

/* ---- wb_pads.c: getters of the output stores (Part F, for later use) ---- */
void wb_hidpad_get_output(struct winios_hidpad_output *out);
void wb_gamepad_get_vibration(int index, struct winios_gamepad_vibration *out);

/* ---- wb_windows.m, wb_display.m (Parts D and E) ---- */
void wb_windows_mark_metal(HWND hwnd);
void wb_windows_forget(HWND hwnd);
void winios_session_reset(void);
void wb_display_reset_mode(void);

/* ---- wb_audio.m ---- */
void wb_audio_activate(void);

#ifdef __cplusplus
}
#endif

#ifdef __OBJC__
#import <Foundation/Foundation.h>
/// Posted on the main queue when winios_display_mode_changed changes the size.
extern NSString *const wb_display_mode_notification;
#endif

#endif
