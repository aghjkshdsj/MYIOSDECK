// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The controller a game sees, and how the on-screen controller looks: Xbox or PlayStation.
/// Without UIKit (tests/host/pad_style checks it).
///
/// What each one gives a Windows game (ControllerBridge, WineController):
/// - Xbox: player 1 is an XInput (Xbox) controller; a game that only reads DirectInput (found from
///   its files, PadAPIScan) also gets MYIOSDECK's DirectInput pad.
/// - PlayStation: player 1 is also a DualSense HID controller (Sony's VID/PID, its own reports), which
///   DirectInput lists too, so games with PlayStation support show its buttons; XInput stays
///   connected, so games that only read XInput still work.
enum PadStyle: String, Codable, CaseIterable, Identifiable {
    case xbox, playstation
    var id: String { rawValue }
    var title: String { self == .xbox ? "Xbox" : "PlayStation" }

    /// The style an older per-game Controller choice meant ("xinput", "dinput", "hid", "dualsense",
    /// "keyboard"), and whether it was the keyboard-and-mouse mode.
    static func migrate(legacyAPI api: String?) -> (style: PadStyle, keyboard: Bool)? {
        switch api {
        case nil: return nil
        case "dualsense"?: return (.playstation, false)
        case "keyboard"?: return (.xbox, true)
        default: return (.xbox, false)   // xinput, dinput (now automatic), hid (generic: an Xbox pad's layout)
        }
    }
}

/// Which controller APIs a game's own files name: XInput, DirectInput, or neither found.
enum PadAPIScan {
    enum Result: String, Equatable { case xinput, dinputOnly, unknown }

    /// What MYIOSDECK's DirectInput pad is for: a game that reads DirectInput and never XInput (older
    /// games). A game naming XInput gets none (it would see the controller twice).
    static func needsDInputPad(_ r: Result) -> Bool { r == .dinputOnly }

    private static let xinputMarks = ["xinput1_", "xinput9_1_0", "xinputgetstate", "xinputenable"]
    private static let dinputMarks = ["directinput8create", "directinputcreate", "dinput8.dll", "dinput.dll"]

    /// Looks through the bytes of a game's programs and libraries (ASCII and UTF-16 names, any case).
    static func classify(_ blobs: [Data]) -> Result {
        var xinput = false, dinput = false
        for blob in blobs {
            let lower = Data(blob.map { $0 >= 0x41 && $0 <= 0x5a ? $0 | 0x20 : $0 })
            if !xinput, xinputMarks.contains(where: { contains(lower, $0) }) { xinput = true }
            if !dinput, dinputMarks.contains(where: { contains(lower, $0) }) { dinput = true }
            if xinput { break }
        }
        if xinput { return .xinput }
        return dinput ? .dinputOnly : .unknown
    }

    private static func contains(_ data: Data, _ mark: String) -> Bool {
        let ascii = Data(mark.utf8)
        if data.range(of: ascii) != nil { return true }
        var wide = Data()
        for b in mark.utf8 { wide.append(b); wide.append(0) }
        return data.range(of: wide) != nil
    }
}
