// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

enum BSteamField: Sendable { case number(UInt64), fixed32(UInt32), fixed64(UInt64), bytes(Data) }
struct BSteamProto: Sendable {
    var fields: [Int: [BSteamField]] = [:]
    init() {}
    init(_ data: Data, limit: Int = 64 * 1024 * 1024) throws {
        guard data.count <= limit else { throw SteamError.invalidMessage }
        let bytes = Array(data)
        var p = 0, count = 0
        func varint() throws -> UInt64 {
            var value: UInt64 = 0
            for shift in stride(from: 0, through: 63, by: 7) {
                guard p < bytes.count else { throw SteamError.invalidMessage }
                let b = bytes[p]; p += 1
                if shift == 63 && b > 1 { throw SteamError.invalidMessage }
                value |= UInt64(b & 127) << shift
                if b & 128 == 0 { return value }
            }
            throw SteamError.invalidMessage
        }
        while p < bytes.count {
            count += 1; guard count <= 1_000_000 else { throw SteamError.invalidMessage }
            let tag = try varint(), number = tag >> 3
            guard number > 0, number <= 0x1fffffff else { throw SteamError.invalidMessage }
            let field: BSteamField
            switch tag & 7 {
            case 0: field = .number(try varint())
            case 1:
                guard bytes.count - p >= 8 else { throw SteamError.invalidMessage }
                var value: UInt64 = 0; for i in 0..<8 { value |= UInt64(bytes[p+i]) << (i*8) }; p += 8; field = .fixed64(value)
            case 2:
                let length = try varint()
                guard length <= UInt64(bytes.count - p) else { throw SteamError.invalidMessage }
                let end = p + Int(length); field = .bytes(Data(bytes[p..<end])); p = end
            case 5:
                guard bytes.count - p >= 4 else { throw SteamError.invalidMessage }
                var value: UInt32 = 0; for i in 0..<4 { value |= UInt32(bytes[p+i]) << (i*8) }; p += 4; field = .fixed32(value)
            default: throw SteamError.invalidMessage
            }
            fields[Int(number), default: []].append(field)
        }
    }
    func number(_ field: Int) -> UInt64? {
        switch fields[field]?.last { case .number(let n), .fixed64(let n): return n; case .fixed32(let n): return UInt64(n); default: return nil }
    }
    func data(_ field: Int) -> Data? { if case .bytes(let d) = fields[field]?.last { return d }; return nil }
    func string(_ field: Int) -> String? { data(field).flatMap { String(data: $0, encoding: .utf8) } }
    func messages(_ field: Int) throws -> [BSteamProto] {
        try (fields[field] ?? []).map { value in guard case .bytes(let d) = value else { throw SteamError.invalidMessage }; return try BSteamProto(d) }
    }
    func numbers(_ field: Int) throws -> [UInt64] {
        var values: [UInt64] = []
        for value in fields[field] ?? [] {
            switch value {
            case .number(let n), .fixed64(let n): values.append(n)
            case .fixed32(let n): values.append(UInt64(n))
            case .bytes(let d):
                var reader = BSteamPacked(d)
                while !reader.finished { values.append(try reader.next()); guard values.count <= 100_000 else { throw SteamError.invalidMessage } }
            }
        }
        return values
    }
    mutating func uint(_ field: Int, _ value: UInt64) { fields[field, default: []].append(.number(value)) }
    mutating func fixed(_ field: Int, _ value: UInt64) { fields[field, default: []].append(.fixed64(value)) }
    mutating func bytes(_ field: Int, _ value: Data) { fields[field, default: []].append(.bytes(value)) }
    mutating func text(_ field: Int, _ value: String) { bytes(field, Data(value.utf8)) }
    func encoded() -> Data {
        var result = Data()
        func appendVarint(_ value: UInt64) {
            var n = value
            while n >= 128 { result.append(UInt8(n & 127) | 128); n >>= 7 }; result.append(UInt8(n))
        }
        for key in fields.keys.sorted() {
            for field in fields[key] ?? [] {
                switch field {
                case .number(let n): appendVarint(UInt64(key) << 3); appendVarint(n)
                case .fixed64(let n): appendVarint(UInt64(key) << 3 | 1); result.bAppendLE(n, count: 8)
                case .bytes(let d): appendVarint(UInt64(key) << 3 | 2); appendVarint(UInt64(d.count)); result.append(d)
                case .fixed32(let n): appendVarint(UInt64(key) << 3 | 5); result.bAppendLE(UInt64(n), count: 4)
                }
            }
        }
        return result
    }
}
private struct BSteamPacked {
    private var data: [UInt8]; private var p = 0
    init(_ data: Data) { self.data = Array(data) }
    var finished: Bool { p == data.count }
    mutating func next() throws -> UInt64 {
        var n: UInt64 = 0
        for shift in stride(from: 0, through: 63, by: 7) {
            guard p < data.count else { throw SteamError.invalidMessage }
            let b = data[p]; p += 1
            guard shift != 63 || b <= 1 else { throw SteamError.invalidMessage }
            n |= UInt64(b & 127) << shift
            if b & 128 == 0 { return n }
        }
        throw SteamError.invalidMessage
    }
}
extension Data {
    mutating func bAppendLE(_ n: UInt64, count: Int) { for i in 0..<count { append(UInt8(truncatingIfNeeded: n >> (i*8))) } }
    func bLE(_ offset: Int, _ count: Int) throws -> UInt64 {
        guard offset >= 0, count >= 0, count <= 8, offset <= self.count, count <= self.count - offset else { throw SteamError.invalidMessage }
        var n: UInt64 = 0; for i in 0..<count { n |= UInt64(self[startIndex + offset + i]) << (i*8) }; return n
    }
}
struct BSteamMessage: Sendable {
    var id: UInt32
    var header: BSteamProto
    var body: Data
    func encoded() -> Data {
        let h = header.encoded()
        var data = Data(); data.bAppendLE(UInt64(id | 0x80000000), count: 4); data.bAppendLE(UInt64(h.count), count: 4)
        data.append(h); data.append(body); return data
    }
    init(id: UInt32, header: BSteamProto = BSteamProto(), body: Data) { self.id = id; self.header = header; self.body = body }
    init(_ data: Data) throws {
        guard data.count <= 256 * 1024 * 1024 else { throw SteamError.invalidMessage }
        let raw = UInt32(try data.bLE(0,4)); id = raw & 0x7fffffff
        if raw & 0x80000000 != 0 {
            let size = try data.bLE(4,4)
            guard size <= 65_536, data.count >= 8, size <= UInt64(data.count - 8) else { throw SteamError.invalidMessage }
            let end = 8 + Int(size); header = try BSteamProto(data.subdata(in: 8..<end), limit: 65_536); body = data.subdata(in: end..<data.count)
        } else {
            header = BSteamProto()
            var extended = false
            if data.count >= 7 && data[data.startIndex+4] == 36 { extended = try data.bLE(5,2) == 2 }
            let offset = extended ? 36 : 20
            guard data.count >= offset else { throw SteamError.invalidMessage }
            header.fixed(11, try data.bLE(extended ? 7 : 4,8)); header.fixed(10, try data.bLE(extended ? 15 : 12,8))
            if extended { header.fixed(1, try data.bLE(24,8)); header.uint(2, try data.bLE(32,4)) }
            body = data.subdata(in: offset..<data.count)
        }
    }
}
enum BSteamWire {
    static func messages(_ data: Data) throws -> [BSteamMessage] {
        var budget = 256 * 1024 * 1024, count = 0
        func expand(_ data: Data, depth: Int) throws -> [BSteamMessage] {
            guard depth <= 8 else { throw SteamError.invalidMessage }
            let message = try BSteamMessage(data)
            count += 1; guard count <= 100_000 else { throw SteamError.invalidMessage }
            guard message.id == 1 else { return [message] }
            let multi = try BSteamProto(message.body, limit: 256 * 1024 * 1024)
            let declared = multi.number(1) ?? 0
            guard declared <= UInt64(budget), let payload = multi.data(2) else { throw SteamError.invalidMessage }
            let unpacked: Data
            if declared > 0 { unpacked = try BSteamCodec.inflate(payload, expected: Int(declared), gzip: true) }
            else { unpacked = payload }
            guard unpacked.count <= budget else { throw SteamError.invalidMessage }; budget -= unpacked.count
            var offset = 0, result: [BSteamMessage] = []
            while offset < unpacked.count {
                let length = try unpacked.bLE(offset,4); offset += 4
                guard length > 0, length <= UInt64(unpacked.count - offset) else { throw SteamError.invalidMessage }
                let end = offset + Int(length)
                result += try expand(unpacked.subdata(in: offset..<end), depth: depth + 1); offset = end
            }
            return result
        }
        return try expand(data, depth: 0)
    }
}

