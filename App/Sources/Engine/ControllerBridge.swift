// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// ControllerBridge: game controllers (GameController framework) to Windows input.
// Written from specs/01-input.md, part A. Calls only the C functions in interfaces/wine_host.h
// and the app names in interfaces/app-api.md (dlog, GameInput).
//
// Threading: every piece of mutable state below is touched only on `queue`, except `started`
// and `observers`, which only `start()` (main actor) touches. Keyboard-mode output is handed to
// the main queue in one block per sample, so cursor moves and button presses reach Windows in
// the order the spec asks for (cursor first). Keys and buttons go through GameInput, which
// knows when another source (hardware keyboard or mouse) holds the same one.

import Foundation
import GameController
import UIKit

final class ControllerBridge {
    static let shared = ControllerBridge()

    /// Where player 1 goes.
    enum Mode: String { case xinput, hid, keyboard }

    /// One controller sample in XInput units (XINPUT_GAMEPAD layout).
    struct PadState: Equatable {
        var buttons: UInt16 = 0
        var lt: UInt8 = 0
        var rt: UInt8 = 0
        var lx: Int16 = 0
        var ly: Int16 = 0
        var rx: Int16 = 0
        var ry: Int16 = 0

        static let neutral = PadState()
    }

    /// What keyboard mode wants held for one sample.
    struct KeyboardOutput: Equatable {
        var keys: Set<Int> = []
        var leftButton = false
        var rightButton = false
    }

    /// XINPUT_GAMEPAD_* button bits.
    enum XButton {
        static let dpadUp: UInt16 = 0x0001
        static let dpadDown: UInt16 = 0x0002
        static let dpadLeft: UInt16 = 0x0004
        static let dpadRight: UInt16 = 0x0008
        static let start: UInt16 = 0x0010
        static let back: UInt16 = 0x0020
        static let leftThumb: UInt16 = 0x0040
        static let rightThumb: UInt16 = 0x0080
        static let leftShoulder: UInt16 = 0x0100
        static let rightShoulder: UInt16 = 0x0200
        static let guide: UInt16 = 0x0400
        static let a: UInt16 = 0x1000
        static let b: UInt16 = 0x2000
        static let x: UInt16 = 0x4000
        static let y: UInt16 = 0x8000
    }

    static let slotCount = 4
    static let samplePeriod = 0.004           // seconds between samples
    static let stickKeyThreshold: Int16 = 16384
    static let triggerButtonThreshold: UInt8 = 128
    static let cursorDeadZone = 0.15
    static let cursorTopSpeed = 1400.0        // desktop pixels per second at full deflection

    /// Keyboard mode: XInput button bit → Windows virtual key.
    static let keyboardButtonKeys: [(bit: UInt16, vk: Int)] = [
        (XButton.a, VK.space),
        (XButton.b, VK.control),
        (XButton.x, VK.keyE),
        (XButton.y, VK.keyR),
        (XButton.leftShoulder, VK.keyQ),
        (XButton.rightShoulder, VK.keyF),
        (XButton.leftThumb, VK.shift),
        (XButton.rightThumb, VK.keyC),
        (XButton.start, VK.escape),
        (XButton.back, VK.returnKey),
        (XButton.dpadUp, VK.up),
        (XButton.dpadDown, VK.down),
        (XButton.dpadLeft, VK.left),
        (XButton.dpadRight, VK.right),
    ]

    // MARK: - State (bridge queue only)

    private struct Slot {
        let controller: GCController
        var lastButtons: UInt16 = 0
        var loggedFirstPress = false
    }

    private let queue = DispatchQueue(label: "myiosdeck.controller-bridge", qos: .userInteractive)
    private var slots: [Slot?] = Array(repeating: nil, count: ControllerBridge.slotCount)
    private var waiting: Set<ObjectIdentifier> = []   // connected, no slot, already logged
    private var mode: Mode = .xinput
    private var appActive = false
    private var timer: DispatchSourceTimer?

    // Keyboard mode
    private var held = KeyboardOutput()
    private var cursorCarryX = 0.0
    private var cursorCarryY = 0.0

    // Main actor only
    private var started = false
    private var observers: [NSObjectProtocol] = []

    private init() {}

    // MARK: - Public API

    /// Player 1's route for this session. Any thread.
    func setMode(_ m: Mode) {
        queue.async { [self] in
            let old = mode
            if old == .hid && m != .hid {
                mid_hidpad_set(0, 0, 0, 0, 0, 0, 0, 0)
            }
            if old == .keyboard && m != .keyboard {
                releaseKeyboardMode()
            }
            mode = m
            sampleAll()
        }
    }

