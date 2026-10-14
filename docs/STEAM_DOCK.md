# Steam games "With Steam" (Madeira Dock)

"With Steam" starts a game through Valve's own Windows Steam client: Madeira Dock's
`dockhost.exe` (github.com/willfaust/madeira-dock, built in CI by `engine/pedylib/build-spike.sh`)
loads Valve's unmodified `steamclient64.dll` inside the Wine session, signs in with the user's own
refresh token (a one-use transfer from the Keychain), asks Valve's client whether the account owns
the game and has it start the game with Valve's LaunchApp. App side:
`App/Sources/Steam/MadeiraDock.swift` (vendored), `WineController.runDockGame`,
`LibraryView.startThroughDock`. The host's numeric report lands in the log as `[dock-report]`, and
Valve's own logs (`C:\Program Files (x86)\Steam\logs`) as `[steam-logs]` after the session.

MYIOSDECK's changes to the host, as patches applied in CI:
- `madeira-dock-signin-wait.patch`: `MADEIRA_DOCK_SIGNIN_MS` (90..900 s) lengthens the sign-in and
  game-start waits; the app sets 480 s without JIT.
- `madeira-dock-cm-transport.patch` (build 179): `MADEIRA_DOCK_CM_TRANSPORT=netfilter` makes Valve's
  client prefer its UDP/TCP transport to Steam; the app sets it without JIT. See below.

## Without JIT: sign-in never completed (builds 55, 105, 178: result 34)

Build 178's log (`myiosdeck-log - 2026-10-14T140014.498.txt`): the host submitted the token at
13:31:53 (`session-native-token-submitted=1`), Valve's client then reported a failed connection
(`session-connection-result=3`, k_EResultNoConnection) at 13:32:19, could not log on offline
(`session-offline-fallback=2`), and nothing more happened until the 480 s ran out (result 34).
Valve's connection log says why:

- The plain-HTTP connectivity test passed at once (`Connectivity test ... OK!`, IPv4 and IPv6).
- `GetCMListForConnect -- web API call failed (status = 0)` after 27 s: the HTTPS request to the CM
  directory (api.steampowered.com) did not complete.
- `PingWebSocketCM() ... failed talking to cm (timeout/neterror - Invalid)` for every built-in
  WebSocket CM within the same second: Valve's client gives each CM ping 1000 ms (Madeira's
  wineserver notes), and a TLS handshake in the interpreter takes longer.
- `ConnectFailed('Connection Failed':0) (0.0.0.0:0, WebSocket)`, then `StartAutoReconnect()` in 14 s,
  whose second `GetCMListForConnect` never returned in the remaining 7 minutes.

