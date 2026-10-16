// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Installs a game's Windows content (spec 03 §D1–D8).
//
// Flow: select depots (D1) → depot keys and content servers (D3) → manifests (D4) → prepare
// files (D5) → chunks, at most 8 in flight, recorded for resume (D6, D7) → install record
// written last (D8).
//
// The DepotDownloader itself stays on the main actor and only coordinates. Manifest decoding,
// file preparation, chunk download/decrypt/decompress/hash and all file I/O run in detached
// tasks or task-group children, off the main actor.
//
// Sources: spec 03 §D; refs/steam-protobufs/steammessages_clientserver_2.proto
// (CMsgClientGetDepotDecryptionKey/Response), steammessages_contentsystem.steamclient.proto
// (GetManifestRequestCode, GetCDNAuthToken); Apple Foundation (URLSessionConfiguration,
// URLResourceKey.volumeAvailableCapacityForImportantUsageKey, withThrowingTaskGroup).

import Foundation

// MARK: - Progress

struct SteamDownloadProgress: Equatable, Sendable {
    enum Phase: String, Sendable { case preparing, downloading, finishing }
    var phase: Phase = .preparing
    var totalBytes: UInt64 = 0        // compressed bytes of the whole install
    var doneBytes: UInt64 = 0         // compressed bytes done, resumed ones included
    var bytesPerSecond: Double = 0
    /// doneBytes / totalBytes in 0...1, 0 when total is 0.
    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return min(max(Double(doneBytes) / Double(totalBytes), 0), 1)
    }
    init() {}
}

// MARK: - HTTP

/// GET for the CDN and the content-server directory; tests substitute a scripted one.
protocol SteamHTTPClient: Sendable {
    nonisolated func get(_ url: URL) async throws -> (data: Data, status: Int)
}

struct SteamURLSessionHTTP: SteamHTTPClient {
    let session: URLSession

    /// The dedicated download session (§D6): ephemeral, no URL cache, 30 s request and 300 s
    /// resource timeouts, 8 connections per host.
    nonisolated static let cdn: SteamURLSessionHTTP = {
        let config = URLSessionConfiguration.ephemeral
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 300
        config.httpMaximumConnectionsPerHost = 8
        return SteamURLSessionHTTP(session: URLSession(configuration: config))
    }()

    nonisolated func get(_ url: URL) async throws -> (data: Data, status: Int) {
        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        let (data, response) = try await session.data(for: request)
        return (data, (response as? HTTPURLResponse)?.statusCode ?? 0)
    }
}

// MARK: - Prepared install (§D5)

struct SteamDepotManifestInput: Sendable {
    let depotID: UInt32
    let manifestID: UInt64
    let key: Data
    let ownerApp: UInt32?              // depotfromapp
    let files: [SteamManifestFile]
}

struct SteamPreparedFile: Sendable {
    let mappingIndex: Int              // position in the manifest; part of the resume key
    let relativePath: String           // '/'-separated, directory names folded
    let url: URL
    let size: UInt64
    let chunks: [SteamManifestChunk]
    let hadContent: Bool               // non-empty before this install
}

struct SteamPreparedDepot: Sendable {
    let depotID: UInt32
    let manifestID: UInt64
    let key: Data
    let ownerApp: UInt32?
    var files: [SteamPreparedFile]
    var customExecutables: [String]    // install-relative Windows paths

    nonisolated var size: UInt64 { files.reduce(0) { $0 + $1.size } }
}

enum SteamInstallPreparer {
    struct Output: Sendable {
        var depots: [SteamPreparedDepot]
        var skippedPaths: Int
        var needed: Int64
    }

    nonisolated static func chunksAreValid(_ file: SteamManifestFile) -> Bool {
        for c in file.chunks {
            guard c.sha.count == 20, Int(c.originalSize) <= SteamCodec.maxChunkSize,
                  c.offset <= file.size, UInt64(c.originalSize) <= file.size - c.offset else { return false }
        }
        return true
    }

