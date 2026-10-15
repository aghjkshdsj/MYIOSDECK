<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

Author: GPT-6.1 Sol, clean side, track B. Date:2026-10-15. Independently designed from the behavior specification and fixed API. All files authored for this spec carry the requested MIT header. No upstream code was copied or vendored for spec04.

| Source | Use | Date |
|---|---|---|
| User's attached track B request | Isolation boundaries, numbered workflow, ownership headers, tests and commit footer | 2026-10-15 |
| Shared CLAUDE.md | Clean-room rules with the user's track B overrides | 2026-10-15 |
| Shared specs/04-steam-client-host.md, all sections | Installer constraints, launch descriptors, errors, app-owned session rules and required tests | 2026-10-15 |
| Shared interfaces/steam_host_api.md, full file | Exact types, names and function signatures; allowed app helpers | 2026-10-15 |
| Shared interfaces/app-api.md, full file | dlog and app integration context | 2026-10-15 |
| Shared interfaces/steam_api.md, file-helper API previously read for spec03 | Managed library paths and install-record lookup | 2026-10-15 |
| Track B's own spec03 SteamFiles.swift | Reuse the independently written file-helper implementation, not another track | 2026-10-15 |
| [Valve command-line options](https://developer.valvesoftware.com/wiki/Command_line_options) and [browser protocol](https://developer.valvesoftware.com/wiki/Steam_browser_protocol) | Requested option verification; both returned403/inaccessible. Raw and printable attempts also failed. No option claim is independently verified from those pages | 2026-10-15 |
| [Valve community command-line subpage](https://developer.valvesoftware.com/wiki/CHILLMODEA/Pages/Command_line_options) | Returned by official-domain search; contained -applaunch examples but did not establish the three required CEF flags. Not used as a source of implementation code or additional options | 2026-10-15 |

Valve's public SteamSetup.exe URL is a string from the specification. The installer was not downloaded, inspected or bundled here. The three required CEF flags are retained exactly from the specification while B04-2 records the documentation gap. B04-1 records the first-update text channel missing from the fixed interface; the chosen fallback adds no public function or launch field.

Source review checked API declarations, import restrictions, bounded byte reads, PE offset bounds, exact launch arguments/environment, lock release before delegate completion, error categories and the test fixtures. No compilation or execution is claimed.

No forbidden material appeared incidentally while implementing spec04. Prior incidents involving hyperlink strings only are recorded in spec01/spec03 provenance; those links were not followed or used.
