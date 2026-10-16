// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Valve's KeyValues formats: the text form (spec 03 §L2, §K1), the binary form used in PICS
// package info (§L1), and the writer for install records (§D8).
// Written from the spec's description of the formats; no other implementation was consulted.

import Foundation

// MARK: - Values

/// A value of Valve's text KeyValues format.
indirect enum SteamValue: Sendable {
    case text(String)
    case object([String: SteamValue])

    /// [:] for text.
    nonisolated var fields: [String: SteamValue] {
        if case .object(let f) = self { return f }
        return [:]
    }

    /// nil for an object.
    nonisolated var string: String? {
        if case .text(let s) = self { return s }
        return nil
    }

    /// Case-insensitive key lookup (keys are stored lower-cased).
    nonisolated subscript(_ key: String) -> SteamValue? {
        guard case .object(let f) = self else { return nil }
        return f[key.lowercased()]
    }

    /// Follows a dotted path of keys, e.g. "common.name".
    nonisolated func path(_ dotted: String) -> SteamValue? {
        var v: SteamValue? = self
        for key in dotted.split(separator: ".", omittingEmptySubsequences: true) {
            v = v?[String(key)]
            if v == nil { return nil }
        }
        return v
    }
}

enum SteamFileError: LocalizedError {
    case invalid(String)

    var errorDescription: String? {
        switch self {
        case .invalid(let reason): return "The Steam file is not valid (\(reason))."
        }
    }
}

// MARK: - Text KeyValues

/// Limits for one parse. §K1 gives the file limits; app info (§L2) uses the larger set,
/// because PICS app info of big games is larger than an install record.
struct SteamKeyValuesLimits: Sendable {
    var maxInput: Int
    var maxTokens: Int
    var maxValue: Int
    var maxDepth: Int

    nonisolated static let files = SteamKeyValuesLimits(maxInput: 4 << 20, maxTokens: 100_000,
                                                        maxValue: 16_384, maxDepth: 32)
    nonisolated static let appInfo = SteamKeyValuesLimits(maxInput: 32 << 20, maxTokens: 2_000_000,
                                                          maxValue: 65_536, maxDepth: 32)
}

struct SteamKeyValues {
    private let bytes: [UInt8]
    private let limits: SteamKeyValuesLimits
    private var pos = 0
    private var tokens = 0

    nonisolated init(_ data: Data) throws {
        try self.init(data, limits: .files)
    }

    nonisolated init(_ data: Data, limits: SteamKeyValuesLimits) throws {
        guard data.count <= limits.maxInput else { throw SteamFileError.invalid("too large") }
        var b = [UInt8](data)
        if b.count >= 3, b[0] == 0xEF, b[1] == 0xBB, b[2] == 0xBF { b.removeFirst(3) }
        // PICS buffers end with NUL bytes; they carry no content.
        while let last = b.last, last == 0 { b.removeLast() }
        self.bytes = b
        self.limits = limits
    }

    /// The whole file as one object.
    nonisolated mutating func read() throws -> SteamValue {
        pos = 0
        tokens = 0
        return .object(try readPairs(depth: 0, closedByBrace: false))
    }

    private enum Token {
        case string(String)
        case open
        case close
    }

    nonisolated private mutating func readPairs(depth: Int, closedByBrace: Bool) throws -> [String: SteamValue] {
        guard depth <= limits.maxDepth else { throw SteamFileError.invalid("too deep") }
        var out: [String: SteamValue] = [:]
        while true {
            guard let tok = try nextToken() else {
                if closedByBrace { throw SteamFileError.invalid("incomplete file") }
                return out
            }
            let key: String
            switch tok {
            case .close:
                if closedByBrace { return out }
                throw SteamFileError.invalid("unexpected }")
            case .open:
                throw SteamFileError.invalid("unexpected {")
            case .string(let s):
                key = s.lowercased()
            }
            guard let valueTok = try nextToken() else { throw SteamFileError.invalid("incomplete file") }
            switch valueTok {
            case .string(let s):
                out[key] = .text(s)
            case .open:
                if depth + 1 > limits.maxDepth { throw SteamFileError.invalid("too deep") }
                out[key] = .object(try readPairs(depth: depth + 1, closedByBrace: true))
            case .close:
                throw SteamFileError.invalid("unexpected }")
            }
        }
    }

