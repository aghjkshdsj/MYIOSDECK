// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation
import Security
import CoreFoundation

struct BSteamCredentials: Codable, Sendable {
    var accountName: String
    var refreshToken: String
    var accessToken: String
    var steamID: UInt64
    var savedAt: Date
}
enum BSteamTokenStore {
    private static let service = "MYIOSDECK.trackB.steam.credentials.v1"
    private static let account = "session"
    private static func query(_ service: String, _ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: account]
    }
    private static func read(_ service: String, _ account: String) -> BSteamCredentials? {
        var q = query(service, account); q[kSecReturnData as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitOne
        var value: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &value) == errSecSuccess, let data = value as? Data, data.count <= 65_536 else { return nil }
        return try? JSONDecoder().decode(BSteamCredentials.self, from: data)
    }
    static func load() -> BSteamCredentials? {
        if let value = read(service, account) { return value }
        guard SecItemCopyMatching(query(service,account) as CFDictionary,nil) == errSecItemNotFound else { return nil }
        guard let old = read("madeira.steam.tokens", "steam_tokens") else { return nil }
        do {
            try store(old)
            SecItemDelete(query("madeira.steam.tokens", "steam_tokens") as CFDictionary)
            return old
        } catch { return nil }
    }
    static func store(_ credentials: BSteamCredentials) throws {
        guard let data = try? JSONEncoder().encode(credentials), data.count <= 65_536 else { throw SteamError.keychainStorage }
        let attributes: [String: Any] = [kSecValueData as String: data, kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        let base = query(service, account)
        var status = SecItemUpdate(base as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            var added = base; attributes.forEach { added[$0.key] = $0.value }
            status = SecItemAdd(added as CFDictionary, nil)
            if status == errSecDuplicateItem { status = SecItemUpdate(base as CFDictionary, attributes as CFDictionary) }
        }
        guard status == errSecSuccess, read(service, account)?.refreshToken == credentials.refreshToken,
              !credentials.refreshToken.isEmpty else { throw SteamError.keychainStorage }
        DispatchQueue.main.async { NotificationCenter.default.post(name: SteamSignIn.didChange, object: nil) }
    }
    static func remove() {
        SecItemDelete(query(service, account) as CFDictionary)
        // A sign-out must not let an unimported legacy item sign the user back in.
        SecItemDelete(query("madeira.steam.tokens", "steam_tokens") as CFDictionary)
    }
    static func expired(_ token: String, now: Date = Date()) -> Bool {
        let pieces = token.split(separator: ".", omittingEmptySubsequences: false)
        guard pieces.count == 3, pieces[1].utf8.count <= 8192 else { return false }
        var payload = String(pieces[1]).replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        payload += String(repeating: "=", count: (4 - payload.utf8.count % 4) % 4)
        guard let data = Data(base64Encoded: payload), data.count <= 8192,
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let exp = object["exp"] as? NSNumber, CFGetTypeID(exp) != CFBooleanGetTypeID() else { return false }
        let seconds = exp.doubleValue
        return seconds.isFinite && seconds <= now.timeIntervalSince1970
    }
}

enum SteamSignIn {
    static let didChange = Notification.Name("MYIOSDECKTrackBSteamSignInChanged")
    static func flag(_ name: String, default fallback: Bool) -> Bool {
        let value = MadeiraConfig.get("env.\(name)") ?? ProcessInfo.processInfo.environment[name]
        return value.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) != "0" } ?? fallback
    }
    static var isEnabled: Bool { flag("MADEIRA_STEAM_SIGNIN", default: true) }
    static var isSignedIn: Bool { credentialsForDock() != nil }
    static var accountName: String? { credentialsForDock()?.accountName }
    static func credentialsForDock() -> (accountName: String, refreshToken: String)? {
        guard let value = BSteamTokenStore.load() else { return nil }
        let name = value.accountName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, !value.refreshToken.isEmpty, !BSteamTokenStore.expired(value.refreshToken) else { return nil }
        return (name, value.refreshToken)
    }
    static func signOut() {
        BSteamTokenStore.remove(); SteamLog.event("[steam-signin] signed out")
        DispatchQueue.main.async { NotificationCenter.default.post(name: didChange, object: nil) }
    }
    static func message(_ error: Error) -> String {
        if let error = error as? SteamError { return error.errorDescription ?? "Steam could not complete the request." }
        if let error = error as? URLError {
            switch error.code {
            case .notConnectedToInternet, .networkConnectionLost: return "No internet connection. Connect and try again."
            case .timedOut: return "Steam did not respond in time. Try again."
            default: return "A network error occurred (\(error.code.rawValue)). Try again."
            }
        }
        return error.localizedDescription
    }
    static func reason(_ error: Error) -> String {
        if let e = error as? SteamError {
            switch e {
            case .wrongCredentials: return "credentials"
            case .wrongGuardCode: return "guard-code"
            case .rateLimited: return "rate-limited"
            case .accountDisabled: return "disabled"
            case .signInExpired: return "expired"
            case .qrExpired: return "qr-expired"
            case .passwordPreparation: return "rsa"
            case .signInRefused: return "refused"
            case .keychainStorage: return "keychain"
            default: return "SteamError"
            }
        }
        if let e = error as? URLError { return "url-\(e.code.rawValue)" }
        return String(describing: type(of: error))
    }
}

