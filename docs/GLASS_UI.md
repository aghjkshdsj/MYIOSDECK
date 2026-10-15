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
