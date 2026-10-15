<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

## Design

SteamHost.swift holds the exact launch API and errors. Internal BSteamHostPolicy value helpers validate installer bytes, redirects and launch options, independently testable without Wine/UIKit. A per-download Foundation delegate enforces redirect policy; its only lock protects one Boolean and is released before invoking completions. Ephemeral byte streaming bounds the installer before hashing/atomic writing on a detached worker. No Wine session starts here. Native Steam credentials are never read. The synchronous launch/file checks are small and are callable without UI actor assumptions.

The app owns session orchestration, download exclusion and showing the first-update note. B04-1 records the missing note-return channel: a per-prefix UserDefaults flag, a stored note string and dlog provide the chosen fallback pending the app's display hook. B04-2 records inaccessible canonical Valve option documentation.

| Spec ref | Requirement | File | Function | Status |
|---|---|---|---|---|
| I/root | Exact Windows/prefix folders and regular steam.exe detection | SteamHost.swift | windowsRoot/clientInstalled | done |
| I/prepare 1 | Already-installed error | SteamHost.swift | prepareInstall | done |
| I/prepare 2 | Public installer URL, ephemeral/no cookies/no credentials,60/600time limits, constrained HTTPS redirects | SteamHost.swift | prepareInstall | done |
| I/prepare 3 | HTTP200/512KiB..64MiB/MZ+PE, verbal progress | SteamHost.swift | prepareInstall | done |
| I/prepare 4 | Create Public Downloads; atomic installer write; size+SHA256 log | SteamHost.swift | prepareInstall | done |
| I/prepare 5 | Exact silent installer launch and empty extra environment | SteamHost.swift | prepareInstall | done |
| I/finish | Delete installer; verify client; failed-install hint | SteamHost.swift | finishInstall | done |
| I/first run | Per-install first client/play launch update note and UserDefaults flag | SteamHost.swift | BSteamHostPolicy.note | question |
| L/common executable | Exact exe and windowsRoot workdir | SteamHost.swift | commonLaunch | done |
| L/common args | no-cef-sandbox; conditional two GPU flags; UserDefaults steamhost.gpu; verify docs; no additional flags | SteamHost.swift | BSteamHostPolicy.arguments | question |
| L/common environment | Only MADEIRA_DOCK_SESSION1; no native sign-in credentials | SteamHost.swift | commonLaunch | done |
| L/open | Common arguments only | SteamHost.swift | openClientLaunch | done |
| L/play ID | 1...4294967294 Int bounds | SteamHost.swift | playLaunch | done |
| L/play record | Use spec03 managed library path and buildID nonnil | SteamHost.swift | playLaunch | done |
| L/play args | Append -applaunch ID; no game args | SteamHost.swift | playLaunch | done |
| S/1 | Document app must gracefully close native CM before launch | SteamHost.swift, TESTS.md | integration comments | done |
| S/2 | Document client+game one Wine session; menu forcequit/clientexit ends | SteamHost.swift, TESTS.md | integration comments | done |
| S/3 | Document app blocks downloads during this session | SteamHost.swift, TESTS.md | integration comments | done |
| E/alreadyInstalled | Distinct error with plain description | SteamHost.swift | SteamHostError.alreadyInstalled | done |
| E/downloadFailed(reason) | Distinct error with plain description | SteamHost.swift | SteamHostError.downloadFailed(reason) | done |
| E/installationDidNotFinish | Distinct error with plain description | SteamHost.swift | SteamHostError.installationDidNotFinish | done |
| E/invalidGame | Distinct error with plain description | SteamHost.swift | SteamHostError.invalidGame | done |
| E/notInstalledForClient | Distinct error with plain description | SteamHost.swift | SteamHostError.notInstalledForClient | done |
| E/prefixMissing | Distinct error with plain description | SteamHost.swift | SteamHostError.prefixMissing | done |
| API/SteamHostLaunch.exe:String | Exact declaration | SteamHost.swift | SteamHostLaunch.exe:String | done |
| API/SteamHostLaunch.arguments:String | Exact declaration | SteamHost.swift | SteamHostLaunch.arguments:String | done |
| API/SteamHostLaunch.workingFolder:String | Exact declaration | SteamHost.swift | SteamHostLaunch.workingFolder:String | done |
| API/SteamHostLaunch.environment:[String:String] | Exact declaration | SteamHost.swift | SteamHostLaunch.environment:[String:String] | done |
| API/SteamHostLaunch:Equatable,Sendable | Exact declaration | SteamHost.swift | SteamHostLaunch:Equatable,Sendable | done |
| API/SteamHost.windowsRoot | Exact declaration | SteamHost.swift | SteamHost.windowsRoot | done |
| API/SteamHost.clientInstalled(prefix:) | Exact declaration | SteamHost.swift | SteamHost.clientInstalled(prefix:) | done |
| API/SteamHost.prepareInstall(prefix:progress:) | Exact declaration | SteamHost.swift | SteamHost.prepareInstall(prefix:progress:) | done |
| API/SteamHost.finishInstall(prefix:) | Exact declaration | SteamHost.swift | SteamHost.finishInstall(prefix:) | done |
| API/SteamHost.playLaunch(appID:prefix:) | Exact declaration | SteamHost.swift | SteamHost.playLaunch(appID:prefix:) | done |
| API/SteamHost.openClientLaunch(prefix:) | Exact declaration | SteamHost.swift | SteamHost.openClientLaunch(prefix:) | done |
| API/SteamHostError:LocalizedError,Equatable | Exact declaration | SteamHost.swift | SteamHostError:LocalizedError,Equatable | done |
| Done/tests | XCTest launch/record/PE/redirect; install/selfupdate/signin/play/client survives game | tests/, TESTS.md | tests | done |
| Process | MIT headers/provenance/questions/review/commit; no compilation claim | PROVENANCE.md | review | done |
