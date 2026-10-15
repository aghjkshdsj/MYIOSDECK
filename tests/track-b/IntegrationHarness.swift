// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import UIKit
import QuartzCore

@MainActor enum TrackBSessionGate { static var reserved = false }
final class TrackBPadIntegration {
    static let shared = TrackBPadIntegration()
    var hasTouch = false
    func adapt(_ pad: ControllerBridge.Pad, slot: Int, active: Bool) -> ControllerBridge.Pad { pad }
}
@MainActor final class GameHostView: UIView {
    static let shared = GameHostView()
    static var onMenu: (() -> Void)?
    let metalLayer = CAMetalLayer()
    func showCursor(_ point: CGPoint?) {}
}
