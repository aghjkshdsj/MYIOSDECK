// SPDX-License-Identifier: GPL-3.0-or-later
// Madeira Converter Exception: see LICENSE-EXCEPTION.md
//
// Steam Cloud saves, the part that decides (docs/STEAM_CLOUD.md): where a cloud file lives in the
// Wine prefix, how the cloud and this device compare, what is copied which way, and which old
// backups may go. Foundation only, no network and no hashing, so tests/host runs it on the CI host.
//
// Ported from Madeira app/Madeira/SteamCloud.swift at commit 65e6fe8f (willfaust/Madeira:
// SteamCloudPaths, SteamCloudEntry, SteamCloudAudit, SteamCloudPlan) and changed for MYIOSDECK's
// stricter rules: a typed record per game (file hashes, sizes, times and the cloud's change number),
// no upload from a session whose start was not synced while the cloud has moved, an empty save
// never replaces a cloud file unasked, and backups that are never thinned below the newest ones.

import Foundation

// MARK: - The cloud's file list

/// One file of an app's Steam Cloud, as Cloud.GetAppFileChangelist lists it.
struct SteamCloudFile: Equatable, Sendable {
    /// Folder part, usually starting with a `%Root%` placeholder; may be empty.
    var prefix: String
    var name: String
    var sha: Data
    /// Unix seconds.
    var timestamp: UInt64
    var size: UInt64
    /// persist_state: 0 = present; anything else is a forgotten or deleted file.
    var persistState: UInt32

    var path: String { prefix + name }
}

struct SteamCloudListing: Equatable, Sendable {
    var changeNumber: UInt64 = 0
    var files: [SteamCloudFile] = []
}

// MARK: - Where a cloud path lives in the Wine prefix

/// Maps Steam's save folders to folders of the Wine prefix (Documents/wine-prefix/drive_c).
struct SteamCloudPaths: Sendable, Equatable {
    /// drive_c/users/<name>
    var userFolder: URL
    /// The game's folder under steamapps/common.
    var installFolder: URL
    /// Valve's client's folder for files written through the Steam Cloud API:
    /// C:\Program Files (x86)\Steam\userdata\<account>\<app>\remote.
    var remoteFolder: URL
    var steamID: UInt64
    var overrides: [RootOverride]

    /// One `ufs.rootoverrides` entry: on `os`, files the cloud lists under `root` are kept under
    /// `useInstead` + `addPath`.
    struct RootOverride: Equatable, Codable, Sendable {
        var root: String
        var os: String
        var useInstead: String
        var addPath: String
    }

    /// The local folder a Steam root name stands for on Windows, or nil for a root this prefix
    /// has no folder for (another platform's).
    func folder(root: String) -> URL? {
        switch root.lowercased() {
        case "gameinstall": return installFolder
        case "winmydocuments": return userFolder.appendingPathComponent("Documents", isDirectory: true)
        case "winappdatalocal": return userFolder.appendingPathComponent("AppData/Local", isDirectory: true)
        case "winappdatalocallow": return userFolder.appendingPathComponent("AppData/LocalLow", isDirectory: true)
        case "winappdataroaming": return userFolder.appendingPathComponent("AppData/Roaming", isDirectory: true)
        case "winsavedgames": return userFolder.appendingPathComponent("Saved Games", isDirectory: true)
        default: return nil
        }
    }

    /// Steam's own spelling of a root name (product info is not consistent about case).
    static func canonical(root: String) -> String {
        ["GameInstall", "WinMyDocuments", "WinAppDataLocal", "WinAppDataLocalLow", "WinAppDataRoaming", "WinSavedGames"]
            .first { $0.caseInsensitiveCompare(root) == .orderedSame } ?? root
    }

    /// Splits "%Root%rest" into its root name and the rest; no placeholder gives a nil root
    /// (Steam's `remote` folder).
    static func split(_ path: String) -> (root: String?, rest: String) {
        guard path.hasPrefix("%"), let end = path.dropFirst().firstIndex(of: "%") else { return (nil, path) }
        let root = String(path[path.index(after: path.startIndex)..<end])
        return (root, String(path[path.index(after: end)...]))
    }

