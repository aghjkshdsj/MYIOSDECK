// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// A game's page: install it (mockup 6), follow its download, or play it, open its settings,
/// its Steam Cloud saves, and update, uninstall or delete it. Added games choose their program here.
struct GameDetailView: View {
    let gameID: String
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var recent = RecentlyPlayed.shared
    @ObservedObject private var favorites = Favorites.shared
    @ObservedObject private var launcher = GameLauncher.shared
    @EnvironmentObject private var wine: WineController
    @Environment(\.isLandscape) private var landscape

    @State private var sizes: (download: UInt64, disk: UInt64)?
    @State private var sizesLoaded = false
    @State private var confirmUninstall = false
    @State private var confirmDelete = false
    @State private var choosingProgram = false

    private var game: LibraryGame? { GameCatalog.find(gameID, steam: steam, custom: custom) }

    var body: some View {
        if let game {
            GlassPage(titledBack: game.name, artwork: game.artwork) {
                if landscape {
                    HStack(alignment: .top, spacing: 24) {
                        art(game).frame(maxWidth: 380)
                        VStack(alignment: .leading, spacing: 16) { details(game) }
                    }
                } else {
                    art(game)
                    details(game)
                }
            }
            .onAppear { ui.focusedGameID = game.id }
            .glassDialog("Uninstall \(game.name)?", isPresented: $confirmUninstall,
                         message: "Removes the game's folder and Steam's install record from this iPhone. Saves kept elsewhere (Documents, AppData, Steam Cloud) stay. Install downloads it again.") {
                [DialogButton(title: "Delete the game's files", role: .destructive) {
                    if let id = game.steamID {
                        steam.uninstall(id)
                        recent.forget(game.id)
                    }
                 }, .cancel()]
            }
            .glassDialog("Delete \(game.name)?", isPresented: $confirmDelete, message: deleteMessage(game)) {
                [DialogButton(title: "Delete the game's files", role: .destructive) { delete(game) }, .cancel()]
            }
            .sheet(isPresented: $choosingProgram) { if let g = game.custom { ProgramPicker(game: g) } }
            .padMenu { ui.open(.gameSettings(game.id)) }
        } else {
            GlassPage(back: "Back", title: "Game not found") {
                Text("It was removed from the library.").foregroundStyle(Glass.secondary)
            }
        }
    }

    // MARK: art

