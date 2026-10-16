/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Assembly identities (name, publicKeyToken, version), file lists and the Common Controls
 * window-class list, transcribed from the WINE_MANIFEST manifests of upstream Wine 11.4
 * (gitlab.winehq.org/wine/wine, tag wine-11.4):
 *   dlls/comctl32_v6/comctl32.manifest, dlls/msvcr80/msvcr80.manifest,
 *   dlls/msvcr90/msvcr90.manifest, dlls/atl80/atl80.manifest, dlls/atl90/atl90.manifest,
 *   dlls/gdiplus/gdiplus.manifest, dlls/gdiplus/gdiplus11.manifest,
 *   dlls/msxml3/msxml3.manifest, dlls/msxml4/msxml4.manifest, dlls/msxml6/msxml6.manifest.
 * Copyright the Wine project authors (see AUTHORS in Wine 11.4); licensed under the GNU
 * Lesser General Public License version 2.1 or later, like Wine.
 *
 * Changed for MYIOSDECK, 2026-10-15: the data was put into this C table (the table layout is
 * new) and each assembly's first file names its source DLL in the 32-bit set, which is the
 * DLL that carries the manifest upstream (comctl32_v6.dll for Common Controls).
 */

#include "wb_sxs.h"

#include <stddef.h>

static const char *const comctl32_classes[] = {
    "Button", "ButtonListBox", "ComboBoxEx32", "ComboLBox", "ComboBox", "Edit", "ListBox",
    "NativeFontCtl", "ReBarWindow32", "ScrollBar", "Static", "SysAnimate32",
    "SysDateTimePick32", "SysHeader32", "SysIPAddress32", "SysLink", "SysListView32",
    "SysMonthCal32", "SysPager", "SysTabControl32", "SysTreeView32", "ToolbarWindow32",
    "msctls_hotkey32", "msctls_progress32", "msctls_statusbar32", "msctls_trackbar32",
    "msctls_updown32", "tooltips_class32", NULL,
};

static const struct wb_sxs_file comctl32_files[] = {
    { "comctl32.dll", "comctl32_v6.dll", comctl32_classes },
};
static const struct wb_sxs_file vc80_crt_files[] = {
    { "msvcr80.dll", "msvcr80.dll", NULL },
    { "msvcp80.dll", "msvcp80.dll", NULL },
    { "msvcm80.dll", "msvcm80.dll", NULL },
};
static const struct wb_sxs_file vc90_crt_files[] = {
    { "msvcr90.dll", "msvcr90.dll", NULL },
    { "msvcp90.dll", "msvcp90.dll", NULL },
    { "msvcm90.dll", "msvcm90.dll", NULL },
};
static const struct wb_sxs_file atl80_files[] = { { "atl80.dll", "atl80.dll", NULL } };
static const struct wb_sxs_file atl90_files[] = { { "atl90.dll", "atl90.dll", NULL } };
static const struct wb_sxs_file gdiplus_files[] = { { "gdiplus.dll", "gdiplus.dll", NULL } };
static const struct wb_sxs_file msxml3_files[] = { { "msxml3.dll", "msxml3.dll", NULL } };
static const struct wb_sxs_file msxml4_files[] = { { "msxml4.dll", "msxml4.dll", NULL } };
static const struct wb_sxs_file msxml6_files[] = { { "msxml6.dll", "msxml6.dll", NULL } };

#define FILES(a) a, (int)(sizeof(a) / sizeof(a[0]))

const struct wb_sxs_assembly wb_sxs_assemblies[] = {
    { "Microsoft.Windows.Common-Controls", "6595b64144ccf1df", "6.0.2600.2982", FILES(comctl32_files) },
    { "Microsoft.VC80.CRT", "1fc8b3b9a1e18e3b", "8.0.50727.9672", FILES(vc80_crt_files) },
    { "Microsoft.VC90.CRT", "1fc8b3b9a1e18e3b", "9.0.30729.6161", FILES(vc90_crt_files) },
    { "Microsoft.VC80.ATL", "1fc8b3b9a1e18e3b", "8.0.50727.4053", FILES(atl80_files) },
    { "Microsoft.VC90.ATL", "1fc8b3b9a1e18e3b", "9.0.30729.6161", FILES(atl90_files) },
    { "Microsoft.Windows.GdiPlus", "6595b64144ccf1df", "1.0.6000.16386", FILES(gdiplus_files) },
    { "Microsoft.Windows.GdiPlus", "6595b64144ccf1df", "1.1.7601.23038", FILES(gdiplus_files) },
    { "Microsoft-Windows-MSXML30", "31bf3856ad364e35", "6.0.6000.16386", FILES(msxml3_files) },
    { "Microsoft.MSXML2", "6bd6b9abf345378f", "4.1.0.0", FILES(msxml4_files) },
    { "Microsoft-Windows-MSXML60", "31bf3856ad364e35", "6.0.6000.16386", FILES(msxml6_files) },
};

const int wb_sxs_assembly_count = (int)(sizeof(wb_sxs_assemblies) / sizeof(wb_sxs_assemblies[0]));
