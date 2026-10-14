// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Portrait cover art that falls through the candidate URLs until one loads,
/// then shows the title on a plain tile if none does.
struct SteamCover: View {
    let urls: [URL]
    let title: String
    @State private var index = 0

    var body: some View {
        // The 2:3 frame sets the size; the image only fills and is cropped to
        // it, so a wide header-image fallback cannot spill into the next tile.
        Deck.panelHi
            .aspectRatio(2 / 3, contentMode: .fit)
            .overlay {
                if index < urls.count {
                    AsyncImage(url: urls[index]) { phase in
                        switch phase {
                        case .success(let image): image.resizable().scaledToFill()
                        case .failure: Color.clear.onAppear { index += 1 }
                        default: ProgressView()
                        }
                    }
                    .id(index)
                } else {
                    Text(title).font(.caption.weight(.semibold)).multilineTextAlignment(.center).padding(6)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8))
    }
}

/// Stage 4b: the signed-in account's Windows games.
struct SteamGamesGrid: View {
    @ObservedObject var library: SteamLibrary
    /// Why Play is unavailable (no JIT, Wine busy), or nil.
    var playBlocker: String?
    var onPlay: (OwnedSteamGame) -> Void = { _ in }
    @State private var filter = ""
    @ObservedObject private var recent = RecentlyPlayed.shared

    /// The recently played first, then by name.
    private var shown: [OwnedSteamGame] {
        let f = filter.trimmingCharacters(in: .whitespaces)
        let games = f.isEmpty ? library.games : library.games.filter { $0.name.localizedCaseInsensitiveContains(f) }
        return recent.sorted(games) { RecentlyPlayed.steam($0.id) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(library.games.isEmpty ? "Owned games" : "Owned games (\(library.games.count))")
                    .font(.headline)
                Spacer()
                if library.refreshing { ProgressView() }
                Button("Refresh") { Task { await library.refresh() } }
                    .buttonStyle(DeckButtonStyle())
                    .frame(width: 110)
                    .disabled(library.refreshing)
            }
            if let error = library.error {
                StatusRow(label: "Could not load the library", detail: error, level: .bad)
            }
            if library.games.count > 12 {
                TextField("Search your games", text: $filter)
                    .textFieldStyle(.roundedBorder)
            }
            if library.games.isEmpty, !library.refreshing, library.error == nil {
                Text("No Windows games loaded yet. Tap Refresh.").font(.caption).foregroundStyle(Deck.dim)
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), spacing: 10)], spacing: 12) {
                ForEach(shown) { game in
                    Button { selected = game } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            SteamCover(urls: library.artwork(game.id), title: game.name)
                                .overlay(alignment: .bottomLeading) { badge(game.id) }
                            Text(game.name).font(.caption2).lineLimit(2).foregroundStyle(Deck.dim)
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .sheet(item: $selected) { SteamGameSheet(game: $0, library: library, playBlocker: playBlocker, onPlay: onPlay) }
    }

    @State private var selected: OwnedSteamGame?

    @ViewBuilder private func badge(_ id: Int) -> some View {
        let text: String? = {
            switch library.downloads[id] {
            case .queued: return "Waiting"
            case .active(let p): return p.phase == .downloading ? "\(Int(p.fraction * 100))%" : "Preparing"
            case .failed: return "Failed"
            case nil: return library.installedBuild(id) != nil ? "Installed" : nil
            }
        }()
        if let text {
            Text(text).font(.caption2.weight(.bold))
                .padding(.horizontal, 6).padding(.vertical, 3)
                .background(Deck.accent, in: Capsule())
                .padding(6)
        }
    }
}

/// The Steam games installed on this iPhone (every Steam library on C:, downloaded here or by
/// Valve's client), the recently played first, each with its size and Delete, which removes its
/// files and its install record. Shown signed in or not.
struct SteamInstalledList: View {
    @ObservedObject var library: SteamLibrary
    /// Why nothing can be deleted now (a Windows game is running), or nil.
    var blocker: String?
    @ObservedObject private var recent = RecentlyPlayed.shared
    @State private var deleting: DockGame?

