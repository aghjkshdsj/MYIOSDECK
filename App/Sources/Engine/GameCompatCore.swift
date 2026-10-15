// SPDX-License-Identifier: GPL-3.0-or-later
//
// Launch-time compatibility checks that need only the game's files (docs/COMPATIBILITY.md).
// Foundation only, so tests/host runs them. Nothing here changes a game that starts today:
//   - steamAppIDFolders: a Steamworks game started without Steam gets the steam_appid.txt Valve
//     documents for running a game outside Steam, only where the game ships steam_api and has none.
//   - engine / airRuntime: what the game is built with and, for Adobe AIR, where its runtime is,
//     for the log (the next log names the engine and the runtime's state without asking).

import Foundation

enum GameCompat {
    // MARK: - steam_appid.txt (Without Steam)

    /// Folders that should get a steam_appid.txt holding the App ID before a Steam game starts
    /// without Steam: the program's folder and its working folder (steam_api reads the file from the
    /// working folder; newer SDKs also look next to the program), when the install folder ships
    /// steam_api(64).dll anywhere a few levels down (Unity keeps it in <Game>_Data/Plugins/x86_64)
    /// and the folder has no steam_appid.txt yet. Without the file, SteamAPI_RestartAppIfNecessary
    /// finds no Steam client, asks steam.exe to start the game and the game quits: 911 Operator
    /// (build 190) started C:\Program Files (x86)\Steam\steam.exe steam://run/503560 and exited 53.
    static func steamAppIDFolders(installFolder: URL, programFolder: URL, workingFolder: URL) -> [URL] {
        guard shipsSteamAPI(installFolder) else { return [] }
        var out: [URL] = []
        for folder in [programFolder, workingFolder] {
            let standard = folder.standardizedFileURL
            guard !out.contains(where: { $0.path == standard.path }),
                  isDirectory(standard), !hasFile(named: "steam_appid.txt", in: standard) else { continue }
            out.append(standard)
        }
        return out
    }

    /// Writes "<appID>\n" as steam_appid.txt in each folder (never over an existing file). Returns
    /// the files written.
    @discardableResult
    static func writeSteamAppID(_ appID: Int, folders: [URL]) -> [URL] {
        var written: [URL] = []
        for folder in folders {
            let file = folder.appendingPathComponent("steam_appid.txt")
            guard !FileManager.default.fileExists(atPath: file.path) else { continue }
            if (try? Data("\(appID)\n".utf8).write(to: file, options: .withoutOverwriting)) != nil { written.append(file) }
        }
        return written
    }

    /// steam_api.dll or steam_api64.dll within `depth` folder levels (names without case).
    static func shipsSteamAPI(_ root: URL, depth: Int = 4) -> Bool {
        findFile(in: root, depth: depth) { ["steam_api.dll", "steam_api64.dll"].contains($0.lowercased()) } != nil
    }

    // MARK: - Engine (for the log)

    /// The engine a Windows game's folder shows, from file names only ("unknown" when nothing matches).
    static func engine(programFolder: URL) -> String {
        let names = Set(((try? FileManager.default.contentsOfDirectory(atPath: programFolder.path)) ?? []).map { $0.lowercased() })
        let dataDirs = names.filter { $0.hasSuffix("_data") && isDirectory(programFolder.appendingPathComponent(original($0, in: programFolder))) }
        if names.contains("gameassembly.dll") { return "Unity (IL2CPP)" }
        if names.contains("unityplayer.dll") || !dataDirs.isEmpty {
            for dir in dataDirs {
                let d = programFolder.appendingPathComponent(original(dir, in: programFolder))
                if hasFile(named: "il2cpp_data", in: d) { return "Unity (IL2CPP)" }
                if hasFile(named: "Managed", in: d) { return "Unity (Mono)" }
            }
            if names.contains("monobleedingedge") || names.contains("mono") { return "Unity (Mono)" }
            if names.contains("unityplayer.dll") { return "Unity" }
        }
        if isAIR(programFolder) { return "Adobe AIR" }
        if names.contains("data.win") || (names.contains("options.ini") && names.contains("audiogroup1.dat")) { return "GameMaker" }
        if names.contains("fna.dll") { return "FNA" }
        if names.contains("monogame.framework.dll") { return "MonoGame" }
        if names.contains("microsoft.xna.framework.dll") || names.contains("microsoft.xna.framework.game.dll") { return "XNA" }
        if names.contains(where: { $0.hasSuffix(".pck") }) { return "Godot" }
        if names.contains("renpy") || names.contains(where: { $0.hasSuffix(".rpa") }) { return "Ren'Py" }
        if names.contains("rgss3a") || names.contains(where: { $0.hasPrefix("rgss") && $0.hasSuffix(".dll") }) { return "RPG Maker (RGSS)" }
        if names.contains("nw.dll") || names.contains("nw_elf.dll") { return "NW.js (Chromium)" }
        if names.contains("libcef.dll") { return "Chromium Embedded" }
        if names.contains("sdl2.dll") || names.contains("sdl3.dll") { return "SDL" }
        // Unreal: <Game>\Binaries\Win64\<Game>-Win64-Shipping.exe
        if programFolder.lastPathComponent.lowercased() == "win64",
           programFolder.deletingLastPathComponent().lastPathComponent.lowercased() == "binaries" { return "Unreal" }
        return "unknown"
    }

