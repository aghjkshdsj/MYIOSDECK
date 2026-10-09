// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The game library. Until the Wine and Steam stages land, it holds the
/// built-in x86-64 programs and shows exactly what each stage still needs.
struct LibraryView: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var wine: WineController
    @EnvironmentObject private var jit: JITController
    @State private var noJITAction: (() -> Void)?
    @State private var surfaceTitle = ""
    @State private var showSurface = false
    @State private var showSteamSignIn = false
    @ObservedObject private var steam = SteamSignInModel.shared
    @ObservedObject private var steamLibrary = SteamLibrary.shared

    private struct Stage: Identifiable {
        let id: Int
        let title: String
        let detail: String
        let done: Bool
    }

    private let stages = [
        Stage(id: 1, title: "JIT + FEX x86-64 translation", detail: "StikDebug JIT pool, FEXCore tuned to the A17 Pro, benchmarks, plus a no-JIT interpreter. Verified on iPhone 15 Pro Max / iOS 27.", done: true),
        Stage(id: 2, title: "Wine ARM64EC (Windows)", detail: "Wine running natively on ARM64 in-process, wineserver as a thread, FEX as xtajit64.dll for x86-64 code. Verified on device (build 20).", done: true),
        Stage(id: 3, title: "Direct3D 9/10/11/12 on Metal", detail: "DXMT for D3D9-11 and Madeira's D3D12 converter. D3D11 and D3D12 cubes render at 60 fps on device (builds 21-22).", done: true),
        Stage(id: 4, title: "Steam", detail: "Steam sign-in (QR or password + Steam Guard), owned library, depot downloads, cloud saves, launch through Valve's Windows client.", done: false),
        Stage(id: 5, title: "Deck input", detail: "Bluetooth controllers as XInput, touch controls, keyboard and mouse.", done: false),
    ]

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                DeckCard(title: "Installed (built-in x86-64 programs)", icon: "square.grid.2x2.fill") {
                    programRow(name: "x86-64 Hello", detail: "Prints the CPU the guest sees (CPUID) and times a loop") {
                        gated(jit: { engine.runHello(mode: .jit) }, noJIT: { engine.runHello(mode: settings.linuxNoJITMode) })
                    }
                    Divider().overlay(Deck.panelHi)
                    programRow(name: "CPU benchmark suite", detail: "Native vs FEX vs no-JIT, five kernels (also on Performance)") {
                        let preset = settings.fexPreset.title
                        gated(jit: { engine.runBenchmarks(fex: true, interpreter: settings.benchInterpreter, presetName: preset) },
                              noJIT: { engine.runBenchmarks(fex: false, interpreter: true, presetName: preset) })
                    }
                }
                windowsCard
                steamCard
                DeckCard(title: "Road to Steam games", icon: "map.fill") {
                    ForEach(stages) { s in
                        StatusRow(label: "Stage \(s.id): \(s.title)", detail: s.detail, level: s.done ? .good : .idle)
                    }
                }
            }
            .padding(16)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
        .jitGate(pending: $noJITAction)
        .alert("You are running this without JIT", isPresented: noJITGamePresented) {
            Button("Enable JIT") {
                let launch = noJITGame
                noJITGame = nil
                jit.enableWithStikDebug(poolMB: settings.jitPoolMB) {
                    engine.start(settings: settings)
                    DispatchQueue.main.async { launch?() }   // the game starts once JIT is ready
                }
            }
            Button("Cancel", role: .cancel) { noJITGame = nil }
        } message: {
            Text("This test program uses the JIT path (Wine code copied into JIT memory, x64 code through FEX). The entries marked \"no JIT\" and Steam games can run without JIT. Enable JIT and the program starts right after.")
        }
        .fullScreenCover(isPresented: $showSurface) { gameSurface }
        .onChange(of: wine.state) { _, state in
            // A crashed program: close the surface so the crash report can show.
            if case .finished(_, let how) = state, how.contains("error") { showSurface = false }
        }
    }

    /// Full-screen surface for a Windows program's Direct3D window, edge to edge.
    /// The menu button is drawn by GameHostView (the Metal host is above every
    /// SwiftUI view); while the menu is open the host is hidden so it shows.
    private var gameSurface: some View {
        GameSurfaceView()
            .ignoresSafeArea()
            .background(Color.black)
            .statusBarHidden()
            .persistentSystemOverlays(.hidden)
            .onAppear {
                GameHostView.onMenu = {
                    GameHostView.shared.isHidden = true
                    showGameMenu = true
                }
            }
            .confirmationDialog(surfaceTitle, isPresented: $showGameMenu, titleVisibility: .visible) {
                Button("Resume") {}
                Button("Controller: \(Self.controllerNames[settings.controllerAPI] ?? "XInput")…") {
                    openingControllerMenu = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { showControllerMenu = true }
                }
                Button("Hide game (keeps running)") { showSurface = false }
                Button("Quit game and close MYIOSDECK", role: .destructive) { CrashReporter.shared.quitApp() }
            } message: {
                Text("A Windows game can only be stopped by closing MYIOSDECK; unsaved progress is lost.")
            }
            .confirmationDialog("Controller", isPresented: $showControllerMenu, titleVisibility: .visible) {
                ForEach(Self.controllerOrder, id: \.self) { api in
                    let current = api == settings.controllerAPI
                    let later = (api == "hid" || api == "dualsense") && api != wine.sessionAPI
                    Button((current ? "✓ " : "") + (Self.controllerNames[api] ?? api) + (later ? " (next launch)" : "")) {
                        settings.controllerAPI = api
                        wine.switchController(to: api)
                    }
                }
            } message: {
                Text("XInput and Keyboard and mouse switch at once. HID modes need the game to start with them, so they apply at the next launch.")
            }
            .onChange(of: showGameMenu) { _, open in
                if !open, showSurface, !openingControllerMenu { GameHostView.shared.isHidden = false }
            }
            .onChange(of: showControllerMenu) { _, open in
                if open { openingControllerMenu = false }
                if !open, showSurface { GameHostView.shared.isHidden = false }
            }
    }

    @State private var showGameMenu = false
    @State private var showControllerMenu = false
    @State private var openingControllerMenu = false

    private static let controllerOrder = ["xinput", "dinput", "hid", "dualsense", "keyboard"]
    private static let controllerNames = [
        "xinput": "Xbox (XInput)", "dinput": "XInput + DirectInput", "hid": "HID gamepad",
        "dualsense": "DualSense (PS5)", "keyboard": "Keyboard and mouse",
    ]

    /// Stage 4: Steam account (SwiftSteam, from Madeira). The owned library and
    /// downloads build on this sign-in.
    private var steamCard: some View {
        DeckCard(title: "Steam", icon: "gamecontroller.fill") {
            if let name = steam.accountName {
                HStack {
                    StatusRow(label: "Signed in as \(name)", detail: "Tap a game to install or play it.", level: .good)
                    Button("Sign out") { steam.signOut() }
                        .buttonStyle(DeckButtonStyle())
                        .frame(width: 110)
                }
                Divider().overlay(Deck.panelHi)
                SteamGamesGrid(library: steamLibrary, playBlocker: steamPlayBlocker, onPlay: { chooseSteamLaunch($0) })
                if let dockProgress {
                    HStack { ProgressView(); Text(dockProgress).font(.subheadline).foregroundStyle(Deck.dim) }
                }
                if let playError {
                    StatusRow(label: "Could not start the game", detail: playError, level: .bad)
                }
                if let failure = wine.dockFailure {
                    StatusRow(label: "Steam (Madeira Dock) did not start the game", detail: failure, level: .bad)
                }
            } else {
                Text("Sign in with your Steam account name and password (Steam Guard supported) or a QR code from the Steam app. The sign-in token stays in this device's Keychain; the password is never stored.")
                    .font(.subheadline).foregroundStyle(Deck.dim)
                Button("Sign in to Steam") { showSteamSignIn = true }
                    .buttonStyle(DeckButtonStyle())
            }
        }
        .sheet(isPresented: $showSteamSignIn) { SteamSignInView() }
        .onAppear { steamLibrary.start() }
        .confirmationDialog(steamChoiceGame.map { "Start \($0.name)" } ?? "", isPresented: steamChoicePresented,
                            titleVisibility: .visible, presenting: steamChoiceGame) { game in
            Button("With Steam") { playSteam(game, viaDock: true) }
            Button("Without Steam") { playSteam(game, viaDock: false) }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("With Steam: Valve's Steam client signs in and starts the game, for games that need Steam running (without JIT the sign-in takes several minutes). Without Steam: the game starts on its own, faster. Settings › Steam can make either the default.")
        }
        .alert("JIT is off", isPresented: steamNoJITPresented, presenting: steamNoJITGame) { game in
            Button("Play without JIT (experimental)") { playSteam(game, noJIT: true, viaDock: steamPendingDock) }
            Button("Enable JIT") {
                let viaDock = steamPendingDock
                jit.enableWithStikDebug(poolMB: settings.jitPoolMB) {
                    engine.start(settings: settings)
                    DispatchQueue.main.async { playSteam(game, viaDock: viaDock) }   // the game starts once JIT is ready
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: { _ in
            Text("Without JIT, Wine loads from signed app files and the game's x64 code runs in MYIOSDECK's interpreter (FXI), about 10× slower than native. Small 64-bit Direct3D 11/12 games may run; 32-bit games need JIT. If it stops, send myiosdeck-log.txt from the Logs tab.")
        }
    }

    /// A Steam game waiting for the JIT / no-JIT choice.
    @State private var steamNoJITGame: OwnedSteamGame?
    private var steamNoJITPresented: Binding<Bool> {
        Binding(get: { steamNoJITGame != nil }, set: { if !$0 { steamNoJITGame = nil } })
    }

    @State private var playError: String?
    /// A Windows launch waiting for JIT (the no-JIT alert).
    @State private var noJITGame: (() -> Void)?
    private var noJITGamePresented: Binding<Bool> {
        Binding(get: { noJITGame != nil }, set: { if !$0 { noJITGame = nil } })
    }

    /// Runs a Windows launch now, or asks to enable JIT first.
    private func withJIT(_ launch: @escaping () -> Void) {
        if jit.isReady { launch() } else { noJITGame = launch }
    }

    private var steamPlayBlocker: String? {
        if !wine.linked { return "This build does not include Wine." }
        switch wine.state {
        case .booting, .running, .finished: return "Wine already ran a program in this session. Restart MYIOSDECK to play."
        default: return nil
        }
    }

    /// A Steam game waiting for the with / without Steam choice.
    @State private var steamChoiceGame: OwnedSteamGame?
    private var steamChoicePresented: Binding<Bool> {
        Binding(get: { steamChoiceGame != nil }, set: { if !$0 { steamChoiceGame = nil } })
    }
    /// The with / without Steam choice of the game waiting in the JIT alert.
    @State private var steamPendingDock = false

    /// Play: with Steam (Madeira Dock) or without, as Settings › Steam says, or asked each time.
    private func chooseSteamLaunch(_ game: OwnedSteamGame) {
        guard MadeiraDock.bundled else { playSteam(game, viaDock: false); return }
        switch settings.steamLaunchMode {
        case "dock": playSteam(game, viaDock: true)
        case "direct": playSteam(game, viaDock: false)
        default:
            // Let the game sheet close before the dialog is presented.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { steamChoiceGame = game }
        }
    }

    /// Stage 4d: start of an installed Steam game on the game surface, through Valve's Steam
    /// client (viaDock) or directly. Without JIT it asks first: play without JIT (FXI) or enable JIT.
    private func playSteam(_ game: OwnedSteamGame, noJIT: Bool = false, viaDock: Bool) {
        playError = nil
        guard jit.isReady || noJIT else {
            steamPendingDock = viaDock
            // Let the game sheet (or the choice dialog) close before the alert is presented.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { steamNoJITGame = game }
            return
        }
        Task { @MainActor in
            do {
                let plan = try await steamLibrary.launchPlan(game.id)
                if MadeiraDock.bundled && viaDock {
                    try await startThroughDock(game, plan: plan)
                } else {
                    wine.runSteamGame(appID: game.id, title: game.name, plan: plan)
                }
                if case .failed(let why) = wine.state { playError = why; return }
                guard case .booting = wine.state else { return }
                // Let the game sheet finish closing before the surface is presented.
                try? await Task.sleep(for: .milliseconds(600))
                surfaceTitle = game.name
                showSurface = true
            } catch {
                playError = error.localizedDescription
            }
        }
    }

    /// Progress of the one-time download of Valve's client components (Madeira Dock).
    @State private var dockProgress: String?

    /// Madeira Dock: Valve's own Steam client starts the game (MadeiraDock.swift). The first
    /// time, Valve's client components come from Valve's update servers (pinned sizes and
    /// SHA-256, about 72 MB); then the one-use sign-in transfer is written, MYIOSDECK's own
    /// Steam connection logs off (Dock becomes this account's client) and the host starts.
    @MainActor private func startThroughDock(_ game: OwnedSteamGame, plan: SteamLaunchPlan) async throws {
        if !MadeiraDock.clientInstalled {
            dockProgress = "Downloading Steam components from Valve…"
            defer { dockProgress = nil }
            try await SteamRuntimeInstaller.shared.prepare(prefix: wine.prefixURL) { text in
                await MainActor.run { dockProgress = text }
            }
            dlog("[dock-setup] Valve's client components installed")
        }
        guard let dockGame = MadeiraDock.games(drive: MadeiraDock.drive).first(where: { $0.id == game.id }) else {
            throw DockError.message("Steam's install record for this game was not found. Reinstall the game from the Steam library.")
        }
        try MadeiraDock.validate(dockGame, drive: MadeiraDock.drive)
        guard let sign = SteamSignIn.credentialsForDock() else {
            throw DockError.message("Your Steam sign-in has expired. Sign in to Steam again.")
        }
        await steamLibrary.endSessionForDock()
        try MadeiraDock.writeHandoff(account: sign.accountName, token: sign.refreshToken, appID: game.id)
        wine.runDockGame(dockGame, launchOption: plan.launchIndex, title: game.name)
        if case .booting = wine.state { return }
        MadeiraDock.cleanup()
    }

    private var windowsCard: some View {
        DeckCard(title: "Windows programs (Wine + Direct3D)", icon: "macwindow") {
            if !wine.linked {
                Text("Wine is being built for iOS in CI (Stage 2). Once it links, Windows x64 programs run here through Wine ARM64EC + FEX.")
                    .font(.subheadline).foregroundStyle(Deck.dim)
            } else {
                ForEach(WineController.programs) { p in
                    HStack(spacing: 12) {
                        RoundedRectangle(cornerRadius: 8).fill(Deck.accent.opacity(0.25)).frame(width: 52, height: 52)
                            .overlay(Image(systemName: "macwindow").foregroundStyle(Deck.accent))
                        VStack(alignment: .leading, spacing: 2) {
                            Text(p.title).font(.body.weight(.semibold))
                            Text(p.detail).font(.caption).foregroundStyle(Deck.dim)
                        }
                        Spacer()
                        Button("Play") {
                            let launch = {
                                wine.run(p)
                                if p.graphics, case .booting = wine.state {
                                    surfaceTitle = p.title
                                    showSurface = true
                                }
                            }
                            if p.noJIT { launch() } else { withJIT(launch) }
                        }
                            .buttonStyle(DeckButtonStyle())
                            .frame(width: 90)
                    }
                }
                if !surfaceTitle.isEmpty, !showSurface {
                    Button("Show \(surfaceTitle)") { showSurface = true }
                        .buttonStyle(DeckButtonStyle())
                }
                switch wine.state {
                case .idle:
                    Text(jit.isReady ? "Output appears in the Logs tab ([stdio] lines)." : "JIT is off: the \"no JIT\" entries run as they are; the others offer to enable JIT.")
                        .font(.caption).foregroundStyle(Deck.dim)
                case .booting(let t): HStack { ProgressView(); Text("Starting Wine for \(t)…").foregroundStyle(Deck.dim) }
                case .running(let t): StatusRow(label: "Running \(t)", detail: "Watch the Logs tab for its output.", level: .good)
                case .finished(let t, let how): StatusRow(label: "\(t) finished", detail: "\(how). Its output is in the Logs tab. Restart MYIOSDECK to run another Windows program.", level: how.contains("error") ? .warn : .good)
                case .failed(let why): StatusRow(label: "Wine failed", detail: why, level: .bad)
                }
            }
        }
    }

    private func gated(jit: @escaping () -> Void, noJIT: @escaping () -> Void) {
        if engine.state == .ready { jit() } else { noJITAction = noJIT }
    }

    private func programRow(name: String, detail: String, action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            RoundedRectangle(cornerRadius: 8).fill(Deck.accent.opacity(0.25)).frame(width: 52, height: 52)
                .overlay(Image(systemName: "cpu").foregroundStyle(Deck.accent))
            VStack(alignment: .leading, spacing: 2) {
                Text(name).font(.body.weight(.semibold))
                Text(detail).font(.caption).foregroundStyle(Deck.dim)
            }
            Spacer()
            Button("Play", action: action)
                .buttonStyle(DeckButtonStyle())
                .frame(width: 90)
                .disabled(engine.busy != nil)
        }
    }
}
