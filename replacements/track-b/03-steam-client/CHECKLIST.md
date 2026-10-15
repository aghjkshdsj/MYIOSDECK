<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

## Design

SteamTypes.swift defines API values, errors, logging and an internal CM protocol for test doubles. SteamWire.swift owns independently written bounded protobuf/binary-KeyValues/envelope parsing and resolve-once timeouts. SteamFiles.swift owns text KeyValues, safe disk helpers and launch choice. SteamSignIn.swift owns HTTPS auth, RSA, Keychain and an actor per sign-in flow; SteamSignInView.swift holds main-actor model and SwiftUI. SteamSession.swift confines websocket lifecycle, jobs, license capture and timers to main. SteamLibrary.swift turns PICS data into values and iteratively resolves shared owners. SteamManifest.swift parses bounded file/chunk mappings. SteamInstallRecord.swift writes the exact ACF layout. SteamInstallWorker.swift anchors disk operations and journals to directory descriptors. SteamDepot.swift coordinates installation on main with that separate worker actor for filesystem/crypto/resume and bounded child tasks. BSteamCodecs.c/.h wraps zlib/liblzma/CommonCrypto and the permitted unchanged BSD educational Zstd source with bounded allocations and error containment; codec declarations enter the app bridging header.

No network/body/credential values enter logs. Main-actor classes publish UI state; CPU/file work uses detached tasks or the install worker. The only custom lock is the small resolve-once promise, with continuations resumed after unlock. Codec scratch ownership is thread-local per synchronous decode and released on all outcomes. All transport/job/request counts and payloads have explicit caps. A protocol double drives install tests without a live Steam account.

