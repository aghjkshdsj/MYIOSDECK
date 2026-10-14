// SPDX-License-Identifier: GPL-3.0-or-later
// Madeira Converter Exception: see LICENSE-EXCEPTION.md
//
// Steam Cloud saves for MYIOSDECK's Steam games (docs/STEAM_CLOUD.md): before a game starts its
// cloud saves come down to where it reads them, and when it ends (its own quit, Quit game, Force
// quit, a crash) what it changed goes up, through the app's own Steam connection (SteamLibrary's
// SwiftSteam session) with Steam's published Cloud service messages. The decisions are in
// SteamCloudCore.swift; this file does the transfers, the backups and the per-game record.
//
// The service-message parsing and the transfer helpers are ported from Madeira
// app/Madeira/SteamCloud.swift and SteamOwnedLibrary.swift at commit 65e6fe8f (willfaust/Madeira).
// MYIOSDECK changes: every download and every upload is all or nothing (all files fetched and
// checked before any is written; all parts sent before any file is committed, in one upload
// batch), backups go to Documents/save-backups/<App ID>/<time> with a manifest that Restore
// uses, a request Steam answered with an error never reads as an empty cloud, and the start and
// end of a game session are recorded so a session that started unsynced never overwrites the
// cloud unasked. Log tag [steam-cloud]: App IDs, counts and results; no account data or paths.

import Compression
import CryptoKit
import Foundation

// MARK: - Steam's answers

extension SteamCloudListing {
    /// CCloud_GetAppFileChangelist_Response: current_change_number = 1, files = 2 { file_name = 1,
    /// sha_file = 2, time_stamp = 3, raw_file_size = 4, persist_state = 5, path_prefix_index = 7 },
    /// is_only_delta = 3, path_prefixes = 4.
    static func parse(_ data: Data) throws -> SteamCloudListing {
        var decoder = ProtobufDecoder(data)
        var listing = SteamCloudListing()
        var prefixes: [String] = []
        var onlyDelta = false
        var raw: [(file: SteamCloudFile, prefixIndex: Int?)] = []
        while let tag = try decoder.readTag() {
            switch (tag.fieldNumber, tag.wireType) {
            case (1, .varint): listing.changeNumber = try decoder.readVarint()
            case (3, .varint): onlyDelta = try decoder.readVarint() != 0
            case (4, .lengthDelimited): prefixes.append(try decoder.readString())
            case (2, .lengthDelimited):
                var sub = ProtobufDecoder(try decoder.readBytes())
                var file = SteamCloudFile(prefix: "", name: "", sha: Data(), timestamp: 0, size: 0, persistState: 0)
                var prefixIndex: Int?
                while let field = try sub.readTag() {
                    switch (field.fieldNumber, field.wireType) {
                    case (1, .lengthDelimited): file.name = try sub.readString()
                    case (2, .lengthDelimited): file.sha = try sub.readBytes()
                    case (3, .varint): file.timestamp = try sub.readVarint()
                    case (4, .varint): file.size = try sub.readVarint()
                    case (5, .varint): file.persistState = UInt32(truncatingIfNeeded: try sub.readVarint())
                    case (7, .varint): prefixIndex = Int(truncatingIfNeeded: try sub.readVarint())
                    default: try sub.skip(wireType: field.wireType)
                    }
                }
                raw.append((file, prefixIndex))
                if raw.count > 20_000 { throw SteamFileError.invalid("Too many Steam Cloud files.") }
            default: try decoder.skip(wireType: tag.wireType)
            }
        }
        // The whole list was asked for (synced_change_number 0): a delta would not show what is gone.
        if onlyDelta { throw SteamFileError.invalid("Steam sent only part of the Steam Cloud file list.") }
        listing.files = raw.map { entry in
            var file = entry.file
            if let index = entry.prefixIndex, prefixes.indices.contains(index) { file.prefix = prefixes[index] }
            return file
        }
        return listing
    }
}

/// Where Steam says a cloud file can be fetched (Cloud.ClientFileDownload).
struct SteamCloudDownloadInfo: Sendable {
    var fileSize: UInt64 = 0      // bytes on the wire
    var rawSize: UInt64 = 0       // bytes of the file itself
    var host = ""
    var path = ""
    var https = true
    var headers: [(name: String, value: String)] = []
    var encrypted = false

    /// CCloud_ClientFileDownload_Response: file_size = 2, raw_file_size = 3, url_host = 7,
    /// url_path = 8, use_https = 9, request_headers = 10 { name = 1, value = 2 }, encrypted = 11.
    static func parse(_ data: Data) throws -> SteamCloudDownloadInfo {
        var decoder = ProtobufDecoder(data)
        var info = SteamCloudDownloadInfo()
        while let tag = try decoder.readTag() {
            switch (tag.fieldNumber, tag.wireType) {
            case (2, .varint): info.fileSize = try decoder.readVarint()
            case (3, .varint): info.rawSize = try decoder.readVarint()
            case (7, .lengthDelimited): info.host = try decoder.readString()
            case (8, .lengthDelimited): info.path = try decoder.readString()
            case (9, .varint): info.https = try decoder.readVarint() != 0
            case (11, .varint): info.encrypted = try decoder.readVarint() != 0
            case (10, .lengthDelimited):
                var sub = ProtobufDecoder(try decoder.readBytes())
                var name = "", value = ""
                while let field = try sub.readTag() {
                    switch (field.fieldNumber, field.wireType) {
                    case (1, .lengthDelimited): name = try sub.readString()
                    case (2, .lengthDelimited): value = try sub.readString()
                    default: try sub.skip(wireType: field.wireType)
                    }
                }
                if !name.isEmpty, info.headers.count < 32 { info.headers.append((name, value)) }
            default: try decoder.skip(wireType: tag.wireType)
            }
        }
        return info
    }

    var url: URL? {
        guard !host.isEmpty, !host.contains("/"), !host.contains("@") else { return nil }
        return URL(string: (https ? "https://" : "http://") + host + (path.hasPrefix("/") ? path : "/" + path))
    }
}

/// One HTTP request of a file upload (ClientCloudFileUploadBlockDetails).
struct SteamCloudUploadBlock: Sendable {
    var host = ""
    var path = ""
    var https = true
    var method: Int32 = 0
    var headers: [(name: String, value: String)] = []
    var offset: UInt64 = 0
    var length: UInt64 = 0
    var explicitBody = Data()

