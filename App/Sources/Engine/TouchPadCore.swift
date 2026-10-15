// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
#if canImport(CoreGraphics)
import CoreGraphics
#endif

/// The on-screen controller's model, without UIKit (tests/host/touch_pad checks it): the
/// controls and where they are, the XInput state they produce, and how it combines with a
/// physical controller on player 1.

/// One XInput gamepad state (XINPUT_GAMEPAD: buttons, triggers 0-255, sticks with up positive).
struct PadState: Equatable {
    var buttons: UInt16 = 0
    var lt: UInt8 = 0
    var rt: UInt8 = 0
    var lx: Int16 = 0
    var ly: Int16 = 0
    var rx: Int16 = 0
    var ry: Int16 = 0

    static let up: UInt16 = 0x0001, down: UInt16 = 0x0002, left: UInt16 = 0x0004, right: UInt16 = 0x0008
    static let start: UInt16 = 0x0010, back: UInt16 = 0x0020, l3: UInt16 = 0x0040, r3: UInt16 = 0x0080
    static let lb: UInt16 = 0x0100, rb: UInt16 = 0x0200
    static let a: UInt16 = 0x1000, b: UInt16 = 0x2000, x: UInt16 = 0x4000, y: UInt16 = 0x8000

    /// Player 1 with a physical controller and the on-screen one together: buttons of either,
    /// the deeper trigger, and per stick the one pushed further.
    func merged(with o: PadState) -> PadState {
        func stick(_ ax: Int16, _ ay: Int16, _ bx: Int16, _ by: Int16) -> (Int16, Int16) {
            let ma = Int(ax) * Int(ax) + Int(ay) * Int(ay), mb = Int(bx) * Int(bx) + Int(by) * Int(by)
            return ma >= mb ? (ax, ay) : (bx, by)
        }
        let l = stick(lx, ly, o.lx, o.ly), r = stick(rx, ry, o.rx, o.ry)
        return PadState(buttons: buttons | o.buttons, lt: max(lt, o.lt), rt: max(rt, o.rt), lx: l.0, ly: l.1, rx: r.0, ry: r.1)
    }

    /// A stick deflection in -1...1 (y up) as XInput's signed range.
    static func axis(_ v: Double) -> Int16 {
        guard v.isFinite else { return 0 }
        let c = max(-1, min(1, v))
        return Int16((c * (c < 0 ? 32768 : 32767)).rounded())
    }
}

/// The controls the on-screen controller has.
enum PadControl: String, Codable, CaseIterable, Identifiable {
    case leftStick, rightStick, dpad, face, lb, rb, lt, rt, view, menu
    var id: String { rawValue }

    var title: String {
        switch self {
        case .leftStick: return "Left stick"
        case .rightStick: return "Right stick"
        case .dpad: return "D-pad"
        case .face: return "A B X Y"
        case .lb: return "LB (L)"
        case .rb: return "RB (R)"
        case .lt: return "LT"
        case .rt: return "RT"
        case .view: return "View (Back)"
        case .menu: return "Menu (Start)"
        }
    }

    /// Size at scale 1 on a 390-point-high screen, in points.
    var baseSize: CGSize {
        switch self {
        case .leftStick, .rightStick: return CGSize(width: 136, height: 136)
        case .dpad: return CGSize(width: 136, height: 136)
        case .face: return CGSize(width: 150, height: 150)
        case .lb, .rb: return CGSize(width: 104, height: 50)
        case .lt, .rt: return CGSize(width: 76, height: 50)
        case .view, .menu: return CGSize(width: 46, height: 46)
        }
    }

    /// A one-button control's XInput bit (sticks, the D-pad, the face buttons and triggers have their own logic).
    var button: UInt16? {
        switch self {
        case .lb: return PadState.lb
        case .rb: return PadState.rb
        case .view: return PadState.back
        case .menu: return PadState.start
        default: return nil
        }
    }
}

/// One control in a layout: its centre as a fraction of the safe area, its scale, whether it shows.
struct PadControlSpec: Codable, Equatable, Identifiable {
    var control: PadControl
    var x: Double
    var y: Double
    var scale: Double = 1
    var hidden = false
    var id: PadControl { control }

    static let scaleRange = 0.5...2.0
}

/// A whole layout (one orientation).
struct TouchLayout: Codable, Equatable {
    var controls: [PadControlSpec]
    var opacity: Double = 0.8

    static let opacityRange = 0.15...1.0

    func spec(_ c: PadControl) -> PadControlSpec? { controls.first { $0.control == c } }

    mutating func update(_ c: PadControl, _ change: (inout PadControlSpec) -> Void) {
        guard let i = controls.firstIndex(where: { $0.control == c }) else { return }
        change(&controls[i])
        controls[i].scale = min(max(controls[i].scale, PadControlSpec.scaleRange.lowerBound), PadControlSpec.scaleRange.upperBound)
        controls[i].x = min(max(controls[i].x, 0), 1)
        controls[i].y = min(max(controls[i].y, 0), 1)
    }