    /// Checks and folds every path, checks free space, creates directories, and sets every
    /// file's length to its manifest size (keeping existing bytes).
    nonisolated static func prepare(_ inputs: [SteamDepotManifestInput], installFolder: URL, steamApps: URL,
                                    freeSpace: @Sendable (URL) -> Int64?) throws -> Output {
        struct Entry {
            var depot: Int
            var mapping: Int
            var parts: [String]
            var file: SteamManifestFile
        }
        var folding = SteamDirectoryFolding(root: installFolder)
        var entries: [Entry] = []
        var directories: [[String]] = []
        var skipped = 0
        var symlinks = 0
        for (d, input) in inputs.enumerated() {
            for (m, file) in input.files.enumerated() {
                if file.isSymlink { symlinks += 1; continue }
                guard let parts = SteamPathRules.manifestComponents(file.name) else { skipped += 1; continue }
                if file.isDirectory {
                    directories.append(folding.fold(parts, isDirectory: true))
                    continue
                }
                guard chunksAreValid(file) else { skipped += 1; continue }
                entries.append(Entry(depot: d, mapping: m, parts: folding.fold(parts, isDirectory: false), file: file))
            }
        }
        // A path claimed by two depots: the later depot (higher ID) wins.
        var owner: [String: Int] = [:]
        for (i, e) in entries.enumerated() { owner[e.parts.joined(separator: "/").lowercased()] = i }

        let fm = FileManager.default
        var depots = inputs.map {
            SteamPreparedDepot(depotID: $0.depotID, manifestID: $0.manifestID, key: $0.key,
                               ownerApp: $0.ownerApp, files: [], customExecutables: [])
        }
        var needed: Int64 = 0
        var parents = Set<String>()
        for (i, e) in entries.enumerated() {
            let relative = e.parts.joined(separator: "/")
            guard owner[relative.lowercased()] == i else { continue }
            let url = installFolder.appendingPathComponent(relative, isDirectory: false)
            var oldSize: UInt64 = 0
            if let attributes = try? fm.attributesOfItem(atPath: url.path) {
                if (attributes[.type] as? FileAttributeType) == .typeDirectory {
                    throw SteamError.fileWriteFailed("a folder is in the way of a file")
                }
                oldSize = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
            }
            if e.file.size > oldSize { needed += Int64(clamping: e.file.size - oldSize) }
            depots[e.depot].files.append(SteamPreparedFile(mappingIndex: e.mapping, relativePath: relative, url: url,
                                                           size: e.file.size, chunks: e.file.chunks,
                                                           hadContent: oldSize > 0))
            if e.file.isCustomExecutable {
                depots[e.depot].customExecutables.append(relative.replacingOccurrences(of: "/", with: "\\"))
            }
            parents.insert(url.deletingLastPathComponent().path)
        }

        try fm.createDirectory(at: steamApps, withIntermediateDirectories: true)
        if let available = freeSpace(steamApps), needed > available {
            throw SteamError.notEnoughSpace(needed: needed, available: available)
        }

        do {
            try fm.createDirectory(at: installFolder, withIntermediateDirectories: true)
            for dir in directories {
                try fm.createDirectory(at: installFolder.appendingPathComponent(dir.joined(separator: "/"), isDirectory: true),
                                       withIntermediateDirectories: true)
            }
            for path in parents.sorted() {
                try fm.createDirectory(atPath: path, withIntermediateDirectories: true)
            }
        } catch {
            throw SteamError.fileWriteFailed("could not create folders")
        }
        for depot in depots {
            for file in depot.files {
                let fd = open(file.url.path, O_RDWR | O_CREAT, 0o644)
                guard fd >= 0 else { throw SteamError.fileWriteFailed("create \(errno)") }
                let rc = ftruncate(fd, off_t(clamping: file.size))
                let err = errno
                close(fd)
                guard rc == 0 else { throw SteamError.fileWriteFailed(err == ENOSPC ? "disk full" : "size \(err)") }
            }
        }
        if symlinks > 0 { SteamLog.event("[steam-depot] symlinks skipped=\(symlinks)") }
        return Output(depots: depots, skippedPaths: skipped, needed: needed)
    }
}

// MARK: - Chunk work (§D6, §D7)

