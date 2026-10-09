// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// A DRM-free Windows game the user added (GOG, itch.io, a copy of an installed game): one
/// folder under Documents/Games and the program in it that starts the game. Files shows
/// Documents as On My iPhone › MYIOSDECK, so a folder dropped into its Games folder appears
/// here without a copy; Wine sees the folder as C:\Games (a link in the prefix's drive_c).
struct CustomGame: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var folder: String       // folder name under Documents/Games
    var exe: String          // the program inside the folder, "/"-separated
    var arguments = ""

    var windowsExe: String { CustomGames.windowsRoot + "\\" + folder + "\\" + exe.replacingOccurrences(of: "/", with: "\\") }
    /// The program's own folder: games load their data relative to it.
    var windowsWorkdir: String {
        let dir = (exe as NSString).deletingLastPathComponent
        return CustomGames.windowsRoot + "\\" + folder + (dir.isEmpty ? "" : "\\" + dir.replacingOccurrences(of: "/", with: "\\"))
    }
}

/// A Windows program found in a game folder, with its CPU (0x8664 x64, 0x14c 32-bit x86).
struct GameProgram: Identifiable, Hashable {
    var id: String { path }
    let path: String         // inside the game folder, "/"-separated
    let size: Int
    let machine: UInt16
    var kind: String { machine == 0x8664 ? "64-bit" : machine == 0x14c ? "32-bit" : "?" }
}

@MainActor final class CustomGames: ObservableObject {
    static let shared = CustomGames()
    nonisolated static let windowsRoot = "C:\\Games"

    @Published private(set) var games: [CustomGame] = []
    /// Folders in Games that have no program to start, with why (e.g. only a GOG installer).
    @Published private(set) var unusable: [String: String] = [:]
    @Published private(set) var copying: String?

    nonisolated static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    nonisolated static var root: URL { documents.appendingPathComponent("Games", isDirectory: true) }
    private var storeURL: URL { Self.documents.appendingPathComponent("custom-games.json") }

    private init() {
        if let data = try? Data(contentsOf: storeURL), let list = try? JSONDecoder().decode([CustomGame].self, from: data) {
            games = list
        }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(games) { try? data.write(to: storeURL, options: .atomic) }
    }

    /// Documents/Games, and drive_c/Games in the prefix as a relative link to it (the app's
    /// container path changes between installs, a relative link does not).
    nonisolated static func prepare(prefix: URL) {
        prefix.path.withCString { mid_wine_seed_prefix($0) }   // a first start: drive_c exists before the link
        let fm = FileManager.default
        try? fm.createDirectory(at: root, withIntermediateDirectories: true)
        let driveC = prefix.appendingPathComponent("drive_c", isDirectory: true)
        guard fm.fileExists(atPath: driveC.path) else { return }
        let link = driveC.appendingPathComponent("Games")
        if (try? fm.destinationOfSymbolicLink(atPath: link.path)) != nil || fm.fileExists(atPath: link.path) { return }
        do {
            try fm.createSymbolicLink(atPath: link.path, withDestinationPath: "../../Games")
            dlog("[games] C:\\Games -> Documents/Games")
        } catch {
            dlog("[games] could not link C:\\Games: \(error.localizedDescription)")
        }
    }

