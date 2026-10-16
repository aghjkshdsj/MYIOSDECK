// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// A small protobuf encoder and decoder for the Steam messages (spec 03 §S, §C, §L, §D).
// Written from the protobuf wire-format documentation (varint, fixed64, length-delimited,
// fixed32). Field numbers come from Valve's .proto files in refs/steam-protobufs/; no generated
// code is used.
//
// Everything here is a value type with nonisolated members, so it can run on any thread.

import Foundation

// MARK: - Data helpers (offsets are relative to startIndex, and never trap)

extension Data {
    nonisolated func steamByte(at offset: Int) -> UInt8? {
        guard offset >= 0, offset < count else { return nil }
        return self[startIndex + offset]
    }

    nonisolated func steamU16(at offset: Int) -> UInt16? {
        guard offset >= 0, offset + 2 <= count else { return nil }
        let i = startIndex + offset
        return UInt16(self[i]) | UInt16(self[i + 1]) << 8
    }

    nonisolated func steamU32(at offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= count else { return nil }
        let i = startIndex + offset
        var v: UInt32 = 0
        for k in 0..<4 { v |= UInt32(self[i + k]) << (8 * UInt32(k)) }
        return v
    }

    nonisolated func steamU64(at offset: Int) -> UInt64? {
        guard offset >= 0, offset + 8 <= count else { return nil }
        let i = startIndex + offset
        var v: UInt64 = 0
        for k in 0..<8 { v |= UInt64(self[i + k]) << (8 * UInt64(k)) }
        return v
    }

    /// The bytes in `range` (relative offsets) as a new, zero-based Data, or nil when out of range.
    nonisolated func steamSlice(_ range: Range<Int>) -> Data? {
        guard range.lowerBound >= 0, range.upperBound <= count, range.lowerBound <= range.upperBound else {
            return nil
        }
        return Data(self[(startIndex + range.lowerBound)..<(startIndex + range.upperBound)])
    }

    nonisolated mutating func steamAppend(u32 value: UInt32) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
    }

    nonisolated mutating func steamAppend(u64 value: UInt64) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { append(contentsOf: $0) }
    }

    /// Lower-case hex of all bytes.
    nonisolated var steamHex: String {
        let digits = Array("0123456789abcdef".utf8)
        var out = [UInt8]()
        out.reserveCapacity(count * 2)
        for b in self {
            out.append(digits[Int(b >> 4)])
            out.append(digits[Int(b & 0x0F)])
        }
        return String(decoding: out, as: UTF8.self)
    }
}

// MARK: - Writer

struct ProtoWriter {
    private(set) var data = Data()

    nonisolated init() {}

    nonisolated mutating func varint(_ value: UInt64) {
        var v = value
        while v >= 0x80 {
            data.append(UInt8(v & 0x7F) | 0x80)
            v >>= 7
        }
        data.append(UInt8(v))
    }

    nonisolated private mutating func tag(_ field: Int, _ wire: UInt64) {
        varint(UInt64(field) << 3 | wire)
    }

    nonisolated mutating func uint64(_ field: Int, _ value: UInt64) {
        tag(field, 0)
        varint(value)
    }

    nonisolated mutating func uint32(_ field: Int, _ value: UInt32) {
        uint64(field, UInt64(value))
    }

    /// Negative values are sign-extended to 64 bits, as protobuf requires for int32.
    nonisolated mutating func int32(_ field: Int, _ value: Int32) {
        uint64(field, UInt64(bitPattern: Int64(value)))
    }

    nonisolated mutating func bool(_ field: Int, _ value: Bool) {
        uint64(field, value ? 1 : 0)
    }

    nonisolated mutating func fixed64(_ field: Int, _ value: UInt64) {
        tag(field, 1)
        data.steamAppend(u64: value)
    }

    nonisolated mutating func fixed32(_ field: Int, _ value: UInt32) {
        tag(field, 5)
        data.steamAppend(u32: value)
    }

    nonisolated mutating func bytes(_ field: Int, _ value: Data) {
        tag(field, 2)
        varint(UInt64(value.count))
        data.append(value)
    }

    nonisolated mutating func string(_ field: Int, _ value: String) {
        bytes(field, Data(value.utf8))
    }

    nonisolated mutating func message(_ field: Int, _ value: ProtoWriter) {
        bytes(field, value.data)
    }
}

// MARK: - Reader

enum ProtoValue {
    case varint(UInt64)
    case fixed64(UInt64)
    case fixed32(UInt32)
    case bytes(Data)
}

/// Reads one field at a time. Bad data throws; it never traps.
struct ProtoReader {
    private let data: Data
    private var pos: Int          // relative offset
    nonisolated init(_ data: Data) {
        self.data = data
        self.pos = 0
    }

    nonisolated var isAtEnd: Bool { pos >= data.count }