    /// CCloud_ClientBeginFileUpload_Response: encrypt_file = 1, block_requests = 2 { url_host = 1,
    /// url_path = 2, use_https = 3, http_method = 4, request_headers = 5 { name = 1, value = 2 },
    /// block_offset = 6, block_length = 7, explicit_body_data = 8 }.
    static func parse(_ data: Data) throws -> (encrypt: Bool, blocks: [SteamCloudUploadBlock]) {
        var decoder = ProtobufDecoder(data)
        var encrypt = false
        var blocks: [SteamCloudUploadBlock] = []
        while let tag = try decoder.readTag() {
            switch (tag.fieldNumber, tag.wireType) {
            case (1, .varint): encrypt = try decoder.readVarint() != 0
            case (2, .lengthDelimited):
                var sub = ProtobufDecoder(try decoder.readBytes())
                var block = SteamCloudUploadBlock()
                while let field = try sub.readTag() {
                    switch (field.fieldNumber, field.wireType) {
                    case (1, .lengthDelimited): block.host = try sub.readString()
                    case (2, .lengthDelimited): block.path = try sub.readString()
                    case (3, .varint): block.https = try sub.readVarint() != 0
                    case (4, .varint): block.method = Int32(truncatingIfNeeded: try sub.readVarint())
                    case (6, .varint): block.offset = try sub.readVarint()
                    case (7, .varint): block.length = try sub.readVarint()
                    case (8, .lengthDelimited): block.explicitBody = try sub.readBytes()
                    case (5, .lengthDelimited):
                        var header = ProtobufDecoder(try sub.readBytes())
                        var name = "", value = ""
                        while let part = try header.readTag() {
                            switch (part.fieldNumber, part.wireType) {
                            case (1, .lengthDelimited): name = try header.readString()
                            case (2, .lengthDelimited): value = try header.readString()
                            default: try header.skip(wireType: part.wireType)
                            }
                        }
                        if !name.isEmpty, block.headers.count < 32 { block.headers.append((name, value)) }
                    default: try sub.skip(wireType: field.wireType)
                    }
                }
                blocks.append(block)
                if blocks.count > 4096 { throw SteamFileError.invalid("Steam asked for too many upload parts.") }
            default: try decoder.skip(wireType: tag.wireType)
            }
        }
        return (encrypt, blocks)
    }

    var url: URL? {
        guard !host.isEmpty, !host.contains("/"), !host.contains("@") else { return nil }
        return URL(string: (https ? "https://" : "http://") + host + (path.hasPrefix("/") ? path : "/" + path))
    }
}

enum SteamCloudTransfer {
    /// Saves are small; anything larger than this is refused.
    static let maxFileBytes = 256 << 20

    private static let http: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 60
        config.urlCache = nil
        return URLSession(configuration: config)
    }()

    static func sha1(_ data: Data) -> Data { Data(Insecure.SHA1.hash(data: data)) }

    static func sha1(of url: URL) -> Data? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        var hasher = Insecure.SHA1()
        while true {
            guard let chunk = try? handle.read(upToCount: 1 << 20), !chunk.isEmpty else { break }
            hasher.update(data: chunk)
        }
        return Data(hasher.finalize())
    }

    /// The first entry of a ZIP archive (Steam sends a compressed cloud file as a one-entry
    /// archive): stored or deflated. Sizes come from the central directory, which is always
    /// filled in.
    static func unzipFirstEntry(_ data: Data, rawSize: Int) -> Data? {
        let bytes = [UInt8](data)
        func u16(_ at: Int) -> Int { Int(bytes[at]) | Int(bytes[at + 1]) << 8 }
        func u32(_ at: Int) -> Int { u16(at) | u16(at + 2) << 16 }
        guard bytes.count >= 30, u32(0) == 0x04034b50, rawSize >= 0, rawSize <= maxFileBytes else { return nil }
        let method = u16(8)
        let start = 30 + u16(26) + u16(28)
        var packed = u32(18)
        if packed == 0 {   // sizes deferred to the central directory
            var at = bytes.count - 46
            var found = false
            while at >= start {
                if u32(at) == 0x02014b50 { packed = u32(at + 20); found = true; break }
                at -= 1
            }
            guard found else { return nil }
        }
        guard start <= bytes.count, packed >= 0, start + packed <= bytes.count else { return nil }
        let payload = Array(bytes[start..<(start + packed)])
        if method == 0 { return payload.count == rawSize ? Data(payload) : nil }
        guard method == 8, rawSize > 0 else { return rawSize == 0 && method == 8 ? Data() : nil }
        let capacity = rawSize + 64
        var output = [UInt8](repeating: 0, count: capacity)
        let written = compression_decode_buffer(&output, capacity, payload, payload.count, nil, COMPRESSION_ZLIB)
        return written == rawSize ? Data(output[0..<rawSize]) : nil
    }

    /// Fetches one cloud file's bytes and checks them against the SHA-1 the cloud's own list
    /// gives. Throws rather than return anything unverified.
    static func fetch(_ info: SteamCloudDownloadInfo, expectedSHA: Data) async throws -> Data {
        guard !info.encrypted else { throw SteamFileError.invalid("This Steam Cloud file is encrypted, which is not supported.") }
        guard let url = info.url, info.fileSize <= UInt64(maxFileBytes), info.rawSize <= UInt64(maxFileBytes) else {
            throw SteamFileError.invalid("Steam gave no usable address for a Steam Cloud file.")
        }
        var request = URLRequest(url: url)
        for header in info.headers { request.setValue(header.value, forHTTPHeaderField: header.name) }
        let (body, response) = try await http.data(for: request)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            throw SteamFileError.invalid("A Steam Cloud download failed (HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0)).")
        }
        var file = body
        if info.fileSize != info.rawSize || (body.count != Int(info.rawSize) && body.starts(with: [0x50, 0x4b, 0x03, 0x04])) {
            guard let unpacked = unzipFirstEntry(body, rawSize: Int(info.rawSize)) else {
                throw SteamFileError.invalid("A Steam Cloud file could not be unpacked.")
            }
            file = unpacked
        }
        guard sha1(file) == expectedSHA else {
            throw SteamFileError.invalid("A Steam Cloud file did not match its checksum.")
        }
        return file
    }

    /// Sends the parts of one file where Steam asked for them (k_EHTTPMethodPOST is 3; Steam's
    /// storage otherwise takes PUT). Returns each part's HTTP status.
    @discardableResult
    static func send(_ file: Data, blocks: [SteamCloudUploadBlock]) async throws -> [Int] {
        var statuses: [Int] = []
        for block in blocks {
            guard let url = block.url else { throw SteamFileError.invalid("Steam gave no usable address for an upload.") }
            var body = block.explicitBody
            if body.isEmpty, block.length > 0 {
                let start = Int(min(block.offset, UInt64(file.count)))
                let end = Int(min(block.offset + block.length, UInt64(file.count)))
                guard end - start == Int(block.length) else { throw SteamFileError.invalid("Steam asked for a part outside the file.") }
                body = file.subdata(in: start..<end)
            }
            var request = URLRequest(url: url)
            request.httpMethod = block.method == 3 ? "POST" : "PUT"
            for header in block.headers { request.setValue(header.value, forHTTPHeaderField: header.name) }
            let (_, response) = try await uploadSession.upload(for: request, from: body)
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            statuses.append(status)
            guard (200..<300).contains(status) else { throw SteamFileError.invalid("A Steam Cloud upload failed (HTTP \(status)).") }
        }
        return statuses
    }

    private static let uploadSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 120
        config.urlCache = nil
        return URLSession(configuration: config)
    }()
}