    /// The path components of a relative save path, or nil when it is not a plain relative path
    /// (empty, absolute, a drive, or leaving its folder).
    func components(_ relative: String) -> [String]? {
        let text = relative.replacingOccurrences(of: "\\", with: "/")
            .replacingOccurrences(of: "{64BitSteamID}", with: String(steamID))
            .replacingOccurrences(of: "{Steam3AccountID}", with: String(steamID & 0xFFFF_FFFF))
        let parts = text.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        guard !parts.isEmpty, parts.count <= 32,
              !parts.contains(where: { $0 == "." || $0 == ".." || $0.contains(":") || $0.utf8.count > 255 || $0.contains("\0") })
        else { return nil }
        return parts
    }

    /// The base folder and relative components a cloud path maps to, with the Windows root
    /// override applied. nil: no folder here, or an unsafe path.
    func location(cloudPath: String) -> (base: URL, parts: [String])? {
        var (root, rest) = Self.split(cloudPath)
        if let name = root,
           let override = overrides.first(where: { $0.root.caseInsensitiveCompare(name) == .orderedSame
                                                   && $0.os.caseInsensitiveCompare("Windows") == .orderedSame }) {
            if !override.useInstead.isEmpty { root = override.useInstead }
            if !override.addPath.isEmpty { rest = override.addPath + "/" + rest }
        }
        guard let parts = components(rest) else { return nil }
        if let root {
            guard let base = folder(root: root) else { return nil }
            return (base, parts)
        }
        return (remoteFolder, parts)
    }

    /// Wine treats names without regard to case and iOS does not: follow the components by
    /// case-insensitive match, falling back to the given spelling for the part that does not
    /// exist yet.
    static func resolve(base: URL, parts: [String]) -> URL {
        var url = base
        for (index, part) in parts.enumerated() {
            let exact = url.appendingPathComponent(part, isDirectory: index < parts.count - 1)
            if FileManager.default.fileExists(atPath: exact.path) { url = exact; continue }
            let names = (try? FileManager.default.contentsOfDirectory(atPath: url.path)) ?? []
            if let match = names.first(where: { $0.caseInsensitiveCompare(part) == .orderedSame }) {
                url = url.appendingPathComponent(match, isDirectory: index < parts.count - 1)
            } else {
                url = exact
            }
        }
        return url
    }

    /// The local file a cloud path stands for, or nil.
    func url(cloudPath: String) -> URL? {
        location(cloudPath: cloudPath).map { Self.resolve(base: $0.base, parts: $0.parts) }
    }
}

/// One `ufs.savefiles` entry: `root` names a folder ("WinAppDataLocalLow", "GameInstall", ...),
/// `path` a subfolder of it, `pattern` a wildcard.
struct SteamCloudSaveRule: Equatable, Codable, Sendable {
    var root: String
    var path: String
    var pattern: String
    var recursive: Bool
    /// Lower-cased platform names the entry is limited to; empty = all.
    var platforms: [String]
}

// MARK: - Comparison

/// One save file, on either side or both.
struct SteamCloudEntry: Identifiable, Equatable, Sendable {
    enum Kind: String, Equatable, Sendable { case same, differ, cloudOnly, localOnly }
    /// The cloud path with its %Root% placeholder (for a local-only file, the path it would have).
    var path: String
    var kind: Kind
    var cloudSize: UInt64 = 0
    var cloudTime: UInt64 = 0
    var localSize: UInt64 = 0
    var localTime: UInt64 = 0
    var cloudSHA = Data()
    var localSHA = Data()

    var id: String { path }
    /// Record key: one spelling per file.
    var key: String { path.lowercased() }
    /// The file name without its folders, for display.
    var name: String { path.split(separator: "/").last.map(String.init) ?? path }
}

/// How one app's cloud and local save files compare.
struct SteamCloudAudit: Equatable, Sendable {
    var changeNumber: UInt64 = 0
    var cloudFiles = 0
    var entries: [SteamCloudEntry] = []
    /// Cloud paths under a root this prefix has no folder for.
    var unmapped: [String] = []

    func count(_ kind: SteamCloudEntry.Kind) -> Int { entries.reduce(0) { $0 + ($1.kind == kind ? 1 : 0) } }

