// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The owned-games library (spec 03 §L1–L6) and depot selection (§D1).
//
// Sources: spec 03 §L, §D1; refs/steam-protobufs/steammessages_clientserver_appinfo.proto
// (CMsgClientPICSAccessTokenRequest/Response, CMsgClientPICSProductInfoRequest/Response).

import Foundation

// MARK: - Model

/// Codable: the app caches arrays of these as JSON, so the property names below are also the
/// JSON keys and must not change.
struct SteamLaunchOption: Codable, Hashable, Sendable {
    var executable: String
    var index: UInt32?          // the config.launch key; nil in old caches
    var requiredDLC: String?
    var arguments: String = ""
    var workingDir: String = ""
    var type: String = ""
    var oslist: String = ""
    var osarch: String = ""
    var betaKey: String = ""
}

extension SteamLaunchOption {
    private enum CodingKeys: String, CodingKey {
        case executable, index, requiredDLC, arguments, workingDir, type, oslist, osarch, betaKey
    }

    /// Lenient decoding: every key but `executable` may be missing in an older cache.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        executable = try c.decode(String.self, forKey: .executable)
        index = try c.decodeIfPresent(UInt32.self, forKey: .index)
        requiredDLC = try c.decodeIfPresent(String.self, forKey: .requiredDLC)
        arguments = try c.decodeIfPresent(String.self, forKey: .arguments) ?? ""
        workingDir = try c.decodeIfPresent(String.self, forKey: .workingDir) ?? ""
        type = try c.decodeIfPresent(String.self, forKey: .type) ?? ""
        oslist = try c.decodeIfPresent(String.self, forKey: .oslist) ?? ""
        osarch = try c.decodeIfPresent(String.self, forKey: .osarch) ?? ""
        betaKey = try c.decodeIfPresent(String.self, forKey: .betaKey) ?? ""
    }
}

enum SteamAppType: String, Sendable {
    case game, dlc, tool, demo, application, music, unknown

    nonisolated init(steamType: String) {
        self = SteamAppType(rawValue: steamType.trimmingCharacters(in: .whitespaces).lowercased()) ?? .unknown
    }

    /// Playable types (§L1 step 5).
    nonisolated var isPlayable: Bool { self == .game || self == .demo || self == .application }
}

struct SteamDepotInfo: Sendable, Equatable {
    let id: UInt32
    var name = ""
    var maxSize: UInt64?
    var dlcAppID: UInt32?
    var sharedInstall = false
    var depotFromApp: UInt32?
    var oslist = ""
    var osarch = ""
    var language = ""
    var lowViolence = false
    /// Branch → manifest gid.
    var manifests: [String: UInt64] = [:]
    var publicSize: UInt64?
    var publicDownload: UInt64?

    nonisolated var publicManifest: UInt64? { manifests["public"] }
}

/// An app that owns shared depots of another app (§L4, recorded for §D8).
struct SteamSharedOwner: Sendable, Equatable {
    var name: String
    var installDir: String
    var buildID: UInt32
}

struct SteamAppInfo {
    let appID: UInt32
    var name: String
    var installDir: String
    var buildID: UInt32
    var libraryCapsule: String?
    var headerImage: String?
    var parentID: UInt32?
    var launches: [SteamLaunchOption]

    // Internal detail used by the downloader (declared after the interface's fields, with
    // defaults, so the memberwise initializer still takes the interface's fields alone).
    var type: SteamAppType = .unknown
    var oslist: String = ""
    var freeToDownload = false
    var depots: [SteamDepotInfo] = []
    var sharedOwners: [UInt32: SteamSharedOwner] = [:]

    var installableOnWindows: Bool {
        SteamOSList.includesWindows(oslist) && type.isPlayable && !SteamDepotSelection.select(self).depots.isEmpty
    }
}

extension SteamAppInfo: Sendable {}

enum SteamOSList {
    /// An empty list means every OS.
    nonisolated static func includesWindows(_ list: String) -> Bool {
        let items = list.lowercased().split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        return items.allSatisfy(\.isEmpty) || items.contains("windows")
    }
}

// MARK: - Depot selection (§D1)

struct SteamDepotSelection {
    var depots: [SteamDepotInfo]
    var skipped: [(id: UInt32, reason: String)]
    var arch: String

