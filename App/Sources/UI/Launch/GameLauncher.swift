// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Starts games from anywhere in the app (Home, Library, a game's page, Search): the JIT / no-JIT
/// choice, With / Without Steam, Steam Cloud before play, Madeira Dock's first-time setup, and
/// the errors. Its questions are shown by LaunchAlerts (on the root view); the game's view by
/// GameSessionHost. Moved here from the old Library tab, unchanged in what it does.
@MainActor
final class GameLauncher: ObservableObject {
    static let shared = GameLauncher()

    private var settings: Settings!
    private var jit: JITController!
    private var engine: EngineController!
    private var wine: WineController!
    private var steamLibrary: SteamLibrary { SteamLibrary.shared }
    private var session: GameSession { GameSession.shared }

    /// Called once by the root view with the app's controllers.
    func attach(settings: Settings, jit: JITController, engine: EngineController, wine: WineController) {
        self.settings = settings; self.jit = jit; self.engine = engine; self.wine = wine
    }

    // MARK: state the alerts show

    /// A Steam game waiting for the JIT / no-JIT choice, and its With / Without Steam choice.
    @Published var steamNoJITGame: OwnedSteamGame?
    @Published var steamPendingDock = false
    /// A Steam game waiting for the with / without Steam choice.
    @Published var steamChoiceGame: OwnedSteamGame?
    /// An added game waiting for the JIT / no-JIT choice.
    @Published var customNoJITGame: CustomGame?
    /// A Windows test program waiting for JIT (it uses the JIT path).
    @Published var noJITProgram: WineController.Program?
    /// Why the last start failed.
    @Published var playError: String?
    /// Progress of the one-time download of Valve's client components (Madeira Dock).
    @Published var dockProgress: String?
    /// The game whose saves sync before it starts.
    @Published var cloudSyncing: String?
    /// A Steam game whose start waits for the user: its Steam Cloud saves could not be synced,
    /// or some need a choice.
    @Published var cloudHold: CloudHold?
    /// The game whose Steam Cloud page is open from the hold alert.
    @Published var cloudPage: OwnedSteamGame?

    struct CloudHold: Identifiable {
        let game: OwnedSteamGame
        let noJIT: Bool
        let viaDock: Bool
        let hold: SteamCloudHold
        var id: Int { game.id }
    }

    /// Something is happening before the game's view appears (shown on Home and the game page).
    var busyText: String? {
        if let dockProgress { return dockProgress }
        if let cloudSyncing { return "Syncing Steam Cloud saves of \(cloudSyncing)…" }
        return nil
    }

    // MARK: blockers

    /// Why Play is unavailable now (a game is running, or another cannot start in this Wine
    /// session), or nil.
    var playBlocker: String? {
        guard let wine else { return nil }
        if !wine.linked { return "This build does not include Wine." }
        switch wine.state {
        case .booting, .running:
            return "\(session.title.isEmpty ? "A game" : session.title) is still running: return to it from the top of the screen."
        default:
            // After a game: another can start in the same Wine session (session reuse), or not.
            return wine.programsThisRun > 0 ? wine.nextGameRefusal() : nil
        }
    }

    /// Installed games cannot be deleted while a Windows game runs (it may be one of them).
    var deleteBlocker: String? {
        guard let wine else { return nil }
        switch wine.state {
        case .booting, .running: return "Quit the running game to delete an installed one."
        default: return nil
        }
    }

    /// Why this game cannot be played now, including its own reasons (not installed, not owned).
    func blocker(for game: LibraryGame) -> String? {
        if let id = game.steamID {
            if steamLibrary.game(id) == nil {
                return steamLibrary.signedIn ? "This account does not list the game." : "Sign in to Steam (Settings › Steam) to play it."
            }
            if !GameCatalog.isInstalled(game, steam: steamLibrary) { return "Install it first." }
        }
        return playBlocker
    }

    // MARK: play

    func play(_ game: LibraryGame) {
        playError = nil
        switch game.kind {
        case .steam(let id):
            guard let owned = steamLibrary.game(id) else { playError = blocker(for: game); return }
            chooseSteamLaunch(owned)
        case .custom(let g):
            playCustom(g)
        }
    }

