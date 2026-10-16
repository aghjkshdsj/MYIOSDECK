// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Depot manifests (spec 03 §D4), the install record writer (§D8), the resume record (§D7)
// and the content-server list (§D3). All of it runs off the main actor.
//
// Sources: spec 03 §D; refs/steam-protobufs/content_manifest.proto (ContentManifestPayload,
// FileMapping, ChunkData field numbers); POSIX open/pwrite/pread/fsync/ftruncate.

import Foundation

// MARK: - Manifest model

struct SteamManifestChunk: Sendable, Equatable {
    let sha: Data            // 20 bytes; also the chunk's ID
    let crc: UInt32
    let offset: UInt64
    let originalSize: UInt32
    let compressedSize: UInt32
}

struct SteamManifestFile: Sendable, Equatable {
    var name: String
    let size: UInt64
    let flags: UInt32
    let chunks: [SteamManifestChunk]

    nonisolated static let directoryFlag: UInt32 = 0x40
    nonisolated static let customExecutableFlag: UInt32 = 0x80
    nonisolated static let symlinkFlag: UInt32 = 0x200

    nonisolated var isDirectory: Bool { flags & Self.directoryFlag != 0 }
    nonisolated var isSymlink: Bool { flags & Self.symlinkFlag != 0 }
    nonisolated var isCustomExecutable: Bool { flags & Self.customExecutableFlag != 0 }
}

enum SteamManifestParser {
    /// The payload: the whole data when it starts with tag 0x0A, else the bytes after an
    /// 8-byte header (u32 magic, u32 payload length), bounded by that length.
    nonisolated static func payload(_ data: Data) -> Data? {
        if data.steamByte(at: 0) == 0x0A { return data }
        guard let length = data.steamU32(at: 4), Int(length) <= data.count - 8 else { return nil }
        return data.steamSlice(8..<(8 + Int(length)))
    }

    /// ContentManifestPayload → files. Throws on malformed protobuf.
    nonisolated static func parsePayload(_ payload: Data) throws -> [SteamManifestFile] {
        var files: [SteamManifestFile] = []
        var reader = ProtoReader(payload)
        while let (field, value) = try reader.next() {
            guard field == 1, case .bytes(let mapping) = value else { continue }
            files.append(try parseMapping(mapping))
            if files.count > 2_000_000 { throw SteamError.invalidMessage("manifest") }
        }
        return files
    }

    nonisolated private static func parseMapping(_ data: Data) throws -> SteamManifestFile {
        var name = ""
        var size: UInt64 = 0
        var flags: UInt32 = 0
        var chunks: [SteamManifestChunk] = []
        var reader = ProtoReader(data)
        while let (field, value) = try reader.next() {
            switch (field, value) {
            case (1, .bytes(let b)): name = String(decoding: b, as: UTF8.self)
            case (2, .varint(let v)): size = v
            case (3, .varint(let v)): flags = UInt32(truncatingIfNeeded: v)
            case (6, .bytes(let b)): chunks.append(try parseChunk(b))
            default: break
            }
        }
        return SteamManifestFile(name: name, size: size, flags: flags, chunks: chunks)
    }

    nonisolated private static func parseChunk(_ data: Data) throws -> SteamManifestChunk {
        var sha = Data()
        var crc: UInt32 = 0
        var offset: UInt64 = 0
        var original: UInt32 = 0
        var compressed: UInt32 = 0
        var reader = ProtoReader(data)
        while let (field, value) = try reader.next() {
            switch (field, value) {
            case (1, .bytes(let b)): sha = Data(b)
            case (2, .fixed32(let v)): crc = v
            case (2, .varint(let v)): crc = UInt32(truncatingIfNeeded: v)
            case (3, .varint(let v)): offset = v
            case (4, .varint(let v)): original = UInt32(truncatingIfNeeded: v)
            case (5, .varint(let v)): compressed = UInt32(truncatingIfNeeded: v)
            default: break
            }
        }
        return SteamManifestChunk(sha: sha, crc: crc, offset: offset, originalSize: original, compressedSize: compressed)
    }

