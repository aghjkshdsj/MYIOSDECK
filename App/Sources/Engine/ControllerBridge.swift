// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController
import UIKit

/// Controllers (Bluetooth Xbox, DualSense, DualShock 4, Backbone, MFi) for Windows games: each
/// GCController's extended gamepad is sampled every 4 ms and published to Wine's iOS driver.
/// Player 1, as the game's controller choice says (PadPrefs; switchable while it plays):
/// - Xbox: an XInput controller (and MYIOSDECK's DirectInput pad for DirectInput-only games, set
///   when the game starts: WineController);
/// - PlayStation: XInput as well, and the DualSense HID controller Madeira's wineserver serves
///   (Sony's identity and reports: games with PlayStation support show its buttons; DirectInput
///   lists it). With the hidpad gate (engine/wine/patches/hidpad_gate.py) that device exists in
///   every session and is absent while nothing is published here, so the switch works both ways;
/// - keyboard and mouse: keys and mouse buttons, for games without controller support (Madeira's
///   PadKeyboardMouse defaults); the right stick moves the cursor (GameInput).
/// Players 2-4 stay XInput. The mapping is Madeira's (GamepadInput.swift): a PlayStation pad's
/// Cross / Circle / Square / Triangle are A / B / X / Y, Create is View, Options is Menu, the
/// touchpad click is the DualSense's own touchpad button (View on an Xbox controller).
/// While the app is inactive, pads stay connected with everything released. The game's rumble,
/// lightbar and trigger effects reach the pad through Madeira's PadOutput.m.
final class ControllerBridge: @unchecked Sendable {
    static let shared = ControllerBridge()

    enum Mode: String { case xbox, playstation, keyboard }

    private let queue = DispatchQueue(label: "myiosdeck.controllers", qos: .userInteractive)
    private var pads: [GCExtendedGamepad?] = [nil, nil, nil, nil]
    private var owners: [GCController?] = [nil, nil, nil, nil]
    private var active = true
    private var firstPress = [false, false, false, false]
    private var lastButtons: [UInt16] = [0, 0, 0, 0]
    private var mode = Mode.xbox
    private var heldKeys = Set<Int32>()      // keyboard mode: VKs currently down
    private var heldMouse: UInt16 = 0        // keyboard mode: 1 left, 2 right

    /// The on-screen controller (TouchPadOverlay): player 1, with a physical controller in that
    /// slot too if one is connected (PadState.merged). Connected while it is shown.
    private var touchConnected = false
    private var touchState = PadState()

    func setTouchPad(connected: Bool, _ state: PadState) {
        queue.async { [self] in
            let changed = connected != touchConnected
            touchConnected = connected
            touchState = connected ? state : PadState()
            if changed {
                dlog("[xinput] on-screen controller \(connected ? "connected" : "disconnected") as player 1")
                updateTimer()
            }
            sample()
        }
    }

    /// The app's UI has the controller (no game on screen: the library, the game menu): the game
    /// sees its pads connected with nothing pressed (PadUIInput).
    private var uiOwns = false

