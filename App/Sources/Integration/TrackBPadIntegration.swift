// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

// Owned by ControllerBridge.queue; keeps app UI capture and touch-pad composition outside
// the clean-room input implementation. Package sampling invokes one integration hook.
final class TrackBPadIntegration {
    static let shared = TrackBPadIntegration()
    var hasTouch = false
    var touch = PadState()
    var uiOwnsPads = false
    func adapt(_ input: ControllerBridge.Pad, slot: Int, active: Bool) -> ControllerBridge.Pad {
        var result = input
        if uiOwnsPads { return ControllerBridge.Pad(connected: input.connected) }
        guard slot == 0, hasTouch else { return result }
        result.connected = true
        guard active else { return ControllerBridge.Pad(connected: true) }
        let physical = PadState(buttons: input.buttons, lt: input.lt, rt: input.rt, lx: input.lx, ly: input.ly, rx: input.rx, ry: input.ry)
        let combined = physical.merged(with: touch)
        result.buttons=combined.buttons; result.lt=combined.lt; result.rt=combined.rt
        result.lx=combined.lx; result.ly=combined.ly; result.rx=combined.rx; result.ry=combined.ry
        return result
    }
}
extension ControllerBridge {
    func setTouchPad(connected: Bool, _ state: PadState) {
        queue.async {
            TrackBPadIntegration.shared.hasTouch = connected
            TrackBPadIntegration.shared.touch = state
            self.sample(); self.updateTimer()
        }
    }
    func setUIOwnsPads(_ owns: Bool) {
        queue.async { TrackBPadIntegration.shared.uiOwnsPads = owns; self.sample() }
    }
}
