// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation
import SwiftUI

@MainActor enum TrackBSessionGate {
    static var reserved = false
}

@MainActor enum TrackBSteamHostIntegration {
    static func play(_ appID: Int, title: String, wine: WineController, progress: @escaping (String?) -> Void) async throws {
        guard !TrackBSessionGate.reserved, mid_wine_running() == 0, wine.programsThisRun == 0 else {
            throw SteamError.files("Restart MYIOSDECK before opening another Windows session.")
        }
        TrackBSessionGate.reserved = true
        defer { progress(nil) }
        var started = false
        defer { if !started { release() } }
        let library = SteamLibrary.shared
        // Admission closes before cancellation; wait until every file worker has unwound.
        library.active?.task.cancel()
        while library.active != nil { try await Task.sleep(nanoseconds: 50_000_000) }
        SteamCloudSync.shared.holdForDock()
        await library.session.disconnectGracefully()
        let prefix = wine.prefixURL
        prefix.path.withCString { mid_wine_seed_prefix($0) }
        if !SteamHost.clientInstalled(prefix: prefix) {
            progress("Downloading Steam's installer…")
            let descriptor = try await SteamHost.prepareInstall(prefix: prefix) { text in
                Task { @MainActor in GameLauncher.shared.dockProgress = text }
            }
            wine.runSteamHost(descriptor, title: "Install Steam")
            guard case .booting = wine.state else { throw SteamError.files("Steam's installer could not start.") }
            GameSession.shared.title = "Install Steam"; GameSession.shared.shown = true
            while true {
                try await Task.sleep(nanoseconds: 250_000_000)
                switch wine.state {
                case .booting, .running: continue
                default: break
                }
                break
            }
            try SteamHost.finishInstall(prefix: prefix)
            throw SteamError.files("Steam is installed. Restart MYIOSDECK, then choose With Steam again. Sign in through Steam's own window.")
        }
        let descriptor = try SteamHost.playLaunch(appID: appID, prefix: prefix)
        let notice = UserDefaults.standard.string(forKey: "steamhost.noticeText")
        UserDefaults.standard.removeObject(forKey: "steamhost.noticeText")
        wine.runSteamHost(descriptor, title: title)
        guard case .booting = wine.state else { throw SteamError.files("Steam could not start. Restart MYIOSDECK and try again.") }
        if let notice { GameHostView.shared.setStatus(notice) }
        started = true
        Task {
            while true {
                try? await Task.sleep(nanoseconds: 250_000_000)
                switch wine.state {
                case .booting, .running: continue
                default: break
                }
                break
            }
            release()
        }
    }
    static func release() {
        TrackBSessionGate.reserved = false
        SteamCloudSync.shared.releaseDock()
        SteamLibrary.shared.refreshInstalled()
        SteamLibrary.shared.pump()
    }
}

// Compatibility names for UI/disk models only. No legacy client, handoff or authentication.
struct DockGame: Identifiable, Equatable, Sendable {
    let id: Int
    let name: String
    let installDir: String
    let library: String
    let installed: Bool
    let customExecutables: Bool
    var windowsInstallPath: String { "C:\\" + (library + "/common/" + installDir).replacingOccurrences(of: "/", with: "\\") }
}
enum MadeiraDock {
    static var bundled: Bool { mid_wine_linked() }
    static var clientRoot: URL { SteamLibrary.drive.appendingPathComponent("Program Files (x86)/Steam") }
    static func games(drive: URL) -> [DockGame] {
        let apps = SteamInstallPaths.steamApps(drive: drive)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: apps.path)) ?? []
        return names.sorted().prefix(10000).compactMap { file in
            guard file.hasPrefix("appmanifest_"), file.hasSuffix(".acf"),
                  let id = Int(file.dropFirst(12).dropLast(4)), id > 0,
                  let state = SteamInstallFiles.bRecord(appID: id, steamApps: apps),
                  let folder = state["installdir"]?.string, !folder.isEmpty,
                  !folder.contains("/"), !folder.contains("\\"), folder != ".." else { return nil }
            return DockGame(id: id, name: state["name"]?.string ?? "App \(id)", installDir: folder,
                            library: SteamInstallPaths.libraryRelative,
                            installed: (state["stateflags"]?.string.flatMap(UInt32.init) ?? 0) & 4 != 0,
                            customExecutables: !(state["checkguid"]?.fields.isEmpty ?? true))
        }
    }
}