    var body: some View {
        let games = recent.sorted(library.installed) { RecentlyPlayed.steam($0.id) }
        if !games.isEmpty {
            VStack(alignment: .leading, spacing: 10) {
                Text("Installed on this iPhone (\(games.count))").font(.headline)
                ForEach(games) { game in
                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(game.name).font(.body.weight(.semibold)).lineLimit(1)
                            Text(detail(game)).font(.caption).foregroundStyle(Deck.dim).lineLimit(1)
                        }
                        Spacer()
                        Button("Delete…", role: .destructive) { deleting = game }
                            .disabled(blocker != nil || library.downloads[game.id] != nil)
                    }
                }
                if let blocker {
                    Text(blocker).font(.caption).foregroundStyle(Deck.dim)
                }
            }
            .confirmationDialog(deleting.map { "Delete \($0.name)?" } ?? "", isPresented: deletingPresented,
                                titleVisibility: .visible, presenting: deleting) { game in
                Button("Delete the game's files", role: .destructive) { library.delete(game) }
                Button("Cancel", role: .cancel) {}
            } message: { game in
                Text("Removes C:\\\(game.library.replacingOccurrences(of: "/", with: "\\"))\\common\\\(game.installDir) and Steam's install record from this iPhone. Saves kept elsewhere (Documents, AppData, Steam Cloud) stay. This cannot be undone.")
            }
        }
    }

    private var deletingPresented: Binding<Bool> {
        Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })
    }

    private func detail(_ game: DockGame) -> String {
        let apps = SteamLibrary.drive.appendingPathComponent(game.library, isDirectory: true)
        var parts: [String] = []
        if let size = SteamInstallFiles.sizeOnDisk(appID: game.id, steamApps: apps) {
            parts.append(ByteCountFormatter.string(fromByteCount: size, countStyle: .file))
        }
        if !game.installed { parts.append("not fully installed") }
        if let at = recent.started[RecentlyPlayed.steam(game.id)] {
            parts.append("played " + Date(timeIntervalSince1970: at).formatted(.relative(presentation: .named)))
        }
        return parts.isEmpty ? "App ID \(game.id)" : parts.joined(separator: " · ")
    }
}

/// One owned game: play (4d, direct start), install, cancel, uninstall (4c).
struct SteamGameSheet: View {
    let game: OwnedSteamGame
    @ObservedObject var library: SteamLibrary
    var playBlocker: String?
    var onPlay: (OwnedSteamGame) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var confirmUninstall = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    HStack(alignment: .top, spacing: 16) {
                        SteamCover(urls: library.artwork(game.id), title: game.name).frame(width: 120)
                        VStack(alignment: .leading, spacing: 6) {
                            Text(game.name).font(.title3.weight(.bold))
                            Text("App ID \(String(game.id))").font(.caption).foregroundStyle(Deck.dim)
                            if let size = library.installedSize(game.id), library.installedBuild(game.id) != nil {
                                Text("Installed, \(ByteCountFormatter.string(fromByteCount: size, countStyle: .file))")
                                    .font(.caption).foregroundStyle(Deck.dim)
                            }
                        }
                    }
                    state
                    Text("Games install into MYIOSDECK's Windows drive (C:\\Program Files (x86)\\Steam\\steamapps). Keep the app open while downloading: iOS pauses downloads in the background, and Install picks up where it stopped. Play starts the game's own program in Wine without the Steam client, so games that need Steam running (most online and DRM-protected ones) will not start yet.")
                        .font(.caption).foregroundStyle(Deck.dim)
                }
                .padding(16)
            }
            .background(Deck.bg)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }

    @ViewBuilder private var state: some View {
        switch library.downloads[game.id] {
        case .queued:
            StatusRow(label: "Waiting", detail: "Another download is running.", level: .idle)
            Button("Cancel") { library.cancel(game.id) }.buttonStyle(DeckButtonStyle())
        case .active(let p):
            VStack(alignment: .leading, spacing: 6) {
                ProgressView(value: p.fraction)
                Text(progressText(p)).font(.caption).foregroundStyle(Deck.dim)
            }
            Button("Cancel") { library.cancel(game.id) }.buttonStyle(DeckButtonStyle())
        case .failed(let why):
            StatusRow(label: "Download failed", detail: why, level: .bad)
            Button("Retry") { library.install(game.id) }.buttonStyle(DeckButtonStyle())
        case nil:
            if library.installedBuild(game.id) != nil {
                Button("Play") { dismiss(); onPlay(game) }
                    .buttonStyle(DeckButtonStyle())
                    .disabled(playBlocker != nil)
                if let playBlocker {
                    Text(playBlocker).font(.caption).foregroundStyle(Deck.dim)
                }
                Button("Check for update / repair") { library.install(game.id) }.buttonStyle(DeckButtonStyle())
                Button("Uninstall…", role: .destructive) { confirmUninstall = true }
                    .confirmationDialog("Uninstall \(game.name)?", isPresented: $confirmUninstall, titleVisibility: .visible) {
                        Button("Delete the game's files", role: .destructive) {
                            library.uninstall(game.id)
                            RecentlyPlayed.shared.forget(RecentlyPlayed.steam(game.id))
                        }
                        Button("Cancel", role: .cancel) {}
                    } message: {
                        Text("Removes the game's folder and Steam's install record from this iPhone. Install downloads it again.")
                    }
            } else {
                Button("Install") { library.install(game.id) }.buttonStyle(DeckButtonStyle())
            }
        }
    }

    private func progressText(_ p: SteamDownloadProgress) -> String {
        let f = ByteCountFormatter()
        switch p.phase {
        case .preparing: return "Preparing (reading Steam's manifests)…"
        case .finishing: return "Finishing…"
        case .downloading:
            let speed = p.bytesPerSecond > 0 ? " · \(f.string(fromByteCount: Int64(p.bytesPerSecond)))/s" : ""
            return "\(f.string(fromByteCount: Int64(p.doneBytes))) of \(f.string(fromByteCount: Int64(p.totalBytes)))\(speed)"
        }
    }
}