    private static func attributes(_ url: URL) -> (size: UInt64, time: UInt64)? {
        guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey, .isRegularFileKey]),
              values.isRegularFile == true else { return nil }
        return (UInt64(max(0, values.fileSize ?? 0)), UInt64(max(0, values.contentModificationDate?.timeIntervalSince1970 ?? 0)))
    }

    /// One spelling per file, whatever route led to it and whatever its case.
    private static func key(_ url: URL) -> String { url.resolvingSymlinksInPath().path.lowercased() }

    /// Compares the listing with the prefix. Reads files; changes nothing. `sha1` hashes a local
    /// file (nil when it cannot be read).
    static func run(listing: SteamCloudListing, rules: [SteamCloudSaveRule], paths: SteamCloudPaths,
                    sha1: (URL) -> Data?) -> SteamCloudAudit {
        var audit = SteamCloudAudit()
        audit.changeNumber = listing.changeNumber
        var known = Set<String>()   // local paths the cloud lists
        for file in listing.files where file.persistState == 0 {
            audit.cloudFiles += 1
            guard let url = paths.url(cloudPath: file.path) else { audit.unmapped.append(file.path); continue }
            known.insert(key(url))
            var entry = SteamCloudEntry(path: file.path, kind: .cloudOnly, cloudSize: file.size, cloudTime: file.timestamp, cloudSHA: file.sha)
            if let local = attributes(url) {
                entry.localSize = local.size; entry.localTime = local.time
                entry.localSHA = sha1(url) ?? Data()
                entry.kind = (local.size == file.size && entry.localSHA == file.sha) ? .same : .differ
            }
            audit.entries.append(entry)
        }
        // Local files the game's save rules cover that the cloud does not list.
        var folders: [(label: String, url: URL, pattern: String, recursive: Bool)] = []
        for rule in rules where rule.platforms.isEmpty || rule.platforms.contains("windows") || rule.platforms.contains("all") {
            guard let place = paths.location(cloudPath: "%\(rule.root)%" + (rule.path.isEmpty ? "x" : rule.path + "/x")) else { continue }
            let folder = SteamCloudPaths.resolve(base: place.base, parts: Array(place.parts.dropLast()))
            // A Windows override moves the folder: its cloud name is not this one, so files found
            // there are not offered for upload.
            if paths.overrides.contains(where: { $0.root.caseInsensitiveCompare(rule.root) == .orderedSame
                                                 && $0.os.caseInsensitiveCompare("Windows") == .orderedSame }) { continue }
            // The cloud name has the account IDs filled in, as the device folder does: Steam does
            // not commit an upload named with the placeholder itself.
            let folderPath = rule.path.isEmpty ? "" : (paths.components(rule.path)?.joined(separator: "/") ?? rule.path) + "/"
            let label = "%\(SteamCloudPaths.canonical(root: rule.root))%" + folderPath
            folders.append((label, folder, rule.pattern.isEmpty ? "*" : rule.pattern, rule.recursive))
        }
        folders.append(("", paths.remoteFolder, "*", true))
        var seen = Set<String>()
        for folder in folders {
            let options: FileManager.DirectoryEnumerationOptions = folder.recursive ? [] : [.skipsSubdirectoryDescendants]
            guard let walk = FileManager.default.enumerator(at: folder.url, includingPropertiesForKeys: [.isRegularFileKey],
                                                            options: options) else { continue }
            let base = folder.url.resolvingSymlinksInPath().path
            var visited = 0
            for case let url as URL in walk {
                visited += 1
                if visited > 5_000 { break }
                guard wildcard(folder.pattern, url.lastPathComponent), let local = attributes(url) else { continue }
                let fileKey = key(url)
                guard !known.contains(fileKey), seen.insert(fileKey).inserted else { continue }
                // Both resolved: the enumerator may spell the same folder differently (/private/var).
                let relative = url.resolvingSymlinksInPath().path.dropFirst(base.count).drop { $0 == "/" }
                guard !relative.isEmpty else { continue }
                audit.entries.append(SteamCloudEntry(path: folder.label + String(relative), kind: .localOnly,
                                                     localSize: local.size, localTime: local.time,
                                                     localSHA: sha1(url) ?? Data()))
            }
        }
        return audit
    }

    /// Steam's save patterns: `*` and `?`, without regard to case.
    static func wildcard(_ pattern: String, _ name: String) -> Bool {
        let p = Array(pattern.lowercased()), n = Array(name.lowercased())
        var pi = 0, ni = 0, star = -1, mark = 0
        while ni < n.count {
            if pi < p.count, p[pi] == "?" || p[pi] == n[ni] { pi += 1; ni += 1 }
            else if pi < p.count, p[pi] == "*" { star = pi; mark = ni; pi += 1 }
            else if star >= 0 { pi = star + 1; mark += 1; ni = mark }
            else { return false }
        }
        while pi < p.count, p[pi] == "*" { pi += 1 }
        return pi == p.count
    }

    var summary: String {
        "cloud-change=\(changeNumber) cloud-files=\(cloudFiles) same=\(count(.same)) differ=\(count(.differ)) " +
        "missing-local=\(count(.cloudOnly)) local-only=\(count(.localOnly)) unmapped=\(unmapped.count)"
    }
}

