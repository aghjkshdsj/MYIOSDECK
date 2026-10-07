// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import UIKit

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
            queue.removeAll()
            active?.task.cancel()
            downloads = [:]
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

    // MARK: Downloads (stage 4c)
    //
    // Into the Wine prefix's C:\Program Files (x86)\Steam\steamapps as
    // common/<installdir> plus appmanifest_<appid>.acf (written last by
    // DepotDownloader), the layout Valve's client reads. One at a time; chunks
    // are journaled, so a cancelled or interrupted download resumes.

    enum DownloadState: Equatable {
        case queued
        case active(SteamDownloadProgress)
        case failed(String)
    }

    @Published private(set) var downloads: [Int: DownloadState] = [:]
    /// Bumped when an install finishes or is removed, so views re-read disk state.
    @Published private(set) var installGeneration = 0
    private var queue: [Int] = []
    private var active: (id: Int, task: Task<Void, Never>)?
    private lazy var downloader = DepotDownloader(session: session)

    static var prefix: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("wine-prefix")
    }
    static var drive: URL { prefix.appendingPathComponent("drive_c", isDirectory: true) }
    static var steamApps: URL { SteamInstallPaths.steamApps(drive: drive) }

    /// The installed build, or nil when Steam's install record is absent.
    func installedBuild(_ appID: Int) -> Int? {
        _ = installGeneration
        return SteamInstallFiles.buildID(appID: appID, steamApps: Self.steamApps)
    }

    func installedSize(_ appID: Int) -> Int64? {
        SteamInstallFiles.sizeOnDisk(appID: appID, steamApps: Self.steamApps)
    }

    func install(_ appID: Int) {
        guard account != nil else { return }
        switch downloads[appID] {
        case nil, .failed: break        // new, or a retry
        default: return                 // already queued or running
        }
        downloads[appID] = .queued
        queue.append(appID)
        dlog("[steam-depot] queued app=\(appID)")
        pump()
    }

    func cancel(_ appID: Int) {
        queue.removeAll { $0 == appID }
        if active?.id == appID { active?.task.cancel() } else { downloads[appID] = nil }
    }

    func uninstall(_ appID: Int) {
        guard downloads[appID] == nil, let game = game(appID) else { return }
        let folder = game.installDir.isEmpty ? "app_\(appID)" : game.installDir, apps = Self.steamApps
        Task.detached(priority: .utility) {
            SteamInstallFiles.delete(appID: appID, folderName: folder, steamApps: apps)
            await MainActor.run { self.installGeneration += 1 }
        }
        dlog("[steam-depot] uninstalled app=\(appID)")
    }

    private func pump() {
        guard active == nil, !queue.isEmpty else { return }
        let appID = queue.removeFirst()
        let task = Task { @MainActor [weak self] in await self?.run(appID) }
        active = (appID, task)
    }

    private func run(_ appID: Int) async {
        // Keep the screen awake and ask iOS for background time: a download
        // only advances while the app runs.
        UIApplication.shared.isIdleTimerDisabled = true
        let bg = UIApplication.shared.beginBackgroundTask(withName: "steam-download")
        defer {
            UIApplication.shared.endBackgroundTask(bg)
            UIApplication.shared.isIdleTimerDisabled = false
        }
        downloads[appID] = .active(SteamDownloadProgress())
        do {
            guard let info = try await fetcher.fetchInstallInfo(appID: UInt32(appID)) else {
                throw SteamError.appInfoNotFound(UInt32(appID))
            }
            Self.prefix.path.withCString { mid_wine_seed_prefix($0) }
            try FileManager.default.createDirectory(at: SteamInstallPaths.common(drive: Self.drive), withIntermediateDirectories: true)
            _ = try await downloader.install(info, steamApps: Self.steamApps,
                                             ownedDepots: { [weak self] in try? await self?.fetcher.ownedDepotIDs() }) { [weak self] progress in
                Task { @MainActor in
                    if case .active = self?.downloads[appID] { self?.downloads[appID] = .active(progress) }
                }
            }
            downloads[appID] = nil
            installGeneration += 1
            dlog("[steam-depot] installed app=\(appID) build=\(info.buildID)")
        } catch {
            if Task.isCancelled || error is CancellationError || (error as? URLError)?.code == .cancelled {
                downloads[appID] = nil
                dlog("[steam-depot] cancelled app=\(appID) (finished chunks are kept; Install resumes)")
            } else {
                downloads[appID] = .failed(SteamSignIn.message(error))
                dlog("[steam-depot] failed app=\(appID) reason=\(SteamSignIn.reason(error))")
            }
        }
        active = nil
        pump()
    }

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
