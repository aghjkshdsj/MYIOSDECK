// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

extension GameDefaults {
    /// The app defaults as Settings holds them now.
    static var current: GameDefaults {
        let d = UserDefaults.standard
        Settings.registerDefaults(d)
        let cap = d.integer(forKey: "frameCap")
        return GameDefaults(fxr: d.bool(forKey: "winFXR"),
                            frameCap: cap >= 120 ? 0 : cap,
                            performanceHUD: d.bool(forKey: "gameHUD"),
                            audio: !d.bool(forKey: "noJITMuteAudio"),
                            resolution: d.integer(forKey: "monitorHeight"),
                            touchPad: TouchPadMode(rawValue: d.string(forKey: "touchPadMode") ?? "") ?? .auto)
    }
}

/// Game settings by game ("steam.<App ID>" / "custom.<UUID>"), in UserDefaults.
enum GameSettingsStore {
    private static func key(_ game: String) -> String { "gameSettings.\(game)" }

    static func load(_ game: String) -> GameSettings {
        guard let data = UserDefaults.standard.data(forKey: key(game)),
              let s = try? JSONDecoder().decode(GameSettings.self, from: data) else { return GameSettings() }
        return s
    }

    static func save(_ s: GameSettings, for game: String) {
        if s == GameSettings() { UserDefaults.standard.removeObject(forKey: key(game)); return }
        if let data = try? JSONEncoder().encode(s) { UserDefaults.standard.set(data, forKey: key(game)) }
    }

    static func overrides(_ game: String) -> LaunchOverrides {
        LaunchOverrides(gameKey: game, settings: load(game), defaults: .current)
    }

    /// Before a game starts: its settings for WineController.run() to apply.
    static func prepareLaunch(_ game: String, wine: WineController) {
        let o = overrides(game)
        dlog("[game-settings] \(game): \(o.fxr ? "FXR" : "FXI") cap=\(o.frameCap == 0 ? "none" : "\(o.frameCap)") hud=\(o.performanceHUD ? 1 : 0) audio=\(o.audio ? 1 : 0) monitor=\(o.resolution)p touch-pad=\(o.touchPad.rawValue)")
        wine.pendingOverrides = o
    }
}
