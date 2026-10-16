// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// SteamLog and SteamError: logging and the user-visible errors of the Steam client.
// Written from specs/03-steam-client.md ("Ground rules", §E) and interfaces/steam_api.md.
//
// Privacy (spec ground rules): log lines carry app IDs, depot IDs, counts, sizes and result
// codes only. Never pass account names, Steam IDs, tokens, codes or message bodies here.

import Foundation

enum SteamLog {
    /// Always-on, low-volume line to the app log.
    nonisolated static func event(_ message: String) {
        LogStore.log(message)
    }

    /// Protocol detail, only when flag("MADEIRA_STEAM_TRACE", default: false).
    nonisolated static func trace(_ message: @autoclosure () -> String) {
        guard SteamSignIn.flag("MADEIRA_STEAM_TRACE", default: false) else { return }
        LogStore.log(message())
    }

    /// A short description of an error for log lines. SteamError values carry only codes,
    /// IDs and short reasons, never account data.
    nonisolated static func describe(_ error: Error) -> String {
        if let steam = error as? SteamError { return String(describing: steam) }
        if let url = error as? URLError { return "url-\(url.code.rawValue)" }
        if error is CancellationError { return "cancelled" }
        return String(describing: type(of: error))
    }
}

/// Every failure the user can see (spec 03 §E).
enum SteamError: LocalizedError, Equatable {
    // Sign-in (§S)
    case wrongCredentials
    case wrongGuardCode
    case rateLimited
    case accountDisabled
    case signInExpired
    case qrExpired
    case couldNotPreparePassword
    case signInRefused(Int)                 // x-eresult that has no better meaning
    case signInUnavailable(Int)             // HTTP status other than 200 and 429
    case unexpectedReply                    // a JSON or unreadable reply
    case keychainFailed
    case notSignedIn

    // CM connection (§C)
    case noServers
    case connectionFailed
    case timeout
    case disconnected
    case logonDenied(Int)
    case invalidMessage(String)

    // Library (§L)
    case appInfoNotFound(UInt32)            // the app constructs this one
    case noWindowsDepot
    case sharedContentMissing

    // Downloads (§D)
    case depotKeyRefused(UInt32)
    case manifestFailed(UInt32)             // depot ID
    case chunkFailed(String)                // short reason
    case decryptionFailed
    case chunkUndecodable(String)           // first bytes, in hex
    case checksumMismatch
    case notEnoughSpace(needed: Int64, available: Int64)
    case fileWriteFailed(String)            // short reason, never a full path

    /// Logon results that mean the stored sign-in no longer works (§C4).
    static let staleSignInCodes: Set<Int> = [5, 15, 18, 26, 27, 94]

    var errorDescription: String? {
        switch self {
        case .wrongCredentials:
            return "The account name or password is wrong."
        case .wrongGuardCode:
            return "The Steam Guard code is wrong. Check it and try again."
        case .rateLimited:
            return "Steam is limiting sign-in attempts. Wait a few minutes and try again."
        case .accountDisabled:
            return "This Steam account is disabled."
        case .signInExpired:
            return "The sign-in took too long and expired. Start over."
        case .qrExpired:
            return "The QR code has expired. Get a new code."
        case .couldNotPreparePassword:
            return "The app could not prepare the password for Steam. Try again."
        case .signInRefused(let code):
            return "Steam refused the sign-in (code \(code))."
        case .signInUnavailable(let status):
            return "Steam sign-in is unavailable right now (HTTP \(status))."
        case .unexpectedReply:
            return "Steam sent an unexpected reply. Try again."
        case .keychainFailed:
            return "Steam accepted the sign-in, but the app could not save it in this device's Keychain."
        case .notSignedIn:
            return "Sign in to Steam first."
        case .noServers:
            return "No Steam servers are available right now."
        case .connectionFailed:
            return "Could not connect to Steam."
        case .timeout:
            return "Steam did not respond in time."
        case .disconnected:
            return "The connection to Steam was lost."
        case .logonDenied(let code):
            if SteamError.staleSignInCodes.contains(code) {
                return "Steam no longer accepts the saved sign-in (code \(code)). Sign in to Steam again."
            }
            return "Steam logon denied (code \(code))."
        case .invalidMessage(let what):
            return "Steam sent a message the app could not read (\(what))."
        case .appInfoNotFound(let appID):
            return "Steam has no information for app \(appID)."
        case .noWindowsDepot:
            return "Steam has no Windows download for this game."
        case .sharedContentMissing:
            return "Steam did not provide required shared content metadata."
        case .depotKeyRefused:
            return "Steam did not allow this account to download the game's files."
        case .manifestFailed(let depot):
            return "The file list for depot \(depot) could not be downloaded."
        case .chunkFailed(let reason):
            return "A piece of the game could not be downloaded (\(reason))."
        case .decryptionFailed:
            return "A piece of the game could not be decrypted."
        case .chunkUndecodable(let format):
            return "A piece of the game could not be decoded (\(format))."
        case .checksumMismatch:
            return "A piece of the game failed its checksum."
        case .notEnoughSpace(let needed, let available):
            return "Not enough free space: the download needs \(SteamError.gigabytes(needed)) GB and \(SteamError.gigabytes(available)) GB is available."
        case .fileWriteFailed(let reason):
            return "The game's files could not be written (\(reason))."
        }
    }

    /// Bytes as GB with one decimal (decimal gigabytes, as iOS shows storage).
    static func gigabytes(_ bytes: Int64) -> String {
        let value = Double(max(bytes, 0)) / 1_000_000_000
        return String(format: "%.1f", value)
    }
}
