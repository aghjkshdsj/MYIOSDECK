// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Decryption, decompression and checksums for Steam content (spec 03 §C5 Multi, §D4
// manifests, §D6 chunks). Pure functions, safe on any thread.
//
// Sources: Apple CommonCrypto (CCCrypt, AES-256 ECB/CBC, PKCS#7), CryptoKit (Insecure.SHA1),
// Compression (compression_stream, COMPRESSION_LZMA), RFC 1950 (Adler-32), RFC 1952 (gzip
// header), PKWARE APPNOTE (ZIP local file header). The C side is SteamCodecs.c and
// SteamZstdGlue.c.

import Compression
import CommonCrypto
import CryptoKit
import Foundation

enum SteamCrypto {
    /// AES-256 decryption with the given CommonCrypto options.
    nonisolated private static func aesDecrypt(_ input: Data, key: Data, iv: Data?, options: CCOptions) -> Data? {
        guard key.count == kCCKeySizeAES256, !input.isEmpty else { return nil }
        if let iv, iv.count != kCCBlockSizeAES128 { return nil }
        var out = Data(count: input.count + kCCBlockSizeAES128)
        let outCapacity = out.count
        var moved = 0
        let ivBytes = iv ?? Data(count: kCCBlockSizeAES128)
        let status: CCCryptorStatus = out.withUnsafeMutableBytes { outPtr in
            input.withUnsafeBytes { inPtr in
                key.withUnsafeBytes { keyPtr in
                    ivBytes.withUnsafeBytes { ivPtr in
                        CCCrypt(CCOperation(kCCDecrypt), CCAlgorithm(kCCAlgorithmAES), options,
                                keyPtr.baseAddress, key.count,
                                iv == nil ? nil : ivPtr.baseAddress,
                                inPtr.baseAddress, input.count,
                                outPtr.baseAddress, outCapacity, &moved)
                    }
                }
            }
        }
        guard status == CCCryptorStatus(kCCSuccess), moved <= outCapacity else { return nil }
        out.count = moved
        return out
    }

    /// Steam's symmetric scheme (§D6): the first 16 bytes, decrypted with AES-ECB, give the IV;
    /// the rest is AES-CBC with PKCS#7 padding. nil when the data or the padding is bad.
    nonisolated static func symmetricDecrypt(_ data: Data, key: Data) -> Data? {
        guard data.count >= 32, (data.count - 16) % 16 == 0 else { return nil }
        guard let head = data.steamSlice(0..<16), let body = data.steamSlice(16..<data.count) else { return nil }
        guard let iv = aesDecrypt(head, key: key, iv: nil, options: CCOptions(kCCOptionECBMode)),
              iv.count == 16 else { return nil }
        return aesDecrypt(body, key: key, iv: iv, options: CCOptions(kCCOptionPKCS7Padding))
    }

    nonisolated static func sha1(_ data: Data) -> Data {
        Data(Insecure.SHA1.hash(data: data))
    }
}

enum SteamCodec {
    /// Every decoded chunk is capped at 64 MB (§D6).
    nonisolated static let maxChunkSize = 64 << 20
    /// Manifests: our own bound, larger than any chunk.
    nonisolated static let maxManifestSize = 256 << 20
    /// Multi payloads (§C5).
    nonisolated static let maxMultiSize = 256 << 20

    // MARK: Adler-32