The no-JIT profiler (`[fxi-prof]`) showed Valve's `CHTTPClientThreadPool:0` thread in x64 code the
whole time, its hottest code OpenSSL's `bn_mul_mont` (generic path: `steamclient64.dll+0x54d2d`,
named in CI with `tools/pe_hot.pl` through `steam-scan.yml`'s `hot` input) and the debug free hook
`VALVE_CRYPTO_dbg_free` (byte-wise clearing of freed memory). Valve's TLS setup prefers curves
`P-521:P-384:P-256` (strings of the client, `steam-scan.yml`'s `strings` input): elliptic-curve math
on 6- and 9-word numbers in OpenSSL's generic code, slow in an interpreter.

Is the interpreter computing it wrong? No: the new FXI/FXR gate `engine/guest/bignum.c` runs the same
`bn_mul_mont` instruction sequence (mul/adc carry chains, the final `sbb` borrow across `dec` +
`jne`) for 2..32-word moduli and squaring chains, and FXI and FXR print what the native run prints
(fxr.yml, fxi.yml). So the client is slow, not wrong, in TLS.

### The change (build 179)

Valve's client has two transports to Steam's connection managers (CM): WebSockets over TLS, and
"NetFilter" (UDP/TCP with Valve's own channel encryption: one RSA public-key operation for the
session key, then AES; no TLS). It rolls which one to use against a share that defaults to 50%
WebSockets (`CM Directory list says 50% of connections should be websockets, we rolled 8 - using
WebSockets as default`), kept in `HKCU\Software\Valve\Steam\PercentDefaultWebSockets`. The client
also carries built-in NetFilter CM addresses for when the CM directory cannot be reached. Without
JIT the host now sets that value to 0 before it loads the client (`cm-transport-netfilter=1` in the
report), so the roll picks NetFilter; with JIT it removes the value again (`-1`) and the client
decides as usual. Nothing about sign-in, ownership or launch changes: the same client signs in the
same way over its other transport.

The profiler also lists, per 10 s, the return addresses found on the sampled x64 stacks
(`[fxi-prof]   on the stack N: ...% of x64 samples return to module+rva`), the callers of the hot
code; name them with `gh workflow run steam-scan.yml -f hot="0x... 0x..."`.

### What the next log should show

- `[dock-report] cm-transport-netfilter=1`.
- In `[steam-logs] == connection_log.txt`: `... we rolled N - using NetFilter as default` (or the
  client's wording for it), then a UDP or TCP connection to a CM (`Connect() starting connection
  (..., UDP|TCP)`), `[Logged On, ...]`, and in the report `session-authenticated-online=1`,
  `session-requested-app-entitled=1`, `launch-request-submitted`.
- If instead `Chose a netfilter connection type, but no netfilter CMs. Forcing WebSocket.` appears,
  the client has no NetFilter CM list without the (HTTPS) directory; the next step is then to give
  it one: the app's own SwiftSteam connection already fetches the CM list over iOS's TLS
  (`CMServerList.swift`), which could be handed to the client's cache before it starts.
- `[fxi-prof] on the stack` lines naming what the HTTPS thread computes, if it still does.

## Steam Cloud with Dock

See `docs/STEAM_CLOUD.md`: MYIOSDECK syncs before the host starts and after the session ends, and
makes no Steam request while Valve's client holds the account; Valve's client may sync too, and
both compare hashes, so nothing is sent twice.

## Build 180 result and the CM list (build 181)

Build 180 (Dokimon, `myiosdeck-log - 2026-10-14T155511.684.txt`, "black screen": the game surface
waiting for Steam): `cm-transport-netfilter=1`, and Valve's client did take the NetFilter path,
but logged `No NetFilter CMs, defaulting to WebSockets`: without the (HTTPS) CM directory it has no
UDP/TCP server to use, so it fell back to the same WebSocket failure. Steam Cloud before Play was
clear in 0.6 s and is not involved.

Where the client keeps a CM list (`tools/pe_hot.pl str:CMWS`, `0x57b7a0`, `dump:0x12266e0`): at
start-up its CM interface reads `Software\Valve\Steam\CM` (NetFilter) and `...\CMWS` (WebSockets)
from config store 1, the install store `config\config.vdf` (`InstallConfigStore`), and splits the
value at `;`. Build 181 fills that value before Dock starts (`DockCMCache`, `SteamCMList.swift`):
the same public directory (`ISteamDirectory/GetCMListForConnect`), fetched over iOS's HTTPS,
NetFilter servers of the public realm, least loaded first (the last good list is kept for a start
without network). The rest of config.vdf is kept token for token (tests/host/steam_cm); comments
are not kept (Valve's client writes none). Log: `[dock-cm] netfilter CMs=N (directory) written`.

The next log should show, in `[steam-logs]`, the CM directory still failing after its timeout, then
NetFilter instead of `No NetFilter CMs`: a UDP or TCP `Connect()` to one of the listed addresses,
`[Logged On, ...]`, and in the report `session-authenticated-online=1`.

## GameNative's "headless Steam" (reviewed for build 181)

GameNative 1.3.0 (Android, Winlator/Box64/FEX based; app code GPL-3.0) added a "headless" Steam
mode: `steamhost`, a `steam.exe` stand-in plus `steamhost64.exe` that load Valve's real Windows
client (build 1788652215) without its UI, sign it in with the user's refresh token (passed as
`STEAMHOST_TOKEN`, lightly obfuscated with the app's package name), and start the game through it
(`STEAMHOST_APPID`, `STEAMHOST_GAME_CMD`, a CEG pass, offline mode, Steam Input layouts, a one-word
status file). That is the same design as Madeira Dock. The host itself is closed source: per its
THIRD_PARTY_NOTICES it is "Proprietary — all rights reserved", downloaded from
downloads.gamenative.app at run time and not in the source archive, so nothing of it can be used
here. GameNative's open app code does nothing special for Valve's network: on Android the client
runs under a JIT (Box64/FEX), where its TLS completes in time. Its other mode, Valve's GUI client
with a token written into `config.vdf` (`ConnectCache`) and `loginusers.vdf`, needs the whole GUI
client (Chromium web helper) and would hit the same TLS problem without JIT. Nothing ported.

## Build 181 result and the directory request (build 182)

Build 181 (`myiosdeck-log - 2026-10-14T164843.844.txt`, log taken 5.5 minutes in): the CM list was
written (`[dock-cm] netfilter CMs=18 (directory)`), but Valve's client asks its HTTPS CM directory
before every connection (`@nClientQueryCMDirectoryBeforeConnect`, default 1, read at
`steamclient64.dll+0x580780` by the connect routine; no supported way to change it from outside),
and this time that request neither answered nor failed for over 4 minutes, so the client never
reached its cached list. Steam Cloud before Play was clear and is not involved.

Build 182 makes that request fail at once instead: for a Dock session without JIT the app sets
`MYIOSDECK_DNS_REFUSE=api.steampowered.com`, and Wine's `getaddrinfo` (Madeira's
`dlls/ws2_32/unixlib.c`, `engine/wine/patches/dns_refuse.py`) answers "host not found" for the
listed names. The connectivity test (test.steampowered.com), the CM servers (IP addresses) and every
other name resolve as usual; the variable is cleared when the Dock session ends and for every other
launch. Valve's client is not changed: it takes its own fallback, the cached `CM` list. Web API
calls to that host during the session fail at once too (not a hang); a game started by the client
that calls api.steampowered.com itself would see the same during the session.

What the next log should show: `GetCMListForConnect -- web API call failed` within seconds of
`LogOn() called`, then NetFilter (not `No NetFilter CMs`), a UDP or TCP `Connect()` to one of the
listed addresses, `[Logged On`. The dump now also includes Valve's `configstore_log.txt`.
