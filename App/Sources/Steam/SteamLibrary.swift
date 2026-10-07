// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// One game the signed-in account owns that has a Windows build.
struct OwnedSteamGame: Codable, Identifiable, Hashable, Sendable {
    var id: Int
    var name: String
    var installDir: String
    var buildID: Int
    var libraryCapsule: String?
    var headerImage: String?
    var parentID: Int?

    init(_ info: SteamAppInfo) {
        id = Int(info.appID)
        name = info.name
        installDir = info.installDir
        buildID = Int(info.buildID)
        libraryCapsule = info.libraryCapsule
        headerImage = info.headerImage
        parentID = info.parentID.map(Int.init)
    }
}

/// Stage 4b: the account's owned library, from Steam's own CM connection
/// (SwiftSteam's SteamSession + SteamLibraryFetcher, the path Madeira's
/// SteamOwnedLibrary uses). Cached per account; refreshed when older than six
/// hours or on request. Logs App ID counts only, never account data.
@MainActor
final class SteamLibrary: ObservableObject {
    static let shared = SteamLibrary()

    @Published private(set) var games: [OwnedSteamGame] = []
    @Published private(set) var refreshing = false
    @Published private(set) var updated: Date?
    @Published var error: String?

    private let session = SteamSession()
    private lazy var fetcher = SteamLibraryFetcher(session: session)
    private var account: String?
    private var started = false

    private struct Cache: Codable { var version: Int; var account: String; var updated: Date; var games: [OwnedSteamGame] }
    private static var cacheURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("MYIOSDECK/steam-library.json")
    }

    /// Called when the Library shows; idempotent.
    func start() {
        guard !started else { return }
        started = true
        NotificationCenter.default.addObserver(forName: SteamSignIn.didChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.signInChanged() }
        }
        signInChanged()
    }

    private func signInChanged() {
        let name = SteamSignIn.accountName
        guard name != account else { return }
        account = name
        games = []; updated = nil; error = nil
        guard let name else {
            session.logoff()
            try? FileManager.default.removeItem(at: Self.cacheURL)
            dlog("[steam-library] signed out: list cleared")
            return
        }
        if let data = try? Data(contentsOf: Self.cacheURL),
           let cache = try? JSONDecoder().decode(Cache.self, from: data), cache.version == 1, cache.account == name {
            games = cache.games; updated = cache.updated
        }
        dlog("[steam-library] signed in, cached games=\(games.count)")
        if Date().timeIntervalSince(updated ?? .distantPast) > 6 * 3600 {
            Task { await refresh() }
        }
    }

    func refresh() async {
        guard let account, !refreshing else { return }
        refreshing = true
        error = nil
        defer { refreshing = false }
        do {
            let apps = try await fetcher.fetchOwnedApps()
            let owned = apps.filter(\.installableOnWindows).map(OwnedSteamGame.init)
                .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
            guard account == SteamSignIn.accountName else { return }   // signed out meanwhile
            games = owned
            updated = Date()
            try? FileManager.default.createDirectory(at: Self.cacheURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? JSONEncoder().encode(Cache(version: 1, account: account, updated: updated!, games: owned))
                .write(to: Self.cacheURL, options: .atomic)
            dlog("[steam-library] owned apps=\(apps.count) windows-installable=\(owned.count)")
        } catch {
            self.error = SteamSignIn.message(error)
            dlog("[steam-library] refresh failed: \(SteamSignIn.reason(error))")
        }
    }

    func game(_ id: Int) -> OwnedSteamGame? { games.first { $0.id == id } }

    // MARK: Artwork (Steam's public store images, no account data)

    private static let assetBase = "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/"

    private static func safeAssetName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= 256 && !name.hasPrefix("/") && !name.contains("..") &&
            name.unicodeScalars.allSatisfy { $0.isASCII && (CharacterSet.alphanumerics.contains($0) || "._-/".unicodeScalars.contains($0)) }
    }

    /// Portrait capsule candidates, tried in order: the capsule named in product
    /// info (newer apps publish it only under a hashed path), the legacy path,
    /// the same for a demo's full game, then the wide header image.
    func artwork(_ appID: Int) -> [URL] {
        var urls: [URL] = []
        func add(_ url: URL?) { if let url, !urls.contains(url) { urls.append(url) } }
        func asset(_ id: Int, _ name: String?) -> URL? {
            guard let name, Self.safeAssetName(name) else { return nil }
            return URL(string: Self.assetBase + "\(id)/" + name)
        }
        func direct(_ id: Int) {
            add(asset(id, game(id)?.libraryCapsule))
            add(URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(id)/library_600x900.jpg"))
        }
        direct(appID)
        if let parent = game(appID)?.parentID, parent != appID { direct(parent) }
        add(asset(appID, game(appID)?.headerImage))
        return urls
    }
}
