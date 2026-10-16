// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// GameInput: touch, hardware keyboard, hardware mouse and on-screen keyboard to Windows input.
// Written from specs/01-input.md, part B. Calls only the C functions in interfaces/wine_host.h
// and the app names in interfaces/app-api.md (dlog, GameHostView).
//
// Everything runs on the main actor. GameController handlers are given the main queue as their
// handler queue, so they hop in with MainActor.assumeIsolated.

import Foundation
import GameController
import UIKit

/// Desktop, picture and reach rectangles in guest desktop pixels (spec 01, "Geometry").
struct InputGeometry: Equatable {
    var desktop: CGRect
    var picture: CGRect
    var reach: CGRect
}

/// One character from the on-screen keyboard as a key on a US layout.
struct InputKeyStroke: Equatable {
    var vk: Int
    var shift: Bool
}

/// One key press (down) or release posted for typed text.
struct InputKeyEvent: Equatable {
    var vk: Int
    var down: Bool
}

@MainActor
final class GameInput {
    static let shared = GameInput()

    enum TouchMode: String, CaseIterable { case off, trackpad, direct }

    /// Where the program last saw the pointer from us, in desktop pixels.
    private(set) var cursor = CGPoint(x: 512, y: 384)
    private(set) var touchMode: TouchMode = .off

    /// A hardware keyboard is attached.
    var hardwareKeyboard: Bool { GCKeyboard.coalesced != nil }

    // MARK: - State

    private enum MouseButton: CaseIterable {
        case left, right, middle

        var downFlag: UInt32 {
            switch self {
            case .left: return MouseFlag.leftDown
            case .right: return MouseFlag.rightDown
            case .middle: return MouseFlag.middleDown
            }
        }

        var upFlag: UInt32 {
            switch self {
            case .left: return MouseFlag.leftUp
            case .right: return MouseFlag.rightUp
            case .middle: return MouseFlag.middleUp
            }
        }
    }

    /// Something a touch gesture holds down that must be let go.
    private enum DragHold: Equatable {
        case leftButton                 // trackpad tap-then-hold
        case touch(ObjectIdentifier)    // direct-mode drag, by finger
    }

    /// What a direct-mode gesture turned into.
    private enum Resolution { case none, drag, wheel }

    private struct Finger {
        var start: CGPoint          // view points
        var last: CGPoint           // view points
        var startDesktop: CGPoint   // desktop pixels where it landed
    }

    private struct Gesture {
        var fingers: [ObjectIdentifier: Finger] = [:]       // fingers down now
        var lastPoints: [ObjectIdentifier: CGPoint] = [:]   // every finger of this gesture
        var landings: [ObjectIdentifier: CGPoint] = [:]     // where each finger first touched
        var startTime: TimeInterval = 0
        var maxFingers = 0
        var moved = false
        var drag: DragHold?
        var resolution: Resolution = .none
        var wheelReferenceY: CGFloat?
        var wheelTravel: CGFloat = 0        // since the last notch, view points
        var wheelTotal: CGFloat = 0         // all travel, for "moved"
        var wheelPositioned = false         // direct: cursor moved under the fingers already
        var motionCarry: CGPoint = .zero    // trackpad sub-pixel remainder
    }

    private var started = false
    private var observers: [NSObjectProtocol] = []
    private var programID: String?
    private var cursorPlaced = false

    private var attachedKeyboard: ObjectIdentifier?
    private var attachedMice: Set<ObjectIdentifier> = []
    /// Who can hold a key or mouse button down (spec 01, clarification 27).
    enum HoldSource: Hashable { case hardwareKeyboard, hardwareMouse, keyboardMode }

    // Per VK / button, the sources holding it. The press goes out when the first source
    // presses, the release when the last one lets go. Empty sets are removed.
    private var keyHolders: [Int: Set<HoldSource>] = [:]
    private var buttonHolders: [MouseButton: Set<HoldSource>] = [:]
    private var motionCarryX = 0.0
    private var motionCarryY = 0.0
    private var wheelCarry = 0.0

    private var gesture = Gesture()
    private var holdGeneration = 0
    private var lastTapClickTime: TimeInterval = -.infinity

    private init() {}

    // MARK: - Start

    /// Idempotent. Attaches the current keyboard and mice and watches for changes.
    func start() {
        guard !started else { return }
        started = true

        observe(.GCKeyboardDidConnect) { note in
            GameInput.shared.attachKeyboard((note.object as? GCKeyboard) ?? GCKeyboard.coalesced)
        }
        observe(.GCKeyboardDidDisconnect) { _ in
            let input = GameInput.shared
            input.attachedKeyboard = nil
            input.releaseAll("keyboard disconnected")
        }
        observe(.GCMouseDidConnect) { note in
            if let mouse = note.object as? GCMouse {
                GameInput.shared.attachMouse(mouse)
            }
        }
        observe(.GCMouseDidDisconnect) { note in
            let input = GameInput.shared
            if let mouse = note.object as? GCMouse {
                input.attachedMice.remove(ObjectIdentifier(mouse))
            }
            input.releaseAll("mouse disconnected")
        }
        observe(UIApplication.willResignActiveNotification) { _ in
            GameInput.shared.releaseAll("app inactive")
        }

        attachKeyboard(GCKeyboard.coalesced)
        for mouse in GCMouse.mice() {
            attachMouse(mouse)
        }
    }

    private func observe(_ name: Notification.Name, _ handler: @escaping @MainActor (Notification) -> Void) {
        let token = NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { note in
            MainActor.assumeIsolated { handler(note) }
        }
        observers.append(token)
    }

    // MARK: - Program and touch mode

    nonisolated static func defaultsKey(for programID: String) -> String {
        "touchMode.\(programID)"
    }

    /// Remembers the program and applies its saved touch mode (missing or unknown → off).
    func load(for programID: String) {
        self.programID = programID
        cursorPlaced = false
        let saved = UserDefaults.standard.string(forKey: Self.defaultsKey(for: programID))
        apply(saved.flatMap(TouchMode.init(rawValue:)) ?? .off)
    }