    private func art(_ game: LibraryGame) -> some View {
        ArtworkView(source: game.artwork, kind: .hero, name: game.name)
            .aspectRatio(1.75, contentMode: .fit)
            .overlay(alignment: .bottomLeading) {
                Text(game.name)
                    .font(.system(size: 44, weight: .heavy)).fontWidth(.condensed)
                    .lineLimit(1).minimumScaleFactor(0.4)
                    .foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.6), radius: 8, y: 2)
                    .padding(16)
            }
            .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).strokeBorder(.white.opacity(0.45), lineWidth: 1.5))
            .accessibilityHidden(true)
    }

    // MARK: state

    private enum Phase { case notInstalled, queued, downloading(SteamDownloadProgress), failed(String), installed }

    private func phase(_ game: LibraryGame) -> Phase {
        guard let id = game.steamID else { return .installed }
        switch steam.downloads[id] {
        case .queued: return .queued
        case .active(let p): return .downloading(p)
        case .failed(let why): return .failed(why)
        case nil: return GameCatalog.isInstalled(game, steam: steam) ? .installed : .notInstalled
        }
    }

    @ViewBuilder private func details(_ game: LibraryGame) -> some View {
        switch phase(game) {
        case .notInstalled: installSection(game)
        case .queued:
            header(game, title: game.name)
            StatusRow(label: "Waiting", detail: "Another download is running. You can keep browsing.", level: .idle)
                .padding(14).glassPanel()
            Button("Cancel download") { if let id = game.steamID { steam.cancel(id) } }
                .buttonStyle(GlassButtonStyle(fullWidth: true))
                .padFocus("game.cancelDownload1", radius: 25, preferred: true) { if let id = game.steamID { steam.cancel(id) } }
        case .downloading(let p):
            header(game, title: game.name)
            VStack(alignment: .leading, spacing: 8) {
                Text("Installing").font(.headline)
                ProgressView(value: p.fraction).tint(Glass.primary)
                Text(DownloadsView.progressText(p)).font(.footnote).foregroundStyle(Glass.secondary).monospacedDigit()
            }
            .padding(16).glassPanel()
            Text("You can keep browsing while it downloads. Keep MYIOSDECK open: iOS pauses downloads in the background, and Install picks up where it stopped.")
                .font(.footnote).foregroundStyle(Glass.secondary)
            Button("Cancel download") { if let id = game.steamID { steam.cancel(id) } }
                .buttonStyle(GlassButtonStyle(fullWidth: true))
                .padFocus("game.cancelDownload2", radius: 25, preferred: true) { if let id = game.steamID { steam.cancel(id) } }
        case .failed(let why):
            header(game, title: game.name)
            StatusRow(label: "Download failed", detail: why, level: .bad).padding(14).glassPanel()
            Button { if let id = game.steamID { steam.install(id) } } label: { Label("Retry", systemImage: "arrow.clockwise") }
                .buttonStyle(PrimaryButtonStyle(fullWidth: true))
                .padFocus("game.retry", radius: 28, preferred: true) { if let id = game.steamID { steam.install(id) } }
        case .installed: installedSection(game)
        }
    }

    private func header(_ game: LibraryGame, title: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.largeTitle.weight(.bold)).lineLimit(2).minimumScaleFactor(0.6)
            HStack(spacing: 6) {
                Image(systemName: game.source.symbol)
                Text(game.source.title)
            }
            .font(.body)
            .padding(.horizontal, 14).frame(height: 38)
            .glassCapsule()
        }
    }

    // MARK: install (mockup 6)

    @ViewBuilder private func installSection(_ game: LibraryGame) -> some View {
        header(game, title: "Install \(game.name)")
        GlassCard {
            infoRow("arrow.down.to.line", "Download size", sizes.map { bytesText($0.download) } ?? (sizesLoaded ? "Unknown" : "…"))
            infoRow("internaldrive", "Space required", sizes.map { bytesText($0.disk) } ?? (sizesLoaded ? "Unknown" : "…"))
            infoRow("chart.pie", "Available space", StorageInfo.availableText)
            infoRow("folder", "Install location", "On this iPhone", last: true)
        }
        .task(id: game.id) {
            guard let id = game.steamID, !sizesLoaded else { return }
            sizes = await steam.installSizes(id)
            sizesLoaded = true
        }
        GlassCard {
            GlassRow(last: true) {
                Image(systemName: "star").font(.title3).frame(width: 28)
                Text("Add to Favorites")
                Spacer()
                Toggle("Add to Favorites", isOn: favorites.binding(game.id)).labelsHidden().tint(Glass.primary)
            }
            .padToggle("game.favorite.install", isOn: favorites.binding(game.id))
        }
        if let sizes, let free = StorageInfo.available, Int64(clamping: sizes.disk) > free {
            Label("Not enough space: free some on this iPhone first (Settings › Storage).", systemImage: "exclamationmark.triangle.fill")
                .font(.footnote).foregroundStyle(Glass.warn)
        }
        Text(steam.signedIn ? "You can keep browsing while it downloads." : "Sign in to Steam to install it.")
            .font(.footnote).foregroundStyle(Glass.secondary)
        Button {
            if let id = game.steamID { steam.install(id) }
        } label: {
            Label("Install game", systemImage: "arrow.down.to.line")
        }
        .buttonStyle(PrimaryButtonStyle(fullWidth: true))
        .disabled(!steam.signedIn || game.steamID.flatMap { steam.game($0) } == nil)
        .padFocus("game.install", radius: 28, preferred: true) {
            if steam.signedIn, let id = game.steamID, steam.game(id) != nil { steam.install(id) }
        }
        Button("Cancel") { ui.back() }.buttonStyle(GlassButtonStyle(fullWidth: true))
            .padFocus("game.installCancel", radius: 25) { ui.back() }
    }

    private func infoRow(_ icon: String, _ label: String, _ value: String, last: Bool = false) -> some View {
        GlassRow(last: last) {
            Image(systemName: icon).font(.title3).frame(width: 28)
            Text(label)
            Spacer()
            Text(value).foregroundStyle(Glass.text.opacity(0.9)).monospacedDigit()
        }
        .accessibilityElement(children: .combine)
    }

    // MARK: installed

    @ViewBuilder private func installedSection(_ game: LibraryGame) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(game.name).font(.largeTitle.weight(.bold)).lineLimit(2).minimumScaleFactor(0.6)
            HStack(spacing: 10) {
                SourceBadge(source: game.source)
                Text("·").foregroundStyle(Glass.secondary)
                Text(GameCatalog.lastPlayedText(game, recent: recent)).foregroundStyle(Glass.secondary)
                if let size = installedSize(game) {
                    Text("·").foregroundStyle(Glass.secondary)
                    Text(bytesText(size)).foregroundStyle(Glass.secondary)
                }
            }
            .font(.subheadline)
        }
        HStack(spacing: 12) {
            Button { launcher.play(game) } label: { Label("Play", systemImage: "play.fill").frame(maxWidth: .infinity) }
                .buttonStyle(PrimaryButtonStyle(fullWidth: true))
                .disabled(launcher.blocker(for: game) != nil || launcher.busyText != nil)
                .padFocus("game.play", radius: 28, preferred: true, secondary: { ui.open(.gameSettings(game.id)) }) {
                    if launcher.blocker(for: game) == nil, launcher.busyText == nil { launcher.play(game) }
                }
            Button { ui.open(.gameSettings(game.id)) } label: {
                Image(systemName: "slider.horizontal.3").font(.system(size: 20, weight: .semibold))
                    .frame(width: 56, height: 56).glassCircle()
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Game settings")
            .padFocus("game.settingsButton", radius: 28) { ui.open(.gameSettings(game.id)) }
        }
        if let busy = launcher.busyText {
            HStack(spacing: 8) { ProgressView(); Text(busy) }.font(.subheadline).foregroundStyle(Glass.secondary)
        } else if let why = launcher.blocker(for: game) {
            Text(why).font(.footnote).foregroundStyle(Glass.secondary)
        }
        if let failure = wine.dockFailure, game.steamID != nil {
            StatusRow(label: "Steam (Madeira Dock) did not start the game", detail: failure, level: .bad)
                .padding(14).glassPanel()
        }
        GlassCard {
            GlassRow {
                Image(systemName: "star").font(.title3).frame(width: 28)
                Text("Favorite")
                Spacer()
                Toggle("Favorite", isOn: favorites.binding(game.id)).labelsHidden().tint(Glass.primary)
            }
            .padToggle("game.favorite", isOn: favorites.binding(game.id))
            Button { ui.open(.gameSettings(game.id)) } label: {
                GlassRow(last: true) {
                    Image(systemName: "slider.horizontal.3").font(.title3).frame(width: 28)
                    Text("Game settings")
                    Spacer()
                    Text(GameSettingsStore.load(game.id).useDefaults ? "App defaults" : "Own settings").foregroundStyle(Glass.secondary)
                    Image(systemName: "chevron.right").foregroundStyle(Glass.secondary)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padFocus("game.settingsRow", radius: 12) { ui.open(.gameSettings(game.id)) }
        }
        if let id = game.steamID, steam.installedBuild(id) != nil {
            SteamCloudSection(appID: id)
                .padding(16)
                .glassPanel()
        }
        manage(game)
    }

    private func installedSize(_ game: LibraryGame) -> Int64? {
        if let id = game.steamID {
            if let s = steam.installedSize(id) { return s }
            if let dock = steam.installed.first(where: { $0.id == id }) {
                return SteamInstallFiles.sizeOnDisk(appID: id, steamApps: SteamLibrary.drive.appendingPathComponent(dock.library, isDirectory: true))
            }
        }
        return nil
    }

    @ViewBuilder private func manage(_ game: LibraryGame) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Manage").font(.headline)
            switch game.kind {
            case .steam(let id):
                if steam.game(id) != nil, steam.installedBuild(id) != nil {
                    Button { steam.install(id) } label: { Label("Check for update / repair", systemImage: "arrow.triangle.2.circlepath") }
                        .buttonStyle(GlassButtonStyle(fullWidth: true))
                        .padFocus("game.update", radius: 25) { steam.install(id) }
                    Button { confirmUninstall = true } label: { Label("Uninstall…", systemImage: "trash") }
                        .buttonStyle(GlassButtonStyle(fullWidth: true, destructive: true))
                        .disabled(launcher.deleteBlocker != nil)
                        .padFocus("game.uninstall", radius: 25) { if launcher.deleteBlocker == nil { confirmUninstall = true } }
                } else {
                    Button { confirmDelete = true } label: { Label("Delete…", systemImage: "trash") }
                        .buttonStyle(GlassButtonStyle(fullWidth: true, destructive: true))
                        .disabled(launcher.deleteBlocker != nil)
                        .padFocus("game.delete", radius: 25) { if launcher.deleteBlocker == nil { confirmDelete = true } }
                }
            case .custom(let g):
                Text(g.exe).font(.footnote.monospaced()).foregroundStyle(Glass.secondary).lineLimit(1).truncationMode(.middle)
                Button { choosingProgram = true } label: { Label("Choose the program…", systemImage: "doc.badge.gearshape") }
                    .buttonStyle(GlassButtonStyle(fullWidth: true))
                    .padFocus("game.program", radius: 25) { choosingProgram = true }
                Button {
                    custom.remove(g, files: false)
                    ui.back()
                } label: { Label("Remove from the list (keep files)", systemImage: "minus.circle") }
                    .buttonStyle(GlassButtonStyle(fullWidth: true))
                    .padFocus("game.remove", radius: 25) { custom.remove(g, files: false); ui.back() }
                Button { confirmDelete = true } label: { Label("Delete the game's files…", systemImage: "trash") }
                    .buttonStyle(GlassButtonStyle(fullWidth: true, destructive: true))
                    .disabled(launcher.deleteBlocker != nil)
                    .padFocus("game.deleteFiles", radius: 25) { if launcher.deleteBlocker == nil { confirmDelete = true } }
            }
            if let why = launcher.deleteBlocker { Text(why).font(.footnote).foregroundStyle(Glass.secondary) }
            Text(game.steamID != nil
                 ? "Games install into MYIOSDECK's Windows drive (C:\\Program Files (x86)\\Steam\\steamapps). Play starts the game with Valve's Steam client (With Steam) or on its own (Without Steam, for DRM-free games)."
                 : "Wine sees this game as C:\\Games\\\(game.custom?.folder ?? ""). 64-bit games run with or without JIT; 32-bit games without JIT are experimental.")
                .font(.caption).foregroundStyle(Glass.secondary)
        }
    }

    private func deleteMessage(_ game: LibraryGame) -> String {
        switch game.kind {
        case .steam(let id):
            if let dock = steam.installed.first(where: { $0.id == id }) {
                return "Removes C:\\\(dock.library.replacingOccurrences(of: "/", with: "\\"))\\common\\\(dock.installDir) and Steam's install record from this iPhone. Saves kept elsewhere (Documents, AppData, Steam Cloud) stay. This cannot be undone."
            }
            return "Removes the game's files from this iPhone. This cannot be undone."
        case .custom(let g):
            return "Removes Games/\(g.folder) from this iPhone. This cannot be undone."
        }
    }

    private func delete(_ game: LibraryGame) {
        switch game.kind {
        case .steam(let id):
            if let dock = steam.installed.first(where: { $0.id == id }) { steam.delete(dock) }
        case .custom(let g):
            custom.remove(g, files: true)
            recent.forget(game.id)
            ui.back()
        }
    }
}

