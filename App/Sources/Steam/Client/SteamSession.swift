// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The logged-on connection to a Steam CM server (spec 03 §C1–C7).
//
// Sources: spec 03 §C; refs/steam-protobufs/ (CMsgProtoBufHeader, CMsgMulti, CMsgClientHello,
// CMsgClientLogon, CMsgClientLogonResponse, CMsgClientLoggedOff, CMsgClientLicenseList,
// CMsgIPAddress; EMsg numbers in enums_clientserver.proto); Apple Foundation
// (URLSessionWebSocketTask: protocols:, maximumMessageSize, sendPing, receive, cancel(with:));
// getifaddrs(3) for the device's IPv4 address.

import Foundation
import UIKit

// MARK: - Message numbers (§C3)

enum SteamEMsg {
    static let multi: UInt32 = 1
    static let serviceMethod: UInt32 = 146
    static let serviceMethodResponse: UInt32 = 147
    static let serviceMethodCallFromClient: UInt32 = 151
    static let clientHeartBeat: UInt32 = 703
    static let clientLogOff: UInt32 = 706
    static let clientLogOnResponse: UInt32 = 751            // protobuf or the older form
    static let clientLoggedOff: UInt32 = 757
    static let clientLicenseList: UInt32 = 780
    static let clientGetDepotDecryptionKey: UInt32 = 5438
    static let clientGetDepotDecryptionKeyResponse: UInt32 = 5439
    static let clientLogon: UInt32 = 5514
    static let clientPICSProductInfoRequest: UInt32 = 8903
    static let clientPICSProductInfoResponse: UInt32 = 8904
    static let clientPICSAccessTokenRequest: UInt32 = 8905
    static let clientPICSAccessTokenResponse: UInt32 = 8906
    static let clientHello: UInt32 = 9805
}

// MARK: - Packets (§C3)

struct SteamPacket {
    var emsg: UInt32
    var isProtobuf: Bool
    var steamID: UInt64 = 0
    var sessionID: Int32 = 0
    var jobSource: UInt64 = .max
    var jobTarget: UInt64 = .max
    var targetJobName: String?
    var eresult: Int32?
    var body: Data
}

enum SteamPacketCodec {
    nonisolated static let protoFlag: UInt32 = 0x8000_0000
    /// Public universe, individual account, desktop instance, account 0 (§C4).
    nonisolated static let anonymousSteamID: UInt64 = 76_561_197_960_265_728

    nonisolated static func encode(emsg: UInt32, steamID: UInt64, sessionID: Int32,
                                   jobSource: UInt64?, targetJobName: String?, body: Data) -> Data {
        var header = ProtoWriter()
        header.fixed64(1, steamID)
        header.int32(2, sessionID)
        if let jobSource { header.fixed64(10, jobSource) }
        if let targetJobName { header.string(12, targetJobName) }
        var out = Data()
        out.steamAppend(u32: emsg | protoFlag)
        out.steamAppend(u32: UInt32(header.data.count))
        out.append(header.data)
        out.append(body)
        return out
    }