    func setTouchMode(_ mode: TouchMode) {
        if let programID {
            UserDefaults.standard.set(mode.rawValue, forKey: Self.defaultsKey(for: programID))
        }
        dlog("[input] touch for this game: \(mode.rawValue)")
        apply(mode)
    }

    private func apply(_ mode: TouchMode) {
        endGesture()
        lastTapClickTime = -.infinity
        touchMode = mode
        GameHostView.shared.showCursor(nil)
        surfaceShown()
    }

    /// The game view came on screen. Trackpad mode only: place the cursor the first time, then
    /// post it where it is and draw the arrow.
    func surfaceShown() {
        guard touchMode == .trackpad else { return }
        if !cursorPlaced {
            cursor = Self.reachCentre(geometry().reach)
            cursorPlaced = true
        }
        moveCursor(to: cursor, showArrow: true)
    }

    // MARK: - Geometry

    private func geometry() -> InputGeometry {
        var w: Int32 = 0
        var h: Int32 = 0
        mid_display_screen_size(&w, &h)

        let drawable = GameHostView.shared.metalLayer.drawableSize
        var px: Int32 = 0
        var py: Int32 = 0
        var pw: Int32 = 0
        var ph: Int32 = 0
        let found = mid_game_picture_rect(Self.int32(drawable.width), Self.int32(drawable.height),
                                          &px, &py, &pw, &ph) != 0
        let picture: CGRect? = (found && pw > 0 && ph > 0)
            ? CGRect(x: Int(px), y: Int(py), width: Int(pw), height: Int(ph))
            : nil
        return Self.makeGeometry(screenWidth: Int(w), screenHeight: Int(h), picture: picture)
    }

    /// Desktop is (0, 0, w, h), each at least 1. Picture is `picture` when it has positive size,
    /// else the desktop. Reach is picture ∩ desktop, or the desktop when that is empty.
    nonisolated static func makeGeometry(screenWidth: Int, screenHeight: Int, picture: CGRect?) -> InputGeometry {
        let desktop = CGRect(x: 0, y: 0, width: max(screenWidth, 1), height: max(screenHeight, 1))
        var shown = desktop
        // `size` rather than `width`/`height`: those report a negative size as positive.
        if let picture, picture.size.width > 0, picture.size.height > 0 {
            shown = picture
        }
        var reach = shown.intersection(desktop)
        if reach.isNull || reach.width <= 0 || reach.height <= 0 {
            reach = desktop
        }
        return InputGeometry(desktop: desktop, picture: shown, reach: reach)
    }

    /// x into minX...maxX−1, y into minY...maxY−1. Non-finite coordinates go to the minimum.
    nonisolated static func clamp(_ p: CGPoint, into r: CGRect) -> CGPoint {
        let x = p.x.isFinite ? p.x : r.minX
        let y = p.y.isFinite ? p.y : r.minY
        return CGPoint(x: min(max(x, r.minX), r.maxX - 1),
                       y: min(max(y, r.minY), r.maxY - 1))
    }

    /// A view point mapped proportionally into the picture, rounded down, clamped to reach.
    nonisolated static func mapToDesktop(_ p: CGPoint, viewBounds b: CGRect, geometry g: InputGeometry) -> CGPoint {
        let fx = b.width > 0 ? (p.x - b.minX) / b.width : 0
        let fy = b.height > 0 ? (p.y - b.minY) / b.height : 0
        let q = CGPoint(x: (g.picture.minX + fx * g.picture.width).rounded(.down),
                        y: (g.picture.minY + fy * g.picture.height).rounded(.down))
        return clamp(q, into: g.reach)
    }

    /// A desktop point as a fraction of the picture (what GameHostView.showCursor takes).
    nonisolated static func unitPoint(_ p: CGPoint, in picture: CGRect) -> CGPoint {
        CGPoint(x: picture.width > 0 ? (p.x - picture.minX) / picture.width : 0,
                y: picture.height > 0 ? (p.y - picture.minY) / picture.height : 0)
    }

    /// The whole-pixel centre of a rectangle, clamped into it.
    nonisolated static func reachCentre(_ r: CGRect) -> CGPoint {
        clamp(CGPoint(x: (r.minX + r.width / 2).rounded(.down), y: (r.minY + r.height / 2).rounded(.down)), into: r)
    }

    /// Rounds down and saturates into Int32; non-finite → 0.
    nonisolated static func int32(_ v: CGFloat) -> Int32 {
        guard v.isFinite else { return 0 }
        return Int32(min(max(v.rounded(.down), CGFloat(Int32.min)), CGFloat(Int32.max)))
    }

    func desktopPoint(_ p: CGPoint, in view: UIView) -> CGPoint {
        Self.mapToDesktop(p, viewBounds: view.bounds, geometry: geometry())
    }

    /// Clamps into reach, stores it, posts MOVE|ABSOLUTE there, and draws or hides the arrow.
    func moveCursor(to p: CGPoint, showArrow: Bool) {
        let g = geometry()
        let c = Self.clamp(p, into: g.reach)
        cursor = c
        cursorPlaced = true
        postPointer(at: c, MouseFlag.move | MouseFlag.absolute)
        GameHostView.shared.showCursor(showArrow ? Self.unitPoint(c, in: g.picture) : nil)
    }

    /// Right-stick cursor motion from ControllerBridge's keyboard mode.
    func stickMoved(dx: CGFloat, dy: CGFloat) {
        guard isFocused else { return }
        moveCursor(to: CGPoint(x: cursor.x + dx, y: cursor.y + dy), showArrow: true)
    }

    /// The game view is in a window, not hidden, and the app is active.
    private var isFocused: Bool {
        let view = GameHostView.shared
        return view.window != nil && !view.isHidden && UIApplication.shared.applicationState == .active
    }

    // MARK: - Posting helpers

