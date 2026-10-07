// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 125hz
// Madeira Converter Exception: see LICENSE-EXCEPTION.md

// Vendored unchanged from Madeira app/Madeira/SteamGames.swift (SteamDirectStart.choose
// and its path helpers) at commit 65e6fe8f; see SwiftSteam/README.md.

import Foundation

/// "Start with: The game" on a Steam game's Game details page: the game's own
/// program runs in Wine without Steam, which suits games that do not need Steam
/// (DRM-free ones). Which program is Steam's own launch configuration for the app
/// (its product info's `config.launch`), never a list of program names; when that
/// names nothing that can run here, the user picks one of the install folder's
/// programs. Madeira Dock stays the default.
enum SteamDirectStart {
    /// LibraryEntry.steamStart for this start; nil there is Madeira Dock.
    static let mode = "game"

    /// What "The game" starts: the program, relative to the install folder and
    /// spelt as on disk, its arguments, and its working folder (nil: the program's
    /// own folder; "": the install folder).
    struct Choice: Equatable {
        var program: String
        var arguments: String
        var folder: String?
        var launchIndex: UInt32? = nil
    }

    /// Launch types Steam gives entries that are not the game itself.
    static let otherKinds: Set<String> = ["server", "editor", "vr", "othervr", "openvroverlay", "osvr", "manual"]

    /// A path from Steam's launch configuration in slash form (bin/game.exe), or nil
    /// for anything that could leave the install folder or is not a plain name: an
    /// absolute path, a drive, "..", a control or reserved character. "." parts go.
    static func relativePath(_ raw: String) -> String? {
        let text = raw.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "\\", with: "/")
        guard !text.hasPrefix("/"), text.utf8.count <= 512,
              !text.unicodeScalars.contains(where: { $0.value < 0x20 || "<>:\"|?*".unicodeScalars.contains($0) }) else { return nil }
        var parts: [Substring] = []
        for part in text.split(separator: "/") where part != "." {
            if part == ".." { return nil }
            parts.append(part)
        }
        return parts.joined(separator: "/")
    }

    /// `relative` found under `root` one name at a time, exactly or else without case
    /// (as Windows finds it): its spelling on disk, or nil when a name is missing, the
    /// last one is not of the kind asked for, or the result leaves `root`.
    static func onDisk(_ relative: String, in root: URL, directory: Bool) -> String? {
        let fm = FileManager.default
        var url = root
        var spelled: [String] = []
        for part in relative.split(separator: "/").map(String.init) {
            guard let names = try? fm.contentsOfDirectory(atPath: url.path),
                  let name = names.first(where: { $0 == part }) ?? names.first(where: { $0.caseInsensitiveCompare(part) == .orderedSame })
            else { return nil }
            url.appendPathComponent(name)
            spelled.append(name)
        }
        var isDirectory: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue == directory else { return nil }
        let base = root.resolvingSymlinksInPath().path
        let target = url.resolvingSymlinksInPath().path
        guard target == base ? directory : target.hasPrefix(base + "/") else { return nil }
        return spelled.joined(separator: "/")
    }

    /// The launch entry "The game" starts. Candidates are the Windows entries (no
    /// platform list, or one naming Windows) outside beta branches and of a kind that is
    /// the game itself; Steam's default comes first ("default", then no type or "none",
    /// then the other options), each in Steam's order with a 64-bit entry before one
    /// for any architecture before a 32-bit one. The first candidate whose program is a
    /// Windows program (.exe) inside the install folder, and whose working folder (when
    /// it names one) exists there, is taken.
    static func choose(_ options: [SteamLaunchOption], installFolder root: URL) -> Choice? {
        func rank(_ option: SteamLaunchOption) -> Int? {
            let type = option.type.lowercased()
            guard option.betaKey.isEmpty, !otherKinds.contains(type),
                  option.requiredDLC == nil || option.requiredDLC == "" || option.requiredDLC == "0",
                  option.oslist.isEmpty || option.oslist.lowercased().contains("windows") else { return nil }
            let kind: Int
            if type == "default" { kind = 0 } else if type.isEmpty || type == "none" { kind = 1 } else { kind = 2 }
            let arch: Int
            if option.osarch == "64" { arch = 0 } else if option.osarch.isEmpty { arch = 1 } else { arch = 2 }
            return kind * 3 + arch
        }
        var ranked: [(rank: Int, index: Int, option: SteamLaunchOption)] = []
        for (index, option) in options.enumerated() {
            if let value = rank(option) { ranked.append((rank: value, index: index, option: option)) }
        }
        ranked.sort { a, b in a.rank != b.rank ? a.rank < b.rank : a.index < b.index }
        let spaces = CharacterSet.whitespaces
        for entry in ranked {
            let option = entry.option
            guard let path = relativePath(option.executable), !path.isEmpty,
                  URL(fileURLWithPath: path).pathExtension.lowercased() == "exe",
                  let program = onDisk(path, in: root, directory: false) else { continue }
            var folder: String? = nil
            if !option.workingDir.trimmingCharacters(in: spaces).isEmpty {
                guard let relative = relativePath(option.workingDir) else { continue }
                if relative.isEmpty {
                    folder = ""
                } else {
                    guard let found = onDisk(relative, in: root, directory: true) else { continue }
                    folder = found
                }
            }
            return Choice(program: program, arguments: option.arguments.trimmingCharacters(in: spaces), folder: folder,
                          launchIndex: option.index)
        }
        return nil
    }
}
