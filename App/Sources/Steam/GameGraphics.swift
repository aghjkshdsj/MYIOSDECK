// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Per-game renderer choice (the game sheet's "Graphics" picker), saved per App ID.
/// Default starts the game exactly as Steam's launch configuration says. Direct3D or
/// OpenGL first prefer a Steam launch entry that names that renderer, then add the game
/// engine's own switch (detected from the install folder). A game can only switch to a
/// renderer it ships: many Unity games include Direct3D only, Godot 3 has no Direct3D.
/// OpenGL runs through Mesa Zink on MoltenVK (engine/gl), Direct3D through DXMT / D3D12.
enum GameGraphics: String, CaseIterable, Identifiable {
    case automatic = "default"
    case direct3d = "d3d"
    case opengl = "opengl"

    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: return "Default"
        case .direct3d: return "Direct3D"
        case .opengl: return "OpenGL"
        }
    }

    private static func key(_ appID: Int) -> String { "graphics.\(appID)" }

    static func choice(appID: Int) -> GameGraphics {
        UserDefaults.standard.string(forKey: key(appID)).flatMap(GameGraphics.init(rawValue:)) ?? .automatic
    }

    static func setChoice(_ choice: GameGraphics, appID: Int) {
        if choice == .automatic {
            UserDefaults.standard.removeObject(forKey: key(appID))
        } else {
            UserDefaults.standard.set(choice.rawValue, forKey: key(appID))
        }
    }

    /// Engines whose renderer switch is known.
    enum Engine: String {
        case unity, godot3, godot4, godot, factorio, unknown
    }

    /// Whether a Steam launch entry names this renderer in its program or arguments
    /// (e.g. "Game_OpenGL.exe", "-opengl", "-dx11"). Default matches nothing.
    func matches(_ option: SteamLaunchOption) -> Bool {
        let text = (option.executable + " " + option.arguments).lowercased()
        let words = Set(text.split(whereSeparator: { " \t\"".contains($0) }).map(String.init))
        switch self {
        case .automatic: return false
        case .opengl:
            return text.contains("opengl") || !words.isDisjoint(with: ["-gl", "--gl", "-glcore", "-force-glcore"])
        case .direct3d:
            return text.contains("d3d") || text.contains("directx")
                || !words.isDisjoint(with: ["-dx11", "-dx12", "--dx11", "--dx12", "-force-d3d11", "-force-d3d12"])
        }
    }

    /// The engine of `program` (slash path relative to `root`, the install folder).
    static func engine(program: String, root: URL) -> Engine {
        let exe = root.appendingPathComponent(program)
        let folder = exe.deletingLastPathComponent()
        let stem = exe.deletingPathExtension().lastPathComponent
        let fm = FileManager.default
        let names = (try? fm.contentsOfDirectory(atPath: folder.path)) ?? []
        func has(_ name: String) -> Bool { names.contains { $0.caseInsensitiveCompare(name) == .orderedSame } }

        if exe.lastPathComponent.lowercased() == "factorio.exe" { return .factorio }
        if has("UnityPlayer.dll") || has(stem + "_Data") { return .unity }
        if let pck = names.first(where: { $0.caseInsensitiveCompare(stem + ".pck") == .orderedSame }),
           let major = godotMajor(pck: folder.appendingPathComponent(pck)) {
            return major >= 4 ? .godot4 : .godot3
        }
        if let major = godotEmbeddedMajor(exe: exe) { return major >= 4 ? .godot4 : .godot3 }
        if names.contains(where: { $0.lowercased().hasSuffix(".pck") }) { return .godot }
        return .unknown
    }

    /// The engine's switch for this renderer; empty when there is none (or Default).
    func arguments(for engine: Engine) -> [String] {
        switch (self, engine) {
        case (.opengl, .unity): return ["-force-glcore"]
        case (.direct3d, .unity): return ["-force-d3d11"]
        case (.opengl, .godot4): return ["--rendering-driver", "opengl3"]
        case (.direct3d, .godot4): return ["--rendering-driver", "d3d12"]
        case (.opengl, .godot3): return ["--video-driver", "GLES3"]
        case (.opengl, .godot): return ["--rendering-driver", "opengl3"]
        case (.opengl, .factorio): return ["--force-opengl"]
        case (.direct3d, .factorio): return ["--force-d3d"]
        default: return []
        }
    }

    /// SDL's renderer hint (SDL_Renderer games), exported into the Windows environment.
    var sdlRenderDriver: String? {
        switch self {
        case .automatic: return nil
        case .opengl: return "opengl"
        case .direct3d: return "direct3d11"
        }
    }

    // A Godot pack starts with "GDPC", then the pack format, then the engine's
    // major, minor and patch version (little-endian 32-bit words).
    private static func godotMajor(pck url: URL) -> Int? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        guard let d = try? h.read(upToCount: 12), d.count == 12 else { return nil }
        return godotHeaderMajor(d)
    }

    // A pack embedded in the program ends with its size (64-bit) and "GDPC".
    private static func godotEmbeddedMajor(exe url: URL) -> Int? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        guard let end = try? h.seekToEnd(), end > 12 + 12 else { return nil }
        guard (try? h.seek(toOffset: end - 12)) != nil, let data = try? h.read(upToCount: 12), data.count == 12 else { return nil }
        let tail = [UInt8](data)
        guard tail[8] == 0x47, tail[9] == 0x44, tail[10] == 0x50, tail[11] == 0x43 else { return nil }
        var size: UInt64 = 0
        for i in 0..<8 { size |= UInt64(tail[i]) << UInt64(8 * i) }
        guard size > 12, size <= end - 12 else { return nil }
        guard (try? h.seek(toOffset: end - 12 - size)) != nil, let d = try? h.read(upToCount: 12), d.count == 12 else { return nil }
        return godotHeaderMajor(d)
    }

    private static func godotHeaderMajor(_ d: Data) -> Int? {
        let b = [UInt8](d)
        guard b.count >= 12, b[0] == 0x47, b[1] == 0x44, b[2] == 0x50, b[3] == 0x43 else { return nil }
        let major = Int(b[8]) | Int(b[9]) << 8 | Int(b[10]) << 16 | Int(b[11]) << 24
        return (1...9).contains(major) ? major : nil
    }
}
