// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController
import UIKit

/// Controllers (Bluetooth, Backbone, MFi) for Windows games: each GCController's
/// extended gamepad is sampled every 4 ms and published to Wine's iOS driver.
/// Player 1 goes where Settings › Controller API says (per session):
/// - XInput (also "+ DirectInput", which only adds a DirectInput view in Wine);
/// - HID: the virtual HID gamepad, leaving XInput as a HID pad on Windows does;
/// - Keyboard: keys and mouse buttons, for games whose controller support fails
///   (Madeira's PadKeyboardMouse defaults); the right stick moves the cursor (GameInput).
/// Players 2-4 stay XInput. The XInput mapping is Madeira's (GamepadInput.swift).
/// While the app is inactive, pads stay connected with everything released.
final class ControllerBridge: @unchecked Sendable {
    static let shared = ControllerBridge()

    enum Mode: String { case xinput, hid, keyboard }

    private let queue = DispatchQueue(label: "myiosdeck.controllers", qos: .userInteractive)
    private var pads: [GCExtendedGamepad?] = [nil, nil, nil, nil]
    private var owners: [GCController?] = [nil, nil, nil, nil]
    private var active = true
    private var firstPress = [false, false, false, false]
    private var mode = Mode.xinput
    private var heldKeys = Set<Int32>()      // keyboard mode: VKs currently down
    private var heldMouse: UInt16 = 0        // keyboard mode: 1 left, 2 right

    /// Called before a Wine session starts; the mode holds for that session.
    func setMode(_ m: Mode) {
        queue.async { [self] in
            if mode == .hid, m != .hid { mid_hidpad_set(0, 0, 0, 0, 0, 0, 0, 0) }
            if mode == .keyboard, m != .keyboard { applyKeyboard(keys: [], mouse: 0) }
            mode = m
            sample()
        }
    }

    // Keyboard mode bindings: XINPUT_GAMEPAD_* bit -> Windows virtual key.
    private static let keyMap: [(UInt16, Int32)] = [
        (0x1000, 0x20), (0x2000, 0x11), (0x4000, 0x45), (0x8000, 0x52),  // A Space, B Ctrl, X E, Y R
        (0x0100, 0x51), (0x0200, 0x46),                                  // LB Q, RB F
        (0x0040, 0x10), (0x0080, 0x43),                                  // L3 Shift, R3 C
        (0x0010, 0x1B), (0x0020, 0x0D),                                  // Menu Esc, Options Enter
        (0x0001, 0x26), (0x0002, 0x28), (0x0004, 0x25), (0x0008, 0x27),  // D-pad arrows
    ]

    /// Releases and presses only what changed since the last sample.
    private func applyKeyboard(keys: Set<Int32>, mouse: UInt16) {
        for vk in heldKeys.subtracting(keys) { mid_post_key(vk, 0) }
        for vk in keys.subtracting(heldKeys) { mid_post_key(vk, 1) }
        heldKeys = keys
        let changed = heldMouse ^ mouse
        if changed & 1 != 0 { mid_post_mouse_button(mouse & 1 != 0 ? 0x0002 : 0x0004) }   // left down/up
        if changed & 2 != 0 { mid_post_mouse_button(mouse & 2 != 0 ? 0x0008 : 0x0010) }   // right down/up
        heldMouse = mouse
    }

    private func keyboard(_ b: UInt16, _ lt: UInt8, _ rt: UInt8, _ lx: Int16, _ ly: Int16, _ rx: Int16, _ ry: Int16) {
        var keys = Set<Int32>()
        for (mask, vk) in Self.keyMap where b & mask != 0 { keys.insert(vk) }
        if ly > 16384 { keys.insert(0x57) }    // left stick: W
        if ly < -16384 { keys.insert(0x53) }   // S
        if lx < -16384 { keys.insert(0x41) }   // A
        if lx > 16384 { keys.insert(0x44) }    // D
        stickMouse(rx, ry)   // before the buttons: a click lands where the cursor went
        applyKeyboard(keys: keys, mouse: (rt > 128 ? 1 : 0) | (lt > 128 ? 2 : 0))
    }

    // Keyboard mode: the right stick moves the cursor (Madeira's PadStickMouse), up to
    // 1400 desktop pixels a second at full tilt on a quadratic curve past a 15% dead zone,
    // so menus and launchers can be clicked with the triggers. Fractions carry over.
    private var stickCarry = (x: 0.0, y: 0.0)
    private func stickMouse(_ rx: Int16, _ ry: Int16) {
        func speed(_ v: Int16) -> Double {
            let f = max(-1, min(1, Double(v) / 32767)), dead = 0.15
            guard abs(f) > dead else { return 0 }
            let t = (abs(f) - dead) / (1 - dead)
            return (f < 0 ? -1 : 1) * t * t * 1400
        }
        stickCarry.x += speed(rx) * 0.004
        stickCarry.y -= speed(ry) * 0.004   // stick up (+y) moves the cursor up the screen
        let dx = stickCarry.x.rounded(.towardZero), dy = stickCarry.y.rounded(.towardZero)
        guard dx != 0 || dy != 0 else { return }
        stickCarry.x -= dx; stickCarry.y -= dy
        DispatchQueue.main.async {
            MainActor.assumeIsolated { GameInput.shared.stickMoved(dx: CGFloat(dx), dy: CGFloat(dy)) }
        }
    }
    private var timer: DispatchSourceTimer?
    private var started = false

