// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

enum BSteamCodec {
    static let limit = 64 * 1024 * 1024
    static func output(capacity: Int, _ body: (UnsafeMutableRawPointer, inout Int) -> Int32) throws -> Data {
        guard capacity >= 0, capacity <= 256 * 1024 * 1024 else { throw SteamError.invalidMessage }
        var result = Data(count: max(1, capacity)), count = 0
        let status = result.withUnsafeMutableBytes { (buffer: UnsafeMutableRawBufferPointer) -> Int32 in
            guard let base = buffer.baseAddress else { return -1 }; return body(base, &count)
        }
        guard status == 0, count >= 0, count <= capacity else { throw SteamError.invalidMessage }
        result.count = count; return result
    }
    static func decrypt(_ data: Data, key: Data) throws -> Data {
        guard key.count == 32, data.count >= 32, data.count <= limit else { throw SteamError.decryptionFailed }
        do {
            return try output(capacity: data.count) { destination, count in
                data.withUnsafeBytes { source in key.withUnsafeBytes { k in bsteam_aes(source.baseAddress, data.count, k.baseAddress, destination, data.count, &count) } }
            }
        } catch { throw SteamError.decryptionFailed }
    }
    static func inflate(_ data: Data, expected: Int, gzip: Bool) throws -> Data {
        guard data.count <= 256 * 1024 * 1024 else { throw SteamError.invalidMessage }
        let out = try output(capacity: expected) { destination, count in
            data.withUnsafeBytes { source in bsteam_inflate(source.baseAddress, data.count, destination, max(1,expected), gzip ? 1 : 0, &count) }
        }
        guard out.count == expected else { throw SteamError.invalidMessage }; return out
    }
    static func zip(_ data: Data, expected: Int?) throws -> Data {
        guard data.count >= 30, data.prefix(4) == Data([0x50,0x4b,3,4]) else { throw SteamError.invalidMessage }
        let flags = try data.bLE(6,2), method = try data.bLE(8,2), compressed = try data.bLE(18,4), original = try data.bLE(22,4)
        let header = 30 + Int(try data.bLE(26,2)) + Int(try data.bLE(28,2))
        guard flags & 1 == 0, header <= data.count, original <= UInt64(limit), compressed <= UInt64(data.count - header) else { throw SteamError.invalidMessage }
        let cap = expected ?? (original > 0 ? Int(original) : limit)
        guard cap <= limit, cap >= 0 else { throw SteamError.invalidMessage }
        let size = compressed > 0 ? Int(compressed) : data.count - header
        let content = data.subdata(in: header..<(header+size))
        let out: Data
        if method == 0 {
            let length = expected ?? Int(original)
            guard length <= content.count else { throw SteamError.invalidMessage }; out = Data(content.prefix(length))
        } else if method == 8 {
            out = try output(capacity: cap) { destination, count in
                content.withUnsafeBytes { source in bsteam_inflate(source.baseAddress, content.count, destination, max(1,cap), 0, &count) }
            }
        } else { throw SteamError.invalidMessage }
        if let expected = expected, out.count != expected { throw SteamError.invalidMessage }; return out
    }
    static func decode(_ data: Data, expected: Int?) throws -> Data {
        guard data.count <= limit else { throw SteamError.invalidMessage }
        if let expected = expected, expected < 0 || expected > limit { throw SteamError.invalidMessage }
        let out: Data
        if data.starts(with: [0x56,0x53,0x5a,0x61]) {
            guard data.count >= 23 else { throw SteamError.invalidMessage }
            let frame = data.subdata(in: 8..<(data.count-15))
            out = try output(capacity: expected ?? limit) { destination, count in
                frame.withUnsafeBytes { source in bsteam_zstd(source.baseAddress, frame.count, destination, expected ?? limit, &count) }
            }
        } else if data.starts(with: [0x56,0x5a]) {
            guard data.count >= 22 else { throw SteamError.invalidMessage }
            let original = try data.bLE(data.count-6,4)
            guard original <= UInt64(limit) else { throw SteamError.invalidMessage }
            if let expected = expected, original != UInt64(expected) { throw SteamError.invalidMessage }
            let stream = data.subdata(in: 12..<(data.count-10)), props = data.subdata(in: 7..<12)
            out = try output(capacity: Int(original)) { destination, count in
                stream.withUnsafeBytes { source in props.withUnsafeBytes { p in bsteam_lzma(source.baseAddress, stream.count, p.baseAddress, destination, max(1,Int(original)), &count) } }
            }
        } else if data.starts(with: [0x50,0x4b,3,4]) { out = try zip(data, expected: expected) }
        else {
            let decoded = try? output(capacity: expected ?? limit) { destination, count in
                data.withUnsafeBytes { source in bsteam_apple_lzma(source.baseAddress, data.count, destination, max(1,expected ?? limit), &count) }
            }
            if let decoded = decoded { out = decoded }
            else if expected == nil || expected == data.count { out = data }
            else { throw SteamError.chunkDecode(data.prefix(8).map { String(format: "%02x", $0) }.joined()) }
        }
        guard out.count <= limit, expected == nil || out.count == expected else { throw SteamError.invalidMessage }; return out
    }
    static func sha1(_ data: Data) -> Data {
        var digest = [UInt8](repeating: 0, count: 20)
        data.withUnsafeBytes { source in bsteam_sha1(source.baseAddress, data.count, &digest) }; return Data(digest)
    }
    static func adler(_ data: Data) -> UInt32 { data.withUnsafeBytes { source in bsteam_adler(source.baseAddress, data.count) } }
}