    private func postPointer(at p: CGPoint, _ flags: UInt32, data: Int32 = 0) {
        mid_post_pointer(Self.int32(p.x), Self.int32(p.y), flags, data)
    }

    /// A finger as the left button (direct mode). Keeps `cursor` on the posted point.
    private func postTouch(_ phase: Int32, at p: CGPoint) {
        cursor = p
        cursorPlaced = true
        mid_post_touch(phase, Self.int32(p.x), Self.int32(p.y))
    }

    /// A DOWN then an UP, both ABSOLUTE at `p`.
    private func click(_ button: MouseButton, at p: CGPoint) {
        Self.traceInput()
        postPointer(at: p, button.downFlag | MouseFlag.absolute)
        postPointer(at: p, button.upFlag | MouseFlag.absolute)
    }

    /// Wheel notches with WHEEL only (no position): they go where the program's cursor is.
    private func postWheel(notches: Int) {
        guard notches != 0 else { return }
        let delta: Int32 = notches > 0 ? Int32(Tuning.wheelNotch) : -Int32(Tuning.wheelNotch)
        for _ in 0..<abs(notches) {
            mid_post_pointer(0, 0, MouseFlag.wheel, delta)
        }
    }

    private func openMenu() {
        GameHostView.onMenu?()
    }

    nonisolated static func traceInput() {
        mid_wow_trace_calls(3)
    }

    // MARK: - Release

    /// Lets go of every key and mouse button held by the hardware keyboard and mouse and by
    /// controller keyboard mode. Touch holds are left alone: they end through
    /// touchesEnded/touchesCancelled (UIKit cancels touches when the app goes inactive), and
    /// unplugging a keyboard must not end a drag the finger is doing.
    func releaseAll(_ why: String) {
        for vk in keyHolders.keys.sorted() {
            mid_post_key(Int32(vk), 0)
        }
        keyHolders.removeAll()
        for button in MouseButton.allCases {
            guard let holders = buttonHolders[button] else { continue }
            postButton(button, down: false, as: holders.contains(.hardwareMouse) ? .hardwareMouse : .keyboardMode)
        }
        buttonHolders.removeAll()
        // Keyboard mode must press again what is still held, not think it still holds it.
        ControllerBridge.shared.forgetKeyboardModeHolds()
        motionCarryX = 0
        motionCarryY = 0
        wheelCarry = 0
        dlog("[input] released all keys and buttons (\(why))")
    }

    // MARK: - Shared holds (clarifications 22, 27)

    func isKeyHeld(_ vk: Int) -> Bool {
        keyHolders[vk] != nil
    }

    private func isKeyHeld(_ vk: Int, by source: HoldSource) -> Bool {
        keyHolders[vk]?.contains(source) ?? false
    }

    /// `source` presses `vk`. Nothing if it already holds it; the press is posted only when no
    /// other source holds it.
    private func pressKey(_ vk: Int, by source: HoldSource) {
        var holders = keyHolders[vk] ?? []
        guard holders.insert(source).inserted else { return }
        keyHolders[vk] = holders
        if holders.count == 1 {
            mid_post_key(Int32(vk), 1)
        }
    }

    /// `source` lets go of `vk`. Nothing if it doesn't hold it; the release is posted only
    /// when it was the last holder.
    private func releaseKey(_ vk: Int, by source: HoldSource) {
        guard var holders = keyHolders[vk], holders.remove(source) != nil else { return }
        if holders.isEmpty {
            keyHolders[vk] = nil
            mid_post_key(Int32(vk), 0)
        } else {
            keyHolders[vk] = holders
        }
    }

    private func pressButton(_ button: MouseButton, by source: HoldSource) {
        var holders = buttonHolders[button] ?? []
        guard holders.insert(source).inserted else { return }
        buttonHolders[button] = holders
        if holders.count == 1 {
            postButton(button, down: true, as: source)
        }
    }

    private func releaseButton(_ button: MouseButton, by source: HoldSource) {
        guard var holders = buttonHolders[button], holders.remove(source) != nil else { return }
        if holders.isEmpty {
            buttonHolders[button] = nil
            postButton(button, down: false, as: source)
        } else {
            buttonHolders[button] = holders
        }
    }

    /// Hardware mouse buttons carry x = y = 0 and no ABSOLUTE (clarification 8); keyboard mode
    /// uses `mid_post_mouse_button`, as part A says.
    private func postButton(_ button: MouseButton, down: Bool, as source: HoldSource) {
        let flag = down ? button.downFlag : button.upFlag
        if source == .keyboardMode {
            mid_post_mouse_button(flag)
        } else {
            mid_post_pointer(0, 0, flag, 0)
        }
    }

    /// Keyboard-mode key output from ControllerBridge (main queue, in sample order).
    func keyboardModeKey(_ vk: Int, down: Bool) {
        if down { pressKey(vk, by: .keyboardMode) } else { releaseKey(vk, by: .keyboardMode) }
    }

    /// Keyboard-mode trigger buttons from ControllerBridge: `right` false is the left button.
    func keyboardModeButton(right: Bool, down: Bool) {
        let button: MouseButton = right ? .right : .left
        if down { pressButton(button, by: .keyboardMode) } else { releaseButton(button, by: .keyboardMode) }
    }

    // MARK: - Touch entry points (called by GameHostView)

    func touchesBegan(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let list = usable(touches)
        guard !list.isEmpty else { return }
        switch touchMode {
        case .off: offBegan(list, in: view)
        case .trackpad: trackpadBegan(list, in: view)
        case .direct: directBegan(list, in: view)
        }
    }

    func touchesMoved(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let list = usable(touches)
        guard !list.isEmpty else { return }
        switch touchMode {
        case .off: _ = updateFingers(list, in: view)
        case .trackpad: trackpadMoved(list, in: view)
        case .direct: directMoved(list, in: view)
        }
    }

