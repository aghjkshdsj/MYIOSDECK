// SPDX-License-Identifier: LGPL-2.1-or-later
// Assembly metadata derived from upstream Wine 11.4 manifest resources.
// Copyright: Wine contributors; source manifests have no individual copyright notice.
// Track B transformation (data declarations only), 2026 MYIOSDECK author.
// See PROVENANCE.md for exact upstream paths and license boundary.
#ifndef B_SXS_DATA_H
#define B_SXS_DATA_H
#include <stddef.h>
struct wb_b_assembly {
    const char *name, *token, *version;
    const char *files[4];
    int controls;
};
static const struct wb_b_assembly wb_b_assemblies[] = {
    {"Microsoft.Windows.Common-Controls", "6595b64144ccf1df", "6.0.2600.2982", {"comctl32.dll", NULL}, 1},
    {"Microsoft.VC80.CRT", "1fc8b3b9a1e18e3b", "8.0.50727.9672", {"msvcr80.dll", "msvcp80.dll", "msvcm80.dll", NULL}, 0},
    {"Microsoft.VC90.CRT", "1fc8b3b9a1e18e3b", "9.0.30729.6161", {"msvcr90.dll", "msvcp90.dll", "msvcm90.dll", NULL}, 0},
    {"Microsoft.VC80.ATL", "1fc8b3b9a1e18e3b", "8.0.50727.4053", {"atl80.dll", NULL}, 0},
    {"Microsoft.VC90.ATL", "1fc8b3b9a1e18e3b", "9.0.30729.6161", {"atl90.dll", NULL}, 0},
    {"Microsoft.Windows.GdiPlus", "6595b64144ccf1df", "1.0.6000.16386", {"gdiplus.dll", NULL}, 0},
    {"Microsoft.Windows.GdiPlus", "6595b64144ccf1df", "1.1.7601.23038", {"gdiplus.dll", NULL}, 0},
    {"Microsoft-Windows-MSXML30", "31bf3856ad364e35", "6.0.6000.16386", {"msxml3.dll", NULL}, 0},
    {"Microsoft.MSXML2", "6bd6b9abf345378f", "4.1.0.0", {"msxml4.dll", NULL}, 0},
    {"Microsoft-Windows-MSXML60", "31bf3856ad364e35", "6.0.6000.16386", {"msxml6.dll", NULL}, 0}
};
static const char *const wb_b_control_classes[] = {
    "Button", "ButtonListBox", "ComboBoxEx32", "ComboLBox", "ComboBox", "Edit", "ListBox", "NativeFontCtl",
    "ReBarWindow32", "ScrollBar", "Static", "SysAnimate32", "SysDateTimePick32", "SysHeader32", "SysIPAddress32",
    "SysLink", "SysListView32", "SysMonthCal32", "SysPager", "SysTabControl32", "SysTreeView32", "ToolbarWindow32",
    "msctls_hotkey32", "msctls_progress32", "msctls_statusbar32", "msctls_trackbar32", "msctls_updown32", "tooltips_class32"
};
#endif
