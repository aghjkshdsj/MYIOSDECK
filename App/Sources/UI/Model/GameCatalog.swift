// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Where a game comes from (its badge).
enum GameSource: Hashable {
    case steam, gog, drmFree
    var title: String {
        switch self {
        case .steam: return "Steam"
        case .gog: return "GOG"
        case .drmFree: return "My games"
        }
    }
    var symbol: String {
        switch self {
        case .steam: return "cloud.fill"
        case .gog: return "shippingbox.fill"
        case .drmFree: return "folder.fill"
        }
    }
}

/// One game of the user's library, whatever its source: an owned or installed Steam game, or an
/// added DRM-free game. `id` is RecentlyPlayed's key ("steam.<App ID>" or "custom.<UUID>"), and
/// the key of everything stored per game (GameSettings, touch layouts, favorites).
struct LibraryGame: Identifiable, Hashable {
    enum Kind: Hashable { case steam(Int), custom(CustomGame) }
    let kind: Kind
    let name: String
    let source: GameSource

    var id: String {
        switch kind {
        case .steam(let appID): return "steam.\(appID)"
        case .custom(let g): return "custom.\(g.id.uuidString)"
        }
    }
    var artwork: ArtworkSource {
        switch kind {
        case .steam(let appID): return .steam(appID)
        case .custom(let g): return .custom(g)
        }
    }
    var steamID: Int? { if case .steam(let id) = kind { return id } else { return nil } }
    var custom: CustomGame? { if case .custom(let g) = kind { return g } else { return nil } }
}

/// The library as one list, built from the stores (SteamLibrary, CustomGames).
@MainActor
enum GameCatalog {
    /// Every game: owned Steam games, Steam games installed on this iPhone that the account does
    /// not list (installed by Valve's client, or while signed out), and the added games.
    static func all(steam: SteamLibrary, custom: CustomGames) -> [LibraryGame] {
        var out: [LibraryGame] = []
        var seen = Set<Int>()
        for g in steam.games where seen.insert(g.id).inserted {
            out.append(LibraryGame(kind: .steam(g.id), name: g.name, source: .steam))
        }
        for g in steam.installed where seen.insert(g.id).inserted {
            out.append(LibraryGame(kind: .steam(g.id), name: g.name, source: .steam))
        }
        for g in custom.games {
            out.append(LibraryGame(kind: .custom(g), name: g.name, source: CustomGames.isGOG(g.folder) ? .gog : .drmFree))
        }
        return out
    }

    /// Installed (playable here) games only.
    static func installed(steam: SteamLibrary, custom: CustomGames) -> [LibraryGame] {
        let ids = Set(steam.installed.filter { $0.installed }.map { $0.id })
        return all(steam: steam, custom: custom).filter { g in g.steamID.map { ids.contains($0) } ?? true }
    }

    static func isInstalled(_ game: LibraryGame, steam: SteamLibrary) -> Bool {
        switch game.kind {
        case .steam(let id): return steam.installed.contains { $0.id == id && $0.installed }
        case .custom: return true
        }
    }

    /// The installed games, most recently played first (Home's hero and its row).
    static func recent(steam: SteamLibrary, custom: CustomGames, recent: RecentlyPlayed) -> [LibraryGame] {
        recent.sorted(installed(steam: steam, custom: custom)) { $0.id }
    }

    static func find(_ id: String, steam: SteamLibrary, custom: CustomGames) -> LibraryGame? {
        all(steam: steam, custom: custom).first { $0.id == id }
    }

    /// "Last played yesterday", "Last played 3 days ago", "Not played yet".
    static func lastPlayedText(_ game: LibraryGame, recent: RecentlyPlayed) -> String {
        guard let at = recent.started[game.id] else { return "Not played yet" }
        let date = Date(timeIntervalSince1970: at)
        let cal = Calendar.current
        if cal.isDateInToday(date) { return "Last played today" }
        if cal.isDateInYesterday(date) { return "Last played yesterday" }
        return "Last played " + date.formatted(.relative(presentation: .named))
    }
}

/// Favorite games (the game page's Add to Favorites; Library's Favorites filter).
@MainActor
final class Favorites: ObservableObject {
    static let shared = Favorites()
    @Published private(set) var ids: Set<String>

    private init() { ids = Set(UserDefaults.standard.stringArray(forKey: "favorites") ?? []) }

    func contains(_ id: String) -> Bool { ids.contains(id) }
    func set(_ id: String, _ on: Bool) {
        if on { ids.insert(id) } else { ids.remove(id) }
        UserDefaults.standard.set(Array(ids).sorted(), forKey: "favorites")
    }
    func binding(_ id: String) -> Binding<Bool> {
        Binding(get: { self.contains(id) }, set: { self.set(id, $0) })
    }
}

/// Navigation and the game whose artwork is behind the app.
@MainActor
final class UIState: ObservableObject {
    static let shared = UIState()

    /// The pill's three tabs, and Settings (the gear button), which takes the same place.
    enum Tab: String, CaseIterable, Identifiable {
        case home = "Home", library = "Library", downloads = "Downloads", settings = "Settings"
        /// The top bar's round buttons: full pages too, like the tabs.
        case search = "Search", controllers = "Controllers"
        var id: String { rawValue }
    }
    enum Route: Hashable {
        case game(String)              // a LibraryGame id
        case gameSettings(String)
        case settings(SettingsSection?)
        case storage
        case logs
        case benchmarks
        case developer
    }

    @Published var tab: Tab = .home
    @Published var path: [Route] = []
    /// The game shown behind the tabs (Home's selected game).
    @Published var focusedGameID: String?

    func open(_ route: Route) { withGlassAnimation { path.append(route) } }
    func back() { if !path.isEmpty { withGlassAnimation { _ = path.removeLast() } } }
    func show(_ tab: Tab) { withGlassAnimation { path.removeAll(); self.tab = tab } }
}

enum SettingsSection: String, CaseIterable, Identifiable, Hashable {
    case appearance = "Appearance", performance = "Performance", controls = "Controls", steam = "Steam",
         storage = "Storage", logs = "Logs", about = "About"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .appearance: return "paintbrush.fill"
        case .performance: return "gauge.with.dots.needle.67percent"
        case .controls: return "gamecontroller"
        case .steam: return "cloud"
        case .storage: return "internaldrive"
        case .logs: return "text.alignleft"
        case .about: return "info.circle"
        }
    }
}
