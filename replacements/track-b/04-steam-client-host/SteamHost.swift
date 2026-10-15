// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation
import CryptoKit

struct SteamHostLaunch: Equatable, Sendable {
    var exe: String
    var arguments: String
    var workingFolder: String
    var environment: [String:String]
}
enum SteamHostError: LocalizedError, Equatable {
    case alreadyInstalled
    case downloadFailed(String)
    case installationDidNotFinish
    case invalidGame
    case notInstalledForClient
    case prefixMissing
    var errorDescription: String? {
        switch self {
        case .alreadyInstalled: return "Steam is already installed in this prefix."
        case .downloadFailed(let reason): return "Steam's installer could not be downloaded: \(reason)."
        case .installationDidNotFinish: return "Steam installation did not finish. Try again with a working internet connection."
        case .invalidGame: return "This game ID is invalid."
        case .notInstalledForClient: return "Steam doesn't see this game as installed."
        case .prefixMissing: return "The Windows prefix folder is missing."
        }
    }
}

enum BSteamHostPolicy {
    static let minimum = 512 * 1024
    static let maximum = 64 * 1024 * 1024
    static let installerWindows = "C:\\users\\Public\\Downloads\\SteamSetup.exe"
    static let installerFolder = "C:\\users\\Public\\Downloads"
    static let firstUpdateNote = "Steam will download its first update, a few hundred MB. Keep the connection working, then sign in through Steam's own window."
    static func redirectAllowed(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "https", let host = url.host?.lowercased(), url.user == nil, url.password == nil else { return false }
        guard host.hasSuffix(".steamstatic.com") || host.hasSuffix(".steampowered.com"), host.utf8.count <= 253,
              url.port == nil || (1...65535).contains(url.port ?? 0) else { return false }
        return host.split(separator:".",omittingEmptySubsequences:false).allSatisfy { label in
            !label.isEmpty && label.utf8.count <= 63 && !label.hasPrefix("-") && !label.hasSuffix("-") && label.utf8.allSatisfy { (48...57).contains($0) || (97...122).contains($0) || $0 == 45 }
        }
    }
    static func arguments(gpu: Bool) -> String {
        gpu ? "-no-cef-sandbox" : "-no-cef-sandbox -cef-disable-gpu -cef-disable-gpu-compositing"
    }
    static func validPE(_ data: Data) -> Bool {
        guard data.count >= 64, data[data.startIndex] == 0x4d, data[data.startIndex+1] == 0x5a else { return false }
        var offset: UInt32 = 0
        for i in 0..<4 { offset |= UInt32(data[data.startIndex+0x3c+i]) << (i*8) }
        guard UInt64(offset) <= UInt64(data.count-24) else { return false }
        let index = data.startIndex+Int(offset)
        return Array(data[index..<(index+4)]) == [0x50,0x45,0,0]
    }
    static func validateSize(_ count: Int) throws {
        guard (minimum...maximum).contains(count) else { throw SteamHostError.downloadFailed("the file size is not between 512 KB and 64 MB") }
    }
    static func validate(_ data: Data) throws {
        try validateSize(data.count)
        guard validPE(data) else { throw SteamHostError.downloadFailed("the file is not a Windows program") }
    }
    static func prefix(_ prefix: URL) throws -> URL {
        var directory: ObjCBool = false
        guard prefix.isFileURL, FileManager.default.fileExists(atPath:prefix.path,isDirectory:&directory), directory.boolValue else { throw SteamHostError.prefixMissing }
        return prefix.standardizedFileURL.resolvingSymlinksInPath()
    }
    static func installer(_ prefix: URL) -> URL { prefix.appendingPathComponent("drive_c/users/Public/Downloads/SteamSetup.exe") }
    static func client(_ prefix: URL) -> URL { prefix.appendingPathComponent("drive_c/Program Files (x86)/Steam/steam.exe") }
    static func noticeKey(_ prefix: URL) -> String {
        "steamhost.first-update." + SHA256.hash(data:Data(prefix.standardizedFileURL.resolvingSymlinksInPath().path.utf8)).map { String(format:"%02x",$0) }.joined()
    }
    static func note(_ prefix: URL) {
        let key = noticeKey(prefix)
        guard UserDefaults.standard.bool(forKey:key) else { return }
        UserDefaults.standard.set(false,forKey:key)
        // Fixed API has no note field: expose text through settings and the app log.
        // The app can display steamhost.noticeText on this first launch (B04-1).
        UserDefaults.standard.set(firstUpdateNote,forKey:"steamhost.noticeText")
        dlog("[steamhost] \(firstUpdateNote)")
    }
    static func save(_ data: Data, prefix: URL) throws {
        let root = try self.prefix(prefix), target = installer(root), parent = target.deletingLastPathComponent()
        func inside(_ url: URL) -> Bool { url.resolvingSymlinksInPath().standardizedFileURL.path.hasPrefix(root.path+"/") }
        guard inside(parent) else { throw SteamHostError.downloadFailed("the installer folder leaves the prefix") }
        try FileManager.default.createDirectory(at:parent,withIntermediateDirectories:true)
        guard inside(parent), (try? target.resourceValues(forKeys:[.isSymbolicLinkKey]).isSymbolicLink) != true else { throw SteamHostError.downloadFailed("the installer path is not a safe file") }
        try data.write(to:target,options:.atomic)
        let hash = SHA256.hash(data:data).map { String(format:"%02x",$0) }.joined()
        dlog("[steamhost] installer bytes=\(data.count) sha256=\(hash)")
    }
}