    nonisolated static func decode(_ data: Data) throws -> SteamPacket {
        guard let raw = data.steamU32(at: 0) else { throw SteamError.invalidMessage("short") }
        let emsg = raw & ~protoFlag
        if raw & protoFlag != 0 {
            guard let length = data.steamU32(at: 4), Int(length) <= data.count - 8,
                  let headerBytes = data.steamSlice(8..<(8 + Int(length))),
                  let body = data.steamSlice((8 + Int(length))..<data.count) else {
                throw SteamError.invalidMessage("header")
            }
            let h = try ProtoFields(headerBytes)
            var p = SteamPacket(emsg: emsg, isProtobuf: true, body: body)
            p.steamID = h.uint64(1) ?? 0
            p.sessionID = h.int32(2) ?? 0
            p.jobSource = h.uint64(10) ?? .max
            p.jobTarget = h.uint64(11) ?? .max
            p.targetJobName = h.string(12)
            p.eresult = h.int32(13)
            return p
        }
        // Older messages: the 36-byte extended header, or the 20-byte simple one.
        if data.steamByte(at: 4) == 36, data.steamU16(at: 5) == 2 {
            guard let target = data.steamU64(at: 7), let source = data.steamU64(at: 15),
                  let sid = data.steamU64(at: 24), let session = data.steamU32(at: 32),
                  let body = data.steamSlice(36..<data.count) else { throw SteamError.invalidMessage("header") }
            var p = SteamPacket(emsg: emsg, isProtobuf: false, body: body)
            p.jobTarget = target
            p.jobSource = source
            p.steamID = sid
            p.sessionID = Int32(bitPattern: session)
            return p
        }
        guard let target = data.steamU64(at: 4), let source = data.steamU64(at: 12),
              let body = data.steamSlice(20..<data.count) else { throw SteamError.invalidMessage("header") }
        var p = SteamPacket(emsg: emsg, isProtobuf: false, body: body)
        p.jobTarget = target
        p.jobSource = source
        return p
    }

    /// The messages inside a CMsgMulti body (§C5).
    nonisolated static func unpackMulti(_ body: Data) throws -> [Data] {
        let multi = try ProtoFields(body)
        guard var payload = multi.bytes(2).map({ Data($0) }) else { return [] }
        let unzipped = Int(multi.uint32(1) ?? 0)
        if unzipped > 0 {
            payload = try SteamCodec.gunzip(payload, declaredSize: unzipped)
        }
        var out: [Data] = []
        var pos = 0
        while pos < payload.count {
            guard let length = payload.steamU32(at: pos), Int(length) <= payload.count - pos - 4,
                  let message = payload.steamSlice((pos + 4)..<(pos + 4 + Int(length))) else {
                throw SteamError.invalidMessage("multi")
            }
            out.append(message)
            pos += 4 + Int(length)
            if out.count > 100_000 { throw SteamError.invalidMessage("multi") }
        }
        return out
    }

    /// `response_pending` (field 6) of a CMsgClientPICSProductInfoResponse; absent is false.
    nonisolated static func responsePending(_ body: Data) -> Bool {
        var reader = ProtoReader(body)
        var pending = false
        while let field = try? reader.next() {
            if field.field == 6, case .varint(let v) = field.value { pending = v != 0 }
        }
        return pending
    }
}

// MARK: - What the library and downloader need from a session

/// SteamSession conforms; tests pass a scripted double.
@MainActor
protocol SteamConnection: AnyObject {
    /// The logged-on Steam ID, 0 if unknown.
    var steamID: UInt64 { get }
    /// The cell ID from the last logon, 0 if unknown.
    var cellID: UInt32 { get }
    /// The package IDs of the latest licence list (logs on first; waits up to 15 s).
    func licensedPackageIDs() async throws -> [UInt32]
    /// One request, one response (matched by job ID).
    func request(_ emsg: UInt32, body: Data, timeout: TimeInterval) async throws -> SteamPacket
    /// One request, a multi-part response (product info, §C5).
    func requestParts(_ emsg: UInt32, body: Data, timeout: TimeInterval) async throws -> [SteamPacket]
    /// A unified service method, e.g. "ContentServerDirectory.GetCDNAuthToken".
    func callService(_ method: String, body: Data, timeout: TimeInterval) async throws -> SteamPacket
}

// MARK: - One-shot results

/// A result that is set exactly once. Every racing outcome (a reply, a logged-off message, a
/// send error, a timeout, a disconnect, cancellation) calls resolve(); the first one wins and
/// the rest are ignored. This is the "resolve once" rule of §C4.
@MainActor
final class SteamWaiter<T> {
    private var continuation: CheckedContinuation<T, Error>?
    private var early: Result<T, Error>?
    private var timeoutTask: Task<Void, Never>?
    private(set) var isResolved = false

    init() {}

    @discardableResult
    func resolve(_ result: Result<T, Error>) -> Bool {
        guard !isResolved else { return false }
        isResolved = true
        timeoutTask?.cancel()
        timeoutTask = nil
        if let c = continuation {
            continuation = nil
            c.resume(with: result)
        } else {
            early = result
        }
        return true
    }

