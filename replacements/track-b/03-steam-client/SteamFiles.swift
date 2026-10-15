// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

indirect enum SteamValue: Sendable {
    case text(String)
    case object([String: SteamValue])
    var fields: [String: SteamValue] { if case .object(let value) = self { return value }; return [:] }
    var string: String? { if case .text(let value) = self { return value }; return nil }
    subscript(_ key: String) -> SteamValue? { fields[key.lowercased()] ?? fields.first(where:{ $0.key.caseInsensitiveCompare(key) == .orderedSame })?.value }
}
enum SteamFileError: LocalizedError {
    case invalid(String)
    var errorDescription: String? { if case .invalid(let why) = self { return "Could not read the Steam file: \(why)." }; return nil }
}
struct SteamKeyValues {
    private enum Token { case word(String), open, close }
    private var bytes: [UInt8]
    private var position = 0
    private var tokens = 0
    init(_ data: Data) throws {
        guard data.count <= 4 * 1024 * 1024 else { throw SteamFileError.invalid("too large") }
        bytes = Array(data)
        if bytes.starts(with: [0xef,0xbb,0xbf]) { position = 3 }
    }
    private mutating func token() throws -> Token? {
        while position < bytes.count {
            let c = bytes[position]
            if c == 32 || c == 9 || c == 10 || c == 13 { position += 1; continue }
            if c == 47 && position + 1 < bytes.count && bytes[position + 1] == 47 {
                while position < bytes.count && bytes[position] != 10 { position += 1 }; continue
            }
            break
        }
        guard position < bytes.count else { return nil }
        tokens += 1
        guard tokens <= 100_000 else { throw SteamFileError.invalid("too large") }
        let first = bytes[position]; position += 1
        if first == 123 { return .open }; if first == 125 { return .close }
        var value: [UInt8] = []
        var closed = first != 34
        if first != 34 { value.append(first) }
        while position < bytes.count {
            let c = bytes[position]
            if first == 34 {
                position += 1
                if c == 34 { closed = true; break }
                if c == 92 && position < bytes.count && (bytes[position] == 34 || bytes[position] == 92) {
                    value.append(bytes[position]); position += 1
                } else { value.append(c) }
            } else {
                if c == 32 || c == 9 || c == 10 || c == 13 || c == 123 || c == 125 || (c == 47 && position + 1 < bytes.count && bytes[position+1] == 47) { break }
                value.append(c); position += 1
            }
            guard value.count <= 16_384 else { throw SteamFileError.invalid("too large") }
        }
        guard closed else { throw SteamFileError.invalid("incomplete file") }
        guard let text = String(bytes: value, encoding: .utf8) else { throw SteamFileError.invalid("invalid text") }
        return .word(text)
    }
    private mutating func object(depth: Int, nested: Bool) throws -> SteamValue {
        guard depth <= 32 else { throw SteamFileError.invalid("too deep") }
        var fields: [String: SteamValue] = [:]
        while let next = try token() {
            if case .close = next { if nested { return .object(fields) }; throw SteamFileError.invalid("unexpected }") }
            guard case .word(let key) = next, let value = try token() else { throw SteamFileError.invalid("incomplete file") }
            switch value {
            case .open: fields[key.lowercased()] = try object(depth: depth + 1, nested: true)
            case .word(let text): fields[key.lowercased()] = .text(text)
            case .close: throw SteamFileError.invalid("incomplete file")
            }
        }
        if nested { throw SteamFileError.invalid("incomplete file") }
        return .object(fields)
    }
    mutating func read() throws -> SteamValue { try object(depth: 0, nested: false) }
}

