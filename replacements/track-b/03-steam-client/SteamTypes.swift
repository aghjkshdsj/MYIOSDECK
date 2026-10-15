// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

enum SteamError: LocalizedError, Equatable {
    case wrongCredentials, wrongGuardCode, rateLimited, accountDisabled, signInExpired, qrExpired, passwordPreparation
    case signInRefused(String), keychainStorage, noServers, connectionFailed, timeout, disconnected, logonDenied(Int), invalidMessage
    case appInfoNotFound(UInt32), noWindowsDepot, sharedMetadata, manifestFailed, chunkFailed, decryptionFailed
    case chunkDecode(String), checksumMismatch, depotKeyRefused, notEnoughSpace(needed: UInt64, available: UInt64), files(String)
    var errorDescription: String? {
        switch self {
        case .wrongCredentials: return "The account name or password is incorrect."
        case .wrongGuardCode: return "The Steam Guard code is incorrect. Try again."
        case .rateLimited: return "Steam is receiving too many requests. Try again later."
        case .accountDisabled: return "This Steam account is disabled."
        case .signInExpired: return "The sign-in session expired. Start again."
        case .qrExpired: return "QR code has expired. Get a new code."
        case .passwordPreparation: return "The app could not prepare the password."
        case .signInRefused(let text): return text
        case .keychainStorage: return "Steam accepted the sign-in, but the app could not save it in this device's Keychain."
        case .noServers: return "No Steam servers are available. Try again later."
        case .connectionFailed: return "Could not connect to Steam. Try again."
        case .timeout: return "Steam did not respond in time. Try again."
        case .disconnected: return "The connection to Steam closed. Try again."
        case .logonDenied(let code):
            return [5,15,18,26,27,94].contains(code) ? "Steam logon denied (code \(code)). Sign in again." : "Steam logon denied (code \(code))."
        case .invalidMessage: return "Steam sent a message the app could not read."
        case .appInfoNotFound(let id): return "Steam did not provide information for app \(id)."
        case .noWindowsDepot: return "Steam has no Windows download for this game."
        case .sharedMetadata: return "Steam did not provide required shared content metadata."
        case .manifestFailed: return "The game's file manifest could not be downloaded."
        case .chunkFailed: return "A game file could not be downloaded. Try again to resume."
        case .decryptionFailed: return "A game file could not be decrypted."
        case .chunkDecode(let format): return "A game file could not be decoded (\(format))."
        case .checksumMismatch: return "A game file failed its checksum. Try again to resume."
        case .depotKeyRefused: return "Steam did not allow this account to download the game's files."
        case .notEnoughSpace(let needed, let available): return String(format: "Not enough free space: the download needs %.2f GB and %.2f GB is available.", Double(needed) / 1e9, Double(available) / 1e9)
        case .files(let text): return text
        }
    }
}

enum SteamLog {
    static func event(_ message: String) { LogStore.log(message) }
    static func trace(_ message: @autoclosure () -> String) {
        if SteamSignIn.flag("MADEIRA_STEAM_TRACE", default: false) { LogStore.log(message()) }
    }
}

struct SteamLaunchOption: Codable, Hashable, Sendable {
    var executable: String
    var index: UInt32?
    var requiredDLC: String?
    var arguments: String = ""
    var workingDir: String = ""
    var type: String = ""
    var oslist: String = ""
    var osarch: String = ""
    var betaKey: String = ""
    init(executable: String, index: UInt32? = nil, requiredDLC: String? = nil, arguments: String = "", workingDir: String = "", type: String = "", oslist: String = "", osarch: String = "", betaKey: String = "") {
        self.executable = executable; self.index = index; self.requiredDLC = requiredDLC; self.arguments = arguments
        self.workingDir = workingDir; self.type = type; self.oslist = oslist; self.osarch = osarch; self.betaKey = betaKey
    }
    enum CodingKeys: String, CodingKey { case executable, index, requiredDLC, arguments, workingDir, type, oslist, osarch, betaKey }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        executable = try c.decode(String.self, forKey: .executable)
        index = try c.decodeIfPresent(UInt32.self, forKey: .index); requiredDLC = try c.decodeIfPresent(String.self, forKey: .requiredDLC)
        arguments = try c.decodeIfPresent(String.self, forKey: .arguments) ?? ""
        workingDir = try c.decodeIfPresent(String.self, forKey: .workingDir) ?? ""
        type = try c.decodeIfPresent(String.self, forKey: .type) ?? ""
        oslist = try c.decodeIfPresent(String.self, forKey: .oslist) ?? ""
        osarch = try c.decodeIfPresent(String.self, forKey: .osarch) ?? ""
        betaKey = try c.decodeIfPresent(String.self, forKey: .betaKey) ?? ""
    }
}