    /// Waits for the result. When `timeout` passes first, `onTimeout` supplies the result.
    func wait(timeout: TimeInterval,
              onTimeout: @escaping @MainActor () -> Result<T, Error> = { .failure(SteamError.timeout) }) async throws -> T {
        if let early { return try early.get() }
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (c: CheckedContinuation<T, Error>) in
                if let early {
                    c.resume(with: early)
                    return
                }
                continuation = c
                timeoutTask = Task { [weak self] in
                    try? await Task.sleep(nanoseconds: UInt64(max(timeout, 0) * 1_000_000_000))
                    guard !Task.isCancelled, let self else { return }
                    self.resolve(onTimeout())
                }
            }
        } onCancel: {
            Task { @MainActor [weak self] in self?.resolve(.failure(CancellationError())) }
        }
    }
}

// MARK: - Server directory (§C1)

@MainActor
final class SteamServerDirectory {
    private struct Health {
        var failures = 0
        var lastTried = Date.distantPast
    }

    private static let defaultsKey = "SteamCMServerList"
    private static let directoryURL =
        "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets"

    private var servers: [String] = []
    private var fetchedAt: Date?
    private var health: [String: Health] = [:]
    private let urlSession: URLSession

    init(urlSession: URLSession) {
        self.urlSession = urlSession
    }

    /// "host:port" with a plain DNS name or IPv4 address and a valid port.
    nonisolated static func isValidEndpoint(_ endpoint: String) -> Bool {
        let parts = endpoint.split(separator: ":", omittingEmptySubsequences: false)
        guard parts.count == 2, let port = Int(parts[1]), (1...65535).contains(port) else { return false }
        let host = parts[0]
        guard !host.isEmpty, host.utf8.count <= 253 else { return false }
        return host.utf8.allSatisfy { c in
            (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) || c == 0x2E || c == 0x2D
        }
    }

    nonisolated static func parseServerList(_ data: Data) -> [String] {
        guard data.count <= 1 << 20,
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let response = object["response"] as? [String: Any],
              let list = response["serverlist"] as? [[String: Any]] else { return [] }
        var out: [String] = []
        for entry in list.prefix(500) {
            if let endpoint = entry["endpoint"] as? String, isValidEndpoint(endpoint), !out.contains(endpoint) {
                out.append(endpoint)
            }
        }
        return out
    }

    private func refreshIfNeeded() async {
        if let fetchedAt, Date().timeIntervalSince(fetchedAt) < 3600, !servers.isEmpty { return }
        var fresh: [String] = []
        if let url = URL(string: Self.directoryURL) {
            var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalAndRemoteCacheData, timeoutInterval: 15)
            request.httpMethod = "GET"
            if let reply = try? await urlSession.data(for: request),
               (reply.1 as? HTTPURLResponse)?.statusCode == 200 {
                fresh = Self.parseServerList(reply.0)
            }
        }
        if !fresh.isEmpty {
            servers = fresh
            fetchedAt = Date()
            UserDefaults.standard.set(fresh, forKey: Self.defaultsKey)
            SteamLog.event("[steam-cm] server list count=\(fresh.count)")
        } else if servers.isEmpty {
            let cached = (UserDefaults.standard.stringArray(forKey: Self.defaultsKey) ?? []).filter(Self.isValidEndpoint)
            servers = cached
            SteamLog.event("[steam-cm] server directory failed, cached count=\(cached.count)")
        }
    }

    /// The least recently tried healthy server (fewer than 3 failures; all reset when none is).
    func pick() async throws -> String {
        await refreshIfNeeded()
        guard !servers.isEmpty else { throw SteamError.noServers }
        var healthy = servers.filter { (health[$0]?.failures ?? 0) < 3 }
        if healthy.isEmpty {
            for key in health.keys { health[key]?.failures = 0 }
            healthy = servers
        }
        var best = healthy[0]
        for s in healthy where (health[s]?.lastTried ?? .distantPast) < (health[best]?.lastTried ?? .distantPast) {
            best = s
        }
        health[best, default: Health()].lastTried = Date()
        return best
    }

    func markSuccess(_ server: String) {
        health[server, default: Health()].failures = 0
    }

    func markFailure(_ server: String) {
        health[server, default: Health()].failures += 1
    }
}

