// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

enum FEXPreset: Int, CaseIterable, Identifiable {
    case compat = 0, fast = 1, fastest = 2
    var id: Int { rawValue }
    var title: String {
        switch self {
        case .compat: return "Compat"
        case .fast: return "Fast"
        case .fastest: return "Fastest"
        }
    }
    var detail: String {
        switch self {
        case .compat: return "Strict x86 memory ordering, vector and memcpy included, split locks. For games that crash."
        case .fast: return "x86 memory ordering kept, x87 at reduced precision. Safe for most games."
        case .fastest: return "Memory ordering emulation off. Biggest CPU win; multithreaded games may crash."
        }
    }
}

/// Persistent user settings (UserDefaults).
final class Settings: ObservableObject {
    private let d = UserDefaults.standard

    @Published var jitPoolMB: Int { didSet { d.set(jitPoolMB, forKey: "jitPoolMB") } }
    @Published var fexPreset: FEXPreset { didSet { d.set(fexPreset.rawValue, forKey: "fexPreset") } }
    @Published var multiblock: Bool { didSet { d.set(multiblock, forKey: "multiblock") } }
    @Published var maxInst: Int { didSet { d.set(maxInst, forKey: "maxInst") } }
    @Published var showHUD: Bool { didSet { d.set(showHUD, forKey: "showHUD") } }
    @Published var frameCap: Int { didSet { d.set(frameCap, forKey: "frameCap") } }
    @Published var verboseFEXLog: Bool {
        didSet { d.set(verboseFEXLog, forKey: "verboseFEXLog"); mid_engine_set_verbose(verboseFEXLog) }
    }
    /// Performance tab: also measure the no-JIT interpreter when JIT is on.
    @Published var benchInterpreter: Bool { didSet { d.set(benchInterpreter, forKey: "benchInterpreter") } }
    /// Without JIT, run the built-in x86-64 Linux programs with FXR instead of FXI (experimental).
    /// Windows programs use winFXR.
    @Published var useFXR: Bool { didSet { d.set(useFXR, forKey: "useFXR") } }
    /// The no-JIT engine for the built-in x86-64 Linux programs.
    var linuxNoJITMode: RunMode { useFXR ? .fxr : .interpreter }
    /// Without JIT, start Windows games with no audio device (Wine's mmdevapi disabled), so the
    /// game's audio engine does not decode and mix in the interpreter. Read at launch.
    @Published var noJITMuteAudio: Bool { didSet { d.set(noJITMuteAudio, forKey: "noJITMuteAudio") } }
    /// Without JIT, run Windows games' x64 code with FXR (on by default since it can run every 64-bit
    /// game of a kept session, docs/FXR.md) instead of FXI. Read when a game starts.
    @Published var winFXR: Bool { didSet { d.set(winFXR, forKey: "winFXR") } }
    /// Without JIT, keep the Wine session when a game ends, so the next game starts without
    /// closing MYIOSDECK (experimental, docs/SESSION_REUSE.md). Read at launch (WineController).
    @Published var sessionReuse: Bool { didSet { d.set(sessionReuse, forKey: "sessionReuse") } }
    /// How Steam games start: "ask" (a choice at every Play), "dock" (Madeira Dock: Valve's own
    /// Steam client signs in and starts the game, for games that need Steam running) or "direct"
    /// (the game's program starts on its own: faster, DRM-free games).
    @Published var steamLaunchMode: String { didSet { d.set(steamLaunchMode, forKey: "steamLaunchMode") } }
    /// Steam Cloud saves: synced before a Steam game starts and after it ends (SteamCloudSync).
    @Published var steamCloud: Bool { didSet { d.set(steamCloud, forKey: "steamCloud") } }
    /// How Windows games see the controller: "xinput", "dinput" (XInput plus a
    /// DirectInput joystick) or "hid" (player 1 as a HID gamepad). Read at launch.
    @Published var controllerAPI: String { didSet { d.set(controllerAPI, forKey: "controllerAPI") } }

    /// Games: the Metal performance HUD over the game (frame rate, GPU time). Per game in Game settings.
    @Published var gameHUD: Bool { didSet { d.set(gameHUD, forKey: "gameHUD") } }
    /// Games: the virtual monitor's height (540, 720, 900, 1080). Read when Wine starts.
    @Published var monitorHeight: Int { didSet { d.set(monitorHeight, forKey: "monitorHeight") } }
    /// The on-screen controller: "auto" (while no controller is connected), "always" or "off".
    @Published var touchPadMode: String { didSet { d.set(touchPadMode, forKey: "touchPadMode") } }
    /// A light tap felt when an on-screen button is pressed.
    @Published var touchPadHaptics: Bool { didSet { d.set(touchPadHaptics, forKey: "touchPadHaptics") } }

    /// The defaults of every setting (also read by GameDefaults without a Settings object).
    static func registerDefaults(_ d: UserDefaults) {
        // Wine copies every loaded DLL's code into the pool: give it room by default.
        d.register(defaults: ["jitPoolMB": mid_wine_linked() ? 1024 : 512, "fexPreset": FEXPreset.fast.rawValue, "multiblock": true,
                              "maxInst": 5000, "showHUD": true, "frameCap": 120, "verboseFEXLog": false,
                              "benchInterpreter": true, "useFXR": false, "noJITMuteAudio": false, "winFXR": true, "sessionReuse": false,
                              "steamLaunchMode": "ask", "steamCloud": true, "controllerAPI": "xinput",
                              "gameHUD": false, "monitorHeight": 720, "touchPadMode": "auto", "touchPadHaptics": true])
    }

    init() {
        Settings.registerDefaults(d)
        jitPoolMB = d.integer(forKey: "jitPoolMB")
        fexPreset = FEXPreset(rawValue: d.integer(forKey: "fexPreset")) ?? .fast
        multiblock = d.bool(forKey: "multiblock")
        maxInst = d.integer(forKey: "maxInst")
        showHUD = d.bool(forKey: "showHUD")
        frameCap = d.integer(forKey: "frameCap")
        verboseFEXLog = d.bool(forKey: "verboseFEXLog")
        benchInterpreter = d.bool(forKey: "benchInterpreter")
        useFXR = d.bool(forKey: "useFXR")
        noJITMuteAudio = d.bool(forKey: "noJITMuteAudio")
        winFXR = d.bool(forKey: "winFXR")
        sessionReuse = d.bool(forKey: "sessionReuse")
        steamLaunchMode = d.string(forKey: "steamLaunchMode") ?? "ask"
        steamCloud = d.bool(forKey: "steamCloud")
        controllerAPI = d.string(forKey: "controllerAPI") ?? "xinput"
        gameHUD = d.bool(forKey: "gameHUD")
        monitorHeight = d.integer(forKey: "monitorHeight")
        touchPadMode = d.string(forKey: "touchPadMode") ?? "auto"
        touchPadHaptics = d.bool(forKey: "touchPadHaptics")
    }
}