    /// Windows, 64-bit, English; falls back to 32-bit when no 64-bit depot was taken.
    nonisolated static func select(_ app: SteamAppInfo, requireManifest: Bool = true) -> SteamDepotSelection {
        let sorted = app.depots.sorted { $0.id < $1.id }
        let first = evaluate(sorted, arch: "64", requireManifest: requireManifest)
        let took64 = first.depots.contains { $0.osarch == "64" }
        let has32 = sorted.contains { $0.osarch == "32" && SteamOSList.includesWindows($0.oslist) }
        if !took64, has32 {
            return evaluate(sorted, arch: "32", requireManifest: requireManifest)
        }
        return first
    }

    nonisolated private static func evaluate(_ depots: [SteamDepotInfo], arch: String,
                                             requireManifest: Bool) -> SteamDepotSelection {
        var taken: [SteamDepotInfo] = []
        var skipped: [(id: UInt32, reason: String)] = []
        for d in depots {
            let reason: String?
            if !SteamOSList.includesWindows(d.oslist) {
                reason = "os"
            } else if let dlc = d.dlcAppID, dlc != 0 {
                reason = "dlc"
            } else if d.sharedInstall {
                reason = "shared"
            } else if requireManifest, d.publicManifest == nil {
                reason = "no-manifest"
            } else if d.lowViolence {
                reason = "lowviolence"
            } else if !(d.osarch.isEmpty || d.osarch == arch) {
                reason = "arch"
            } else if !(d.language.isEmpty || d.language.lowercased() == "english") {
                reason = "language"
            } else {
                reason = nil
            }
            if let reason { skipped.append((d.id, reason)) } else { taken.append(d) }
        }
        return SteamDepotSelection(depots: taken, skipped: skipped, arch: arch)
    }

    nonisolated var logLine: String {
        let ids = depots.map { String($0.id) }.joined(separator: ",")
        let left = skipped.map { "\($0.id):\($0.reason)" }.joined(separator: ",")
        return "arch=\(arch) depots=[\(ids)] skipped=[\(left)]"
    }
}

// MARK: - App info parsing (§L2, §L3, §L5)

enum SteamAppInfoParser {
    /// Parses one PICS app buffer (text KeyValues). nil when the app has no name.
    nonisolated static func parse(appID: UInt32, buffer: Data) throws -> SteamAppInfo? {
        var reader = try SteamKeyValues(buffer, limits: .appInfo)
        return parse(appID: appID, root: try reader.read())
    }

    nonisolated static func locate(appID: UInt32, root: SteamValue) -> SteamValue? {
        let key = String(appID)
        let candidates: [SteamValue?] = [root, root[key], root["appinfo"], root["appinfo"]?[key]]
        for c in candidates {
            if let c, c["common"] != nil || c["depots"] != nil || c["config"] != nil { return c }
        }
        return nil
    }