    /// Adler-32 computed with a starting value of 0 (not zlib's usual 1), as Steam's chunk
    /// `crc` field uses (§D6).
    nonisolated static func adler32Seed0(_ data: Data) -> UInt32 {
        let mod: UInt32 = 65521
        var a: UInt32 = 0
        var b: UInt32 = 0
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            var i = 0
            let n = raw.count
            while i < n {
                // 5552 is the largest run before b can overflow 32 bits (RFC 1950's NMAX).
                let end = min(i + 5552, n)
                while i < end {
                    a &+= UInt32(raw[i])
                    b &+= a
                    i += 1
                }
                a %= mod
                b %= mod
            }
        }
        return (b << 16) | a
    }

    // MARK: Raw deflate

    /// Inflates raw deflate data. With `exactSize`, the output buffer is that size; otherwise it
    /// grows up to `cap`. Throws on bad data or when the output would exceed the bound.
    nonisolated static func inflateRaw(_ input: Data, exactSize: Int?, cap: Int) throws -> Data {
        guard !input.isEmpty else { throw SteamError.chunkUndecodable("deflate") }
        var capacity: Int
        if let exactSize {
            guard exactSize >= 0, exactSize <= cap else { throw SteamError.chunkUndecodable("deflate size") }
            capacity = max(exactSize, 1)
        } else {
            capacity = min(cap, max(1 << 20, input.count * 8))
        }
        while true {
            var out = Data(count: capacity)
            var produced = 0
            let rc: Int32 = out.withUnsafeMutableBytes { dst in
                input.withUnsafeBytes { src in
                    steamcodec_inflate_raw(src.bindMemory(to: UInt8.self).baseAddress, src.count,
                                           dst.bindMemory(to: UInt8.self).baseAddress, dst.count, &produced)
                }
            }
            if rc == STEAMCODEC_OK {
                out.count = produced
                return out
            }
            if rc == STEAMCODEC_TOO_BIG, exactSize == nil, capacity < cap {
                capacity = min(cap, capacity * 4)
                continue
            }
            throw SteamError.chunkUndecodable(rc == STEAMCODEC_TOO_BIG ? "deflate too large" : "deflate")
        }
    }

    // MARK: gzip (RFC 1952), for CMsgMulti

    nonisolated static func gunzip(_ data: Data, declaredSize: Int) throws -> Data {
        let bad = SteamError.invalidMessage("gzip")
        guard data.steamByte(at: 0) == 0x1F, data.steamByte(at: 1) == 0x8B,
              data.steamByte(at: 2) == 8, let flags = data.steamByte(at: 3) else { throw bad }
        var pos = 10
        if flags & 0x04 != 0 {                         // FEXTRA
            guard let xlen = data.steamU16(at: pos) else { throw bad }
            pos += 2 + Int(xlen)
        }
        for bit: UInt8 in [0x08, 0x10] where flags & bit != 0 {   // FNAME, FCOMMENT
            while true {
                guard let c = data.steamByte(at: pos) else { throw bad }
                pos += 1
                if c == 0 { break }
            }
        }
        if flags & 0x02 != 0 { pos += 2 }              // FHCRC
        // The last 8 bytes are CRC32 and ISIZE; the deflate stream ends by itself before them.
        guard pos < data.count, let body = data.steamSlice(pos..<data.count) else { throw bad }
        let cap = min(declaredSize, maxMultiSize)
        guard declaredSize <= maxMultiSize else { throw bad }
        do {
            return try inflateRaw(body, exactSize: cap, cap: cap)
        } catch {
            throw bad
        }
    }

    // MARK: ZIP (one entry)

    nonisolated static func isZip(_ data: Data) -> Bool {
        data.steamU32(at: 0) == 0x0403_4B50
    }

    /// The first entry of a ZIP, stored (0) or deflated (8). `expectedSize`, when known, is
    /// used instead of the header's sizes (they may sit in a data descriptor).
    nonisolated static func unzipFirstEntry(_ data: Data, expectedSize: Int?, cap: Int) throws -> Data {
        let bad = SteamError.chunkUndecodable("zip")
        guard isZip(data),
              let flags = data.steamU16(at: 6),
              let method = data.steamU16(at: 8),
              let compressedSize = data.steamU32(at: 18),
              let uncompressedSize = data.steamU32(at: 22),
              let nameLength = data.steamU16(at: 26),
              let extraLength = data.steamU16(at: 28) else { throw bad }
        let start = 30 + Int(nameLength) + Int(extraLength)
        guard start <= data.count else { throw bad }
        let hasDescriptor = flags & 0x0008 != 0
        let headerSizesKnown = !hasDescriptor && compressedSize != 0xFFFF_FFFF

        var end = data.count
        if headerSizesKnown, Int(compressedSize) <= data.count - start {
            end = start + Int(compressedSize)
        }
        guard let body = data.steamSlice(start..<end) else { throw bad }

        switch method {
        case 0:
            let size = expectedSize ?? (headerSizesKnown ? Int(uncompressedSize) : body.count)
            guard size <= cap, size <= body.count, let stored = body.steamSlice(0..<size) else { throw bad }
            return stored
        case 8:
            if let expectedSize {
                return try inflateRaw(body, exactSize: expectedSize, cap: cap)
            }
            if headerSizesKnown, uncompressedSize != 0, Int(uncompressedSize) <= cap {
                return try inflateRaw(body, exactSize: Int(uncompressedSize), cap: cap)
            }
            return try inflateRaw(body, exactSize: nil, cap: cap)
        default:
            throw SteamError.chunkUndecodable("zip method \(method)")
        }
    }

    // MARK: Apple Compression (xz-wrapped LZMA)

    /// Streams `input` through Apple's decoder; nil on any error or when output exceeds `cap`.
    nonisolated static func appleDecode(_ input: Data, algorithm: compression_algorithm, cap: Int) -> Data? {
        guard !input.isEmpty else { return nil }
        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { stream.deallocate() }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, algorithm) == COMPRESSION_STATUS_OK else {
            return nil
        }
        defer { compression_stream_destroy(stream) }
        let bufferSize = 1 << 16
        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
        defer { buffer.deallocate() }

        return input.withUnsafeBytes { (src: UnsafeRawBufferPointer) -> Data? in
            guard let base = src.bindMemory(to: UInt8.self).baseAddress else { return nil }
            stream.pointee.src_ptr = base
            stream.pointee.src_size = src.count
            var out = Data()
            var idleRounds = 0
            while true {
                stream.pointee.dst_ptr = buffer
                stream.pointee.dst_size = bufferSize
                let status = compression_stream_process(stream, Int32(COMPRESSION_STREAM_FINALIZE.rawValue))
                let produced = bufferSize - stream.pointee.dst_size
                if produced > 0 {
                    out.append(buffer, count: produced)
                    idleRounds = 0
                } else {
                    idleRounds += 1
                }
                if out.count > cap { return nil }
                switch status {
                case COMPRESSION_STATUS_END:
                    return out
                case COMPRESSION_STATUS_OK:
                    if idleRounds > 2 { return nil }      // no progress: truncated or not this format
                default:
                    return nil
                }
            }
        }
    }

    // MARK: Zstandard and LZMA in Valve's wrappers

    nonisolated static func hasPrefix(_ data: Data, _ bytes: [UInt8]) -> Bool {
        guard data.count >= bytes.count else { return false }
        for (k, b) in bytes.enumerated() where data.steamByte(at: k) != b { return false }
        return true
    }

    nonisolated static let zstdMagic: [UInt8] = [0x56, 0x53, 0x5A, 0x61]     // "VSZa"
    nonisolated static let lzmaMagic: [UInt8] = [0x56, 0x5A]                 // "VZ"

    /// `VSZa`: the Zstandard frame is bytes 8 up to (length − 15).
    nonisolated static func decodeVSZa(_ data: Data, size: Int?, cap: Int) throws -> Data {
        let bad = SteamError.chunkUndecodable("zstd")
        guard data.count >= 8 + 15, let frame = data.steamSlice(8..<(data.count - 15)), !frame.isEmpty else { throw bad }
        var capacity: Int
        if let size {
            capacity = size
        } else {
            var declared: UInt64 = 0
            let rc = frame.withUnsafeBytes { src in
                steamcodec_zstd_content_size(src.bindMemory(to: UInt8.self).baseAddress, src.count, &declared)
            }
            guard rc == STEAMCODEC_OK else { throw bad }
            guard declared <= UInt64(cap) else { throw bad }
            capacity = Int(declared)
        }
        guard capacity <= cap else { throw bad }
        var out = Data(count: max(capacity, 1))
        var produced = 0
        let rc: Int32 = out.withUnsafeMutableBytes { dst in
            frame.withUnsafeBytes { src in
                steamcodec_zstd(src.bindMemory(to: UInt8.self).baseAddress, src.count,
                                dst.bindMemory(to: UInt8.self).baseAddress, capacity, &produced)
            }
        }
        guard rc == STEAMCODEC_OK else { throw bad }
        out.count = produced
        return out
    }

    /// `VZ`: props are bytes 7..<12, the stream is 12..<(length − 10), and the u32 at
    /// (length − 6) is the decoded size. `size`, when given, must match it.
    nonisolated static func decodeVZ(_ data: Data, size: Int?, cap: Int) throws -> Data {
        let bad = SteamError.chunkUndecodable("lzma")
        guard data.count >= 12 + 10,
              let props = data.steamSlice(7..<12),
              let stream = data.steamSlice(12..<(data.count - 10)),
              let declared = data.steamU32(at: data.count - 6) else { throw bad }
        if let size, Int(declared) != size { throw SteamError.chunkUndecodable("lzma size") }
        let length = Int(declared)
        guard length > 0, length <= cap, !stream.isEmpty else { throw bad }
        var out = Data(count: length)
        let rc: Int32 = out.withUnsafeMutableBytes { dst in
            stream.withUnsafeBytes { src in
                props.withUnsafeBytes { p in
                    steamcodec_lzma_raw(p.bindMemory(to: UInt8.self).baseAddress,
                                        src.bindMemory(to: UInt8.self).baseAddress, src.count,
                                        dst.bindMemory(to: UInt8.self).baseAddress, length)
                }
            }
        }
        guard rc == STEAMCODEC_OK else { throw bad }
        return out
    }

    // MARK: Chunks (§D6)

    /// Decompresses decrypted chunk data by its first bytes.
    nonisolated static func decompressChunk(_ data: Data, expectedSize: Int) throws -> Data {
        guard expectedSize >= 0, expectedSize <= maxChunkSize else { throw SteamError.chunkUndecodable("size") }
        if hasPrefix(data, zstdMagic) {
            return try decodeVSZa(data, size: expectedSize, cap: maxChunkSize)
        }
        if hasPrefix(data, lzmaMagic) {
            return try decodeVZ(data, size: expectedSize, cap: maxChunkSize)
        }
        if isZip(data) {
            return try unzipFirstEntry(data, expectedSize: expectedSize, cap: maxChunkSize)
        }
        if let decoded = appleDecode(data, algorithm: COMPRESSION_LZMA, cap: maxChunkSize), !decoded.isEmpty {
            return decoded
        }
        if data.count == expectedSize {
            return data
        }
        let head = data.steamSlice(0..<min(4, data.count)) ?? Data()
        throw SteamError.chunkUndecodable(head.steamHex)
    }

    /// Download → decrypt → decompress → check, for one chunk (§D6).
    nonisolated static func decodeChunk(_ raw: Data, key: Data, originalSize: Int, crc: UInt32) throws -> Data {
        guard let decrypted = SteamCrypto.symmetricDecrypt(raw, key: key) else {
            throw SteamError.decryptionFailed
        }
        let plain = try decompressChunk(decrypted, expectedSize: originalSize)
        guard plain.count == originalSize else { throw SteamError.checksumMismatch }
        if crc != 0, adler32Seed0(plain) != crc { throw SteamError.checksumMismatch }
        return plain
    }

    // MARK: Manifests (§D4)

    /// Best-effort decompression of a decrypted manifest whose size is not known in advance.
    /// Returns the input unchanged when it is in none of the known wrappers.
    nonisolated static func decompressManifest(_ data: Data) -> Data {
        if hasPrefix(data, zstdMagic), let d = try? decodeVSZa(data, size: nil, cap: maxManifestSize) { return d }
        if hasPrefix(data, lzmaMagic), let d = try? decodeVZ(data, size: nil, cap: maxManifestSize) { return d }
        if isZip(data), let d = try? unzipFirstEntry(data, expectedSize: nil, cap: maxManifestSize) { return d }
        if let d = appleDecode(data, algorithm: COMPRESSION_LZMA, cap: maxManifestSize), !d.isEmpty { return d }
        return data
    }
}
