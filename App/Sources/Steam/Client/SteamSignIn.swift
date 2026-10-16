// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Steam account sign-in (spec 03 §S1–S4): IAuthenticationService over HTTPS, RSA password
// encryption, Steam Guard, QR, polling, and the stored sign-in in the Keychain.
//
// Sources: spec 03 §S; refs/steam-protobufs/steammessages_auth.steamclient.proto (field
// numbers); Apple Security (SecKeyCreateWithData, SecKeyCreateEncryptedData,
// .rsaEncryptionPKCS1, SecItem* generic passwords, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
// ITU-T X.690 DER (SEQUENCE, INTEGER, length octets); RFC 4648 (base64, base64url);
// RFC 7519 (JWT layout, the `exp` claim).

import Foundation
import Security
import UIKit

// MARK: - Public face

enum SteamSignIn {
    /// Posted on the main queue after a sign-in is stored or removed.
    static let didChange = Notification.Name("SteamSignInDidChange")

    /// A switch: `env.<name>` from the settings file, else the process environment;
    /// a value of "0" (after trimming spaces) is off, any other value is on, absent is `default`.
    nonisolated static func flag(_ name: String, default fallback: Bool) -> Bool {
        let raw = MadeiraConfig.get("env.\(name)") ?? ProcessInfo.processInfo.environment[name]
        guard let raw else { return fallback }
        return raw.trimmingCharacters(in: .whitespaces) != "0"
    }

    /// flag("MADEIRA_STEAM_SIGNIN", default: true)
    nonisolated static var isEnabled: Bool { flag("MADEIRA_STEAM_SIGNIN", default: true) }

    nonisolated static var isSignedIn: Bool { credentialsForDock() != nil }

    nonisolated static var accountName: String? { credentialsForDock()?.accountName }

    /// The stored account name and refresh token, or nil when there is no usable sign-in.
    nonisolated static func credentialsForDock() -> (accountName: String, refreshToken: String)? {
        guard let stored = SteamSignInStore.load() else { return nil }
        let name = stored.accountName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, !stored.refreshToken.isEmpty else { return nil }
        if jwtHasExpired(stored.refreshToken, now: Date()) { return nil }
        return (name, stored.refreshToken)
    }

    nonisolated static func signOut() {
        SteamSignInStore.delete()
        SteamLog.event("[steam-signin] signed out")
        postDidChange()
    }

    /// A sentence for the user explaining `error`.
    nonisolated static func message(_ error: Error) -> String {
        if let steam = error as? SteamError {
            return steam.errorDescription ?? "Steam sign-in failed."
        }
        if let url = error as? URLError {
            switch url.code {
            case .notConnectedToInternet, .networkConnectionLost:
                return "No internet connection. Connect and try again."
            case .timedOut:
                return "Steam did not respond in time. Try again."
            default:
                return "A network error occurred (\(url.code.rawValue)). Try again."
            }
        }
        return error.localizedDescription
    }

    /// A short reason for the log, never containing account data.
    nonisolated static func reason(_ error: Error) -> String {
        if let steam = error as? SteamError {
            switch steam {
            case .wrongCredentials: return "credentials"
            case .wrongGuardCode: return "guard-code"
            case .unexpectedReply: return "unexpected-reply"
            case .notSignedIn: return "signed-out"
            case .sharedContentMissing: return "shared-content"
            case .fileWriteFailed: return "file-write"
            case .rateLimited: return "rate-limited"
            case .accountDisabled: return "disabled"
            case .signInExpired: return "expired"
            case .qrExpired: return "qr-expired"
            case .couldNotPreparePassword: return "rsa"
            case .signInRefused: return "refused"
            case .keychainFailed: return "keychain"
            case .signInUnavailable(let status): return "http-\(status)"
            default: return String(describing: type(of: error))
            }
        }
        if let url = error as? URLError {
            return "url-\(url.code.rawValue)"
        }
        return String(describing: type(of: error))
    }

    // MARK: Internal helpers

