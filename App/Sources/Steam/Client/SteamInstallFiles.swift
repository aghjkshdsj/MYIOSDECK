// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Steam files on disk and the direct-start choice (spec 03 §K2, §K3), plus the path checks
// and directory-name folding the downloader uses (§D5).
//
// Sources: spec 03 §K, §D5; POSIX realpath(3); Apple Foundation FileManager.

import Foundation

// MARK: - Paths

enum SteamInstallPaths {
    /// "Program Files (x86)/Steam/steamapps", relative to drive_c.
    static let libraryRelative = "Program Files (x86)/Steam/steamapps"

    nonisolated static func steamApps(drive: URL) -> URL {
        drive.appendingPathComponent("Program Files (x86)", isDirectory: true)
            .appendingPathComponent("Steam", isDirectory: true)
            .appendingPathComponent("steamapps", isDirectory: true)
    }

    nonisolated static func common(drive: URL) -> URL {
        steamApps(drive: drive).appendingPathComponent("common", isDirectory: true)
    }

    /// Whether `library` names our Steam library. Backslashes count as slashes, a leading
    /// drive letter and outer slashes are ignored, and case doesn't matter.
    nonisolated static func isManaged(library: String) -> Bool {
        var s = library.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "\\", with: "/")
        if s.count >= 2, s.dropFirst().first == ":", s.first?.isLetter == true { s = String(s.dropFirst(2)) }
        while s.hasPrefix("/") { s.removeFirst() }
        while s.hasSuffix("/") { s.removeLast() }
        return s.caseInsensitiveCompare(libraryRelative) == .orderedSame
    }
}

/// Path rules for names that come from Steam (§D5) and from launch options (§K3).
enum SteamPathRules {
    nonisolated static func hasControlCharacter(_ s: String) -> Bool {
        s.unicodeScalars.contains { $0.value < 0x20 || $0.value == 0x7F }
    }

    /// A manifest path ('/'-separated) split into checked parts, or nil: at most 64 parts and
    /// 1024 bytes; no empty, "." or ".." part; no ":"; no control characters.
    nonisolated static func manifestComponents(_ path: String) -> [String]? {
        guard !path.isEmpty, path.utf8.count <= 1024 else { return nil }
        let parts = path.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count <= 64 else { return nil }
        for p in parts {
            if p.isEmpty || p == "." || p == ".." || p.contains(":") || hasControlCharacter(p) { return nil }
        }
        return parts
    }

    /// A launch-option path made relative (§K3): trimmed, '\' → '/', not absolute, at most 512
    /// bytes, no control characters or <>:"|?*, "." parts dropped, any ".." rejects it.
    nonisolated static func relativeComponents(_ raw: String) -> [String]? {
        let s = raw.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "\\", with: "/")
        guard s.utf8.count <= 512, !s.hasPrefix("/"), !hasControlCharacter(s) else { return nil }
        if s.contains(where: { "<>:\"|?*".contains($0) }) { return nil }
        let parts = s.split(separator: "/", omittingEmptySubsequences: true).map(String.init).filter { $0 != "." }
        if parts.contains("..") { return nil }
        return parts
    }

    /// realpath(3), or nil when the path doesn't exist.
    nonisolated static func realPath(_ url: URL) -> String? {
        guard let resolved = realpath(url.path, nil) else { return nil }
        defer { free(resolved) }
        return String(cString: resolved)
    }

    nonisolated static func isInside(_ path: String, root: String) -> Bool {
        path == root || path.hasPrefix(root.hasSuffix("/") ? root : root + "/")
    }
}

/// One spelling per directory, ignoring case (§D5, clarification 16). A directory that already
/// exists under the install folder keeps its on-disk spelling; otherwise the first spelling
/// seen in this install is used for every later path with the same lower-cased directory path.
struct SteamDirectoryFolding {
    private var spelling: [String: String] = [:]    // lower-cased directory path → its last part
    private var listings: [String: [String]] = [:]  // folded parent path → its subdirectories on disk
    private let root: URL?

    /// `root`: the install folder whose existing directories win; nil folds within the paths only.
    nonisolated init(root: URL? = nil) {
        self.root = root
    }

    nonisolated mutating func fold(_ parts: [String], isDirectory: Bool) -> [String] {
        let directoryCount = isDirectory ? parts.count : parts.count - 1
        var out: [String] = []
        out.reserveCapacity(parts.count)
        var lowered = ""
        for i in 0..<max(directoryCount, 0) {
            lowered += (i == 0 ? "" : "/") + parts[i].lowercased()
            if let known = spelling[lowered] {
                out.append(known)
            } else {
                let chosen = existingSpelling(parent: out.joined(separator: "/"), name: parts[i]) ?? parts[i]
                spelling[lowered] = chosen
                out.append(chosen)
            }
        }
        if !isDirectory, let last = parts.last { out.append(last) }
        return out
    }

