// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the on-screen controller's model (App/Sources/Engine/TouchPadCore.swift) and the
// per-game settings resolution (App/Sources/Engine/GameSettingsCore.swift).

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String, line: Int = #line) {
    checks += 1
    if !ok { failures += 1; print("FAIL line \(line): \(what)") }
}

// MARK: - Sticks

var v = PadMath.stick(dx: 0, dy: -50, radius: 50)
check(abs(v.x) < 1e-9 && abs(v.y - 1) < 1e-9, "finger up is +y: \(v)")
v = PadMath.stick(dx: 200, dy: 0, radius: 50)
check(abs(v.x - 1) < 1e-9 && abs(v.y) < 1e-9, "past the edge is limited to 1: \(v)")
v = PadMath.stick(dx: 100, dy: 100, radius: 50)
check(abs((v.x * v.x + v.y * v.y).squareRoot() - 1) < 1e-9 && v.y < 0, "diagonal limited to the circle, down is -y: \(v)")
check(PadMath.stick(dx: 10, dy: 10, radius: 0) == (0, 0), "no radius, no deflection")
check(PadState.axis(1) == 32767 && PadState.axis(-1) == -32768 && PadState.axis(0) == 0, "XInput range")
check(PadState.axis(2) == 32767 && PadState.axis(.nan) == 0, "clamped, NaN is centre")

// MARK: - D-pad and face buttons

check(PadMath.dpad(dx: 5, dy: 0, radius: 60) == 0, "dead zone")
check(PadMath.dpad(dx: 40, dy: 0, radius: 60) == PadState.right, "right")
check(PadMath.dpad(dx: 0, dy: -40, radius: 60) == PadState.up, "up (screen y down)")
check(PadMath.dpad(dx: -40, dy: 0, radius: 60) == PadState.left, "left")
check(PadMath.dpad(dx: 0, dy: 40, radius: 60) == PadState.down, "down")
check(PadMath.dpad(dx: 30, dy: -30, radius: 60) == PadState.up | PadState.right, "up-right")
check(PadMath.dpad(dx: -30, dy: 30, radius: 60) == PadState.down | PadState.left, "down-left")
check(PadMath.dpad(dx: 40, dy: -10, radius: 60) == PadState.right, "mostly right stays right")
check(PadMath.face(dx: 0, dy: 30) == PadState.a, "A bottom")
check(PadMath.face(dx: 30, dy: 0) == PadState.b, "B right")
check(PadMath.face(dx: -30, dy: 5) == PadState.x, "X left")
check(PadMath.face(dx: 2, dy: -30) == PadState.y, "Y top")

// MARK: - Physical + on-screen on player 1

let physical = PadState(buttons: PadState.a, lt: 40, rt: 0, lx: 1000, ly: 0, rx: 0, ry: -20000)
let touch = PadState(buttons: PadState.lb, lt: 255, rt: 10, lx: 20000, ly: 20000, rx: 100, ry: 0)
let m = physical.merged(with: touch)
check(m.buttons == PadState.a | PadState.lb, "buttons of both")
check(m.lt == 255 && m.rt == 10, "deeper triggers")
check(m.lx == 20000 && m.ly == 20000, "left stick: the on-screen one is pushed further")
check(m.rx == 0 && m.ry == -20000, "right stick: the physical one is pushed further")
check(PadState().merged(with: PadState()) == PadState(), "idle stays idle")

// MARK: - Layouts

for landscape in [true, false] {
    let l = TouchLayout.standard(landscape: landscape)
    check(Set(l.controls.map(\.control)) == Set(PadControl.allCases), "every control once (\(landscape))")
    check(l.controls.count == PadControl.allCases.count, "no duplicates (\(landscape))")
    let safe = landscape ? CGRect(x: 59, y: 0, width: 814, height: 409) : CGRect(x: 0, y: 59, width: 430, height: 839)
    for s in l.controls where !s.hidden {
        let f = TouchLayout.frame(s, in: safe)
        check(f.minX >= safe.minX - 1 && f.maxX <= safe.maxX + 1 && f.minY >= safe.minY - 1 && f.maxY <= safe.maxY + 1,
              "\(s.control) inside the safe area (\(landscape)): \(f)")
    }
    // The game view's menu and keyboard buttons (6...96 x 6...50 points, top left) stay reachable:
    // no control's touch area (14 points larger than drawn) reaches them.
    if landscape {
        for s in l.controls where !s.hidden {
            let touchArea = TouchLayout.frame(s, in: safe).insetBy(dx: -14, dy: -14)
            check(!touchArea.intersects(CGRect(x: 0, y: 0, width: 100, height: 50)), "\(s.control) clear of the menu buttons: \(touchArea)")
        }
    }
    // Visible controls do not overlap.
    let shown = l.controls.filter { !$0.hidden }.map { ($0.control, TouchLayout.frame($0, in: safe)) }
    for i in shown.indices { for j in shown.indices where j > i {
        check(!shown[i].1.intersects(shown[j].1), "\(shown[i].0) and \(shown[j].0) apart (\(landscape))")
    } }
}
var edited = TouchLayout.standard(landscape: true)
edited.update(.face) { $0.scale = 9; $0.x = -1; $0.y = 0.5 }
check(edited.spec(.face)?.scale == PadControlSpec.scaleRange.upperBound && edited.spec(.face)?.x == 0, "update clamps scale and position")
let data = try! JSONEncoder().encode(edited)
let back = try! JSONDecoder().decode(TouchLayout.self, from: data)
check(back == edited, "layout survives JSON")
var old = edited
old.controls.removeAll { $0.control == .rightStick }
old.opacity = 5
let done = old.completed(landscape: true)
check(done.spec(.rightStick) != nil && done.opacity == TouchLayout.opacityRange.upperBound, "an older layout gains new controls, opacity clamped")
check(abs(TouchLayout.unit(for: CGSize(width: 900, height: 390)) - 1) < 1e-9, "390 high is unit 1")

