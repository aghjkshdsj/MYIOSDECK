// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

import Foundation
import GameController
import UIKit

// Internal value functions: no device, queue, UIKit, or C-bridge dependencies.
enum BPadMath {
    static func trigger(_ value: Float) -> UInt8 {
        guard value.isFinite else { return 0 }
        return UInt8((min(1, max(0, value)) * 255).rounded())
    }

    static func axis(_ value: Float) -> Int16 {
        guard value.isFinite else { return 0 }
        let v = Double(min(1, max(-1, value)))
        return Int16((v * (v < 0 ? 32768 : 32767)).rounded())
    }

    static func curve(_ value: Int16) -> Double {
        let v = Double(value) / (value < 0 ? 32768 : 32767)
        let amount = max(0, (abs(v) - 0.15) / 0.85)
        return (v < 0 ? -1 : 1) * amount * amount
    }

    static func keys(buttons: UInt16, lx: Int16, ly: Int16) -> Set<Int32> {
        let mapping: [(UInt16, Int32)] = [
            (0x1000, 0x20), (0x2000, 0x11), (0x4000, 0x45), (0x8000, 0x52),
            (0x0100, 0x51), (0x0200, 0x46), (0x0040, 0x10), (0x0080, 0x43),
            (0x0010, 0x1b), (0x0020, 0x0d), (0x0001, 0x26), (0x0002, 0x28),
            (0x0004, 0x25), (0x0008, 0x27)
        ]
        var result = Set(mapping.compactMap { buttons & $0.0 != 0 ? $0.1 : nil })
        if lx < -16384 { result.insert(0x41) }
        if lx > 16384 { result.insert(0x44) }
        if ly < -16384 { result.insert(0x53) }
        if ly > 16384 { result.insert(0x57) }
        return result
    }
}

final class ControllerBridge {
    static let shared = ControllerBridge()
    enum Mode: String { case xinput, hid, keyboard }

    struct Pad {
        var connected = false
        var buttons: UInt16 = 0
        var lt: UInt8 = 0
        var rt: UInt8 = 0
        var lx: Int16 = 0
        var ly: Int16 = 0
        var rx: Int16 = 0
        var ry: Int16 = 0
    }

    let queue = DispatchQueue(label: "myiosdeck.trackB.controllers", qos: .userInteractive)
    // Except observer installation, all mutable state below belongs to queue.
    private var slots = [GCController?](repeating: nil, count: 4)
    private var waiting = Set<ObjectIdentifier>()
    private var firstPress = [Bool](repeating: false, count: 4)
    private var previousButtons = [UInt16](repeating: 0, count: 4)
    private var mode: Mode = .xinput
    private var active = false
    private var timer: DispatchSourceTimer?
    private var carryX = 0.0
    private var carryY = 0.0
    @MainActor private var started = false
    @MainActor private var observers: [NSObjectProtocol] = []

    private init() {}

    func setMode(_ m: Mode) {
        queue.async {
            if self.mode != m {
                if self.mode == .hid { self.publishHID(Pad()) }
                if self.mode == .keyboard {
                    DispatchQueue.main.async { GameInput.shared.bClearController() }
                }
                self.carryX = 0
                self.carryY = 0
                self.mode = m
            }
            self.sample()
        }
    }