struct BSteamBinaryKV {
    private var data: [UInt8]; private var p = 0; private var count = 0
    init(_ data: Data) throws {
        guard data.count <= 4 * 1024 * 1024 else { throw SteamError.invalidMessage }; self.data = Array(data)
    }
    private mutating func text() throws -> String {
        let start = p
        while p < data.count && data[p] != 0 { p += 1; guard p-start <= 16_384 else { throw SteamError.invalidMessage } }
        guard p < data.count, let value = String(bytes: data[start..<p], encoding: .utf8) else { throw SteamError.invalidMessage }
        p += 1; return value
    }
    private mutating func object(_ depth: Int) throws -> SteamValue {
        guard depth <= 32 else { throw SteamError.invalidMessage }
        var fields: [String: SteamValue] = [:]
        while p < data.count {
            let type = data[p]; p += 1
            if type == 8 { return .object(fields) }
            count += 1; guard count <= 100_000 else { throw SteamError.invalidMessage }
            let key = try text().lowercased()
            switch type {
            case 0: fields[key] = try object(depth + 1)
            case 1: fields[key] = .text(try text())
            case 2:
                guard data.count-p >= 4 else { throw SteamError.invalidMessage }
                var n: UInt32 = 0; for i in 0..<4 { n |= UInt32(data[p+i]) << (8*i) }; p += 4
                fields[key] = .text(String(Int32(bitPattern: n)))
            default: throw SteamError.invalidMessage
            }
        }
        if depth > 0 { throw SteamError.invalidMessage }; return .object(fields)
    }
    mutating func read() throws -> SteamValue { try object(0) }
}

