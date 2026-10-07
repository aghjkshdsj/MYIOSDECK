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
    /// How Windows games see the controller: "xinput", "dinput" (XInput plus a
    /// DirectInput joystick) or "hid" (player 1 as a HID gamepad). Read at launch.
    @Published var controllerAPI: String { didSet { d.set(controllerAPI, forKey: "controllerAPI") } }

    init() {
        // Wine copies every loaded DLL's code into the pool: give it room by default.
        d.register(defaults: ["jitPoolMB": mid_wine_linked() ? 1024 : 512, "fexPreset": FEXPreset.fast.rawValue, "multiblock": true,
                              "maxInst": 5000, "showHUD": true, "frameCap": 120, "verboseFEXLog": false,
                              "benchInterpreter": true, "controllerAPI": "xinput"])
        jitPoolMB = d.integer(forKey: "jitPoolMB")
        fexPreset = FEXPreset(rawValue: d.integer(forKey: "fexPreset")) ?? .fast
        multiblock = d.bool(forKey: "multiblock")
        maxInst = d.integer(forKey: "maxInst")
        showHUD = d.bool(forKey: "showHUD")
        frameCap = d.integer(forKey: "frameCap")
        verboseFEXLog = d.bool(forKey: "verboseFEXLog")
        benchInterpreter = d.bool(forKey: "benchInterpreter")
        controllerAPI = d.string(forKey: "controllerAPI") ?? "xinput"
    }
}
