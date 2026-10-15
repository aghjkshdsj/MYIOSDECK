// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

struct BSteamChunk: Sendable { var sha: Data; var crc: UInt32; var offset: UInt64; var original: Int; var compressed: UInt64 }
struct BSteamManifestFile: Sendable { var name: String; var size: UInt64; var flags: UInt32; var chunks: [BSteamChunk] }
struct BSteamManifest: Sendable {
    var depot: BSteamDepot
    var gid: UInt64
    var bytes: Data
    var files: [BSteamManifestFile]
    static func parse(_ source: Data, key: Data, depot: BSteamDepot, gid: UInt64) throws -> BSteamManifest {
        guard source.count <= BSteamCodec.limit else { throw SteamError.manifestFailed }
        let unwrapped: Data
        if source.starts(with:[0x50,0x4b,3,4]) { unwrapped = try BSteamCodec.zip(source,expected:nil) } else { unwrapped = source }
        func mappings(_ bytes: Data) throws -> [BSteamManifestFile] {
            let payload: Data
            if bytes.first == 0x0a { payload = bytes }
            else {
                guard bytes.count >= 8 else { throw SteamError.invalidMessage }
                let size = try bytes.bLE(4,4)
                guard size <= UInt64(BSteamCodec.limit), size <= UInt64(bytes.count-8) else { throw SteamError.invalidMessage }
                payload = bytes.subdata(in:8..<(8+Int(size)))
            }
            let entries = try BSteamProto(payload).messages(1)
            guard entries.count <= 100_000 else { throw SteamError.invalidMessage }
            var result: [BSteamManifestFile] = [], totalChunks = 0
            for entry in entries {
                guard var name = entry.string(1), name.utf8.count <= 4096 else { throw SteamError.invalidMessage }
                if let encrypted = Data(base64Encoded:name), let plain = try? BSteamCodec.decrypt(encrypted,key:key) {
                    var text = plain; while text.last == 0 { text.removeLast() }
                    if let decoded = String(data:text,encoding:.utf8), !decoded.unicodeScalars.contains(where:{ CharacterSet.controlCharacters.contains($0) }) { name = decoded }
                }
                name = name.replacingOccurrences(of:"\\",with:"/")
                let size = entry.number(2) ?? 0
                guard size <= UInt64(Int64.max), let flags = UInt32(exactly:entry.number(3) ?? 0) else { throw SteamError.invalidMessage }
                var chunks: [BSteamChunk] = []
                for chunk in try entry.messages(6) {
                    totalChunks += 1; guard totalChunks <= 1_000_000 else { throw SteamError.invalidMessage }
                    let offset = chunk.number(3) ?? 0, original = chunk.number(4) ?? 0, compressed = chunk.number(5) ?? 0
                    guard let sha = chunk.data(1), sha.count == 20, original <= UInt64(BSteamCodec.limit), compressed <= UInt64(BSteamCodec.limit),
                          offset <= size, original <= size-offset, let crc = UInt32(exactly:chunk.number(2) ?? 0) else { throw SteamError.invalidMessage }
                    chunks.append(BSteamChunk(sha:sha,crc:crc,offset:offset,original:Int(original),compressed:compressed))
                }
                let ordered = chunks.sorted { $0.offset < $1.offset }
                var end: UInt64 = 0
                for c in ordered { guard c.offset >= end else { throw SteamError.invalidMessage }; end = c.offset + UInt64(c.original) }
                result.append(BSteamManifestFile(name:name,size:size,flags:flags,chunks:chunks))
            }
            return result
        }
        if let files = try? mappings(unwrapped), !files.isEmpty { return BSteamManifest(depot:depot,gid:gid,bytes:unwrapped,files:files) }
        let decrypted = try BSteamCodec.decrypt(unwrapped,key:key)
        let decoded = (try? BSteamCodec.decode(decrypted,expected:nil)) ?? decrypted
        let files = try mappings(decoded)
        guard !files.isEmpty else { throw SteamError.manifestFailed }
        return BSteamManifest(depot:depot,gid:gid,bytes:decoded,files:files)
    }
}
enum BSteamDownloadPath {
    static func parts(_ name: String) -> [String]? {
        guard !name.isEmpty, name.utf8.count <= 1024, !name.contains(":"), !name.unicodeScalars.contains(where:{ CharacterSet.controlCharacters.contains($0) }) else { return nil }
        let parts = name.components(separatedBy:"/")
        guard parts.count <= 64, parts.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." && !$0.contains("\\") }) else { return nil }; return parts
    }
    static func fold(_ parts: [String], directory: Bool, known: inout [String:String]) -> [String] {
        var result: [String] = [], key: [String] = []
        for (i, part) in parts.enumerated() {
            key.append(part.lowercased())
            if directory || i < parts.count-1 {
                let path = key.joined(separator:"/"); let spelling = known[path] ?? part; known[path] = spelling; result.append(spelling)
            } else { result.append(part) }
        }
        return result
    }
}