struct SteamChunkTarget: Sendable {
    let url: URL
    let offset: UInt64
    let recordKey: String
    let verify: Bool
    let log: SteamResumeLog
}

/// One chunk of one depot, with every place it goes.
struct SteamChunkWork: Sendable {
    let depotID: UInt32
    let key: Data
    let chunk: SteamManifestChunk
    var targets: [SteamChunkTarget]
}

struct SteamChunkPlan: Sendable {
    var works: [SteamChunkWork]
    var totalBytes: UInt64
    var resumedBytes: UInt64
    var resumedChunks: Int
}

enum SteamChunkPlanner {
    nonisolated static func recordKey(mapping: Int, chunk: SteamManifestChunk) -> String {
        "\(mapping) \(chunk.offset) \(chunk.sha.steamHex)"
    }

    /// Opens each depot's resume record and lists the chunk work that is still to do.
    nonisolated static func plan(_ depots: [SteamPreparedDepot], resumeFolder: URL) throws -> SteamChunkPlan {
        var works: [SteamChunkWork] = []
        var index: [String: Int] = [:]
        var total: UInt64 = 0
        var resumed: UInt64 = 0
        var resumedChunks = 0
        for depot in depots {
            let log = try SteamResumeLog(folder: resumeFolder, depot: depot.depotID, manifest: depot.manifestID)
            for file in depot.files {
                for chunk in file.chunks {
                    let compressed = UInt64(chunk.compressedSize)
                    total += compressed
                    let key = recordKey(mapping: file.mappingIndex, chunk: chunk)
                    if log.done.contains(key) {
                        resumed += compressed
                        resumedChunks += 1
                        continue
                    }
                    let target = SteamChunkTarget(url: file.url, offset: chunk.offset, recordKey: key,
                                                  verify: file.hadContent, log: log)
                    let id = "\(depot.depotID)-\(chunk.sha.steamHex)"
                    if let i = index[id] {
                        works[i].targets.append(target)
                    } else {
                        index[id] = works.count
                        works.append(SteamChunkWork(depotID: depot.depotID, key: depot.key, chunk: chunk, targets: [target]))
                    }
                }
            }
        }
        return SteamChunkPlan(works: works, totalBytes: total, resumedBytes: resumed, resumedChunks: resumedChunks)
    }
}

/// Host order shared by every chunk of one install. Failures push a host back; successes
/// slowly earn its place back.
final class SteamHostBoard: @unchecked Sendable {
    private let lock = NSLock()
    private let hosts: [String]
    private var penalty: [String: Double] = [:]
    private var failures: [String: Int] = [:]
    private var loggedLines = 0

    nonisolated init(hosts: [String]) {
        self.hosts = hosts
    }

    /// The hosts to try for one chunk, starting at a different healthy host per ordinal.
    nonisolated func order(for ordinal: Int) -> [String] {
        lock.lock()
        defer { lock.unlock() }
        let ranked = hosts.enumerated().sorted { a, b in
            let pa = penalty[a.element] ?? 0
            let pb = penalty[b.element] ?? 0
            return pa != pb ? pa < pb : a.offset < b.offset
        }.map(\.element)
        let healthy = ranked.filter { (penalty[$0] ?? 0) < 2 }
        let rest = ranked.filter { (penalty[$0] ?? 0) >= 2 }
        guard !healthy.isEmpty else { return ranked }
        let start = ordinal % healthy.count
        return Array(healthy[start...] + healthy[..<start]) + rest
    }

    nonisolated func succeeded(_ host: String) {
        lock.lock()
        defer { lock.unlock() }
        penalty[host] = max(0, (penalty[host] ?? 0) - 0.25)
    }

    nonisolated func failed(_ host: String) {
        lock.lock()
        let count = (failures[host] ?? 0) + 1
        failures[host] = count
        penalty[host] = (penalty[host] ?? 0) + 1
        let shouldLog = count == 3 && loggedLines < 8
        if shouldLog { loggedLines += 1 }
        lock.unlock()
        if shouldLog {
            SteamLog.event("[steam-depot] content host failing: \(SteamContentServers.bareHost(host)) failures=3")
        }
    }
}

