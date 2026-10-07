// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController
import UIKit

/// Controllers (Bluetooth, Backbone, MFi) as XInput pads for Windows games: each
/// GCController's extended gamepad is sampled every 4 ms and published to Wine's
/// iOS driver (winios_gamepad_set_state). The mapping is Madeira's
/// (GamepadInput.swift); its touch controls, keyboard/mouse mode and HID
/// DualSense mode are not carried over. While the app is inactive the pads stay
/// connected with everything released.
final class ControllerBridge: @unchecked Sendable {
    static let shared = ControllerBridge()

    private let queue = DispatchQueue(label: "myiosdeck.controllers", qos: .userInteractive)
    private var pads: [GCExtendedGamepad?] = [nil, nil, nil, nil]
    private var owners: [GCController?] = [nil, nil, nil, nil]
    private var active = true
    private var firstPress = [false, false, false, false]
    /// HID sessions (Settings › Controller API: HID): player 1 feeds the HID
    /// gamepad and leaves XInput, as a HID pad on Windows is not an XInput pad.
    private var hidMode = false

    /// Called before a Wine session starts; the mode holds for that session.
    func setHIDMode(_ on: Bool) {
        queue.async { [self] in
            hidMode = on
            if !on { mid_hidpad_set(0, 0, 0, 0, 0, 0, 0, 0) }
            sample()
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
            let hid = i == 0 && hidMode
            // One publisher per slot: XInput, or (player 1 in HID mode) the HID gamepad.
            func publish(_ connected: Int32, _ b: UInt16, _ lt: UInt8, _ rt: UInt8,
                         _ lx: Int16, _ ly: Int16, _ rx: Int16, _ ry: Int16) {
                if hid {
                    mid_pad_set(Int32(i), 0, 0, 0, 0, 0, 0, 0, 0)
                    mid_hidpad_set(connected, b, lt, rt, lx, ly, rx, ry)
                } else {
                    mid_pad_set(Int32(i), connected, b, lt, rt, lx, ly, rx, ry)
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
                dlog("[xinput] slot=\(i) first press buttons=0x\(String(buttons, radix: 16)) published to \(hid ? "HID gamepad" : "XInput")")
            }
            publish(1, buttons,
                    Self.trigger(pad.leftTrigger.value), Self.trigger(pad.rightTrigger.value),
                    Self.axis(pad.leftThumbstick.xAxis.value), Self.axis(pad.leftThumbstick.yAxis.value),
                    Self.axis(pad.rightThumbstick.xAxis.value), Self.axis(pad.rightThumbstick.yAxis.value))
        }
    }
}