    nonisolated static func postDidChange() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: didChange, object: nil)
        }
    }

    /// Stores a completed sign-in, reads it back, and posts didChange (§S4).
    nonisolated static func store(_ tokens: SteamSignInTokens) throws {
        let record = StoredSteamSignIn(accountName: tokens.accountName,
                                       refreshToken: tokens.refreshToken,
                                       accessToken: tokens.accessToken,
                                       steamID: tokens.steamID,
                                       savedAt: Date())
        SteamSignInStore.save(record)
        let readBack = SteamSignInStore.load()
        postDidChange()
        guard let readBack, readBack.refreshToken == tokens.refreshToken else {
            throw SteamError.keychainFailed
        }
    }

    /// True when `token` is a JWT whose payload has a numeric `exp` at or before `now`.
    /// Anything that isn't such a JWT is left for Steam to judge (§S4).
    nonisolated static func jwtHasExpired(_ token: String, now: Date) -> Bool {
        let parts = token.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3 else { return false }
        let payload = parts[1]
        guard !payload.isEmpty, payload.utf8.count <= 8192 else { return false }
        guard let data = base64URLDecode(String(payload)),
              let object = try? JSONSerialization.jsonObject(with: data),
              let claims = object as? [String: Any],
              let exp = claims["exp"] as? NSNumber else { return false }
        // A JSON true/false also bridges to NSNumber; only real numbers count.
        if CFGetTypeID(exp) == CFBooleanGetTypeID() { return false }
        return exp.doubleValue <= now.timeIntervalSince1970
    }

    nonisolated static func base64URLDecode(_ text: String) -> Data? {
        var s = text.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        while s.count % 4 != 0 { s += "=" }
        return Data(base64Encoded: s)
    }
}

/// The result of a completed sign-in.
struct SteamSignInTokens: Equatable {
    var accountName: String
    var refreshToken: String
    var accessToken: String
    var steamID: UInt64
}

// MARK: - Keychain (§S4)

struct StoredSteamSignIn: Codable, Equatable {
    var accountName: String
    var refreshToken: String
    var accessToken: String
    var steamID: UInt64
    var savedAt: Date
}

enum SteamSignInStore {
    // Our item.
    nonisolated static let service = "app.myiosdeck.steam.signin"
    nonisolated static let account = "steam-signin"
    // The item earlier builds kept (imported once, then deleted).
    nonisolated static let legacyService = "madeira.steam.tokens"
    nonisolated static let legacyAccount = "steam_tokens"

    nonisolated private static let lock = NSLock()

    nonisolated private static func baseQuery(service: String, account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    nonisolated private static func readData(service: String, account: String) -> Data? {
        var query = baseQuery(service: service, account: account)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess else { return nil }
        return result as? Data
    }

    nonisolated private static func writeData(_ data: Data, service: String, account: String) -> Bool {
        SecItemDelete(baseQuery(service: service, account: account) as CFDictionary)
        var add = baseQuery(service: service, account: account)
        add[kSecValueData as String] = data
        add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }

    /// Our item, importing the older one first if ours is missing.
    nonisolated static func load() -> StoredSteamSignIn? {
        lock.lock()
        defer { lock.unlock() }
        if let data = readData(service: service, account: account) {
            return try? JSONDecoder().decode(StoredSteamSignIn.self, from: data)
        }
        guard let legacy = readData(service: legacyService, account: legacyAccount),
              let record = decodeLegacy(legacy),
              let data = try? JSONEncoder().encode(record) else { return nil }
        if writeData(data, service: service, account: account),
           readData(service: service, account: account) != nil {
            SecItemDelete(baseQuery(service: legacyService, account: legacyAccount) as CFDictionary)
            SteamLog.event("[steam-signin] imported the sign-in of an earlier build")
        }
        return record
    }

    nonisolated static func save(_ record: StoredSteamSignIn) {
        guard let data = try? JSONEncoder().encode(record) else { return }
        lock.lock()
        defer { lock.unlock() }
        _ = writeData(data, service: service, account: account)
    }

    nonisolated static func delete() {
        lock.lock()
        defer { lock.unlock() }
        SecItemDelete(baseQuery(service: service, account: account) as CFDictionary)
    }

    /// The older JSON: accountName, refreshToken, accessToken, steamID (number) and savedAt
    /// (seconds since 2001-01-01). Read leniently, field by field.
    nonisolated static func decodeLegacy(_ data: Data) -> StoredSteamSignIn? {
        guard let object = try? JSONSerialization.jsonObject(with: data),
              let dict = object as? [String: Any],
              let name = dict["accountName"] as? String,
              let refresh = dict["refreshToken"] as? String else { return nil }
        let access = dict["accessToken"] as? String ?? ""
        let steamID = (dict["steamID"] as? NSNumber)?.uint64Value ?? 0
        let savedAt = (dict["savedAt"] as? NSNumber).map { Date(timeIntervalSinceReferenceDate: $0.doubleValue) } ?? Date()
        return StoredSteamSignIn(accountName: name, refreshToken: refresh, accessToken: access,
                                 steamID: steamID, savedAt: savedAt)
    }
}

// MARK: - HTTPS transport (§S1)

enum SteamAuthAPI {
    nonisolated static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        config.timeoutIntervalForRequest = 20
        return URLSession(configuration: config)
    }()