    nonisolated static func parse(appID: UInt32, root: SteamValue) -> SteamAppInfo? {
        guard let app = locate(appID: appID, root: root) else { return nil }
        let name = (app.path("common.name")?.string ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty else { return nil }

        var parent: UInt32?
        if let p = UInt32(app.path("common.parent")?.string ?? ""), p != 0, p != appID { parent = p }

        let capsule = localized(app.path("common.library_assets_full.library_capsule.image2x"))
            ?? localized(app.path("common.library_assets_full.library_capsule.image"))

        var info = SteamAppInfo(appID: appID,
                                name: name,
                                installDir: app.path("config.installdir")?.string ?? "",
                                buildID: UInt32(app.path("depots.branches.public.buildid")?.string ?? "") ?? 0,
                                libraryCapsule: capsule,
                                headerImage: localized(app.path("common.header_image")),
                                parentID: parent,
                                launches: launchOptions(app.path("config.launch")))
        info.type = SteamAppType(steamType: app.path("common.type")?.string ?? "")
        info.oslist = app.path("common.oslist")?.string ?? ""
        info.freeToDownload = app.path("common.freetodownload")?.string == "1"
        info.depots = depots(app["depots"], appID: appID)
        return info
    }

    /// A per-language value: `english`, else the first language in sorted order. Values over
    /// 256 bytes, containing "..", or starting with "/" are rejected.
    nonisolated static func localized(_ value: SteamValue?) -> String? {
        guard let value else { return nil }
        let raw: String?
        switch value {
        case .text(let s):
            raw = s
        case .object(let table):
            if let english = table["english"]?.string {
                raw = english
            } else {
                raw = table.keys.sorted().lazy.compactMap { table[$0]?.string }.first
            }
        }
        guard let s = raw?.trimmingCharacters(in: .whitespaces), !s.isEmpty,
              s.utf8.count <= 256, !s.contains(".."), !s.hasPrefix("/") else { return nil }
        return s
    }

    /// `config.launch` in numeric key order, at most 32 (§L5).
    nonisolated static func launchOptions(_ launch: SteamValue?) -> [SteamLaunchOption] {
        guard let launch else { return [] }
        var keyed: [(UInt32, SteamValue)] = []
        for (key, value) in launch.fields {
            if let n = UInt32(key) { keyed.append((n, value)) }
        }
        keyed.sort { $0.0 < $1.0 }

        var out: [SteamLaunchOption] = []
        for (index, entry) in keyed.prefix(32) {
            guard let exe = entry["executable"]?.string, !exe.trimmingCharacters(in: .whitespaces).isEmpty else { continue }
            let arguments = entry["arguments"]?.string ?? ""
            let workingDir = entry["workingdir"]?.string ?? ""
            let type = entry["type"]?.string ?? ""
            let oslist = entry.path("config.oslist")?.string ?? ""
            let osarch = entry.path("config.osarch")?.string ?? ""
            let betaKey = entry.path("config.betakey")?.string ?? ""
            let ownsDLC = entry.path("config.ownsdlc")?.string
            guard exe.utf8.count <= 512, arguments.utf8.count <= 2048, workingDir.utf8.count <= 512,
                  type.utf8.count <= 256, oslist.utf8.count <= 256, osarch.utf8.count <= 256,
                  betaKey.utf8.count <= 256, (ownsDLC?.utf8.count ?? 0) <= 256 else { continue }
            out.append(SteamLaunchOption(executable: exe, index: index, requiredDLC: ownsDLC,
                                         arguments: arguments, workingDir: workingDir, type: type,
                                         oslist: oslist, osarch: osarch, betaKey: betaKey))
        }
        return out
    }

    /// `depots.<id>` for numeric keys only (§L3).
    nonisolated static func depots(_ section: SteamValue?, appID: UInt32) -> [SteamDepotInfo] {
        guard let section else { return [] }
        var out: [SteamDepotInfo] = []
        for (key, value) in section.fields {
            guard let id = UInt32(key), case .object = value else { continue }
            var d = SteamDepotInfo(id: id)
            d.name = value["name"]?.string ?? ""
            d.maxSize = UInt64(value["maxsize"]?.string ?? "")
            if let dlc = UInt32(value["dlcappid"]?.string ?? ""), dlc != 0 { d.dlcAppID = dlc }
            d.sharedInstall = value["sharedinstall"]?.string == "1"
            if let owner = UInt32(value["depotfromapp"]?.string ?? ""), owner != 0, owner != appID {
                d.depotFromApp = owner
            }
            d.oslist = value.path("config.oslist")?.string ?? ""
            d.osarch = value.path("config.osarch")?.string ?? ""
            d.language = value.path("config.language")?.string ?? ""
            d.lowViolence = value.path("config.lowviolence")?.string == "1"
            for (branch, entry) in value["manifests"]?.fields ?? [:] {
                switch entry {
                case .text(let gid):
                    if let g = UInt64(gid) { d.manifests[branch] = g }
                case .object:
                    if let g = UInt64(entry["gid"]?.string ?? "") { d.manifests[branch] = g }
                    if branch == "public" {
                        d.publicSize = UInt64(entry["size"]?.string ?? "")
                        d.publicDownload = UInt64(entry["download"]?.string ?? "")
                    }
                }
            }
            out.append(d)
        }
        return out.sorted { $0.id < $1.id }
    }

    /// Package buffer (binary KeyValues): the integer values of its `appids` and `depotids`
    /// sections. Steam may put the package ID as a u32 in front of the KeyValues; it is skipped
    /// when present (QUESTIONS.md, spec 03).
    nonisolated static func packageContents(packageID: UInt32, buffer: Data) throws -> SteamPackageInfo {
        let offset = buffer.steamU32(at: 0) == packageID && buffer.steamByte(at: 4) == 0 ? 4 : 0
        var reader = SteamBinaryKeyValues(buffer, offset: offset)
        let root = try reader.read()
        func ints(_ key: String) -> [UInt32] {
            (root.find(key, depth: 2)?.integerValues ?? []).compactMap { UInt32(exactly: $0) }
        }
        return SteamPackageInfo(appIDs: ints("appids"), depotIDs: ints("depotids"))
    }
}

struct SteamPackageInfo: Sendable, Equatable {
    var appIDs: [UInt32]
    var depotIDs: [UInt32]
}

// MARK: - Fetcher (§L1, §L4, §L6)

@MainActor
final class SteamLibraryFetcher {
    private let session: any SteamConnection
    private var packageCache: [UInt32: SteamPackageInfo] = [:]
    private var ownedDepotCache: Set<UInt32>?