    /// Starts an added DRM-free game (CustomGames). Without JIT it asks first: play without JIT
    /// (FXI, 64-bit; 32-bit through WoW64) or enable JIT.
    func playCustom(_ game: CustomGame, noJIT: Bool = false) {
        playError = nil
        guard jit.isReady || noJIT else { customNoJITGame = game; return }
        GameSettingsStore.prepareLaunch(RecentlyPlayed.custom(game.id), wine: wine)
        wine.runCustomGame(game)
        if case .failed(let why) = wine.state { playError = why; return }
        guard case .booting = wine.state else { return }
        RecentlyPlayed.shared.note(RecentlyPlayed.custom(game.id))
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(600))   // let the alert finish closing
            session.title = game.name
            session.gameID = RecentlyPlayed.custom(game.id)
            session.shown = true
        }
    }

    /// Play: with Steam (Madeira Dock) or without, as Settings › Steam says, or asked each time.
    func chooseSteamLaunch(_ game: OwnedSteamGame) {
        guard MadeiraDock.bundled else { playSteam(game, viaDock: false); return }
        switch settings.steamLaunchMode {
        case "dock": playSteam(game, viaDock: true)
        case "direct": playSteam(game, viaDock: false)
        default:
            // Let a sheet close before the dialog is presented.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { self.steamChoiceGame = game }
        }
    }

    /// An installed Steam game started on the game surface, through Valve's Steam client
    /// (viaDock) or directly. Without JIT it asks first: play without JIT (FXI) or enable JIT.
    /// Its Steam Cloud saves sync first (SteamCloudSync); a sync that fails or leaves saves to
    /// choose holds the start (`skipCloud`: the user chose to play without it).
    func playSteam(_ game: OwnedSteamGame, noJIT: Bool = false, viaDock: Bool, skipCloud: Bool = false) {
        playError = nil
        guard jit.isReady || noJIT else {
            steamPendingDock = viaDock
            // Let the choice dialog close before the alert is presented.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { self.steamNoJITGame = game }
            return
        }
        Task { @MainActor in
            if !skipCloud, SteamCloudSync.enabled {
                cloudSyncing = game.name
                let hold = await SteamCloudSync.shared.beforePlay(game.id)
                cloudSyncing = nil
                if let hold {
                    try? await Task.sleep(for: .milliseconds(600))
                    cloudHold = CloudHold(game: game, noJIT: noJIT, viaDock: viaDock, hold: hold)
                    return
                }
            }
            do {
                let plan = try await steamLibrary.launchPlan(game.id)
                let dock = MadeiraDock.bundled && viaDock
                GameSettingsStore.prepareLaunch(RecentlyPlayed.steam(game.id), wine: wine)
                if dock {
                    try await startThroughDock(game, plan: plan)
                } else {
                    wine.runSteamGame(appID: game.id, title: game.name, plan: plan)
                }
                if case .failed(let why) = wine.state { playError = why; return }
                guard case .booting = wine.state else { return }
                SteamCloudSync.shared.gameStarting(game.id, synced: !skipCloud, dock: dock)
                RecentlyPlayed.shared.note(RecentlyPlayed.steam(game.id))
                try? await Task.sleep(for: .milliseconds(600))
                session.title = game.name
                session.gameID = RecentlyPlayed.steam(game.id)
                session.shown = true
            } catch {
                playError = error.localizedDescription
            }
        }
    }

    /// Madeira Dock: Valve's own Steam client starts the game (MadeiraDock.swift). The first
    /// time, Valve's client components come from Valve's update servers (pinned sizes and
    /// SHA-256, about 72 MB); then the one-use sign-in transfer is written, MYIOSDECK's own
    /// Steam connection logs off (Dock becomes this account's client) and the host starts.
    private func startThroughDock(_ game: OwnedSteamGame, plan: SteamLaunchPlan) async throws {
        if !MadeiraDock.clientInstalled {
            dockProgress = "Downloading Steam components from Valve…"
            defer { dockProgress = nil }
            try await SteamRuntimeInstaller.shared.prepare(prefix: wine.prefixURL) { text in
                await MainActor.run { GameLauncher.shared.dockProgress = text }
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
        // Steam's UDP/TCP servers into Valve's client's cache: without JIT it cannot fetch them itself.
        await DockCMCache.prepare(drive: MadeiraDock.drive)
        // Steam Cloud makes no request while Valve's client holds the account (until the game ends).
        SteamCloudSync.shared.holdForDock()
        await steamLibrary.endSessionForDock()
        do {
            try MadeiraDock.writeHandoff(account: sign.accountName, token: sign.refreshToken, appID: game.id)
        } catch {
            SteamCloudSync.shared.releaseDock()
            throw error
        }
        wine.runDockGame(dockGame, launchOption: plan.launchIndex, title: game.name)
        if case .booting = wine.state { return }
        MadeiraDock.cleanup()
        SteamCloudSync.shared.releaseDock()
    }

    /// A Windows test program (Settings › About › Test programs): JIT-path ones ask for JIT first.
    func runProgram(_ p: WineController.Program) {
        if p.noJIT || jit.isReady { startProgram(p) } else { noJITProgram = p }
    }

    private func startProgram(_ p: WineController.Program) {
        wine.run(p)
        if p.graphics, case .booting = wine.state {
            session.title = p.title
            session.gameID = ""
            session.shown = true
        }
    }

    /// Enable JIT (StikDebug), then run `then` once it is ready.
    func enableJIT(then: (() -> Void)? = nil) {
        jit.enableWithStikDebug(poolMB: settings.jitPoolMB) { [self] in
            engine.start(settings: settings)
            if let then { DispatchQueue.main.async(execute: then) }   // the game starts once JIT is ready
        }
    }

    fileprivate func startWithJIT(_ p: WineController.Program) {
        enableJIT { [weak self] in self?.startProgram(p) }
    }

    static func holdTitle(_ hold: SteamCloudHold) -> String {
        switch hold {
        case .failed: return "Steam Cloud saves were not synced"
        case .conflicts(let n): return n == 1 ? "A save needs your choice" : "\(n) saves need your choice"
        }
    }

    static func holdMessage(_ hold: SteamCloudHold) -> String {
        let anyway = "Play without syncing starts the game with this iPhone's saves; what it changes is not uploaded over cloud saves that changed meanwhile (those wait for your choice at the next sync)."
        switch hold {
        case .failed(let why):
            return "\(why)\n\n\(anyway)"
        case .conflicts:
            return "This iPhone and Steam Cloud have different versions, and both changed since the last sync (or there was no sync yet). Choose which to keep on the game's Steam Cloud page; the other version is kept in save-backups.\n\n\(anyway)"
        }
    }
}

/// The launcher's questions, on the root view so they show wherever Play was pressed (glass dialogs:
/// a controller can answer them).
struct LaunchAlerts: ViewModifier {
    @ObservedObject private var launcher = GameLauncher.shared

    func body(content: Content) -> some View {
        content
            .glassDialog(item: $launcher.steamChoiceGame, title: { "Start \($0.name)" }, message: { _ in
                "With Steam: Valve's Steam client signs in and starts the game, for games that need Steam running (without JIT the sign-in takes several minutes). Without Steam: the game starts on its own, faster. Settings › Steam can make either the default."
            }) { game in
                [DialogButton(title: "With Steam") { launcher.playSteam(game, viaDock: true) },
                 DialogButton(title: "Without Steam") { launcher.playSteam(game, viaDock: false) },
                 .cancel()]
            }
            .glassDialog(item: $launcher.steamNoJITGame, title: { _ in "JIT is off" }, message: { _ in
                "Without JIT, Wine loads from signed app files and the game's x64 code runs in MYIOSDECK's interpreter (FXI), about 10× slower than native. Small 64-bit Direct3D 11/12 games may run; 32-bit games are experimental. If it stops, send myiosdeck-log.txt from Settings › Logs."
            }) { game in
                let viaDock = launcher.steamPendingDock
                return [DialogButton(title: "Play without JIT (experimental)") { launcher.playSteam(game, noJIT: true, viaDock: viaDock) },
                        DialogButton(title: "Enable JIT") { launcher.enableJIT { launcher.playSteam(game, viaDock: viaDock) } },
                        .cancel()]
            }
            .glassDialog(item: $launcher.customNoJITGame, title: { _ in "JIT is off" }, message: { _ in
                "Without JIT the game's x86 code runs in MYIOSDECK's interpreter (FXI), about 10× slower than native. 32-bit games run through WoW64 (experimental, and slower still: their Windows libraries are interpreted too). With JIT (StikDebug) everything runs through FEX at full speed."
            }) { game in
                [DialogButton(title: "Play without JIT (experimental)") { launcher.playCustom(game, noJIT: true) },
                 DialogButton(title: "Enable JIT") { launcher.enableJIT { launcher.playCustom(game) } },
                 .cancel()]
            }
            .glassDialog(item: $launcher.noJITProgram, title: { _ in "You are running this without JIT" }, message: { _ in
                "This test program uses the JIT path (Wine code copied into JIT memory, x64 code through FEX). The entries marked \"no JIT\" and Steam games can run without JIT. Enable JIT and the program starts right after."
            }) { p in
                [DialogButton(title: "Enable JIT") { launcher.startWithJIT(p) }, .cancel()]
            }
            .glassDialog(item: $launcher.cloudHold, title: { GameLauncher.holdTitle($0.hold) }, message: { GameLauncher.holdMessage($0.hold) }) { h in
                var b: [DialogButton] = []
                switch h.hold {
                case .conflicts: b.append(DialogButton(title: "Choose…") { launcher.cloudPage = h.game })
                case .failed: b.append(DialogButton(title: "Try again") { launcher.playSteam(h.game, noJIT: h.noJIT, viaDock: h.viaDock) })
                }
                b.append(DialogButton(title: "Play without syncing") { launcher.playSteam(h.game, noJIT: h.noJIT, viaDock: h.viaDock, skipCloud: true) })
                b.append(.cancel())
                return b
            }
            .glassDialog(item: $launcher.playError, title: { _ in "Could not start the game" }, message: { $0 }) { _ in
                [DialogButton(title: "OK", role: .cancel)]
            }
            .sheet(item: $launcher.cloudPage) { game in SteamCloudPage(appID: game.id, name: game.name) }
    }
}