    @MainActor func start() {
        guard !started else { return }
        started = true
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                // Snapshot ordering on main ensures every notification can promote waiters.
                let devices = GCController.controllers()
                self?.queue.async { self?.reconcile(devices) }
            })
        }
        for name in [UIApplication.didBecomeActiveNotification, UIApplication.willResignActiveNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                let isActive = note.name == UIApplication.didBecomeActiveNotification
                self?.queue.async {
                    guard let self = self else { return }
                    self.active = isActive
                    self.carryX = 0
                    self.carryY = 0
                    self.sample()
                    self.updateTimer()
                }
            })
        }
        let devices = GCController.controllers()
        let isActive = UIApplication.shared.applicationState == .active
        queue.async {
            self.active = isActive
            self.reconcile(devices)
        }
    }

    // Called by GameInput.releaseAll; no cross-component call occurs under a lock.
    func bResetCursorCarry() {
        queue.async { self.carryX = 0; self.carryY = 0 }
    }

    private func reconcile(_ devices: [GCController]) {
        let eligible = devices.filter { $0.extendedGamepad != nil }
        let live = Set(eligible.map { ObjectIdentifier($0) })
        waiting.formIntersection(live)
        for index in slots.indices {
            if let old = slots[index], !live.contains(ObjectIdentifier(old)) {
                slots[index] = nil
                firstPress[index] = false
                previousButtons[index] = 0
                dlog("[xinput] slot=\(index) disconnected")
                // Publish the disconnection before assigning a waiting controller.
                publish(Pad(), slot: index)
                if index == 0 { carryX = 0; carryY = 0 }
            }
        }
        for device in eligible {
            guard !slots.contains(where: { $0 === device }) else { continue }
            let id = ObjectIdentifier(device)
            if let index = slots.firstIndex(where: { $0 == nil }) {
                device.handlerQueue = queue
                slots[index] = device
                waiting.remove(id)
                firstPress[index] = false
                previousButtons[index] = 0
                dlog("[xinput] slot=\(index) connected: \(device.vendorName ?? "unknown")")
            } else if waiting.insert(id).inserted {
                dlog("[xinput] no free slot for \(device.vendorName ?? "unknown")")
            }
        }
        sample()
        updateTimer()
    }

    func updateTimer() {
        let needsTimer = active && (slots.contains(where: { $0 != nil }) || TrackBPadIntegration.shared.hasTouch)
        if !needsTimer { timer?.cancel(); timer = nil; return }
        guard timer == nil else { return }
        let source = DispatchSource.makeTimerSource(queue: queue)
        source.schedule(deadline: .now(), repeating: .milliseconds(4), leeway: .milliseconds(1))
        source.setEventHandler { [weak self] in self?.sample() }
        timer = source
        source.resume()
    }

    private func read(_ controller: GCController) -> Pad {
        guard let g = controller.extendedGamepad else { return Pad() }
        var p = Pad(connected: true)
        let inputs: [(GCControllerButtonInput?, UInt16)] = [
            (g.dpad.up, 0x0001), (g.dpad.down, 0x0002), (g.dpad.left, 0x0004), (g.dpad.right, 0x0008),
            (g.buttonMenu, 0x0010), (g.buttonOptions, 0x0020),
            (g.leftThumbstickButton, 0x0040), (g.rightThumbstickButton, 0x0080),
            (g.leftShoulder, 0x0100), (g.rightShoulder, 0x0200), (g.buttonHome, 0x0400),
            (g.buttonA, 0x1000), (g.buttonB, 0x2000), (g.buttonX, 0x4000), (g.buttonY, 0x8000)
        ]
        for (input, bit) in inputs where input?.isPressed == true { p.buttons |= bit }
        p.lt = BPadMath.trigger(g.leftTrigger.value)
        p.rt = BPadMath.trigger(g.rightTrigger.value)
        p.lx = BPadMath.axis(g.leftThumbstick.xAxis.value)
        p.ly = BPadMath.axis(g.leftThumbstick.yAxis.value)
        p.rx = BPadMath.axis(g.rightThumbstick.xAxis.value)
        p.ry = BPadMath.axis(g.rightThumbstick.yAxis.value)
        return p
    }

    func sample() {
        for index in slots.indices {
            var p = Pad()
            if let device = slots[index] {
                p = active ? read(device) : Pad(connected: true)
            }
            p = TrackBPadIntegration.shared.adapt(p, slot: index, active: active)
            if p.buttons != 0 {
                if !firstPress[index] {
                    firstPress[index] = true
                    let route = index != 0 || mode == .xinput ? "XInput" : (mode == .hid ? "HID gamepad" : "keyboard")
                    dlog("[xinput] slot=\(index) first press buttons=0x\(String(p.buttons, radix: 16)) published to \(route)")
                }
                if previousButtons[index] == 0 { mid_wow_trace_calls(3) }
            }
            previousButtons[index] = p.buttons
            publish(p, slot: index)
        }
    }

    private func publishHID(_ p: Pad) {
        mid_hidpad_set(p.connected ? 1 : 0, p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry)
    }

    private func publish(_ p: Pad, slot: Int) {
        let xinput = slot != 0 || mode == .xinput
        mid_pad_set(Int32(slot), xinput && p.connected ? 1 : 0,
                    xinput ? p.buttons : 0, xinput ? p.lt : 0, xinput ? p.rt : 0,
                    xinput ? p.lx : 0, xinput ? p.ly : 0, xinput ? p.rx : 0, xinput ? p.ry : 0)
        guard slot == 0 else { return }
        if mode == .hid { publishHID(p) }
        if mode == .keyboard {
            if !p.connected || !active { carryX = 0; carryY = 0 }
            carryX += BPadMath.curve(p.rx) * 1400 * 0.004
            carryY -= BPadMath.curve(p.ry) * 1400 * 0.004
            let dx = carryX.rounded(.towardZero)
            let dy = carryY.rounded(.towardZero)
            carryX -= dx
            carryY -= dy
            let keys = BPadMath.keys(buttons: p.buttons, lx: p.lx, ly: p.ly)
            var buttons = Set<UInt32>()
            if p.rt > 128 { buttons.insert(0x0002) }
            if p.lt > 128 { buttons.insert(0x0008) }
            DispatchQueue.main.async {
                GameInput.shared.bApplyController(dx: CGFloat(dx), dy: CGFloat(dy), keys: keys, buttons: buttons)
            }
        }
    }
}