    func touchesEnded(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let list = usable(touches)
        guard !list.isEmpty else { return }
        switch touchMode {
        case .off: offEnded(list, in: view)
        case .trackpad: trackpadEnded(list, in: view)
        case .direct: directEnded(list, in: view)
        }
    }

    func touchesCancelled(_ touches: Set<UITouch>, in view: UIView) {
        let list = usable(touches)
        guard list.contains(where: { gesture.fingers[ObjectIdentifier($0)] != nil }) else { return }
        if touchMode == .direct, case let .touch(id)? = gesture.drag {
            // The drag ends where its finger was cancelled.
            let point: CGPoint
            if let t = list.first(where: { ObjectIdentifier($0) == id }) {
                point = desktopPoint(t.location(in: view), in: view)
            } else if let last = gesture.lastPoints[id] {
                point = desktopPoint(last, in: view)
            } else {
                point = cursor
            }
            gesture.drag = nil
            postTouch(2, at: point)
        }
        // Trackpad: never leave the left button held.
        endGesture()
    }

    /// With a GCMouse attached, its clicks already arrive through GameController, so the
    /// pointer touches UIKit makes for the same clicks are dropped.
    private func usable(_ touches: Set<UITouch>) -> [UITouch] {
        let mouseAttached = !GCMouse.mice().isEmpty
        return touches.filter { !(mouseAttached && $0.type == .indirectPointer) }
    }

    // MARK: - Gesture bookkeeping

    private func resetGesture() {
        gesture = Gesture()
        holdGeneration &+= 1   // a pending hold timer now does nothing
    }

    /// Lets go of whatever the gesture holds, then resets it.
    private func endGesture() {
        switch gesture.drag {
        case .leftButton?:
            postPointer(at: cursor, MouseFlag.leftUp | MouseFlag.absolute)
        case .touch?:
            postTouch(2, at: cursor)
        case nil:
            break
        }
        resetGesture()
    }

    /// Starts a new gesture when no finger is down. Returns true when it did.
    private func beginIfIdle(_ touches: [UITouch]) -> Bool {
        guard gesture.fingers.isEmpty else { return false }
        resetGesture()
        gesture.startTime = touches.map(\.timestamp).min() ?? 0
        return true
    }

    private func addFingers(_ touches: [UITouch], in view: UIView) {
        for t in touches {
            let p = t.location(in: view)
            let id = ObjectIdentifier(t)
            gesture.fingers[id] = Finger(start: p, last: p, startDesktop: desktopPoint(p, in: view))
            gesture.lastPoints[id] = p
            gesture.landings[id] = p
        }
        gesture.maxFingers = max(gesture.maxFingers, gesture.fingers.count)
        gesture.wheelReferenceY = nil   // the average jumps when a finger joins
    }

    /// Updates the fingers we track among `touches`. Returns each one's previous and new point.
    private func updateFingers(_ touches: [UITouch], in view: UIView) -> [(id: ObjectIdentifier, from: CGPoint, to: CGPoint)] {
        var moves: [(id: ObjectIdentifier, from: CGPoint, to: CGPoint)] = []
        for t in touches {
            let id = ObjectIdentifier(t)
            guard var finger = gesture.fingers[id] else { continue }
            let p = t.location(in: view)
            moves.append((id, finger.last, p))
            finger.last = p
            gesture.fingers[id] = finger
            gesture.lastPoints[id] = p
            if Self.distance(p, finger.start) > Tuning.moveSlop {
                gesture.moved = true
            }
        }
        return moves
    }

    /// Removes the fingers we track among `touches`. Returns each one's lift point and time.
    private func removeFingers(_ touches: [UITouch], in view: UIView) -> [(id: ObjectIdentifier, point: CGPoint, time: TimeInterval)] {
        var lifts: [(id: ObjectIdentifier, point: CGPoint, time: TimeInterval)] = []
        for t in touches {
            let id = ObjectIdentifier(t)
            guard gesture.fingers.removeValue(forKey: id) != nil else { continue }
            let p = t.location(in: view)
            gesture.lastPoints[id] = p
            lifts.append((id, p, t.timestamp))
        }
        if !lifts.isEmpty {
            gesture.wheelReferenceY = nil   // the average jumps when a finger leaves
        }
        return lifts
    }

    /// Feeds the wheel the live fingers' average y. Returns whole notches (+ is fingers down).
    private func wheelStep(averageY y: CGFloat) -> Int {
        defer { gesture.wheelReferenceY = y }
        guard let reference = gesture.wheelReferenceY else { return 0 }
        let d = y - reference
        gesture.wheelTotal += abs(d)
        if gesture.wheelTotal > Tuning.wheelSlop {
            gesture.moved = true
        }
        let step = Self.wheelNotches(travel: gesture.wheelTravel + d)
        gesture.wheelTravel = step.rest
        return step.notches
    }

    /// Every 14 points of travel is one notch, sign kept; the rest carries over.
    nonisolated static func wheelNotches(travel: CGFloat) -> (notches: Int, rest: CGFloat) {
        guard travel.isFinite else { return (0, 0) }
        let n = (travel / Tuning.wheelPointsPerNotch).rounded(.towardZero)
        return (Int(n), travel - n * Tuning.wheelPointsPerNotch)
    }

    nonisolated static func distance(_ a: CGPoint, _ b: CGPoint) -> CGFloat {
        hypot(a.x - b.x, a.y - b.y)
    }

    private static func average(_ points: [CGPoint]) -> CGPoint {
        guard !points.isEmpty else { return .zero }
        let n = CGFloat(points.count)
        return CGPoint(x: points.reduce(0) { $0 + $1.x } / n, y: points.reduce(0) { $0 + $1.y } / n)
    }

    // MARK: - Touch: off

    private func offBegan(_ touches: [UITouch], in view: UIView) {
        _ = beginIfIdle(touches)
        addFingers(touches, in: view)
    }

