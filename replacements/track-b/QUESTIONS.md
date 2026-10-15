# Track B questions for the spec writer

One section per spec (`## Spec NN (name)`). Each entry: the part, the question, why it matters,
and `Chosen:` with what the code does now. Answers come back as edits to the shared specs.

## Spec 01 (input)

### B01-1 — B/hardware and controller key ownership

Question: when physical and keyboard-mode controller sources hold the same VK/button, should releasing one release it even while the other still holds it? This affects mixed-input play; per-VK clarification22 describes multiple HID usages but not separate routes.

Chosen: maintain separate physical/controller ownership and publish the union. An UP is sent only when both owners release. Within hardware input, clarification22's exact per-VK semantics apply. Touch holds remain independent.

Resolved by shared spec01 clarification27 (reread2026-10-15); chosen behavior matches.

### B01-2 — B/text while controller holds a key

Question: should on-screen text pulse a VK already held by keyboard-mode controller input? An unconditional UP could end the controller hold.

Chosen: skip a transient text pulse for an already held VK; preserve held Shift. This does not affect normal text entry or the specified physical-keyboard suppression.

Resolved by shared spec01 clarification28 (reread2026-10-15); chosen behavior matches.

## Spec 02 (Wine bridge)

### B02-1 — C/launch17 versus wine_bridge.h running lifetime

Question: numbered step17 clears running before server stop, while the interface explicitly says running lasts until the launch thread has finished (server stopped). Which lifetime should callers rely on? An early zero can allow the next launch to overlap server teardown.

Chosen: keep running true through server stop and cleanup, then release-store zero immediately before the launch thread returns, following the interface's stronger guarantee.

Resolved by shared spec02 clarification1 and revised steps17–18 (reread2026-10-15).

### B02-2 — C/start socket ownership on pthread creation failure

Question: does wineserver_inject_client_fd take ownership immediately, or duplicate the fd? The mandated inject-before-create sequence and close-both-on-create-failure rule can otherwise close an fd the server already consumed or reused.

Chosen: follow the specified order and close both original descriptors on thread-creation failure. On successful handoff, the server owns its end and Wine owns the client end from __wine_main onward. Before entering Wine, cleanup closes an unused client end. This needs the unix-side ownership guarantee clarified.

### B02-3 — E/unknown-position surfaces

Question: surface handling says keep an image on a hidden layer until position arrives, but the draw tests say remove the layer when position is unknown. The displayed result agrees, but object lifetime differs.

Chosen: retain the CGImage in the window record without an attached CALayer while position is unknown. Create its layer when position arrives and draw tests pass. No image is lost and nothing is visible prematurely.

Resolved by shared spec02 clarification16 (reread2026-10-15).

### B02-4 — E/session reset stored rectangles

Question: does "all layers and stored rects" include the overlay frame and desktop mapping, or only per-HWND rectangles? This affects whether the app should republish layout after reset.

Chosen: reset both overlay mapping flags/rectangles and all per-window rectangles. A new session starts from window bounds and aspect fit until the app supplies new layout.

### B02-5 — H/tar template root name

Question: does "strip a leading prefix/ component" name the literal component `prefix`, or mean any archive top folder?

Chosen: after the shared spec02 clarification11, drop a leading `./`, then the literal `prefix/`. Preserve archives already rooted at drive_c; reject absolute paths, any dotdot component, backslashes and colons. Extraction also refuses to traverse existing filesystem symlinks. Resolved by clarification11 (reread2026-10-15).

### B02-6 — C/working folder with non-C Windows paths

Question: full Windows targets may carry another drive or be drive-relative, but prefix seeding provides only c: and the specified POSIX translation uses drive_c. Should those targets have a working-folder mapping?

Chosen: preserve their target path verbatim, report PE type unknown, and leave working folder unchanged unless a valid rooted C override is supplied. Rooted C targets use their containing folder, including legitimate relative parent components in the supplied target.

Resolved by shared spec02 clarification4 (reread2026-10-15).

### B02-7 — C/arguments cap and unmatched quotes

Question: is the64-argument cap for MADEIRA_ARGS alone or the complete argv including wine/path, and should an unmatched quote reject a launch?

Chosen: accept at most64 user arguments (argc at most66); unmatched quote groups through end of string. Empty quotes form an empty argument. No escaping beyond the specified double-quote grouping is inferred.

### B02-8 — E/disabled overlay and picture query

Question: when GAME_WINDOWS=0, should client rectangles still be collected for picture queries, or is the whole window subsystem disabled?

