# SwiftSteam (vendored)

Native Steam client in Swift: sign-in (password + Steam Guard, QR), the CM
connection, owned library, product info, depot downloader and content decoders.

Copied unchanged from [Madeira](https://github.com/willfaust/Madeira)
`app/Madeira/SwiftSteam/` at commit `65e6fe8f0da45b28b610c71fda9791e89d1060a6`
(the commit pinned in `engine/wine/PIN` and `engine/dxmt/PIN`). Licence:
GPL-3.0-or-later with the Madeira Converter Exception, as in each file's header.

MYIOSDECK supplies the two app types it uses (`MadeiraConfig`, `LogStore.log`)
in `../MadeiraShims.swift`. Keep local changes out of this folder so it can be
refreshed from a newer Madeira commit.