    /// An encrypted file name: base64 of AES ciphertext under the depot key. Returns the
    /// decoded name when that works and it is UTF-8 without control characters; else `name`.
    nonisolated static func decodeFileName(_ name: String, key: Data) -> String {
        guard name.utf8.count >= 44, let cipher = Data(base64Encoded: name, options: .ignoreUnknownCharacters),
              var plain = SteamCrypto.symmetricDecrypt(cipher, key: key) else { return name }
        while let last = plain.last, last == 0 { plain.removeLast() }
        guard !plain.isEmpty, let text = String(data: plain, encoding: .utf8),
              !SteamPathRules.hasControlCharacter(text) else { return name }
        return text
    }

    /// The files of a manifest's payload; [] when there is no payload or it doesn't parse.
    nonisolated static func filesIn(_ data: Data) -> [SteamManifestFile] {
        guard let p = payload(data), let files = try? parsePayload(p) else { return [] }
        return files
    }

    /// Raw CDN body → (files with readable '/'-separated names, the unzipped manifest bytes).
    nonisolated static func decode(_ raw: Data, depotKey: Data) throws -> (files: [SteamManifestFile], bytes: Data) {
        var bytes = raw
        if SteamCodec.isZip(raw) {
            bytes = try SteamCodec.unzipFirstEntry(raw, expectedSize: nil, cap: SteamCodec.maxManifestSize)
        }
        var files = filesIn(bytes)
        if files.isEmpty {
            // The whole manifest is encrypted with the depot key, then maybe compressed.
            guard let decrypted = SteamCrypto.symmetricDecrypt(bytes, key: depotKey) else {
                throw SteamError.decryptionFailed
            }
            files = filesIn(SteamCodec.decompressManifest(decrypted))
        }
        guard !files.isEmpty else { throw SteamError.invalidMessage("manifest") }
        for i in files.indices {
            files[i].name = decodeFileName(files[i].name, key: depotKey).replacingOccurrences(of: "\\", with: "/")
        }
        return (files, bytes)
    }
}

// MARK: - Content servers (§D3)

enum SteamContentServers {
    nonisolated static let fallbackHost = "https://steampipe.akamaized.net"
    nonisolated static let allowedSuffixes = [".steamcontent.com", ".akamaized.net", ".steamstatic.com"]

    nonisolated static func directoryURL(cellID: UInt32) -> URL? {
        URL(string: "https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=\(cellID)&max_servers=20")
    }

    /// A plain DNS name under one of Steam's CDN domains: letters, digits, '-' and '.' only
    /// (so no port, slash, space or '*'), and never a "lancache".
    nonisolated static func isAllowedHost(_ host: String) -> Bool {
        let h = host.lowercased()
        guard !h.isEmpty, h.utf8.count <= 253, !h.contains("lancache"), !h.hasPrefix("."), !h.contains("..") else {
            return false
        }
        guard h.utf8.allSatisfy({ ($0 >= 0x61 && $0 <= 0x7A) || ($0 >= 0x30 && $0 <= 0x39) || $0 == 0x2D || $0 == 0x2E }) else {
            return false
        }
        return allowedSuffixes.contains { h.hasSuffix($0) && h.count > $0.count }
    }

    /// `https://<host>` entries in Steam's order, then the fallback host.
    nonisolated static func parse(_ data: Data, appID: UInt32) -> (hosts: [String], listed: Int) {
        var hosts: [String] = []
        var listed = 0
        if data.count <= 1 << 20,
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let response = object["response"] as? [String: Any],
           let servers = response["servers"] as? [[String: Any]] {
            listed = servers.count
            for s in servers.prefix(100) {
                let https = (s["https_support"] as? String ?? "").lowercased()
                guard https == "mandatory" || https == "optional" else { continue }
                if (s["use_as_proxy"] as? Bool) == true { continue }
                if let allowed = s["allowed_app_ids"] as? [Any] {
                    let ids = allowed.compactMap { ($0 as? NSNumber)?.uint32Value }
                    if !ids.contains(appID) { continue }
                }
                let vhost = (s["vhost"] as? String ?? "").trimmingCharacters(in: .whitespaces)
                let host = vhost.isEmpty ? (s["host"] as? String ?? "") : vhost
                guard isAllowedHost(host) else { continue }
                let url = "https://" + host.lowercased()
                if !hosts.contains(url) { hosts.append(url) }
            }
        }
        if !hosts.contains(fallbackHost) { hosts.append(fallbackHost) }
        return (hosts, listed)
    }