    private func offEnded(_ touches: [UITouch], in view: UIView) {
        let lifts = removeFingers(touches, in: view)
        guard let time = lifts.map({ $0.time }).max(), gesture.fingers.isEmpty else { return }
        let isMenuTap = !gesture.moved
            && gesture.maxFingers >= 3
            && time - gesture.startTime < Tuning.menuMaxDuration
        resetGesture()
        if isMenuTap { openMenu() }
    }

    // MARK: - Touch: trackpad

    private func trackpadBegan(_ touches: [UITouch], in view: UIView) {
        if gesture.fingers.isEmpty && !cursorPlaced {
            surfaceShown()
        }
        if beginIfIdle(touches) {
            if gesture.startTime - lastTapClickTime < Tuning.tapHoldWindow {
                // Tap then hold: a drag with the left button.
                lastTapClickTime = -.infinity
                gesture.drag = .leftButton
                Self.traceInput()
                postPointer(at: cursor, MouseFlag.leftDown | MouseFlag.absolute)
            }
        }
        addFingers(touches, in: view)
    }

    private func trackpadMoved(_ touches: [UITouch], in view: UIView) {
        let moves = updateFingers(touches, in: view)
        guard !moves.isEmpty else { return }

        if gesture.fingers.count == 1, let m = moves.first {
            let scale = geometry().picture.width / max(view.bounds.width, 1) * Tuning.trackpadSpeed
            let total = CGPoint(x: gesture.motionCarry.x + (m.to.x - m.from.x) * scale,
                                y: gesture.motionCarry.y + (m.to.y - m.from.y) * scale)
            let whole = CGPoint(x: total.x.rounded(.towardZero), y: total.y.rounded(.towardZero))
            gesture.motionCarry = CGPoint(x: total.x - whole.x, y: total.y - whole.y)
            if whole != .zero {
                moveCursor(to: CGPoint(x: cursor.x + whole.x, y: cursor.y + whole.y), showArrow: true)
            }
        } else if gesture.fingers.count >= 2, gesture.drag == nil {
            let centre = Self.average(gesture.fingers.values.map(\.last))
            postWheel(notches: wheelStep(averageY: centre.y))
        }
    }

    private func trackpadEnded(_ touches: [UITouch], in view: UIView) {
        let lifts = removeFingers(touches, in: view)
        guard let time = lifts.map({ $0.time }).max(), gesture.fingers.isEmpty else { return }

        let duration = time - gesture.startTime
        let wasDragging = gesture.drag != nil
        let moved = gesture.moved
        let fingers = gesture.maxFingers
        gesture.drag = nil
        resetGesture()

        if wasDragging {
            postPointer(at: cursor, MouseFlag.leftUp | MouseFlag.absolute)
        } else if !moved && fingers >= 3 && duration < Tuning.menuMaxDuration {
            openMenu()
        } else if !moved && duration < Tuning.tapMaxDuration {
            if fingers == 1 {
                click(.left, at: cursor)
                lastTapClickTime = time
            } else if fingers == 2 {
                click(.right, at: cursor)
            }
        }
    }

    // MARK: - Touch: direct

    private func directBegan(_ touches: [UITouch], in view: UIView) {
        GameHostView.shared.showCursor(nil)
        if beginIfIdle(touches) {
            scheduleHoldCheck()
        } else if gesture.resolution == .drag {
            return   // fingers that join a drag are ignored
        }
        addFingers(touches, in: view)
    }

    private func scheduleHoldCheck() {
        let generation = holdGeneration
        DispatchQueue.main.asyncAfter(deadline: .now() + Tuning.holdDelay) {
            MainActor.assumeIsolated {
                GameInput.shared.holdCheck(generation)
            }
        }
    }

    /// 0.25 s after the first finger: still the only finger and nothing resolved → drag.
    private func holdCheck(_ generation: Int) {
        guard generation == holdGeneration,
              touchMode == .direct,
              gesture.resolution == .none,
              !gesture.moved,
              gesture.maxFingers == 1,
              gesture.fingers.count == 1,
              let only = gesture.fingers.first
        else { return }
        startDrag(only.key, only.value)
    }

    private func startDrag(_ id: ObjectIdentifier, _ finger: Finger) {
        gesture.resolution = .drag
        gesture.drag = .touch(id)
        Self.traceInput()
        postTouch(0, at: finger.startDesktop)
    }

    private func directMoved(_ touches: [UITouch], in view: UIView) {
        let moves = updateFingers(touches, in: view)
        guard !moves.isEmpty else { return }

        // A lone finger that moves far enough becomes a drag at once.
        if gesture.resolution == .none, gesture.maxFingers == 1, gesture.fingers.count == 1,
           let m = moves.first, let finger = gesture.fingers[m.id],
           Self.distance(m.to, finger.start) > Tuning.moveSlop {
            startDrag(m.id, finger)
        }

        if case let .touch(id)? = gesture.drag {
            if let m = moves.first(where: { $0.id == id }) {
                postTouch(1, at: desktopPoint(m.to, in: view))
            }
            return
        }

        if gesture.fingers.count == 2, gesture.resolution != .drag {
            let centre = Self.average(gesture.fingers.values.map(\.last))
            let notches = wheelStep(averageY: centre.y)
            if gesture.wheelTotal > Tuning.wheelSlop {
                gesture.resolution = .wheel
            }
            if notches != 0 {
                if !gesture.wheelPositioned {
                    // Once per scroll: put the program's cursor under the fingers, so the
                    // WHEEL-only notches reach what is there.
                    gesture.wheelPositioned = true
                    let p = desktopPoint(centre, in: view)
                    cursor = p
                    cursorPlaced = true
                    postPointer(at: p, MouseFlag.move | MouseFlag.absolute)
                }
                postWheel(notches: notches)
            }
        }
    }