enum BSteamAuth {
    private static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral; c.urlCache = nil; c.requestCachePolicy = .reloadIgnoringLocalCacheData; c.timeoutIntervalForRequest = 20
        return URLSession(configuration: c)
    }()
    static func call(_ method: String, _ body: BSteamProto) async throws -> BSteamProto {
        let encoded = body.encoded().base64EncodedString().utf8.map { byte -> String in
            if (48...57).contains(byte) || (65...90).contains(byte) || (97...122).contains(byte) { return String(UnicodeScalar(byte)) }
            return String(format: "%%%02X", byte)
        }.joined()
        let field = "input_protobuf_encoded=\(encoded)"
        guard var components = URLComponents(string: "https://api.steampowered.com/IAuthenticationService/\(method)/v1/") else { throw SteamError.invalidMessage }
        components.percentEncodedQuery = method == "GetPasswordRSAPublicKey" ? field + "&format=protobuf" : "format=protobuf"
        guard let url = components.url else { throw SteamError.invalidMessage }
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 20)
        if method != "GetPasswordRSAPublicKey" {
            request.httpMethod = "POST"; request.httpBody = Data(field.utf8); request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try await BSteamHTTP.data(request, session: session, limit: 1024 * 1024)
        if response.statusCode == 429 { throw SteamError.rateLimited }
        guard response.statusCode == 200 else { throw SteamError.signInRefused("Steam sign-in is unavailable right now (HTTP \(response.statusCode)).") }
        let result = Int(response.value(forHTTPHeaderField: "x-eresult") ?? "1") ?? 0
        switch result {
        case 1: break
        case 5,18: throw SteamError.wrongCredentials
        case 65,88: throw SteamError.wrongGuardCode
        case 84,87: throw SteamError.rateLimited
        case 17: throw SteamError.accountDisabled
        case 9,27: throw SteamError.signInExpired
        default: throw SteamError.signInRefused("Steam refused the sign-in (code \(result)).")
        }
        return try BSteamProto(data, limit: 1024 * 1024)
    }
    static func rsa(password: String, modulus: String, exponent: String) throws -> String {
        func hex(_ text: String, cap: Int) throws -> [UInt8] {
            guard !text.isEmpty, text.utf8.count <= cap * 2 else { throw SteamError.passwordPreparation }
            let normalized = text.utf8.count % 2 == 0 ? text : "0" + text
            let chars = Array(normalized.utf8)
            func digit(_ b: UInt8) -> UInt8? { if (48...57).contains(b) { return b-48 }; if (65...70).contains(b) { return b-55 }; if (97...102).contains(b) { return b-87 }; return nil }
            var bytes: [UInt8] = []
            for i in stride(from: 0, to: chars.count, by: 2) {
                guard let a = digit(chars[i]), let b = digit(chars[i+1]) else { throw SteamError.passwordPreparation }; bytes.append(a << 4 | b)
            }
            while bytes.count > 1 && bytes.first == 0 { bytes.removeFirst() }
            guard bytes.contains(where: { $0 != 0 }) else { throw SteamError.passwordPreparation }; return bytes
        }
        func length(_ count: Int) -> [UInt8] {
            if count < 128 { return [UInt8(count)] }
            if count < 256 { return [0x81, UInt8(count)] }
            return [0x82, UInt8(count >> 8), UInt8(count & 255)]
        }
        func integer(_ raw: [UInt8]) -> [UInt8] {
            let content = (raw.first ?? 0) & 0x80 != 0 ? [0] + raw : raw
            return [2] + length(content.count) + content
        }
        let n = try hex(modulus, cap: 1024), e = try hex(exponent, cap: 8)
        let content = integer(n) + integer(e), der = Data([0x30] + length(content.count) + content)
        var error: Unmanaged<CFError>?
        defer { if let error = error { _ = error.takeRetainedValue() } }
        let attributes: [String: Any] = [kSecAttrKeyType as String: kSecAttrKeyTypeRSA, kSecAttrKeyClass as String: kSecAttrKeyClassPublic, kSecAttrKeySizeInBits as String: n.count * 8]
        guard let key = SecKeyCreateWithData(der as CFData, attributes as CFDictionary, &error),
              SecKeyIsAlgorithmSupported(key, .encrypt, .rsaEncryptionPKCS1),
              let encrypted = SecKeyCreateEncryptedData(key, .rsaEncryptionPKCS1, Data(password.utf8) as CFData, &error) else { throw SteamError.passwordPreparation }
        return (encrypted as Data).base64EncodedString()
    }
}