final class BSteamPromise<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var result: Result<T, Error>?
    private var continuations: [CheckedContinuation<T, Error>] = []
    func resolve(_ value: Result<T, Error>) {
        lock.lock()
        guard result == nil else { lock.unlock(); return }
        result = value; let waiters = continuations; continuations.removeAll(); lock.unlock()
        for waiter in waiters { waiter.resume(with: value) }
    }
    func value() async throws -> T {
        try await withCheckedThrowingContinuation { c in
            lock.lock()
            if let result = result { lock.unlock(); c.resume(with: result) }
            else { continuations.append(c); lock.unlock() }
        }
    }
}
func bSteamTimeout<T>(_ seconds: Double, operation: @escaping () async throws -> T) async throws -> T {
    let duration = seconds.isFinite ? min(3600, max(0.001, seconds)) : 10
    let promise = BSteamPromise<T>()
    let work = Task { do { promise.resolve(.success(try await operation())) } catch { promise.resolve(.failure(error)) } }
    let timer = Task {
        do { try await Task.sleep(nanoseconds: UInt64(duration * 1e9)); promise.resolve(.failure(SteamError.timeout)) } catch {}
    }
    defer { work.cancel(); timer.cancel() }
    return try await withTaskCancellationHandler(operation: { try Task.checkCancellation(); return try await promise.value() }, onCancel: { promise.resolve(.failure(CancellationError())) })
}
enum BSteamHTTP {
    static func data(_ request: URLRequest, session: URLSession, limit: Int) async throws -> (Data, HTTPURLResponse) {
        let (stream, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse, response.expectedContentLength <= Int64(limit) else { throw SteamError.invalidMessage }
        var data = Data(); data.reserveCapacity(min(limit, 65_536))
        for try await byte in stream {
            if data.count >= limit { throw SteamError.invalidMessage }
            data.append(byte)
        }
        try Task.checkCancellation()
        return (data, http)
    }
}
