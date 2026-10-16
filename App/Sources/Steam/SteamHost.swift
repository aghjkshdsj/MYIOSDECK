// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 04: Valve's own Windows Steam client in the Wine prefix.
//
// The app installs Valve's official client from Valve's public installer and runs it in its
// Wine session. The user signs in through Steam's own window, and from then on Steam does
// everything itself (updates, licences, starting games). This file only prepares files and
// describes launches: it never starts a program, never talks to the client from the inside,
// never changes Valve's files and never handles anything Steam treats as a credential. The
// app's own Steam sign-in (spec 03) is not passed to the client.
//
// Session rules (spec 04 §S). The app implements these around the launches built here:
//   1. Before running `openClientLaunch` or `playLaunch`, the app closes its own Steam
//      connection with `SteamSession.disconnectGracefully()` (spec 03). One account signed in
//      twice at once can sign the other session out.
//   2. The client and the game it starts run as one Wine session. The session ends when the
//      user ends it from the game menu (the app's force-quit), or when the client exits by
//      itself.
//   3. Spec 03 downloads must not run while that session runs: both write the same library
//      folder (`SteamInstallPaths.steamApps`). The app checks the session state before
//      starting a download.
//
// First run after an install: the client downloads its own update from Valve, a few hundred
// MB. `finishInstall` sets a flag in UserDefaults; after building the first client launch the
// app calls `takeFirstRunNote()` and shows the text it returns (once; it is also logged).
//
// Spec 04 "Clarifications (round 1)" 1-10 are applied here: the extra `clientNotInstalled`
// error (2), what "prefix missing" means (3), the order of checks in `playLaunch` (4), the
// redirect rule (5), the extra download reasons (6), how `steamhost.gpu` is read (7), the PE
// check (8) and a failed installer delete being ignored (9).

import Foundation
import CryptoKit

/// One Windows program for the app to run in its Wine session.
struct SteamHostLaunch: Equatable, Sendable {
    var exe: String               // Windows path, e.g. C:\Program Files (x86)\Steam\steam.exe
    var arguments: String         // one string, split by the launcher (double quotes group)
    var workingFolder: String     // Windows path of the folder to start in
    var environment: [String: String]   // extra variables for this launch only
}

enum SteamHost {
    /// C:\Program Files (x86)\Steam, the client's install folder (Windows path).
    static let windowsRoot: String = #"C:\Program Files (x86)\Steam"#

    // MARK: Constants (spec 04 §I, §L)

    static let clientWindowsPath = windowsRoot + #"\steam.exe"#
    static let installerURL = URL(string: "https://cdn.akamai.steamstatic.com/client/installer/SteamSetup.exe")!
    static let installerWindowsFolder = #"C:\users\Public\Downloads"#
    static let installerWindowsPath = installerWindowsFolder + #"\SteamSetup.exe"#
    /// 512 KB to 64 MB, both ends included.
    static let installerSizeRange = (512 * 1024)...(64 * 1024 * 1024)
    static let appIDRange = 1...4_294_967_294
    /// Settings are UserDefaults keys `steamhost.<name>`.
    static let settingPrefix = "steamhost."
    static let firstRunNoteKey = "steamhost.firstRunNotePending"
    /// Understood by the app's launcher (spec 02 launch step 6): publish no Steam game
    /// identity of its own, because Valve's client gives each game its own.
    static let dockSessionVariable = "MADEIRA_DOCK_SESSION"

    static let firstRunNote = "Steam is setting itself up for the first time. It downloads its "
        + "own update from Valve, a few hundred MB, so this can take several minutes. Keep the "
        + "app open and connected until Steam's window appears."

    // MARK: Folders in the prefix

    static func driveC(prefix: URL) -> URL {
        prefix.appendingPathComponent("drive_c", isDirectory: true)
    }

    /// `<prefix>/drive_c/Program Files (x86)/Steam`, the folder of `windowsRoot`.
    static func clientFolder(prefix: URL) -> URL {
        driveC(prefix: prefix)
            .appendingPathComponent("Program Files (x86)", isDirectory: true)
            .appendingPathComponent("Steam", isDirectory: true)
    }

    static func clientExecutable(prefix: URL) -> URL {
        clientFolder(prefix: prefix).appendingPathComponent("steam.exe", isDirectory: false)
    }

    /// `<prefix>/drive_c/users/Public/Downloads`, the folder of `installerWindowsFolder`.
    static func installerFolder(prefix: URL) -> URL {
        driveC(prefix: prefix)
            .appendingPathComponent("users", isDirectory: true)
            .appendingPathComponent("Public", isDirectory: true)
            .appendingPathComponent("Downloads", isDirectory: true)
    }

    static func installerFile(prefix: URL) -> URL {
        installerFolder(prefix: prefix).appendingPathComponent("SteamSetup.exe", isDirectory: false)
    }