    func setUIOwnsPads(_ owns: Bool) {
        queue.async { [self] in
            guard owns != uiOwns else { return }
            uiOwns = owns
            dlog("[xinput] controller \(owns ? "drives the app's UI: the game sees it idle" : "back to the game")")
            sample()
        }
    }

    /// What player 1 is; takes effect at once (a game start sets it, and the game menu).
    func setMode(_ m: Mode) {
        queue.async { [self] in
            guard m != mode else { return }
            if mode == .keyboard { applyKeyboard(keys: [], mouse: 0) }
            dlog("[pad] player 1: \(m == .xbox ? "Xbox controller (XInput)" : m == .playstation ? "PlayStation controller (DualSense HID, and XInput)" : "keyboard and mouse")")
            mode = m
            updateTimer()
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
        if !keys.subtracting(heldKeys).isEmpty || mouse & ~heldMouse != 0 { mid_wow_trace_calls(3) }   // GameInput.traceInput
        for vk in heldKeys.subtracting(keys) { mid_post_key(vk, 0) }
        for vk in keys.subtracting(heldKeys) { mid_post_key(vk, 1) }
        heldKeys = keys
        let changed = heldMouse ^ mouse
        if changed & 1 != 0 { mid_post_mouse_button(mouse & 1 != 0 ? 0x0002 : 0x0004) }   // left down/up
        if changed & 2 != 0 { mid_post_mouse_button(mouse & 2 != 0 ? 0x0008 : 0x0010) }   // right down/up
        heldMouse = mouse
    }

    private func keyboard(_ s: PadState) {
        var keys = Set<Int32>()
        for (mask, vk) in Self.keyMap where s.buttons & mask != 0 { keys.insert(vk) }
        if s.ly > 16384 { keys.insert(0x57) }    // left stick: W
        if s.ly < -16384 { keys.insert(0x53) }   // S
        if s.lx < -16384 { keys.insert(0x41) }   // A
        if s.lx > 16384 { keys.insert(0x44) }    // D
        stickMouse(s.rx, s.ry)   // before the buttons: a click lands where the cursor went
        applyKeyboard(keys: keys, mouse: (s.rt > 128 ? 1 : 0) | (s.lt > 128 ? 2 : 0))
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
        // The game's rumble (XInput) and DualSense output reports (rumble, adaptive triggers,
        // lightbar, player LEDs) to the controller in that slot.
        mid_pad_output_configure(1, 1)
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            }
        }
        center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            mid_pad_output_set_active(0)
            self?.setActive(false)
        }
        center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
            mid_pad_output_set_active(1)
            self?.setActive(true)
        }
        mid_pad_output_set_active(1)
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
                let kind = p is GCDualSenseGamepad ? "DualSense" : p is GCDualShockGamepad ? "DualShock 4" : p is GCXboxGamepad ? "Xbox" : "controller"
                dlog("[xinput] slot=\(i) connected: \(c.vendorName ?? "controller") (\(kind))")
            }
            let slots = owners
            DispatchQueue.main.async {
                for (i, c) in slots.enumerated() {
                    mid_pad_output_set_slot(Int32(i), c.map { Unmanaged.passUnretained($0).toOpaque() })
                }
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
        let needed = active && (touchConnected || pads.contains(where: { $0 != nil }))
        if !needed {
            timer?.cancel(); timer = nil
            return
        }
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

    /// A PlayStation pad's touchpad click (nil for other controllers).
    private static func touchpad(_ pad: GCExtendedGamepad) -> Bool {
        if let ds = pad as? GCDualSenseGamepad { return ds.touchpadButton.isPressed }
        if let ds4 = pad as? GCDualShockGamepad { return ds4.touchpadButton.isPressed }
        return false
    }

    private func sample() {
        for i in pads.indices {
            let touch = i == 0 && touchConnected
            var state = PadState()
            var touchpadDown = false
            var connected = touch
            if let pad = pads[i] {
                connected = true
                if active, !uiOwns {
                    let map: [(GCControllerButtonInput?, UInt16)] = [
                        (pad.dpad.up, 0x0001), (pad.dpad.down, 0x0002), (pad.dpad.left, 0x0004), (pad.dpad.right, 0x0008),
                        (pad.buttonMenu, 0x0010), (pad.buttonOptions, 0x0020),
                        (pad.leftThumbstickButton, 0x0040), (pad.rightThumbstickButton, 0x0080),
                        (pad.leftShoulder, 0x0100), (pad.rightShoulder, 0x0200), (pad.buttonHome, 0x0400),
                        (pad.buttonA, 0x1000), (pad.buttonB, 0x2000), (pad.buttonX, 0x4000), (pad.buttonY, 0x8000),
                    ]
                    var buttons: UInt16 = 0
                    for (b, mask) in map where b?.isPressed == true { buttons |= mask }
                    touchpadDown = Self.touchpad(pad)
                    state = PadState(buttons: buttons,
                                     lt: Self.trigger(pad.leftTrigger.value), rt: Self.trigger(pad.rightTrigger.value),
                                     lx: Self.axis(pad.leftThumbstick.xAxis.value), ly: Self.axis(pad.leftThumbstick.yAxis.value),
                                     rx: Self.axis(pad.rightThumbstick.xAxis.value), ry: Self.axis(pad.rightThumbstick.yAxis.value))
                }
            }
            if touch, active, !uiOwns { state = state.merged(with: touchState) }
            let route: Mode = i == 0 ? mode : .xbox
            // On an Xbox controller (and XInput) the touchpad click is View.
            var xbuttons = state.buttons
            if touchpadDown { xbuttons |= PadState.back }
            if xbuttons != 0, lastButtons[i] == 0 { mid_wow_trace_calls(3) }   // GameInput.traceInput
            lastButtons[i] = xbuttons
            if xbuttons != 0, !firstPress[i] {
                firstPress[i] = true
                dlog("[xinput] slot=\(i) first press buttons=0x\(String(xbuttons, radix: 16)) as \(route.rawValue)")
            }
            switch route {
            case .xbox:
                mid_pad_set(Int32(i), connected ? 1 : 0, xbuttons, state.lt, state.rt, state.lx, state.ly, state.rx, state.ry)
                if i == 0 { mid_hidpad_set(0, 0, 0, 0, 0, 0, 0, 0) }
            case .playstation:
                mid_pad_set(Int32(i), connected ? 1 : 0, xbuttons, state.lt, state.rt, state.lx, state.ly, state.rx, state.ry)
                // The DualSense is there in PlayStation mode even with no controller (it is what a
                // game that looks for one at its start finds); the touchpad is its own button.
                let hidButtons = UInt32(state.buttons) | (touchpadDown ? 0x0001_0000 : 0)
                mid_hidpad_set(1, hidButtons, state.lt, state.rt, state.lx, state.ly, state.rx, state.ry)
            case .keyboard:
                mid_pad_set(Int32(i), 0, 0, 0, 0, 0, 0, 0, 0)
                mid_hidpad_set(0, 0, 0, 0, 0, 0, 0, 0)
                if active, !uiOwns { var k = state; k.buttons = xbuttons; keyboard(k) } else { keyboard(PadState()) }
            }
        }
    }
}