    /// Brings the list in line with Documents/Games: a new folder is added with its likeliest
    /// program, a removed one leaves the list.
    func refresh(prefix: URL) {
        Self.prepare(prefix: prefix)
        let fm = FileManager.default
        let folders = ((try? fm.contentsOfDirectory(at: Self.root, includingPropertiesForKeys: [.isDirectoryKey])) ?? [])
            .filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true && !$0.lastPathComponent.hasPrefix(".") }
            .map(\.lastPathComponent)
        var list = games.filter { folders.contains($0.folder) }
        var why: [String: String] = [:]
        for folder in folders.sorted() where !list.contains(where: { $0.folder == folder }) {
            let programs = Self.programs(in: folder)
            if let best = programs.first {
                list.append(CustomGame(name: folder, folder: folder, exe: best.path))
                dlog("[games] added \(folder): \(best.path) (\(best.kind))")
            } else {
                why[folder] = Self.hasInstallerOnly(folder)
                    ? "Only a GOG installer is in this folder. Install the game on a PC (or unpack the installer with innoextract) and copy the installed folder here."
                    : "No Windows program (.exe) in this folder."
            }
        }
        if list != games { games = list; save() }
        unusable = why
    }

    func update(_ game: CustomGame) {
        guard let i = games.firstIndex(where: { $0.id == game.id }) else { return }
        games[i] = game
        save()
    }

    /// Removes the game from the list; with files, also its folder in Documents/Games.
    func remove(_ game: CustomGame, files: Bool) {
        games.removeAll { $0.id == game.id }
        save()
        if files { try? FileManager.default.removeItem(at: Self.root.appendingPathComponent(game.folder, isDirectory: true)) }
    }

    /// Copies a game folder picked in Files (iCloud Drive, another app, a USB drive) into
    /// Documents/Games. Folders already in Documents/Games need no copy.
    func importFolder(_ picked: URL, prefix: URL) {
        let fm = FileManager.default
        var name = picked.lastPathComponent, n = 2
        while fm.fileExists(atPath: Self.root.appendingPathComponent(name).path) { name = "\(picked.lastPathComponent) \(n)"; n += 1 }
        let finalName = name, dest = Self.root.appendingPathComponent(name, isDirectory: true)
        copying = "Copying \(picked.lastPathComponent)…"
        Task.detached {
            let scoped = picked.startAccessingSecurityScopedResource()
            defer { if scoped { picked.stopAccessingSecurityScopedResource() } }
            var failure: String?
            do {
                try FileManager.default.createDirectory(at: CustomGames.root, withIntermediateDirectories: true)
                try FileManager.default.copyItem(at: picked, to: dest)
            } catch { failure = error.localizedDescription }
            await MainActor.run {
                self.copying = failure.map { "Could not copy the folder: \($0)" }
                dlog("[games] import \(picked.lastPathComponent) -> Games/\(finalName):\(failure ?? "ok")")
                self.refresh(prefix: prefix)
            }
        }
    }

    // MARK: - Finding the program

    nonisolated private static let skipFolders: Set<String> = ["__redist", "_redist", "redist", "_commonredist", "commonredist",
                                                   "directx", "dotnet", "vcredist", "__support", "support", "__installer"]
    nonisolated private static let skipPrefixes = ["unins", "setup", "vc_redist", "vcredist", "dxsetup", "dxwebsetup", "dotnet", "ndp",
                                       "crashreport", "crashhandler", "unitycrashhandler", "ue4prereq", "ueprereq", "oalinst",
                                       "physx", "installer", "gog galaxy", "goggalaxy"]

    /// Every program in a game folder (up to 4 folders deep), likeliest first: not an
    /// installer, uninstaller, runtime or crash reporter; shallow; named like the folder; large.
    nonisolated static func programs(in folder: String) -> [GameProgram] {
        let base = root.appendingPathComponent(folder, isDirectory: true)
        let basePath = base.resolvingSymlinksInPath().path
        guard let walker = FileManager.default.enumerator(at: base, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey]) else { return [] }
        var found: [(GameProgram, Int)] = []
        let folderKey = folder.lowercased().filter(\.isLetter)
        for case let url as URL in walker {
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey])
            let rel = String(url.resolvingSymlinksInPath().path.dropFirst(basePath.count + 1))
            let depth = rel.split(separator: "/").count
            if values?.isDirectory == true {
                if depth >= 4 || skipFolders.contains(url.lastPathComponent.lowercased()) { walker.skipDescendants() }
                continue
            }
            let name = url.lastPathComponent.lowercased()
            guard name.hasSuffix(".exe"), !skipPrefixes.contains(where: { name.hasPrefix($0) }) else { continue }
            let machine = peMachine(url)
            guard machine == 0x8664 || machine == 0x14c else { continue }
            let size = values?.fileSize ?? 0
            var score = depth * 100 - min(size / 1_000_000, 90)
            if !folderKey.isEmpty, name.filter(\.isLetter).hasPrefix(String(folderKey.prefix(5))) { score -= 150 }
            if name.contains("launcher") || name.contains("config") || name.contains("settings") || name.contains("editor") { score += 120 }
            found.append((GameProgram(path: rel, size: size, machine: machine), score))
        }
        return found.sorted { $0.1 < $1.1 }.map(\.0)
    }

    nonisolated private static func hasInstallerOnly(_ folder: String) -> Bool {
        let base = root.appendingPathComponent(folder, isDirectory: true)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: base.path)) ?? []
        return names.contains { $0.lowercased().hasPrefix("setup_") && $0.lowercased().hasSuffix(".exe") }
    }

    /// IMAGE_FILE_HEADER.Machine (0 when not a PE file).
    nonisolated static func peMachine(_ url: URL) -> UInt16 {
        guard let h = try? FileHandle(forReadingFrom: url) else { return 0 }
        defer { try? h.close() }
        guard let d = try? h.read(upToCount: 4096), d.count >= 0x40, d[0] == 0x4d, d[1] == 0x5a else { return 0 }
        let pe = Int(d[0x3c]) | Int(d[0x3d]) << 8 | Int(d[0x3e]) << 16 | Int(d[0x3f]) << 24
        guard pe > 0, pe + 6 <= d.count, d[pe] == 0x50, d[pe + 1] == 0x45 else { return 0 }
        return UInt16(d[pe + 4]) | UInt16(d[pe + 5]) << 8
    }
}