    nonisolated private mutating func readVarint() throws -> UInt64 {
        var result: UInt64 = 0
        var shift: UInt64 = 0
        while true {
            guard let b = data.steamByte(at: pos) else { throw SteamError.invalidMessage("protobuf") }
            pos += 1
            if shift < 64 { result |= UInt64(b & 0x7F) << shift }
            if b & 0x80 == 0 { return result }
            shift += 7
            if shift >= 70 { throw SteamError.invalidMessage("protobuf") }
        }
    }

    /// The next field, or nil at the end. Groups (wire types 3 and 4) are rejected.
    nonisolated mutating func next() throws -> (field: Int, value: ProtoValue)? {
        if pos >= data.count { return nil }
        let key = try readVarint()
        let field = Int(truncatingIfNeeded: key >> 3)
        guard field > 0, field < (1 << 29) else { throw SteamError.invalidMessage("protobuf") }
        switch key & 7 {
        case 0:
            return (field, .varint(try readVarint()))
        case 1:
            guard let v = data.steamU64(at: pos) else { throw SteamError.invalidMessage("protobuf") }
            pos += 8
            return (field, .fixed64(v))
        case 2:
            let len = try readVarint()
            guard len <= UInt64(data.count - pos) else { throw SteamError.invalidMessage("protobuf") }
            let n = Int(len)
            let start = data.startIndex + pos
            pos += n
            return (field, .bytes(data[start..<(start + n)]))
        case 5:
            guard let v = data.steamU32(at: pos) else { throw SteamError.invalidMessage("protobuf") }
            pos += 4
            return (field, .fixed32(v))
        default:
            throw SteamError.invalidMessage("protobuf")
        }
    }
}

/// A whole (small) message, decoded into a field table. For large repeated data (manifests)
/// use ProtoReader directly.
struct ProtoFields {
    private var map: [Int: [ProtoValue]] = [:]

    nonisolated init(_ data: Data, maxFields: Int = 2_000_000) throws {
        var reader = ProtoReader(data)
        var n = 0
        while let (field, value) = try reader.next() {
            n += 1
            if n > maxFields { throw SteamError.invalidMessage("protobuf") }
            map[field, default: []].append(value)
        }
    }

    nonisolated func has(_ field: Int) -> Bool { map[field] != nil }

    nonisolated func all(_ field: Int) -> [ProtoValue] { map[field] ?? [] }

    /// The last varint, fixed64 or fixed32 value of `field` (protobuf: the last one wins).
    nonisolated func uint64(_ field: Int) -> UInt64? {
        guard let v = map[field]?.last else { return nil }
        switch v {
        case .varint(let x), .fixed64(let x): return x
        case .fixed32(let x): return UInt64(x)
        case .bytes: return nil
        }
    }

    nonisolated func uint32(_ field: Int) -> UInt32? {
        uint64(field).map { UInt32(truncatingIfNeeded: $0) }
    }

    nonisolated func int32(_ field: Int) -> Int32? {
        uint64(field).map { Int32(truncatingIfNeeded: $0) }
    }

    nonisolated func bool(_ field: Int) -> Bool? {
        uint64(field).map { $0 != 0 }
    }

    /// float fields arrive as fixed32.
    nonisolated func float(_ field: Int) -> Float? {
        guard let v = map[field]?.last, case .fixed32(let bits) = v else { return nil }
        return Float(bitPattern: bits)
    }

    nonisolated func bytes(_ field: Int) -> Data? {
        guard let v = map[field]?.last, case .bytes(let d) = v else { return nil }
        return d
    }

    nonisolated func string(_ field: Int) -> String? {
        bytes(field).map { String(decoding: $0, as: UTF8.self) }
    }

    nonisolated func messages(_ field: Int) throws -> [ProtoFields] {
        try all(field).compactMap { v -> ProtoFields? in
            guard case .bytes(let d) = v else { return nil }
            return try ProtoFields(d)
        }
    }

    nonisolated func message(_ field: Int) throws -> ProtoFields? {
        try messages(field).last
    }

    /// A repeated varint field, packed or not.
    nonisolated func repeatedUInt32(_ field: Int) throws -> [UInt32] {
        var out: [UInt32] = []
        for v in all(field) {
            switch v {
            case .varint(let x), .fixed64(let x):
                out.append(UInt32(truncatingIfNeeded: x))
            case .fixed32(let x):
                out.append(x)
            case .bytes(let d):
                var packed = ProtoReaderVarints(d)
                while let x = try packed.next() { out.append(UInt32(truncatingIfNeeded: x)) }
            }
        }
        return out
    }
}

/// Varints one after the other (a packed repeated field).
struct ProtoReaderVarints {
    private let data: Data
    private var pos = 0
    nonisolated init(_ data: Data) { self.data = data }

    nonisolated mutating func next() throws -> UInt64? {
        if pos >= data.count { return nil }
        var result: UInt64 = 0
        var shift: UInt64 = 0
        while true {
            guard let b = data.steamByte(at: pos) else { throw SteamError.invalidMessage("protobuf") }
            pos += 1
            if shift < 64 { result |= UInt64(b & 0x7F) << shift }
            if b & 0x80 == 0 { return result }
            shift += 7
            if shift >= 70 { throw SteamError.invalidMessage("protobuf") }
        }
    }
}