struct SteamChunkOutcome: Sendable {
    var doneBytes: UInt64       // compressed bytes now done (every target)
    var networkBytes: UInt64    // bytes downloaded for it in this run
}

enum SteamChunkRunner {
    nonisolated static let attempts = 5

    /// Verifies existing content, else downloads, decodes and writes the chunk everywhere it
    /// goes, recording each place in the resume record.
    nonisolated static func run(_ work: SteamChunkWork, ordinal: Int, board: SteamHostBoard,
                                http: any SteamHTTPClient, auth: [String: String]) async throws -> SteamChunkOutcome {
        let size = Int(work.chunk.originalSize)
        let done = UInt64(work.chunk.compressedSize) * UInt64(work.targets.count)
        var pending: [SteamChunkTarget] = []
        for target in work.targets {
            try Task.checkCancellation()
            if target.verify, let existing = SteamFileIO.read(target.url, at: target.offset, length: size),
               SteamCrypto.sha1(existing) == work.chunk.sha {
                target.log.record(target.recordKey)
            } else {
                pending.append(target)
            }
        }
        if pending.isEmpty { return SteamChunkOutcome(doneBytes: done, networkBytes: 0) }

        let (plain, fetched) = try await fetch(work, ordinal: ordinal, board: board, http: http, auth: auth)
        for target in pending {
            try SteamFileIO.write(plain, to: target.url, at: target.offset)
            target.log.record(target.recordKey)
        }
        return SteamChunkOutcome(doneBytes: done, networkBytes: UInt64(fetched))
    }

    /// Up to 5 tries over the first 6 hosts, pausing 0.4 s × attempt between tries.
    nonisolated static func fetch(_ work: SteamChunkWork, ordinal: Int, board: SteamHostBoard,
                                  http: any SteamHTTPClient, auth: [String: String]) async throws -> (Data, Int) {
        let shaHex = work.chunk.sha.steamHex
        let order = board.order(for: ordinal)
        guard !order.isEmpty else { throw SteamError.chunkFailed("no content server") }
        var lastError: Error = SteamError.chunkFailed("no content server")
        for attempt in 0..<attempts {
            try Task.checkCancellation()
            if attempt > 0 {
                try await Task.sleep(nanoseconds: UInt64(attempt) * 400_000_000)
            }
            let host = order[attempt % order.count]
            guard let url = URL(string: "\(host)/depot/\(work.depotID)/chunk/\(shaHex)\(auth[host] ?? "")") else {
                continue
            }
            do {
                let (body, status) = try await http.get(url)
                guard status == 200 else { throw SteamError.chunkFailed("HTTP \(status)") }
                guard body.count <= SteamCodec.maxChunkSize + 4096 else { throw SteamError.chunkFailed("too large") }
                let plain = try SteamCodec.decodeChunk(body, key: work.key, originalSize: Int(work.chunk.originalSize),
                                                       crc: work.chunk.crc)
                board.succeeded(host)
                return (plain, body.count)
            } catch {
                if error is CancellationError || Task.isCancelled { throw CancellationError() }
                board.failed(host)
                lastError = error
            }
        }
        if let url = lastError as? URLError { throw SteamError.chunkFailed("network \(url.code.rawValue)") }
        throw lastError
    }
}

// MARK: - Downloader

@MainActor
final class DepotDownloader {
    private struct Plan {
        let depot: SteamDepotInfo
        let key: Data
        let contentApp: UInt32
        let manifestID: UInt64
    }

    static let maxInFlight = 8
    static let hostsPerChunk = 6

    private let session: any SteamConnection
    private let http: any SteamHTTPClient
    private let freeSpace: @Sendable (URL) -> Int64?
    private var depotKeys: [UInt32: Data] = [:]
    private var cdnTokens: [String: String] = [:]      // "depot|host" → URL suffix

    /// The app passes its SteamSession; tests pass a scripted SteamConnection.
    convenience init(session: any SteamConnection) {
        self.init(session: session, http: SteamURLSessionHTTP.cdn, freeSpace: DepotDownloader.volumeFreeSpace)
    }