// MARK: - Game settings over the app defaults

let defaults = GameDefaults(fxr: true, frameCap: 0, performanceHUD: false, audio: true, resolution: 720, touchPad: .auto)
var gs = GameSettings()
var o = LaunchOverrides(gameKey: "steam.1", settings: gs, defaults: defaults)
check(o.fxr && o.frameCap == 0 && !o.performanceHUD && o.audio && o.resolution == 720 && o.touchPad == .auto && o.dxmtConfig == nil,
      "a new game follows the app defaults")
gs.cpu = .compatibility; gs.frameCap = 40; gs.performanceHUD = true; gs.audio = false; gs.resolution = 540; gs.touchPad = .off
o = LaunchOverrides(gameKey: "steam.1", settings: gs, defaults: defaults)
check(o.fxr && o.frameCap == 0, "overrides are ignored while Use app defaults is on")
gs.useDefaults = false
o = LaunchOverrides(gameKey: "steam.1", settings: gs, defaults: defaults)
check(!o.fxr && o.frameCap == 40 && o.performanceHUD && !o.audio && o.resolution == 540 && o.touchPad == .off, "own settings")
check(o.dxmtConfig == "d3d11.preferredMaxFrameRate=40", "DXMT frame cap line: \(o.dxmtConfig ?? "nil")")
var partial = GameSettings(); partial.useDefaults = false; partial.frameCap = 30
o = LaunchOverrides(gameKey: "custom.x", settings: partial, defaults: defaults)
check(o.frameCap == 30 && o.fxr && o.audio, "unset values follow the defaults")
partial.resolution = 333
check(LaunchOverrides(gameKey: "", settings: partial, defaults: defaults).resolution == 720, "an unknown resolution falls back to 720")
let enc = try! JSONDecoder().decode(GameSettings.self, from: JSONEncoder().encode(gs))
check(enc == gs, "game settings survive JSON")
check((try? JSONDecoder().decode(GameSettings.self, from: Data("{\"useDefaults\":false}".utf8)))?.cpu == nil, "older stored settings decode")

// MARK: - Xbox / PlayStation

check(PadStyle.migrate(legacyAPI: nil) == nil, "no older choice")
let mDS = PadStyle.migrate(legacyAPI: "dualsense")!
check(mDS.style == .playstation && !mDS.keyboard, "an older DualSense choice is PlayStation")
let mKB = PadStyle.migrate(legacyAPI: "keyboard")!
check(mKB.style == .xbox && mKB.keyboard, "keyboard and mouse is kept as its own switch")
for api in ["xinput", "dinput", "hid"] { let m = PadStyle.migrate(legacyAPI: api)!; check(m.style == .xbox && !m.keyboard, "\(api) is Xbox (DirectInput now automatic)") }
check(PadControl.face.title(.playstation).contains("Cross") && PadControl.lb.title(.playstation) == "L1" && PadControl.rt.title(.xbox) == "RT",
      "control names per style")
check(PadControl.home.button == PadState.guide && PadControl.view.button == PadState.back, "the PS / Xbox button is Guide")
check(TouchLayout.standard(landscape: true).spec(.home)?.hidden == true, "the PS / Xbox button starts hidden")

func blob(_ s: String, wide: Bool = false) -> Data {
    var d = Data(repeating: 0x90, count: 64)
    for b in s.utf8 { d.append(b); if wide { d.append(0) } }
    d.append(contentsOf: [0, 0, 0x4d, 0x5a])
    return d
}
check(PadAPIScan.classify([blob("XINPUT1_4.dll")]) == .xinput, "an XInput import (any case)")
check(PadAPIScan.classify([blob("xinput1_3.dll", wide: true)]) == .xinput, "XInput loaded by a UTF-16 name")
check(PadAPIScan.classify([blob("DINPUT8.dll"), blob("DirectInput8Create")]) == .dinputOnly, "DirectInput only")
check(PadAPIScan.classify([blob("dinput8.dll"), blob("XInputGetState")]) == .xinput, "both: XInput (no DirectInput pad, no double controller)")
check(PadAPIScan.classify([blob("kernel32.dll")]) == .unknown, "neither")
check(PadAPIScan.needsDInputPad(.dinputOnly) && !PadAPIScan.needsDInputPad(.xinput) && !PadAPIScan.needsDInputPad(.unknown),
      "only a DirectInput-only game gets the DirectInput pad")

print("touch-pad tests: \(checks - failures)/\(checks) passed")
exit(failures == 0 ? 0 : 1)