    /// The on-disk name of a subdirectory of `parent` equal to `name` ignoring case: an exact
    /// match first, else the first in sorted order.
    nonisolated private mutating func existingSpelling(parent: String, name: String) -> String? {
        guard let root else { return nil }
        let entries: [String]
        if let cached = listings[parent] {
            entries = cached
        } else {
            let fm = FileManager.default
            let folder = parent.isEmpty ? root : root.appendingPathComponent(parent, isDirectory: true)
            let names = (try? fm.contentsOfDirectory(atPath: folder.path)) ?? []
            entries = names.filter { entry in
                var isDir: ObjCBool = false
                return fm.fileExists(atPath: folder.appendingPathComponent(entry).path, isDirectory: &isDir) && isDir.boolValue
            }.sorted()
            listings[parent] = entries
        }
        if entries.contains(name) { return name }
        return entries.first { $0.caseInsensitiveCompare(name) == .orderedSame }
    }
}

// MARK: - Install records and uninstall (§K2)

enum SteamInstallFiles {
    /// `appmanifest_<id>.acf` (at most 1 MB): its AppState block, if its appid is `appID`.
    nonisolated static func record(appID: Int, steamApps: URL) -> SteamValue? {
        let url = steamApps.appendingPathComponent("appmanifest_\(appID).acf")
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        let limit = 1 << 20
        guard let data = try? handle.read(upToCount: limit + 1), data.count <= limit else { return nil }
        guard var reader = try? SteamKeyValues(data), let root = try? reader.read(),
              let state = root["AppState"],
              state["appid"]?.string?.trimmingCharacters(in: .whitespaces) == String(appID) else { return nil }
        return state
    }

    nonisolated static func buildID(appID: Int, steamApps: URL) -> Int? {
        guard let s = record(appID: appID, steamApps: steamApps)?["buildid"]?.string else { return nil }
        return Int(s.trimmingCharacters(in: .whitespaces))
    }

    nonisolated static func sizeOnDisk(appID: Int, steamApps: URL) -> Int64? {
        guard let s = record(appID: appID, steamApps: steamApps)?["SizeOnDisk"]?.string,
              let n = Int64(s.trimmingCharacters(in: .whitespaces)), n > 0 else { return nil }
        return n
    }

    /// Keeps only the last non-empty component after splitting on '/' and '\' (so "Game/" is
    /// "Game"). No component, ".", "..", or a name with ':' or a control character is "app".
    nonisolated static func safeFolderName(_ name: String) -> String {
        let last = name.split(omittingEmptySubsequences: true) { $0 == "/" || $0 == "\\" }.last.map(String.init) ?? ""
        if last.isEmpty || last == "." || last == ".." || last.contains(":") || SteamPathRules.hasControlCharacter(last) {
            return "app"
        }
        return last
    }

    nonisolated static func delete(appID: Int, folderName: String, steamApps: URL) {
        let fm = FileManager.default
        let common = steamApps.appendingPathComponent("common", isDirectory: true)
        let safe = safeFolderName(folderName)
        let folder = common.appendingPathComponent(safe, isDirectory: true)

        var removedFolder = false
        if let commonReal = SteamPathRules.realPath(common), let folderReal = SteamPathRules.realPath(folder),
           (folderReal as NSString).deletingLastPathComponent == commonReal, folderReal != commonReal {
            removedFolder = (try? fm.removeItem(at: folder)) != nil
        }

        // Owner apps whose shared depots live in the same folder.
        var ownersRemoved = 0
        if let shared = record(appID: appID, steamApps: steamApps)?["SharedDepots"]?.fields {
            var seen = Set<Int>()
            for value in shared.values {
                guard let owner = Int(value.string?.trimmingCharacters(in: .whitespaces) ?? ""),
                      owner != appID, !seen.contains(owner) else { continue }
                seen.insert(owner)
                if seen.count > 64 { break }
                guard let ownerFolder = record(appID: owner, steamApps: steamApps)?["installdir"]?.string,
                      safeFolderName(ownerFolder).caseInsensitiveCompare(safe) == .orderedSame else { continue }
                if (try? fm.removeItem(at: steamApps.appendingPathComponent("appmanifest_\(owner).acf"))) != nil {
                    ownersRemoved += 1
                }
            }
        }

        try? fm.removeItem(at: steamApps.appendingPathComponent("appmanifest_\(appID).acf"))
        try? fm.removeItem(at: steamApps.appendingPathComponent("downloading", isDirectory: true)
            .appendingPathComponent(String(appID), isDirectory: true))
        SteamLog.event("[steam-depot] uninstall app=\(appID) folder=\(removedFolder ? "removed" : "kept") owner-records=\(ownersRemoved)")
    }
}