    init(session: any SteamConnection, http: any SteamHTTPClient, freeSpace: @escaping @Sendable (URL) -> Int64?) {
        self.session = session
        self.http = http
        self.freeSpace = freeSpace
    }

    nonisolated static func volumeFreeSpace(_ url: URL) -> Int64? {
        let values = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return values?.volumeAvailableCapacityForImportantUsage
    }

    nonisolated static func contentAppID(_ depot: SteamDepotInfo, app: SteamAppInfo) -> UInt32 {
        if app.freeToDownload { return app.appID }
        return depot.depotFromApp ?? app.appID
    }

    /// Install an app's Windows content into `steamApps` (spec 03 §D). Returns the install
    /// folder. Throws CancellationError when the calling task is cancelled.
    func install(_ app: SteamAppInfo, steamApps: URL,
                 ownedDepots: @escaping () async -> Set<UInt32>?,
                 progress: @escaping (SteamDownloadProgress) -> Void) async throws -> URL {
        do {
            return try await run(app, steamApps: steamApps, ownedDepots: ownedDepots, progress: progress)
        } catch {
            if error is CancellationError || Task.isCancelled {
                SteamLog.event("[steam-depot] install cancelled app=\(app.appID)")
                throw CancellationError()
            }
            SteamLog.event("[steam-depot] install failed app=\(app.appID) reason=\(SteamLog.describe(error))")
            throw error
        }
    }

