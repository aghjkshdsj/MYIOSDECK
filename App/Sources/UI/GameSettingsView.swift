// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// One game's settings (mockup 7): Performance, Controls and Display, over the app defaults.
/// Edited as a draft; Save changes keeps it, and it applies the next time the game starts.
struct GameSettingsView: View {
    let gameID: String
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @Environment(\.isLandscape) private var landscape

    enum Section: String, CaseIterable, Identifiable {
        case performance = "Performance", controls = "Controls", display = "Display"
        var id: String { rawValue }
        var icon: String {
            switch self {
            case .performance: return "gauge.with.dots.needle.67percent"
            case .controls: return "gamecontroller"
            case .display: return "display"
            }
        }
    }

    @State private var section = Section.performance
    @State private var draft = GameSettings()
    @State private var api = "xinput"
    @State private var touch = GameInput.TouchMode.off
    @State private var loaded = false
    @State private var editingLayout = false
    @State private var saved = false
    private let defaults = GameDefaults.current

    private var game: LibraryGame? { GameCatalog.find(gameID, steam: steam, custom: custom) }

    var body: some View {
        GlassPage(titledBack: game?.name ?? "Game", artwork: game?.artwork ?? .blank, scrolls: false,
                  trailing: AnyView(Button("Reset") { reset() }.buttonStyle(.plain).font(.body).accessibilityHint("Back to the app defaults; Save changes keeps it").padFocus("gamesettings.reset", radius: 10) { reset() })) {
            Text("Game settings").font(.largeTitle.weight(.bold))
            if landscape {
                HStack(alignment: .top, spacing: 22) {
                    VStack(alignment: .leading, spacing: 10) {
                        ForEach(Section.allCases) { s in sidebarItem(s) }
                        Spacer(minLength: 0)
                        Text("Changes apply next time you play.").font(.footnote).foregroundStyle(Glass.secondary)
                    }
                    .frame(width: 230)
                    .padSection("sidebar")
                    Rectangle().fill(Glass.separator).frame(width: 1)
                    VStack(alignment: .leading, spacing: 10) {
                        PadScrollView { panel.padding(.bottom, 8) }.scrollIndicators(.hidden)
                        HStack { Spacer(); saveButton }
                    }
                    .padSection("panel")
                }
                .padding(.bottom, 8)
            } else {
                GlassSegmented(options: Section.allCases, selection: $section, compact: true, focusID: "gamesettings.seg1") { $0.rawValue }
                PadScrollView { panel.padding(.bottom, 8) }.scrollIndicators(.hidden)
                VStack(spacing: 8) {
                    Text("Changes apply next time you play.").font(.footnote).foregroundStyle(Glass.secondary)
                    saveButton
                }
                .padding(.bottom, 8)
            }
        }
        .onAppear(perform: load)
        .fullScreenCover(isPresented: $editingLayout) {
            TouchLayoutEditor(gameKey: gameID, title: game?.name ?? "Game")
        }
    }

    private var saveButton: some View {
        Button(saved ? "Saved" : "Save changes") { save() }
            .buttonStyle(PrimaryButtonStyle(fullWidth: !landscape, compact: landscape))
            .padFocus("gamesettings.save", radius: 24) { save() }
    }

    private func sidebarItem(_ s: Section) -> some View {
        Button { withGlassAnimation { section = s } } label: {
            HStack(spacing: 14) {
                Image(systemName: s.icon).font(.title3).frame(width: 30)
                Text(s.rawValue).font(.body)
                Spacer()
                Image(systemName: "chevron.right").font(.subheadline)
            }
            .foregroundStyle(Glass.text)
            .padding(.horizontal, 16).frame(height: 54)
            .background {
                if section == s { Color.clear.glassPanel(cornerRadius: 14, highlighted: true) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(section == s ? .isSelected : [])
        .padFocus("gamesettings.side.\(s.rawValue)", radius: 14, preferred: s == section,
                  onFocus: { withGlassAnimation { section = s } }) { withGlassAnimation { section = s } }
    }

    @ViewBuilder private var panel: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Text(section.rawValue).font(.title.weight(.bold))
                Text(section == .controls ? "This game's controller, touch mouse and on-screen controller."
                     : "Overrides the app defaults for this game.")
                    .foregroundStyle(Glass.secondary)
            }
            switch section {
            case .performance: performance
            case .controls: controls
            case .display: display
            }
        }
    }

