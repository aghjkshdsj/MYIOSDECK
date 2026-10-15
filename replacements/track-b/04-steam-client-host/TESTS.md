<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

No compilation, XCTest execution, installer download, Wine launch or device check was performed on this Windows machine. These tests await the owner's Mac/iOS build.

Add SteamHost.swift and tests/SteamHostTests.swift to the iOS17+ XCTest target described in spec03's TESTS.md, which supplies the real spec03 file helpers and their source dependencies. Include tests/HostHarness.swift only in a standalone test target that does not already provide dlog. Keep spec03's standalone SteamHarness.swift only where the app does not supply its declarations. Do not compile either test harness into the app. Run Product > Test on a Mac or `xcodebuild test -project <integration>.xcodeproj -scheme <SteamTests> -destination 'platform=iOS Simulator,name=<installed simulator>'`.

SteamHostTests covers exact executable/workdir/common arguments, string and numeric GPU settings, the environment dictionary, invalid/edge valid app IDs, missing/present/mismatched install records in a temporary prefix, non-PE rejection, a minimal PE header separately from the download-size rule, size bounds, redirect suffix/scheme/user-info rules, installer cleanup, missing-client failure, the per-prefix first-update flag and prepareInstall guards before network access. Fixtures use no real Steam account, installer or game. Tests that change UserDefaults must run serially.

The prescribed command-line options and their intended purposes are:

| Option | Purpose specified for this implementation | Documentation verification |
|---|---|---|
| `-no-cef-sandbox` | Disable sandboxing for Steam's embedded browser under Wine | Exact spelling required by spec04; canonical Valve page inaccessible, B04-2 pending |
| `-cef-disable-gpu` | Disable embedded-browser GPU acceleration unless steamhost.gpu is1 | Exact spelling required by spec04; canonical Valve page inaccessible, B04-2 pending |
| `-cef-disable-gpu-compositing` | Disable embedded-browser GPU compositing with that same setting | Exact spelling required by spec04; canonical Valve page inaccessible, B04-2 pending |
| `-applaunch <appID>` | Ask the running client to launch that installed game | Required by spec04; official-domain community subpage contained examples, canonical-page verification still pending |
| `/S` | Run Valve's downloaded installer silently | Required by spec04; no other installer switches added |

Manual device/integration checks:

1. Start with an existing, seeded prefix without steam.exe. Download over a working connection, inspect verbal progress and size/SHA256 logging, and check the returned silent installer launch and empty extra environment. Verify failure messages for offline, interrupted, HTTP errors, under/oversize and invalid-PE responses with a controlled transport. Confirm HTTPS redirects accept only the two specified domain suffixes, and no cookies or stored credentials are used.
2. Have the app start the returned installer through its ordinary Wine launch path. Once that session ends, call finishInstall. Confirm the downloaded installer is deleted on success and missing-client failure, and the error explains trying again with a working connection. Repeating prepareInstall after success reports already installed.
3. First open after installation: display the supplied BSteamHostPolicy.firstUpdateNote through the app's pending integration hook (B04-1). The current fixed-API fallback consumes a per-prefix UserDefaults flag, logs the note and stores it at steamhost.noticeText. Confirm the app shows the note once and Steam downloads its initial update, a few hundred MB. A later launch must not show a new first-update note. Reinstallation resets the flag.
4. Before every host launch, the app must await SteamSession.disconnectGracefully(). Keep client and game in one Wine session. Block spec03 downloads throughout that session because they write the same library. These three rules are app responsibilities, documented in source; SteamHost does not run or orchestrate a Wine session.
5. Sign in through Valve's own window. Verify none of the app's native sign-in credentials, account identity, refresh/access tokens or private APIs are passed to that client. Confirm the browser UI works with the default GPU flags, then set steamhost.gpu to1 and check the two GPU flags disappear while no-cef-sandbox remains.
6. Download a game with spec03 so its managed install record is present. Use playLaunch to start it through Valve's client. Verify common arguments precede -applaunch and that no game-specific arguments are appended. A missing/malformed/mismatched record must prevent launch with the specified message.
7. Exit only the game: the client remains usable in its existing Wine session. End from the app's game menu: force-quit the entire session. Separately close the client itself: the session ends and downloads may resume. Confirm MADEIRA_DOCK_SESSION suppresses the app launcher's own Steam game identity while Valve's client handles its games.

Review questions: B04-1 and B04-2 in track B QUESTIONS.md. Source inspection is complete; external option verification, executable tests and all device checks remain unperformed.