    private func run(_ app: SteamAppInfo, steamApps: URL,
                     ownedDepots: @escaping () async -> Set<UInt32>?,
                     progress: @escaping (SteamDownloadProgress) -> Void) async throws -> URL {
        let started = Date()
        var state = SteamDownloadProgress()
        progress(state)

        // D1
        let selection = SteamDepotSelection.select(app)
        SteamLog.event("[steam-depot] install begin app=\(app.appID) build=\(app.buildID) \(selection.logLine)")
        guard !selection.depots.isEmpty else { throw SteamError.noWindowsDepot }

        // D2
        let trimmedDir = app.installDir.trimmingCharacters(in: .whitespaces)
        let folderName = trimmedDir.isEmpty ? "app_\(app.appID)" : SteamInstallFiles.safeFolderName(trimmedDir)
        let installFolder = steamApps.appendingPathComponent("common", isDirectory: true)
            .appendingPathComponent(folderName, isDirectory: true)
        let resumeFolder = steamApps.appendingPathComponent("downloading", isDirectory: true)
            .appendingPathComponent(String(app.appID), isDirectory: true)

        // D3: keys
        var plans: [Plan] = []
        for depot in selection.depots {
            try Task.checkCancellation()
            guard let manifestID = depot.publicManifest else { continue }
            let contentApp = Self.contentAppID(depot, app: app)
            if let key = try await depotKey(depot.id, appID: contentApp) {
                plans.append(Plan(depot: depot, key: key, contentApp: contentApp, manifestID: manifestID))
                continue
            }
            let owned = await ownedDepots()
            if let owned, !owned.isEmpty, !owned.contains(depot.id) {
                SteamLog.event("[steam-depot] depot=\(depot.id) skipped: key refused, not in this account's licences")
                continue
            }
            throw SteamError.depotKeyRefused(depot.id)
        }
        // Every depot skipped is the same failure.
        guard !plans.isEmpty else { throw SteamError.depotKeyRefused(selection.depots[0].id) }

        // D3: content servers
        let hosts = await contentServers(appID: app.appID)
        let chunkHosts = Array(hosts.prefix(Self.hostsPerChunk))

        // D4: manifests
        var inputs: [SteamDepotManifestInput] = []
        var authByDepot: [UInt32: [String: String]] = [:]
        for plan in plans {
            try Task.checkCancellation()
            let auth = await authSuffixes(depot: plan.depot.id, appID: plan.contentApp, hosts: chunkHosts)
            authByDepot[plan.depot.id] = auth
            let code = await manifestRequestCode(appID: plan.contentApp, depot: plan.depot.id, manifest: plan.manifestID)
            let manifest = try await fetchManifest(plan, code: code, hosts: chunkHosts, auth: auth)
            if manifest.files.contains(where: { $0.isCustomExecutable }) {
                let cache = steamApps.appendingPathComponent("depotcache", isDirectory: true)
                    .appendingPathComponent("\(plan.depot.id)_\(plan.manifestID).manifest")
                let bytes = manifest.bytes
                do {
                    try await Task.detached { try SteamFileIO.writeIfSizeDiffers(bytes, to: cache) }.value
                } catch {
                    SteamLog.event("[steam-depot] depot=\(plan.depot.id) depotcache write failed")
                }
            }
            inputs.append(SteamDepotManifestInput(depotID: plan.depot.id, manifestID: plan.manifestID, key: plan.key,
                                                  ownerApp: plan.depot.depotFromApp, files: manifest.files))
        }

        // D5
        try Task.checkCancellation()
        let freeSpace = self.freeSpace
        let preparedInputs = inputs
        let prepared = try await Task.detached(priority: .userInitiated) {
            try SteamInstallPreparer.prepare(preparedInputs, installFolder: installFolder, steamApps: steamApps,
                                             freeSpace: freeSpace)
        }.value
        let fileCount = prepared.depots.reduce(0) { $0 + $1.files.count }
        let totalSize = prepared.depots.reduce(UInt64(0)) { $0 + $1.size }
        SteamLog.event("[steam-depot] prepared files=\(fileCount) skipped-paths=\(prepared.skippedPaths) size=\(totalSize) to-write=\(prepared.needed)")

        // D7: resume records and the work list
        let preparedDepots = prepared.depots
        let chunkPlan = try await Task.detached(priority: .userInitiated) {
            try SteamChunkPlanner.plan(preparedDepots, resumeFolder: resumeFolder)
        }.value
        SteamLog.event("[steam-depot] chunks to-do=\(chunkPlan.works.count) resumed=\(chunkPlan.resumedChunks)")

        // D6: download
        state.phase = .downloading
        state.totalBytes = chunkPlan.totalBytes
        state.doneBytes = chunkPlan.resumedBytes
        progress(state)
        try await download(chunkPlan.works, hosts: chunkHosts, auth: authByDepot, state: &state, progress: progress)

        // D8
        try Task.checkCancellation()
        state.phase = .finishing
        progress(state)
        let steamID = session.steamID
        let records = Self.records(for: app, folderName: folderName, depots: preparedDepots, steamApps: steamApps,
                                   lastOwner: steamID)
        let (written, skipped) = try await Task.detached(priority: .userInitiated) { () -> (Int, Int) in
            for (url, text) in records.owners {
                try Data(text.utf8).write(to: url, options: .atomic)
            }
            do {
                try Data(records.main.text.utf8).write(to: records.main.url, options: .atomic)
            } catch {
                throw SteamError.fileWriteFailed("install record")
            }
            try? FileManager.default.removeItem(at: resumeFolder)
            return (records.owners.count, records.ownersSkipped)
        }.value
        SteamLog.event("[steam-depot] owner records written=\(written) skipped=\(skipped)")
        let seconds = Int(Date().timeIntervalSince(started).rounded())
        SteamLog.event("[steam-depot] install complete app=\(app.appID) bytes=\(totalSize) seconds=\(seconds)")
        return installFolder
    }

    // MARK: Chunks

