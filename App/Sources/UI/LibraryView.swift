// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UniformTypeIdentifiers

/// The whole library: owned and installed Steam games and the added DRM-free games, as cover art.
/// A game opens its page (install, play, settings, Steam Cloud, uninstall).
struct LibraryView: View {
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var recent = RecentlyPlayed.shared
    @ObservedObject private var favorites = Favorites.shared
    @ObservedObject private var signIn = SteamSignInModel.shared
    @EnvironmentObject private var wine: WineController
    @Environment(\.isLandscape) private var landscape
    @State private var filter = Filter.all
    @State private var query = ""
    @State private var showImporter = false
    @State private var showSignIn = false

    enum Filter: String, CaseIterable, Identifiable {
        case all = "All", installed = "Installed", favorites = "Favorites", steam = "Steam", mine = "My games"
        var id: String { rawValue }
    }

    private var shown: [LibraryGame] {
        var list = GameCatalog.all(steam: steam, custom: custom)
        let installed = Set(GameCatalog.installed(steam: steam, custom: custom).map(\.id))
        switch filter {
        case .all: break
        case .installed: list = list.filter { installed.contains($0.id) }
        case .favorites: list = list.filter { favorites.contains($0.id) }
        case .steam: list = list.filter { $0.source == .steam }
        case .mine: list = list.filter { $0.source != .steam }
        }
        let q = query.trimmingCharacters(in: .whitespaces)
        if !q.isEmpty { list = list.filter { $0.name.localizedCaseInsensitiveContains(q) } }
        // Played first (latest first), then installed, then by name.
        let byName = list.sorted { a, b in
            let ia = installed.contains(a.id), ib = installed.contains(b.id)
            return ia != ib ? ia : a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
        return recent.sorted(byName) { $0.id }
    }

    var body: some View {
        PadScrollView {
            VStack(alignment: .leading, spacing: 14) {
                controls
                steamStatus
                customStatus
                let games = shown
                if games.isEmpty {
                    emptyText.padding(.top, 20)
                } else {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: landscape ? 118 : 104), spacing: 14)], spacing: 16) {
                        ForEach(games) { game in tile(game) }
                    }
                }
            }
            .padding(.horizontal, landscape ? 10 : 20)
            .padding(.bottom, 24)
        }
        .scrollIndicators(.hidden)
        .refreshable { await refreshAll() }
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result { custom.importFolder(url, prefix: wine.prefixURL) }
        }
        .sheet(isPresented: $showSignIn) { SteamSignInView() }
        .onAppear {
            steam.refreshInstalled()   // Valve's client (Madeira Dock) may have installed one meanwhile
            custom.refresh(prefix: wine.prefixURL)
        }
    }

    @MainActor private func refreshAll() async {
        custom.refresh(prefix: wine.prefixURL)
        await steam.refresh()
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                HStack(spacing: 8) {
                    Image(systemName: "magnifyingglass").foregroundStyle(Glass.secondary)
                    TextField("Search your games", text: $query)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    if !query.isEmpty {
                        Button { query = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(Glass.secondary) }
                            .buttonStyle(.plain).accessibilityLabel("Clear search")
                    }
                }
                .padding(.horizontal, 16).frame(height: 44)
                .glassCapsule()
                Menu {
                    Button { showImporter = true } label: { Label("Add a game folder…", systemImage: "folder.badge.plus") }
                    Button { custom.refresh(prefix: wine.prefixURL) } label: { Label("Look for new game folders", systemImage: "arrow.clockwise") }
                    if steam.signedIn {
                        Button { Task { await steam.refresh() } } label: { Label("Refresh Steam library", systemImage: "arrow.triangle.2.circlepath") }
                    }
                } label: {
                    Image(systemName: "plus").font(.system(size: 18, weight: .semibold))
                        .frame(width: 44, height: 44).glassCircle()
                }
                .accessibilityLabel("Add or refresh games")
                .padFocus("library.add", radius: 22) {
                    var b = [DialogButton(title: "Add a game folder…") { showImporter = true },
                             DialogButton(title: "Look for new game folders") { custom.refresh(prefix: wine.prefixURL) }]
                    if steam.signedIn { b.append(DialogButton(title: "Refresh Steam library") { Task { await steam.refresh() } }) }
                    showActions("Add or refresh games", b)
                }
            }
            PadScrollView(axes: .horizontal) {
                HStack(spacing: 8) {
                    ForEach(Filter.allCases) { f in
                        Button { withGlassAnimation { filter = f } } label: {
                            Text(f.rawValue).font(.subheadline.weight(filter == f ? .semibold : .regular))
                                .padding(.horizontal, 14).frame(height: 34)
                                .glassCapsule(highlighted: filter == f)
                        }
                        .buttonStyle(.plain)
                        .accessibilityAddTraits(filter == f ? .isSelected : [])
                        .padFocus("library.filter.\(f.rawValue)", radius: 17) { withGlassAnimation { filter = f } }
                    }
                }
                .padding(.vertical, 2)
            }
            .scrollIndicators(.hidden)
        }
    }

    @ViewBuilder private var steamStatus: some View {
        if signIn.accountName == nil {
            HStack(spacing: 14) {
                Image(systemName: "cloud.fill").font(.title2).foregroundStyle(Glass.primary)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Sign in to Steam").font(.headline)
                    Text("Your Steam library: install and play your Windows games. The sign-in token stays in this device's Keychain; the password is never stored.")
                        .font(.footnote).foregroundStyle(Glass.secondary)
                }
                Spacer(minLength: 8)
                Button("Sign in") { showSignIn = true }.buttonStyle(PrimaryButtonStyle(compact: true))
                    .padFocus("library.signIn", radius: 20, preferred: true) { showSignIn = true }
            }
            .padding(16)
            .glassPanel()
        } else if steam.refreshing || steam.error != nil {
            HStack(spacing: 10) {
                if steam.refreshing { ProgressView(); Text("Loading your Steam library…").foregroundStyle(Glass.secondary) }
                if let error = steam.error {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Glass.warn)
                    Text("Could not load the library: \(error)").font(.footnote).foregroundStyle(Glass.secondary)
                    Spacer()
                    Button("Retry") { Task { await steam.refresh() } }.buttonStyle(GlassButtonStyle(compact: true))
                        .padFocus("library.retry", radius: 18) { Task { await steam.refresh() } }
                }
            }
            .padding(14)
            .glassPanel()
        }
    }

    @ViewBuilder private var customStatus: some View {
        if let copying = custom.copying {
            HStack(spacing: 10) {
                if copying.hasPrefix("Copying") { ProgressView() }
                Text(copying).font(.subheadline).foregroundStyle(Glass.secondary)
            }
            .padding(14).glassPanel()
        }
        ForEach(custom.unusable.keys.sorted(), id: \.self) { folder in
            StatusRow(label: folder, detail: custom.unusable[folder] ?? "", level: .warn)
                .padding(14).glassPanel()
        }
    }

    private var emptyText: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(filter == .favorites ? "No favorites yet" : query.isEmpty ? "No games here yet" : "Nothing matches \"\(query)\"")
                .font(.title3.weight(.semibold))
            Text(filter == .favorites
                 ? "Turn on Add to Favorites on a game's page."
                 : "Put an installed game's folder (GOG, itch.io, or copied from a PC) into Files › On My iPhone › MYIOSDECK › Games, or add one with +. Wine sees it as C:\\Games. 64-bit games run with or without JIT; 32-bit games without JIT are experimental.")
                .font(.footnote).foregroundStyle(Glass.secondary)
        }
    }

    private func tile(_ game: LibraryGame) -> some View {
        Button { ui.open(.game(game.id)) } label: {
            VStack(alignment: .leading, spacing: 6) {
                ArtworkView(source: game.artwork, kind: .portrait, name: game.name, compactPlaceholder: true)
                    .aspectRatio(2 / 3, contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Glass.border, lineWidth: 1))
                    .overlay(alignment: .bottomLeading) { badge(game) }
                    .overlay(alignment: .topTrailing) {
                        if favorites.contains(game.id) {
                            Image(systemName: "star.fill").font(.caption).foregroundStyle(.yellow).padding(7)
                        }
                    }
                Text(game.name).font(.caption).lineLimit(2).foregroundStyle(Glass.text.opacity(0.9))
            }
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel([game.name, badgeText(game)].compactMap { $0 }.joined(separator: ", "))
        .accessibilityAddTraits(.isButton)
        .padFocus("library.tile.\(game.id)", radius: 14, secondary: { ui.open(.gameSettings(game.id)) }) { ui.open(.game(game.id)) }
    }

    private func badgeText(_ game: LibraryGame) -> String? {
        if let id = game.steamID {
            switch steam.downloads[id] {
            case .queued: return "Waiting"
            case .active(let p): return p.phase == .downloading ? "\(Int(p.fraction * 100))%" : "Preparing"
            case .failed: return "Failed"
            case nil: return GameCatalog.isInstalled(game, steam: steam) ? "Installed" : nil
            }
        }
        return game.source == .gog ? "GOG" : nil
    }

    @ViewBuilder private func badge(_ game: LibraryGame) -> some View {
        if let text = badgeText(game) {
            Text(text).font(.caption2.weight(.bold))
                .foregroundStyle(.white)
                .padding(.horizontal, 7).padding(.vertical, 3)
                .background(text == "Failed" ? Glass.bad : Glass.primary, in: Capsule())
                .padding(6)
        }
    }
}