    private func directEnded(_ touches: [UITouch], in view: UIView) {
        let lifts = removeFingers(touches, in: view)
        guard !lifts.isEmpty else { return }

        for lift in lifts where gesture.drag == .touch(lift.id) {
            gesture.drag = nil
            postTouch(2, at: desktopPoint(lift.point, in: view))
        }
        guard gesture.fingers.isEmpty else { return }

        let tapped = gesture.resolution == .none && !gesture.moved
        let fingers = gesture.maxFingers
        // A tap goes where the fingers landed (where the user aimed), not where they rolled to.
        let centre = desktopPoint(Self.average(Array(gesture.landings.values)), in: view)
        resetGesture()
        guard tapped else { return }

        switch fingers {
        case 1:
            Self.traceInput()
            postTouch(0, at: centre)
            postTouch(2, at: centre)
        case 2:
            Self.traceInput()
            cursor = centre
            cursorPlaced = true
            postPointer(at: centre, MouseFlag.move | MouseFlag.absolute)
            postPointer(at: centre, MouseFlag.rightDown | MouseFlag.absolute)
            postPointer(at: centre, MouseFlag.rightUp | MouseFlag.absolute)
        default:
            if fingers >= 3 { openMenu() }
        }
    }

    // MARK: - Hardware keyboard

    private func attachKeyboard(_ keyboard: GCKeyboard?) {
        guard let keyboard, let input = keyboard.keyboardInput else { return }
        let id = ObjectIdentifier(keyboard)
        guard attachedKeyboard != id else { return }
        attachedKeyboard = id
        keyboard.handlerQueue = .main
        input.keyChangedHandler = { _, _, keyCode, pressed in
            MainActor.assumeIsolated {
                GameInput.shared.keyChanged(usage: keyCode.rawValue, pressed: pressed)
            }
        }
        dlog("[input] keyboard connected: \(keyboard.vendorName ?? "unknown")")
    }

    private func keyChanged(usage: Int, pressed: Bool) {
        guard let vk = Self.virtualKey(forHIDUsage: usage) else { return }
        if pressed {
            guard isFocused, !isKeyHeld(vk, by: .hardwareKeyboard) else { return }
            Self.traceInput()
            pressKey(vk, by: .hardwareKeyboard)
        } else {
            releaseKey(vk, by: .hardwareKeyboard)
        }
    }

    /// Keyboard/Keypad page (0x07) usage → Windows virtual key, US layout. nil: no Windows key.
    nonisolated static func virtualKey(forHIDUsage usage: Int) -> Int? {
        hidUsageToVirtualKey[usage]
    }

    // MARK: - Hardware mouse

    private func attachMouse(_ mouse: GCMouse) {
        let id = ObjectIdentifier(mouse)
        guard !attachedMice.contains(id), let input = mouse.mouseInput else { return }
        attachedMice.insert(id)
        mouse.handlerQueue = .main
        input.mouseMovedHandler = { _, dx, dy in
            MainActor.assumeIsolated { GameInput.shared.mouseMoved(dx: dx, dy: dy) }
        }
        input.leftButton.pressedChangedHandler = { _, _, pressed in
            MainActor.assumeIsolated { GameInput.shared.mouseButton(.left, pressed: pressed) }
        }
        input.rightButton?.pressedChangedHandler = { _, _, pressed in
            MainActor.assumeIsolated { GameInput.shared.mouseButton(.right, pressed: pressed) }
        }
        input.middleButton?.pressedChangedHandler = { _, _, pressed in
            MainActor.assumeIsolated { GameInput.shared.mouseButton(.middle, pressed: pressed) }
        }
        input.scroll.valueChangedHandler = { _, _, y in
            MainActor.assumeIsolated { GameInput.shared.mouseScrolled(y: y) }
        }
        dlog("[input] mouse connected: \(mouse.vendorName ?? "unknown")")
    }

    /// Adds `delta` to a remainder and splits off the whole part, saturated at ±limit.
    /// Returns the whole part to send and the fraction to keep. Non-finite input clears it.
    nonisolated static func takeWhole(_ carry: Double, adding delta: Double, limit: Double) -> (whole: Int, rest: Double) {
        let total = carry + delta
        guard total.isFinite else { return (0, 0) }
        let whole = total.rounded(.towardZero)
        return (Int(min(max(whole, -limit), limit)), total - whole)
    }

    private func mouseMoved(dx: Float, dy: Float) {
        guard isFocused else { return }
        // GameController's y points up, Windows' points down.
        let x = Self.takeWhole(motionCarryX, adding: Double(dx), limit: Tuning.motionLimit)
        let y = Self.takeWhole(motionCarryY, adding: -Double(dy), limit: Tuning.motionLimit)
        motionCarryX = x.rest
        motionCarryY = y.rest
        guard x.whole != 0 || y.whole != 0 else { return }

        mid_post_pointer(Int32(x.whole), Int32(y.whole), MouseFlag.move, 0)
        let g = geometry()
        cursor = Self.clamp(CGPoint(x: cursor.x + CGFloat(x.whole), y: cursor.y + CGFloat(y.whole)), into: g.desktop)
        cursorPlaced = true
        GameHostView.shared.showCursor(Self.unitPoint(cursor, in: g.picture))
    }

    private func mouseButton(_ button: MouseButton, pressed: Bool) {
        if pressed {
            guard isFocused, !(buttonHolders[button]?.contains(.hardwareMouse) ?? false) else { return }
            Self.traceInput()
            pressButton(button, by: .hardwareMouse)
        } else {
            releaseButton(button, by: .hardwareMouse)
        }
    }

    private func mouseScrolled(y: Float) {
        guard isFocused else { return }
        let step = Self.takeWhole(wheelCarry, adding: Double(y), limit: Tuning.maxNotchesPerScroll)
        wheelCarry = step.rest
        guard step.whole != 0 else { return }
        let delta: Int32 = step.whole > 0 ? Int32(Tuning.wheelNotch) : -Int32(Tuning.wheelNotch)
        for _ in 0..<abs(step.whole) {
            mid_post_pointer(0, 0, MouseFlag.wheel, delta)
        }
    }

    // MARK: - On-screen keyboard