// MARK: - The record of the last sync

/// What MYIOSDECK knows about one game's saves since its last sync with Steam Cloud. Kept per
/// account (SteamCloudSync); without it a difference cannot be told from a change, so a file with
/// no record is never copied over the other side unasked.
struct SteamCloudRecord: Codable, Equatable, Sendable {
    /// A file as it was when last identical on both sides, or the user's choice about it.
    struct FileState: Codable, Equatable, Sendable {
        enum State: String, Codable, Sendable {
            /// Identical on both sides at `sha`.
            case synced
            /// Synced at `sha` and gone from this device while the cloud still has it; the user has
            /// not chosen yet. A new local file of that name is never uploaded over the cloud's copy.
            case missingHere
            /// The user chose to leave it gone here while the cloud held `sha` (never deleted there).
            case leftDeleted
        }
        var state: State = .synced
        /// SHA-1, hex.
        var sha: String
        var size: UInt64 = 0
        var time: UInt64 = 0
    }

    var files: [String: FileState] = [:]
    /// The cloud's change number at the last complete sync (0: never).
    var changeNumber: UInt64 = 0
    var lastSync: Date?
    /// A game session started without a successful sync first (offline, or the user started it
    /// anyway): nothing is uploaded until a full comparison has run again, and while the cloud
    /// has moved since the last sync, every save that differs is the user's choice.
    var unsyncedStart = false
    /// A game session started and its end-of-game sync has not completed (the app was closed, or
    /// the sync failed): the next sync uploads what the game changed, under the rules above.
    var sessionOpen = false
    /// The last download or upload stopped; the next sync tries again. Shown on the game's page.
    var problem: String?
    /// The app's save rules and the Windows overrides, from Steam's product info, so the page and
    /// a restore work offline.
    var rules: [SteamCloudSaveRule]?
    var overrides: [SteamCloudPaths.RootOverride]?
    /// The account's Steam ID (for {64BitSteamID} in save paths) and the app's build, kept for
    /// the same reason.
    var steamID: UInt64?
    var buildID: UInt32?

    /// Whether a listing looks like a cloud that lost track of this game rather than its real
    /// state: no files at all although saves were synced before, or a change number that went
    /// backwards. Nothing is uploaded on such an answer (an upload would then replace cloud files
    /// that were not seen, without a backup).
    func implausible(_ listing: SteamCloudListing) -> Bool {
        let syncedBefore = files.values.contains { $0.state == .synced }
        let present = listing.files.contains { $0.persistState == 0 }
        return (syncedBefore && !present) || (changeNumber != 0 && listing.changeNumber < changeNumber)
    }

    static func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }
}

// MARK: - What to do with a comparison

/// A save that needs the user's choice, and why.
struct SteamCloudConflict: Equatable, Sendable, Identifiable {
    enum Reason: String, Equatable, Sendable {
        /// Changed on this device and in the cloud since the last sync.
        case bothChanged
        /// Different on the two sides and there is no record of a sync (a first sync).
        case noRecord
        /// Synced before, gone from this device, still in the cloud, while other saves are here.
        case missingHere
        /// Changed here during a session that started without syncing, and the cloud has moved
        /// since the last sync.
        case unsyncedSession
        /// The device's copy is empty and the cloud's is not.
        case emptyHere
    }
    var entry: SteamCloudEntry
    var reason: Reason
    var id: String { entry.key }
}

/// Decides, from a comparison and the record of the last sync, which files changed on one side
/// only (copied without asking) and which need the user's choice. Never deletes anything on
/// either side: a file gone from one side is either restored from the other or left as it is.
struct SteamCloudPlan: Equatable, Sendable {
    var download: [SteamCloudEntry] = []
    var upload: [SteamCloudEntry] = []
    var conflicts: [SteamCloudConflict] = []
    /// Record entries to write now (files identical on both sides, and marks).
    var settled: [String: SteamCloudRecord.FileState] = [:]
    /// Local saves the cloud dropped since the last sync (deleted elsewhere): left on this device,
    /// not sent back.
    var droppedInCloud: [SteamCloudEntry] = []

    var isClear: Bool { download.isEmpty && upload.isEmpty && conflicts.isEmpty }