enum SteamInstallPaths {
    static let libraryRelative = "Program Files (x86)/Steam/steamapps"
    static func steamApps(drive: URL) -> URL { drive.appendingPathComponent(libraryRelative, isDirectory: true) }
    static func common(drive: URL) -> URL { steamApps(drive: drive).appendingPathComponent("common", isDirectory: true) }
    static func isManaged(library: String) -> Bool {
        library.replacingOccurrences(of: "\\", with: "/").trimmingCharacters(in: CharacterSet(charactersIn: "/")).caseInsensitiveCompare(libraryRelative) == .orderedSame
    }
}
enum SteamInstallFiles {
    static func bRecord(appID: Int, steamApps: URL) -> SteamValue? {
        guard appID >= 0 else { return nil }
        let url = steamApps.appendingPathComponent("appmanifest_\(appID).acf")
        guard let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize, size <= 1024 * 1024,
              let data = try? Data(contentsOf: url), data.count <= 1024 * 1024,
              var parser = try? SteamKeyValues(data), let root = try? parser.read(), let state = root["appstate"],
              state["appid"]?.string.flatMap(Int.init) == appID else { return nil }
        return state
    }
    static func buildID(appID: Int, steamApps: URL) -> Int? {
        guard let n = bRecord(appID:appID,steamApps:steamApps)?["buildid"]?.string.flatMap(Int.init), n > 0 else { return nil }; return n
    }
    static func sizeOnDisk(appID: Int, steamApps: URL) -> Int64? {
        guard let n = bRecord(appID: appID, steamApps: steamApps)?["sizeondisk"]?.string.flatMap(Int64.init), n > 0 else { return nil }; return n
    }
    nonisolated static func safeFolderName(_ name: String) -> String {
        let tail = name.components(separatedBy: CharacterSet(charactersIn: "/\\")).last ?? ""
        if tail.isEmpty || tail == "." || tail == ".." || tail.contains(":") || tail.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) { return "app" }
        return tail
    }
    nonisolated static func delete(appID: Int, folderName: String, steamApps: URL) {
        guard appID >= 0 else { return }
        let folder = safeFolderName(folderName)
        let common = steamApps.appendingPathComponent("common", isDirectory: true).resolvingSymlinksInPath().standardizedFileURL
        let candidate = common.appendingPathComponent(folder).resolvingSymlinksInPath().standardizedFileURL
        if candidate.deletingLastPathComponent() == common { try? FileManager.default.removeItem(at: candidate) }
        if let record = bRecord(appID: appID, steamApps: steamApps) {
            let owners = Set((record["shareddepots"]?.fields ?? [:]).values.compactMap { $0.string.flatMap(Int.init) }).sorted().prefix(64)
            for id in owners where id != appID && id >= 0 {
                if let other = bRecord(appID: id, steamApps: steamApps),
                   safeFolderName(other["installdir"]?.string ?? "").caseInsensitiveCompare(folder) == .orderedSame {
                    try? FileManager.default.removeItem(at: steamApps.appendingPathComponent("appmanifest_\(id).acf"))
                }
            }
        }
        try? FileManager.default.removeItem(at: steamApps.appendingPathComponent("appmanifest_\(appID).acf"))
        try? FileManager.default.removeItem(at: steamApps.appendingPathComponent("downloading/\(appID)"))
    }
}
enum SteamDirectStart {
    static let mode = "game"
    struct Choice: Equatable { var program: String; var arguments: String; var folder: String?; var launchIndex: UInt32? }
    static func bRelative(_ raw: String) -> [String]? {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: "\\", with: "/")
        guard text.utf8.count <= 512, !text.hasPrefix("/"),
              !text.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) || "<>:\"|?*".unicodeScalars.contains($0) }) else { return nil }
        let parts = text.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard !parts.contains("..") else { return nil }
        return parts.filter { !$0.isEmpty && $0 != "." }
    }
    static func bFind(_ parts: [String], root: URL, directory: Bool) -> String? {
        let base = root.resolvingSymlinksInPath().standardizedFileURL
        var current = base, spelling: [String] = []
        for (index, part) in parts.enumerated() {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: current.path)) ?? []
            let name = names.contains(part) ? part : names.sorted().first { $0.caseInsensitiveCompare(part) == .orderedSame }
            guard let name = name else { return nil }
            current.appendPathComponent(name); spelling.append(name)
            let resolved = current.resolvingSymlinksInPath().standardizedFileURL
            guard resolved.path == base.path || resolved.path.hasPrefix(base.path + "/") else { return nil }
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: current.path, isDirectory: &isDirectory),
                  isDirectory.boolValue == (index < parts.count - 1 || directory) else { return nil }
        }
        if parts.isEmpty && !directory { return nil }
        return spelling.joined(separator: "/")
    }
    static func choose(_ options: [SteamLaunchOption], installFolder root: URL) -> Choice? {
        let excluded: Set<String> = ["server","editor","vr","othervr","openvroverlay","osvr","manual"]
        func rank(_ option: SteamLaunchOption) -> (Int, Int) {
            let t = option.type.lowercased(), a = option.osarch.lowercased()
            return (t == "default" ? 0 : (t.isEmpty || t == "none" ? 1 : 2), a == "64" ? 0 : a.isEmpty ? 1 : 2)
        }
        let candidates = options.enumerated().filter { _, o in
            o.betaKey.isEmpty && !excluded.contains(o.type.lowercased()) && (o.requiredDLC == nil || o.requiredDLC == "" || o.requiredDLC == "0") && BSteamDepotSelection.windows(o.oslist)
        }.sorted { lhs, rhs in
            let l = rank(lhs.element), r = rank(rhs.element)
            if l.0 != r.0 { return l.0 < r.0 }; if l.1 != r.1 { return l.1 < r.1 }; return lhs.offset < rhs.offset
        }
        for (_, option) in candidates {
            guard let parts = bRelative(option.executable), let last = parts.last, last.lowercased().hasSuffix(".exe"),
                  let executable = bFind(parts, root: root, directory: false) else { continue }
            var folder: String?
            if !option.workingDir.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                guard let parts = bRelative(option.workingDir), let found = bFind(parts, root: root, directory: true) else { continue }
                folder = found
            }
            return Choice(program: executable, arguments: option.arguments.trimmingCharacters(in: .whitespacesAndNewlines), folder: folder, launchIndex: option.index)
        }
        return nil
    }
}