    /// The suffix for a CDN auth token (§D3).
    nonisolated static func authSuffix(_ token: String) -> String {
        if token.isEmpty { return "" }
        if token.hasPrefix("?") || token.hasPrefix("&") { return token }
        return "?auth=" + token
    }

    nonisolated static func bareHost(_ url: String) -> String {
        url.hasPrefix("https://") ? String(url.dropFirst("https://".count)) : url
    }
}

// MARK: - Install record (§D8)

struct SteamInstallRecord {
    struct Depot {
        var id: UInt32
        var manifest: UInt64
        var size: UInt64
    }

    var appID: UInt32
    var name: String
    var installDir: String
    var lastUpdated: Int64
    var sizeOnDisk: UInt64
    var buildID: UInt32
    var lastOwner: UInt64
    var installedDepots: [Depot]
    var sharedDepots: [(depot: UInt32, owner: UInt32)] = []
    var checkGuid: [String] = []

    /// Valve's text layout, in Valve's key order (§D8).
    nonisolated func render() -> String {
        var w = SteamKeyValuesWriter()
        w.open("AppState")
        w.pair("appid", String(appID))
        w.pair("universe", "1")
        w.pair("name", name)
        w.pair("StateFlags", "4")
        w.pair("installdir", installDir)
        w.pair("LastUpdated", String(lastUpdated))
        w.pair("LastPlayed", "0")
        w.pair("SizeOnDisk", String(sizeOnDisk))
        w.pair("StagingSize", "0")
        w.pair("buildid", String(buildID))
        w.pair("LastOwner", String(lastOwner))
        w.pair("DownloadType", "1")
        w.pair("UpdateResult", "0")
        w.pair("BytesToDownload", "0")
        w.pair("BytesDownloaded", "0")
        w.pair("BytesToStage", "0")
        w.pair("BytesStaged", "0")
        w.pair("TargetBuildID", String(buildID))
        w.pair("AutoUpdateBehavior", "0")
        w.pair("AllowOtherDownloadsWhileRunning", "0")
        w.pair("ScheduledAutoUpdate", "0")
        w.open("InstalledDepots")
        for d in installedDepots.sorted(by: { $0.id < $1.id }) {
            w.open(String(d.id))
            w.pair("manifest", String(d.manifest))
            w.pair("size", String(d.size))
            w.close()
        }
        w.close()
        if !sharedDepots.isEmpty {
            w.open("SharedDepots")
            for s in sharedDepots.sorted(by: { $0.depot < $1.depot }) {
                w.pair(String(s.depot), String(s.owner))
            }
            w.close()
        }
        if !checkGuid.isEmpty {
            w.open("CheckGuid")
            for (i, path) in checkGuid.prefix(256).enumerated() {
                w.pair(String(i), path)
            }
            w.close()
        }
        w.open("UserConfig")
        w.pair("language", "english")
        w.close()
        w.open("MountedConfig")
        w.pair("language", "english")
        w.close()
        w.close()
        return w.text
    }

    /// The depots listed in an existing record's InstalledDepots.
    nonisolated static func installedDepots(in state: SteamValue?) -> [Depot] {
        guard let section = state?["InstalledDepots"]?.fields else { return [] }
        var out: [Depot] = []
        for (key, value) in section {
            guard let id = UInt32(key) else { continue }
            out.append(Depot(id: id, manifest: UInt64(value["manifest"]?.string ?? "") ?? 0,
                             size: UInt64(value["size"]?.string ?? "") ?? 0))
        }
        return out
    }
}

// MARK: - Resume record (§D7)

/// Finished chunks of one depot manifest, one line each, appended and synced. Each line carries
/// its own checksum, and the file is cut back to its last complete line when opened, so a torn
/// last write costs at most that one chunk.
final class SteamResumeLog: @unchecked Sendable {
    let done: Set<String>
    private let lock = NSLock()
    private let fd: Int32