    // MARK: performance

    private var cpu: Binding<CPUPreset> {
        Binding(get: { draft.useDefaults ? (defaults.fxr ? .fast : .compatibility) : (draft.cpu ?? (defaults.fxr ? .fast : .compatibility)) },
                set: { v in own(); draft.cpu = v })
    }
    private var frameCap: Binding<Int> {
        Binding(get: { draft.useDefaults ? defaults.frameCap : (draft.frameCap ?? defaults.frameCap) }, set: { v in own(); draft.frameCap = v })
    }
    private var hud: Binding<Bool> {
        Binding(get: { draft.useDefaults ? defaults.performanceHUD : (draft.performanceHUD ?? defaults.performanceHUD) },
                set: { v in own(); draft.performanceHUD = v })
    }
    private var audio: Binding<Bool> {
        Binding(get: { draft.useDefaults ? defaults.audio : (draft.audio ?? defaults.audio) }, set: { v in own(); draft.audio = v })
    }
    private var resolution: Binding<Int> {
        Binding(get: { LaunchOverrides.validResolution(draft.useDefaults ? defaults.resolution : (draft.resolution ?? defaults.resolution)) },
                set: { v in own(); draft.resolution = v })
    }
    private var touchPad: Binding<TouchPadMode> {
        Binding(get: { draft.useDefaults ? defaults.touchPad : (draft.touchPad ?? defaults.touchPad) }, set: { v in own(); draft.touchPad = v })
    }
    /// A change makes the game use its own settings (starting from the defaults it showed).
    private func own() {
        saved = false
        if draft.useDefaults { draft.useDefaults = false }
    }

    @ViewBuilder private var performance: some View {
        GlassCard {
            GlassRow {
                Text("Preset")
                Spacer(minLength: 12)
                GlassSegmented(options: CPUPreset.allCases, selection: cpu, compact: true, focusID: "gamesettings.seg2") { $0.title }.frame(maxWidth: 330)
            }
            GlassRow(last: true) {
                Text("Frame rate")
                Spacer(minLength: 12)
                GlassSegmented(options: [30, 40, 60, 0], selection: frameCap, compact: true, focusID: "gamesettings.seg3") { $0 == 0 ? "Off" : "\($0)" }
                    .frame(maxWidth: 330)
            }
        }
        Text("Preset, without JIT: Fast runs the game's x64 code with FXR, Compatibility with FXI (for a game that misbehaves only with FXR); the first 64-bit game since MYIOSDECK opened sets it for the later ones. Frame rate caps Direct3D 10 and 11 games (Off leaves the pace to the game).")
            .font(.footnote).foregroundStyle(Glass.secondary)
        GlassCard {
            GlassToggleRow(title: "Show performance overlay", isOn: hud)
            GlassToggleRow(title: "Game audio without JIT", isOn: audio)
            GlassToggleRow(title: "Use app defaults", isOn: Binding(get: { draft.useDefaults }, set: { draft.useDefaults = $0; saved = false }), last: true)
        }
        Text("The overlay is Metal's own: frame rate, frame time and GPU time. Game audio off starts the game with no audio device, which saves a lot of CPU without JIT.")
            .font(.footnote).foregroundStyle(Glass.secondary)
    }

    // MARK: controls