    private func download(_ works: [SteamChunkWork], hosts: [String], auth: [UInt32: [String: String]],
                          state: inout SteamDownloadProgress,
                          progress: @escaping (SteamDownloadProgress) -> Void) async throws {
        guard !works.isEmpty else { return }
        let board = SteamHostBoard(hosts: hosts)
        let http = self.http
        let runStart = Date()
        var networkBytes: UInt64 = 0
        var lastReport = Date.distantPast
        var current = state

        try await withThrowingTaskGroup(of: SteamChunkOutcome.self) { group in
            var next = 0
            var running = 0
            while running < Self.maxInFlight, next < works.count {
                let work = works[next]
                let ordinal = next
                let depotAuth = auth[work.depotID] ?? [:]
                group.addTask {
                    try await SteamChunkRunner.run(work, ordinal: ordinal, board: board, http: http, auth: depotAuth)
                }
                next += 1
                running += 1
            }
            while running > 0 {
                guard let outcome = try await group.next() else { break }
                running -= 1
                current.doneBytes += outcome.doneBytes
                networkBytes += outcome.networkBytes
                let now = Date()
                if now.timeIntervalSince(lastReport) >= 0.25 {
                    current.bytesPerSecond = Double(networkBytes) / max(now.timeIntervalSince(runStart), 0.001)
                    progress(current)
                    lastReport = now
                }
                // Cancellation stops between chunks; finished chunks stay recorded.
                if Task.isCancelled { throw CancellationError() }
                if next < works.count {
                    let work = works[next]
                    let ordinal = next
                    let depotAuth = auth[work.depotID] ?? [:]
                    group.addTask {
                        try await SteamChunkRunner.run(work, ordinal: ordinal, board: board, http: http, auth: depotAuth)
                    }
                    next += 1
                    running += 1
                }
            }
        }
        current.bytesPerSecond = Double(networkBytes) / max(Date().timeIntervalSince(runStart), 0.001)
        progress(current)
        state = current
    }

    // MARK: Install records (§D8)

    private struct RecordFiles: Sendable {
        var main: (url: URL, text: String)
        var owners: [(URL, String)]
        var ownersSkipped: Int
    }

    private static func records(for app: SteamAppInfo, folderName: String, depots: [SteamPreparedDepot],
                                steamApps: URL, lastOwner: UInt64) -> RecordFiles {
        let now = Int64(Date().timeIntervalSince1970)
        let own = depots.filter { $0.ownerApp == nil }
        let shared = depots.filter { $0.ownerApp != nil }
        var record = SteamInstallRecord(appID: app.appID, name: app.name, installDir: folderName, lastUpdated: now,
                                        sizeOnDisk: depots.reduce(0) { $0 + $1.size }, buildID: app.buildID,
                                        lastOwner: lastOwner,
                                        installedDepots: own.map { .init(id: $0.depotID, manifest: $0.manifestID, size: $0.size) })
        record.sharedDepots = shared.compactMap { d -> (depot: UInt32, owner: UInt32)? in
            guard let owner = d.ownerApp else { return nil }
            return (depot: d.depotID, owner: owner)
        }
        record.checkGuid = Array(depots.flatMap(\.customExecutables).prefix(256))
        let mainURL = steamApps.appendingPathComponent("appmanifest_\(app.appID).acf")

        var owners: [(URL, String)] = []
        var skipped = 0
        let byOwner = Dictionary(grouping: shared, by: { $0.ownerApp ?? 0 })
        for ownerID in byOwner.keys.sorted() {
            guard let info = app.sharedOwners[ownerID],
                  SteamInstallFiles.safeFolderName(info.installDir).caseInsensitiveCompare(folderName) == .orderedSame else {
                skipped += 1
                continue
            }
            var merged: [UInt32: SteamInstallRecord.Depot] = [:]
            let existing = SteamInstallFiles.record(appID: Int(ownerID), steamApps: steamApps)
            for d in SteamInstallRecord.installedDepots(in: existing) { merged[d.id] = d }
            for d in byOwner[ownerID] ?? [] { merged[d.depotID] = .init(id: d.depotID, manifest: d.manifestID, size: d.size) }
            let list = merged.values.sorted { $0.id < $1.id }
            let ownerRecord = SteamInstallRecord(appID: ownerID, name: info.name.isEmpty ? "App \(ownerID)" : info.name,
                                                 installDir: folderName, lastUpdated: now,
                                                 sizeOnDisk: list.reduce(0) { $0 + $1.size }, buildID: info.buildID,
                                                 lastOwner: lastOwner, installedDepots: list)
            owners.append((steamApps.appendingPathComponent("appmanifest_\(ownerID).acf"), ownerRecord.render()))
        }
        return RecordFiles(main: (mainURL, record.render()), owners: owners, ownersSkipped: skipped)
    }

