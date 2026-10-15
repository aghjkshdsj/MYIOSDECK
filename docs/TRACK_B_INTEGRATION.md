# Track B integration

Integration baseline: main f3fd8ed0d87ce1f734ee3c3d688abd63c61b6c8e.
Clean-side package: commit 1ed04c742ad1a1f22750697ad8465aa318a64b6d.
Archive SHA256: 1c05b0eaf64a283746fe7df4cfbb0424cf60b5970cb546ff817f8d7f3002d936.

The four folders under replacements/track-b are the exported independent implementation.
Their original CHECKLIST.md, TESTS.md, PROVENANCE.md and QUESTIONS.md remain intact. The
PACKAGE-SHA256.json manifest records the exact exported bytes before integration edits.
The integration role can inspect the application; the independent authoring checkout and
shared behavior inputs were never edited by this integration. No track A output or history
was consulted. This branch does not claim the whole application or its runtime binaries
have been rewritten or relicensed.

All production Swift/C/Objective-C compilation units directly inside the four numbered
folders are app sources. The decoder vendor C file is included only by BSteamCodecs.c.
No tests or test harnesses belong to the app. tools/audit-track-b.py audits the generated
app source phase and embeds the resulting membership manifest into the IPA. The source
membership and required dynamic exports are hard CI gates.

Excluded old source groups are the two prior input files, Native/wine_host.c, the entire
Steam/SwiftSteam directory, the previous install/KeyValues/direct-start helpers, and the
previous SteamRuntime, MadeiraDock and DockCMCache implementations. Wine resource staging
no longer adds or patches any former app-side bridge, display, input or output sources.
The former headless client host is not built for this branch. Existing prebuilt Wine/DXMT
runtime libraries and resource farms continue through the repository's normal workflows.

Integration files live in App/Sources/Integration. They adapt the fixed C host interface,
coordinate Valve's official client, preserve app UI/touch-controller composition, and
connect the existing Steam Cloud feature to the replacement transport and protobuf logic.
Native account sign-in is independent of Valve's client sign-in; there is no token handoff.
Client launch admission first closes downloads, cancels and awaits active file workers,
holds Cloud traffic, then awaits graceful native disconnect. It remains closed for the
whole client session. The menu can end the entire session. Client exit releases admission.

The existing runtime is treated as one Wine session per app process. Session reuse is
disabled for this integration. After the official installer completes, finishInstall
validates it and the user is told to restart before opening Steam. The next client launch
shows the package's first-update notice. No extra client command-line flags are added.

Narrow package integration edits:

- ControllerBridge exposes its queue, sample/updateTimer and value state internally and
  invokes the separate UI-capture/touch-composition hook before publishing a sample.
- SteamSession consults the separate launch-admission gate before connecting.
- SteamAppInfo carries optional Cloud metadata; its parser preserves the public metadata
  subtree for the separate Cloud adapter. Neither change imports original implementation
  logic into the package.
- Subsequent compiler/test fixes, if needed, are recorded in the branch commits and below.

License handling: authored MIT headers remain; the LGPL assembly-data notice and COPYING.LIB
remain; educational decoder files and BSD notice remain byte-identical. The LGPL and decoder
license texts are also included in the application resources. The original provenance and
pending chosen-behavior notes remain available with the source.

Validation is performed by the branch's Build IPA workflow: existing host tests, the two
replacement C test executables, standalone iOS Simulator XCTest for the replacement value,
codec, install/resume and client-host cases, release app compilation, source-membership
audit and release-export audit. The workflow creates an ad-hoc-signed IPA artifact, does
not publish a release and does not merge main. Sideloading requires re-signing with the
user's provisioning identity. Successful CI is not evidence of device execution, live
Steam sign-in, live downloads, Wine gameplay or sanitizer results.

## Integration build fixes

- Build 204: the simulator SDK has the system liblzma binary but not its public headers.
  CI now stages unchanged public headers from Homebrew xz for compilation and still links
  Apple's system liblzma. No replacement decoder or unrelated runtime is rebuilt.
- Host settings now query the replacement touch-mode store through a separate adapter;
  host launch selection and single-session refusal messages were aligned with the new path.
- Runtime limitation evidence: the repository's existing SESSION_REUSE investigation records
  that server and loader state cannot be torn down and initialized again in one app process.
  Its supported reuse path keeps the same runtime session alive. Track B's fixed lifecycle
  instead stops the server after a launch, so this integration explicitly requires restarting
  the app before another session; it does not bypass cleanup or silently keep a stopped session.
- Build 205: made the input test NaN explicitly CGFloat for the current SDK overloads; simulator tests run serially because some tests exercise UserDefaults.

- Build 206: all 18 simulator tests passed; both replacement C programs and the source
  membership audit passed. Release compilation then identified host declaration, actor
  isolation and optional-setting errors, corrected in separate integration glue/host files.
  Log capture and crash-tail paths now follow the replacement log location. The obsolete
  session-reuse toggle is replaced with the runtime limitation text. Unchanged generated
  no-JIT runtime dylibs are cached by dependency/content hashes to avoid relinking them
  during further source-only retries.
