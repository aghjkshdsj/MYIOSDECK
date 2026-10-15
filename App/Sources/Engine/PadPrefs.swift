// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The Xbox / PlayStation choice (Settings › Controls, Game settings, the game menu) and the
/// controller-as-keyboard-and-mouse mode, per game with the app default under them. Choices made
/// before (the per-game "Controller API") are read as the style they meant (PadStyle.migrate).
enum PadPrefs {
    private static var d: UserDefaults { .standard }

    static var defaultStyle: PadStyle {
        get {
            if let s = d.string(forKey: "padStyle").flatMap(PadStyle.init) { return s }
            return PadStyle.migrate(legacyAPI: d.string(forKey: "controllerAPI"))?.style ?? .xbox
        }
        set { d.set(newValue.rawValue, forKey: "padStyle") }
    }

    /// A game's style: its own, an older per-game choice, else the default.
    static func style(gameKey: String?, programID: String? = nil) -> PadStyle {
        if let k = gameKey, !k.isEmpty {
            if let s = d.string(forKey: "padStyle.\(k)").flatMap(PadStyle.init) { return s }
            if let m = PadStyle.migrate(legacyAPI: d.string(forKey: "controllerAPI.\(k)")) { return m.style }
        }
        if let p = programID, let m = PadStyle.migrate(legacyAPI: d.string(forKey: "controllerAPI.\(p)")) { return m.style }
        return defaultStyle
    }

    /// Whether the game has its own style (Game settings shows "Use the app's").
    static func hasOwnStyle(_ gameKey: String) -> Bool { d.string(forKey: "padStyle.\(gameKey)") != nil }

    static func setStyle(_ s: PadStyle?, gameKey: String) {
        guard !gameKey.isEmpty else { if let s { defaultStyle = s }; return }
        if let s { d.set(s.rawValue, forKey: "padStyle.\(gameKey)") } else { d.removeObject(forKey: "padStyle.\(gameKey)") }
    }

    /// The controller as keyboard and mouse for this game (games without controller support).
    static func keyboard(gameKey: String?, programID: String? = nil) -> Bool {
        if let k = gameKey, !k.isEmpty {
            if d.object(forKey: "padKeyboard.\(k)") != nil { return d.bool(forKey: "padKeyboard.\(k)") }
            if let m = PadStyle.migrate(legacyAPI: d.string(forKey: "controllerAPI.\(k)")) { return m.keyboard }
        }
        if let p = programID, let m = PadStyle.migrate(legacyAPI: d.string(forKey: "controllerAPI.\(p)")) { return m.keyboard }
        return false
    }

    static func setKeyboard(_ on: Bool, gameKey: String) {
        guard !gameKey.isEmpty else { return }
        d.set(on, forKey: "padKeyboard.\(gameKey)")
    }

    // MARK: which APIs a game reads

    /// XInput, DirectInput only, or unknown, from the game's program and the libraries beside it,
    /// remembered per program (and its size and date, so an update is looked at again).
    static func scanAPIs(exe: URL) -> PadAPIScan.Result {
        let fm = FileManager.default
        let attrs = try? fm.attributesOfItem(atPath: exe.path)
        let stamp = "\((attrs?[.size] as? NSNumber)?.int64Value ?? 0)-\(Int((attrs?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0))"
        var h: UInt64 = 0xcbf29ce484222325   // a stable key (String.hashValue changes every launch)
        for b in exe.path.utf8 { h = (h ^ UInt64(b)) &* 0x100000001b3 }
        let key = "padScan.\(String(h, radix: 16))"
        if let cached = d.string(forKey: key), cached.hasPrefix(stamp + ":"),
           let r = PadAPIScan.Result(rawValue: String(cached.dropFirst(stamp.count + 1))) { return r }
        let dir = exe.deletingLastPathComponent()
        var files = [exe]
        for name in (try? fm.contentsOfDirectory(atPath: dir.path)) ?? [] {
            let l = name.lowercased()
            if (l.hasSuffix(".dll") || l.hasSuffix(".exe")), name != exe.lastPathComponent { files.append(dir.appendingPathComponent(name)) }
        }
        var blobs: [Data] = [], total = 0
        for f in files.prefix(64) {
            guard let fh = try? FileHandle(forReadingFrom: f) else { continue }
            let data = (try? fh.read(upToCount: 64 << 20)) ?? Data()
            try? fh.close()
            blobs.append(data)
            total += data.count
            if total > 384 << 20 { break }
            // XInput named early: no need to read the rest.
            if PadAPIScan.classify([data]) == .xinput { blobs = [data]; break }
        }
        let r = PadAPIScan.classify(blobs)
        d.set("\(stamp):\(r.rawValue)", forKey: key)
        dlog("[pad] \(exe.lastPathComponent): \(r == .xinput ? "reads XInput" : r == .dinputOnly ? "reads DirectInput only: the DirectInput pad is on" : "no controller API found in its files")")
        return r
    }
}