    static let batchSize = 50
    static let tokenTimeout: TimeInterval = 30
    static let infoTimeout: TimeInterval = 30

    /// The app passes its SteamSession; tests pass a scripted SteamConnection.
    init(session: any SteamConnection) {
        self.session = session
    }

    /// Every owned app of a playable type (game, demo, application).
    func fetchOwnedApps() async throws -> [SteamAppInfo] {
        let packages = try await session.licensedPackageIDs()
        let contents = try await packageInfos(packages)
        var appIDs = Set<UInt32>()
        for info in contents.values { appIDs.formUnion(info.appIDs) }
        let apps = try await appInfos(Array(appIDs).sorted())
        let playable = apps.values.filter { $0.type.isPlayable }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        SteamLog.event("[steam-library] packages=\(packages.count) apps=\(appIDs.count) playable=\(playable.count)")
        return playable
    }

    /// One app's metadata, ready for an install (shared depots resolved, §L4).
    func fetchInstallInfo(appID: UInt32) async throws -> SteamAppInfo? {
        guard var info = try await appInfos([appID])[appID] else { return nil }
        try await resolveSharedDepots(&info)
        return info
    }

    /// Depot IDs included in the account's licences (§L6). Cached for the fetcher's life.
    func ownedDepotIDs() async throws -> Set<UInt32> {
        if let ownedDepotCache { return ownedDepotCache }
        let packages = try await session.licensedPackageIDs()
        let contents = try await packageInfos(packages)
        var depots = Set<UInt32>()
        for info in contents.values { depots.formUnion(info.depotIDs) }
        ownedDepotCache = depots
        return depots
    }

    // MARK: PICS

    private func packageInfos(_ ids: [UInt32]) async throws -> [UInt32: SteamPackageInfo] {
        let missing = Array(Set(ids).subtracting(packageCache.keys)).sorted()
        if !missing.isEmpty {
            var tokens: [UInt32: UInt64] = [:]
            for batch in missing.steamChunks(of: 1000) {
                var request = ProtoWriter()
                for id in batch { request.uint32(1, id) }
                let reply = try await session.request(SteamEMsg.clientPICSAccessTokenRequest, body: request.data,
                                                      timeout: Self.tokenTimeout)
                for t in try ProtoFields(reply.body).messages(1) {
                    if let id = t.uint32(1) { tokens[id] = t.uint64(2) ?? 0 }
                }
            }
            for batch in missing.steamChunks(of: Self.batchSize) {
                try Task.checkCancellation()
                var request = ProtoWriter()
                for id in batch {
                    var p = ProtoWriter()
                    p.uint32(1, id)
                    p.uint64(2, tokens[id] ?? 0)
                    request.message(1, p)
                }
                let parts = try await session.requestParts(SteamEMsg.clientPICSProductInfoRequest,
                                                           body: request.data, timeout: Self.infoTimeout)
                for part in parts {
                    for package in try ProtoFields(part.body).messages(3) {
                        guard let id = package.uint32(1), let buffer = package.bytes(5) else { continue }
                        do {
                            packageCache[id] = try SteamAppInfoParser.packageContents(packageID: id, buffer: Data(buffer))
                        } catch {
                            SteamLog.event("[steam-library] package=\(id) unreadable")
                        }
                    }
                }
            }
        }
        var out: [UInt32: SteamPackageInfo] = [:]
        for id in ids { if let p = packageCache[id] { out[id] = p } }
        return out
    }