    @MainActor func start() {
        guard !started else { return }
        started = true
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            }
        }
        center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.setActive(false)
        }
        center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
            self?.setActive(true)
        }
        refresh()
    }

    @MainActor private func refresh() {
        let live = GCController.controllers().compactMap { c -> (GCController, GCExtendedGamepad)? in
            guard let p = c.extendedGamepad else { return nil }
            return (c, p)
        }
        queue.async { [self] in
            for i in owners.indices {
                guard let old = owners[i], !live.contains(where: { $0.0 === old }) else { continue }
                owners[i] = nil; pads[i] = nil
                dlog("[xinput] slot=\(i) disconnected")
            }
            for (c, p) in live {
                guard !owners.contains(where: { $0 === c }), let i = owners.firstIndex(where: { $0 == nil }) else { continue }
                owners[i] = c; pads[i] = p
                dlog("[xinput] slot=\(i) connected: \(c.vendorName ?? "controller")")
            }
            updateTimer()
            sample()
        }
    }

    private func setActive(_ value: Bool) {
        queue.async { [self] in
            active = value
            updateTimer()
            sample()
        }
    }

    private func updateTimer() {
        let needed = active && pads.contains(where: { $0 != nil })
        if !needed { timer?.cancel(); timer = nil; return }
        guard timer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .milliseconds(4), leeway: .milliseconds(1))
        t.setEventHandler { [weak self] in self?.sample() }
        timer = t
        t.resume()
    }

    // XInput leaves dead zones to the game: keep the full signed range.
    private static func axis(_ v: Float) -> Int16 {
        guard v.isFinite else { return 0 }
        let c = max(-1, min(1, v))
        return Int16((c * (c < 0 ? 32768 : 32767)).rounded())
    }
    private static func trigger(_ v: Float) -> UInt8 {
        guard v.isFinite else { return 0 }
        return UInt8((max(0, min(1, v)) * 255).rounded())
    }

    private func sample() {
        for i in pads.indices {
            let route: Mode = i == 0 ? mode : .xinput
            let hid = route == .hid
            // One publisher per slot: XInput, the HID gamepad, or keys (player 1).
            func publish(_ connected: Int32, _ b: UInt16, _ lt: UInt8, _ rt: UInt8,
                         _ lx: Int16, _ ly: Int16, _ rx: Int16, _ ry: Int16) {
                switch route {
                case .xinput:
                    mid_pad_set(Int32(i), connected, b, lt, rt, lx, ly, rx, ry)
                case .hid:
                    mid_pad_set(Int32(i), 0, 0, 0, 0, 0, 0, 0, 0)
                    mid_hidpad_set(connected, b, lt, rt, lx, ly, rx, ry)
                case .keyboard:
                    mid_pad_set(Int32(i), 0, 0, 0, 0, 0, 0, 0, 0)
                    keyboard(b, lt, rt, lx, ly, rx, ry)
                }
            }
            guard let pad = pads[i] else { publish(0, 0, 0, 0, 0, 0, 0, 0); continue }
            guard active else { publish(1, 0, 0, 0, 0, 0, 0, 0); continue }
            let map: [(GCControllerButtonInput?, UInt16)] = [
                (pad.dpad.up, 0x0001), (pad.dpad.down, 0x0002), (pad.dpad.left, 0x0004), (pad.dpad.right, 0x0008),
                (pad.buttonMenu, 0x0010), (pad.buttonOptions, 0x0020),
                (pad.leftThumbstickButton, 0x0040), (pad.rightThumbstickButton, 0x0080),
                (pad.leftShoulder, 0x0100), (pad.rightShoulder, 0x0200), (pad.buttonHome, 0x0400),
                (pad.buttonA, 0x1000), (pad.buttonB, 0x2000), (pad.buttonX, 0x4000), (pad.buttonY, 0x8000),
            ]
            var buttons: UInt16 = 0
            for (b, mask) in map where b?.isPressed == true { buttons |= mask }
            if buttons != 0, !firstPress[i] {
                firstPress[i] = true
                dlog("[xinput] slot=\(i) first press buttons=0x\(String(buttons, radix: 16)) published to \(hid ? "HID gamepad" : route == .keyboard ? "keyboard" : "XInput")")
            }
            publish(1, buttons,
                    Self.trigger(pad.leftTrigger.value), Self.trigger(pad.rightTrigger.value),
                    Self.axis(pad.leftThumbstick.xAxis.value), Self.axis(pad.leftThumbstick.yAxis.value),
                    Self.axis(pad.rightThumbstick.xAxis.value), Self.axis(pad.rightThumbstick.yAxis.value))
        }
    }
}