// MARK: - Backups

/// One backup folder: Documents/save-backups/<App ID>/<time>/, with the saved files under
/// device/ (this device's copies a download or a restore replaced) and cloud/ (the cloud's copies
/// an upload replaced; Steam keeps none), and backup.json, which Restore reads. The Files app
/// shows the folder (On My iPhone › MYIOSDECK › save-backups).
struct SteamCloudBackup: Codable, Identifiable, Equatable, Sendable {
    struct File: Codable, Equatable, Sendable {
        /// The cloud path (%Root%...), which says where the file goes back.
        var cloudPath: String
        /// Where the copy is, relative to the backup folder.
        var stored: String
        /// "device" or "cloud".
        var side: String
        var size: UInt64
        var sha: String
        /// The file's own modification time (Unix seconds).
        var time: UInt64
    }
    var appID: Int
    var created: Date
    var reason: String
    var files: [File]
    /// The folder's name (its time), not stored.
    var folder = ""

    var id: String { folder }
    private enum CodingKeys: String, CodingKey { case appID, created, reason, files }
}

enum SteamCloudBackups {
    static var root: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("save-backups", isDirectory: true)
    }
    static func folder(_ appID: Int) -> URL { root.appendingPathComponent(String(appID), isDirectory: true) }

    /// Collects the files of one backup; the folder and its manifest are written as files arrive.
    final class Writer {
        let appID: Int
        let reason: String
        private(set) var files: [SteamCloudBackup.File] = []
        private var url: URL?
        private var created = Date()

        init(appID: Int, reason: String) { self.appID = appID; self.reason = reason }

        var isEmpty: Bool { files.isEmpty }
        var folderName: String? { url?.lastPathComponent }

        private func ensureFolder() throws -> URL {
            if let url { return url }
            let format = DateFormatter()
            format.locale = Locale(identifier: "en_US_POSIX")
            format.dateFormat = "yyyy-MM-dd HH-mm-ss"
            created = Date()
            let base = SteamCloudBackups.folder(appID)
            var name = format.string(from: created)
            var n = 2
            while FileManager.default.fileExists(atPath: base.appendingPathComponent(name).path) {
                name = format.string(from: created) + " (\(n))"; n += 1
            }
            let made = base.appendingPathComponent(name, isDirectory: true)
            try FileManager.default.createDirectory(at: made, withIntermediateDirectories: true)
            url = made
            return made
        }

        /// Copies `data` (one side's copy of the save at `cloudPath`) into the backup and writes the
        /// manifest. Throws when it cannot: the caller then changes nothing.
        func add(_ data: Data, cloudPath: String, side: String, time: UInt64) throws {
            let folder = try ensureFolder()
            let (rootName, rest) = SteamCloudPaths.split(cloudPath)
            var parts = [side, rootName ?? "remote"]
            parts += rest.replacingOccurrences(of: "\\", with: "/").split(separator: "/").map(String.init)
                .filter { $0 != "." && $0 != ".." && !$0.isEmpty }
            let stored = parts.joined(separator: "/")
            let target = folder.appendingPathComponent(stored)
            try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: target, options: .atomic)
            if time > 0 {
                try? FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: TimeInterval(time))], ofItemAtPath: target.path)
            }
            files.append(.init(cloudPath: cloudPath, stored: stored, side: side, size: UInt64(data.count),
                               sha: SteamCloudRecord.hex(SteamCloudTransfer.sha1(data)), time: time))
            let manifest = SteamCloudBackup(appID: appID, created: created, reason: reason, files: files)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            encoder.dateEncodingStrategy = .iso8601
            try encoder.encode(manifest).write(to: folder.appendingPathComponent("backup.json"), options: .atomic)
        }
    }

    /// The game's backups, newest first.
    static func list(_ appID: Int) -> [SteamCloudBackup] {
        let base = folder(appID)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: base.path)) ?? []
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        var out: [SteamCloudBackup] = []
        for name in names where !name.hasPrefix(".") {
            let manifest = base.appendingPathComponent(name).appendingPathComponent("backup.json")
            guard let data = try? Data(contentsOf: manifest), data.count < 8 << 20,
                  var backup = try? decoder.decode(SteamCloudBackup.self, from: data), backup.appID == appID else { continue }
            backup.folder = name
            out.append(backup)
        }
        return out.sorted { $0.created > $1.created }
    }

    /// Removes the backups SteamCloudBackupRetention lets go (never the newest ones).
    static func prune(_ appID: Int) {
        let all = list(appID)
        let drop = SteamCloudBackupRetention.expired(all.map { ($0.folder, $0.created) }, now: Date())
        for name in drop {
            try? FileManager.default.removeItem(at: folder(appID).appendingPathComponent(name, isDirectory: true))
        }
        if !drop.isEmpty { SteamLog.event("[steam-cloud] app=\(appID) backups pruned=\(drop.count) kept=\(all.count - drop.count)") }
    }
}

// MARK: - The sync

/// What Play does about a game's cloud saves before the game starts.
enum SteamCloudHold: Equatable {
    /// The saves could not be compared (offline, Steam did not answer): why.
    case failed(String)
    /// Saves that need the user's choice (count).
    case conflicts(Int)
}