    nonisolated private static func isSpace(_ c: UInt8) -> Bool {
        c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D || c == 0x0B || c == 0x0C
    }

    nonisolated private mutating func nextToken() throws -> Token? {
        // Skip white space and // comments.
        while pos < bytes.count {
            let c = bytes[pos]
            if SteamKeyValues.isSpace(c) {
                pos += 1
            } else if c == UInt8(ascii: "/"), pos + 1 < bytes.count, bytes[pos + 1] == UInt8(ascii: "/") {
                while pos < bytes.count, bytes[pos] != 0x0A { pos += 1 }
            } else {
                break
            }
        }
        guard pos < bytes.count else { return nil }
        tokens += 1
        guard tokens <= limits.maxTokens else { throw SteamFileError.invalid("too large") }

        let c = bytes[pos]
        if c == UInt8(ascii: "{") { pos += 1; return .open }
        if c == UInt8(ascii: "}") { pos += 1; return .close }

        var value = [UInt8]()
        if c == UInt8(ascii: "\"") {
            pos += 1
            while true {
                guard pos < bytes.count else { throw SteamFileError.invalid("incomplete file") }
                let ch = bytes[pos]
                if ch == UInt8(ascii: "\"") { pos += 1; break }
                if ch == UInt8(ascii: "\\"), pos + 1 < bytes.count,
                   bytes[pos + 1] == UInt8(ascii: "\"") || bytes[pos + 1] == UInt8(ascii: "\\") {
                    value.append(bytes[pos + 1])
                    pos += 2
                } else {
                    // Any other backslash is literal (Windows paths).
                    value.append(ch)
                    pos += 1
                }
                if value.count > limits.maxValue { throw SteamFileError.invalid("too large") }
            }
        } else {
            while pos < bytes.count {
                let ch = bytes[pos]
                if SteamKeyValues.isSpace(ch) || ch == UInt8(ascii: "{") || ch == UInt8(ascii: "}")
                    || ch == UInt8(ascii: "\"") { break }
                value.append(ch)
                pos += 1
                if value.count > limits.maxValue { throw SteamFileError.invalid("too large") }
            }
        }
        return .string(String(decoding: value, as: UTF8.self))
    }
}

// MARK: - Binary KeyValues (PICS package info, §L1)

/// A node of binary KeyValues. Integers keep their value; strings their text.
indirect enum SteamBinaryValue {
    case section([(key: String, value: SteamBinaryValue)])
    case string(String)
    case int(Int64)
    case other

    nonisolated var entries: [(key: String, value: SteamBinaryValue)] {
        if case .section(let e) = self { return e }
        return []
    }

    nonisolated func child(_ key: String) -> SteamBinaryValue? {
        let lower = key.lowercased()
        return entries.first { $0.key.lowercased() == lower }?.value
    }

    /// Integer values of a section's children (strings that hold integers count too).
    nonisolated var integerValues: [Int64] {
        entries.compactMap { entry in
            switch entry.value {
            case .int(let v): return v
            case .string(let s): return Int64(s.trimmingCharacters(in: .whitespaces))
            default: return nil
            }
        }
    }

    /// Depth-first search for a section named `key`, at most `depth` levels down.
    nonisolated func find(_ key: String, depth: Int) -> SteamBinaryValue? {
        if let direct = child(key) { return direct }
        guard depth > 0 else { return nil }
        for entry in entries {
            if case .section = entry.value, let hit = entry.value.find(key, depth: depth - 1) { return hit }
        }
        return nil
    }
}