// MARK: - Session

@MainActor
final class SteamSession: SteamConnection {
    private enum State { case idle, connecting, loggedOn }

    private enum LogonOutcome {
        case result(Int, SteamPacket)
        case loggedOff(Int)
    }

    @MainActor
    private final class PendingJob {
        let waiter = SteamWaiter<[SteamPacket]>()
        let multipart: Bool
        var parts: [SteamPacket] = []
        init(multipart: Bool) { self.multipart = multipart }
    }

    static let protocolVersion: UInt32 = 65580
    static let clientPackageVersion: UInt32 = 1771
    private static let cellDefaultsKey = "SteamCMCellID"

    private(set) var steamID: UInt64 = 0
    private(set) var cellID: UInt32
    private var sessionID: Int32 = 0
    private var heartbeatSeconds = 30
    private var state = State.idle

    private let urlSession: URLSession
    private let directory: SteamServerDirectory
    private var socket: URLSessionWebSocketTask?
    private var generation = 0

    private var connectTask: Task<Void, Never>?
    private var connectWaiters: [SteamWaiter<Void>] = []
    private var logonWaiter: SteamWaiter<LogonOutcome>?
    private var closeWaiter: SteamWaiter<Void>?

    private var jobs: [UInt64: PendingJob] = [:]
    private var lastJobID: UInt64 = 0

    private var licenses: [UInt32]?
    private var licenseWaiters: [SteamWaiter<[UInt32]>] = []

    private var heartbeatTask: Task<Void, Never>?
    private var idleTask: Task<Void, Never>?
    private var reconnectTask: Task<Void, Never>?
    private var lastActivity = Date.distantPast
    private var suppressReconnect = false

    init() {
        let config = URLSessionConfiguration.ephemeral
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        urlSession = URLSession(configuration: config)
        directory = SteamServerDirectory(urlSession: urlSession)
        cellID = UInt32(clamping: UserDefaults.standard.integer(forKey: Self.cellDefaultsKey))
    }

    // MARK: Public API

    /// Log off (best effort) and disconnect. The stored sign-in stays.
    func logoff() {
        suppressReconnect = true
        reconnectTask?.cancel()
        reconnectTask = nil
        connectTask?.cancel()
        if state == .loggedOn, let socket {
            socket.send(.data(logOffPacket())) { _ in }
            SteamLog.event("[steam-cm] logged off")
        }
        tearDown(.disconnected, closeCode: .normalClosure)
    }

    /// Send a log-off, wait for the socket to close, and stay disconnected until the next
    /// request needs Steam. Used before Valve's own client signs in with the same account.
    func disconnectGracefully() async {
        suppressReconnect = true
        reconnectTask?.cancel()
        reconnectTask = nil
        if let running = connectTask {
            running.cancel()
            await running.value
        }
        if state == .loggedOn, let socket {
            do {
                try await socket.send(.data(logOffPacket()))
                SteamLog.event("[steam-cm] logged off before handing over")
            } catch {
                SteamLog.event("[steam-cm] log-off send failed")
            }
            let closed = SteamWaiter<Void>()
            closeWaiter = closed
            socket.cancel(with: .normalClosure, reason: nil)
            _ = try? await closed.wait(timeout: 5)
        }
        tearDown(.disconnected, closeCode: .normalClosure)
    }

    // MARK: SteamConnection

    func licensedPackageIDs() async throws -> [UInt32] {
        try await ensureLoggedOn()
        touch()
        if let licenses { return licenses }
        let waiter = SteamWaiter<[UInt32]>()
        licenseWaiters.append(waiter)
        return try await waiter.wait(timeout: 15)
    }