    /// A prefix counts as present when its `drive_c` folder exists.
    static func prefixExists(_ prefix: URL) -> Bool {
        var isDirectory: ObjCBool = false
        return FileManager.default.fileExists(atPath: driveC(prefix: prefix).path,
                                              isDirectory: &isDirectory) && isDirectory.boolValue
    }

    // MARK: Installing (spec 04 §I)

    /// Whether steam.exe exists in the prefix (`prefix` is the Wine prefix folder).
    static func clientInstalled(prefix: URL) -> Bool {
        isRegularFile(clientExecutable(prefix: prefix))
    }

    /// Download Valve's official installer into the prefix and return the launch that runs
    /// it silently. Throws SteamHostError.
    static func prepareInstall(prefix: URL,
                               progress: @escaping @Sendable (String) -> Void) async throws -> SteamHostLaunch {
        guard prefixExists(prefix) else {
            dlog("[steamhost] install: prefix missing at \(prefix.path)")
            throw SteamHostError.prefixMissing
        }
        guard !clientInstalled(prefix: prefix) else {
            dlog("[steamhost] install: the client is already installed")
            throw SteamHostError.alreadyInstalled
        }

        progress("Downloading Steam's installer…")
        dlog("[steamhost] install: downloading \(installerURL.absoluteString)")
        let data = try await SteamHostInstallerDownload.fetch(installerURL)

        progress("Checking Steam's installer…")
        if let problem = installerProblem(data) {
            dlog("[steamhost] install: installer rejected (\(problem.text)) size \(data.count)")
            throw SteamHostError.downloadFailed(problem)
        }

        progress("Saving Steam's installer…")
        let file = installerFile(prefix: prefix)
        do {
            try FileManager.default.createDirectory(at: installerFolder(prefix: prefix),
                                                    withIntermediateDirectories: true)
            try data.write(to: file, options: .atomic)
        } catch {
            dlog("[steamhost] install: could not save the installer: \(error.localizedDescription)")
            throw SteamHostError.downloadFailed(.saveFailed)
        }
        // Valve updates the installer, so its hash is logged, not pinned.
        dlog("[steamhost] install: saved SteamSetup.exe, \(data.count) bytes, SHA-256 \(sha256Hex(data))")

        return SteamHostLaunch(exe: installerWindowsPath,
                               arguments: "/S",   // the installer's silent mode
                               workingFolder: installerWindowsFolder,
                               environment: [:])
    }

    /// After the install session ended: whether it worked, removing the downloaded installer.
    static func finishInstall(prefix: URL) throws {
        guard prefixExists(prefix) else {
            dlog("[steamhost] install: prefix missing at \(prefix.path)")
            throw SteamHostError.prefixMissing
        }
        let file = installerFile(prefix: prefix)
        if FileManager.default.fileExists(atPath: file.path) {
            do {
                try FileManager.default.removeItem(at: file)
            } catch {
                dlog("[steamhost] install: could not delete the installer: \(error.localizedDescription)")
            }
        }
        guard clientInstalled(prefix: prefix) else {
            dlog("[steamhost] install: steam.exe missing after the installer ran")
            throw SteamHostError.installationIncomplete
        }
        UserDefaults.standard.set(true, forKey: firstRunNoteKey)
        dlog("[steamhost] install: Steam client installed")
    }

    /// The note about Steam's first-run self-update: returns its text once after a successful
    /// install (the next time the app asks), then nil until the next install. The app calls
    /// this after building a client launch and shows the text (clarification 1).
    static func takeFirstRunNote() -> String? {
        let defaults = UserDefaults.standard
        guard defaults.bool(forKey: firstRunNoteKey) else { return nil }
        defaults.removeObject(forKey: firstRunNoteKey)
        dlog("[steamhost] first run: \(firstRunNote)")
        return firstRunNote
    }

    // MARK: Launches (spec 04 §L)

    /// The launch that opens Valve's client and asks it to start `appID`.
    static func playLaunch(appID: Int, prefix: URL) throws -> SteamHostLaunch {
        guard appIDRange.contains(appID) else {
            dlog("[steamhost] play: invalid app ID \(appID)")
            throw SteamHostError.invalidGame
        }
        try requireClient(prefix: prefix)
        // The library folder downloads go into is also the client's default library, so a
        // record there is one the client reads.
        let steamApps = SteamInstallPaths.steamApps(drive: driveC(prefix: prefix))
        guard SteamInstallFiles.buildID(appID: appID, steamApps: steamApps) != nil else {
            dlog("[steamhost] play: no install record for app \(appID)")
            throw SteamHostError.notInstalledForClient
        }
        // No game arguments: Steam uses the game's own launch configuration.
        let launch = clientLaunch(extraArguments: ["-applaunch", String(appID)], defaults: .standard)
        dlog("[steamhost] play: app \(appID) through the client")
        return launch
    }

