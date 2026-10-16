// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 04 §E: the errors of the Steam client host.

import Foundation

enum SteamHostError: LocalizedError, Equatable {
    /// Why the installer download failed (spec 04 §I steps 2-4).
    enum DownloadReason: Equatable, Sendable {
        /// No usable connection: offline, host not reachable, or timed out.
        case noConnection
        /// The final answer was not HTTP 200 (0: not an HTTP answer at all).
        case httpStatus(Int)
        /// The file is smaller than 512 KB or larger than 64 MB; the value is its size in bytes.
        case size(Int)
        /// The file doesn't start with `MZ` or has no PE header.
        case notWindowsProgram
        /// A redirect went to a host outside Valve's download domains, or not over HTTPS.
        case redirectRefused(String)
        /// Any other network error; the value is the `URLError.Code` raw value.
        case network(Int)
        /// The file couldn't be written into the prefix.
        case saveFailed
    }

    /// `prepareInstall` was called while steam.exe already exists.
    case alreadyInstalled
    case downloadFailed(DownloadReason)
    /// The install session ended without leaving steam.exe behind.
    case installationIncomplete
    /// An app ID outside 1...4294967294.
    case invalidGame
    /// The game has no install record in the client's library (no build ID).
    case notInstalledForClient
    /// A launch of the client was asked for before the client was installed.
    case clientNotInstalled
    /// The Wine prefix (its `drive_c` folder) doesn't exist.
    case prefixMissing

    var errorDescription: String? {
        switch self {
        case .alreadyInstalled:
            return "Steam is already installed."
        case .downloadFailed(let reason):
            return "Couldn't download Steam's installer: " + reason.text
        case .installationIncomplete:
            return "Steam's installation did not finish. Try again with a working internet connection."
        case .invalidGame:
            return "This isn't a valid Steam game."
        case .notInstalledForClient:
            return "Steam doesn't see this game as installed."
        case .clientNotInstalled:
            return "Steam isn't installed yet."
        case .prefixMissing:
            return "The Windows folder for games is missing."
        }
    }
}

extension SteamHostError.DownloadReason {
    /// The end of the download sentence, starting in lower case.
    var text: String {
        switch self {
        case .noConnection:
            return "there is no working internet connection."
        case .httpStatus(let status):
            return "the server answered with HTTP status \(status)."
        case .size(let bytes):
            return "the file has an unexpected size (\(bytes) bytes)."
        case .notWindowsProgram:
            return "the file isn't a Windows program."
        case .redirectRefused(let host):
            return "the download was sent on to \(host), which isn't one of Valve's servers."
        case .network(let code):
            return "a network error happened (code \(code))."
        case .saveFailed:
            return "it couldn't be saved in the Windows folder."
        }
    }
}