    func request(_ emsg: UInt32, body: Data, timeout: TimeInterval = 10) async throws -> SteamPacket {
        let parts = try await perform(emsg, body: body, targetJobName: nil, multipart: false, timeout: timeout)
        guard let first = parts.first else { throw SteamError.invalidMessage("empty reply") }
        return first
    }

    func requestParts(_ emsg: UInt32, body: Data, timeout: TimeInterval = 10) async throws -> [SteamPacket] {
        try await perform(emsg, body: body, targetJobName: nil, multipart: true, timeout: timeout)
    }

    func callService(_ method: String, body: Data, timeout: TimeInterval = 10) async throws -> SteamPacket {
        let parts = try await perform(SteamEMsg.serviceMethodCallFromClient, body: body,
                                      targetJobName: method + "#1", multipart: false, timeout: timeout)
        guard let reply = parts.first else { throw SteamError.invalidMessage("empty reply") }
        if let e = reply.eresult, e != 1 {
            SteamLog.trace("[steam-cm] service \(method) eresult=\(e)")
        }
        return reply
    }

    // MARK: Requests (§C5)

    private func perform(_ emsg: UInt32, body: Data, targetJobName: String?, multipart: Bool,
                         timeout: TimeInterval) async throws -> [SteamPacket] {
        try await ensureLoggedOn()
        lastJobID &+= 1
        if lastJobID == .max || lastJobID == 0 { lastJobID = 1 }
        let jobID = lastJobID
        let job = PendingJob(multipart: multipart)
        jobs[jobID] = job
        touch()
        defer {
            jobs[jobID] = nil
            touch()
        }
        do {
            try await send(emsg, body: body, jobSource: jobID, targetJobName: targetJobName)
        } catch {
            job.waiter.resolve(.failure(SteamError.disconnected))
        }
        return try await job.waiter.wait(timeout: timeout) { [weak job] in
            if let job, job.multipart, !job.parts.isEmpty { return .success(job.parts) }
            return .failure(SteamError.timeout)
        }
    }

    private func send(_ emsg: UInt32, body: Data, jobSource: UInt64? = nil, targetJobName: String? = nil,
                      beforeLogon: Bool = false) async throws {
        guard let socket else { throw SteamError.disconnected }
        let packet = SteamPacketCodec.encode(emsg: emsg,
                                             steamID: beforeLogon ? SteamPacketCodec.anonymousSteamID : steamID,
                                             sessionID: beforeLogon ? 0 : sessionID,
                                             jobSource: jobSource, targetJobName: targetJobName, body: body)
        do {
            try await socket.send(.data(packet))
        } catch {
            throw SteamError.disconnected
        }
    }

    private func logOffPacket() -> Data {
        SteamPacketCodec.encode(emsg: SteamEMsg.clientLogOff, steamID: steamID, sessionID: sessionID,
                                jobSource: nil, targetJobName: nil, body: Data())
    }

    // MARK: Connecting (§C2, §C4)

    /// Logs on if needed. A request that finds a connect already running waits for it (up to 20 s).
    private func ensureLoggedOn() async throws {
        suppressReconnect = false
        try await connectIfNeeded()
    }

    private func connectIfNeeded() async throws {
        if state == .loggedOn, socket != nil { return }
        let joining = connectTask != nil
        let waiter = SteamWaiter<Void>()
        connectWaiters.append(waiter)
        if !joining { startConnect() }
        // Two attempts of (10 s ping + 20 s logon) plus the directory: 90 s bounds the starter.
        try await waiter.wait(timeout: joining ? 20 : 90)
    }

    private func startConnect() {
        connectTask = Task { [weak self] in
            guard let self else { return }
            let result: Result<Void, Error>
            do {
                try await self.connectAndLogOn()
                result = .success(())
            } catch {
                if self.state != .loggedOn { self.tearDown(.disconnected, closeCode: .goingAway) }
                result = .failure(error)
            }
            self.connectTask = nil
            let waiters = self.connectWaiters
            self.connectWaiters = []
            for w in waiters { w.resolve(result) }
        }
    }

