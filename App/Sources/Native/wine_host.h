// SPDX-License-Identifier: GPL-3.0-or-later
// Stage 2 host side for Wine (Madeira's iOS port): hands Wine a slice of
// MYIOSDECK's JIT pool and starts wineserver + the Windows process in-process.

#ifndef MYIOSDECK_WINE_HOST_H
#define MYIOSDECK_WINE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// True when this build links Wine's unix side and bundles the DLLs.
bool mid_wine_linked(void);

/// Boot Wine with `prefix` (created from the bundled template on first use)
/// and run `exe` (a path inside the prefix, e.g. "C:\\windows\\system32\\cmd.exe",
/// or a bare name looked up in system32). Requires the JIT pool. Wine can only
/// be started once per app run. Returns false and fills err on failure.
bool mid_wine_boot(const char *prefix, const char *exe, const char *args, char *err, size_t errlen);

/// Create the prefix from the bundled template if it is new (idempotent), so
/// files can be placed in drive_c before Wine first boots. No-op without Wine.
void mid_wine_seed_prefix(const char *prefix);

/// 1 while the Windows process runs.
int mid_wine_running(void);

/// 1 and the NTSTATUS when the program ended with an error (0xC...).
int wine_crash_exit_status(uint32_t *status);

/// Route this process's stdout/stderr (where Wine and its loader print) into
/// the MYIOSDECK log, line by line. Idempotent.
void mid_capture_stdio(void);

/// Stage 3 display: hand DXMT the CAMetalLayer every Wine window presents into
/// (once per process; the layer must outlive Wine).
void mid_display_set_layer(void *metal_layer);

/// Where the game surface sits: `frame` in window points (winios desktop
/// compositor), `rect` the presented area relative to that frame.
void mid_display_layout(double fx, double fy, double fw, double fh,
                        double rx, double ry, double rw, double rh);

/// Publish one XInput controller slot (0-3) to Wine's iOS driver; XINPUT_GAMEPAD_*
/// button bits, 0-255 triggers, full-range signed sticks (+y up). connected=0
/// unplugs the slot. No-op without Wine.
void mid_pad_set(int slot, int connected, uint16_t buttons, uint8_t lt, uint8_t rt,
                 int16_t lx, int16_t ly, int16_t rx, int16_t ry);

/// The guest's virtual monitor in pixels (1024x768 unless a program changes it).
void mid_display_screen_size(int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