    /// Base64 of the message, then every byte that isn't a letter or digit as %XX.
    nonisolated static func encodeField(_ message: Data) -> String {
        let base64 = message.base64EncodedString()
        var out = ""
        out.reserveCapacity(base64.utf8.count * 2)
        for b in base64.utf8 {
            let isAlnum = (b >= 0x30 && b <= 0x39) || (b >= 0x41 && b <= 0x5A) || (b >= 0x61 && b <= 0x7A)
            if isAlnum {
                out.unicodeScalars.append(Unicode.Scalar(b))
            } else {
                out += String(format: "%%%02X", b)
            }
        }
        return out
    }

    /// Maps a non-1 x-eresult to the user-facing error (§S1).
    nonisolated static func error(forResult code: Int) -> SteamError {
        switch code {
        case 5, 18: return .wrongCredentials
        case 65, 88: return .wrongGuardCode
        case 84, 87: return .rateLimited
        case 17: return .accountDisabled
        case 9, 27: return .signInExpired
        default: return .signInRefused(code)
        }
    }

    nonisolated static func makeRequest(method: String, message: Data) -> URLRequest? {
        let base = "https://api.steampowered.com/IAuthenticationService/\(method)/v1/"
        let field = "input_protobuf_encoded=" + encodeField(message)
        let isGet = method == "GetPasswordRSAPublicKey"
        guard let url = URL(string: isGet ? base + "?" + field : base) else { return nil }
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalAndRemoteCacheData, timeoutInterval: 20)
        if isGet {
            request.httpMethod = "GET"
        } else {
            request.httpMethod = "POST"
            request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
            request.httpBody = Data(field.utf8)
        }
        return request
    }

    /// One call. Throws SteamError for Steam's refusals, URLError when nothing came back.
    nonisolated static func call(_ method: String, _ message: ProtoWriter) async throws -> ProtoFields {
        guard let request = makeRequest(method: method, message: message.data) else {
            throw SteamError.signInRefused(0)
        }
        let (body, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        if http.statusCode == 429 { throw SteamError.rateLimited }
        guard http.statusCode == 200 else { throw SteamError.signInUnavailable(http.statusCode) }
        if let header = http.value(forHTTPHeaderField: "x-eresult"),
           let code = Int(header.trimmingCharacters(in: .whitespaces)), code != 1 {
            SteamLog.trace("[steam-signin] \(method) eresult=\(code)")
            throw error(forResult: code)
        }
        // No format parameter is sent, so the reply is protobuf; JSON means something else
        // answered (clarification 4).
        if let type = http.value(forHTTPHeaderField: "Content-Type"), type.lowercased().contains("json") {
            throw SteamError.unexpectedReply
        }
        guard body.count <= 1 << 20 else { throw SteamError.unexpectedReply }
        do {
            return try ProtoFields(body)
        } catch {
            throw SteamError.unexpectedReply
        }
    }
}

// MARK: - RSA (§S2 step 2)

enum SteamPasswordCipher {
    nonisolated static func bytes(fromHex hex: String) -> Data? {
        var digits = Array(hex.utf8.filter { !($0 == 0x20 || $0 == 0x0A || $0 == 0x0D || $0 == 0x09) })
        if digits.count % 2 == 1 { digits.insert(UInt8(ascii: "0"), at: 0) }
        guard !digits.isEmpty else { return nil }
        func value(_ c: UInt8) -> UInt8? {
            switch c {
            case 0x30...0x39: return c - 0x30
            case 0x41...0x46: return c - 0x41 + 10
            case 0x61...0x66: return c - 0x61 + 10
            default: return nil
            }
        }
        var out = Data(capacity: digits.count / 2)
        var i = 0
        while i < digits.count {
            guard let hi = value(digits[i]), let lo = value(digits[i + 1]) else { return nil }
            out.append(hi << 4 | lo)
            i += 2
        }
        return out
    }

    nonisolated static func derLength(_ n: Int) -> Data {
        if n < 0x80 { return Data([UInt8(n)]) }
        var bytes: [UInt8] = []
        var v = n
        while v > 0 {
            bytes.insert(UInt8(v & 0xFF), at: 0)
            v >>= 8
        }
        return Data([0x80 | UInt8(bytes.count)] + bytes)
    }