    private func connectAndLogOn() async throws {
        guard let credentials = SteamSignIn.credentialsForDock() else { throw SteamError.notSignedIn }
        var lastError: Error = SteamError.connectionFailed
        for attempt in 1...2 {
            try Task.checkCancellation()
            let server = try await directory.pick()
            try Task.checkCancellation()
            SteamLog.event("[steam-cm] connecting attempt=\(attempt)")
            do {
                try await openSocket(server)
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                directory.markFailure(server)
                tearDown(.connectionFailed, closeCode: .goingAway)
                lastError = SteamError.connectionFailed
                continue
            }

            let outcome: LogonOutcome
            do {
                outcome = try await logOn(credentials)
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                // Timeout, send error or a dropped socket: try another server.
                SteamLog.event("[steam-cm] logon attempt=\(attempt) failed: \(SteamLog.describe(error))")
                directory.markFailure(server)
                tearDown(.disconnected, closeCode: .goingAway)
                lastError = error
                continue
            }

            switch outcome {
            case .result(1, let packet):
                directory.markSuccess(server)
                adoptLogon(packet)
                return
            case .result(48, _):
                SteamLog.event("[steam-cm] logon result=48, trying another server")
                directory.markFailure(server)
                tearDown(.disconnected, closeCode: .goingAway)
                lastError = SteamError.logonDenied(48)
            case .result(let code, _):
                SteamLog.event("[steam-cm] logon denied code=\(code)")
                tearDown(.logonDenied(code), closeCode: .normalClosure)
                throw SteamError.logonDenied(code)
            case .loggedOff(let code):
                SteamLog.event("[steam-cm] logged off during logon code=\(code)")
                tearDown(.logonDenied(code), closeCode: .normalClosure)
                throw SteamError.logonDenied(code)
            }
        }
        throw lastError
    }

    private func openSocket(_ server: String) async throws {
        tearDown(.disconnected, closeCode: .goingAway)
        generation += 1
        let gen = generation
        state = .connecting
        licenses = nil                       // forgotten at the start of each connection (§C7)
        guard let url = URL(string: "wss://\(server)/cmsocket/") else { throw SteamError.connectionFailed }
        // The subprotocol must go through `protocols:`; a hand-set header is ignored (§C2).
        let task = urlSession.webSocketTask(with: url, protocols: ["steamdataport"])
        task.maximumMessageSize = 64 << 20
        socket = task
        task.resume()
        startReceiving(task, generation: gen)

        let pong = SteamWaiter<Void>()
        task.sendPing { error in
            let result: Result<Void, Error> = error.map { .failure($0) } ?? .success(())
            Task { @MainActor in pong.resolve(result) }
        }
        try await pong.wait(timeout: 10)
        guard gen == generation else { throw SteamError.disconnected }
    }

    private func logOn(_ credentials: (accountName: String, refreshToken: String)) async throws -> LogonOutcome {
        let waiter = SteamWaiter<LogonOutcome>()
        logonWaiter = waiter
        defer { if logonWaiter === waiter { logonWaiter = nil } }

        var hello = ProtoWriter()
        hello.uint32(1, Self.protocolVersion)

        var ip = ProtoWriter()
        ip.fixed32(1, Self.firstIPv4HostOrder() ^ 0xBAAD_F00D)

        var logon = ProtoWriter()
        logon.uint32(1, Self.protocolVersion)
        if cellID != 0 { logon.uint32(3, cellID) }
        logon.uint32(5, Self.clientPackageVersion)
        logon.string(6, "english")
        logon.uint32(7, UInt32(bitPattern: -102))
        logon.bool(8, true)
        logon.message(11, ip)
        logon.bytes(30, Data())
        logon.string(50, credentials.accountName)
        logon.string(96, UIDevice.current.name)
        logon.bool(102, true)
        logon.string(108, credentials.refreshToken)   // the refresh token; an access token fails here

        do {
            try await send(SteamEMsg.clientHello, body: hello.data, beforeLogon: true)
            try await send(SteamEMsg.clientLogon, body: logon.data, beforeLogon: true)
        } catch {
            waiter.resolve(.failure(SteamError.connectionFailed))
        }
        return try await waiter.wait(timeout: 20)
    }