/// Every program in an added game's folder, likeliest first, with its CPU: pick the one that starts the game.
struct ProgramPicker: View {
    let game: CustomGame
    @Environment(\.dismiss) private var dismiss

    private func choose(_ p: GameProgram) {
        var g = game
        g.exe = p.path
        CustomGames.shared.update(g)
        dismiss()
    }

    var body: some View {
        let programs = CustomGames.programs(in: game.folder)
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(game.name).font(.title2.weight(.bold)).lineLimit(1)
                Spacer()
                Button("Close") { dismiss() }.buttonStyle(GlassButtonStyle(compact: true))
                    .padFocus("programs.close", radius: 18) { dismiss() }
            }
            Text("The program that starts the game:").font(.subheadline).foregroundStyle(Glass.secondary)
            PadScrollView {
                VStack(spacing: 8) {
                    ForEach(programs) { p in
                        Button { choose(p) } label: {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(p.path).font(.body).multilineTextAlignment(.leading)
                                    Text("\(p.kind), \(bytesText(Int64(p.size)))").font(.caption).foregroundStyle(Glass.secondary)
                                }
                                Spacer()
                                if p.path == game.exe { Image(systemName: "checkmark").foregroundStyle(Glass.primary) }
                            }
                            .padding(14)
                            .glassPanel(cornerRadius: 14)
                        }
                        .buttonStyle(.plain)
                        .padFocus("programs.\(p.path)", radius: 14, preferred: p.path == game.exe) { choose(p) }
                    }
                }
                .padding(.vertical, 6)
            }
        }
        .padding(20)
        .foregroundStyle(Glass.text)
        .background { ArtworkBackdrop(source: .blank) }
        .presentationBackground(.clear)
        .padScope("programs") { dismiss() }
    }
}

/// Free space on the iPhone.
enum StorageInfo {
    static var available: Int64? {
        let home = URL(fileURLWithPath: NSHomeDirectory())
        return (try? home.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage
    }
    static var total: Int64? {
        let home = URL(fileURLWithPath: NSHomeDirectory())
        return (try? home.resourceValues(forKeys: [.volumeTotalCapacityKey]))?.volumeTotalCapacity.map(Int64.init)
    }
    static var availableText: String { available.map { bytesText($0) } ?? "Unknown" }
}
