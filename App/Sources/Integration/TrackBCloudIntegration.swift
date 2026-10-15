// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
// Compatibility for the app's existing Steam Cloud feature, backed only by track B.
import Foundation

enum SteamServiceMethod: String {
    case cloudGetAppFileChangelist = "Cloud.GetAppFileChangelist#1"
    case cloudClientFileDownload = "Cloud.ClientFileDownload#1"
    case cloudBeginAppUploadBatch = "Cloud.BeginAppUploadBatch#1"
    case cloudClientBeginFileUpload = "Cloud.ClientBeginFileUpload#1"
    case cloudClientCommitFileUpload = "Cloud.ClientCommitFileUpload#1"
    case cloudCompleteAppUploadBatch = "Cloud.CompleteAppUploadBatchBlocking#1"
}
struct ProtobufEncoder {
    private var message = BSteamProto()
    var data: Data { message.encoded() }
    mutating func writeUInt32(fieldNumber: Int, value: UInt32) { message.uint(fieldNumber, UInt64(value)) }
    mutating func writeUInt64(fieldNumber: Int, value: UInt64) { message.uint(fieldNumber, value) }
    mutating func writeBool(fieldNumber: Int, value: Bool) { message.uint(fieldNumber, value ? 1 : 0) }
    mutating func writeString(fieldNumber: Int, value: String) { message.text(fieldNumber, value) }
    mutating func writeBytes(fieldNumber: Int, value: Data) { message.bytes(fieldNumber, value) }
}
struct ProtobufDecoder {
    enum WireType { case varint, fixed64, lengthDelimited, fixed32 }
    struct Tag { var fieldNumber: Int; var wireType: WireType }
    private let data: Data
    private var entries: [(Int, BSteamField)]?
    private var index = 0
    private var value: BSteamField?
    init(_ data: Data) { self.data = data }
    mutating func readTag() throws -> Tag? {
        if entries == nil {
            let parsed = try BSteamProto(data)
            entries = parsed.fields.keys.sorted().flatMap { key in (parsed.fields[key] ?? []).map { (key, $0) } }
        }
        guard let entries, index < entries.count else { return nil }
        let entry = entries[index]; index += 1; value = entry.1
        let type: WireType
        switch entry.1 {
        case .number: type = .varint
        case .fixed64: type = .fixed64
        case .fixed32: type = .fixed32
        case .bytes: type = .lengthDelimited
        }
        return Tag(fieldNumber: entry.0, wireType: type)
    }
    mutating func readVarint() throws -> UInt64 {
        guard case .number(let number) = value else { throw SteamError.invalidMessage }
        value = nil; return number
    }
    mutating func readBytes() throws -> Data {
        guard case .bytes(let bytes) = value else { throw SteamError.invalidMessage }
        value = nil; return bytes
    }
    mutating func readString() throws -> String {
        guard let text = String(data: try readBytes(), encoding: .utf8) else { throw SteamError.invalidMessage }
        return text
    }
    mutating func skip(wireType: WireType) throws { value = nil }
}
extension SteamSession {
    var steamID: UInt64 { bSteamID }
    var cellID: UInt32 { bCellID }
    func ensureConnected() async throws { try await bConnect() }
    func callServiceMethodResult(method: SteamServiceMethod, body: Data, timeout: TimeInterval) async throws -> (eresult: Int, body: Data) {
        guard !TrackBSessionGate.reserved else { throw SteamError.disconnected }
        let replies = try await bRequest(151, body: body, name: method.rawValue, timeout: timeout, multipart: false)
        guard let reply = replies.first else { throw SteamError.invalidMessage }
        return (Int(clamping: reply.header.number(13) ?? 0), reply.body)
    }
}
extension SteamLibraryFetcher {
    func fetchAppInfo(appID: UInt32) async throws -> SteamAppInfo? { try await fetchInstallInfo(appID: appID) }
}
extension SteamAppInfo {
    var saveFiles: [SteamCloudSaveRule] {
        (bIntegrationCloudRoot?["savefiles"]?.fields ?? [:]).sorted { $0.key < $1.key }.prefix(1000).compactMap { _, entry in
            guard let root = entry["root"]?.string, let path = entry["path"]?.string else { return nil }
            let platforms = (entry["platforms"]?.fields.values ?? Dictionary<String, SteamValue>().values).compactMap { $0.string?.lowercased() }
            return SteamCloudSaveRule(root: root, path: path, pattern: entry["pattern"]?.string ?? "*", recursive: entry["recursive"]?.string == "1", platforms: platforms)
        }
    }
    var rootOverrides: [SteamCloudPaths.RootOverride] {
        (bIntegrationCloudRoot?["rootoverrides"]?.fields ?? [:]).sorted { $0.key < $1.key }.prefix(1000).compactMap { _, entry in
            guard let root = entry["root"]?.string, let os = entry["os"]?.string, let replacement = entry["useinstead"]?.string else { return nil }
            return SteamCloudPaths.RootOverride(root: root, os: os, useInstead: replacement, addPath: entry["addpath"]?.string ?? "")
        }
    }
}