@MainActor
final class SteamCloudSync: ObservableObject {
    static let shared = SteamCloudSync()

    /// Settings › Steam › Steam Cloud saves (on by default).
    static var enabled: Bool { UserDefaults.standard.object(forKey: "steamCloud") as? Bool ?? true }

    /// One game's state, as its page shows it.
    struct GameState: Equatable {
        enum Phase: Equatable {
            case idle, checking, failed(String)
            case downloading(done: Int, of: Int), uploading(done: Int, of: Int)
        }
        var phase: Phase = .idle
        var audit = SteamCloudAudit()
        var conflicts: [SteamCloudConflict] = []
        var droppedInCloud = 0
        var checked: Date?
        /// What the last sync did, in words.
        var lastResult: String?
    }

    @Published private(set) var games: [Int: GameState] = [:]
    /// The Steam game running now (its end triggers the upload).
    private(set) var activeApp: Int?
    private var busy: Set<Int> = []
    private var account: String?
    private var records: [Int: SteamCloudRecord] = [:]

    // MARK: Records (Application Support/MYIOSDECK/steam-cloud/<account hash>.json)

    private static func accountKey(_ name: String) -> String {
        SteamCloudRecord.hex(Data(SHA256.hash(data: Data(name.lowercased().utf8)))).prefix(16).description
    }
    private var recordsURL: URL? {
        guard let account else { return nil }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("MYIOSDECK/steam-cloud/\(Self.accountKey(account)).json")
    }
    private struct Stored: Codable { var version: Int; var apps: [String: SteamCloudRecord] }

    func record(_ appID: Int) -> SteamCloudRecord { records[appID] ?? SteamCloudRecord() }