    private func adoptLogon(_ packet: SteamPacket) {
        steamID = packet.steamID
        sessionID = packet.sessionID
        var heartbeat = 30
        if packet.isProtobuf, let body = try? ProtoFields(packet.body) {
            if let h = body.int32(3), h > 0 { heartbeat = Int(h) }
            if let cell = body.uint32(7), cell != 0 {
                cellID = cell
                UserDefaults.standard.set(Int(cell), forKey: Self.cellDefaultsKey)
            }
        }
        heartbeatSeconds = min(max(heartbeat, 5), 300)
        state = .loggedOn
        SteamLog.event("[steam-cm] logged on heartbeat=\(heartbeatSeconds)s")
        startHeartbeat()
        touch()
    }

    // MARK: Receiving (§C2, §C5)

    private func startReceiving(_ task: URLSessionWebSocketTask, generation gen: Int) {
        Task { [weak self] in
            while true {
                let message: URLSessionWebSocketTask.Message
                do {
                    message = try await task.receive()
                } catch {
                    self?.socketEnded(error, generation: gen)
                    return
                }
                guard let self, gen == self.generation else { return }
                if case .data(let data) = message {
                    self.received(data, depth: 0)
                }
                // A text frame is ignored.
            }
        }
    }

    private func received(_ data: Data, depth: Int) {
        let packet: SteamPacket
        do {
            packet = try SteamPacketCodec.decode(data)
        } catch {
            SteamLog.trace("[steam-cm] unreadable message bytes=\(data.count)")
            return
        }
        handle(packet, depth: depth)
    }

    private func handle(_ packet: SteamPacket, depth: Int) {
        switch packet.emsg {
        case SteamEMsg.multi:
            guard depth < 2 else { return }
            do {
                for inner in try SteamPacketCodec.unpackMulti(packet.body) { received(inner, depth: depth + 1) }
            } catch {
                SteamLog.trace("[steam-cm] unreadable multi bytes=\(packet.body.count)")
            }
            return

        case SteamEMsg.clientLogOnResponse:
            let code: Int
            if packet.isProtobuf {
                code = Int((try? ProtoFields(packet.body))?.int32(1) ?? 2)
            } else {
                code = Int(Int32(bitPattern: packet.body.steamU32(at: 0) ?? 2))
            }
            logonWaiter?.resolve(.success(.result(code, packet)))
            return

        case SteamEMsg.clientLoggedOff:
            let code = packet.isProtobuf ? Int((try? ProtoFields(packet.body))?.int32(1) ?? 2)
                                         : Int(Int32(bitPattern: packet.body.steamU32(at: 0) ?? 2))
            if let waiter = logonWaiter, !waiter.isResolved {
                waiter.resolve(.success(.loggedOff(code)))
            } else {
                // Steam ended the session (for example, the account logged on elsewhere).
                // Reconnecting could push that other session off, so stay disconnected.
                SteamLog.event("[steam-cm] logged off by Steam code=\(code)")
                suppressReconnect = true
                tearDown(.disconnected, closeCode: .normalClosure)
            }
            return

        case SteamEMsg.clientLicenseList:
            if let list = try? ProtoFields(packet.body), let entries = try? list.messages(2) {
                let ids = entries.prefix(200_000).compactMap { $0.uint32(1) }
                licenses = ids
                SteamLog.event("[steam-cm] licence list packages=\(ids.count)")
                let waiters = licenseWaiters
                licenseWaiters = []
                for w in waiters { w.resolve(.success(ids)) }
            }
            return

        default:
            break
        }

        guard packet.jobTarget != .max, let job = jobs[packet.jobTarget] else { return }
        if job.multipart {
            job.parts.append(packet)
            if !SteamPacketCodec.responsePending(packet.body) {
                job.waiter.resolve(.success(job.parts))
            }
        } else {
            job.waiter.resolve(.success([packet]))
        }
    }