    /// Text from the on-screen keyboard, typed as US-layout key presses.
    func type(_ text: String) {
        Self.traceInput()
        // A hardware keyboard's keys already arrive raw; UIKit also sends them here as text.
        guard !hardwareKeyboard, isFocused else { return }
        for character in text {
            for stroke in Self.keyStrokes(for: character) {
                for event in Self.typedEvents(for: stroke, isHeld: { self.isKeyHeld($0) }) {
                    mid_post_key(Int32(event.vk), event.down ? 1 : 0)
                }
            }
        }
    }

    func backspace() {
        Self.traceInput()
        guard !hardwareKeyboard, isFocused else { return }
        let stroke = InputKeyStroke(vk: VK.back, shift: false)
        for event in Self.typedEvents(for: stroke, isHeld: { self.isKeyHeld($0) }) {
            mid_post_key(Int32(event.vk), event.down ? 1 : 0)
        }
    }

    /// The key events for one typed stroke, given which keys other sources hold
    /// (clarification 28): a key already held is neither pressed nor released for it, and a
    /// Shift already held (either side) is used as it is, not pressed or released.
    /// On-screen typing holds nothing itself, so it isn't one of the tracked sources.
    nonisolated static func typedEvents(for stroke: InputKeyStroke, isHeld: (Int) -> Bool) -> [InputKeyEvent] {
        guard !isHeld(stroke.vk) else { return [] }
        let shiftHeld = isHeld(VK.shift) || isHeld(VK.leftShift) || isHeld(VK.rightShift)
        let pressShift = stroke.shift && !shiftHeld
        var events: [InputKeyEvent] = []
        if pressShift { events.append(InputKeyEvent(vk: VK.shift, down: true)) }
        events.append(InputKeyEvent(vk: stroke.vk, down: true))
        events.append(InputKeyEvent(vk: stroke.vk, down: false))
        if pressShift { events.append(InputKeyEvent(vk: VK.shift, down: false)) }
        return events
    }

    /// The US-layout key for a character and whether it needs Shift. nil: skip it.
    nonisolated static func keyStroke(for character: Character) -> InputKeyStroke? {
        usCharacterKeys[character]
    }

    /// What `type(_:)` presses for a character: iOS smart punctuation becomes its ASCII form
    /// first (… becomes three periods), then each character is looked up on the US layout.
    /// Empty: skip it.
    nonisolated static func keyStrokes(for character: Character) -> [InputKeyStroke] {
        if let ascii = smartPunctuationASCII[character] {
            return ascii.compactMap { keyStroke(for: $0) }
        }
        return keyStroke(for: character).map { [$0] } ?? []
    }
}

// MARK: - Tuning (values from spec 01)

private enum Tuning {
    static let moveSlop: CGFloat = 10           // points before a finger counts as moved
    static let wheelSlop: CGFloat = 2           // points of wheel travel before "moved"
    static let wheelPointsPerNotch: CGFloat = 14
    static let wheelNotch = 120
    static let trackpadSpeed: CGFloat = 1.3
    static let tapMaxDuration: TimeInterval = 0.3
    static let menuMaxDuration: TimeInterval = 0.6
    static let tapHoldWindow: TimeInterval = 0.35
    static let holdDelay: TimeInterval = 0.25
    static let motionLimit = 30000.0
    static let maxNotchesPerScroll = 100.0     // our own bound, see QUESTIONS.md
}

// MARK: - Windows constants (Microsoft "Virtual-Key Codes", MOUSEINPUT)

private enum VK {
    static let back = 0x08
    static let tab = 0x09
    static let returnKey = 0x0D
    static let shift = 0x10
    static let space = 0x20
    static let leftShift = 0xA0
    static let rightShift = 0xA1
}

private enum MouseFlag {
    static let move: UInt32 = 0x0001
    static let leftDown: UInt32 = 0x0002
    static let leftUp: UInt32 = 0x0004
    static let rightDown: UInt32 = 0x0008
    static let rightUp: UInt32 = 0x0010
    static let middleDown: UInt32 = 0x0020
    static let middleUp: UInt32 = 0x0040
    static let wheel: UInt32 = 0x0800
    static let absolute: UInt32 = 0x8000
}

// MARK: - Key tables