    // MARK: Steam requests (§D3)

    /// The depot key, or nil when Steam refused it.
    private func depotKey(_ depot: UInt32, appID: UInt32) async throws -> Data? {
        if let cached = depotKeys[depot] { return cached }
        var w = ProtoWriter()
        w.uint32(1, depot)
        w.uint32(2, appID)
        let reply = try await session.request(SteamEMsg.clientGetDepotDecryptionKey, body: w.data, timeout: 15)
        let fields = try ProtoFields(reply.body)
        let result = Int(fields.int32(1) ?? 2)
        guard result == 1, let key = fields.bytes(3), key.count == 32 else {
            SteamLog.event("[steam-depot] depot=\(depot) key refused result=\(result)")
            return nil
        }
        let k = Data(key)
        depotKeys[depot] = k
        return k
    }

    private func contentServers(appID: UInt32) async -> [String] {
        var data = Data()
        if let url = SteamContentServers.directoryURL(cellID: session.cellID),
           let reply = try? await http.get(url), reply.status == 200 {
            data = reply.data
        }
        let parsed = SteamContentServers.parse(data, appID: appID)
        SteamLog.event("[steam-depot] content servers listed=\(parsed.listed) usable=\(parsed.hosts.count)")
        return parsed.hosts
    }

    private func authSuffixes(depot: UInt32, appID: UInt32, hosts: [String]) async -> [String: String] {
        var out: [String: String] = [:]
        for host in hosts {
            let cacheKey = "\(depot)|\(host)"
            if let cached = cdnTokens[cacheKey] {
                out[host] = cached
                continue
            }
            var w = ProtoWriter()
            w.uint32(1, depot)
            w.string(2, SteamContentServers.bareHost(host))
            w.uint32(3, appID)
            var suffix = ""
            if let reply = try? await session.callService("ContentServerDirectory.GetCDNAuthToken", body: w.data, timeout: 10),
               let token = (try? ProtoFields(reply.body))?.string(1) {
                suffix = SteamContentServers.authSuffix(token)
            }
            cdnTokens[cacheKey] = suffix
            out[host] = suffix
        }
        return out
    }

    private func manifestRequestCode(appID: UInt32, depot: UInt32, manifest: UInt64) async -> UInt64 {
        var w = ProtoWriter()
        w.uint32(1, appID)
        w.uint32(2, depot)
        w.uint64(3, manifest)
        guard let reply = try? await session.callService("ContentServerDirectory.GetManifestRequestCode",
                                                         body: w.data, timeout: 10),
              let fields = try? ProtoFields(reply.body) else { return 0 }
        return fields.uint64(1) ?? 0
    }

    private func fetchManifest(_ plan: Plan, code: UInt64, hosts: [String],
                               auth: [String: String]) async throws -> (files: [SteamManifestFile], bytes: Data) {
        let http = self.http
        for host in hosts.prefix(6) {
            try Task.checkCancellation()
            var path = "\(host)/depot/\(plan.depot.id)/manifest/\(plan.manifestID)/5"
            if code != 0 { path += "/\(code)" }
            path += auth[host] ?? ""
            guard let url = URL(string: path) else { continue }
            do {
                let (data, status) = try await http.get(url)
                guard status == 200, !data.isEmpty, data.count <= SteamCodec.maxManifestSize else {
                    SteamLog.event("[steam-depot] depot=\(plan.depot.id) manifest HTTP \(status)")
                    continue
                }
                let key = plan.key
                let decoded = try await Task.detached(priority: .userInitiated) {
                    try SteamManifestParser.decode(data, depotKey: key)
                }.value
                SteamLog.event("[steam-depot] depot=\(plan.depot.id) manifest files=\(decoded.files.count) bytes=\(data.count)")
                return decoded
            } catch {
                if error is CancellationError || Task.isCancelled { throw CancellationError() }
                SteamLog.event("[steam-depot] depot=\(plan.depot.id) manifest attempt failed: \(SteamLog.describe(error))")
            }
        }
        throw SteamError.manifestFailed(plan.depot.id)
    }
}