    // MARK: - Adobe AIR

    /// An AIR application: META-INF/AIR/application.xml next to the program.
    static func isAIR(_ programFolder: URL) -> Bool {
        guard let meta = childNamed("META-INF", in: programFolder), let air = childNamed("AIR", in: meta) else { return false }
        return hasFile(named: "application.xml", in: air)
    }

    /// What an AIR application's launcher finds: the captive runtime it carries
    /// ("Adobe AIR\Versions\1.0\Adobe AIR.dll" next to the program) or none (then it looks for a
    /// shared runtime and, finding none, says "This application requires a version of Adobe AIR
    /// which cannot be found"). nil when the folder is not an AIR application.
    static func airRuntime(programFolder: URL) -> String? {
        guard isAIR(programFolder) else { return nil }
        guard let air = childNamed("Adobe AIR", in: programFolder),
              let versions = childNamed("Versions", in: air), let v1 = childNamed("1.0", in: versions) else {
            return "no captive runtime (no Adobe AIR\\Versions\\1.0 folder next to the program): it needs a shared AIR runtime"
        }
        guard let dll = childNamed("Adobe AIR.dll", in: v1) else {
            return "captive runtime folder without Adobe AIR.dll"
        }
        let machine = peMachine(dll)
        let kind = machine == 0x14c ? "32-bit" : machine == 0x8664 ? "64-bit" : "machine 0x" + String(machine, radix: 16)
        let resources = childNamed("Resources", in: v1) != nil ? "with Resources" : "no Resources folder"
        return "captive runtime Adobe AIR.dll (\(kind), \(resources))"
    }

    /// The PE machine field of a file (0 when it is not a PE file).
    static func peMachine(_ file: URL) -> UInt16 {
        guard let h = try? FileHandle(forReadingFrom: file) else { return 0 }
        defer { try? h.close() }
        guard let head = try? h.read(upToCount: 0x400), head.count >= 0x40 else { return 0 }
        let lfanew = Int(head[0x3c]) | Int(head[0x3d]) << 8 | Int(head[0x3e]) << 16 | Int(head[0x3f]) << 24
        guard lfanew >= 0x40, lfanew + 6 <= head.count, head[lfanew] == 0x50, head[lfanew + 1] == 0x45 else { return 0 }
        return UInt16(head[lfanew + 4]) | UInt16(head[lfanew + 5]) << 8
    }

    // MARK: - Files (names without case, as Windows finds them)

    static func isDirectory(_ url: URL) -> Bool {
        var dir: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path, isDirectory: &dir) && dir.boolValue
    }

    static func childNamed(_ name: String, in folder: URL) -> URL? {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: folder.path),
              let hit = names.first(where: { $0 == name }) ?? names.first(where: { $0.caseInsensitiveCompare(name) == .orderedSame })
        else { return nil }
        return folder.appendingPathComponent(hit)
    }

    static func hasFile(named name: String, in folder: URL) -> Bool { childNamed(name, in: folder) != nil }

    private static func original(_ lowered: String, in folder: URL) -> String {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).first { $0.lowercased() == lowered } ?? lowered
    }

    /// The first file under `root` (at most `depth` folder levels down) whose name matches.
    static func findFile(in root: URL, depth: Int, _ match: (String) -> Bool) -> URL? {
        guard depth >= 0, let names = try? FileManager.default.contentsOfDirectory(atPath: root.path) else { return nil }
        var dirs: [URL] = []
        for name in names {
            let url = root.appendingPathComponent(name)
            if isDirectory(url) { dirs.append(url) } else if match(name) { return url }
        }
        for dir in dirs { if let hit = findFile(in: dir, depth: depth - 1, match) { return hit } }
        return nil
    }

    /// A Windows path on drive C: ("C:\Program Files (x86)\...") under the prefix's drive_c.
    static func unixURL(windowsPath: String, driveC: URL) -> URL? {
        let p = windowsPath.replacingOccurrences(of: "/", with: "\\")
        guard p.count >= 3, p.prefix(3).lowercased() == "c:\\" else { return nil }
        let rest = p.dropFirst(3).split(separator: "\\").map(String.init)
        return rest.reduce(driveC) { $0.appendingPathComponent($1) }
    }
}