    private func socketEnded(_ error: Error, generation gen: Int) {
        guard gen == generation else { return }
        let ns = error as NSError
        if ns.domain == NSPOSIXErrorDomain, ns.code == 57 {
            SteamLog.event("[steam-cm] connection closed")
        } else {
            SteamLog.event("[steam-cm] connection ended code=\(ns.code)")
        }
        let wasLoggedOn = state == .loggedOn
        tearDown(.disconnected, closeCode: .goingAway)
        if wasLoggedOn, !suppressReconnect { scheduleReconnect() }
    }

    // MARK: Keeping it alive, and letting go (§C6)

    private func startHeartbeat() {
        heartbeatTask?.cancel()
        let interval = UInt64(heartbeatSeconds) * 1_000_000_000
        heartbeatTask = Task { [weak self] in
            while true {
                try? await Task.sleep(nanoseconds: interval)
                guard let self, !Task.isCancelled, self.state == .loggedOn else { return }
                try? await self.send(SteamEMsg.clientHeartBeat, body: Data())
            }
        }
    }

    /// Records activity and re-arms the idle log-off.
    private func touch() {
        lastActivity = Date()
        idleTask?.cancel()
        idleTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 60 * 1_000_000_000)
            guard let self, !Task.isCancelled else { return }
            self.idleCheck()
        }
    }

    private func idleCheck() {
        guard state == .loggedOn else { return }
        if !jobs.isEmpty || !licenseWaiters.isEmpty || Date().timeIntervalSince(lastActivity) < 59 {
            touch()
            return
        }
        SteamLog.event("[steam-cm] idle for 60 s, logging off")
        logoff()
    }

    private func scheduleReconnect() {
        reconnectTask?.cancel()
        reconnectTask = Task { [weak self] in
            let delays: [UInt64] = [1, 2, 4, 8, 16]
            for (index, delay) in delays.enumerated() {
                try? await Task.sleep(nanoseconds: delay * 1_000_000_000)
                guard let self, !Task.isCancelled, !self.suppressReconnect else { return }
                if self.state == .loggedOn { return }
                SteamLog.event("[steam-cm] reconnecting attempt=\(index + 1)")
                do {
                    try await self.connectIfNeeded()
                    self.touch()
                    return
                } catch {
                    if Task.isCancelled { return }
                }
            }
            SteamLog.event("[steam-cm] reconnect gave up")
        }
    }

    /// Ends the connection: the socket, timers, the logon wait and every pending job. Safe to
    /// call more than once; stale socket callbacks are ignored by the generation check.
    private func tearDown(_ error: SteamError, closeCode: URLSessionWebSocketTask.CloseCode) {
        generation += 1
        socket?.cancel(with: closeCode, reason: nil)
        socket = nil
        heartbeatTask?.cancel()
        heartbeatTask = nil
        idleTask?.cancel()
        idleTask = nil
        state = .idle
        sessionID = 0
        logonWaiter?.resolve(.failure(error))
        closeWaiter?.resolve(.success(()))
        closeWaiter = nil
        let pending = jobs
        jobs.removeAll()
        for job in pending.values { job.waiter.resolve(.failure(error)) }
        let waiting = licenseWaiters
        licenseWaiters = []
        for w in waiting { w.resolve(.failure(error)) }
    }

    // MARK: Device address (§C4)

    /// The first non-loopback IPv4 address, in host byte order; 0 if none.
    nonisolated static func firstIPv4HostOrder() -> UInt32 {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return 0 }
        defer { freeifaddrs(list) }
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard let addr = entry.pointee.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else { continue }
            if (entry.pointee.ifa_flags & UInt32(IFF_LOOPBACK)) != 0 { continue }
            let value = addr.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr.s_addr }
            let host = UInt32(bigEndian: value)
            if host == 0 || (host >> 24) == 127 { continue }
            return host
        }
        return 0
    }
}