| Spec ref | Requirement | File | Function | Status |
|---|---|---|---|---|
| Ground/privacy | Never log account identifiers, secrets, bodies | SteamTypes.swift | SteamLog | done |
| Ground/input | Bound network sizes/counts, paths and decoding; no traps | all sources | validation | done |
| Ground/concurrency | Main actor public classes; heavy parsing/crypto/I/O off main | all sources | worker tasks | done |
| Ground/errors | Visible failures use SteamError; cancellation propagates | SteamTypes.swift | SteamError | done |
| S1/1 | Exact HTTPS method URLs and protobuf/base64/alphanumeric-only form encoding | SteamSignIn.swift | BSteamAuth | question |
| S1/2 | GET RSA; POST others Content-Type | SteamSignIn.swift | BSteamAuth | done |
| S1/3 | 20s, no cache, raw response | SteamSignIn.swift | BSteamAuth | done |
| S1/4 | x-eresult mapping for credentials/Guard/rate/disabled/expired/other | SteamSignIn.swift | BSteamAuth | done |
| S1/5 | HTTP429/other status/no-response messages | SteamSignIn.swift | BSteamAuth | done |
| S2/1 | RSA key request/empty modulus credentials | SteamSignIn.swift | BSteamAuthFlow | done |
| S2/2 | PKCS1 DER integers/sign octet/SecKey encryption/base64 | SteamSignIn.swift | BSteamAuthFlow | done |
| S2/3 | Credentials request platform1/persistence1/device/name/timestamp | SteamSignIn.swift | BSteamAuthFlow | done |
| S2/4 | Confirmations code preference3 then2/email hint, approvals4/5, both visible | SteamSignIn.swift | BSteamAuthFlow | done |
| S2/5 | Trim/uppercase code and selected type; polling continues | SteamSignIn.swift | BSteamAuthFlow | done |
| S2/6 | Polling interval/new client/both tokens/name fallback/5min/cancel | SteamSignIn.swift | BSteamAuthFlow | question |
| S3/1 | QR begin platform1/friendly URL nonempty | SteamSignIn.swift, SteamSignInView.swift | QR flow | done |
| S3/2 | CoreImage M crisp CGImage-backed UIImage and same-device URL | SteamSignIn.swift, SteamSignInView.swift | QR flow | done |
| S3/3 | Poll updates URL; ~10min deadline/QR-expired | SteamSignIn.swift, SteamSignInView.swift | QR flow | question |
| S4/1 | One JSON generic-password item; all five values/accessibility/replace | SteamSignIn.swift | BSteamTokenStore | done |
| S4/2 | Import legacy service/account JSON date2001 once, delete only after copy | SteamSignIn.swift | BSteamTokenStore | done |
| S4/3 | Readback refresh-token verification and specific save error | SteamSignIn.swift | BSteamTokenStore | done |
| S4/4 | Trim name, empty rejection, bounded JWT numeric exp check; otherwise opaque | SteamSignIn.swift | BSteamTokenStore | done |
| S4/5 | Main didChange every store/signout | SteamSignIn.swift | BSteamTokenStore | done |
| S4/6 | Signout deletes/logs/posts | SteamSignIn.swift | BSteamTokenStore | done |
| S4/7 | Method-only success/failure reason logs | SteamSignIn.swift | BSteamTokenStore | done |
| S5/1 | NavigationStack/Form/title/Cancel-or-Done | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/2 | Signedin account/signout/destructive note/auto-close completion | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/3 | Segmented password/QR; iPadQR/iPhonepassword | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/4 | Username traits/password/enabled/clear/footer | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/5 | Guard code traits>=5/approval spinner/startover | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/6 | QR white/rounded/instructions/link/wait/retry | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/7 | Red errors and privacy footer | SteamSignInView.swift | SteamSignInModel/View | done |
| S5/8 | Switch/dismiss cancels; one operation, account mirror, signout cancels | SteamSignInView.swift | SteamSignInModel/View | done |
| C1/1 | Directory URL/schema,1h list and last-good cache | SteamSession.swift | directory | done |
| C1/2 | Least-recent healthy server,3fails/reset/success tracking | SteamSession.swift | directory | done |
| C2/1 | wss cmsocket protocols steamdataport | SteamSession.swift | transport | done |
| C2/2 | Connected only ping roundtrip | SteamSession.swift | transport | done |
| C2/3 | Binary messages/continuous receive/text ignored/errors close incl57 | SteamSession.swift | transport | done |
| C3/1 | LE proto envelope and header fields | SteamWire.swift | BSteamMessage | done |
| C3/2 | Old extended36/simple20 offsets | SteamWire.swift | BSteamMessage | done |
| C3/3 | All prescribed message IDs | SteamWire.swift | BSteamMessage | done |
| C4/1 | Register license capture before send | SteamSession.swift | connect | done |
| C4/2 | Hello65580/default steamID/session0 | SteamSession.swift | connect | done |
| C4/3 | Logon all fields, refresh token, IP hostorder xor | SteamSession.swift | connect | question |
| C4/4 | 20s response, 5515/751/loggedoff race, status1,48 retry twice, other denial/signinagain | SteamSession.swift | connect | done |
| C4/5 | Single coalesced connect,20s wait, explicit resolve-once | SteamSession.swift | connect | done |
| C5/1 | Fresh job/source ID, target match | SteamSession.swift, SteamWire.swift | requests | done |
| C5/2 | Per-request10s timeout and disconnect failure | SteamSession.swift, SteamWire.swift | requests | done |
| C5/3 | Multipart8904 until pendingfalse; partial timeout allowed | SteamSession.swift, SteamWire.swift | requests | done |
| C5/4 | 151 service target name #1/147/header result trace | SteamSession.swift, SteamWire.swift | requests | done |
| C5/5 | Multi gzip optionalheaders,declared/256MB cap,LE message lengths | SteamSession.swift, SteamWire.swift | requests | done |
| C5/6 | Unsolicited loggedoff closes; rest ignored | SteamSession.swift, SteamWire.swift | requests | done |
| C6/1 | Heartbeat interval/default30 while loggedon | SteamSession.swift | lifecycle | done |
| C6/2 | Idle60s after lastrequest logoff; next request reconnects | SteamSession.swift | lifecycle | done |
| C6/3 | Loss backoff1/2/4/8/16 five attempts | SteamSession.swift | lifecycle | done |
| C6/4 | Best-effort logoff sessionheader, failjobs | SteamSession.swift | lifecycle | done |
| C6/5 | Graceful await logoff send and socketclose; remain disconnected | SteamSession.swift | lifecycle | done |
| C7 | Perconnection license reset/capture/wait15s | SteamSession.swift | licenses | done |
| L1/1 | Ensurelogon/license | SteamLibrary.swift | fetchOwnedApps | done |
| L1/2 | Package access tokens30s | SteamLibrary.swift | fetchOwnedApps | done |
| L1/3 | Packages50/binaryKV collect appids | SteamLibrary.swift | fetchOwnedApps | done |
| L1/4 | App tokens/appinfo50/textKV | SteamLibrary.swift | fetchOwnedApps | done |
| L1/5 | Playable game/demo/application | SteamLibrary.swift | fetchOwnedApps | done |
| L1/binary | Recursive types0/1/2/8,NUL strings,LE32,boundeddepth/bytes | SteamWire.swift | BSteamBinaryKV | done |
| L2 | Text quoted/bare/braces/comments, quote/backslash escapes and literalother backslashes | SteamFiles.swift | SteamKeyValues | done |
| L3/1 | Top/ID/appinfo case-insensitive wrapper/name/type/os/free | SteamLibrary.swift | BSteamAppParser | done |
| L3/2 | Artwork language priority and256byte/no-dotdot/no-leading-slash | SteamLibrary.swift | BSteamAppParser | done |
| L3/3 | Parent excludes0/self | SteamLibrary.swift | BSteamAppParser | done |
| L3/4 | Install folder and launch options | SteamLibrary.swift | BSteamAppParser | done |
| L3/5 | Numericdepots/all metadata/branch oldgid/newgid/size/download | SteamLibrary.swift | BSteamAppParser | done |
| L3/6 | Public buildID | SteamLibrary.swift | BSteamAppParser | done |
| L3/7 | installable Windows/playable/nonempty depotselection | SteamLibrary.swift | BSteamAppParser | done |
| L4 | Fetchone; sharedowner iterative <=32apps/1024refs, copy metadata, fail unresolved, record owners,cancel | SteamLibrary.swift | fetchInstallInfo | question |
| L5 | Numeric stableorder<=32,required exe,all field limits/index/DLC | SteamLibrary.swift | launches | done |
| L6 | Package depotids cached for fetcher lifetime | SteamLibrary.swift | ownedDepotIDs | done |
| D1/1 | Windows/nonDLC/nonshared/public/nonlowviolence/arch/language | SteamLibrary.swift | BSteamDepotSelection | done |
| D1/2 | Sorted IDs;32fallback only no64selected;noWindows error; exclusion reasons | SteamLibrary.swift | BSteamDepotSelection | done |
| D2 | Safe common folder/resume downloading appid/delete after success | SteamDepot.swift | install | done |
| D3/1 | Depotkey5438/15s/result1/key32/cache | SteamDepot.swift | metadata | done |
| D3/2 | Refused+nonemptyownedwithoutit skip;otherwise fail;allskipfail | SteamDepot.swift | metadata | done |
| D3/3 | Servers URL/order/vhost/HTTPS/proxy/allowedapp/DNS suffix validation/fallback/countlogs | SteamDepot.swift | metadata | done |
| D3/4 | Owner content app unlessfree | SteamDepot.swift | metadata | done |
| D3/5 | Manifest request service fields/fixed64orvarint/zero none | SteamDepot.swift | metadata | done |
| D3/6 | CDNauth perhost/depot/app/cache/appendform/failure none | SteamDepot.swift | metadata | done |
| D4/1 | Manifest URL/gid/code/auth/up to6hosts | SteamDepot.swift | BSteamManifest | done |
| D4/2 | Single ZIP stored/rawdeflate/size bound | SteamDepot.swift | BSteamManifest | done |
| D4/3 | Protobuf directtag or8byte lengthheader/trailer;encrypted fallback | SteamDepot.swift | BSteamManifest | done |
| D4/4 | Exact file/chunk field mapping | SteamDepot.swift | BSteamManifest | done |
| D4/5 | AES/base64 filename UTF8/control validation/trailingNUL/slash normalize | SteamDepot.swift | BSteamManifest | done |
| D4/6 | Directory/symlink/custom executable flags | SteamDepot.swift | BSteamManifest | done |
| D4/7 | Custom depotcache manifest atomic only size differs | SteamDepot.swift | BSteamManifest | done |
| D5/1 | Filepaths<=64parts/1024bytes/noempty/dot/dotdot/colon/control | SteamDepot.swift | BSteamInstallWorker | done |
| D5/2 | Directory casefold first spelling | SteamDepot.swift | BSteamInstallWorker | done |
| D5/3 | Createdirs/open/create/setlength preserveexisting | SteamDepot.swift | BSteamInstallWorker | question |
| D5/4 | Free-space sumremaininggrowth/ImportantUsagecapacity/specificerror | SteamDepot.swift | BSteamInstallWorker | done |
| D6/1 | Lowercase sha chunk URL | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/2 | AES256 ECB-IV then CBC PKCS7 | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/3 | VSZa frame8..<len-15 | SteamDepot.swift, BSteamCodecs.c | chunks | question |
| D6/4 | VZ props7..<12/stream12..<len-10/u32len-6/rawLZMA1 | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/5 | ZIP stored/deflate/datadescriptor expectedsize | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/6 | CompressionLZMA fallback/stored exact/formaterror | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/7 | 64MB limit/expectedsize/Adler seed0 checksum | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/8 | pwrite offset offmain | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/9 | Existingranges SHA1 skip | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/10 | Max8inflight/5tries/6hosts/delay0.4attempt/rotation/globalhealth/logcap8 | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D6/11 | Dedicated ephemeral session30s/300s/8connections | SteamDepot.swift, BSteamCodecs.c | chunks | done |
| D7/1 | Durable per-depot-manifest records/torn-tail safety/stale removal | SteamDepot.swift | resume/progress | done |
| D7/2 | Cancellation betweenchunks preservesfinishedwork | SteamDepot.swift | resume/progress | done |
| D7/3 | Preparing/downloading/finishing/compressedresumed/runspeed/0.25s throttle | SteamDepot.swift | resume/progress | question |
| D8/1 | Exact AppState ordered keys/quotes/tabs/backslash escapes/zero counters | SteamDepot.swift | BSteamInstallRecord | done |
| D8/2 | Own InstalledDepots/shared sorted/customCheckGuid<=256 omit empty | SteamDepot.swift | BSteamInstallRecord | done |
| D8/3 | Shared owner same safe folder merge existing depots/name/build/size/counts | SteamDepot.swift | BSteamInstallRecord | done |
| D8/4 | Recordlast/atomic/failonwrite/begin-complete logs | SteamDepot.swift | BSteamInstallRecord | done |
| K1 | Value fields/string/casefoldsubscript;BOM/input4MB/tokens100k/value16384/depth32/errors | SteamFiles.swift | SteamValue/SteamKeyValues | done |
| K2/1 | Exact libraryrelative/appendpaths/caseinsensitive managed | SteamFiles.swift | SteamInstallPaths/Files | done |
| K2/2 | Record<=1MB/AppState/appidmatch/build/positivesize | SteamFiles.swift | SteamInstallPaths/Files | done |
| K2/3 | Safe lastfolder component/defaultapp | SteamFiles.swift | SteamInstallPaths/Files | done |
| K2/4 | Delete confined resolved directchild/<=64shared-owner records/samefolder/ownrecord/resume | SteamFiles.swift | SteamInstallPaths/Files | done |
| K3/1 | Candidate beta/type/DLC/Windows filters | SteamFiles.swift | SteamDirectStart | done |
| K3/2 | Typepriority/archpriority/stableties | SteamFiles.swift | SteamDirectStart | done |
| K3/3 | Safe exe relative/name-by-name exactorcasefold/file/symlinkcontained;workdirsamecheck | SteamFiles.swift | SteamDirectStart | done |
| K3/4 | Returnactualspelling/trimargs/folder/index/modegame | SteamFiles.swift | SteamDirectStart | done |
| E/cases | All distinguishable errors and plain descriptions | SteamTypes.swift | SteamError | done |
| E/message | SteamError/URLError connectivity-timeout-code/otherlocalized | SteamSignIn.swift | message | done |
| E/reason | Stable privacy-safe tags/types | SteamSignIn.swift | reason | done |
| Done/tests | All named XCTest areas/scriptedwholepipeline/devicechecks | tests/, TESTS.md | tests | done |
| Process | Provenance including authorized refs/BSDcodec/header/review/questions/commits | documentation | review | done |
| API/SteamSignIn.didChange | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.didChange | done |
| API/SteamSignIn.flag(_:default:) | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.flag(_:default:) | done |
| API/SteamSignIn.isEnabled | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.isEnabled | done |
| API/SteamSignIn.isSignedIn | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.isSignedIn | done |
| API/SteamSignIn.accountName | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.accountName | done |
| API/SteamSignIn.credentialsForDock() | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.credentialsForDock() | done |
| API/SteamSignIn.signOut() | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.signOut() | done |
| API/SteamSignIn.message(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.message(_:) | done |
| API/SteamSignIn.reason(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamSignIn.reason(_:) | done |
| API/SteamSignInModel.shared | Exact name/type/actor/Codable contract | Swift sources | SteamSignInModel.shared | done |
| API/SteamSignInModel.accountName | Exact name/type/actor/Codable contract | Swift sources | SteamSignInModel.accountName | done |
| API/SteamSignInModel.signOut() | Exact name/type/actor/Codable contract | Swift sources | SteamSignInModel.signOut() | done |
| API/SteamSignInView.init() | Exact name/type/actor/Codable contract | Swift sources | SteamSignInView.init() | done |
| API/SteamLog.event(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamLog.event(_:) | done |
| API/SteamLog.trace(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamLog.trace(_:) | done |
| API/SteamSession.init() | Exact name/type/actor/Codable contract | Swift sources | SteamSession.init() | done |
| API/SteamSession.logoff() | Exact name/type/actor/Codable contract | Swift sources | SteamSession.logoff() | done |
| API/SteamSession.disconnectGracefully() | Exact name/type/actor/Codable contract | Swift sources | SteamSession.disconnectGracefully() | done |
| API/SteamLibraryFetcher.init(session:) | Exact name/type/actor/Codable contract | Swift sources | SteamLibraryFetcher.init(session:) | done |
| API/SteamLibraryFetcher.fetchOwnedApps() | Exact name/type/actor/Codable contract | Swift sources | SteamLibraryFetcher.fetchOwnedApps() | done |
| API/SteamLibraryFetcher.fetchInstallInfo(appID:) | Exact name/type/actor/Codable contract | Swift sources | SteamLibraryFetcher.fetchInstallInfo(appID:) | done |
| API/SteamLibraryFetcher.ownedDepotIDs() | Exact name/type/actor/Codable contract | Swift sources | SteamLibraryFetcher.ownedDepotIDs() | done |
| API/SteamAppInfo.appID | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.appID | done |
| API/SteamAppInfo.name | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.name | done |
| API/SteamAppInfo.installDir | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.installDir | done |
| API/SteamAppInfo.buildID | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.buildID | done |
| API/SteamAppInfo.libraryCapsule | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.libraryCapsule | done |
| API/SteamAppInfo.headerImage | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.headerImage | done |
| API/SteamAppInfo.parentID | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.parentID | done |
| API/SteamAppInfo.launches | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.launches | done |
| API/SteamAppInfo.installableOnWindows | Exact name/type/actor/Codable contract | Swift sources | SteamAppInfo.installableOnWindows | done |
| API/SteamLaunchOption.executable (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.executable (Codable key) | done |
| API/SteamLaunchOption.index (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.index (Codable key) | done |
| API/SteamLaunchOption.requiredDLC (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.requiredDLC (Codable key) | done |
| API/SteamLaunchOption.arguments (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.arguments (Codable key) | done |
| API/SteamLaunchOption.workingDir (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.workingDir (Codable key) | done |
| API/SteamLaunchOption.type (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.type (Codable key) | done |
| API/SteamLaunchOption.oslist (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.oslist (Codable key) | done |
| API/SteamLaunchOption.osarch (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.osarch (Codable key) | done |
| API/SteamLaunchOption.betaKey (Codable key) | Exact name/type/actor/Codable contract | Swift sources | SteamLaunchOption.betaKey (Codable key) | done |
| API/DepotDownloader.init(session:) | Exact name/type/actor/Codable contract | Swift sources | DepotDownloader.init(session:) | done |
| API/DepotDownloader.install(_:steamApps:ownedDepots:progress:) | Exact name/type/actor/Codable contract | Swift sources | DepotDownloader.install(_:steamApps:ownedDepots:progress:) | done |
| API/SteamDownloadProgress.Phase | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.Phase | done |
| API/SteamDownloadProgress.phase | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.phase | done |
| API/SteamDownloadProgress.totalBytes | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.totalBytes | done |
| API/SteamDownloadProgress.doneBytes | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.doneBytes | done |
| API/SteamDownloadProgress.bytesPerSecond | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.bytesPerSecond | done |
| API/SteamDownloadProgress.fraction | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.fraction | done |
| API/SteamDownloadProgress.init() | Exact name/type/actor/Codable contract | Swift sources | SteamDownloadProgress.init() | done |
| API/SteamError.appInfoNotFound(UInt32) | Exact name/type/actor/Codable contract | Swift sources | SteamError.appInfoNotFound(UInt32) | done |
| API/SteamValue.text | Exact name/type/actor/Codable contract | Swift sources | SteamValue.text | done |
| API/SteamValue.object | Exact name/type/actor/Codable contract | Swift sources | SteamValue.object | done |
| API/SteamValue.fields | Exact name/type/actor/Codable contract | Swift sources | SteamValue.fields | done |
| API/SteamValue.string | Exact name/type/actor/Codable contract | Swift sources | SteamValue.string | done |
| API/SteamValue.subscript(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamValue.subscript(_:) | done |
| API/SteamFileError.invalid(String) | Exact name/type/actor/Codable contract | Swift sources | SteamFileError.invalid(String) | done |
| API/SteamKeyValues.init(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamKeyValues.init(_:) | done |
| API/SteamKeyValues.read() | Exact name/type/actor/Codable contract | Swift sources | SteamKeyValues.read() | done |
| API/SteamInstallPaths.libraryRelative | Exact name/type/actor/Codable contract | Swift sources | SteamInstallPaths.libraryRelative | done |
| API/SteamInstallPaths.steamApps(drive:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallPaths.steamApps(drive:) | done |
| API/SteamInstallPaths.common(drive:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallPaths.common(drive:) | done |
| API/SteamInstallPaths.isManaged(library:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallPaths.isManaged(library:) | done |
| API/SteamInstallFiles.buildID(appID:steamApps:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallFiles.buildID(appID:steamApps:) | done |
| API/SteamInstallFiles.sizeOnDisk(appID:steamApps:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallFiles.sizeOnDisk(appID:steamApps:) | done |
| API/SteamInstallFiles.delete(appID:folderName:steamApps:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallFiles.delete(appID:folderName:steamApps:) | done |
| API/SteamInstallFiles.safeFolderName(_:) | Exact name/type/actor/Codable contract | Swift sources | SteamInstallFiles.safeFolderName(_:) | done |
| API/SteamDirectStart.mode | Exact name/type/actor/Codable contract | Swift sources | SteamDirectStart.mode | done |
| API/SteamDirectStart.choose(_:installFolder:) | Exact name/type/actor/Codable contract | Swift sources | SteamDirectStart.choose(_:installFolder:) | done |
| API/SteamDirectStart.Choice.program | Exact name/type/actor/Codable contract | Swift sources | SteamDirectStart.Choice.program | done |
| API/SteamDirectStart.Choice.arguments | Exact name/type/actor/Codable contract | Swift sources | SteamDirectStart.Choice.arguments | done |
| API/SteamDirectStart.Choice.folder | Exact name/type/actor/Codable contract | Swift sources | SteamDirectStart.Choice.folder | done |
| API/SteamDirectStart.Choice.launchIndex | Exact name/type/actor/Codable contract | Swift sources | SteamDirectStart.Choice.launchIndex | done |
