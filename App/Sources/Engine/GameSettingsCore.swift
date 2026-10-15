// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// How the on-screen controller shows (Settings › Controls, and per game).
enum TouchPadMode: String, Codable, CaseIterable, Identifiable {
    /// Shown while no physical controller is connected, hidden when one connects.
    case auto
    case always
    case off
    var id: String { rawValue }
    var title: String {
        switch self {
        case .auto: return "Automatic"
        case .always: return "Always"
        case .off: return "Off"
        }
    }
}

/// The x64 CPU without JIT for a game's settings (FXR fast, FXI compatible).
enum CPUPreset: String, Codable, CaseIterable, Identifiable {
    case fast, compatibility
    var id: String { rawValue }
    var title: String { self == .fast ? "Fast" : "Compatibility" }
}

/// One game's own settings (Game settings, mockup 7). Every value is optional: nil follows the app
/// default. With `useDefaults` on, the overrides are kept but not used.
struct GameSettings: Codable, Equatable {
    var useDefaults = true
    var cpu: CPUPreset?
    /// Frames per second for Direct3D 10/11 games; 0 is no limit.
    var frameCap: Int?
    var performanceHUD: Bool?
    /// Without JIT: the game's audio (false starts it with no audio device).
    var audio: Bool?
    /// Height of the virtual monitor in pixels (its width follows the screen's shape).
    var resolution: Int?
    var touchPad: TouchPadMode?
}

/// The app defaults the overrides fall back to (Settings).
struct GameDefaults: Equatable {
    var fxr: Bool
    var frameCap: Int
    var performanceHUD: Bool
    var audio: Bool
    var resolution: Int
    var touchPad: TouchPadMode

}

/// What a launch uses after the game's settings and the app defaults are combined.
struct LaunchOverrides: Equatable {
    var gameKey: String
    var fxr: Bool
    var frameCap: Int
    var performanceHUD: Bool
    var audio: Bool
    var resolution: Int
    var touchPad: TouchPadMode

    init(gameKey: String, settings s: GameSettings, defaults d: GameDefaults) {
        self.gameKey = gameKey
        let own = !s.useDefaults
        fxr = own ? (s.cpu.map { $0 == .fast } ?? d.fxr) : d.fxr
        frameCap = own ? (s.frameCap ?? d.frameCap) : d.frameCap
        performanceHUD = own ? (s.performanceHUD ?? d.performanceHUD) : d.performanceHUD
        audio = own ? (s.audio ?? d.audio) : d.audio
        resolution = LaunchOverrides.validResolution(own ? (s.resolution ?? d.resolution) : d.resolution)
        touchPad = own ? (s.touchPad ?? d.touchPad) : d.touchPad
    }

    static let resolutions = [540, 720, 900, 1080]
    static func validResolution(_ h: Int) -> Int { resolutions.contains(h) ? h : 720 }

    /// DXMT's configuration line for the frame cap (its d3d11 swapchain paces presents to
    /// d3d11.preferredMaxFrameRate), or nil for no cap.
    var dxmtConfig: String? { frameCap > 0 ? "d3d11.preferredMaxFrameRate=\(frameCap)" : nil }
}