    @ViewBuilder private var controls: some View {
        GlassCard {
            GlassRow {
                Text("Controller")
                Spacer()
                Picker("Controller", selection: Binding(get: { api }, set: { api = $0; saved = false })) {
                    ForEach(GameSessionHost.controllerOrder, id: \.self) { Text(GameSessionHost.controllerNames[$0] ?? $0).tag($0) }
                }
                .pickerStyle(.menu)
                .padFocus("gamesettings.controller", radius: 10, adjust: { d in
                    let all = GameSessionHost.controllerOrder, i = all.firstIndex(of: api) ?? 0
                    guard let n = ChoiceStep.index(i, count: all.count, delta: d) else { return false }
                    api = all[n]; saved = false
                    return true
                }) {
                    showActions("Controller", GameSessionHost.controllerOrder.map { a in
                        DialogButton(title: (a == api ? "✓ " : "") + (GameSessionHost.controllerNames[a] ?? a)) { api = a; saved = false }
                    })
                }
            }
            GlassRow {
                Text("Touch mouse")
                Spacer(minLength: 12)
                GlassSegmented(options: GameInput.TouchMode.allCases, selection: Binding(get: { touch }, set: { touch = $0; saved = false }),
                               compact: true, focusID: "gamesettings.seg4") { $0 == .off ? "Off" : $0 == .trackpad ? "Trackpad" : "Tap" }
                    .frame(maxWidth: 330)
            }
            GlassRow(last: true) {
                Text("On-screen controller")
                Spacer(minLength: 12)
                GlassSegmented(options: TouchPadMode.allCases, selection: touchPad, compact: true, focusID: "gamesettings.seg5") { $0.title }.frame(maxWidth: 330)
            }
        }
        Text("Controller: how the game sees a controller (XInput suits most games; DirectInput and HID start with the game). Touch mouse: Trackpad moves a cursor, Tap clicks where you touch; a three-finger tap opens the game menu in every mode. On-screen controller: Automatic hides it while a controller is connected.")
            .font(.footnote).foregroundStyle(Glass.secondary)
        HStack(spacing: 12) {
            Button { editingLayout = true } label: { Label("Edit on-screen layout", systemImage: "square.and.pencil") }
                .buttonStyle(GlassButtonStyle(compact: true))
                .padFocus("gamesettings.editLayout", radius: 18) { editingLayout = true }
            if TouchLayoutStore.hasOwnLayout(gameID) {
                Button { TouchLayoutStore.removeOwnLayout(gameID); saved = false } label: { Text("Use the default layout") }
                    .buttonStyle(GlassButtonStyle(compact: true))
                    .padFocus("gamesettings.defaultLayout", radius: 18) { TouchLayoutStore.removeOwnLayout(gameID); saved = false }
            }
        }
        Text(TouchLayoutStore.hasOwnLayout(gameID) ? "This game has its own on-screen layout." : "This game uses the default on-screen layout (Settings › Controls).")
            .font(.footnote).foregroundStyle(Glass.secondary)
    }

    // MARK: display

    @ViewBuilder private var display: some View {
        GlassCard {
            GlassRow(last: true) {
                Text("Resolution")
                Spacer(minLength: 12)
                GlassSegmented(options: LaunchOverrides.resolutions, selection: resolution, compact: true, focusID: "gamesettings.seg6") { "\($0)p" }.frame(maxWidth: 360)
            }
        }
        Text("The height of the screen Windows sees (its width follows this iPhone's shape). Lower is lighter for the GPU, higher is sharper. Wine reads it once per session: with Play another game without restarting, the first game's size stays until MYIOSDECK closes.")
            .font(.footnote).foregroundStyle(Glass.secondary)
    }

    // MARK: load / save

    private func load() {
        guard !loaded else { return }
        loaded = true
        draft = GameSettingsStore.load(gameID)
        api = UserDefaults.standard.string(forKey: "controllerAPI.\(gameID)") ?? UserDefaults.standard.string(forKey: "controllerAPI") ?? "xinput"
        touch = GameInput.touchMode(gameKey: gameID)
    }

    private func reset() {
        draft = GameSettings()
        api = UserDefaults.standard.string(forKey: "controllerAPI") ?? "xinput"
        touch = .off
        saved = false
    }

    private func save() {
        GameSettingsStore.save(draft, for: gameID)
        let d = UserDefaults.standard
        // Kept under the game even when equal to the default: an older choice kept under the
        // game's program (before games had keys) must not win over this one.
        d.set(api, forKey: "controllerAPI.\(gameID)")
        d.set(touch.rawValue, forKey: "touchMode.\(gameID)")
        dlog("[game-settings] saved for \(gameID): defaults=\(draft.useDefaults ? 1 : 0) api=\(api) touch=\(touch.rawValue)")
        withGlassAnimation { saved = true }
        UINotificationFeedbackGenerator().notificationOccurred(.success)
    }
}
