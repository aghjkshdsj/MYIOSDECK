# Glass UI (branch claude/ui-glass-90beec)

The app's look and navigation follow the user's seven mockups ("Glass / Refined"): a dark navy base,
the selected game's artwork behind everything, frosted panels with thin light borders, a bright
blue primary button. Artwork comes from the user's own library at runtime (Steam's public store
images, the icons inside added games' programs); nothing is bundled.

## Map

| Where | File |
|---|---|
| Theme tokens, Appearance settings, panels, buttons, segmented pill | `App/Sources/UI/Glass/GlassTheme.swift` |
| Artwork cache (Caches/artwork), blurred backdrop, PE icon extraction | `App/Sources/UI/Glass/Artwork.swift` |
| One list of games (Steam owned + installed, added), favorites, navigation state | `App/Sources/UI/Model/GameCatalog.swift` |
| Starting games from anywhere (JIT choice, With/Without Steam, Steam Cloud hold, Dock setup) | `App/Sources/UI/Launch/GameLauncher.swift` |
| Game view, game menu (Quit / Force quit / Hide, controller, touch mouse, on-screen controller) | `App/Sources/UI/Launch/GameSessionHost.swift` |
| Header, tab pill, pushed pages | `App/Sources/UI/RootView.swift` |
| Home (mockups 1, 2), Library, Downloads, Search | `HomeView.swift`, `LibraryView.swift`, `DownloadsView.swift`, `SearchView.swift` |
| Game page / install (mockup 6) | `GameDetailView.swift` |
| Game settings (mockup 7) | `GameSettingsView.swift`, `Engine/GameSettingsCore.swift`, `Engine/GameSettingsStore.swift` |
| Settings (mockup 5), Storage (mockup 4) | `SettingsView.swift`, `StorageView.swift` |
| On-screen controller and its editor (mockup 3) | `Engine/TouchPadCore.swift`, `Render/TouchController.swift`, `UI/TouchLayoutEditor.swift` |

Where the old screens went: Home's readiness checklist and Enable JIT are in Settings › Performance;
the Performance tab is Settings › Performance › Benchmarks; Logs are Settings › Logs; the built-in
x86-64 and Windows test programs are Settings › About › Test programs; Steam sign-in, With/Without
Steam and Steam Cloud are Settings › Steam; each game's Steam Cloud section (Sync now, Upload,
Download, backups) is on its page. The old "Road to Steam games" stage list was dropped (stale text).

## Per-game settings, applied at launch

`GameSettingsStore.prepareLaunch` hands `WineController.run()` a `LaunchOverrides`:

- Preset: Fast = FXR, Compatibility = FXI as the x64 CPU without JIT (the first 64-bit game of a
  run still fixes the CPU for the later ones). There is no "Balanced": FEX's preset (with JIT) is
  an engine-wide setting that needs an engine restart, so it stays in Settings › Performance.
- Frame rate (30/40/60/Off): `DXMT_CONFIG=d3d11.preferredMaxFrameRate=N`, which DXMT's D3D10/11
  swapchain paces presents to (src/d3d11/d3d11_swapchain.cpp). D3D9 and D3D12 games are not capped.
