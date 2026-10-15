<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

Author: GPT-6.1 Sol, clean side, track B. Date: 2026-10-15.

Independent design and code, written from the following sources. No upstream implementation material is included; all new code carries the requested MIT header.

| Source consulted | Section/use | Date |
|---|---|---|
| User's attached track B request | Isolation, layout, workflow, ownership and commit rules | 2026-10-15 |
| Shared CLAUDE.md | Clean-side constraints, with user-specified track B overrides | 2026-10-15 |
| Shared specs/01-input.md | All parts and clarifications1–28 (including reread after questions), required behavior and tests | 2026-10-15 |
| Shared interfaces/wine_host.h | Exact C call signatures and units | 2026-10-15 |
| Shared interfaces/app-api.md | GameHostView and caller integration | 2026-10-15 |
| Track B README.md and QUESTIONS.md | Own repository layout and questions template | 2026-10-15 |
| [Apple GCDevice](https://developer.apple.com/documentation/gamecontroller/gcdevice), handlerQueue | Device queue ownership and common device properties | 2026-10-15 |
| [Apple GCMouseInput](https://developer.apple.com/documentation/gamecontroller/gcmouseinput), mouseInput, mouseMovedHandler, scroll | Optional button/profile types and raw movement/scroll callback APIs | 2026-10-15 |
| [Apple GCKeyboardInput](https://developer.apple.com/documentation/gamecontroller/gckeyboardinput), keyChangedHandler, GCKeyboardValueChangedHandler, GCKeyCode | Keyboard callback parameters and HID key representation | 2026-10-15 |
| [Microsoft Virtual-Key Codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes) | Numeric VK identities; original mapping written against HID usages | 2026-10-15 |
| [USB HID Usage Tables1.5](https://www.usb.org/sites/default/files/hut1_5.pdf), section10, pages89–95 | Keyboard/Keypad page0x07 identities; no source code taken | 2026-10-15 |

Apple documentation HTML initially returned a JavaScript notice; Markdown and JSON attempts returned tool errors. Official-domain searches supplied the device and callback declarations. Unrelated Apple archive results in those searches were not used.

Incidental exposure: the Microsoft page's navigation included a GitHub documentation edit hyperlink. It was not opened, fetched, searched, or used. No forbidden project code, track A code, or forbidden repository content was encountered. Before the request attachment was fully available, the initial workspace-instruction search ran in the default chat workspace and printed no files; no material from that search was used. All subsequent shell sessions started in the designated track B directory.