    nonisolated static func derInteger(_ magnitude: Data) -> Data {
        var m = [UInt8](magnitude)
        while m.count > 1, m[0] == 0 { m.removeFirst() }
        if m.isEmpty { m = [0] }
        if m[0] & 0x80 != 0 { m.insert(0, at: 0) }
        return Data([0x02]) + derLength(m.count) + Data(m)
    }

    /// DER RSAPublicKey: SEQUENCE { INTEGER modulus, INTEGER exponent }.
    nonisolated static func rsaPublicKeyDER(modulus: Data, exponent: Data) -> Data {
        let body = derInteger(modulus) + derInteger(exponent)
        return Data([0x30]) + derLength(body.count) + body
    }

    /// Encrypts the UTF-8 password with PKCS#1 v1.5 and returns base64.
    nonisolated static func encrypt(password: String, modulusHex: String, exponentHex: String) throws -> String {
        guard let modulus = bytes(fromHex: modulusHex), let exponent = bytes(fromHex: exponentHex) else {
            throw SteamError.couldNotPreparePassword
        }
        let der = rsaPublicKeyDER(modulus: modulus, exponent: exponent)
        let significant = modulus.drop { $0 == 0 }.count
        let attributes: [String: Any] = [
            kSecAttrKeyType as String: kSecAttrKeyTypeRSA,
            kSecAttrKeyClass as String: kSecAttrKeyClassPublic,
            kSecAttrKeySizeInBits as String: significant * 8,
        ]
        var error: Unmanaged<CFError>?
        guard let key = SecKeyCreateWithData(der as CFData, attributes as CFDictionary, &error) else {
            throw SteamError.couldNotPreparePassword
        }
        guard SecKeyIsAlgorithmSupported(key, .encrypt, .rsaEncryptionPKCS1),
              let cipher = SecKeyCreateEncryptedData(key, .rsaEncryptionPKCS1, Data(password.utf8) as CFData, &error) else {
            throw SteamError.couldNotPreparePassword
        }
        return (cipher as Data).base64EncodedString()
    }
}

// MARK: - Flows (§S2, §S3)

/// What the user must do after a password was accepted.
struct SteamGuardPrompt: Equatable {
    enum CodeKind: Int { case email = 2, device = 3 }
    var code: CodeKind?          // a code field (device wins over email)
    var emailHint: String?       // Steam's hint text for type 2
    var approval: Bool           // "approve in the Steam app"
    var isEmpty: Bool { code == nil && !approval }

    nonisolated static func from(_ confirmations: [(type: Int, message: String)]) -> SteamGuardPrompt {
        let types = Set(confirmations.map(\.type))
        var prompt = SteamGuardPrompt(code: nil, emailHint: nil, approval: false)
        if types.isEmpty || types == [1] { return prompt }
        if types.contains(3) {
            prompt.code = .device
        } else if types.contains(2) {
            prompt.code = .email
            let hint = confirmations.first { $0.type == 2 }?.message ?? ""
            prompt.emailHint = hint.isEmpty ? nil : hint
        }
        prompt.approval = types.contains(4) || types.contains(5)
        return prompt
    }
}

/// One running authentication session. The client ID may change while polling.
@MainActor
final class SteamAuthSession {
    var clientID: UInt64
    let requestID: Data
    let steamID: UInt64
    var interval: Double
    var challengeURL: String
    let typedAccountName: String?

    init(clientID: UInt64, requestID: Data, steamID: UInt64, interval: Double,
         challengeURL: String, typedAccountName: String?) {
        self.clientID = clientID
        self.requestID = requestID
        self.steamID = steamID
        self.interval = interval
        self.challengeURL = challengeURL
        self.typedAccountName = typedAccountName
    }
}

@MainActor
enum SteamAuthFlow {
    static let platformSteamClient: UInt32 = 1
    static let persistent: Int32 = 1
    static let passwordPollLimit: TimeInterval = 5 * 60
    static let qrPollLimit: TimeInterval = 10 * 60

    static var deviceName: String { UIDevice.current.name }

    nonisolated static func clampInterval(_ seconds: Float?) -> Double {
        guard let seconds, seconds.isFinite, seconds > 0 else { return 5 }
        return min(max(Double(seconds), 1), 60)
    }