private final class BSteamHostRedirect: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var rejected = false
    var wasRejected: Bool { lock.lock(); let value = rejected; lock.unlock(); return value }
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest,
                    completionHandler: @escaping (URLRequest?) -> Void) {
        let allowed = request.url.map(BSteamHostPolicy.redirectAllowed) ?? false
        if !allowed { lock.lock(); rejected = true; lock.unlock() }
        completionHandler(allowed ? request : nil)
    }
}

enum SteamHost {
    static let windowsRoot = "C:\\Program Files (x86)\\Steam"
    static func clientInstalled(prefix: URL) -> Bool {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath:BSteamHostPolicy.client(prefix).path),
              let type = attributes[.type] as? FileAttributeType else { return false }
        return type == .typeRegular
    }
    static func prepareInstall(prefix: URL, progress: @escaping @Sendable (String) -> Void) async throws -> SteamHostLaunch {
        _ = try BSteamHostPolicy.prefix(prefix)
        guard !clientInstalled(prefix:prefix) else { throw SteamHostError.alreadyInstalled }
        progress("Downloading Steam's installer…")
        let policy = BSteamHostRedirect()
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil; config.httpShouldSetCookies = false; config.urlCredentialStorage = nil; config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalCacheData; config.timeoutIntervalForRequest = 60; config.timeoutIntervalForResource = 600
        let session = URLSession(configuration:config,delegate:policy,delegateQueue:nil); defer { session.invalidateAndCancel() }
        do {
            guard let url = URL(string:"https://cdn.akamai.steamstatic.com/client/installer/SteamSetup.exe") else { throw SteamHostError.downloadFailed("the installer address is invalid") }
            let request = URLRequest(url:url,cachePolicy:.reloadIgnoringLocalCacheData,timeoutInterval:60)
            let (stream,response) = try await session.bytes(for:request)
            guard !policy.wasRejected else { throw SteamHostError.downloadFailed("Steam redirected the installer to an unapproved address") }
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                throw SteamHostError.downloadFailed("HTTP \(status)")
            }
            guard response.expectedContentLength <= Int64(BSteamHostPolicy.maximum) else { throw SteamHostError.downloadFailed("the file is larger than 64 MB") }
            var data = Data(); data.reserveCapacity(BSteamHostPolicy.minimum)
            for try await byte in stream {
                if data.count >= BSteamHostPolicy.maximum { throw SteamHostError.downloadFailed("the file is larger than 64 MB") }
                data.append(byte)
            }
            try Task.checkCancellation()
            let downloaded = data
            try await Task.detached(priority:.utility) { try BSteamHostPolicy.validate(downloaded); try BSteamHostPolicy.save(downloaded,prefix:prefix) }.value
            try Task.checkCancellation()
            return SteamHostLaunch(exe:BSteamHostPolicy.installerWindows,arguments:"/S",workingFolder:BSteamHostPolicy.installerFolder,environment:[:])
        } catch is CancellationError { throw CancellationError() }
        catch let error as SteamHostError { throw error }
        catch let error as URLError {
            if error.code == .cancelled && Task.isCancelled { throw CancellationError() }
            if [.notConnectedToInternet,.networkConnectionLost].contains(error.code) { throw SteamHostError.downloadFailed("no connection") }
            throw SteamHostError.downloadFailed(error.code == .timedOut ? "the connection timed out" : "a network error occurred (\(error.code.rawValue))")
        } catch { throw SteamHostError.downloadFailed("the installer file could not be saved") }
    }
    static func finishInstall(prefix: URL) throws {
        let prefix = try BSteamHostPolicy.prefix(prefix)
        let installer = BSteamHostPolicy.installer(prefix)
        if FileManager.default.fileExists(atPath:installer.path) {
            do { try FileManager.default.removeItem(at:installer) } catch { throw SteamHostError.installationDidNotFinish }
        }
        guard clientInstalled(prefix:prefix) else { throw SteamHostError.installationDidNotFinish }
        UserDefaults.standard.set(true,forKey:BSteamHostPolicy.noticeKey(prefix))
    }
    // Integration contract (the APP owns this orchestration):
    // 1. Await SteamSession.disconnectGracefully() before running these launches.
    // 2. Keep client and game in one Wine session until menu force-quit or client exit.
    // 3. Block spec03 downloads throughout that session; they share the library.
    // These builders only describe a program. They neither start Wine nor read tokens.
    private static func commonLaunch(prefix: URL) throws -> SteamHostLaunch {
        _ = try BSteamHostPolicy.prefix(prefix)
        guard clientInstalled(prefix:prefix) else { throw SteamHostError.installationDidNotFinish }
        BSteamHostPolicy.note(prefix)
        let setting = UserDefaults.standard.object(forKey:"steamhost.gpu")
        let gpu = (setting as? String) == "1" || (setting as? NSNumber)?.intValue == 1
        return SteamHostLaunch(exe:windowsRoot+"\\steam.exe",arguments:BSteamHostPolicy.arguments(gpu:gpu),
            workingFolder:windowsRoot,environment:["MADEIRA_DOCK_SESSION":"1"])
    }
    static func openClientLaunch(prefix: URL) throws -> SteamHostLaunch { try commonLaunch(prefix:prefix) }
    static func playLaunch(appID: Int, prefix: URL) throws -> SteamHostLaunch {
        guard appID >= 1 && appID <= 4_294_967_294 else { throw SteamHostError.invalidGame }
        _ = try BSteamHostPolicy.prefix(prefix)
        let drive = prefix.appendingPathComponent("drive_c",isDirectory:true), apps = SteamInstallPaths.steamApps(drive:drive)
        guard SteamInstallFiles.buildID(appID:appID,steamApps:apps) != nil else { throw SteamHostError.notInstalledForClient }
        var launch = try commonLaunch(prefix:prefix); launch.arguments += " -applaunch \(appID)"; return launch
    }
}
