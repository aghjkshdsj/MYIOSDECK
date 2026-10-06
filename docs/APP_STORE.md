# Could MYIOSDECK go on the App Store?

Not in its current form. The three blockers, and what each would take:

## 1. JIT

iOS lets a process execute code it generated only while a debugger is attached (that is what
StikDebug provides). App Store apps cannot get that: the `dynamic-codesigning` entitlement is
reserved for Apple and, in the EU, for alternative browser engines (BrowserEngineKit), not for
emulators.

Without JIT, x86 code would have to be **interpreted**. FEX has no interpreter today, and any
interpreter is roughly 10–50× slower than a JIT: fine for 1990s games, not for Steam games.

A middle path that may be worth building later: an **ahead-of-time (AOT) cache**. Translate a
game's x86 code to ARM64 on a Mac or PC, ship the ARM64 code as part of a signed bundle, and fall
back to an interpreter for code that was not pre-translated. App Store rule 2.5.2 forbids
downloading executable code, so the translated code would have to be inside the app itself,
which rules out arbitrary user-owned games.

## 2. Executable content

Guideline 2.5.2 says apps may not download or run code that changes their features. Apple has
allowed retro-game emulators since 2024, and PC emulators (UTM SE) were blocked from the
worldwide App Store. Running Steam's Windows client and PC games falls on the blocked side today.

## 3. Licensing

MYIOSDECK builds on GPL code (Madeira's FEX/Wine/DXMT changes, DroidDeck's design). The App
Store's terms add restrictions the GPL forbids, which is why GPL apps (VLC, for example) were
removed in the past. A store release would need every GPL copyright holder's permission, or a
rewrite of those parts.

## What this means

Sideloading (iloader, SideStore, AltStore) with StikDebug is the way to run MYIOSDECK at full
speed. In the EU, alternative marketplaces avoid blocker 2 but not blocker 1.