/// USB HID Usage Tables, Keyboard/Keypad page (0x07) → Windows virtual-key code, US mapping.
/// Written from the HID usage list and Microsoft's virtual-key list; see PROVENANCE.md.
private let hidUsageToVirtualKey: [Int: Int] = {
    var t: [Int: Int] = [:]
    for i in 0..<26 { t[0x04 + i] = 0x41 + i }     // a…z → 'A'…'Z'
    for i in 0..<9 { t[0x1E + i] = 0x31 + i }      // 1…9 → '1'…'9'
    t[0x27] = 0x30                                  // 0 → '0'
    t[0x28] = 0x0D                                  // Return → VK_RETURN
    t[0x29] = 0x1B                                  // Escape → VK_ESCAPE
    t[0x2A] = 0x08                                  // Backspace → VK_BACK
    t[0x2B] = 0x09                                  // Tab → VK_TAB
    t[0x2C] = 0x20                                  // Space → VK_SPACE
    t[0x2D] = 0xBD                                  // - _ → VK_OEM_MINUS
    t[0x2E] = 0xBB                                  // = + → VK_OEM_PLUS
    t[0x2F] = 0xDB                                  // [ { → VK_OEM_4
    t[0x30] = 0xDD                                  // ] } → VK_OEM_6
    t[0x31] = 0xDC                                  // \ | → VK_OEM_5
    t[0x32] = 0xDC                                  // ISO # ~ → like backslash (spec)
    t[0x33] = 0xBA                                  // ; : → VK_OEM_1
    t[0x34] = 0xDE                                  // ' " → VK_OEM_7
    t[0x35] = 0xC0                                  // ` ~ → VK_OEM_3
    t[0x36] = 0xBC                                  // , < → VK_OEM_COMMA
    t[0x37] = 0xBE                                  // . > → VK_OEM_PERIOD
    t[0x38] = 0xBF                                  // / ? → VK_OEM_2
    t[0x39] = 0x14                                  // Caps Lock → VK_CAPITAL
    for i in 0..<12 { t[0x3A + i] = 0x70 + i }     // F1…F12 → VK_F1…VK_F12
    t[0x46] = 0x2C                                  // Print Screen → VK_SNAPSHOT
    t[0x47] = 0x91                                  // Scroll Lock → VK_SCROLL
    t[0x48] = 0x13                                  // Pause → VK_PAUSE
    t[0x49] = 0x2D                                  // Insert → VK_INSERT
    t[0x4A] = 0x24                                  // Home → VK_HOME
    t[0x4B] = 0x21                                  // Page Up → VK_PRIOR
    t[0x4C] = 0x2E                                  // Delete Forward → VK_DELETE
    t[0x4D] = 0x23                                  // End → VK_END
    t[0x4E] = 0x22                                  // Page Down → VK_NEXT
    t[0x4F] = 0x27                                  // Right Arrow → VK_RIGHT
    t[0x50] = 0x25                                  // Left Arrow → VK_LEFT
    t[0x51] = 0x28                                  // Down Arrow → VK_DOWN
    t[0x52] = 0x26                                  // Up Arrow → VK_UP
    t[0x53] = 0x90                                  // Num Lock → VK_NUMLOCK
    t[0x54] = 0x6F                                  // Keypad / → VK_DIVIDE
    t[0x55] = 0x6A                                  // Keypad * → VK_MULTIPLY
    t[0x56] = 0x6D                                  // Keypad - → VK_SUBTRACT
    t[0x57] = 0x6B                                  // Keypad + → VK_ADD
    t[0x58] = 0x0D                                  // Keypad Enter → VK_RETURN (spec)
    for i in 0..<9 { t[0x59 + i] = 0x61 + i }      // Keypad 1…9 → VK_NUMPAD1…9
    t[0x62] = 0x60                                  // Keypad 0 → VK_NUMPAD0
    t[0x63] = 0x6E                                  // Keypad . → VK_DECIMAL
    t[0x64] = 0xE2                                  // ISO \ | (the <> key) → VK_OEM_102
    t[0x65] = 0x5D                                  // Application (Menu) → VK_APPS
    for i in 0..<12 { t[0x68 + i] = 0x7C + i }     // F13…F24 → VK_F13…VK_F24
    t[0xE0] = 0xA2                                  // Left Control → VK_LCONTROL
    t[0xE1] = 0xA0                                  // Left Shift → VK_LSHIFT
    t[0xE2] = 0xA4                                  // Left Alt (Option) → VK_LMENU
    t[0xE3] = 0x5B                                  // Left GUI (Command) → VK_LWIN
    t[0xE4] = 0xA3                                  // Right Control → VK_RCONTROL
    t[0xE5] = 0xA1                                  // Right Shift → VK_RSHIFT
    t[0xE6] = 0xA5                                  // Right Alt (Option) → VK_RMENU
    t[0xE7] = 0x5C                                  // Right GUI (Command) → VK_RWIN
    return t
}()

/// iOS smart punctuation and its ASCII form (spec 01, answer 20).
private let smartPunctuationASCII: [Character: String] = [
    "\u{2018}": "'",    // ‘ left single quotation mark
    "\u{2019}": "'",    // ’ right single quotation mark
    "\u{201A}": "'",    // ‚ single low-9 quotation mark
    "\u{2032}": "'",    // ′ prime
    "\u{201C}": "\"",   // “ left double quotation mark
    "\u{201D}": "\"",   // ” right double quotation mark
    "\u{201E}": "\"",   // „ double low-9 quotation mark
    "\u{2033}": "\"",   // ″ double prime
    "\u{2013}": "-",    // – en dash
    "\u{2014}": "-",    // — em dash
    "\u{2212}": "-",    // − minus sign
    "\u{2026}": "...",  // … horizontal ellipsis
    "\u{00A0}": " ",    // no-break space
]

/// Characters the on-screen keyboard can type, as US-layout keys.
private let usCharacterKeys: [Character: InputKeyStroke] = {
    var t: [Character: InputKeyStroke] = [:]
    func pair(_ plain: Character, _ shifted: Character, _ vk: Int) {
        t[plain] = InputKeyStroke(vk: vk, shift: false)
        t[shifted] = InputKeyStroke(vk: vk, shift: true)
    }
    for (i, scalar) in (UInt8(ascii: "a")...UInt8(ascii: "z")).enumerated() {
        pair(Character(UnicodeScalar(scalar)), Character(UnicodeScalar(scalar - 0x20)), 0x41 + i)
    }
    // Digit row: unshifted digit, shifted symbol.
    pair("1", "!", 0x31)
    pair("2", "@", 0x32)
    pair("3", "#", 0x33)
    pair("4", "$", 0x34)
    pair("5", "%", 0x35)
    pair("6", "^", 0x36)
    pair("7", "&", 0x37)
    pair("8", "*", 0x38)
    pair("9", "(", 0x39)
    pair("0", ")", 0x30)
    pair("`", "~", 0xC0)
    pair("-", "_", 0xBD)
    pair("=", "+", 0xBB)
    pair("[", "{", 0xDB)
    pair("]", "}", 0xDD)
    pair("\\", "|", 0xDC)
    pair(";", ":", 0xBA)
    pair("'", "\"", 0xDE)
    pair(",", "<", 0xBC)
    pair(".", ">", 0xBE)
    pair("/", "?", 0xBF)
    t[" "] = InputKeyStroke(vk: 0x20, shift: false)
    t["\t"] = InputKeyStroke(vk: 0x09, shift: false)
    t["\n"] = InputKeyStroke(vk: 0x0D, shift: false)
    t["\r"] = InputKeyStroke(vk: 0x0D, shift: false)
    t["\r\n"] = InputKeyStroke(vk: 0x0D, shift: false)
    return t
}()