    /// A layout from an older version lacks controls added since: they come in at their default place.
    func completed(landscape: Bool) -> TouchLayout {
        var out = self
        let base = TouchLayout.standard(landscape: landscape)
        for s in base.controls where out.spec(s.control) == nil { out.controls.append(s) }
        out.opacity = min(max(out.opacity, TouchLayout.opacityRange.lowerBound), TouchLayout.opacityRange.upperBound)
        return out
    }

    /// MYIOSDECK's layouts (mockup 3 in landscape): shoulders up top, the D-pad and left stick on the
    /// left, A B X Y on the right, View and Menu in the middle; the right stick is there but hidden.
    static func standard(landscape: Bool) -> TouchLayout {
        if landscape {
            return TouchLayout(controls: [
                PadControlSpec(control: .lb, x: 0.08, y: 0.23),
                PadControlSpec(control: .rb, x: 0.92, y: 0.23),
                PadControlSpec(control: .lt, x: 0.22, y: 0.23),
                PadControlSpec(control: .rt, x: 0.78, y: 0.23),
                PadControlSpec(control: .view, x: 0.45, y: 0.23),
                PadControlSpec(control: .menu, x: 0.55, y: 0.23),
                PadControlSpec(control: .dpad, x: 0.17, y: 0.47, scale: 0.9),
                PadControlSpec(control: .leftStick, x: 0.11, y: 0.82, scale: 0.9),
                PadControlSpec(control: .face, x: 0.88, y: 0.60),
                PadControlSpec(control: .rightStick, x: 0.70, y: 0.84, hidden: true),
            ])
        }
        return TouchLayout(controls: [
            PadControlSpec(control: .lt, x: 0.12, y: 0.47),
            PadControlSpec(control: .rt, x: 0.88, y: 0.47),
            PadControlSpec(control: .lb, x: 0.14, y: 0.55),
            PadControlSpec(control: .rb, x: 0.86, y: 0.55),
            PadControlSpec(control: .dpad, x: 0.24, y: 0.68),
            PadControlSpec(control: .leftStick, x: 0.26, y: 0.88),
            PadControlSpec(control: .face, x: 0.76, y: 0.70),
            PadControlSpec(control: .rightStick, x: 0.74, y: 0.89, hidden: true),
            PadControlSpec(control: .view, x: 0.42, y: 0.50),
            PadControlSpec(control: .menu, x: 0.58, y: 0.50),
        ])
    }

    /// Points per base point for a safe area (390 high is 1).
    static func unit(for safe: CGSize) -> Double {
        let short = Double(min(safe.width, safe.height))
        return min(max(short / 390, 0.75), 1.45)
    }

    /// A control's frame in a safe area (its origin at the safe area's top left).
    static func frame(_ s: PadControlSpec, in safe: CGRect) -> CGRect {
        let u = unit(for: safe.size) * s.scale
        let w = Double(s.control.baseSize.width) * u, h = Double(s.control.baseSize.height) * u
        let cx = Double(safe.minX) + s.x * Double(safe.width), cy = Double(safe.minY) + s.y * Double(safe.height)
        return CGRect(x: CGFloat(cx - w / 2), y: CGFloat(cy - h / 2), width: CGFloat(w), height: CGFloat(h))
    }
}

/// The touch math of the round and cross controls.
enum PadMath {
    /// A stick: the finger's offset from the centre over the stick's travel (its radius), as
    /// -1...1 with y up, limited to the circle.
    static func stick(dx: Double, dy: Double, radius: Double) -> (x: Double, y: Double) {
        guard radius > 0 else { return (0, 0) }
        var x = dx / radius, y = -dy / radius
        let m = (x * x + y * y).squareRoot()
        if m > 1 { x /= m; y /= m }
        return (x, y)
    }

    /// A D-pad: eight directions in 45° sectors, nothing inside the dead zone (a fraction of the radius).
    static func dpad(dx: Double, dy: Double, radius: Double, deadZone: Double = 0.22) -> UInt16 {
        let m = (dx * dx + dy * dy).squareRoot()
        guard radius > 0, m > radius * deadZone else { return 0 }
        let angle = atan2(-dy, dx) * 180 / .pi            // 0 right, 90 up
        let sector = Int(((angle + 360 + 22.5).truncatingRemainder(dividingBy: 360)) / 45)
        let bits: [UInt16] = [PadState.right, PadState.right | PadState.up, PadState.up, PadState.up | PadState.left,
                              PadState.left, PadState.left | PadState.down, PadState.down, PadState.down | PadState.right]
        return bits[min(max(sector, 0), 7)]
    }

    /// The face button nearest the finger: A at the bottom, B right, X left, Y top (Xbox).
    static func face(dx: Double, dy: Double) -> UInt16 {
        if abs(dx) > abs(dy) { return dx > 0 ? PadState.b : PadState.x }
        return dy > 0 ? PadState.a : PadState.y
    }
}