Chosen: after shared spec02 clarification7, record window frames and answer picture queries while disabled. Do not create overlay views/layers or copy surface pixels. Hide an existing overlay when checked on main. Metal marking remains available to presentation hooks. Resolved by clarification7 (reread2026-10-15).

### B02-9 — C/derived environment between sessions

Question: besides the eleven explicitly cleared per-launch settings, may internally generated INITIAL_CWD, CA_BUNDLE, JIT_WRITE_OFFSET and WINESERVERSOCKET be cleared? Otherwise a later bare or non-JIT launch can inherit stale values.

Chosen: clear those four internally generated variables during teardown, and consume/unset WORKDIR on every outcome. App pass-through settings outside the spec's cleanup list remain untouched.

## Spec 03 (Steam client)

### B03-1 — S1 raw protobuf response selection

Question: should requests explicitly include `format=protobuf` to select the specified raw response, in addition to input_protobuf_encoded? The transport section specifies raw protobuf but omits a response-format selector.

Chosen: send `format=protobuf` in the query for both GET and POST. The POST form body contains only input_protobuf_encoded; the RSA GET includes that field in the query too. Parsing never assumes a JSON response is protobuf.

### B03-2 — C4 missing stored sign-in

Question: what should CM logon do when credentialsForDock() is nil? The specified logon fields require the refresh token, and no anonymous logon behavior is described.

Chosen: fail with logonDenied(5), whose user message asks to sign in again. Do not invent anonymous-session behavior.

### B03-3 — D6 educational decoder error behavior

Question: the permitted unchanged educational Zstd source calls exit(1) on malformed input. Is an external compilation wrapper acceptable to contain this without editing the vendor files?

Chosen: keep the vendor files byte-identical. The MIT wrapper includes the source with macro redirections: exit becomes a thread-local longjmp into the wrapper, allocation/free are tracked with a256MB scratch cap and65536-allocation cap, all scratch is freed after success/error, and vendor diagnostics are suppressed. No app-process exit is possible through that error path. No source from another decoder is consulted.

### B03-4 — L4 chains and D3 content-owner identity

Question: for depotfromapp chains, should requests and SharedDepots name the original owner or the final metadata supplier?

Chosen: preserve the depotfromapp owner of the root app's depot as D3 specifies, while iteratively borrowing manifests/sizes/missing config fields through the chain. Record metadata for every visited owner. Fetch only shared references needed by the chosen depot architecture, with the32-app/1024-reference limits.

### B03-5 — D5/D6 files shared by two selected depots

Question: if selected depots contain the same relative filename, which content wins? Parallel writes could otherwise make the result nondeterministic.

Chosen: process depots in sorted ID order, with up to8 concurrent chunks inside one depot. Reapply each depot's file lengths before its chunks. The later depot wins an overlapping filename. Directory spelling is still folded across the whole install.

### B03-6 — S2/S3 untrusted polling interval limits

Question: must arbitrary positive polling intervals be honored, including pathological values from the network?

Chosen: use the reply's finite positive interval bounded to0.05...60s; absent/non-finite/non-positive means5s. Overall password/QR deadlines remain300/600s, including the final poll request.

### B03-7 — D7 progress at phase transitions

Question: does the0.25s callback limit also apply to preparing/downloading/finishing transitions?

Chosen: yes. Preserve phase order and wait at most the remaining quarter-second before publishing a new phase. Chunk callbacks are emitted only after the same interval. Resumed compressed bytes count as done but do not inflate current-run transfer speed.

## Spec 04 (Steam client host)

### B04-1 — I/first-update note has no API slot

Question: how should the app retrieve the first-update note? steam_host_api.md returns only SteamHostLaunch's four fixed fields and provides no text callback or note property on openClientLaunch/playLaunch. Changing that fixed interface would violate the contract.

Chosen: after successful finishInstall, set a per-prefix UserDefaults flag. On the first successful client/play launch builder, clear that flag, log the exact note with dlog and store the text at `steamhost.noticeText` for the app to show. The app must provide the display hook; no new public API, launch field or extra environment variable is invented. The chosen note is in BSteamHostPolicy.firstUpdateNote.

### B04-2 — L/common option verification blocked by Valve documentation access

Question: can the spec writer supply behavior-only confirmation of the three required CEF option names and descriptions? The canonical Valve command-line and browser-protocol pages returned403/inaccessible, including raw/printable variants. Official-domain search retrieved an unrelated Valve community subpage with -applaunch, but did not independently establish the three CEF flags.

Chosen: use only the exact three CEF options prescribed by the spec, plus its required -applaunch for games. Add no other option and mark external verification pending in TESTS.md. Do not consult other launchers or any prohibited project.
