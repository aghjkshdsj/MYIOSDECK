// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

enum BSteamAppParser {
    static func parse(_ data: Data, appID: UInt32) throws -> SteamAppInfo? {
        guard data.count <= 4 * 1024 * 1024 else { throw SteamError.invalidMessage }
        var bytes = data
        while bytes.last == 0 { bytes.removeLast() }
        var root: SteamValue
        do { var parser = try SteamKeyValues(bytes); root = try parser.read() } catch { throw SteamError.invalidMessage }
        for _ in 0..<4 {
            if let next = root[String(appID)] { root = next }
            else if let next = root["appinfo"] { root = next }
            else { break }
        }
        let common = root["common"] ?? .object([:]), config = root["config"] ?? .object([:])
        guard let name = common["name"]?.string, !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        var info = SteamAppInfo(appID:appID,name:name,installDir:config["installdir"]?.string ?? "",buildID:0,libraryCapsule:nil,headerImage:nil,parentID:nil,launches:launches(config["launch"]))
        info.bIntegrationCloudRoot = root["ufs"]
        info.bType = common["type"]?.string?.lowercased() ?? "unknown"; info.bOS = common["oslist"]?.string ?? ""; info.bFree = common["freetodownload"]?.string == "1"
        let capsule = common["library_assets_full"]?["library_capsule"]
        info.libraryCapsule = artwork(capsule?["image2x"]) ?? artwork(capsule?["image"]) ?? artwork(common["header_image"])
        info.headerImage = artwork(common["header_image"])
        if let parent = common["parent"]?.string.flatMap(UInt32.init), parent != 0, parent != appID { info.parentID = parent }
        let depots = root["depots"] ?? .object([:])
        info.buildID = depots["branches"]?["public"]?["buildid"]?.string.flatMap(UInt32.init) ?? 0
        guard depots.fields.count <= 10_000 else { throw SteamError.invalidMessage }
        for key in depots.fields.keys.sorted() {
            guard let id = UInt32(key), let value = depots[key] else { continue }
            var d = BSteamDepot(id:id)
            d.name = value["name"]?.string ?? ""; d.maxSize = value["maxsize"]?.string.flatMap(UInt64.init) ?? 0
            d.dlc = value["dlcappid"]?.string.flatMap(UInt32.init) ?? 0; d.shared = value["sharedinstall"]?.string == "1"
            if let owner = value["depotfromapp"]?.string.flatMap(UInt32.init), owner != appID, owner != 0 { d.owner = owner }
            let options = value["config"]
            d.os = options?["oslist"]?.string ?? ""; d.arch = options?["osarch"]?.string ?? ""; d.language = options?["language"]?.string ?? ""; d.lowViolence = options?["lowviolence"]?.string == "1"
            for (branch, manifest) in value["manifests"]?.fields ?? [:] {
                if let gid = (manifest.string ?? manifest["gid"]?.string).flatMap(UInt64.init), gid != 0 { d.manifests[branch] = gid }
                if branch == "public" { d.size = manifest["size"]?.string.flatMap(UInt64.init) ?? 0; d.download = manifest["download"]?.string.flatMap(UInt64.init) ?? 0 }
            }
            info.bDepots.append(d)
        }
        return info
    }
    static func artwork(_ table: SteamValue?) -> String? {
        let fields = table?.fields ?? [:]
        let value = fields["english"]?.string ?? fields.keys.sorted().compactMap { fields[$0]?.string }.first
        guard let value = value, !value.isEmpty, value.utf8.count <= 256, !value.contains(".."), !value.hasPrefix("/") else { return nil }; return value
    }
    static func launches(_ value: SteamValue?) -> [SteamLaunchOption] {
        let ordered = (value?.fields ?? [:]).compactMap { key, value -> (UInt32,SteamValue)? in guard let id = UInt32(key) else { return nil }; return (id,value) }.sorted { $0.0 < $1.0 }
        var result: [SteamLaunchOption] = []
        for (index, value) in ordered.prefix(32) {
            let executable = value["executable"]?.string ?? "", arguments = value["arguments"]?.string ?? "", working = value["workingdir"]?.string ?? ""
            let config = value["config"], type = value["type"]?.string ?? "", os = config?["oslist"]?.string ?? "", arch = config?["osarch"]?.string ?? "", beta = config?["betakey"]?.string ?? "", dlc = config?["ownsdlc"]?.string
            guard !executable.isEmpty, executable.utf8.count <= 512, arguments.utf8.count <= 2048, working.utf8.count <= 512,
                  [type,os,arch,beta,dlc ?? ""].allSatisfy({ $0.utf8.count <= 256 }) else { continue }
            result.append(SteamLaunchOption(executable:executable,index:index,requiredDLC:dlc,arguments:arguments,workingDir:working,type:type,oslist:os,osarch:arch,betaKey:beta))
        }
        return result
    }
    static func integers(_ name: String, in value: SteamValue) -> Set<UInt32> {
        var stack = [value], result: Set<UInt32> = [], visited = 0
        while let value = stack.popLast() {
            visited += 1; if visited > 100_000 { break }
            if let target = value[name] { for leaf in target.fields.values { if let id = leaf.string.flatMap(UInt32.init) { result.insert(id) } } }
            for child in value.fields.values { if case .object = child { stack.append(child) } }
        }
        return result
    }
}