    /// S2 steps 1–3. Returns the session and what to ask the user.
    static func beginPassword(account: String, password: String) async throws -> (SteamAuthSession, SteamGuardPrompt) {
        var keyRequest = ProtoWriter()
        keyRequest.string(1, account)
        let key = try await SteamAuthAPI.call("GetPasswordRSAPublicKey", keyRequest)
        let modulus = key.string(1) ?? ""
        guard !modulus.isEmpty else { throw SteamError.wrongCredentials }
        let encrypted = try SteamPasswordCipher.encrypt(password: password, modulusHex: modulus,
                                                        exponentHex: key.string(2) ?? "")

        var begin = ProtoWriter()
        begin.string(1, deviceName)
        begin.string(2, account)
        begin.string(3, encrypted)
        begin.uint64(4, key.uint64(3) ?? 0)
        begin.uint32(6, platformSteamClient)
        begin.int32(7, persistent)
        let reply = try await SteamAuthAPI.call("BeginAuthSessionViaCredentials", begin)

        let clientID = reply.uint64(1) ?? 0
        guard clientID != 0 else { throw SteamError.wrongCredentials }
        let confirmations = try reply.messages(4).map { c in
            (type: Int(c.uint32(1) ?? 0), message: c.string(2) ?? "")
        }
        let session = SteamAuthSession(clientID: clientID, requestID: reply.bytes(2).map { Data($0) } ?? Data(),
                                       steamID: reply.uint64(5) ?? 0, interval: clampInterval(reply.float(3)),
                                       challengeURL: "", typedAccountName: account)
        return (session, SteamGuardPrompt.from(confirmations))
    }

    /// S2 step 5.
    static func submitCode(_ code: String, kind: SteamGuardPrompt.CodeKind, session: SteamAuthSession) async throws {
        var update = ProtoWriter()
        update.uint64(1, session.clientID)
        update.fixed64(2, session.steamID)
        update.string(3, code.trimmingCharacters(in: .whitespacesAndNewlines).uppercased())
        update.uint32(4, UInt32(kind.rawValue))
        _ = try await SteamAuthAPI.call("UpdateAuthSessionWithSteamGuardCode", update)
    }

    /// S3 step 1.
    static func beginQR() async throws -> SteamAuthSession {
        var begin = ProtoWriter()
        begin.string(1, deviceName)
        begin.uint32(2, platformSteamClient)
        let reply = try await SteamAuthAPI.call("BeginAuthSessionViaQR", begin)
        let url = reply.string(2) ?? ""
        guard !url.isEmpty else { throw SteamError.unexpectedReply }
        return SteamAuthSession(clientID: reply.uint64(1) ?? 0, requestID: reply.bytes(3).map { Data($0) } ?? Data(),
                                steamID: 0, interval: clampInterval(reply.float(4)),
                                challengeURL: url, typedAccountName: nil)
    }

    /// S2 step 6 / S3 step 3: poll once per interval until both tokens arrive or `limit` runs
    /// out. A new challenge URL is passed to `onChallenge`. Cancelling the task stops it.
    static func poll(_ session: SteamAuthSession, limit: TimeInterval, expired: SteamError,
                     onChallenge: @escaping @MainActor (String) -> Void = { _ in }) async throws -> SteamSignInTokens {
        let deadline = Date().addingTimeInterval(limit)
        var networkFailures = 0
        while true {
            try await Task.sleep(nanoseconds: UInt64(session.interval * 1_000_000_000))
            try Task.checkCancellation()
            guard Date() < deadline else { throw expired }

            var request = ProtoWriter()
            request.uint64(1, session.clientID)
            request.bytes(2, session.requestID)
            let reply: ProtoFields
            do {
                reply = try await SteamAuthAPI.call("PollAuthSessionStatus", request)
                networkFailures = 0
            } catch let error as URLError {
                // A short network drop shouldn't end a five-minute wait; three in a row do.
                networkFailures += 1
                if networkFailures >= 3 { throw error }
                continue
            }
            try Task.checkCancellation()

            if let newID = reply.uint64(1), newID != 0 { session.clientID = newID }
            if let url = reply.string(2), !url.isEmpty, url != session.challengeURL {
                session.challengeURL = url
                onChallenge(url)
            }
            let refresh = reply.string(3) ?? ""
            let access = reply.string(4) ?? ""
            if !refresh.isEmpty, !access.isEmpty {
                let replied = (reply.string(6) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                let name = replied.isEmpty ? (session.typedAccountName ?? "") : replied
                return SteamSignInTokens(accountName: name, refreshToken: refresh,
                                         accessToken: access, steamID: session.steamID)
            }
        }
    }
}
