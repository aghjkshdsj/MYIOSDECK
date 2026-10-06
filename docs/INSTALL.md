# Installing MYIOSDECK and enabling JIT

Tested target: iPhone 15 Pro Max (A17 Pro), iOS 27. iOS 26 works the same way.

## 1. Get the IPA

Open [Releases](../../../releases) and download the newest file ending in **`-fex.ipa`**.
A `-uionly.ipa` means the FEX engine did not build for that commit: the app opens, but cannot
translate x86 code.

## 2. Sideload with iloader

1. Install [iloader](https://github.com/nab138/iloader) on your PC and connect the iPhone by USB.
2. Sign in with your Apple ID (a free one works; apps then expire after 7 days and need a refresh).
3. **Import IPA** → choose the MYIOSDECK IPA.
4. Keep its entitlements. iloader signs with a development profile, which includes `get-task-allow`
   (required for JIT). MYIOSDECK also asks for the increased memory limit ("Memory+").
5. On the iPhone: **Settings → General → VPN & Device Management** → trust your Apple ID.
6. **Settings → Privacy & Security → Developer Mode** must be on.

## 3. Set up StikDebug (one time)

1. Install [StikDebug](https://github.com/StikDebug/StikDebug) (iloader can install it and place
   its pairing file for you).
2. Install **LocalDevVPN** from the App Store and connect it. StikDebug reaches your iPhone's
   debug service through it. It works on Wi-Fi or with no network; it does not work over
   cellular data.

## 4. Enable JIT

Open MYIOSDECK → **Enable JIT**.

- MYIOSDECK opens StikDebug with its process ID and its own script (`myiosdeck-jit.js`).
- StikDebug attaches, the script prepares one large executable region (the JIT pool, 512 MB by
  default), and the debugger detaches again.
- Back in MYIOSDECK, the checklist shows **JIT: … self-test passed (42)** and FEX starts.

Always start JIT **from inside MYIOSDECK**. Enabling it from StikDebug's own app list only sets
the debug flag; on iOS 26/27 (TXM) the memory still has to be prepared, which needs our script.

## Troubleshooting

| What you see | Fix |
|---|---|
| "not debuggable (no get-task-allow)" | Re-sideload with iloader/SideStore/AltStore (development signing). |
| "StikDebug did not attach within 2 minutes" | Connect LocalDevVPN; use Wi-Fi, not cellular; re-import the pairing file in StikDebug. |
| "The debugger did not answer the prepare request" | You enabled JIT from StikDebug's list. Force-quit MYIOSDECK and use **Enable JIT** inside it. |
| Memory+ shows a warning | Your signing did not grant the increased memory limit. JIT still works; big games will not fit. |
| App closes during JIT setup | Lower **Settings → JIT pool** to 256 MB and try again; share the log from the Logs tab. |

Logs: the **Logs** tab, or Files → On My iPhone → MYIOSDECK → `myiosdeck-log.txt`.