    /// Access tokens, then product info, for apps; parsed off the main actor.
    private func appInfos(_ ids: [UInt32]) async throws -> [UInt32: SteamAppInfo] {
        guard !ids.isEmpty else { return [:] }
        var tokens: [UInt32: UInt64] = [:]
        for batch in ids.steamChunks(of: 1000) {
            var request = ProtoWriter()
            for id in batch { request.uint32(2, id) }
            let reply = try await session.request(SteamEMsg.clientPICSAccessTokenRequest, body: request.data,
                                                  timeout: Self.tokenTimeout)
            for t in try ProtoFields(reply.body).messages(3) {
                if let id = t.uint32(1) { tokens[id] = t.uint64(2) ?? 0 }
            }
        }
        var out: [UInt32: SteamAppInfo] = [:]
        for batch in ids.steamChunks(of: Self.batchSize) {
            try Task.checkCancellation()
            var request = ProtoWriter()
            for id in batch {
                var a = ProtoWriter()
                a.uint32(1, id)
                a.uint64(2, tokens[id] ?? 0)
                request.message(2, a)
            }
            let parts = try await session.requestParts(SteamEMsg.clientPICSProductInfoRequest,
                                                       body: request.data, timeout: Self.infoTimeout)
            var buffers: [(UInt32, Data)] = []
            for part in parts {
                for app in try ProtoFields(part.body).messages(1) {
                    guard let id = app.uint32(1), let buffer = app.bytes(5) else { continue }
                    buffers.append((id, Data(buffer)))
                }
            }
            let received = buffers
            let parsed = await Task.detached(priority: .userInitiated) { () -> [(UInt32, SteamAppInfo?)] in
                received.map { id, data in
                    (id, try? SteamAppInfoParser.parse(appID: id, buffer: data))
                }
            }.value
            for (id, info) in parsed {
                if let info { out[id] = info }
            }
        }
        return out
    }

    // MARK: Shared depots (§L4)

    private func resolveSharedDepots(_ info: inout SteamAppInfo) async throws {
        var owners: [UInt32: SteamAppInfo?] = [:]          // fetched apps; nil = Steam had none
        var lookup: [UInt32: UInt32] = [:]                  // depot → app to look in next
        let candidates = Set(SteamDepotSelection.select(info, requireManifest: false).depots.map(\.id))
        for d in info.depots where d.depotFromApp != nil && d.publicManifest == nil && candidates.contains(d.id) {
            lookup[d.id] = d.depotFromApp
        }

        func owner(_ id: UInt32) async throws -> SteamAppInfo? {
            if let known = owners[id] { return known }
            guard owners.count < 32 else { return nil }
            try Task.checkCancellation()
            let fetched = try await appInfos([id])[id]
            owners[id] = .some(fetched)
            return fetched
        }

        var references = 0
        var changed = true
        while changed, !lookup.isEmpty, references < 1024 {
            changed = false
            for (depotID, appID) in lookup.sorted(by: { $0.key < $1.key }) {
                references += 1
                if references > 1024 { break }
                guard let ownerApp = try await owner(appID),
                      let source = ownerApp.depots.first(where: { $0.id == depotID }),
                      let index = info.depots.firstIndex(where: { $0.id == depotID }) else {
                    lookup[depotID] = nil
                    continue
                }
                if source.publicManifest != nil {
                    var d = info.depots[index]
                    d.manifests = source.manifests
                    d.publicSize = source.publicSize
                    d.publicDownload = source.publicDownload
                    if d.maxSize == nil { d.maxSize = source.maxSize }
                    if d.oslist.isEmpty { d.oslist = source.oslist }
                    if d.osarch.isEmpty { d.osarch = source.osarch }
                    if d.language.isEmpty { d.language = source.language }
                    info.depots[index] = d
                    lookup[depotID] = nil
                    changed = true
                } else if let next = source.depotFromApp, next != appID, next != info.appID {
                    lookup[depotID] = next
                    changed = true
                } else {
                    lookup[depotID] = nil
                }
            }
        }

        // Owner names, folders and builds for the install record (§D8).
        let selected = SteamDepotSelection.select(info, requireManifest: false).depots
        for d in selected {
            guard let ownerID = d.depotFromApp, info.sharedOwners[ownerID] == nil else { continue }
            if let ownerApp = try await owner(ownerID) {
                info.sharedOwners[ownerID] = SteamSharedOwner(name: ownerApp.name, installDir: ownerApp.installDir,
                                                              buildID: ownerApp.buildID)
            }
        }

        if selected.contains(where: { $0.depotFromApp != nil && $0.publicManifest == nil }) {
            throw SteamError.sharedContentMissing
        }
        let shared = selected.filter { $0.depotFromApp != nil }.count
        if shared > 0 {
            SteamLog.event("[steam-library] app=\(info.appID) shared depots=\(shared) owners=\(info.sharedOwners.count)")
        }
    }
}

extension Array {
    nonisolated func steamChunks(of size: Int) -> [ArraySlice<Element>] {
        guard size > 0 else { return [self[...]] }
        return stride(from: 0, to: count, by: size).map { self[$0..<Swift.min($0 + size, count)] }
    }
}