- Show performance overlay: `CAMetalLayer.developerHUDProperties` on the game's layer (Metal's HUD).
- Game audio without JIT (WINEDLLOVERRIDES=mmdevapi=d when off), resolution (MADEIRA_SCREEN_H; the
  session's first game decides), on-screen controller mode.
- Controller API and touch mouse are kept per game key (`controllerAPI.<game>`, `touchMode.<game>`),
  falling back to the older per-program keys.

Game keys are RecentlyPlayed's: `steam.<App ID>`, `custom.<UUID>`.

## On-screen controller

A `TouchControllerView` in the window above `GameHostView` (the game view is a raw UIView above
SwiftUI). It takes only touches on its controls; others fall through to the game view (touch mouse,
three-finger tap). Its state is player 1's XInput pad through `ControllerBridge.setTouchPad`,
merged with a physical controller on slot 0 (buttons OR'd, deeper trigger, stick pushed further).
It follows the game view's visibility (hidden behind the game menu and the editor) and, in
Automatic mode, hides while a controller is connected. Layouts are per orientation, per game or
default (`touchLayout.<scope>.<landscape|portrait>`), positions as fractions of the safe area.
A double tap that stays down on a stick is L3/R3. tests/host/touch_pad checks the math and layouts.

## Storage › Temporary files

Clears Library/Caches, tmp and Windows' temp folders in the prefix (users/*/AppData/Local/Temp,
windows/temp). Never Documents (games, saves, save-backups, logs). Once Wine has run in this app
run it only clears the artwork cache: Wine keeps live files (server files, mappings) in tmp/Caches.

## Controller navigation of the app

iOS gives SwiftUI no controller-driven focus on iPhone, so the app has its own
(`UI/Focus/PadFocus.swift`, pure logic in `Engine/PadNavCore.swift`, tests in tests/host/pad_nav):

- Views mark themselves `.padFocus(id) { action }` (also `padToggle`, `padChoice`, `padSlider`) and
  report their frames; the D-pad or left stick moves a white ring with a blue glow to the nearest item
  that way (`FocusMath.next`), repeating while held (0.38 s, then every 85 ms).
- A activates, B goes back (closes a sheet or dialog, pops a page, else returns to Home), LB / RB
  go through the top bar in its order: Home, Library, Downloads, Search, Settings, Controllers (LB the
  reverse), Y opens Search, X is the focused game's Game
  settings, Menu opens the game's "…" on Home, its Game settings on a game page, else Settings.
  Left / right change segmented choices, sliders, pickers and steppers; at the first or last value
  the next press in that direction moves focus out (a control never traps the focus). A hint bar at
  the bottom shows the buttons while the ring is visible.
- Sections (`.padSection`: the top bar, a settings sidebar and its panel): a move takes the item in
  line first, then one of its own section; Left from a panel lands on the sidebar's current section;
  the end of a panel stays put. The stick counts only when one axis clearly dominates (no diagonal
  becomes Up by accident). Focus scrolls the real UIKit scroll views (`ScrollIntoView`).
- Sheets, dialogs, the editor and the game view are scopes (`.padScope`): only the top one's items
  take focus. `PadScrollView` scrolls the focused item into view.
- iOS alerts and confirmation dialogs cannot be driven by a controller, so every question is a glass
  dialog in a window of its own above everything (`UI/Focus/GlassDialogs.swift`), the game menu too.
  Menus also open as such a list when chosen with the controller.
- A touch hides the ring; the next controller press shows it again where it was.

Who has the controller: while a game's view is on screen (no dialog over it) the game does, as
before. Otherwise (Library, Settings, a hidden game, the game menu) the UI does and the game sees its
pad connected with nothing pressed (`ControllerBridge.setUIOwnsPads`); going back to the game waits
until every button is released, so the A that chose Resume never reaches the game.

The game menu while playing: the Home / guide button if the controller has one iOS hands to apps,
or View + Menu (Select + Start) held together for 0.6 s. A three-finger tap still works.

Not driven by the controller: text fields (Search's field, Steam sign-in by password: use QR sign-in,
whose method choice the controller can switch), iOS's own share sheet and Files picker, and dragging
controls in the layout editor (the controller can pick a control with X on Size and resize it).

## Xbox and PlayStation controllers

One choice, Xbox or PlayStation (Settings › Controls and the Controllers panel for the default, Game
settings › Controls per game, the game menu while playing), plus a separate per-game switch for the
controller as keyboard and mouse. The player never picks an API (`Engine/PadStyleCore.swift`,
`Engine/PadPrefs.swift`, `Engine/ControllerBridge.swift`, `WineController.run`):

| | Xbox | PlayStation |
| --- | --- | --- |
| XInput (player 1) | yes | yes (XInput-only games keep working) |
| DualSense HID (Sony 054C:0CE6, Madeira's `hidpad_ios.c`) | absent | present: SDL/HIDAPI, libScePad, hid.dll, DirectInput's HID joystick |
| MYIOSDECK's DirectInput pad (`MADEIRA_DINPUT_PAD`) | only for a game that names DirectInput and never XInput in its files (`PadAPIScan`, cached per program) | no (DirectInput lists the DualSense) |
| On-screen controller | A B X Y, LB/RB/LT/RT, View/Menu, Xbox button | Cross Circle Square Triangle, L1/R1/L2/R2, Create/Options, PS button; its own layout per orientation (starts as the Xbox one) |

What switches while a game runs, without restarting it or MYIOSDECK: the physical controller's mapping,
the on-screen controller's look and layout, XInput (always connected), and the DualSense's presence.
The DualSense is made when Wine starts, for every session (engine/wine/patches/hidpad_gate.py: in
Xbox mode opening it fails as for an unplugged pad), so every game of a kept session can have either.
What a game does with a controller that appears or goes is up to the game: most look for HID and
DirectInput devices only when they start, so their buttons (prompts) change at their next start; the
DirectInput pad too is decided at a game's start. A Wine build without the gate makes the DualSense only
for a session that starts in PlayStation mode (the app logs which; stage-app.sh writes
`MYIOSDECK_HIDPAD_GATE` when the staged wineserver has the gate).

Physical PlayStation controllers (DualSense, DualShock 4): Cross/Circle/Square/Triangle are A/B/X/Y,
Create is View, Options is Menu, the touchpad click is the DualSense's touchpad button in PlayStation
mode and View in Xbox mode, the PS button is Guide where iOS gives it to apps. The game's output
reaches the pad through Madeira's PadOutput.m (now armed by ControllerBridge): XInput rumble, and in
PlayStation mode the DualSense output reports (rumble, adaptive triggers as the closest GameController
mode, lightbar, player LEDs). iOS gives apps no access to the DualSense's speaker, microphone or
audio-driven haptics.

Older per-game choices (XInput, XInput + DirectInput, HID, DualSense, Keyboard and mouse) are read as
the style they meant (`PadStyle.migrate`).

## Build 197 changes

- Search and Controllers are pages like the tabs (the top bar stays), not sheets; LB/RB go through
  Home, Library, Downloads, Search, Settings, Controllers.
- With Steam / Without Steam is remembered per game (`steamLaunch.steam.<App ID>`: dock, direct,
  ask): the first Play asks (when the app default is Ask) and keeps the answer; the game page's
  Start with and Game settings › Steam change it. Both modes sync Steam Cloud saves through
  MYIOSDECK before and after the game (SteamCloudSync.beforePlay / gameEnded); With Steam adds Valve's
  client (Steam DRM, achievements, online play through Steam).
- Steam Cloud Upload / Download ask "Are you sure?" with what is overwritten, a destructive confirm,
  and the file list.
- The on-screen controller stays player 1 (at rest) behind the game menu and the layout editor
  (`TouchPadPresence`): unplugging it there made DirectInput games lose it until they restarted,
  which looked like the Xbox / PlayStation switch breaking it. The editor's Xbox / PlayStation toggle
  is the controller type: saving sets it (for the game, or as the default) and applies it at once in a game.
- With Steam starts no longer turn on the DirectInput pad (the scan read Valve's dockhost.exe).
