// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

protocol BSteamContentTransport: Sendable { func get(_ url: URL, limit: Int) async throws -> Data }
final class BSteamURLContent: BSteamContentTransport, @unchecked Sendable {
    private let session: URLSession
    init() {
        let c = URLSessionConfiguration.ephemeral
        c.urlCache = nil; c.requestCachePolicy = .reloadIgnoringLocalCacheData
        c.timeoutIntervalForRequest = 30; c.timeoutIntervalForResource = 300; c.httpMaximumConnectionsPerHost = 8
        session = URLSession(configuration:c)
    }
    func get(_ url: URL, limit: Int) async throws -> Data {
        let (data,response) = try await BSteamHTTP.data(URLRequest(url:url,cachePolicy:.reloadIgnoringLocalCacheData,timeoutInterval:30),session:session,limit:limit)
        guard response.statusCode == 200 else { throw SteamError.chunkFailed }; return data
    }
}
enum BSteamContentServers {
    static let fallback = "https://steampipe.akamaized.net"
    static func valid(_ host: String) -> Bool {
        let lower = host.lowercased()
        guard !host.isEmpty, host.utf8.count <= 253, !lower.contains("lancache"), [".steamcontent.com",".akamaized.net",".steamstatic.com"].contains(where:{ lower.hasSuffix($0) }) else { return false }
        return host.split(separator:".",omittingEmptySubsequences:false).allSatisfy { label in
            !label.isEmpty && label.utf8.count <= 63 && !label.hasPrefix("-") && !label.hasSuffix("-") && label.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 45 }
        }
    }
    static func parse(_ data: Data, appID: UInt32) throws -> [String] {
        guard data.count <= 1024*1024, let object = try JSONSerialization.jsonObject(with:data) as? [String:Any],
              let response = object["response"] as? [String:Any], let list = response["servers"] as? [[String:Any]], list.count <= 4096 else { throw SteamError.invalidMessage }
        var hosts: [String] = []
        for entry in list.prefix(20) {
            guard let https = entry["https_support"] as? String, ["mandatory","optional"].contains(https.lowercased()), (entry["use_as_proxy"] as? Bool) != true else { continue }
            if let allowed = entry["allowed_app_ids"] as? [Any], !allowed.contains(where:{ value in
                if let n = value as? NSNumber { return n.uint64Value == UInt64(appID) && n.doubleValue == Double(appID) }
                if let s = value as? String { return UInt32(s) == appID }; return false
            }) { continue }
            let vhost = entry["vhost"] as? String ?? "", host = vhost.isEmpty ? entry["host"] as? String ?? "" : vhost
            guard valid(host) else { continue }
            let url = "https://\(host.lowercased())"
            if url != fallback && !hosts.contains(url) { hosts.append(url) }
        }
        hosts.append(fallback); return hosts
    }
    static func suffix(_ token: String) -> String {
        if token.isEmpty { return "" }; if token.hasPrefix("?") || token.hasPrefix("&") { return token }; return "?auth=\(token)"
    }
}
actor BSteamHostPool {
    private struct Health { var failures = 0; var penalty = 0; var successes = 0; var logged = false }
    private let hosts: [String]
    private var health: [String:Health] = [:]
    private var logCount = 0
    init(_ hosts: [String]) { self.hosts = Array(hosts.prefix(6)) }
    func host(rotation: Int) throws -> String {
        guard !hosts.isEmpty else { throw SteamError.noServers }
        let ordered = hosts.enumerated().sorted { lhs,rhs in
            let l = health[lhs.element]?.penalty ?? 0, r = health[rhs.element]?.penalty ?? 0
            return l == r ? lhs.offset < rhs.offset : l < r
        }.map(\.element)
        return ordered[rotation % ordered.count]
    }
    func result(_ host: String, success: Bool) {
        var value = health[host] ?? Health()
        if success { value.successes += 1; if value.successes >= 3 { value.penalty = max(0,value.penalty-1); value.successes = 0 } }
        else {
            value.failures += 1; value.penalty += 1; value.successes = 0
            if value.failures >= 3 && !value.logged && logCount < 8 {
                value.logged = true; logCount += 1; SteamLog.event("[steam-depot] content host repeatedly failed host=\(host)")
            }
        }
        health[host] = value
    }
}
private struct BSteamChunkResult: Sendable { var completed: UInt64; var transferred: UInt64 }
private enum BSteamChunkDownload {
    static func run(_ job: BSteamChunkJob, key: Data, tokens: [String:String], pool: BSteamHostPool,
                    content: any BSteamContentTransport, worker: BSteamInstallWorker) async throws -> BSteamChunkResult {
        try Task.checkCancellation()
        if try await worker.isFinished(job) { try await worker.complete(job,bytes:nil); return BSteamChunkResult(completed:job.chunk.compressed,transferred:0) }
        var last: Error = SteamError.chunkFailed
        for attempt in 0..<5 {
            try Task.checkCancellation()
            let host = try await pool.host(rotation:job.index+attempt)
            do {
                let sha = job.chunk.sha.map { String(format:"%02x",$0) }.joined()
                guard let url = URL(string:"\(host)/depot/\(job.depot)/chunk/\(sha)\(BSteamContentServers.suffix(tokens[host] ?? ""))") else { throw SteamError.chunkFailed }
                let encrypted = try await content.get(url,limit:BSteamCodec.limit)
                let bytes = try await Task.detached(priority:.utility) { () throws -> Data in
                    let plain = try BSteamCodec.decrypt(encrypted,key:key)
                    let data: Data
                    do { data = try BSteamCodec.decode(plain,expected:job.chunk.original) }
                    catch let error as SteamError {
                        if case .chunkDecode = error { throw error }
                        throw SteamError.chunkDecode(plain.prefix(8).map { String(format:"%02x",$0) }.joined())
                    }
                    if (job.chunk.crc != 0 && BSteamCodec.adler(data) != job.chunk.crc) || BSteamCodec.sha1(data) != job.chunk.sha { throw SteamError.checksumMismatch }
                    return data
                }.value
                try Task.checkCancellation(); try await worker.complete(job,bytes:bytes)
                await pool.result(host,success:true); return BSteamChunkResult(completed:job.chunk.compressed,transferred:job.chunk.compressed)
            } catch is CancellationError { throw CancellationError() }
            catch {
                last = error; await pool.result(host,success:false)
                if attempt < 4 { try await Task.sleep(nanoseconds:UInt64(Double(attempt+1)*0.4*1e9)) }
            }
        }
        if let error = last as? SteamError {
            switch error { case .checksumMismatch,.decryptionFailed,.chunkDecode,.files: throw error; default: break }
        }
        throw SteamError.chunkFailed
    }
}