@MainActor final class SteamLibraryFetcher {
    private let session: any BSteamSessionProtocol
    private var packages: [SteamValue]?
    private var depotCache: Set<UInt32>?
    init(session: any BSteamSessionProtocol) { self.session = session }
    private func tokens(_ ids: [UInt32], packages: Bool) async throws -> [UInt32:UInt64] {
        if ids.isEmpty { return [:] }
        guard ids.count <= 100_000 else { throw SteamError.invalidMessage }
        var body = BSteamProto(); for id in ids { body.uint(packages ? 1 : 2,UInt64(id)) }
        let replies = try await session.bRequest(8905,body:body.encoded(),name:nil,timeout:30,multipart:false)
        guard let reply = replies.first else { throw SteamError.invalidMessage }
        return try await Task.detached(priority:.utility) {
            let response = try BSteamProto(reply.body)
            var result: [UInt32:UInt64] = [:]
            for token in try response.messages(packages ? 1 : 3) {
                if let n = token.number(1), let id = UInt32(exactly:n) { result[id] = token.number(2) ?? 0 }
            }
            return result
        }.value
    }
    private func buffers(_ ids: [UInt32], packages: Bool) async throws -> [(UInt32,Data)] {
        if ids.isEmpty { return [] }
        let access = try await tokens(ids,packages:packages)
        var result: [(UInt32,Data)] = []
        for start in stride(from:0,to:ids.count,by:50) {
            try Task.checkCancellation()
            var body = BSteamProto()
            for id in ids[start..<min(start+50,ids.count)] {
                var entry = BSteamProto(); entry.uint(1,UInt64(id)); entry.uint(2,access[id] ?? 0); body.bytes(packages ? 1 : 2,entry.encoded())
            }
            let replies = try await session.bRequest(8903,body:body.encoded(),name:nil,timeout:30,multipart:true)
            let found = try await Task.detached(priority:.utility) { () throws -> [(UInt32,Data)] in
                var list: [(UInt32,Data)] = []
                for reply in replies {
                    for entry in try BSteamProto(reply.body).messages(packages ? 3 : 1) {
                        guard let n = entry.number(1), let id = UInt32(exactly:n), let buffer = entry.data(5), buffer.count <= 4*1024*1024 else { continue }
                        list.append((id,buffer)); guard list.count <= 100_000 else { throw SteamError.invalidMessage }
                    }
                }
                return list
            }.value
            result += found; guard result.count <= 100_000 else { throw SteamError.invalidMessage }
        }
        return result
    }
    private func packageInfo() async throws -> [SteamValue] {
        if let packages = packages { return packages }
        try await session.bConnect(); let licenses = try await session.bLicenses()
        let data = try await buffers(Array(Set(licenses)).sorted(),packages:true)
        let values = try await Task.detached(priority:.utility) { try data.map { _, bytes -> SteamValue in var reader = try BSteamBinaryKV(bytes); return try reader.read() } }.value
        packages = values; return values
    }
    func ownedDepotIDs() async throws -> Set<UInt32> {
        if let depotCache = depotCache { return depotCache }
        let values = try await packageInfo()
        let result = await Task.detached(priority:.utility) { values.reduce(into:Set<UInt32>()) { $0.formUnion(BSteamAppParser.integers("depotids",in:$1)) } }.value
        depotCache = result; return result
    }
    func fetchOwnedApps() async throws -> [SteamAppInfo] {
        let packages = try await packageInfo()
        let ids = await Task.detached(priority:.utility) { packages.reduce(into:Set<UInt32>()) { $0.formUnion(BSteamAppParser.integers("appids",in:$1)) }.sorted() }.value
        let data = try await buffers(ids,packages:false)
        let apps = try await Task.detached(priority:.utility) {
            try data.compactMap { id, bytes in try BSteamAppParser.parse(bytes,appID:id) }.filter { ["game","demo","application"].contains($0.bType) }
        }.value
        SteamLog.event("[steam-library] owned apps=\(apps.count)")
        return apps.sorted { $0.appID < $1.appID }
    }
    private func app(_ id: UInt32) async throws -> SteamAppInfo? {
        let values = try await buffers([id],packages:false)
        guard let data = values.first(where:{ $0.0 == id })?.1 else { return nil }
        return try await Task.detached(priority:.utility) { try BSteamAppParser.parse(data,appID:id) }.value
    }
    func fetchInstallInfo(appID: UInt32) async throws -> SteamAppInfo? {
        try await session.bConnect()
        guard let initial = try await app(appID) else { return nil }
        let assumed = initial.bDepots.map { value -> BSteamDepot in var d = value; if d.owner != nil && d.manifests["public"] == nil { d.manifests["public"] = 1 }; return d }
        var queue: [(UInt32,UInt32)] = BSteamDepotSelection.select(assumed).compactMap { d in d.owner.map { ($0,d.id) } }
        var apps: [UInt32:SteamAppInfo] = [appID:initial], visited: Set<UInt32> = [appID], seen: Set<String> = [], index = 0, references = 0
        while index < queue.count {
            try Task.checkCancellation()
            let (owner,depot) = queue[index]; index += 1
            guard seen.insert("\(owner):\(depot)").inserted else { continue }
            references += 1; guard references <= 1024 else { throw SteamError.sharedMetadata }
            if visited.insert(owner).inserted {
                guard visited.count <= 32 else { throw SteamError.sharedMetadata }
                guard let info = try await app(owner) else { throw SteamError.sharedMetadata }; apps[owner] = info
            }
            guard let source = apps[owner]?.bDepots.first(where:{ $0.id == depot }) else { throw SteamError.sharedMetadata }
            if let next = source.owner { queue.append((next,depot)) }
        }
        let sourceApps = apps
        return try await Task.detached(priority:.utility) { () throws -> SteamAppInfo? in
            var resolved = sourceApps
            for _ in 0..<32 {
                var changed = false
                for id in resolved.keys.sorted() {
                    guard var info = resolved[id] else { continue }
                    for i in info.bDepots.indices {
                        var depot = info.bDepots[i]
                        guard let owner = depot.owner, let source = resolved[owner]?.bDepots.first(where:{ $0.id == depot.id }) else { continue }
                        let old = depot
                        depot.manifests.merge(source.manifests,uniquingKeysWith:{ own,_ in own })
                        if depot.size == 0 { depot.size = source.size }; if depot.download == 0 { depot.download = source.download }; if depot.maxSize == 0 { depot.maxSize = source.maxSize }
                        if depot.os.isEmpty { depot.os = source.os }; if depot.arch.isEmpty { depot.arch = source.arch }; if depot.language.isEmpty { depot.language = source.language }
                        info.bDepots[i] = depot
                        if depot != old { changed = true }
                    }
                    resolved[id] = info
                }
                if !changed { break }
            }
            guard var result = resolved[appID] else { return nil }
            let assumed = result.bDepots.map { value -> BSteamDepot in var d = value; if d.owner != nil && d.manifests["public"] == nil { d.manifests["public"] = 1 }; return d }
            let needed = Set(BSteamDepotSelection.select(assumed).map(\.id))
            for d in result.bDepots where needed.contains(d.id) && d.manifests["public"] == nil { throw SteamError.sharedMetadata }
            for (id, info) in resolved where id != appID { result.bOwners[id] = BSteamOwner(name:info.name,folder:info.installDir,build:info.buildID) }
            return result
        }.value
    }
}
