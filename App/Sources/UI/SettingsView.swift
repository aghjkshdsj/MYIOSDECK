// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Settings (mockup 5): a sidebar of sections and the chosen one beside it in landscape; a list
/// whose sections open as pages in portrait.
struct SettingsView: View {
    @ObservedObject private var ui = UIState.shared
    @Environment(\.isLandscape) private var landscape
    @State private var section = SettingsSection.appearance

    var body: some View {
        if landscape {
            HStack(alignment: .top, spacing: 24) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Settings").font(.title2.weight(.bold)).padding(.bottom, 6)
                    PadScrollView {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(SettingsSection.allCases) { s in sidebarItem(s) }
                        }
                    }
                    .scrollIndicators(.hidden)
                }
                .frame(width: 240)
                .padSection("sidebar")
                VStack(alignment: .leading, spacing: 12) {
                    Text(section.rawValue).font(.title.weight(.bold))
                    if section.scrolls {
                        PadScrollView {
                            SettingsSectionContent(section: section)
                                .padding(.bottom, 24)
                                .frame(maxWidth: 760, alignment: .leading)
                        }
                        .scrollIndicators(.hidden)
                    } else {
                        SettingsSectionContent(section: section)
                    }
                }
                .id(section)
                .padSection("panel")
            }
            .padding(.horizontal, 10)
            .padding(.top, 6)
        } else {
            PadScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    Text("Settings").font(.largeTitle.weight(.bold))
                    GlassCard {
                        ForEach(SettingsSection.allCases) { s in
                            Button { ui.open(.settings(s)) } label: {
                                GlassRow(last: s == SettingsSection.allCases.last) {
                                    Image(systemName: s.icon).font(.title3).frame(width: 30)
                                    Text(s.rawValue)
                                    Spacer()
                                    Image(systemName: "chevron.right").foregroundStyle(Glass.secondary)
                                }
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .padFocus("settings.row.\(s.rawValue)", radius: 12) { ui.open(.settings(s)) }
                        }
                    }
                }
                .padding(.horizontal, 20)
                .padding(.bottom, 24)
            }
            .scrollIndicators(.hidden)
        }
    }

    private func sidebarItem(_ s: SettingsSection) -> some View {
        Button { withGlassAnimation { section = s } } label: {
            HStack(spacing: 16) {
                Image(systemName: s.icon).font(.title3).frame(width: 30)
                Text(s.rawValue)
                Spacer()
            }
            .foregroundStyle(Glass.text)
            .padding(.horizontal, 16).frame(height: 52)
            .background {
                if section == s { Color.clear.glassPanel(cornerRadius: 14, highlighted: true) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(section == s ? .isSelected : [])
        .padFocus("settings.side.\(s.rawValue)", radius: 14, preferred: s == section,
                  onFocus: { withGlassAnimation { section = s } }) { withGlassAnimation { section = s } }
    }
}

extension SettingsSection {
    /// Logs scroll on their own.
    var scrolls: Bool { self != .logs }
}

/// A section as its own page (portrait).
struct SettingsPage: View {
    let section: SettingsSection?
    var body: some View {
        let s = section ?? .appearance
        GlassPage(back: "Settings", title: s.rawValue, scrolls: s.scrolls) {
            SettingsSectionContent(section: s)
        }
    }
}

struct SettingsSectionContent: View {
    let section: SettingsSection
    var body: some View {
        switch section {
        case .appearance: AppearanceSettings()
        case .performance: PerformanceSettings()
        case .controls: ControlsSettings()
        case .steam: SteamSettings()
        case .storage: StorageView()
        case .logs: LogView()
        case .about: AboutSettings()
        }
    }
}

// MARK: - Appearance (mockup 5)

private struct AppearanceSettings: View {
    @ObservedObject private var look = Appearance.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            GlassCard {
                GlassRow {
                    Text("Theme")
                    Spacer(minLength: 12)
                    GlassSegmented(options: Appearance.Theme.allCases, selection: $look.theme, compact: true, focusID: "settings.seg1") { $0.title }
                        .frame(maxWidth: 330)
                }
                GlassRow(last: true) {
                    Text("Glass intensity")
                    Spacer(minLength: 12)
                    Slider(value: $look.glassIntensity, in: 0...1).tint(Glass.primary).frame(maxWidth: 330)
                        .padSlider("settings.glass", value: $look.glassIntensity, range: 0...1, step: 0.1)
                        .accessibilityValue("\(Int(look.glassIntensity * 100)) percent")
                }
            }
            GlassCard {
                GlassToggleRow(title: "Game artwork background", isOn: $look.artworkBackground)
                GlassToggleRow(title: "Animated transitions",
                               detail: look.systemReduceMotion ? "Off while Reduce Motion is on in iOS Settings › Accessibility." : nil,
                               isOn: Binding(get: { look.animations }, set: { look.animatedTransitions = $0 }),
                               disabled: look.systemReduceMotion)
                GlassToggleRow(title: "Reduce transparency",
                               detail: look.systemReduceTransparency ? "On while Reduce Transparency is on in iOS Settings › Accessibility." : nil,
                               isOn: Binding(get: { look.reduceTransparency }, set: { look.reduceTransparencySetting = $0 }),
                               last: true, disabled: look.systemReduceTransparency)
            }
            Text("Use the selected game's artwork throughout the app.")
                .font(.footnote).foregroundStyle(Glass.secondary).padding(.leading, 4)
        }
    }
}