@MainActor final class DepotDownloader {
    private let session: any BSteamSessionProtocol
    private let content: any BSteamContentTransport
    private var keys: [UInt32:Data] = [:]
    private var auth: [String:String] = [:]
    private var installing = false
    init(session: any BSteamSessionProtocol) { self.session = session; content = BSteamURLContent() }
    init(bSession: any BSteamSessionProtocol, bContent: any BSteamContentTransport) { session = bSession; content = bContent }
    private func key(_ depot: UInt32, appID: UInt32) async throws -> Data {
        if let key = keys[depot] { return key }
        var body = BSteamProto(); body.uint(1,UInt64(depot)); body.uint(2,UInt64(appID))
        let replies = try await session.bRequest(5438,body:body.encoded(),name:nil,timeout:15,multipart:false)
        guard let reply = replies.first else { throw SteamError.depotKeyRefused }
        let decoded = try BSteamProto(reply.body)
        guard decoded.number(1) == 1, let key = decoded.data(3), key.count == 32 else { throw SteamError.depotKeyRefused }
        keys[depot] = key; return key
    }
    private func token(depot: UInt32, appID: UInt32, host: String) async -> String {
        let cacheKey = "\(depot):\(appID):\(host)"
        if let token = auth[cacheKey] { return token }
        var body = BSteamProto(); body.uint(1,UInt64(depot)); body.text(2,URL(string:host)?.host ?? ""); body.uint(3,UInt64(appID))
        let reply = try? await session.bService("ContentServerDirectory.GetCDNAuthToken#1",body.encoded())
        let value = reply?.string(1) ?? ""
        let token = value.utf8.count <= 8192 && !value.unicodeScalars.contains(where:{ CharacterSet.controlCharacters.contains($0) }) ? value : ""
        auth[cacheKey] = token; return token
    }
    private func requestCode(depot: UInt32, gid: UInt64, appID: UInt32) async throws -> UInt64 {
        var body = BSteamProto(); body.uint(1,UInt64(appID)); body.uint(2,UInt64(depot)); body.uint(3,gid)
        return try await session.bService("ContentServerDirectory.GetManifestRequestCode#1",body.encoded()).number(1) ?? 0
    }
    func install(_ app: SteamAppInfo, steamApps: URL, ownedDepots: @escaping () async -> Set<UInt32>?, progress: @escaping (SteamDownloadProgress) -> Void) async throws -> URL {
        do { return try await bInstall(app,steamApps:steamApps,ownedDepots:ownedDepots,progress:progress) }
        catch is CancellationError { throw CancellationError() }
        catch let error as SteamError { throw error }
        catch { throw SteamError.files("Could not prepare or save the game's files.") }
    }
    private func report(_ value: SteamDownloadProgress, callback: (SteamDownloadProgress) -> Void, last: Double) async throws -> Double {
        let delay = 0.25 - (ProcessInfo.processInfo.systemUptime-last)
        if delay > 0 { try await Task.sleep(nanoseconds:UInt64(delay*1e9)) }
        try Task.checkCancellation(); let now = ProcessInfo.processInfo.systemUptime; callback(value); return now
    }
    private func bInstall(_ app: SteamAppInfo, steamApps: URL, ownedDepots: @escaping () async -> Set<UInt32>?, progress: @escaping (SteamDownloadProgress) -> Void) async throws -> URL {
        guard !installing else { throw SteamError.files("A download is already running.") }
        installing = true; defer { installing = false }
        let start = ProcessInfo.processInfo.systemUptime
        SteamLog.event("[steam-depot] install begin app=\(app.appID)")
        var state = SteamDownloadProgress(); progress(state)
        var lastProgress = ProcessInfo.processInfo.systemUptime
        try await session.bConnect(); try Task.checkCancellation()
        let selected = BSteamDepotSelection.select(app.bDepots,log:true)
        guard BSteamDepotSelection.windows(app.bOS), !selected.isEmpty else { throw SteamError.noWindowsDepot }
        let folder = app.installDir.isEmpty ? "app_\(app.appID)" : SteamInstallFiles.safeFolderName(app.installDir)
        let worker = BSteamInstallWorker(steamApps:steamApps,appID:app.appID,folder:folder)
        var manifests: [BSteamManifest] = [], tokenMaps: [UInt32:[String:String]] = [:], pools: [UInt32:BSteamHostPool] = [:]
        var checkedOwned = false, owned: Set<UInt32>?
        for depot in selected {
            try Task.checkCancellation()
            let contentApp = app.bFree ? app.appID : depot.owner ?? app.appID
            let depotKey: Data
            do { depotKey = try await key(depot.id,appID:contentApp) }
            catch SteamError.depotKeyRefused {
                if !checkedOwned { owned = await ownedDepots(); checkedOwned = true }
                if let owned = owned, !owned.isEmpty, !owned.contains(depot.id) { SteamLog.event("[steam-depot] skip unowned depot=\(depot.id)"); continue }
                throw SteamError.depotKeyRefused
            }
            guard let gid = depot.manifests["public"] else { throw SteamError.noWindowsDepot }
            let serverURL = URL(string:"https://api.steampowered.com/IContentServerDirectoryService/GetServersForSteamPipe/v1/?cell_id=\(session.bCellID)&max_servers=20")
            var hosts = [BSteamContentServers.fallback]
            if let url = serverURL {
                do {
                    let bytes = try await content.get(url,limit:1024*1024)
                    hosts = try await Task.detached(priority:.utility) { try BSteamContentServers.parse(bytes,appID:contentApp) }.value
                } catch is CancellationError { throw CancellationError() } catch {}
            }
            SteamLog.event("[steam-depot] depot=\(depot.id) content servers=\(hosts.count)")
            let code = try await requestCode(depot:depot.id,gid:gid,appID:contentApp)
            var tokens: [String:String] = [:]
            for host in hosts.prefix(6) { try Task.checkCancellation(); tokens[host] = await token(depot:depot.id,appID:contentApp,host:host) }
            tokenMaps[depot.id] = tokens; pools[depot.id] = BSteamHostPool(hosts)
            var manifest: BSteamManifest?
            for host in hosts.prefix(6) {
                try Task.checkCancellation()
                let tail = code == 0 ? "" : "/\(code)"
                guard let url = URL(string:"\(host)/depot/\(depot.id)/manifest/\(gid)/5\(tail)\(BSteamContentServers.suffix(tokens[host] ?? ""))") else { continue }
                do {
                    let bytes = try await content.get(url,limit:BSteamCodec.limit)
                    manifest = try await Task.detached(priority:.utility) { try BSteamManifest.parse(bytes,key:depotKey,depot:depot,gid:gid) }.value
                    break
                } catch is CancellationError { throw CancellationError() } catch {}
            }
            guard let manifest = manifest else { throw SteamError.manifestFailed }; manifests.append(manifest)
        }
        guard !manifests.isEmpty else { throw SteamError.depotKeyRefused }
        let prepared = try await worker.prepare(manifests)
        let grouped = await Task.detached(priority:.utility) { Dictionary(grouping:prepared.jobs,by:\.depot) }.value
        state.phase = .downloading; state.totalBytes = prepared.total
        var runDone: UInt64 = 0
        lastProgress = try await report(state,callback:progress,last:lastProgress)
        let downloadStart = ProcessInfo.processInfo.systemUptime
        for manifest in manifests {
            try Task.checkCancellation(); try await worker.beginDepot(manifest.depot.id)
            guard let depotKey = keys[manifest.depot.id], let pool = pools[manifest.depot.id] else { throw SteamError.invalidMessage }
            let jobs = grouped[manifest.depot.id] ?? [], tokens = tokenMaps[manifest.depot.id] ?? [:], transport = content
            try await withThrowingTaskGroup(of:BSteamChunkResult.self) { group in
                var cursor = 0
                while cursor < min(8,jobs.count) {
                    let job = jobs[cursor]; cursor += 1
                    group.addTask { try await BSteamChunkDownload.run(job,key:depotKey,tokens:tokens,pool:pool,content:transport,worker:worker) }
                }
                while let done = try await group.next() {
                    try Task.checkCancellation()
                    state.doneBytes += done.completed; runDone += done.transferred
                    let now = ProcessInfo.processInfo.systemUptime
                    if now-lastProgress >= 0.25 {
                        state.bytesPerSecond = Double(runDone)/max(0.001,now-downloadStart); progress(state); lastProgress = now
                    }
                    if cursor < jobs.count {
                        let job = jobs[cursor]; cursor += 1
                        group.addTask { try await BSteamChunkDownload.run(job,key:depotKey,tokens:tokens,pool:pool,content:transport,worker:worker) }
                    }
                }
            }
        }
        try Task.checkCancellation()
        state.phase = .finishing; state.doneBytes = state.totalBytes
        state.bytesPerSecond = Double(runDone)/max(0.001,ProcessInfo.processInfo.systemUptime-downloadStart)
        lastProgress = try await report(state,callback:progress,last:lastProgress)
        try Task.checkCancellation()
        try await worker.finish(prepared,app:app,owner:session.bSteamID)
        let elapsed = ProcessInfo.processInfo.systemUptime-start
        SteamLog.event("[steam-depot] install complete app=\(app.appID) bytes=\(prepared.size) seconds=\(String(format:"%.2f",elapsed))")
        return prepared.folder
    }
}
