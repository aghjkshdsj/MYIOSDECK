# Steam Cloud saves

MYIOSDECK syncs the saves of installed Steam games with the signed-in account's Steam Cloud:
before a game starts its cloud saves come down to where it reads them, and when it ends what it
changed goes up. It uses the app's own Steam connection (SwiftSteam's `SteamSession`, the one the
library and downloads use) and Steam's published Cloud service messages. On by default;
**Settings › Steam › Steam Cloud saves** turns it off.

Code: `App/Sources/Steam/SteamCloudCore.swift` (the decisions, Foundation only, host-tested),
`SteamCloudSync.swift` (transfers, backups, the per-game record, game sessions),
`App/Sources/UI/SteamCloudView.swift` (the game's Steam Cloud section). Ported from Madeira
(`app/Madeira/SteamCloud.swift`, `SteamOwnedLibrary.swift` at 65e6fe8f) with the stricter rules
below. Log tag `[steam-cloud]`: App IDs, counts and results; no account data, paths or file names.

## When it runs

- **Before Play** (with or without Steam): the game's saves sync first. The game starts at once
  when the sync completes with nothing left to choose. Otherwise Play stops and asks:
  - the sync failed (offline, Steam did not answer): **Try again**, **Play without syncing**, Cancel;
  - saves need a choice: **Choose…** (the game's Steam Cloud page), **Play without syncing**, Cancel.
- **When the game ends**, however it ends (its own quit, Quit game, Force quit, a crash, a start
  that failed): the saves it changed go up. "Quit game and close MYIOSDECK" closes the app at
  once; the upload then happens at the next start of MYIOSDECK (the game is recorded as having
  an open session).
- **At the start of MYIOSDECK** (once the Steam library loads): every game with an open session
  or a sync that stopped is synced.
- **On the game's sheet**: the Steam Cloud section compares on opening; **Sync now**.

## What it does

1. Asks Steam for the app's cloud file list (`Cloud.GetAppFileChangelist`, the whole list). A
   request Steam answers with an error is an error, never an empty cloud; an answer with no files
   although saves were synced before, or with an older change number than the last sync, is not
   taken as the cloud's state either (nothing is copied, and it is retried).
2. Maps each cloud path to the Wine prefix (`Documents/wine-prefix/drive_c`) through its `%Root%`
   placeholder and the app's `ufs` product info: `WinMyDocuments` → `users/<user>/Documents`,
   `WinAppDataLocal`/`LocalLow`/`Roaming` → `users/<user>/AppData/...`, `WinSavedGames` →
   `users/<user>/Saved Games`, `GameInstall` → the game's folder, no placeholder → Valve's
   `C:\Program Files (x86)\Steam\userdata\<account>\<app>\remote`. `{64BitSteamID}` and
   `{Steam3AccountID}` are filled in, Windows `rootoverrides` are applied, names match without
   regard to case (Wine's view), and a path that would leave its folder is refused. The user folder
   is the one Wine uses (`$USER`, else the passwd name, else the most recently used).
3. Compares SHA-1 and size, and also lists files matching the app's save rules (`ufs.savefiles`
   patterns, Windows only) and files in the `remote` folder that the cloud does not have.
4. Decides with the record of the last sync (per account, in Application Support/MYIOSDECK/
   steam-cloud/: each file's SHA-1, size and time when it was last identical on both sides, the
   cloud's change number at the last complete sync, and the session flags):

| This device | Steam Cloud | Record | Result |
|---|---|---|---|
| = cloud | | | in sync (recorded) |
| changed | unchanged | synced | **upload** |
| unchanged | changed | synced | **download** |
| changed | changed | synced | **you choose** |
| differs | differs | none (first sync here) | **you choose** |
| empty file | not empty | | **you choose** |
| — (missing) | has it | none | **download** |
| — (missing) | has it | synced, other saves still here | **you choose** (the game may have deleted it) |
| — (missing) | has it | synced, no save of the game left here | **download** (new or reset drive) |
| — (missing) | changed since | synced | **download** |
| has it | — | none | **upload** (new save) |
| has it | — (deleted elsewhere) | synced | kept here, not sent back |

Nothing is ever deleted, on either side. A save gone from this device is restored from the
cloud or, by your choice, left gone here (the cloud keeps it, and it is not asked again until
the cloud's copy changes).

### A session that started without syncing

If Play went ahead without a sync (no network, or **Play without syncing**), the game is marked.
Until a complete sync clears the mark, a save that changed here is uploaded only if the cloud's
change number is still the one of the last complete sync (nothing at all changed in the cloud
since); otherwise every such save is your choice. So a game played offline never overwrites
cloud saves made meanwhile on another device, and an old or empty local state never replaces
newer cloud saves unasked.

## Safety

- **Downloads are all or nothing**: every file is fetched and checked against the SHA-1 in
  Steam's list first; then this device's copies they replace are backed up; then they are written
  (each atomically). A failure before the write phase changes nothing.
- **Uploads are one batch** (`Cloud.BeginAppUploadBatch`): the cloud's copies being replaced are
  downloaded (SHA-1 checked) and backed up first, and without that backup nothing is uploaded;
  every file's parts are sent before any file is committed (`ClientBeginFileUpload`, the HTTP
  parts, then `ClientCommitFileUpload` for each); a failed part ends every begun file as failed
  and the batch as failed (`CompleteAppUploadBatchBlocking`), so nothing is committed. Steam's
  service has no multi-file transaction: only a failure between two commits can leave part of a
  batch in the cloud, and the next sync then sees those files as identical and sends the rest.
  A device file that changed after the comparison stops the upload (it goes next time).
- **Backups**: `Documents/save-backups/<App ID>/<time>/` (Files app: On My iPhone › MYIOSDECK ›
  save-backups), with `device/` (this device's copies a download, a choice or a restore replaced),
  `cloud/` (the cloud's copies an upload replaced; Steam keeps none) and `backup.json` (each file's
  cloud path, side, size, SHA-1 and time). Every backup younger than 60 days stays, and so do the
  newest 10 whatever their age; beyond 30 backups the oldest older ones go.
- **Errors**: a failed request or transfer stops the sync, changes nothing further, is shown on
  the game's page and in the log, and is retried before the next start.

## The game's Steam Cloud section

On the game's sheet: last sync, counts, what the last sync did, problems; each save that needs a
choice with both sides' size and date and **Keep cloud's** / **Keep this iPhone's** (or, for a save
gone here, **Restore from cloud** / **Leave deleted here**), plus "all" buttons; **Sync now**; the
file list; the backups with **Restore…**. Restore puts a backup's files back where the game reads
them (this device's current copies are backed up first, so a restore can be undone); the usual
sync follows, which uploads the restored saves unless the cloud changed meanwhile (then you choose).

## With Steam (Madeira Dock)

Valve's own client inside the Wine session syncs Steam Cloud itself when it starts a game and when
the game ends (its `logs/cloud_log.txt` goes into MYIOSDECK's log after a Dock session). MYIOSDECK
syncs before the host starts (while its own connection is still signed in) and after the session
ends, and makes no Steam request while Valve's client holds the account (a second sign-in would
end the client's). There is no double sync: both compare hashes, so whatever one side already
copied the other finds identical. MYIOSDECK's sync before the start also means Valve's client
finds the saves identical and has no conflict to ask about (it could not show one).

## Limits

- Encrypted cloud files are not supported (the sync stops; nothing is changed).
- New saves under a Windows `rootoverrides` folder are compared and downloaded, not uploaded.
- Games that keep saves outside their `ufs` rules (and outside the `remote` folder) are not synced.

## Tests

`tests/host/run.sh` (CI: build-ipa.yml, job "Host tests") compiles `SteamCloudCore.swift` with
`tests/host/steam_cloud/main.swift` for the CI host: path mapping (roots, overrides, account
placeholders, refused paths, case), the comparison over real files in a temporary prefix, every
row of the table above, the unsynced-session rule, implausible answers, the record's JSON, and
backup retention.