    /// The launch that opens Valve's client on its library (no game).
    static func openClientLaunch(prefix: URL) throws -> SteamHostLaunch {
        try requireClient(prefix: prefix)
        let launch = clientLaunch(extraArguments: [], defaults: .standard)
        dlog("[steamhost] open: the client")
        return launch
    }

    /// steam.exe with the common arguments and environment, then `extraArguments`.
    static func clientLaunch(extraArguments: [String], defaults: UserDefaults) -> SteamHostLaunch {
        let arguments = clientArguments(gpu: setting("gpu", in: defaults) == "1") + extraArguments
        return SteamHostLaunch(exe: clientWindowsPath,
                               arguments: arguments.joined(separator: " "),
                               workingFolder: windowsRoot,
                               environment: [dockSessionVariable: "1"])
    }

    /// The options Wine needs for Steam's built-in browser (CEF). Each is listed on Valve's
    /// "Command line options (Steam)" page:
    ///   -no-cef-sandbox               disables sandboxing in CEF
    ///   -cef-disable-gpu              disables GPU usage in CEF (software rendering)
    ///   -cef-disable-gpu-compositing  disables GPU accelerated compositing in browsers
    static func clientArguments(gpu: Bool) -> [String] {
        var arguments = ["-no-cef-sandbox"]
        if !gpu {
            arguments += ["-cef-disable-gpu", "-cef-disable-gpu-compositing"]
        }
        return arguments
    }

    /// The setting `steamhost.<name>` as text: a string is trimmed, a number or Bool becomes
    /// its digits (true is "1"), anything else or nothing is nil.
    static func setting(_ name: String, in defaults: UserDefaults) -> String? {
        switch defaults.object(forKey: settingPrefix + name) {
        case let text as String:
            return text.trimmingCharacters(in: .whitespacesAndNewlines)
        case let number as NSNumber:
            return number.stringValue
        default:
            return nil
        }
    }

    private static func requireClient(prefix: URL) throws {
        guard prefixExists(prefix) else {
            dlog("[steamhost] launch: prefix missing at \(prefix.path)")
            throw SteamHostError.prefixMissing
        }
        guard clientInstalled(prefix: prefix) else {
            dlog("[steamhost] launch: the client isn't installed")
            throw SteamHostError.clientNotInstalled
        }
    }

    // MARK: Checks

    /// What is wrong with a downloaded installer, or nil when it can be used: the size must be
    /// in `installerSizeRange` and the file must be a Windows program.
    static func installerProblem(_ data: Data) -> SteamHostError.DownloadReason? {
        guard installerSizeRange.contains(data.count) else { return .size(data.count) }
        guard looksLikeWindowsProgram(data) else { return .notWindowsProgram }
        return nil
    }

    /// True when `data` starts with the MS-DOS signature `MZ` and the 32-bit little-endian
    /// offset at 0x3C points at a `PE\0\0` signature followed by a whole 20-byte COFF file
    /// header (Microsoft's PE Format document, "MS-DOS Stub" and "Signature").
    static func looksLikeWindowsProgram(_ data: Data) -> Bool {
        func byte(_ offset: Int) -> UInt8? {
            offset >= 0 && offset < data.count ? data[data.startIndex + offset] : nil
        }
        guard byte(0) == 0x4D, byte(1) == 0x5A else { return false }   // "MZ"
        guard let b0 = byte(0x3C), let b1 = byte(0x3D), let b2 = byte(0x3E), let b3 = byte(0x3F) else {
            return false
        }
        let peOffset = Int(b0) | Int(b1) << 8 | Int(b2) << 16 | Int(b3) << 24
        guard peOffset + 4 + 20 <= data.count else { return false }
        return byte(peOffset) == 0x50 && byte(peOffset + 1) == 0x45      // "PE"
            && byte(peOffset + 2) == 0 && byte(peOffset + 3) == 0
    }

    /// The redirect rule for the installer download: HTTPS only, to a host ending in
    /// `.steamstatic.com` or `.steampowered.com` (one trailing dot of a fully qualified name
    /// is ignored; case doesn't matter).
    static func isAllowedRedirect(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "https", var host = url.host?.lowercased() else {
            return false
        }
        if host.hasSuffix(".") { host.removeLast() }
        return host.hasSuffix(".steamstatic.com") || host.hasSuffix(".steampowered.com")
    }

    static func sha256Hex(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    private static func isRegularFile(_ url: URL) -> Bool {
        let path = url.resolvingSymlinksInPath().path
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: path) else {
            return false
        }
        return attributes[.type] as? FileAttributeType == .typeRegular
    }
}