    /// Idempotent. Watches controllers and the app's active state.
    @MainActor func start() {
        guard !started else { return }
        started = true

        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            guard let self else { return }
            var connected = GCController.controllers()
            if let controller = note.object as? GCController, !connected.contains(where: { $0 === controller }) {
                connected.append(controller)
            }
            self.queue.async { self.syncSlots(connected: connected, gone: nil) }
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
            guard let self else { return }
            let connected = GCController.controllers()
            let gone = note.object as? GCController
            self.queue.async { self.syncSlots(connected: connected, gone: gone) }
        })
        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.setAppActive(true) }
        })
        observers.append(nc.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.setAppActive(false) }
        })

        // Controllers already connected before we started, and the state we start in. This block
        // is queued before any of the notifications above can be delivered.
        let active = UIApplication.shared.applicationState == .active
        let current = GCController.controllers()
        queue.async { [self] in
            appActive = active
            syncSlots(connected: current, gone: nil)
        }
    }

    // MARK: - Pure conversions (tested in TESTS.md)

    /// Trigger 0...1 → 0...255, rounded and clamped; non-finite → 0.
    static func triggerValue(_ v: Float) -> UInt8 {
        guard v.isFinite else { return 0 }
        let scaled = (Double(v) * 255).rounded()
        return UInt8(min(max(scaled, 0), 255))
    }

    /// Stick axis -1...1 → full signed 16-bit range (negative × 32768, positive × 32767),
    /// rounded and clamped; non-finite → 0. No dead zone.
    static func axisValue(_ v: Float) -> Int16 {
        guard v.isFinite else { return 0 }
        let d = Double(v)
        let scaled = (d < 0 ? d * 32768 : d * 32767).rounded()
        return Int16(min(max(scaled, -32768), 32767))
    }

    /// Keyboard-mode cursor curve for one right-stick axis: fraction of full scale, zero inside
    /// the 15% dead zone, the rest rescaled to 0...1 and squared, sign kept. Result -1...1.
    static func cursorCurve(_ axis: Int16) -> Double {
        let v = Double(axis)
        let fraction = v < 0 ? v / 32768 : v / 32767
        let magnitude = abs(fraction)
        guard magnitude > cursorDeadZone else { return 0 }
        let r = min((magnitude - cursorDeadZone) / (1 - cursorDeadZone), 1)
        return fraction < 0 ? -(r * r) : r * r
    }

    /// Cursor motion for one sample, in desktop pixels (screen y grows downward, so stick up
    /// gives a negative dy).
    static func cursorStep(rx: Int16, ry: Int16) -> (dx: Double, dy: Double) {
        let perSample = cursorTopSpeed * samplePeriod
        return (cursorCurve(rx) * perSample, -cursorCurve(ry) * perSample)
    }

    /// Keys and mouse buttons keyboard mode holds for this controller state.
    static func keyboardOutput(for s: PadState) -> KeyboardOutput {
        var out = KeyboardOutput()
        for entry in keyboardButtonKeys where s.buttons & entry.bit != 0 {
            out.keys.insert(entry.vk)
        }
        if s.lx > stickKeyThreshold { out.keys.insert(VK.keyD) }
        if s.lx < -stickKeyThreshold { out.keys.insert(VK.keyA) }
        if s.ly > stickKeyThreshold { out.keys.insert(VK.keyW) }
        if s.ly < -stickKeyThreshold { out.keys.insert(VK.keyS) }
        out.leftButton = s.rt > triggerButtonThreshold
        out.rightButton = s.lt > triggerButtonThreshold
        return out
    }

    // MARK: - Slots

    private static func name(of controller: GCController) -> String {
        controller.vendorName ?? "unknown"
    }

    /// Runs on every connect or disconnect notification (and once at start). `connected` is
    /// GameController's list at that moment; `gone` is the controller that just disconnected.
    /// Frees `gone`'s slot, then every connected extended gamepad without a slot takes the
    /// lowest free one, in the order GameController lists them. One that finds none waits,
    /// logged once, and gets a slot on a later notification once one is free.
    private func syncSlots(connected: [GCController], gone: GCController?) {
        if let gone, let index = slots.firstIndex(where: { $0?.controller === gone }) {
            slots[index] = nil
            dlog("[xinput] slot=\(index) disconnected")
        }

        let candidates = connected.filter { $0 !== gone && $0.extendedGamepad != nil }
        for controller in candidates where !slots.contains(where: { $0?.controller === controller }) {
            let id = ObjectIdentifier(controller)
            guard let index = slots.firstIndex(where: { $0 == nil }) else {
                if waiting.insert(id).inserted {
                    dlog("[xinput] no free slot for \(Self.name(of: controller))")
                }
                continue
            }
            waiting.remove(id)
            slots[index] = Slot(controller: controller)
            dlog("[xinput] slot=\(index) connected: \(Self.name(of: controller))")
        }
        // Forget waiters that went away, so they log again if they come back and wait.
        let present = Set(candidates.map { ObjectIdentifier($0) })
        waiting = waiting.intersection(present)

        refresh()
    }

    private func setAppActive(_ active: Bool) {
        appActive = active
        refresh()
    }

    /// Publish every slot now, then start or stop the sample timer to match.
    private func refresh() {
        sampleAll()
        updateTimer()
    }

    private func updateTimer() {
        let wanted = appActive && slots.contains(where: { $0 != nil })
        if wanted, timer == nil {
            let t = DispatchSource.makeTimerSource(queue: queue)
            t.schedule(deadline: .now() + .milliseconds(4), repeating: .milliseconds(4), leeway: .milliseconds(1))
            t.setEventHandler { [weak self] in self?.sampleAll() }
            t.resume()
            timer = t
        } else if !wanted, let t = timer {
            t.cancel()
            timer = nil
        }
    }

    // MARK: - Sampling

    private func sampleAll() {
        for index in 0..<Self.slotCount {
            sample(slot: index)
        }
    }

    private func sample(slot index: Int) {
        guard let slot = slots[index] else {
            publish(slot: index, connected: false, .neutral)
            return
        }
        // While inactive, a filled slot stays plugged in with nothing pressed.
        let state = appActive ? Self.read(slot.controller) : .neutral

        if state.buttons != 0 {
            if slot.lastButtons == 0 {
                mid_wow_trace_calls(3)
            }
            if !slot.loggedFirstPress {
                slots[index]?.loggedFirstPress = true
                let hex = String(state.buttons, radix: 16)
                dlog("[xinput] slot=\(index) first press buttons=0x\(hex) published to \(destinationName(slot: index))")
            }
        }
        slots[index]?.lastButtons = state.buttons

        publish(slot: index, connected: true, state)
    }

    private func destinationName(slot index: Int) -> String {
        guard index == 0 else { return "XInput" }
        switch mode {
        case .xinput: return "XInput"
        case .hid: return "HID gamepad"
        case .keyboard: return "keyboard"
        }
    }

    private func publish(slot index: Int, connected: Bool, _ s: PadState) {
        let c: Int32 = connected ? 1 : 0
        if index > 0 || mode == .xinput {
            mid_pad_set(Int32(index), c, s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry)
            return
        }
        // Player 1 is routed elsewhere: the XInput slot reads as unplugged.
        mid_pad_set(0, 0, 0, 0, 0, 0, 0, 0, 0)
        switch mode {
        case .hid:
            mid_hidpad_set(c, s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry)
        case .keyboard:
            applyKeyboardMode(connected ? s : .neutral)
        case .xinput:
            break
        }
    }

    private static func read(_ controller: GCController) -> PadState {
        guard let pad = controller.extendedGamepad else { return .neutral }
        var buttons: UInt16 = 0
        func mark(_ pressed: Bool, _ bit: UInt16) {
            if pressed { buttons |= bit }
        }
        mark(pad.dpad.up.isPressed, XButton.dpadUp)
        mark(pad.dpad.down.isPressed, XButton.dpadDown)
        mark(pad.dpad.left.isPressed, XButton.dpadLeft)
        mark(pad.dpad.right.isPressed, XButton.dpadRight)
        mark(pad.buttonA.isPressed, XButton.a)
        mark(pad.buttonB.isPressed, XButton.b)
        mark(pad.buttonX.isPressed, XButton.x)
        mark(pad.buttonY.isPressed, XButton.y)
        mark(pad.leftShoulder.isPressed, XButton.leftShoulder)
        mark(pad.rightShoulder.isPressed, XButton.rightShoulder)
        mark(pad.leftThumbstickButton?.isPressed ?? false, XButton.leftThumb)
        mark(pad.rightThumbstickButton?.isPressed ?? false, XButton.rightThumb)
        mark(pad.buttonMenu.isPressed, XButton.start)
        mark(pad.buttonOptions?.isPressed ?? false, XButton.back)
        mark(pad.buttonHome?.isPressed ?? false, XButton.guide)

        return PadState(
            buttons: buttons,
            lt: triggerValue(pad.leftTrigger.value),
            rt: triggerValue(pad.rightTrigger.value),
            lx: axisValue(pad.leftThumbstick.xAxis.value),
            ly: axisValue(pad.leftThumbstick.yAxis.value),
            rx: axisValue(pad.rightThumbstick.xAxis.value),
            ry: axisValue(pad.rightThumbstick.yAxis.value)
        )
    }

    // MARK: - Keyboard mode (player 1)

    private enum KeyboardOp {
        case moveCursor(dx: Int, dy: Int)
        case key(vk: Int, down: Bool)
        case mouseButton(right: Bool, down: Bool)
        case trace
    }

    /// GameInput.releaseAll has let go of everything keyboard mode held. Forget it here too,
    /// so a button still down is pressed again on the next sample. Any thread.
    func forgetKeyboardModeHolds() {
        queue.async { [self] in
            held = KeyboardOutput()
            cursorCarryX = 0
            cursorCarryY = 0
        }
    }

    private func applyKeyboardMode(_ s: PadState) {
        var ops: [KeyboardOp] = []

        // Cursor first, so a trigger click in this same sample lands where the cursor went.
        let step = Self.cursorStep(rx: s.rx, ry: s.ry)
        cursorCarryX += step.dx
        cursorCarryY += step.dy
        let wholeX = cursorCarryX.rounded(.towardZero)
        let wholeY = cursorCarryY.rounded(.towardZero)
        cursorCarryX -= wholeX
        cursorCarryY -= wholeY
        if wholeX != 0 || wholeY != 0 {
            ops.append(.moveCursor(dx: Int(wholeX), dy: Int(wholeY)))
        }

        ops += transition(to: Self.keyboardOutput(for: s))
        perform(ops)
    }

    /// Release everything keyboard mode pressed and forget the cursor remainder.
    private func releaseKeyboardMode() {
        cursorCarryX = 0
        cursorCarryY = 0
        perform(transition(to: KeyboardOutput()))
    }

    /// Releases for what is no longer wanted, then presses for what is newly wanted.
    private func transition(to want: KeyboardOutput) -> [KeyboardOp] {
        var ops: [KeyboardOp] = []

        for vk in held.keys.subtracting(want.keys).sorted() {
            ops.append(.key(vk: vk, down: false))
        }
        if held.leftButton && !want.leftButton { ops.append(.mouseButton(right: false, down: false)) }
        if held.rightButton && !want.rightButton { ops.append(.mouseButton(right: true, down: false)) }

        let newKeys = want.keys.subtracting(held.keys).sorted()
        let newLeft = want.leftButton && !held.leftButton
        let newRight = want.rightButton && !held.rightButton
        if !newKeys.isEmpty || newLeft || newRight {
            ops.append(.trace)
        }
        for vk in newKeys {
            ops.append(.key(vk: vk, down: true))
        }
        if newLeft { ops.append(.mouseButton(right: false, down: true)) }
        if newRight { ops.append(.mouseButton(right: true, down: true)) }

        held = want
        return ops
    }

    private func perform(_ ops: [KeyboardOp]) {
        guard !ops.isEmpty else { return }
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                for op in ops {
                    switch op {
                    case let .moveCursor(dx, dy):
                        GameInput.shared.stickMoved(dx: CGFloat(dx), dy: CGFloat(dy))
                    // Through GameInput, which tracks every source holding a key or button
                    // (clarification 27).
                    case let .key(vk, down):
                        GameInput.shared.keyboardModeKey(vk, down: down)
                    case let .mouseButton(right, down):
                        GameInput.shared.keyboardModeButton(right: right, down: down)
                    case .trace:
                        mid_wow_trace_calls(3)
                    }
                }
            }
        }
    }
}

// MARK: - Windows constants used here (Microsoft "Virtual-Key Codes")

private enum VK {
    static let returnKey = 0x0D
    static let shift = 0x10
    static let control = 0x11
    static let escape = 0x1B
    static let space = 0x20
    static let left = 0x25
    static let up = 0x26
    static let right = 0x27
    static let down = 0x28
    static let keyA = 0x41
    static let keyC = 0x43
    static let keyD = 0x44
    static let keyE = 0x45
    static let keyF = 0x46
    static let keyQ = 0x51
    static let keyR = 0x52
    static let keyS = 0x53
    static let keyW = 0x57
}