actor BSteamAuthFlow {
    struct Confirmation: Sendable { var type: Int; var hint: String }
    private var client: UInt64
    private let request: Data
    private let steamID: UInt64
    private let typedName: String
    private let qr: Bool
    private let interval: Double
    nonisolated let confirmations: [Confirmation]
    nonisolated let challenge: String
    private init(client: UInt64, request: Data, steamID: UInt64, typedName: String, qr: Bool, interval: Double, confirmations: [Confirmation], challenge: String) {
        self.client = client; self.request = request; self.steamID = steamID; self.typedName = typedName; self.qr = qr
        self.interval = interval; self.confirmations = confirmations; self.challenge = challenge
    }
    static func begin(account: String?, password: String?, friendly: String) async throws -> BSteamAuthFlow {
        let qr = account == nil
        var body = BSteamProto(); body.text(1, friendly)
        let reply: BSteamProto
        if let account = account, let password = password {
            var keyRequest = BSteamProto(); keyRequest.text(1, account)
            let key = try await BSteamAuth.call("GetPasswordRSAPublicKey", keyRequest)
            guard let modulus = key.string(1), !modulus.isEmpty else { throw SteamError.wrongCredentials }
            let encrypted = try BSteamAuth.rsa(password: password, modulus: modulus, exponent: key.string(2) ?? "")
            body.text(2, account); body.text(3, encrypted); body.uint(4, key.number(3) ?? 0); body.uint(5, 1); body.uint(6, 1); body.uint(7, 1)
            reply = try await BSteamAuth.call("BeginAuthSessionViaCredentials", body)
        } else {
            body.uint(2, 1); reply = try await BSteamAuth.call("BeginAuthSessionViaQR", body)
        }
        guard let client = reply.number(1), client != 0, let request = reply.data(qr ? 3 : 2), request.count <= 4096 else {
            throw qr ? SteamError.signInRefused("Could not start QR sign-in. Try again.") : SteamError.wrongCredentials
        }
        let challenge = qr ? reply.string(2) ?? "" : ""
        if qr && (challenge.isEmpty || challenge.utf8.count > 4096) { throw SteamError.signInRefused("Could not start QR sign-in. Try again.") }
        let seconds = Double(Float(bitPattern: UInt32(truncatingIfNeeded: reply.number(qr ? 4 : 3) ?? 0)))
        let interval = seconds.isFinite && seconds > 0 ? min(60, max(0.05, seconds)) : 5
        let confirmations = try reply.messages(qr ? 5 : 4).prefix(32).map { Confirmation(type: Int(clamping: $0.number(1) ?? 0), hint: String(($0.string(2) ?? "").prefix(256))) }
        return BSteamAuthFlow(client: client, request: request, steamID: qr ? 0 : reply.number(5) ?? 0, typedName: account ?? "", qr: qr, interval: interval, confirmations: confirmations, challenge: challenge)
    }
    func submit(_ code: String, type: Int) async throws {
        var body = BSteamProto(); body.uint(1, client); body.fixed(2, steamID)
        body.text(3, code.trimmingCharacters(in: .whitespacesAndNewlines).uppercased()); body.uint(4, UInt64(type))
        _ = try await BSteamAuth.call("UpdateAuthSessionWithSteamGuardCode", body)
    }
    func poll(onQR: @Sendable (String) async -> Void) async throws -> BSteamCredentials {
        let end = ProcessInfo.processInfo.systemUptime + (qr ? 600 : 300)
        while true {
            try Task.checkCancellation()
            let remaining = end - ProcessInfo.processInfo.systemUptime
            guard remaining > 0 else { throw qr ? SteamError.qrExpired : SteamError.signInExpired }
            try await Task.sleep(nanoseconds: UInt64(min(interval, remaining) * 1e9))
            if ProcessInfo.processInfo.systemUptime >= end { throw qr ? SteamError.qrExpired : SteamError.signInExpired }
            var body = BSteamProto(); body.uint(1, client); body.bytes(2, request)
            let response: BSteamProto
            do { response = try await bSteamTimeout(min(20, end - ProcessInfo.processInfo.systemUptime)) { try await BSteamAuth.call("PollAuthSessionStatus", body) } }
            catch SteamError.timeout where ProcessInfo.processInfo.systemUptime >= end { throw qr ? SteamError.qrExpired : SteamError.signInExpired }
            if let next = response.number(1), next != 0 { client = next }
            if qr, let url = response.string(2), !url.isEmpty, url.utf8.count <= 4096 { await onQR(url) }
            if let refresh = response.string(3), !refresh.isEmpty, let access = response.string(4), !access.isEmpty {
                guard refresh.utf8.count <= 16_384, access.utf8.count <= 16_384 else { throw SteamError.invalidMessage }
                let returned = response.string(6) ?? ""
                let name = returned.isEmpty ? typedName : returned
                guard name.utf8.count <= 256 else { throw SteamError.invalidMessage }
                return BSteamCredentials(accountName: name, refreshToken: refresh, accessToken: access, steamID: steamID, savedAt: Date())
            }
        }
    }
}
