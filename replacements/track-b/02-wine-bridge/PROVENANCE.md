<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

Author: GPT-6.1 Sol, clean side, track B. Date:2026-10-15.

Independent implementation from behavior specifications; no forbidden original or track A implementation was consulted. The upstream-derived assembly metadata is isolated in b_sxs_data.h with LGPL-2.1-or-later marking. All other new implementation and tests carry the requested MIT header. COPYING.LIB is the unchanged license file extracted from Wine11.4.

| Source | Section/use | Date |
|---|---|---|
| User's attached track B request | Isolation, output layout, workflow, tests and commits | 2026-10-15 |
| Shared CLAUDE.md | Clean-side rules, overridden by user's track B paths | 2026-10-15 |
| Shared specs/02-wine-bridge.md | PartsA–H, Done means, round1 clarifications1–18 reread after questions | 2026-10-15 |
| Shared interfaces/wine_bridge.h | Full bridge API and running lifetime | 2026-10-15 |
| Shared interfaces/wine_unix_contract.h | SectionsA–D: exact ABI/types/state structs/provided dependencies | 2026-10-15 |
| Shared interfaces/wine_host.h and app-api.md | Context: existing app glue and display/input units | 2026-10-15 |
| [Official Wine11.4 source archive](https://dl.winehq.org/wine/source/11.x/wine-11.4.tar.xz) | Only purposes expressly allowed in spec02, detailed below | 2026-10-15 |

Archive SHA256: `1970a46381d3bc2c44d651d08336370e499eeb8b53dc93cbd1ce544f7115e598`. Downloaded to track B work/ (ignored by git). GitLab raw attempts for the comctl32 manifest and fakedll.c failed (browser inaccessible, shell DNS); no content came from those attempts. The initial comctl32 guess used dlls/comctl32; the verified resource is dlls/comctl32_v6.

Upstream inspection was limited to resource-list filenames, WINE_MANIFEST resource lines from dlls/*/*.rc, the following manifests, and fakedll.c's assembly registration/naming sections. The resource scan showed exactly these ten assembly manifests:

- dlls/comctl32_v6/comctl32.manifest: Common Controls identity, version/token, comctl32.dll name and28 window classes.
- dlls/msvcr80/msvcr80.manifest and dlls/msvcr90/msvcr90.manifest: CRT identities/versions/tokens and three files each.
- dlls/atl80/atl80.manifest and dlls/atl90/atl90.manifest: ATL identities/versions/tokens/files.
- dlls/gdiplus/gdiplus.manifest and dlls/gdiplus/gdiplus11.manifest: both GdiPlus versions and identity/file.
- dlls/msxml3/msxml3.manifest, dlls/msxml4/msxml4.manifest, dlls/msxml6/msxml6.manifest: XML identities/versions/tokens/files.
- dlls/setupapi/fakedll.c: append_manifest_filename, create_winsxs_dll_path, create_manifest and register_manifest context: architecture fixup, absent language=none and naming behavior. No source code was copied from these functions.
- COPYING.LIB: unchanged LGPL2.1 text retained beside the derived metadata.

b_sxs_data.h transcribes only the permitted manifest metadata into an independently designed data table. b_prefix.m independently builds XML and links from that table, with Common Controls' source filename comctl32_v6.dll supplied by the behavior spec. There is no vendored Wine runtime implementation.

No web/project search, original iOS port, forbidden repository/history, track A output/questions, other session memory or non-tool temp checkout was read. The incidental GitHub edit hyperlink in Microsoft's key-code page was encountered during spec01, not used here; see that spec's provenance. No further incidental forbidden material appeared during spec02.

The shared spec03 was read only after spec02's implementation existed, to identify the next spec. Its two reference directories were subsequently authorized read-only by the human user; they are not sources for spec02.

Final source review on2026-10-15 cleared wineserver_ready during server cleanup and before publishing a replacement server as running, avoiding stale readiness during prefix seeding. An already-running start returns success before inspecting a redundant prefix argument. These are source-reviewed lifecycle corrections, not results of executing the bridge.