struct BSteamDepot: Sendable, Equatable {
    var id: UInt32
    var name = ""
    var maxSize: UInt64 = 0
    var dlc: UInt32 = 0
    var shared = false
    var owner: UInt32?
    var os = "", arch = "", language = ""
    var lowViolence = false
    var manifests: [String: UInt64] = [:]
    var size: UInt64 = 0
    var download: UInt64 = 0
}
struct BSteamOwner: Sendable { var name: String; var folder: String; var build: UInt32 }
struct SteamAppInfo: Sendable {
    let appID: UInt32
    var name: String
    var installDir: String
    var buildID: UInt32
    var libraryCapsule: String?
    var headerImage: String?
    var parentID: UInt32?
    var launches: [SteamLaunchOption]
    var bType = "game", bOS = ""
    var bFree = false
    var bDepots: [BSteamDepot] = []
    var bIntegrationCloudRoot: SteamValue? = nil
    var bOwners: [UInt32: BSteamOwner] = [:]
    var installableOnWindows: Bool {
        BSteamDepotSelection.windows(bOS) && ["game","demo","application"].contains(bType.lowercased()) && !BSteamDepotSelection.select(bDepots).isEmpty
    }
}
enum BSteamDepotSelection {
    static func windows(_ list: String) -> Bool { list.isEmpty || list.lowercased().contains("windows") }
    static func reason(_ d: BSteamDepot, arch: String) -> String? {
        if !windows(d.os) { return "OS" }
        if d.dlc != 0 { return "DLC" }
        if d.shared { return "shared redistributable" }
        if d.manifests["public", default: 0] == 0 { return "no public manifest" }
        if d.lowViolence { return "low violence" }
        if !d.arch.isEmpty && d.arch != arch { return "architecture" }
        if !d.language.isEmpty && d.language.lowercased() != "english" { return "language" }
        return nil
    }
    static func select(_ depots: [BSteamDepot], log: Bool = false) -> [BSteamDepot] {
        var arch = "64"
        var chosen = depots.filter { reason($0, arch: arch) == nil }
        if !chosen.contains(where: { $0.arch == "64" }) && depots.contains(where: { windows($0.os) && $0.arch == "32" }) {
            arch = "32"; chosen = depots.filter { reason($0, arch: arch) == nil }
        }
        if log {
            for d in depots.sorted(by: { $0.id < $1.id }) {
                if let why = reason(d, arch: arch) { SteamLog.event("[steam-depot] skip depot=\(d.id) reason=\(why)") }
            }
            SteamLog.event("[steam-depot] selected depots=\(chosen.map { String($0.id) }.sorted().joined(separator: ","))")
        }
        return chosen.sorted { $0.id < $1.id }
    }
}
struct SteamDownloadProgress: Equatable, Sendable {
    enum Phase: String, Sendable { case preparing, downloading, finishing }
    var phase: Phase = .preparing
    var totalBytes: UInt64 = 0
    var doneBytes: UInt64 = 0
    var bytesPerSecond: Double = 0
    var fraction: Double { totalBytes == 0 ? 0 : min(1, max(0, Double(doneBytes) / Double(totalBytes))) }
    init() {}
}

@MainActor protocol BSteamSessionProtocol: AnyObject {
    var bSteamID: UInt64 { get }
    var bCellID: UInt32 { get }
    func bConnect() async throws
    func bLicenses() async throws -> [UInt32]
    func bRequest(_ message: UInt32, body: Data, name: String?, timeout: Double, multipart: Bool) async throws -> [BSteamMessage]
}
extension BSteamSessionProtocol {
    func bService(_ name: String, _ body: Data, timeout: Double = 10) async throws -> BSteamProto {
        let replies = try await bRequest(151, body: body, name: name, timeout: timeout, multipart: false)
        guard let reply = replies.first else { throw SteamError.invalidMessage }
        if let result = reply.header.number(13), result != 1 { SteamLog.trace("[steam-cm] service result=\(result)") }
        return try BSteamProto(reply.body)
    }
}
