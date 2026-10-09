// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import UIKit

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

/// A GOG offline installer in a folder under Documents/Games: setup_<game>.exe (or a GOG
/// patch_*.exe) and the setup_<game>-N.bin parts next to it. The phone cannot run its window,
/// so innoextract (engine/innoextract, inno_unpack.h) unpacks the game files instead.
struct GameInstaller: Identifiable, Equatable {
    var id: String { folder + "/" + exe }
    let folder: String       // folder under Documents/Games that holds it
    let exe: String          // the installer's file name
    var url: URL { CustomGames.root.appendingPathComponent(folder, isDirectory: true).appendingPathComponent(exe) }
}

/// What mid_inno_inspect read from an installer's headers.
struct InstallerInfo: Decodable, Equatable {
    struct Part: Decodable, Equatable {
        let name: String
        let present: Bool
    }
    let ok: Bool
    var error: String?
    var appName: String?
    var defaultDirName: String?
    var innoVersion: String?
    var gogId: String?
    var embeddedData: Bool?
    var rarData: Bool?
    var encrypted: Bool?
    var appFiles: Int?
    var appSize: Int64?
    var languages: [String]?
    var parts: [Part]?

    var missing: [String] { (parts ?? []).filter { !$0.present }.map(\.name) }
    /// Why it cannot be unpacked here, if it cannot.
    var problem: String? {
        if !ok { return "MYIOSDECK cannot read this installer: \(error ?? "unknown error")." }
        if rarData == true {
            return "An older GOG installer: the game is in a RAR archive (the .bin file), which MYIOSDECK cannot unpack yet. Download the current offline installer from GOG, or install it on a PC."
        }
        if !missing.isEmpty {
            return "Missing \(missing.joined(separator: ", ")). Copy every part of the installer (the .exe and all its .bin files) into this folder, then tap Refresh."
        }
        if (appFiles ?? 0) == 0 { return "This installer has no game files to unpack." }
        return nil
    }
}

/// mid_inno_extract's answer.
private struct UnpackOutcome: Decodable {
    let ok: Bool
    var error: String?
    var cancelled: Bool?
    var warnings: Int?
    var log: [String]?
}

/// A finished unpack whose installer files the user may want to delete (only when they say so).
struct InstallerCleanup: Identifiable {
    var id: String { installer.id }
    let installer: GameInstaller
    let destination: String
    let files: [String]      // inside installer.folder
    let bytes: Int64
}

@MainActor final class CustomGames: ObservableObject {
    static let shared = CustomGames()
    nonisolated static let windowsRoot = "C:\\Games"

    @Published private(set) var games: [CustomGame] = []
    /// Folders in Games that have no program to start and no installer, with why.
    @Published private(set) var unusable: [String: String] = [:]
    @Published private(set) var copying: String?

    /// GOG installers in Games (a game's own folder holds its add-ons and patches), what their
    /// headers say, and the game folder each one was unpacked into (kept across launches).
    @Published private(set) var installers: [GameInstaller] = []
    @Published private(set) var installerInfo: [String: InstallerInfo] = [:]
    @Published private(set) var unpacked: [String: String] = [:]
    /// The unpack in progress: which installer, 0...1, and a line of detail.
    @Published private(set) var unpacking: String?
    @Published private(set) var unpackFraction = 0.0
    @Published private(set) var unpackDetail = ""
    /// The last unpack's failure (or a check that stopped it), shown on the card.
    @Published var unpackMessage: String?
    /// After a successful unpack: offer to delete the installer files.
    @Published var cleanup: InstallerCleanup?

    private var inspecting = Set<String>()
    private var unpackProgress: UnsafeMutablePointer<mid_inno_progress>?
    private var backgroundTask = UIBackgroundTaskIdentifier.invalid

    nonisolated static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    nonisolated static var root: URL { documents.appendingPathComponent("Games", isDirectory: true) }
    private var storeURL: URL { Self.documents.appendingPathComponent("custom-games.json") }
    private var unpackedURL: URL { Self.documents.appendingPathComponent("custom-games-unpacked.json") }

