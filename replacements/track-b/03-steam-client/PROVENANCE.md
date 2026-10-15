<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

Author: GPT-6.1 Sol, clean side, track B,2026-10-15. Swift, wire encoders/decoders, file parsers, lifecycle and install logic were independently designed and written. No generated protobuf implementation was copied. All authored sources/tests have the requested MIT header.

Sources consulted on2026-10-15:

- User's attached track B request; shared CLAUDE.md, specs/03-steam-client.md (all sections), interfaces/steam_api.md and interfaces/app-api.md: behavior, API, isolation and workflow. Spec04's presence was observed by listing specs only while finishing03.
- The user explicitly authorized read-only access to exactly shared refs/steam-protobufs/ and refs/zstd-educational/ after the initial request's narrower path list was identified. No broader refs/ access was assumed, including refs/README.md.
- refs/steam-protobufs/steammessages_auth.steamclient.proto: read in full; Authentication RSA, credential/QR begin, device details, confirmation kinds, Guard submission and polling field numbers. No generated code used.
- steammessages_base.proto: CMsgProtoBufHeader, CMsgMulti, CMsgIPAddress field numbers and wire types.
- steammessages_clientserver_login.proto: Hello/logon/logon-response/heartbeat field numbers and types.
- steammessages_clientserver_appinfo.proto: PICS token/product request/response, multipart flag and buffer fields.
- steammessages_clientserver.proto: CMsgClientLicenseList and License package ID.
- steammessages_clientserver_2.proto: depot-key request/response fields.
- steammessages_contentsystem.steamclient.proto: manifest request-code and CDN-auth request/response fields.
- content_manifest.proto: file mapping and chunk field numbers/types.
- [Valve Web API Overview](https://partner.steamgames.com/doc/webapi_overview): permitted HTTPS API overview. The fetched overview did not specify the raw-protobuf response selector; B03-1 records the implementation choice.
- [Apple URLSessionWebSocketTask](https://developer.apple.com/documentation/foundation/urlsessionwebsockettask), [webSocketTask(with:protocols:)](https://developer.apple.com/documentation/foundation/urlsession/websockettask(with:protocols:)), sendPing and URLSession/URLSessionWebSocketDelegate documentation: subprotocol negotiation, async send/receive, ping callback, frame size cap, close callbacks, byte-stream HTTP and invalidation. Unrelated Apple archive search results were not used.
- [Apple rsaEncryptionPKCS1](https://developer.apple.com/documentation/security/seckeyalgorithm/rsaencryptionpkcs1): algorithm identifier; key construction follows the behavior spec and general DER knowledge.
- [liblzma filter API](https://tukaani.org/xz/liblzma-api/filter_8h.html): raw decode/encode, property decode/encode and filter-option lifetime for the codec and generated VZ test fixture.
- refs/zstd-educational/README.md, zstd_decompress.h, zstd_decompress.c error/allocator sections and LICENSE: BSD educational decoder API/error model; copied files listed below. The format framing and wrappers are independently written from the spec. No other Zstd implementation consulted.

Vendored material under its OWN BSD license (selected from the offered dual license): vendor/zstd_decompress.c, vendor/zstd_decompress.h and vendor/LICENSE are unchanged copies from the user-authorized reference directory (spec identifies zstd1.5.6). Hashes were compared against that directory and matched:

| File | SHA256 |
|---|---|
| zstd_decompress.c | d6980a5308e488d6b25f22c5c3b8fbc1820579695a2300f8c5cc1356cdccd612 |
| zstd_decompress.h | 70021196b86934b4072428f584304616f382134b1f199c3db363cbfa4468cf43 |
| LICENSE | 7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8 |

BSteamCodecs.c includes the unchanged decoder with external macro adaptation: private symbol names, no diagnostics, bounded tracked allocation and a thread-local error return boundary in place of process exit. This wrapper and all other source are independently authored MIT code. The vendor is compiled only through the wrapper. .gitattributes preserves the exact reference bytes across Git checkouts. Tests generate AES and raw LZMA fixtures locally through platform APIs; the tiny raw-block Zstd frame is hand-built from the standard framing described by the permitted material.

Incidental exposure: the authorized educational README/source contain GitHub hyperlinks to upstream Zstd format/tests. Those link strings were seen as part of the permitted local files; no link was followed or fetched. There was no forbidden project code, track A output/history/questions, other session memory, original app repository or non-tool temp checkout exposure. The prior incidental Microsoft GitHub edit link is documented in spec01 provenance and was not used for this spec.

Validation here is source review and metadata/hash/whitespace checks only. No Swift/C compilation, XCTest, real Steam authentication, live CM session or download was run.