    private func update(_ appID: Int, _ change: (inout SteamCloudRecord) -> Void) {
        var r = records[appID] ?? SteamCloudRecord()
        change(&r)
        records[appID] = r
        guard let url = recordsURL else { return }
        let stored = Stored(version: 1, apps: Dictionary(uniqueKeysWithValues: records.map { (String($0.key), $0.value) }))
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .iso8601
            try encoder.encode(stored).write(to: url, options: .atomic)
        } catch {
            SteamLog.event("[steam-cloud] record not saved: \(SteamSignIn.reason(error))")
        }
    }

    /// The signed-in account changed (SteamLibrary): load its records, and finish what the last
    /// app run left open (a game whose end-of-game upload did not happen, a sync that stopped).
    func accountChanged(_ name: String?) {
        guard name != account else { return }
        account = name
        records = [:]
        games = [:]
        guard let url = recordsURL, let data = try? Data(contentsOf: url) else { return }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        if let stored = try? decoder.decode(Stored.self, from: data) {
            for (key, value) in stored.apps { if let id = Int(key) { records[id] = value } }
        }
        let open = records.filter { $0.value.sessionOpen || $0.value.problem != nil }.keys.sorted()
        SteamLog.event("[steam-cloud] records=\(records.count) open=\(open.count) enabled=\(Self.enabled ? 1 : 0)")
        guard Self.enabled, !open.isEmpty else { return }
        Task {
            try? await Task.sleep(nanoseconds: 3_000_000_000)   // let the library connect first
            for id in open where self.activeApp == nil { await self.sync(id, reason: "start of MYIOSDECK") }
        }
    }

    // MARK: Context

    private struct Context {
        var paths: SteamCloudPaths
        var rules: [SteamCloudSaveRule]
        var buildID: UInt32
    }

    /// The Windows user folder games write to. A prefix can hold several folders under users; Wine
    /// names the live one after $USER, else the passwd name, so those are tried first, then the
    /// most recently changed. nil when the prefix has none yet. (Madeira's SteamCloudPaths.userFolder.)
    static func userFolder(drive: URL) -> URL? {
        let users = drive.appendingPathComponent("users", isDirectory: true)
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: users.path) else { return nil }
        let own = names.filter { !$0.hasPrefix(".") && $0.caseInsensitiveCompare("Public") != .orderedSame }
        func find(_ name: String?) -> URL? {
            guard let name, let match = own.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) else { return nil }
            return users.appendingPathComponent(match, isDirectory: true)
        }
        if let url = find(getenv("USER").map { String(cString: $0) }) { return url }
        if let entry = getpwuid(getuid()), let url = find(String(cString: entry.pointee.pw_name)) { return url }
        func changed(_ name: String) -> Date {
            let appData = users.appendingPathComponent(name + "/AppData", isDirectory: true)
            return (try? appData.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate ?? .distantPast
        }
        guard let newest = own.max(by: { changed($0) < changed($1) }) else { return nil }
        return users.appendingPathComponent(newest, isDirectory: true)
    }

    private static let noFolders = "This game's save folders were not found (it has not been installed or started yet)."

    /// Where the game's saves are, from Steam's product info (fetched online, kept in the record
    /// for offline use).
    private func context(_ appID: Int, online: Bool) async throws -> Context {
        let drive = SteamLibrary.drive
        guard let user = Self.userFolder(drive: drive) else { throw SteamFileError.invalid(Self.noFolders) }
        let library = SteamLibrary.shared
        let install: URL
        if let game = library.installed.first(where: { $0.id == appID }) {
            install = drive.appendingPathComponent(game.library + "/common/" + game.installDir, isDirectory: true)
        } else if let owned = library.game(appID) {
            install = SteamInstallPaths.common(drive: drive).appendingPathComponent(owned.folderName, isDirectory: true)
        } else {
            throw SteamFileError.invalid(Self.noFolders)
        }
        var steamID = record(appID).steamID
        var rules = record(appID).rules
        var overrides = record(appID).overrides
        var buildID = record(appID).buildID ?? 0
        if online {
            let session = library.cloudSession
            try await session.ensureConnected()
            steamID = session.steamID
            if let info = try await library.appInfo(appID) {
                rules = info.saveFiles.map { .init(root: $0.root, path: $0.path, pattern: $0.pattern, recursive: $0.recursive, platforms: $0.platforms) }
                overrides = info.rootOverrides.map { .init(root: $0.root, os: $0.os, useInstead: $0.useInstead, addPath: $0.addPath) }
                buildID = info.buildID
            }
            update(appID) { $0.steamID = steamID; $0.rules = rules; $0.overrides = overrides; $0.buildID = buildID }
        }
        guard let steamID, steamID != 0 else { throw SteamFileError.invalid("Steam Cloud needs a connection to Steam first.") }
        let remote = MadeiraDock.clientRoot.appendingPathComponent("userdata/\(steamID & 0xFFFF_FFFF)/\(appID)/remote", isDirectory: true)
        let paths = SteamCloudPaths(userFolder: user, installFolder: install, remoteFolder: remote,
                                    steamID: steamID, overrides: overrides ?? [])
        return Context(paths: paths, rules: rules ?? [], buildID: buildID)
    }

    // MARK: Steam's Cloud service

    private func call(_ method: SteamServiceMethod, _ body: ProtobufEncoder, timeout: TimeInterval = 30) async throws -> Data {
        let answer = try await SteamLibrary.shared.cloudSession.callServiceMethodResult(method: method, body: body.data, timeout: timeout)
        // 1 = OK. A header without a result (0) counts only with an answer in it.
        guard answer.eresult == 1 || (answer.eresult == 0 && !answer.body.isEmpty) else {
            throw SteamFileError.invalid("Steam Cloud refused the request (\(method.rawValue.split(separator: "#").first ?? ""), result \(answer.eresult)).")
        }
        return answer.body
    }

    private func listing(_ appID: Int) async throws -> SteamCloudListing {
        var request = ProtobufEncoder()
        request.writeUInt32(fieldNumber: 1, value: UInt32(appID))      // appid
        request.writeUInt64(fieldNumber: 2, value: 0)                  // synced_change_number: the whole list
        return try SteamCloudListing.parse(try await call(.cloudGetAppFileChangelist, request, timeout: 30))
    }

    /// One cloud file's bytes, checked against the SHA-1 of the comparison: a file that changed in
    /// the cloud since then throws.
    private func cloudFile(_ appID: Int, _ entry: SteamCloudEntry) async throws -> Data {
        var request = ProtobufEncoder()
        request.writeUInt32(fieldNumber: 1, value: UInt32(appID))    // appid
        request.writeString(fieldNumber: 2, value: entry.path)       // filename
        let info = try SteamCloudDownloadInfo.parse(try await call(.cloudClientFileDownload, request))
        return try await SteamCloudTransfer.fetch(info, expectedSHA: entry.cloudSHA)
    }

    /// A number Steam uses to tell this device's uploads from other machines'.
    private static var clientID: UInt64 {
        let key = "steamCloudClientID"
        if let text = UserDefaults.standard.string(forKey: key), let stored = UInt64(text), stored != 0 { return stored }
        let fresh = UInt64.random(in: 1...UInt64(Int64.max))
        UserDefaults.standard.set(String(fresh), forKey: key)
        return fresh
    }

    // MARK: Compare

    private func setPhase(_ appID: Int, _ phase: GameState.Phase) {
        var state = games[appID] ?? GameState()
        state.phase = phase
        games[appID] = state
    }

    /// Compares the game's cloud saves with the prefix and records the files found identical.
    /// Changes no save. Throws when the cloud could not be read.
    private func compare(_ appID: Int, _ context: Context) async throws -> (SteamCloudListing, SteamCloudAudit, SteamCloudPlan) {
        let list = try await listing(appID)
        if record(appID).implausible(list) {
            SteamLog.event("[steam-cloud] app=\(appID) listing implausible (files=\(list.files.count) change=\(list.changeNumber), recorded change=\(record(appID).changeNumber)): nothing copied")
            throw SteamFileError.invalid("Steam Cloud answered with an empty or older file list for this game than before. Nothing was copied either way; try again later.")
        }
        let rules = context.rules, paths = context.paths
        let audit = await Task.detached(priority: .utility) {
            SteamCloudAudit.run(listing: list, rules: rules, paths: paths, sha1: SteamCloudTransfer.sha1(of:))
        }.value
        let plan = SteamCloudPlan.make(audit: audit, record: record(appID))
        if !plan.settled.isEmpty { update(appID) { r in for (k, v) in plan.settled { r.files[k] = v } } }
        var state = games[appID] ?? GameState()
        state.audit = audit; state.conflicts = plan.conflicts; state.droppedInCloud = plan.droppedInCloud.count
        state.checked = Date()
        games[appID] = state
        let roots = Set(rules.map(\.root)).sorted().joined(separator: ",")
        SteamLog.event("[steam-cloud] app=\(appID) \(audit.summary) rules=\(rules.count) roots=\(roots.isEmpty ? "-" : roots) | plan download=\(plan.download.count) upload=\(plan.upload.count) ask=\(plan.conflicts.count) dropped-in-cloud=\(plan.droppedInCloud.count) unsynced-start=\(record(appID).unsyncedStart ? 1 : 0)")
        return (list, audit, plan)
    }

    /// Reads the cloud and compares, for the game's page. Changes no save.
    func check(_ appID: Int) async {
        guard Self.enabled, account != nil, activeApp != appID, !dockRunning, !busy.contains(appID) else { return }
        busy.insert(appID); defer { busy.remove(appID) }
        setPhase(appID, .checking)
        do {
            let context = try await context(appID, online: true)
            _ = try await compare(appID, context)
            setPhase(appID, .idle)
        } catch {
            setPhase(appID, .failed(SteamSignIn.message(error)))
            SteamLog.event("[steam-cloud] app=\(appID) check failed reason=\(SteamSignIn.reason(error))")
        }
    }

    // MARK: Sync

    /// Compares, then copies what changed on one side only: cloud changes come down (this device's
    /// copies backed up first), this device's changes go up (the cloud's copies backed up first).
    /// Saves that need a choice are left for the game's page. Returns nil when the comparison is
    /// complete and nothing is left to choose, else why not.
    @discardableResult
    func sync(_ appID: Int, reason: String) async -> SteamCloudHold? {
        guard Self.enabled else { return nil }
        guard account != nil else { return .failed("Sign in to Steam to sync Steam Cloud saves.") }
        guard !dockRunning else { return .failed("Valve's Steam client holds the account while the game runs; the saves sync when it ends.") }
        guard !busy.contains(appID), activeApp != appID else { return .failed("Steam Cloud is busy with this game.") }
        busy.insert(appID); defer { busy.remove(appID) }
        setPhase(appID, .checking)
        SteamLog.event("[steam-cloud] app=\(appID) sync (\(reason))")
        do {
            let context = try await context(appID, online: true)
            var (list, _, plan) = try await compare(appID, context)
            var done: [String] = []
            if !plan.download.isEmpty {
                let n = try await download(appID, plan.download, context, reason: "before download")
                done.append("downloaded \(n) save\(n == 1 ? "" : "s")")
            }
            if !plan.upload.isEmpty {
                let n = try await upload(appID, plan.upload, context)
                done.append("uploaded \(n) save\(n == 1 ? "" : "s")")
            }
            // The comparison after a transfer is what records uploaded files as in sync: a cloud
            // that does not list an upload yet shows it as still changed here (uploaded again
            // next time), never as changed in the cloud.
            if !done.isEmpty { (list, _, plan) = try await compare(appID, context) }
            let clear = plan.conflicts.isEmpty && plan.download.isEmpty && plan.upload.isEmpty
            update(appID) { r in
                r.problem = nil
                r.sessionOpen = false
                r.lastSync = Date()
                if clear { r.changeNumber = list.changeNumber; r.unsyncedStart = false }
            }
            var state = games[appID] ?? GameState()
            state.phase = .idle
            var did = ""
            if !done.isEmpty {
                let text = done.joined(separator: ", ")
                did = String(text.prefix(1)).uppercased() + String(text.dropFirst()) + " (replaced copies are in save-backups). "
            }
            if !plan.conflicts.isEmpty {
                state.lastResult = did + "\(plan.conflicts.count) save\(plan.conflicts.count == 1 ? "" : "s") need your choice."
            } else if !clear {
                state.lastResult = did + "Steam Cloud has not confirmed every transfer yet; they are checked again next time."
            } else {
                state.lastResult = did + "In sync with Steam Cloud."
            }
            games[appID] = state
            SteamCloudBackups.prune(appID)
            if !plan.conflicts.isEmpty { return .conflicts(plan.conflicts.count) }
            return clear ? nil : .failed("Steam Cloud has not confirmed the saves just copied. Try again in a moment.")
        } catch {
            let message = SteamSignIn.message(error)
            update(appID) { $0.problem = message }
            setPhase(appID, .failed(message))
            SteamLog.event("[steam-cloud] app=\(appID) sync stopped reason=\(SteamSignIn.reason(error)): nothing else changed, retried next time")
            return .failed(message)
        }
    }

    /// Downloads the given cloud files into the prefix, all or nothing: every file is fetched and
    /// checked against Steam's SHA-1 first, then this device's copies they replace are backed up,
    /// then they are written (each atomically).
    @discardableResult
    private func download(_ appID: Int, _ wanted: [SteamCloudEntry], _ context: Context, reason: String) async throws -> Int {
        setPhase(appID, .downloading(done: 0, of: wanted.count))
        var fetched: [(entry: SteamCloudEntry, url: URL, data: Data)] = []
        var total = 0
        for entry in wanted {
            guard let url = context.paths.url(cloudPath: entry.path) else { continue }
            let data = try await cloudFile(appID, entry)
            total += data.count
            guard total <= 1 << 30 else { throw SteamFileError.invalid("This game's Steam Cloud saves are too large to download.") }
            fetched.append((entry, url, data))
            setPhase(appID, .downloading(done: fetched.count, of: wanted.count))
        }
        // Back up every device file that will be replaced, before any is.
        let backup = SteamCloudBackups.Writer(appID: appID, reason: reason)
        for item in fetched where FileManager.default.fileExists(atPath: item.url.path) {
            let current = try Data(contentsOf: item.url)
            let time = (try? item.url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate?.timeIntervalSince1970 ?? 0
            try backup.add(current, cloudPath: item.entry.path, side: "device", time: UInt64(max(0, time)))
        }
        var settled: [String: SteamCloudRecord.FileState] = [:]
        for item in fetched {
            try FileManager.default.createDirectory(at: item.url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try item.data.write(to: item.url, options: .atomic)
            if item.entry.cloudTime > 0 {
                try? FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: TimeInterval(item.entry.cloudTime))],
                                                       ofItemAtPath: item.url.path)
            }
            settled[item.entry.key] = .init(state: .synced, sha: SteamCloudRecord.hex(item.entry.cloudSHA),
                                            size: item.entry.cloudSize, time: item.entry.cloudTime)
        }
        update(appID) { r in for (k, v) in settled { r.files[k] = v } }
        SteamLog.event("[steam-cloud] app=\(appID) downloaded files=\(fetched.count) backed-up=\(backup.files.count)")
        return fetched.count
    }

    /// Uploads the given device saves in one Steam upload batch, as close to all or nothing as
    /// Steam's service allows: the cloud's copies being replaced are fetched and backed up first
    /// (no backup, no upload); every file's parts are sent before any file is committed; a failed
    /// part commits none (each begun file is ended as failed) and fails the batch. Only a failure
    /// between two commits can leave part of a batch in the cloud; the next sync then sees those
    /// files as identical and retries the rest.
    @discardableResult
    private func upload(_ appID: Int, _ wanted: [SteamCloudEntry], _ context: Context) async throws -> Int {
        setPhase(appID, .uploading(done: 0, of: wanted.count))
        // The device files as they are now; each must still be what the comparison saw.
        var files: [(entry: SteamCloudEntry, data: Data, sha: Data, time: UInt64)] = []
        for entry in wanted {
            guard let url = context.paths.url(cloudPath: entry.path) else { continue }
            let data = try Data(contentsOf: url)
            guard data.count <= SteamCloudTransfer.maxFileBytes else { throw SteamFileError.invalid("A save is too large to upload.") }
            let sha = SteamCloudTransfer.sha1(data)
            guard sha == entry.localSHA else { throw SteamFileError.invalid("A save changed while it was being uploaded; it is uploaded next time.") }
            let time = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate?.timeIntervalSince1970
            files.append((entry, data, sha, UInt64(max(0, time ?? Date().timeIntervalSince1970))))
        }
        guard !files.isEmpty else { return 0 }
        // Steam keeps no copy of a cloud file an upload replaces: keep one here first.
        let replaced = files.filter { !$0.entry.cloudSHA.isEmpty }
        if !replaced.isEmpty {
            let backup = SteamCloudBackups.Writer(appID: appID, reason: "before upload")
            for item in replaced {
                let copy = try await cloudFile(appID, item.entry)
                try backup.add(copy, cloudPath: item.entry.path, side: "cloud", time: item.entry.cloudTime)
            }
            SteamLog.event("[steam-cloud] app=\(appID) cloud copies backed up before upload files=\(replaced.count)")
        }
        let session = SteamLibrary.shared.cloudSession
        var begin = ProtobufEncoder()
        begin.writeUInt32(fieldNumber: 1, value: UInt32(appID))          // appid
        begin.writeString(fieldNumber: 2, value: "MYIOSDECK (iPhone)")   // machine_name
        for item in files { begin.writeString(fieldNumber: 3, value: item.entry.path) }   // files_to_upload
        begin.writeUInt64(fieldNumber: 5, value: Self.clientID)          // client_id
        begin.writeUInt64(fieldNumber: 6, value: UInt64(context.buildID)) // app_build_id
        var batchID: UInt64 = 0
        var batch = ProtobufDecoder(try await call(.cloudBeginAppUploadBatch, begin))
        while let tag = try batch.readTag() {
            if tag.fieldNumber == 1, tag.wireType == .varint { batchID = try batch.readVarint() } else { try batch.skip(wireType: tag.wireType) }
        }
        guard batchID != 0 else { throw SteamFileError.invalid("Steam did not open an upload for this game.") }
        func complete(_ ok: Bool) async {
            var request = ProtobufEncoder()
            request.writeUInt32(fieldNumber: 1, value: UInt32(appID))        // appid
            request.writeUInt64(fieldNumber: 2, value: batchID)              // batch_id
            request.writeUInt32(fieldNumber: 3, value: ok ? 1 : 2)           // batch_eresult: OK / Fail
            _ = try? await session.callServiceMethodResult(method: .cloudCompleteAppUploadBatch, body: request.data, timeout: 30)
        }
        func commit(_ item: (entry: SteamCloudEntry, data: Data, sha: Data, time: UInt64), ok: Bool) async throws -> Bool {
            var request = ProtobufEncoder()
            request.writeBool(fieldNumber: 1, value: ok)                       // transfer_succeeded
            request.writeUInt32(fieldNumber: 2, value: UInt32(appID))          // appid
            request.writeBytes(fieldNumber: 3, value: item.sha)                // file_sha
            request.writeString(fieldNumber: 4, value: item.entry.path)        // filename
            var reply = ProtobufDecoder(try await call(.cloudClientCommitFileUpload, request))
            var committed = false
            while let tag = try reply.readTag() {
                if tag.fieldNumber == 1, tag.wireType == .varint { committed = try reply.readVarint() != 0 } else { try reply.skip(wireType: tag.wireType) }
            }
            return committed
        }
        // 1: every file's parts.
        var begun: [(entry: SteamCloudEntry, data: Data, sha: Data, time: UInt64)] = []
        do {
            for item in files {
                var request = ProtobufEncoder()
                request.writeUInt32(fieldNumber: 1, value: UInt32(appID))              // appid
                request.writeUInt32(fieldNumber: 2, value: UInt32(item.data.count))    // file_size
                request.writeUInt32(fieldNumber: 3, value: UInt32(item.data.count))    // raw_file_size
                request.writeBytes(fieldNumber: 4, value: item.sha)                    // file_sha
                request.writeUInt64(fieldNumber: 5, value: item.time)                  // time_stamp
                request.writeString(fieldNumber: 6, value: item.entry.path)            // filename
                request.writeUInt32(fieldNumber: 7, value: 0xFFFF_FFFF)                // platforms_to_sync: all
                request.writeUInt32(fieldNumber: 9, value: session.cellID)             // cell_id
                request.writeUInt64(fieldNumber: 13, value: batchID)                   // upload_batch_id
                let answer = try SteamCloudUploadBlock.parse(try await call(.cloudClientBeginFileUpload, request))
                begun.append(item)
                guard !answer.encrypt else { throw SteamFileError.invalid("Steam asked for an encrypted upload, which is not supported.") }
                let statuses = try await SteamCloudTransfer.send(item.data, blocks: answer.blocks)
                SteamLog.event("[steam-cloud] app=\(appID) sent file=\(begun.count)/\(files.count) bytes=\(item.data.count) parts=\(answer.blocks.count) http=\(statuses.map(String.init).joined(separator: ","))")
                setPhase(appID, .uploading(done: begun.count, of: files.count * 2))
            }
        } catch {
            for item in begun { _ = try? await commit(item, ok: false) }
            await complete(false)
            SteamLog.event("[steam-cloud] app=\(appID) upload abandoned before any commit: files=\(files.count) sent=\(begun.count)")
            throw error
        }
        // 2: commit them all. Nothing is recorded as in sync here: the comparison that follows
        // records what the cloud then lists.
        var done = 0
        do {
            for item in files {
                guard try await commit(item, ok: true) else { throw SteamFileError.invalid("Steam did not accept a save file.") }
                done += 1
                setPhase(appID, .uploading(done: files.count + done, of: files.count * 2))
            }
        } catch {
            for item in files.dropFirst(done) { _ = try? await commit(item, ok: false) }
            await complete(false)
            SteamLog.event("[steam-cloud] app=\(appID) upload stopped after commits=\(done)/\(files.count)")
            throw error
        }
        await complete(true)
        SteamLog.event("[steam-cloud] app=\(appID) uploaded files=\(done) batch=ok")
        return done
    }

    // MARK: Game sessions

    /// Before Play: waits for any running Steam Cloud work (Madeira Dock takes the account over),
    /// then syncs the game. nil: start; else the user decides (LibraryView).
    func beforePlay(_ appID: Int) async -> SteamCloudHold? {
        guard Self.enabled, account != nil else { return nil }
        await waitIdle()
        let hold = await sync(appID, reason: "before play")
        SteamLog.event("[steam-cloud] app=\(appID) before-play result=\(hold.map { "\($0)" } ?? "clear")")
        return hold
    }

    /// The game is starting. `synced`: its saves were synced just before (nothing held the start).
    /// `dock`: Valve's client (Madeira Dock) holds the account until the game ends, so the app's
    /// own connection must stay off: no Steam Cloud request is made meanwhile.
    func gameStarting(_ appID: Int, synced: Bool, dock: Bool) {
        activeApp = appID
        dockRunning = dock
        guard Self.enabled, account != nil else { return }
        update(appID) { r in
            r.sessionOpen = true
            if !synced { r.unsyncedStart = true }
        }
        if !synced { SteamLog.event("[steam-cloud] app=\(appID) started without a sync: nothing is uploaded over a cloud that moved meanwhile") }
    }

    /// The game's session ended (its own quit, Quit game, Force quit, a crash, a failed start):
    /// what it changed goes up. With Madeira Dock, Valve's client may have synced already; its
    /// uploads then compare as identical and nothing is sent twice.
    func gameEnded() {
        guard let appID = activeApp else { return }
        activeApp = nil
        dockRunning = false
        guard Self.enabled, account != nil else { return }
        // Other games whose end-of-game sync waited for the account (a Dock session held it).
        let open = records.filter { $0.value.sessionOpen && $0.key != appID }.keys.sorted()
        let ids = (record(appID).sessionOpen ? [appID] : []) + open
        guard !ids.isEmpty else { return }
        pending.formUnion(ids)
        Task {
            for id in ids {
                await self.sync(id, reason: id == appID ? "end of game" : "end of an earlier game")
                self.pending.remove(id)
            }
        }
    }

    /// Madeira Dock is about to take the account over (LibraryView.startThroughDock, before the
    /// app's connection logs off): no Steam Cloud request until the Dock game ends, or
    /// `releaseDock()` when it never started.
    func holdForDock() { dockRunning = true }
    func releaseDock() { if activeApp == nil { dockRunning = false } }

    /// Games whose end-of-game sync is about to run.
    private var pending: Set<Int> = []
    /// A Madeira Dock session holds the account (gameStarting).
    private var dockRunning = false

    /// Waits until no Steam Cloud work runs or waits to run (Madeira Dock logs the app's
    /// connection off before it starts), at most two minutes.
    func waitIdle() async {
        var waited = 0
        while (!busy.isEmpty || !pending.isEmpty) && waited < 600 {
            try? await Task.sleep(nanoseconds: 200_000_000)
            waited += 1
        }
    }

    // MARK: Choices

    /// The user's choice for saves that need one: the cloud's copies replace this device's (which
    /// are backed up first), or this device's replace the cloud's (whose copies are backed up
    /// first). A save missing here that the user keeps missing stays in the cloud.
    func resolve(_ appID: Int, _ chosen: [SteamCloudConflict], useCloud: Bool) async {
        guard Self.enabled, !chosen.isEmpty, !dockRunning, !busy.contains(appID), activeApp != appID else { return }
        busy.insert(appID)
        SteamLog.event("[steam-cloud] app=\(appID) choice=\(useCloud ? "cloud" : "device") files=\(chosen.count)")
        do {
            let context = try await context(appID, online: true)
            // Compare again: only what still differs as shown is acted on.
            let (_, audit, _) = try await compare(appID, context)
            let current = Dictionary(audit.entries.map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
            let picked = chosen.compactMap { c -> SteamCloudEntry? in
                guard let now = current[c.entry.key], now.cloudSHA == c.entry.cloudSHA, now.localSHA == c.entry.localSHA else { return nil }
                return now
            }
            if picked.count != chosen.count { throw SteamFileError.invalid("Saves changed since they were shown. Look again and choose.") }
            if useCloud {
                let down = picked.filter { $0.kind != .localOnly }
                if !down.isEmpty { try await download(appID, down, context, reason: "kept the cloud's copies") }
            } else {
                let gone = picked.filter { $0.kind == .cloudOnly }
                update(appID) { r in
                    for e in gone { r.files[e.key] = .init(state: .leftDeleted, sha: SteamCloudRecord.hex(e.cloudSHA), size: e.cloudSize, time: e.cloudTime) }
                }
                let up = picked.filter { $0.kind == .differ || $0.kind == .localOnly }
                if !up.isEmpty { try await upload(appID, up, context) }
            }
            busy.remove(appID)
        } catch {
            busy.remove(appID)
            let message = SteamSignIn.message(error)
            setPhase(appID, .failed(message))
            SteamLog.event("[steam-cloud] app=\(appID) choice stopped reason=\(SteamSignIn.reason(error))")
            return
        }
        await sync(appID, reason: "after a choice")
    }

    /// Puts a backup's files back where the game reads them. This device's current copies are
    /// backed up first (a new backup), so a restore can itself be undone. With a connection the
    /// usual sync follows (the restored saves go up unless the cloud changed meanwhile).
    func restore(_ appID: Int, _ backup: SteamCloudBackup) async -> String {
        guard !busy.contains(appID), activeApp != appID else { return "Wait until the game and its sync have ended." }
        busy.insert(appID)
        var restored = 0
        do {
            let context = try await self.context(appID, online: false)
            let folder = SteamCloudBackups.folder(appID).appendingPathComponent(backup.folder, isDirectory: true)
            var items: [(file: SteamCloudBackup.File, url: URL, data: Data)] = []
            for file in backup.files {
                guard let url = context.paths.url(cloudPath: file.cloudPath) else { continue }
                let data = try Data(contentsOf: folder.appendingPathComponent(file.stored))
                guard SteamCloudRecord.hex(SteamCloudTransfer.sha1(data)) == file.sha else {
                    throw SteamFileError.invalid("A file in this backup was changed or damaged; nothing was restored.")
                }
                items.append((file, url, data))
            }
            let before = SteamCloudBackups.Writer(appID: appID, reason: "before restoring \(backup.folder)")
            for item in items where FileManager.default.fileExists(atPath: item.url.path) {
                let current = try Data(contentsOf: item.url)
                if current == item.data { continue }
                try before.add(current, cloudPath: item.file.cloudPath, side: "device", time: 0)
            }
            for item in items {
                try FileManager.default.createDirectory(at: item.url.deletingLastPathComponent(), withIntermediateDirectories: true)
                try item.data.write(to: item.url, options: .atomic)
                if item.file.time > 0 {
                    try? FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: TimeInterval(item.file.time))], ofItemAtPath: item.url.path)
                }
                restored += 1
            }
            SteamLog.event("[steam-cloud] app=\(appID) restored files=\(restored) from a backup (current copies backed up: \(before.files.count))")
        } catch {
            busy.remove(appID)
            SteamLog.event("[steam-cloud] app=\(appID) restore failed reason=\(SteamSignIn.reason(error))")
            return "Not restored: \(SteamSignIn.message(error))"
        }
        busy.remove(appID)
        if account != nil { await sync(appID, reason: "after a restore") }
        return "Restored \(restored) file\(restored == 1 ? "" : "s")."
    }

    func isBusy(_ appID: Int) -> Bool { busy.contains(appID) }
}