    private init() {
        if let data = try? Data(contentsOf: storeURL), let list = try? JSONDecoder().decode([CustomGame].self, from: data) {
            games = list
        }
        if let data = try? Data(contentsOf: unpackedURL), let map = try? JSONDecoder().decode([String: String].self, from: data) {
            unpacked = map
        }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(games) { try? data.write(to: storeURL, options: .atomic) }
    }

    private func saveUnpacked() {
        if let data = try? JSONEncoder().encode(unpacked) { try? data.write(to: unpackedURL, options: .atomic) }
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
        var found: [GameInstaller] = []
        for folder in folders.sorted() {
            let setups = Self.installers(in: folder)
            found += setups
            if list.contains(where: { $0.folder == folder }) { continue }
            if let task = Self.gogPlayTask(in: folder) {
                list.append(CustomGame(name: folder, folder: folder, exe: task.path, arguments: task.arguments))
                dlog("[games] added \(folder): \(task.path) (GOG play task)")
            } else if let best = Self.programs(in: folder).first {
                list.append(CustomGame(name: folder, folder: folder, exe: best.path))
                dlog("[games] added \(folder): \(best.path) (\(best.kind))")
            } else if setups.isEmpty {
                why[folder] = "No Windows program (.exe) in this folder."
            }
        }
        if list != games { games = list; save() }
        unusable = why
        if found != installers { installers = found }
        let gone = unpacked.keys.filter { id in !found.contains { $0.id == id } }
        if !gone.isEmpty { gone.forEach { unpacked.removeValue(forKey: $0) }; saveUnpacked() }
        inspectInstallers()
        Self.removeStaleStaging(except: unpacking)
    }

    // MARK: - GOG installers

    /// Reads every installer's headers again (cheap: a few KB), so parts copied in later count.
    /// Not while an unpack runs: the unpacker does one thing at a time.
    private func inspectInstallers() {
        guard unpacking == nil else { return }
        for installer in installers where !inspecting.contains(installer.id) {
            inspecting.insert(installer.id)
            let url = installer.url
            Task.detached {
                let info: InstallerInfo = CustomGames.decode(mid_inno_inspect(url.path))
                    ?? InstallerInfo(ok: false, error: "no answer from the unpacker")
                await MainActor.run {
                    self.inspecting.remove(installer.id)
                    if self.installerInfo[installer.id] != info {
                        self.installerInfo[installer.id] = info
                        dlog("[games] installer \(installer.id): " + CustomGames.describe(info))
                    }
                }
            }
        }
    }

    nonisolated private static func describe(_ info: InstallerInfo) -> String {
        guard info.ok else { return "unreadable: \(info.error ?? "?")" }
        let parts = info.parts ?? []
        var s = "\"\(info.appName ?? "?")\", Inno Setup \(info.innoVersion ?? "?"), GOG id \(info.gogId ?? "-"), "
        s += "\(info.appFiles ?? 0) files, \(mb(info.appSize ?? 0)), "
        s += info.embeddedData == true ? "data in the .exe" : "\(parts.count) .bin parts"
        if !info.missing.isEmpty { s += ", missing \(info.missing.joined(separator: " "))" }
        if info.rarData == true { s += ", RAR data (old format)" }
        return s
    }

    nonisolated private static func mb(_ bytes: Int64) -> String { String(format: "%.1f MB", Double(bytes) / 1_048_576) }

    /// Where an installer unpacks to: next to a game already in its folder (an add-on or patch),
    /// else Games/<GOG's install folder name>, which may be the installer's own folder.
    func destination(for installer: GameInstaller) -> String? {
        guard let info = installerInfo[installer.id], info.ok else { return nil }
        // refresh() lists every folder that has a program, so the list answers without a walk.
        if games.contains(where: { $0.folder == installer.folder }) { return installer.folder }
        return Self.gameFolderName(info, fallback: installer.folder)
    }

    /// The last part of DefaultDirName ("{autopf}\GOG Games\Beneath a Steel Sky"), else the
    /// app name, made safe as a folder name.
    nonisolated static func gameFolderName(_ info: InstallerInfo, fallback: String) -> String {
        var name = info.defaultDirName?.split(whereSeparator: { $0 == "\\" || $0 == "/" }).last.map(String.init) ?? ""
        if name.isEmpty || name.contains("{") { name = info.appName ?? "" }
        let bad = CharacterSet(charactersIn: "/\\:*?\"<>|").union(.controlCharacters)
        name = String(String.UnicodeScalarView(name.unicodeScalars.filter { !bad.contains($0) }))
        name = name.trimmingCharacters(in: CharacterSet.whitespaces.union(CharacterSet(charactersIn: ".")))
        return name.isEmpty ? fallback : name
    }