/// A recursive reader with bounded depth, node count and string size.
struct SteamBinaryKeyValues {
    private let data: Data
    private var pos = 0
    private var nodes = 0
    private let maxDepth = 32
    private let maxNodes = 1_000_000
    private let maxString = 65_536

    nonisolated init(_ data: Data, offset: Int = 0) {
        self.data = data
        self.pos = offset
    }

    /// Reads entries until an end marker or the end of the data, as one section.
    nonisolated mutating func read() throws -> SteamBinaryValue {
        guard data.count <= 64 << 20 else { throw SteamFileError.invalid("too large") }
        return try readSection(depth: 0, topLevel: true)
    }

    nonisolated private mutating func readSection(depth: Int, topLevel: Bool) throws -> SteamBinaryValue {
        guard depth <= maxDepth else { throw SteamFileError.invalid("too deep") }
        var entries: [(key: String, value: SteamBinaryValue)] = []
        while true {
            guard let type = data.steamByte(at: pos) else {
                if topLevel { return .section(entries) }
                throw SteamFileError.invalid("incomplete file")
            }
            pos += 1
            if type == 8 || type == 11 { return .section(entries) }   // end of section
            nodes += 1
            guard nodes <= maxNodes else { throw SteamFileError.invalid("too large") }
            let key = try readCString()
            let value: SteamBinaryValue
            switch type {
            case 0:
                value = try readSection(depth: depth + 1, topLevel: false)
            case 1:
                value = .string(try readCString())
            case 2:
                guard let v = data.steamU32(at: pos) else { throw SteamFileError.invalid("incomplete file") }
                pos += 4
                value = .int(Int64(Int32(bitPattern: v)))
            case 3, 4, 6:                        // float, pointer, colour: 4 bytes, not used here
                guard pos + 4 <= data.count else { throw SteamFileError.invalid("incomplete file") }
                pos += 4
                value = .other
            case 7, 10:                          // 64-bit integers
                guard let v = data.steamU64(at: pos) else { throw SteamFileError.invalid("incomplete file") }
                pos += 8
                value = .int(Int64(bitPattern: v))
            default:
                throw SteamFileError.invalid("unknown type \(type)")
            }
            entries.append((key, value))
        }
    }

    nonisolated private mutating func readCString() throws -> String {
        var end = pos
        while true {
            guard let b = data.steamByte(at: end) else { throw SteamFileError.invalid("incomplete file") }
            if b == 0 { break }
            end += 1
            if end - pos > maxString { throw SteamFileError.invalid("too large") }
        }
        let s = String(decoding: data.steamSlice(pos..<end) ?? Data(), as: UTF8.self)
        pos = end + 1
        return s
    }
}

// MARK: - Writer (install records, §D8)

/// Builds Valve's text layout: tabs indent, every key and value quoted, two tabs between them.
struct SteamKeyValuesWriter {
    private(set) var text = ""
    private var depth = 0

    nonisolated init() {}

    nonisolated static func escape(_ s: String) -> String {
        var out = ""
        out.reserveCapacity(s.count)
        for ch in s {
            if ch == "\\" || ch == "\"" { out.append("\\") }
            out.append(ch)
        }
        return out
    }

    nonisolated private var indent: String { String(repeating: "\t", count: depth) }

    nonisolated mutating func pair(_ key: String, _ value: String) {
        text += "\(indent)\"\(SteamKeyValuesWriter.escape(key))\"\t\t\"\(SteamKeyValuesWriter.escape(value))\"\n"
    }

    nonisolated mutating func open(_ key: String) {
        text += "\(indent)\"\(SteamKeyValuesWriter.escape(key))\"\n\(indent){\n"
        depth += 1
    }

    nonisolated mutating func close() {
        depth = max(0, depth - 1)
        text += "\(indent)}\n"
    }
}