    static func make(audit: SteamCloudAudit, record: SteamCloudRecord) -> SteamCloudPlan {
        var plan = SteamCloudPlan()
        let hex = SteamCloudRecord.hex
        // The cloud moved since the last complete sync (or there never was one).
        let cloudMoved = record.changeNumber == 0 || audit.changeNumber != record.changeNumber
        let guarded = record.unsyncedStart && cloudMoved
        // How many saves of the last sync are still on this device: none at all means a new or
        // reset Windows drive, where restoring them loses nothing.
        let syncedKeys = Set(record.files.filter { $0.value.state == .synced }.keys)
        let presentSynced = audit.entries.filter { $0.kind != .cloudOnly && syncedKeys.contains($0.key) }.count
        for entry in audit.entries {
            let known = record.files[entry.key]
            let cloud = hex(entry.cloudSHA), local = hex(entry.localSHA)
            switch entry.kind {
            case .same:
                plan.settled[entry.key] = .init(state: .synced, sha: cloud, size: entry.cloudSize, time: entry.cloudTime)
            case .differ:
                guard let known, known.state == .synced else {
                    plan.conflicts.append(.init(entry: entry, reason: known == nil ? .noRecord : .bothChanged)); continue
                }
                if known.sha == cloud, known.sha != local {
                    // Changed here only.
                    if entry.localSize == 0, entry.cloudSize > 0 { plan.conflicts.append(.init(entry: entry, reason: .emptyHere)) }
                    else if guarded { plan.conflicts.append(.init(entry: entry, reason: .unsyncedSession)) }
                    else { plan.upload.append(entry) }
                } else if known.sha == local, known.sha != cloud {
                    plan.download.append(entry)          // changed in the cloud only
                } else {
                    plan.conflicts.append(.init(entry: entry, reason: .bothChanged))
                }
            case .cloudOnly:
                switch known?.state {
                case nil:
                    plan.download.append(entry)          // new in the cloud: nothing here to lose
                case .leftDeleted?:
                    if known?.sha != cloud { plan.download.append(entry) }   // changed in the cloud since the choice
                case .missingHere?:
                    plan.conflicts.append(.init(entry: entry, reason: .missingHere))
                case .synced?:
                    if known?.sha != cloud || presentSynced == 0 {
                        // Changed in the cloud since, or the whole save set is gone (new drive):
                        // restoring it replaces nothing.
                        plan.download.append(entry)
                    } else {
                        // Gone from this device while its other saves are here: the game may have
                        // deleted it. Never deleted in the cloud; the user chooses. Until then the
                        // mark keeps a new save of that name from going up over the cloud's copy.
                        plan.conflicts.append(.init(entry: entry, reason: .missingHere))
                        plan.settled[entry.key] = .init(state: .missingHere, sha: known?.sha ?? cloud,
                                                        size: entry.cloudSize, time: entry.cloudTime)
                    }
                }
            case .localOnly:
                guard !entry.localSHA.isEmpty else { continue }   // unreadable: left alone
                switch known?.state {
                case nil:
                    plan.upload.append(entry)            // new here: replaces nothing in the cloud
                case .missingHere?, .leftDeleted?:
                    // The cloud's copy is gone too: a new save of that name, nothing to replace.
                    plan.upload.append(entry)
                case .synced?:
                    plan.droppedInCloud.append(entry)    // deleted in the cloud elsewhere: not sent back
                }
            }
        }
        return plan
    }
}

// MARK: - Backups

/// Which save backups may be removed. Backups are folders named by their time
/// (Documents/save-backups/<App ID>/<time>); every one younger than `keepDays` stays, and so do
/// the newest `keepNewest`, whatever their age. Beyond that only the oldest go, down to `keepMax`.
enum SteamCloudBackupRetention {
    static let keepNewest = 10
    static let keepMax = 30
    static let keepDays = 60.0

    static func expired(_ backups: [(name: String, created: Date)], now: Date) -> [String] {
        let newestFirst = backups.sorted { $0.created > $1.created }
        guard newestFirst.count > keepMax else { return [] }
        var excess = newestFirst.count - keepMax
        var drop: [String] = []
        for backup in newestFirst.dropFirst(keepNewest).reversed() where excess > 0 {
            guard now.timeIntervalSince(backup.created) > keepDays * 86_400 else { continue }
            drop.append(backup.name)
            excess -= 1
        }
        return drop
    }
}
