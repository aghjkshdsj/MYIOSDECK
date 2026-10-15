// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation
import Darwin
import UIKit

private final class BSteamSocketDelegate: NSObject, URLSessionWebSocketDelegate, @unchecked Sendable {
    let closed = BSteamPromise<Void>()
    var ended: (@Sendable () -> Void)?
    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        closed.resolve(.success(())); ended?()
    }
    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        closed.resolve(.success(())); ended?()
    }
}

@MainActor final class SteamSession: BSteamSessionProtocol {
    private struct Server { var endpoint: String; var failures = 0; var tried = 0.0 }
    private struct Job { var promise: BSteamPromise<[BSteamMessage]>; var multipart: Bool; var parts: [BSteamMessage] = []; var bytes = 0 }
    private struct Received: Sendable { var message: BSteamMessage; var body: BSteamProto?; var licenses: [UInt32]? }
    private struct Closing { var socket: URLSessionWebSocketTask?; var session: URLSession?; var delegate: BSteamSocketDelegate?; var logoff: Data? }
    private(set) var bSteamID: UInt64 = 0
    private(set) var bCellID: UInt32 = 0
    private var sessionID: UInt32 = 0
    private var loggedOn = false
    private var intentional = false
    private var socket: URLSessionWebSocketTask?
    private var network: URLSession?
    private var delegate: BSteamSocketDelegate?
    private var receiveTask: Task<Void, Never>?
    private var heartbeatTask: Task<Void, Never>?
    private var idleTask: Task<Void, Never>?
    private var retryTask: Task<Void, Never>?
    private var connectTask: Task<Void, Error>?
    private var closingTask: Task<Void, Never>?
    private var serial: UInt64 = 0
    private var connectionTicket: UInt64 = 0
    private var jobs: [UInt64: Job] = [:]
    private var nextJob: UInt64 = 1
    private var logon = BSteamPromise<BSteamMessage>()
    private var licensePromise = BSteamPromise<[UInt32]>()
    private var licenses: [UInt32]?
    private var servers: [Server] = []
    private var directoryDate = Date.distantPast
    private let directorySession: URLSession
    init() {
        let c = URLSessionConfiguration.ephemeral; c.urlCache = nil; c.timeoutIntervalForRequest = 20
        directorySession = URLSession(configuration: c)
        let defaults = UserDefaults.standard
        let cached = defaults.stringArray(forKey: "trackB.steam.cm.servers") ?? []
        servers = cached.prefix(256).filter(Self.validEndpoint).map { Server(endpoint: $0) }
        if let date = defaults.object(forKey: "trackB.steam.cm.date") as? Date { directoryDate = date }
    }
    nonisolated private static func validEndpoint(_ value: String) -> Bool {
        let pieces = value.split(separator: ":", omittingEmptySubsequences: false)
        guard value.utf8.count <= 320, pieces.count == 2, let port = Int(pieces[1]), (1...65535).contains(port), !pieces[0].isEmpty else { return false }
        return pieces[0].utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 46 || $0 == 45 }
    }
    private func refreshDirectory() async throws {
        if !servers.isEmpty && Date().timeIntervalSince(directoryDate) < 3600 { return }
        do {
            guard let url = URL(string: "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets") else { throw SteamError.noServers }
            let (data, response) = try await BSteamHTTP.data(URLRequest(url: url, timeoutInterval: 20), session: directorySession, limit: 1024 * 1024)
            guard response.statusCode == 200 else { throw SteamError.noServers }
            let endpoints = try await Task.detached(priority: .utility) { () throws -> [String] in
                guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any], let response = json["response"] as? [String: Any],
                      let list = response["serverlist"] as? [[String: Any]], list.count <= 4096 else { throw SteamError.noServers }
                return Array(list.prefix(256).compactMap { $0["endpoint"] as? String }.filter(Self.validEndpoint))
            }.value
            guard !endpoints.isEmpty else { throw SteamError.noServers }
            let old = Dictionary(servers.map { ($0.endpoint, $0) }, uniquingKeysWith: { a, _ in a })
            servers = Array(Set(endpoints)).sorted().map { old[$0] ?? Server(endpoint: $0) }; directoryDate = Date()
            UserDefaults.standard.set(endpoints, forKey: "trackB.steam.cm.servers"); UserDefaults.standard.set(directoryDate, forKey: "trackB.steam.cm.date")
        } catch is CancellationError { throw CancellationError() } catch { if servers.isEmpty { throw SteamError.noServers } }
    }
    private func pickServer() throws -> String {
        guard !servers.isEmpty else { throw SteamError.noServers }
        if servers.allSatisfy({ $0.failures >= 3 }) { for i in servers.indices { servers[i].failures = 0 } }
        guard let index = servers.indices.filter({ servers[$0].failures < 3 }).min(by: { servers[$0].tried < servers[$1].tried }) else { throw SteamError.noServers }
        servers[index].tried = ProcessInfo.processInfo.systemUptime; return servers[index].endpoint
    }
    private func serverResult(_ endpoint: String, success: Bool) {
        if let i = servers.firstIndex(where: { $0.endpoint == endpoint }) { servers[i].failures = success ? 0 : servers[i].failures + 1 }
    }
    func bConnect() async throws {
        guard !TrackBSessionGate.reserved else { throw SteamError.disconnected }
        if loggedOn { return }
        if connectTask == nil {
            connectionTicket &+= 1; let ticket = connectionTicket
            connectTask = Task {
                defer { if connectionTicket == ticket { connectTask = nil } }
                if let close = closingTask { await close.value }
                try Task.checkCancellation(); intentional = false
                try await connectAttempts()
            }
        }
        guard let task = connectTask else { throw SteamError.connectionFailed }
        try await bSteamTimeout(20) { try await task.value }
    }
    private func connectAttempts() async throws {
        try await refreshDirectory()
        var last: Error = SteamError.connectionFailed
        for attempt in 0..<2 {
            try Task.checkCancellation()
            guard let credentials = SteamSignIn.credentialsForDock() else { throw SteamError.logonDenied(5) }
            let endpoint = try pickServer()
            do {
                try await open(endpoint, credentials: credentials)
                serverResult(endpoint, success: true); return
            } catch is CancellationError { throw CancellationError() }
            catch {
                let failure = (error as? SteamError) ?? SteamError.connectionFailed
                last = failure; serverResult(endpoint, success: false)
                let close = takeConnection(error: failure); await closeCaptured(close, graceful: false)
                if case .logonDenied(let code) = failure, code != 48 { throw failure }
                if attempt == 1 { throw failure }
            }
        }
        throw last
    }
    private func header(job: UInt64? = nil, name: String? = nil) -> BSteamProto {
        var h = BSteamProto(); h.fixed(1, loggedOn ? bSteamID : 76561197960265728); h.uint(2, UInt64(loggedOn ? sessionID : 0))
        if let job = job { h.fixed(10, job) }; if let name = name { h.text(12, name) }; return h
    }
    private func open(_ endpoint: String, credentials: (accountName: String, refreshToken: String)) async throws {
        guard let url = URL(string: "wss://\(endpoint)/cmsocket/") else { throw SteamError.noServers }
        licenses = nil; licensePromise = BSteamPromise(); logon = BSteamPromise(); loggedOn = false
        serial &+= 1; let current = serial
        let callbacks = BSteamSocketDelegate()
        callbacks.ended = { [weak self] in Task { @MainActor in self?.socketEnded(current) } }
        let c = URLSessionConfiguration.ephemeral; c.urlCache = nil; c.timeoutIntervalForRequest = 20
        let session = URLSession(configuration: c, delegate: callbacks, delegateQueue: nil)
        let task = session.webSocketTask(with: url, protocols: ["steamdataport"]); task.maximumMessageSize = 256 * 1024 * 1024
        network = session; delegate = callbacks; socket = task
        task.resume()
        receiveTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    let frame = try await task.receive()
                    guard let self = self, self.serial == current else { return }
                    guard case .data(let data) = frame else { continue }
                    let messages = try await Task.detached(priority: .userInitiated) { () throws -> [Received] in
                        try BSteamWire.messages(data).map { message in
                            let parsed: BSteamProto?
                            if [UInt32(5515),780,8904].contains(message.id) { parsed = try BSteamProto(message.body) } else { parsed = nil }
                            var list: [UInt32]?
                            if message.id == 780, let body = parsed {
                                let entries = try body.messages(2); guard entries.count <= 100_000 else { throw SteamError.invalidMessage }
                                list = entries.compactMap { value in guard let n = value.number(1), let id = UInt32(exactly: n) else { return nil }; return id }
                            }
                            return Received(message: message, body: parsed, licenses: list)
                        }
                    }.value
                    guard self.serial == current else { return }
                    for message in messages { self.handle(message) }
                } catch {
                    self?.socketEnded(current); return
                }
            }
        }
        let ping = BSteamPromise<Void>()
        task.sendPing { error in ping.resolve(error.map { .failure($0) } ?? .success(())) }
        try await bSteamTimeout(20) { try await ping.value() }
        SteamLog.trace("[steam-cm] websocket ping succeeded")
        var hello = BSteamProto(); hello.uint(1, 65580)
        try await task.send(.data(BSteamMessage(id: 4006, header: header(), body: hello.encoded()).encoded()))
        var body = BSteamProto()
        body.uint(1,65580); body.uint(3,UInt64(bCellID)); body.uint(5,1771); body.text(6,"english")
        body.uint(7,UInt64(UInt32(bitPattern: -102))); body.uint(8,1); body.bytes(30,Data())
        var ip = BSteamProto(); ip.fields[1] = [.fixed32(Self.privateIPv4() ^ 0xbaadf00d)]; body.bytes(11,ip.encoded())
        body.text(50,credentials.accountName); body.text(96,UIDevice.current.name); body.uint(102,1); body.text(108,credentials.refreshToken)
        let promise = logon
        let sending = Task {
            do { try await task.send(.data(BSteamMessage(id:5514,header:header(),body:body.encoded()).encoded())) }
            catch { promise.resolve(.failure(SteamError.connectionFailed)) }
        }
        defer { sending.cancel() }
        let response = try await bSteamTimeout(20) { try await promise.value() }
        try Task.checkCancellation()
        guard serial == current, socket === task else { throw SteamError.disconnected }
        let parsed: BSteamProto?
        if response.id == 751 { parsed = nil } else { parsed = try await Task.detached { try BSteamProto(response.body) }.value }
        let result: Int
        if response.id == 751 { result = Int(try response.body.bLE(0,4)) } else { result = Int(clamping:parsed?.number(1) ?? 2) }
        guard result == 1 else { throw SteamError.logonDenied(result) }
        bSteamID = response.header.number(1) ?? 0; sessionID = UInt32(truncatingIfNeeded: response.header.number(2) ?? 0)
        bCellID = UInt32(truncatingIfNeeded: parsed?.number(7) ?? UInt64(bCellID)); loggedOn = true
        let raw = parsed?.number(3) ?? 30, heartbeat = raw > 0 && raw <= 3600 ? Double(raw) : 30
        heartbeatTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await Task.sleep(nanoseconds:UInt64(heartbeat*1e9)); guard let self = self, self.loggedOn, let socket = self.socket else { return }
                    try await socket.send(.data(BSteamMessage(id:703,header:self.header(),body:Data()).encoded()))
                } catch { self?.socketEnded(current); return }
            }
        }
        armIdle(); SteamLog.event("[steam-cm] logged on")
    }
    private static func privateIPv4() -> UInt32 {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0 else { return 0 }; defer { freeifaddrs(head) }
        var current = head
        while let p = current {
            let value = p.pointee
            if let address = value.ifa_addr, address.pointee.sa_family == UInt8(AF_INET), value.ifa_flags & UInt32(IFF_LOOPBACK) == 0 {
                return address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { UInt32(bigEndian: $0.pointee.sin_addr.s_addr) }
            }
            current = value.ifa_next
        }
        return 0
    }
    private func handle(_ received: Received) {
        let message = received.message
        if message.id == 5517 { socketEnded(serial); return }
        if message.id == 5515 || message.id == 751 { logon.resolve(.success(message)); return }
        if message.id == 780 {
            if let result = received.body?.number(1), result != 1 { licensePromise.resolve(.failure(SteamError.invalidMessage)); return }
            licenses = received.licenses ?? []; licensePromise.resolve(.success(licenses ?? [])); return
        }
        guard let id = message.header.number(11), id != UInt64.max, var job = jobs[id] else { return }
        guard message.body.count <= 256*1024*1024-job.bytes else { finish(id,.failure(SteamError.invalidMessage)); return }
        job.bytes += message.body.count
        job.parts.append(message)
        guard job.parts.count <= 4096 else { finish(id, .failure(SteamError.invalidMessage)); return }
        if job.multipart && message.id == 8904 && received.body?.number(6) == 1 { jobs[id] = job }
        else { finish(id, .success(job.parts)) }
    }
    private func finish(_ id: UInt64, _ result: Result<[BSteamMessage], Error>) { jobs.removeValue(forKey:id)?.promise.resolve(result) }
    func bRequest(_ message: UInt32, body: Data, name: String?, timeout: Double, multipart: Bool) async throws -> [BSteamMessage] {
        try await bConnect(); try Task.checkCancellation()
        guard let socket = socket, loggedOn, jobs.count < 1024, body.count <= 16 * 1024 * 1024 else { throw SteamError.disconnected }
        var id = nextJob
        while jobs[id] != nil || id == 0 || id == UInt64.max { id &+= 1 }
        nextJob = id &+ 1
        let promise = BSteamPromise<[BSteamMessage]>()
        jobs[id] = Job(promise:promise,multipart:multipart); armIdle()
        let data = BSteamMessage(id:message,header:header(job:id,name:name),body:body).encoded()
        let sending = Task { do { try await socket.send(.data(data)) } catch { finish(id,.failure(SteamError.disconnected)) } }
        defer { sending.cancel(); jobs.removeValue(forKey:id) }
        do { return try await bSteamTimeout(timeout) { try await promise.value() } }
        catch SteamError.timeout {
            if multipart, let parts = jobs[id]?.parts, !parts.isEmpty { return parts }; throw SteamError.timeout
        }
    }
    func bLicenses() async throws -> [UInt32] {
        try await bConnect(); if let licenses = licenses { return licenses }
        let promise = licensePromise; return try await bSteamTimeout(15) { try await promise.value() }
    }
    private func armIdle() {
        idleTask?.cancel()
        idleTask = Task { [weak self] in
            do { try await Task.sleep(nanoseconds:60_000_000_000); self?.logoff() } catch {}
        }
    }
    private func socketEnded(_ current: UInt64) {
        guard current == serial, socket != nil else { return }
        let retry = loggedOn && !intentional
        let old = takeConnection(error:SteamError.disconnected)
        Task { await closeCaptured(old,graceful:false) }
        if retry && retryTask == nil {
            retryTask = Task {
                defer { retryTask = nil }
                for delay in [1,2,4,8,16] {
                    do {
                        try await Task.sleep(nanoseconds:UInt64(delay)*1_000_000_000)
                        guard !intentional else { return }; try await bConnect(); return
                    } catch is CancellationError { return } catch {}
                }
                SteamLog.event("[steam-cm] reconnect attempts exhausted")
            }
        }
    }
    private func takeConnection(error: Error) -> Closing {
        let data = loggedOn ? BSteamMessage(id:5516,header:header(),body:Data()).encoded() : nil
        let old = Closing(socket:socket,session:network,delegate:delegate,logoff:data)
        serial &+= 1; loggedOn = false; socket = nil; network = nil; delegate = nil
        receiveTask?.cancel(); receiveTask = nil; heartbeatTask?.cancel(); heartbeatTask = nil; idleTask?.cancel(); idleTask = nil
        logon.resolve(.failure(error)); licensePromise.resolve(.failure(error))
        let pending = jobs.values.map { $0.promise }; jobs.removeAll(); for promise in pending { promise.resolve(.failure(error)) }
        return old
    }
    private func closeCaptured(_ old: Closing, graceful: Bool) async {
        if graceful, let data = old.logoff, let socket = old.socket { _ = try? await bSteamTimeout(5) { try await socket.send(.data(data)) } }
        old.socket?.cancel(with:graceful ? .normalClosure : .goingAway,reason:nil)
        if let delegate = old.delegate { _ = try? await bSteamTimeout(5) { try await delegate.closed.value() } }
        old.session?.invalidateAndCancel()
    }
    private func beginClose() -> Task<Void,Never> {
        intentional = true; retryTask?.cancel(); retryTask = nil; connectTask?.cancel(); connectionTicket &+= 1; connectTask = nil
        let prior = closingTask, old = takeConnection(error:SteamError.disconnected)
        let task = Task { if let prior = prior { await prior.value }; await closeCaptured(old,graceful:true) }
        closingTask = task; return task
    }
    func logoff() { _ = beginClose() }
    func disconnectGracefully() async { let task = beginClose(); await task.value }
}