    nonisolated static func fileName(depot: UInt32, manifest: UInt64) -> String { "\(depot)_\(manifest).chunks" }

    nonisolated static func line(for key: String) -> String {
        let check = SteamCodec.adler32Seed0(Data(key.utf8)) ^ 0x5354_4D52
        return key + " " + String(format: "%08x", check) + "\n"
    }

    /// Opens (or creates) the record, deleting stale records of other manifests of the depot.
    nonisolated init(folder: URL, depot: UInt32, manifest: UInt64) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        let mine = Self.fileName(depot: depot, manifest: manifest)
        for name in (try? fm.contentsOfDirectory(atPath: folder.path)) ?? []
        where name.hasPrefix("\(depot)_") && name.hasSuffix(".chunks") && name != mine {
            try? fm.removeItem(at: folder.appendingPathComponent(name))
        }

        let url = folder.appendingPathComponent(mine)
        var keys = Set<String>()
        var validLength = 0
        if let data = try? Data(contentsOf: url) {
            var lineStart = 0
            let bytes = [UInt8](data)
            for (i, b) in bytes.enumerated() where b == 0x0A {
                let text = String(decoding: bytes[lineStart..<i], as: UTF8.self)
                if let space = text.lastIndex(of: " ") {
                    let key = String(text[..<space])
                    if Self.line(for: key) == text + "\n" { keys.insert(key) }
                }
                lineStart = i + 1
                validLength = lineStart
            }
        }
        done = keys
        fd = open(url.path, O_WRONLY | O_CREAT, 0o644)
        guard fd >= 0 else { throw SteamError.fileWriteFailed("resume record") }
        // Drop a torn last line, then append after the complete ones.
        _ = ftruncate(fd, off_t(validLength))
        _ = lseek(fd, off_t(validLength), SEEK_SET)
    }

    deinit {
        if fd >= 0 { close(fd) }
    }

    nonisolated func record(_ key: String) {
        let bytes = Array(Self.line(for: key).utf8)
        lock.lock()
        defer { lock.unlock() }
        guard fd >= 0 else { return }
        var written = 0
        while written < bytes.count {
            let n = bytes[written...].withUnsafeBufferPointer { write(fd, $0.baseAddress, $0.count) }
            if n <= 0 { return }
            written += n
        }
        _ = fsync(fd)
    }
}

// MARK: - File I/O

enum SteamFileIO {
    /// pwrite all of `data` at `offset`, then fsync, so a resume record written afterwards
    /// never claims bytes that aren't on disk.
    nonisolated static func write(_ data: Data, to url: URL, at offset: UInt64) throws {
        let fd = open(url.path, O_WRONLY)
        guard fd >= 0 else { throw SteamError.fileWriteFailed("open \(errno)") }
        defer { close(fd) }
        var done = 0
        try data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            guard let base = raw.baseAddress else { return }
            while done < raw.count {
                let n = pwrite(fd, base + done, raw.count - done, off_t(offset) + off_t(done))
                if n < 0 {
                    if errno == EINTR { continue }
                    throw SteamError.fileWriteFailed(errno == ENOSPC ? "disk full" : "write \(errno)")
                }
                if n == 0 { throw SteamError.fileWriteFailed("write") }
                done += n
            }
        }
        _ = fsync(fd)
    }

    /// The bytes at [offset, offset + length), or nil when the file is shorter.
    nonisolated static func read(_ url: URL, at offset: UInt64, length: Int) -> Data? {
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var out = Data(count: length)
        var done = 0
        let ok: Bool = out.withUnsafeMutableBytes { (raw: UnsafeMutableRawBufferPointer) in
            guard let base = raw.baseAddress else { return length == 0 }
            while done < length {
                let n = pread(fd, base + done, length - done, off_t(offset) + off_t(done))
                if n < 0, errno == EINTR { continue }
                if n <= 0 { return false }
                done += n
            }
            return true
        }
        return ok ? out : nil
    }

    /// Writes `data` atomically unless a file of the same size is already there.
    nonisolated static func writeIfSizeDiffers(_ data: Data, to url: URL) throws {
        if let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber,
           size.intValue == data.count { return }
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
    }
}