    /// Unpacks a GOG installer's game files ({app}) into its game folder, in the background.
    func unpack(_ installer: GameInstaller, prefix: URL) {
        guard unpacking == nil else { return }
        unpackMessage = nil
        guard let info = installerInfo[installer.id], let dest = destination(for: installer) else {
            unpackMessage = "The installer has not been read yet. Tap Refresh and try again."
            return
        }
        if let problem = info.problem { unpackMessage = problem; return }
        let need = (info.appSize ?? 0) + 200 * 1_048_576
        if let free = Self.freeSpace(), free < need {
            unpackMessage = "Not enough free space: unpacking needs \(Self.size(need)), the iPhone has \(Self.size(free)) free."
            dlog("[games] unpack \(installer.id): not enough space (needs \(Self.mb(need)), \(Self.mb(free)) free)")
            return
        }
        let language = Self.innoLanguage(info.languages ?? [])
        let staging = Self.root.appendingPathComponent(".unpack-\(UUID().uuidString)", isDirectory: true)
        let target = Self.root.appendingPathComponent(dest, isDirectory: true)
        let progress = UnsafeMutablePointer<mid_inno_progress>.allocate(capacity: 1)
        progress.initialize(to: mid_inno_progress(done: 0, total: UInt64(info.appSize ?? 0), cancel: 0))
        unpackProgress = progress
        unpacking = installer.id
        unpackFraction = 0
        unpackDetail = "Starting…"
        ScreenAwake.set("unpack", true)
        // A little time to finish if the user switches apps; past that iOS pauses the unpack
        // until they come back (the card asks them to keep MYIOSDECK open).
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "myiosdeck.unpack") { [weak self] in
            MainActor.assumeIsolated { self?.endBackgroundTask() }
        }
        let started = Date()
        dlog("[games] unpack \(installer.id) -> Games/\(dest): \(info.appFiles ?? 0) files, \(Self.mb(info.appSize ?? 0)), language \(language.isEmpty ? "-" : language), \(String(cString: mid_inno_version()))")

        Task { @MainActor in
            while self.unpacking == installer.id, let p = self.unpackProgress {
                let done = p.pointee.done, total = max(p.pointee.total, 1)
                self.unpackFraction = min(1, Double(done) / Double(total))
                self.unpackDetail = "\(CustomGames.size(Int64(done))) of \(CustomGames.size(Int64(total)))"
                try? await Task.sleep(nanoseconds: 250_000_000)
            }
        }