// MARK: - Direct start (§K3)

/// The program a direct start of a Steam game runs.
enum SteamDirectStart {
    static let mode = "game"

    struct Choice: Equatable {
        var program: String           // relative to the install folder, spelt as on disk
        var arguments: String
        var folder: String?           // nil: the program's own folder; "": the install folder
        var launchIndex: UInt32?
    }

    nonisolated private static let excludedTypes: Set<String> =
        ["server", "editor", "vr", "othervr", "openvroverlay", "osvr", "manual"]

    nonisolated static func isCandidate(_ o: SteamLaunchOption) -> Bool {
        let type = o.type.trimmingCharacters(in: .whitespaces).lowercased()
        let dlc = o.requiredDLC?.trimmingCharacters(in: .whitespaces) ?? ""
        return o.betaKey.trimmingCharacters(in: .whitespaces).isEmpty
            && !excludedTypes.contains(type)
            && (dlc.isEmpty || dlc == "0")
            && SteamOSList.includesWindows(o.oslist)
    }

    /// Candidates in the order they are tried: type "default", then empty or "none", then any
    /// other; within each, osarch "64", then empty, then any other; ties keep Steam's order.
    nonisolated static func ordered(_ options: [SteamLaunchOption]) -> [SteamLaunchOption] {
        func typeRank(_ o: SteamLaunchOption) -> Int {
            switch o.type.trimmingCharacters(in: .whitespaces).lowercased() {
            case "default": return 0
            case "", "none": return 1
            default: return 2
            }
        }
        func archRank(_ o: SteamLaunchOption) -> Int {
            switch o.osarch.trimmingCharacters(in: .whitespaces) {
            case "64": return 0
            case "": return 1
            default: return 2
            }
        }
        return options.enumerated()
            .filter { isCandidate($0.element) }
            .sorted { a, b in
                let ka = (typeRank(a.element), archRank(a.element), a.offset)
                let kb = (typeRank(b.element), archRank(b.element), b.offset)
                return ka < kb
            }
            .map(\.element)
    }

    nonisolated static func choose(_ options: [SteamLaunchOption], installFolder root: URL) -> Choice? {
        guard let rootReal = SteamPathRules.realPath(root) else { return nil }
        for option in ordered(options) {
            guard let exeParts = SteamPathRules.relativeComponents(option.executable), let name = exeParts.last,
                  name.lowercased().hasSuffix(".exe"),
                  let program = locate(exeParts, under: root, rootReal: rootReal, directory: false) else { continue }
            var folder: String?
            if !option.workingDir.trimmingCharacters(in: .whitespaces).isEmpty {
                guard let dirParts = SteamPathRules.relativeComponents(option.workingDir),
                      let found = locate(dirParts, under: root, rootReal: rootReal, directory: true) else { continue }
                folder = found.joined(separator: "/")
            }
            return Choice(program: program.joined(separator: "/"),
                          arguments: option.arguments.trimmingCharacters(in: .whitespacesAndNewlines),
                          folder: folder,
                          launchIndex: option.index)
        }
        return nil
    }

    /// Finds `parts` under `root` one name at a time, exactly or else ignoring case. Returns the
    /// on-disk spelling when the result is a file (or a directory, if asked) that stays inside
    /// the folder after resolving symlinks.
    nonisolated static func locate(_ parts: [String], under root: URL, rootReal: String, directory: Bool) -> [String]? {
        let fm = FileManager.default
        var current = root
        var spelled: [String] = []
        for name in parts {
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: current.path, isDirectory: &isDir), isDir.boolValue else { return nil }
            // Compare against the listing, not fileExists: on a case-insensitive volume
            // fileExists would accept the requested spelling instead of the on-disk one.
            let entries = ((try? fm.contentsOfDirectory(atPath: current.path)) ?? []).sorted()
            let match: String
            if entries.contains(name) {
                match = name
            } else if let found = entries.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }) {
                match = found
            } else {
                return nil
            }
            spelled.append(match)
            current = current.appendingPathComponent(match)
        }
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: current.path, isDirectory: &isDir), isDir.boolValue == directory,
              let real = SteamPathRules.realPath(current), SteamPathRules.isInside(real, root: rootReal) else { return nil }
        if !directory, real == rootReal { return nil }
        return spelled
    }
}