// MARK: - Performance

private struct PerformanceSettings: View {
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var jit: JITController
    @EnvironmentObject private var device: DeviceInfo
    @ObservedObject private var ui = UIState.shared

    private var frameCap: Binding<Int> {
        Binding(get: { settings.frameCap >= 120 ? 0 : settings.frameCap }, set: { settings.frameCap = $0 == 0 ? 120 : $0 })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            DeckCard(title: "Status", icon: "checklist") {
                StatusRow(label: "Debuggable signature",
                          detail: jit.hasGetTaskAllow ? "get-task-allow present: StikDebug can attach"
                                                      : "Missing: re-sideload the IPA with iloader / SideStore",
                          level: jit.hasGetTaskAllow ? .good : .bad)
                StatusRow(label: "JIT", detail: jitDetail, level: jitLevel)
                StatusRow(label: "Memory+", detail: "\(device.availableMB) MB available to the app of \(String(format: "%.0f", device.memoryGB)) GB" +
                          (device.hasIncreasedMemoryLimit ? " (increased limit active)" : " (increased-memory-limit not granted)"),
                          level: device.hasIncreasedMemoryLimit ? .good : .warn)
                StatusRow(label: "FEX engine", detail: engineDetail, level: engine.state == .ready ? .good : .idle)
                if !jit.isReady {
                    Button(jitButtonTitle) { GameLauncher.shared.enableJIT() }
                        .buttonStyle(DeckButtonStyle())
                        .padFocus("settings.enableJIT", radius: 20) { GameLauncher.shared.enableJIT() }
                        .disabled(jit.state == .waitingForDebugger || jit.state == .preparing)
                } else if engine.state != .ready {
                    Button("Start engine") { engine.start(settings: settings) }.buttonStyle(DeckButtonStyle())
                        .padFocus("settings.startEngine", radius: 20) { engine.start(settings: settings) }
                        .disabled(engine.state == .starting)
                }
                Text("Without JIT games run in MYIOSDECK's interpreter: no StikDebug needed, slower. With JIT (StikDebug + LocalDevVPN, docs/INSTALL.md) everything runs through FEX at full speed.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }

            DeckCard(title: "Games", icon: "gamecontroller.fill") {
                HStack {
                    Text("Frame rate")
                    Spacer()
                    GlassSegmented(options: [30, 40, 60, 0], selection: frameCap, compact: true, focusID: "settings.seg2") { $0 == 0 ? "Off" : "\($0)" }
                        .frame(maxWidth: 300)
                }
                Text("Caps Direct3D 10 and 11 games (DXMT paces their frames); 40 is a good battery/smoothness middle ground, as on the Steam Deck. Off leaves the pace to the game. Each game can override it in its Game settings.")
                    .font(.footnote).foregroundStyle(Deck.dim)
                HStack {
                    Text("Resolution")
                    Spacer()
                    GlassSegmented(options: LaunchOverrides.resolutions, selection: $settings.monitorHeight, compact: true, focusID: "settings.seg3") { "\($0)p" }
                        .frame(maxWidth: 330)
                }
                Text("The height of the screen Windows sees; its width follows this iPhone's shape. Read when Wine starts.")
                    .font(.footnote).foregroundStyle(Deck.dim)
                Toggle("Show performance overlay", isOn: $settings.gameHUD).tint(Deck.accent)
                    .padToggle("settings.toggle.Show performance overlay", isOn: $settings.gameHUD)
                Text("Metal's overlay over the game: frame rate, frame time, GPU time.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }

            DeckCard(title: "Without JIT", icon: "lock.shield") {
                Toggle("Windows games: FXR", isOn: $settings.winFXR).tint(Deck.accent)
                    .padToggle("settings.toggle.Windows games: FXR", isOn: $settings.winFXR)
                Text("Runs a 64-bit Windows game's x64 code with FXR (about 2-3× FXI's speed) instead of FXI. On by default; turn it off to compare with FXI or if a game misbehaves only with FXR (or choose Compatibility in that game's settings). Faults (x64 exceptions) are exact; a debugger or garbage collector reading another thread's registers sees its last call into Windows, as with FXI. Applies to the next game you start without JIT; with Play another game without restarting, the first 64-bit game's choice holds until MYIOSDECK closes.")
                    .font(.footnote).foregroundStyle(Deck.dim)
                Toggle("Game audio", isOn: Binding(get: { !settings.noJITMuteAudio }, set: { settings.noJITMuteAudio = !$0 })).tint(Deck.accent)
                    .padToggle("settings.toggle.Game audio", isOn: Binding(get: { !settings.noJITMuteAudio }, set: { settings.noJITMuteAudio = !$0 }))
                Text("Without JIT a game's audio engine decodes and mixes sound in the interpreter, and that can take a whole CPU core (Stick Fight's menu music did). Off: games start with no audio device. Applies to the next game you start without JIT.")
                    .font(.footnote).foregroundStyle(Deck.dim)
                Toggle("Play another game without restarting (experimental)", isOn: $settings.sessionReuse).tint(Deck.accent)
                    .padToggle("settings.toggle.Play another game without restarting (experimental)", isOn: $settings.sessionReuse)
                Text("When a game ends (its own Quit, or Quit game in the game menu, which asks it to close), Wine keeps running and the next game starts in it: no need to close and reopen MYIOSDECK, and 32-bit and 64-bit games can follow each other. A game that does not close can be force-quit from the game menu. The next game must use the same controller mode if that is a HID gamepad; otherwise MYIOSDECK asks for a restart. 64-bit games keep the x64 CPU (FXR or FXI) the first one used. A 32-bit game's memory comes back when the next 32-bit game starts; a 64-bit game's only when MYIOSDECK closes, so MYIOSDECK asks for a restart when memory runs low. Applies from the next game you start.")
                    .font(.footnote).foregroundStyle(Deck.dim)
                Toggle("Use FXR for x86-64 Hello (experimental)", isOn: $settings.useFXR).tint(Deck.accent)
                    .padToggle("settings.toggle.Use FXR for x86-64 Hello (experimental)", isOn: $settings.useFXR)
                Text("FXR is FXI with the x86 registers kept in ARM registers (\(engine.fxrPinned ? "pinned in this build" : "NOT pinned in this build")), about 2-3× FXI's speed. The benchmark measures both.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }

            DeckCard(title: "With JIT (FEX)", icon: "dial.high.fill") {
                GlassSegmented(options: FEXPreset.allCases, selection: $settings.fexPreset, compact: true, focusID: "settings.seg4") { $0.title }
                Text(settings.fexPreset.detail).font(.footnote).foregroundStyle(Deck.dim)
                Toggle("Multiblock compilation", isOn: $settings.multiblock).tint(Deck.accent)
                    .padToggle("settings.toggle.Multiblock compilation", isOn: $settings.multiblock)
                Stepper("Max instructions per block: \(settings.maxInst)", value: $settings.maxInst, in: 500...10000, step: 500)
                    .padFocus("settings.maxInst", radius: 10, adjust: { d in
                        let v = settings.maxInst + d * 500
                        guard (500...10000).contains(v) else { return false }
                        settings.maxInst = v
                        return true
                    }) {}
                Button("Apply (restart engine)") { engine.start(settings: settings) }
                    .buttonStyle(DeckButtonStyle(prominent: false))
                    .padFocus("settings.apply", radius: 20) { if jit.isReady { engine.start(settings: settings) } }
                    .disabled(!jit.isReady || engine.state == .starting)
                Text("Each restart uses a little of the JIT pool; restart MYIOSDECK if it runs low.")
                    .font(.caption).foregroundStyle(Deck.dim)
                Toggle("Verbose FEX log", isOn: $settings.verboseFEXLog).tint(Deck.accent)
                    .padToggle("settings.toggle.Verbose FEX log", isOn: $settings.verboseFEXLog)
                HStack {
                    Text("JIT pool")
                    Spacer()
                    GlassSegmented(options: [256, 512, 1024], selection: $settings.jitPoolMB, compact: true, focusID: "settings.seg5") { $0 == 1024 ? "1 GB" : "\($0) MB" }
                        .frame(maxWidth: 300)
                }
                Text("Memory the debugger makes executable at JIT time. Bigger fits bigger games' translated code; it applies the next time JIT is enabled.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }

            Button { ui.open(.benchmarks) } label: {
                HStack {
                    Image(systemName: "speedometer").font(.title3).frame(width: 30)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Benchmarks")
                        Text("CPU (native, FEX, no JIT), Windows DLLs without JIT, the Metal GPU stage, CPU features")
                            .font(.footnote).foregroundStyle(Glass.secondary)
                    }
                    Spacer()
                    Image(systemName: "chevron.right").foregroundStyle(Glass.secondary)
                }
                .padding(16).glassPanel(cornerRadius: 18)
            }
            .buttonStyle(.plain)
            .padFocus("settings.benchmarks", radius: 18) { ui.open(.benchmarks) }
        }
    }

    private var jitButtonTitle: String {
        switch jit.state {
        case .waitingForDebugger: return "Waiting for StikDebug…"
        case .preparing: return "Preparing JIT memory…"
        default: return "Enable JIT"
        }
    }

    private var jitDetail: String {
        switch jit.state {
        case .off: return "Off. Needs StikDebug + LocalDevVPN (see docs/INSTALL.md)."
        case .waitingForDebugger: return "Waiting for StikDebug to attach…"
        case .preparing: return "Asking the debugger for executable memory…"
        case .ready(let mb): return "\(mb) MB JIT pool ready, self-test passed (\(jit.selfTest))"
        case .failed(let why): return why
        }
    }

    private var jitLevel: StatusRow.Level {
        switch jit.state {
        case .ready: return .good
        case .failed: return .bad
        case .off: return .idle
        default: return .warn
        }
    }

    private var engineDetail: String {
        if !engine.linked { return "Not in this build (UI-only)" }
        switch engine.state {
        case .notStarted: return "Waiting for JIT. FEX \(engine.fexVersion.prefix(10))"
        case .starting: return "Starting FEXCore…"
        case .ready: return "FEXCore \(engine.fexVersion.prefix(10)), preset \(settings.fexPreset.title)"
        case .failed(let why): return why
        }
    }
}

// MARK: - Controls

private struct ControlsSettings: View {
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var device: DeviceInfo
    @State private var editing = false
    @State private var confirmReset = false

    private var padMode: Binding<TouchPadMode> {
        Binding(get: { TouchPadMode(rawValue: settings.touchPadMode) ?? .auto }, set: { settings.touchPadMode = $0.rawValue })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            DeckCard(title: "On-screen controller", icon: "gamecontroller") {
                HStack {
                    Text("Show")
                    Spacer()
                    GlassSegmented(options: TouchPadMode.allCases, selection: padMode, compact: true, focusID: "settings.seg6") { $0.title }.frame(maxWidth: 330)
                }
                Text("A gamepad drawn over the game, seen by the game as an Xbox controller (player 1, with a Bluetooth controller if one is connected). Automatic shows it while no controller is connected and hides it when one connects. Each game can choose its own in Game settings or the game menu.")
                    .font(.footnote).foregroundStyle(Deck.dim)
                Toggle("Haptic tap on press", isOn: $settings.touchPadHaptics).tint(Deck.accent)
                    .padToggle("settings.toggle.Haptic tap on press", isOn: $settings.touchPadHaptics)
                HStack(spacing: 10) {
                    Button { editing = true } label: { Label("Edit default layout", systemImage: "square.and.pencil") }
                        .buttonStyle(DeckButtonStyle())
                        .padFocus("settings.editLayout", radius: 20) { editing = true }
                    Button("Reset…") { confirmReset = true }.buttonStyle(DeckButtonStyle(prominent: false))
                        .padFocus("settings.resetLayout", radius: 20) { confirmReset = true }
                }
                Text("Its buttons work with every touch mouse mode; touches beside them still reach the game, and a three-finger tap away from them opens the game menu.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }
            DeckCard(title: "Controller", icon: "gamecontroller.fill") {
                StatusRow(label: device.controllers.isEmpty ? "No controller connected" : "Connected",
                          detail: device.controllers.isEmpty ? "Bluetooth Xbox or DualSense controllers, Backbone and MFi controllers work." : device.controllers.joined(separator: ", "),
                          level: device.controllers.isEmpty ? .idle : .good)
                Picker("Controller API", selection: $settings.controllerAPI) {
                    Text("XInput (Xbox controller)").tag("xinput")
                    Text("XInput + DirectInput").tag("dinput")
                    Text("HID gamepad").tag("hid")
                    Text("HID DualSense (PS5)").tag("dualsense")
                    Text("Keyboard and mouse").tag("keyboard")
                }
                .pickerStyle(.menu)
                .padFocus("settings.controllerAPI", radius: 10, adjust: { d in
                    let all = GameSessionHost.controllerOrder
                    let i = all.firstIndex(of: settings.controllerAPI) ?? 0
                    guard let n = ChoiceStep.index(i, count: all.count, delta: d) else { return false }
                    settings.controllerAPI = all[n]
                    return true
                }) {
                    showActions("Controller API", GameSessionHost.controllerOrder.map { api in
                        DialogButton(title: (api == settings.controllerAPI ? "✓ " : "") + (GameSessionHost.controllerNames[api] ?? api)) { settings.controllerAPI = api }
                    })
                }
                Text("How Windows games see your controller, for games without their own choice; takes effect the next time a game starts. XInput suits most games. If a game ignores the controller, try DirectInput (older games), HID or DualSense (engines that look for devices, such as Unity's old input system), or Keyboard and mouse, which works with any game that takes keys: A Space, B Ctrl, X E, Y R, LB Q, RB F, L3 Shift, R3 C, Menu Esc, Options Enter, D-pad arrows, left stick WASD, RT left click, LT right click.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }
            DeckCard(title: "Touch mouse", icon: "hand.point.up.left") {
                Text("Per game, in the game menu (three-finger tap or the ⋯ button) or Game settings: Off (touching the game does nothing), Trackpad (a cursor one finger moves, tap to click, two fingers scroll or right-click) or Tap where you click. A hardware keyboard and mouse work in every game.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }
        }
        .fullScreenCover(isPresented: $editing) { TouchLayoutEditor(gameKey: nil, title: "Default layout") }
        .glassDialog("Reset the default layout?", isPresented: $confirmReset,
                     message: "The default layout goes back to MYIOSDECK's. Games with their own layout keep it.") {
            [DialogButton(title: "Reset the default layout", role: .destructive) { TouchLayoutStore.resetDefault() }, .cancel()]
        }
    }
}

// MARK: - Steam

private struct SteamSettings: View {
    @EnvironmentObject private var settings: Settings
    @ObservedObject private var signIn = SteamSignInModel.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @State private var showSignIn = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            DeckCard(title: "Account", icon: "person.crop.circle") {
                if let name = signIn.accountName {
                    StatusRow(label: "Signed in as \(name)",
                              detail: steam.games.isEmpty ? "Owned Windows games appear in the Library." : "\(steam.games.count) owned Windows games.",
                              level: .good)
                    HStack(spacing: 10) {
                        Button("Refresh library") { Task { await steam.refresh() } }
                            .buttonStyle(DeckButtonStyle(prominent: false)).disabled(steam.refreshing)
                            .padFocus("settings.refreshLibrary", radius: 20) { Task { await steam.refresh() } }
                        Button("Sign out") { signIn.signOut() }.buttonStyle(DeckButtonStyle(prominent: false))
                            .padFocus("settings.signOut", radius: 20) { signIn.signOut() }
                    }
                } else {
                    Text("Sign in with your Steam account name and password (Steam Guard supported) or a QR code from the Steam app. The sign-in token stays in this device's Keychain; the password is never stored.")
                        .font(.subheadline).foregroundStyle(Deck.dim)
                    Button("Sign in to Steam") { showSignIn = true }.buttonStyle(DeckButtonStyle())
                        .padFocus("settings.signIn", radius: 20, preferred: true) { showSignIn = true }
                }
            }
            DeckCard(title: "Starting Steam games", icon: "play.circle") {
                GlassSegmented(options: ["ask", "dock", "direct"], selection: $settings.steamLaunchMode, compact: true, focusID: "settings.seg7") {
                    $0 == "ask" ? "Ask each time" : $0 == "dock" ? "With Steam" : "Without Steam"
                }
                Text("With Steam: Valve's own Steam client (downloaded once from Valve, about 72 MB) signs in with your account, checks the licence and starts the game, so games that need Steam running work. Without JIT its sign-in takes several minutes. Without Steam: the game's program starts on its own, faster (for DRM-free games).")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }
            DeckCard(title: "Steam Cloud", icon: "icloud") {
                Toggle("Steam Cloud saves", isOn: $settings.steamCloud).tint(Deck.accent)
                    .padToggle("settings.toggle.Steam Cloud saves", isOn: $settings.steamCloud)
                Text("Before a Steam game starts, its Steam Cloud saves come down to where the game reads them; when it ends (its own quit, Quit game, Force quit or a crash), the saves it changed go up. Nothing is replaced without a copy first: this iPhone's copies and the cloud's go to save-backups (Files › On My iPhone › MYIOSDECK). A save changed on both sides is never picked for you: the game's page asks which to keep. Off: nothing is synced. Each game's page has Sync now, Upload, Download and its backups.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }
        }
        .sheet(isPresented: $showSignIn) { SteamSignInView() }
    }
}

// MARK: - About

private struct AboutSettings: View {
    @EnvironmentObject private var engine: EngineController
    @ObservedObject private var ui = UIState.shared

    var body: some View {
        let info = Bundle.main.infoDictionary
        VStack(alignment: .leading, spacing: 14) {
            DeckCard(title: "MYIOSDECK", icon: "info.circle") {
                Text("Version \(info?["CFBundleShortVersionString"] as? String ?? "?") (build \(info?["CFBundleVersion"] as? String ?? "?"))")
                Text("FEX: \(engine.fexVersion)").font(.footnote.monospaced()).foregroundStyle(Deck.dim)
                Text("Interpreter (Blink): \(engine.interpVersion)").font(.footnote.monospaced()).foregroundStyle(Deck.dim)
                Text("GPL-3.0-or-later. Built on FEX-Emu, Madeira's iOS ports of FEX, Wine and DXMT, Blink (no-JIT interpreter), StikDebug's JIT protocol, and the DroidDeck design. Game artwork comes from your own library (Steam's store images, your games' icons) and is not part of the app.")
                    .font(.footnote).foregroundStyle(Deck.dim)
            }
            GlassCard {
                navRow("Logs", detail: "Share myiosdeck-log.txt when something goes wrong", icon: "text.alignleft") { ui.open(.logs) }
                navRow("Benchmarks", detail: "CPU, GPU and no-JIT tests", icon: "speedometer") { ui.open(.benchmarks) }
                navRow("Test programs", detail: "Built-in x86-64 and Windows programs", icon: "terminal", last: true) { ui.open(.developer) }
            }
        }
    }

    private func navRow(_ title: String, detail: String, icon: String, last: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            GlassRow(last: last) {
                Image(systemName: icon).font(.title3).frame(width: 30)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                    Text(detail).font(.footnote).foregroundStyle(Glass.secondary)
                }
                Spacer()
                Image(systemName: "chevron.right").foregroundStyle(Glass.secondary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padFocus("about.\(title)", radius: 12, activate: action)
    }
}

// MARK: - Test programs (the old Home and Library cards)

struct DeveloperView: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var wine: WineController
    @EnvironmentObject private var jit: JITController
    @ObservedObject private var session = GameSession.shared
    @State private var noJITAction: (() -> Void)?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            DeckCard(title: "Built-in x86-64 programs", icon: "cpu") {
                programRow(name: "x86-64 Hello", detail: "Prints the CPU the guest sees (CPUID) and times a loop") {
                    gated(jit: { engine.runHello(mode: .jit) }, noJIT: { engine.runHello(mode: settings.linuxNoJITMode) })
                }
                programRow(name: "CPU benchmark suite", detail: "Native vs FEX vs no-JIT, five kernels (also in Benchmarks)") {
                    let preset = settings.fexPreset.title
                    gated(jit: { engine.runBenchmarks(fex: true, interpreter: settings.benchInterpreter, presetName: preset) },
                          noJIT: { engine.runBenchmarks(fex: false, interpreter: true, presetName: preset) })
                }
                if let busy = engine.busy {
                    HStack { ProgressView(); Text(busy).foregroundStyle(Deck.dim) }
                }
                if let run = engine.lastRun {
                    Text(run.output.isEmpty ? run.error : run.output)
                        .font(.system(.footnote, design: .monospaced))
                        .textSelection(.enabled)
                    Text(run.mode == .jit
                         ? "exit \(run.exitCode) · \(String(format: "%.1f", run.seconds * 1000)) ms incl. translation · \(run.syscalls) syscalls · \(run.poolUsedKB) KB of ARM64 code generated"
                         : "exit \(run.exitCode) · \(String(format: "%.1f", run.seconds * 1000)) ms interpreted (no JIT)")
                        .font(.caption).foregroundStyle(Deck.dim)
                }
            }
            DeckCard(title: "Windows programs (Wine + Direct3D)", icon: "macwindow") {
                if !wine.linked {
                    Text("This build does not include Wine.").font(.subheadline).foregroundStyle(Deck.dim)
                } else {
                    ForEach(WineController.programs) { p in
                        programRow(name: p.title, detail: p.detail, icon: "macwindow") { GameLauncher.shared.runProgram(p) }
                    }
                    switch wine.state {
                    case .idle:
                        Text(jit.isReady ? "Output appears in Logs ([stdio] lines)." : "JIT is off: the \"no JIT\" entries run as they are; the others offer to enable JIT.")
                            .font(.caption).foregroundStyle(Deck.dim)
                    case .booting(let t): HStack { ProgressView(); Text("Starting Wine for \(t)…").foregroundStyle(Deck.dim) }
                    case .running(let t): StatusRow(label: "Running \(t)", detail: "Watch Logs for its output.", level: .good)
                    case .finished(let t, let how):
                        StatusRow(label: "\(t) finished",
                                  detail: "\(how). Its output is in Logs. " + (wine.nextGameRefusal() == nil
                                      ? "Another program can start now, in the same Wine session."
                                      : "Restart MYIOSDECK to run another Windows program."),
                                  level: how.contains("error") ? .warn : .good)
                    case .failed(let why): StatusRow(label: "Wine failed", detail: why, level: .bad)
                    }
                }
            }
        }
        .jitGate(pending: $noJITAction)
    }

    private func gated(jit: @escaping () -> Void, noJIT: @escaping () -> Void) {
        if engine.state == .ready { jit() } else { noJITAction = noJIT }
    }

    private func programRow(name: String, detail: String, icon: String = "cpu", action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon).foregroundStyle(Deck.accent).frame(width: 44, height: 44).glassPanel(cornerRadius: 10)
            VStack(alignment: .leading, spacing: 2) {
                Text(name).font(.body.weight(.semibold))
                Text(detail).font(.caption).foregroundStyle(Deck.dim)
            }
            Spacer()
            Button("Play", action: action)
                .buttonStyle(PrimaryButtonStyle(compact: true))
                .disabled(engine.busy != nil)
                .padFocus("dev.\(name)", radius: 20) { if engine.busy == nil { action() } }
        }
    }
}