        Task.detached {
            let outcome: UnpackOutcome = CustomGames.decode(mid_inno_extract(installer.url.path, staging.path, language, progress))
                ?? UnpackOutcome(ok: false, error: "no answer from the unpacker")
            var problem: String?
            if !outcome.ok {
                problem = outcome.cancelled == true ? "Cancelled." : (outcome.error ?? "").isEmpty ? "unknown error" : outcome.error
            }
            var counted = (files: 0, bytes: Int64(0))
            if problem == nil {
                let app = staging.appendingPathComponent("app", isDirectory: true)
                if FileManager.default.fileExists(atPath: app.path) {
                    counted = CustomGames.tally(app)
                    do { try CustomGames.merge(app, into: target) } catch { problem = "Could not move the files into Games/\(dest): \(error.localizedDescription)" }
                } else {
                    problem = "The installer put no files into the game folder."
                }
            }
            try? FileManager.default.removeItem(at: staging)
            for line in (outcome.log ?? []) { dlog("[inno] \(line)") }
            let seconds = Int(Date().timeIntervalSince(started))
            let failure = problem, moved = counted
            await MainActor.run {
                self.unpacking = nil
                self.unpackProgress = nil
                progress.deallocate()
                ScreenAwake.set("unpack", false)
                self.endBackgroundTask()
                if let failure {
                    dlog("[games] unpack \(installer.id) failed after \(seconds) s: \(failure)")
                    self.unpackMessage = "Unpacking \(installer.exe) failed: \(failure)"
                    self.refresh(prefix: prefix)
                    return
                }
                dlog("[games] unpacked \(installer.id) -> Games/\(dest): ok, \(moved.files) files, \(CustomGames.mb(moved.bytes)) in \(seconds) s, \(outcome.warnings ?? 0) warnings")
                self.unpacked[installer.id] = dest
                self.saveUnpacked()
                self.refresh(prefix: prefix)
                if let game = self.games.first(where: { $0.folder == dest }) { dlog("[games] \(dest) starts with \(game.exe)") }
                let files = CustomGames.installerFiles(installer, info: info)
                self.cleanup = InstallerCleanup(installer: installer, destination: dest, files: files,
                                                bytes: files.reduce(0) { $0 + CustomGames.fileSize(installer.folder, $1) })
            }
        }
    }

    private func endBackgroundTask() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }

    func cancelUnpack() {
        unpackProgress?.pointee.cancel = 1
        unpackDetail = "Cancelling…"
    }

    /// Deletes an installer's .exe and .bin files (only ever after the user confirmed), and its
    /// folder when nothing else is left in it.
    func deleteInstaller(_ installer: GameInstaller, prefix: URL) {
        let info = installerInfo[installer.id]
        let folder = Self.root.appendingPathComponent(installer.folder, isDirectory: true)
        let files = Self.installerFiles(installer, info: info)
        for name in files { try? FileManager.default.removeItem(at: folder.appendingPathComponent(name)) }
        dlog("[games] deleted installer files in \(installer.folder): \(files.joined(separator: ", "))")
        let rest = ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).filter { !$0.hasPrefix(".") }
        if rest.isEmpty { try? FileManager.default.removeItem(at: folder) }
        unpacked.removeValue(forKey: installer.id)
        installerInfo.removeValue(forKey: installer.id)
        saveUnpacked()
        refresh(prefix: prefix)
    }

    /// The installer's own files in its folder: the .exe and the .bin parts it named (any case).
    nonisolated static func installerFiles(_ installer: GameInstaller, info: InstallerInfo?) -> [String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.appendingPathComponent(installer.folder).path)) ?? []
        let stem = (installer.exe as NSString).deletingPathExtension.lowercased()
        let parts = Set((info?.parts ?? []).map { $0.name.lowercased() })
        return [installer.exe] + names.filter { name in
            let l = name.lowercased()
            return parts.contains(l) || (l.hasPrefix(stem + "-") && l.hasSuffix(".bin"))
        }.sorted()
    }

    nonisolated static func fileSize(_ folder: String, _ name: String) -> Int64 {
        let url = root.appendingPathComponent(folder).appendingPathComponent(name)
        return Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
    }

    nonisolated static func size(_ bytes: Int64) -> String { ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file) }

    nonisolated private static func freeSpace() -> Int64? {
        (try? documents.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage
    }

    /// GOG installers at the top of a game folder: setup_*.exe and patch_*.exe.
    nonisolated static func installers(in folder: String) -> [GameInstaller] {
        let base = root.appendingPathComponent(folder, isDirectory: true)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: base.path)) ?? []
        return names.sorted().filter { name in
            let l = name.lowercased()
            return (l.hasPrefix("setup_") || l.hasPrefix("patch_")) && l.hasSuffix(".exe")
                && peMachine(base.appendingPathComponent(name)) != 0
        }.map { GameInstaller(folder: folder, exe: $0) }
    }

    /// The installer language for the iPhone's language, else English, else the first one.
    nonisolated static func innoLanguage(_ available: [String]) -> String {
        let names = ["en": "english", "de": "german", "fr": "french", "es": "spanish", "it": "italian", "pl": "polish",
                     "ru": "russian", "pt": "brazilianportuguese", "ja": "japanese", "zh": "chinese", "ko": "korean",
                     "nl": "dutch", "cs": "czech", "hu": "hungarian", "tr": "turkish", "uk": "ukrainian"]
        let lower = available.map { $0.lowercased() }
        for code in Locale.preferredLanguages.map({ String($0.prefix(2)) }) {
            if let name = names[code], let i = lower.firstIndex(of: name) { return available[i] }
        }
        if let i = lower.firstIndex(of: "english") { return available[i] }
        return available.first ?? ""
    }

    /// Moves everything in src into dst, replacing files that exist (an add-on or patch) and
    /// matching folder names in any case ("Data" and "data" are one folder to a Windows game).
    nonisolated static func merge(_ src: URL, into dst: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: dst, withIntermediateDirectories: true)
        let existing = Dictionary(((try? fm.contentsOfDirectory(atPath: dst.path)) ?? []).map { ($0.lowercased(), $0) },
                                  uniquingKeysWith: { a, _ in a })
        for item in try fm.contentsOfDirectory(at: src, includingPropertiesForKeys: [.isDirectoryKey]) {
            let name = existing[item.lastPathComponent.lowercased()] ?? item.lastPathComponent
            let target = dst.appendingPathComponent(name)
            var targetIsDir: ObjCBool = false
            let exists = fm.fileExists(atPath: target.path, isDirectory: &targetIsDir)
            if (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true, exists, targetIsDir.boolValue {
                try merge(item, into: target)
            } else {
                if exists { try fm.removeItem(at: target) }
                try fm.moveItem(at: item, to: target)
            }
        }
    }

    nonisolated private static func tally(_ dir: URL) -> (files: Int, bytes: Int64) {
        var files = 0, bytes: Int64 = 0
        let walker = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey])
        while let url = walker?.nextObject() as? URL {
            guard let v = try? url.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]), v.isRegularFile == true else { continue }
            files += 1
            bytes += Int64(v.fileSize ?? 0)
        }
        return (files, bytes)
    }

    /// Leftovers of an unpack the app did not finish (it was closed or killed).
    nonisolated private static func removeStaleStaging(except running: String?) {
        guard running == nil else { return }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        for name in names where name.hasPrefix(".unpack-") {
            try? FileManager.default.removeItem(at: root.appendingPathComponent(name))
        }
    }

    nonisolated private static func decode<T: Decodable>(_ json: UnsafeMutablePointer<CChar>?) -> T? {
        guard let json else { return nil }
        defer { mid_inno_free(json) }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try? decoder.decode(T.self, from: Data(String(cString: json).utf8))
    }

    /// The game's primary play task from GOG's goggame-<id>.info (installed and unpacked GOG
    /// games have one): the program GOG Galaxy starts, and its arguments.
    nonisolated static func gogPlayTask(in folder: String) -> (path: String, arguments: String)? {
        let base = root.appendingPathComponent(folder, isDirectory: true)
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: base.path)) ?? [])
            .filter { $0.lowercased().hasPrefix("goggame-") && $0.lowercased().hasSuffix(".info") }
        var candidates: [(json: [String: Any], base: Bool)] = []
        for name in names {
            guard let data = try? Data(contentsOf: base.appendingPathComponent(name)),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { continue }
            let id = json["gameId"] as? String, rootId = json["rootGameId"] as? String
            candidates.append((json, id != nil && id == rootId))
        }
        for c in candidates.sorted(by: { $0.base && !$1.base }) {
            guard let tasks = c.json["playTasks"] as? [[String: Any]],
                  let task = tasks.first(where: { ($0["isPrimary"] as? Bool) == true && ($0["type"] as? String ?? "FileTask") == "FileTask" }),
                  let path = task["path"] as? String,
                  let actual = resolve(base, path.replacingOccurrences(of: "\\", with: "/")) else { continue }
            let machine = peMachine(base.appendingPathComponent(actual))
            guard machine == 0x8664 || machine == 0x14c else { continue }
            return (actual, task["arguments"] as? String ?? "")
        }
        return nil
    }

    /// A "/"-separated path inside base with each part matched in any case (as Windows would).
    nonisolated private static func resolve(_ base: URL, _ path: String) -> String? {
        var dir = base, parts: [String] = []
        for part in path.split(separator: "/").map(String.init) where !part.isEmpty && part != "." {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
            guard let name = names.first(where: { $0.caseInsensitiveCompare(part) == .orderedSame }) else { return nil }
            parts.append(name)
            dir = dir.appendingPathComponent(name)
        }
        return parts.isEmpty ? nil : parts.joined(separator: "/")
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
