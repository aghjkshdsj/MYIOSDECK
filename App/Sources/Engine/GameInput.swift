// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController
import UIKit

/// Keyboard and mouse for Windows programs (launchers, menus, games without controller
/// support), on the game view (GameHostView). Ported in part from Madeira's
/// HardwareInput.swift and ContentView.swift (GPL-3.0-or-later):
/// - Touch, as a trackpad (default: a drawn cursor that one finger moves, a tap clicks where it
///   is, a tap then hold drags) or direct (a tap is a left click where the finger is; holding
///   or moving drags); in both a two-finger tap is a right click, a two-finger drag the wheel.
/// - Hardware keyboard (GCKeyboard: raw HID usages, modifiers as keys, no repeat) and mouse
///   (GCMouse: deltas posted relative, buttons, wheel), only while the game view is shown and
///   the app is active; losing that releases everything that was posted down.
/// - The on-screen keyboard (the game view's keyboard button): typed text as key presses.
/// - The right stick moves the cursor in the "Keyboard and mouse" controller mode
///   (ControllerBridge), so its trigger clicks land where the drawn cursor is.
/// Coordinates posted with ABSOLUTE are guest desktop pixels (mid_display_screen_size).
@MainActor
final class GameInput {
    static let shared = GameInput()

    private static let move: UInt32 = 0x0001, absolute: UInt32 = 0x8000
    private static let rightDown: UInt32 = 0x0008, rightUp: UInt32 = 0x0010
    private static let wheel: UInt32 = 0x0800

    /// The pointer as the program last saw it from us, in desktop pixels. Main thread.
    private(set) var cursor = CGPoint(x: 512, y: 384)
    private var started = false

    func start() {
        guard !started else { return }
        started = true
        // Observers and GameController handlers run on the main queue (queue: .main,
        // handlerQueue = .main): MainActor.assumeIsolated, as ControllerBridge does.
        let center = NotificationCenter.default
        center.addObserver(forName: .GCKeyboardDidConnect, object: nil, queue: .main) { [weak self] n in
            let kb = n.object as? GCKeyboard
            MainActor.assumeIsolated { self?.attachKeyboard(kb) }
        }
        center.addObserver(forName: .GCKeyboardDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseAll("keyboard disconnected") }
        }
        center.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] n in
            let mouse = n.object as? GCMouse
            MainActor.assumeIsolated { self?.attachMouse(mouse) }
        }
        center.addObserver(forName: .GCMouseDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseAll("mouse disconnected") }
        }
        center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseAll("app inactive") }
        }
        attachKeyboard(GCKeyboard.coalesced)
        for m in GCMouse.mice() { attachMouse(m) }
    }

    // MARK: - focus and cursor

    /// The program gets keys and buttons only while its view is on screen and the app is
    /// active (not behind the game menu, a sheet, or another app).
    private var focused: Bool {
        let host = GameHostView.shared
        return host.window != nil && !host.isHidden && UIApplication.shared.applicationState == .active
    }

    private static func desktop() -> (w: CGFloat, h: CGFloat) {
        var w: Int32 = 0, h: Int32 = 0
        mid_display_screen_size(&w, &h)
        return (CGFloat(max(w, 1)), CGFloat(max(h, 1)))
    }

    /// A point on the game view in desktop pixels (the view is the desktop, letterboxed).
    func desktopPoint(_ p: CGPoint, in view: UIView) -> CGPoint {
        let d = Self.desktop()
        let x = p.x / max(view.bounds.width, 1) * d.w, y = p.y / max(view.bounds.height, 1) * d.h
        return CGPoint(x: min(max(x.rounded(.down), 0), d.w - 1), y: min(max(y.rounded(.down), 0), d.h - 1))
    }

    /// Put the program's cursor at a desktop position (and the drawn arrow, when shown).
    func moveCursor(to p: CGPoint, showArrow: Bool) {
        let d = Self.desktop()
        cursor = CGPoint(x: min(max(p.x, 0), d.w - 1), y: min(max(p.y, 0), d.h - 1))
        mid_post_pointer(Int32(cursor.x), Int32(cursor.y), Self.move | Self.absolute, 0)
        GameHostView.shared.showCursor(showArrow ? CGPoint(x: cursor.x / d.w, y: cursor.y / d.h) : nil)
    }

    /// The right stick's cursor motion, in desktop pixels. Main thread.
    func stickMoved(dx: CGFloat, dy: CGFloat) {
        guard focused else { return }
        moveCursor(to: CGPoint(x: cursor.x + dx, y: cursor.y + dy), showArrow: true)
    }

    // MARK: - touch (called by GameHostView)

    /// How a finger drives the mouse (the game menu switches it; remembered):
    /// - trackpad: a drawn cursor; one finger moves it, a tap clicks where it is, a
    ///   two-finger tap right-clicks, a tap then hold drags, two fingers scroll;
    /// - direct: the finger is the pointer (tap where you click, hold or drag to drag).
    enum TouchMode: String { case trackpad, direct }
    var touchMode = TouchMode(rawValue: UserDefaults.standard.string(forKey: "touchMode") ?? "") ?? .trackpad {
        didSet {
            UserDefaults.standard.set(touchMode.rawValue, forKey: "touchMode")
            resetGesture(); resetTrackpad()
            GameHostView.shared.showCursor(nil)
            surfaceShown()
        }
    }
    private var cursorPlaced = false

    /// The game view is on screen: in trackpad mode the cursor is drawn (the first time at the
    /// desktop's centre, which the program is told too).
    func surfaceShown() {
        guard touchMode == .trackpad else { return }
        let d = Self.desktop()
        if !cursorPlaced { cursorPlaced = true; cursor = CGPoint(x: d.w / 2, y: d.h / 2) }
        moveCursor(to: cursor, showArrow: true)
    }

    func touchesBegan(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        if touchMode == .trackpad { trackpadBegan(fingers(touches), event, in: view) } else { directBegan(touches, in: view) }
    }
    func touchesMoved(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        if touchMode == .trackpad { trackpadMoved(fingers(touches), event, in: view) } else { directMoved(touches, event, in: view) }
    }
    func touchesEnded(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        if touchMode == .trackpad { trackpadEnded(event) } else { directEnded(touches, event, in: view) }
    }
    func touchesCancelled(_ touches: Set<UITouch>, in view: UIView) {
        if touchMode == .trackpad { trackpadCancelled() } else { directCancelled(touches, in: view) }
    }

    // Trackpad: Madeira's desktop-session trackpad (ContentView.swift), with a tap then hold as
    // the drag (a double tap stays two clicks, so Windows sees its double click).
    private var tpTouch: UITouch?
    private var tpLast = CGPoint.zero, tpStart = CGPoint.zero
    private var tpStartTime: TimeInterval = 0, tpLastTap: TimeInterval = 0
    private var tpPeak = 0, tpMoved = false, tpDragging = false
    private var tpTwoY: CGFloat = 0, tpScroll: CGFloat = 0
    private static let tapTime: TimeInterval = 0.3, dragAfterTap: TimeInterval = 0.35
    private static let leftDown: UInt32 = 0x0002, leftUp: UInt32 = 0x0004

    private func resetTrackpad() {
        tpTouch = nil; tpPeak = 0; tpMoved = false; tpTwoY = 0; tpScroll = 0
    }

    /// A click at the cursor (button down and up at its position).
    private func clickAtCursor(_ down: UInt32, _ up: UInt32) {
        let x = Int32(cursor.x), y = Int32(cursor.y)
        mid_post_pointer(x, y, down | Self.absolute, 0)
        mid_post_pointer(x, y, up | Self.absolute, 0)
    }

    private func trackpadBegan(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        guard let t = touches.first else { return }
        let live = liveTouches(event)
        if tpTouch == nil {
            let now = ProcessInfo.processInfo.systemUptime
            tpTouch = t
            tpStart = t.location(in: view); tpLast = tpStart
            tpStartTime = now
            tpPeak = 1; tpMoved = false
            if !cursorPlaced { surfaceShown() }
            if now - tpLastTap < Self.dragAfterTap {   // tap, then hold: drag from the cursor
                tpDragging = true
                mid_post_pointer(Int32(cursor.x), Int32(cursor.y), Self.leftDown | Self.absolute, 0)
            }
        }
        tpPeak = max(tpPeak, live.count)
        if live.count >= 2 { tpTwoY = live.reduce(0) { $0 + $1.location(in: view).y } / CGFloat(live.count); tpScroll = 0 }
    }

    private func trackpadMoved(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let live = liveTouches(event)
        if live.count >= 2, !tpDragging {
            // Two fingers: the wheel, 14 points per notch, content following the fingers.
            let y = live.reduce(0) { $0 + $1.location(in: view).y } / CGFloat(live.count)
            if tpTwoY == 0 { tpTwoY = y }
            tpScroll += y - tpTwoY
            if abs(y - tpTwoY) > 2 { tpMoved = true }
            tpTwoY = y
            while tpScroll <= -14 { tpScroll += 14; mid_post_pointer(Int32(cursor.x), Int32(cursor.y), Self.wheel, -120) }
            while tpScroll >= 14 { tpScroll -= 14; mid_post_pointer(Int32(cursor.x), Int32(cursor.y), Self.wheel, 120) }
            return
        }
        guard let t = tpTouch, touches.contains(t) else { return }
        let p = t.location(in: view)
        if hypot(p.x - tpStart.x, p.y - tpStart.y) > Self.slop { tpMoved = true }
        // About 1.3 desktop widths for a swipe across the view.
        let d = Self.desktop(), gain = d.w / max(view.bounds.width, 1) * 1.3
        let dx = (p.x - tpLast.x) * gain, dy = (p.y - tpLast.y) * gain
        tpLast = p
        if dx != 0 || dy != 0 { moveCursor(to: CGPoint(x: cursor.x + dx, y: cursor.y + dy), showArrow: true) }
    }

    private func trackpadEnded(_ event: UIEvent?) {
        guard liveTouches(event).isEmpty, tpTouch != nil else { return }   // wait for every finger
        let now = ProcessInfo.processInfo.systemUptime
        if tpDragging {
            tpDragging = false
            mid_post_pointer(Int32(cursor.x), Int32(cursor.y), Self.leftUp | Self.absolute, 0)
        } else if !tpMoved, now - tpStartTime < Self.tapTime {
            if tpPeak >= 2 {
                clickAtCursor(Self.rightDown, Self.rightUp)
            } else {
                clickAtCursor(Self.leftDown, Self.leftUp)
                tpLastTap = now
            }
        }
        resetTrackpad()
    }

    private func trackpadCancelled() {
        if tpDragging {
            tpDragging = false
            mid_post_pointer(Int32(cursor.x), Int32(cursor.y), Self.leftUp | Self.absolute, 0)   // never leave it held
        }
        resetTrackpad()
    }

    // Direct: the finger is the pointer.
    private var downPoints: [ObjectIdentifier: CGPoint] = [:]
    private var gestureStart = CGPoint.zero
    private var peak = 0
    private var resolved = false            // a one-finger drag owns the left button
    private var dragTouch: UITouch?
    private var slopBroken = false
    private var generation = 0
    private var lastTwoY: CGFloat = 0
    private var scrollAccum: CGFloat = 0
    private static let holdDelay: TimeInterval = 0.25
    private static let slop: CGFloat = 10

    private func resetGesture() {
        downPoints.removeAll()
        peak = 0
        resolved = false
        dragTouch = nil
        slopBroken = false
        generation += 1
        lastTwoY = 0
        scrollAccum = 0
    }

    private func commitDrag(_ t: UITouch, in view: UIView) {
        guard !resolved else { return }
        resolved = true
        dragTouch = t
        let p = desktopPoint(gestureStart, in: view)
        cursor = p
        mid_post_touch(0, Int32(p.x), Int32(p.y))
    }

    private func midpoint() -> CGPoint {
        let pts = Array(downPoints.values)
        guard !pts.isEmpty else { return gestureStart }
        let n = CGFloat(pts.count)
        return CGPoint(x: pts.reduce(0) { $0 + $1.x } / n, y: pts.reduce(0) { $0 + $1.y } / n)
    }

    private func liveTouches(_ event: UIEvent?) -> [UITouch] {
        fingers(event?.allTouches ?? []).filter { $0.phase != .ended && $0.phase != .cancelled }
    }

    /// A mouse click (iPad) also arrives as an indirect-pointer touch; while GCMouse delivers
    /// the buttons, that touch would click a second time.
    private func fingers(_ touches: Set<UITouch>) -> Set<UITouch> {
        guard GCMouse.current?.mouseInput != nil else { return touches }
        return touches.filter { $0.type != .indirectPointer }
    }

    private func directBegan(_ all: Set<UITouch>, in view: UIView) {
        let touches = fingers(all)
        guard !touches.isEmpty, !resolved else { return }   // a finger joining a drag changes nothing
        GameHostView.shared.showCursor(nil)   // the finger is the pointer now
        for t in touches where downPoints[ObjectIdentifier(t)] == nil {
            downPoints[ObjectIdentifier(t)] = t.location(in: view)
        }
        peak = max(peak, downPoints.count)
        generation += 1
        guard downPoints.count == 1, let t = touches.first else { return }   // only a multi-finger tap now
        gestureStart = t.location(in: view)
        let gen = generation
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.holdDelay) { [weak self, weak t, weak view] in
            MainActor.assumeIsolated {
                guard let self, let t, let view, self.generation == gen, !self.resolved,
                      self.downPoints.count == 1 else { return }
                self.commitDrag(t, in: view)
            }
        }
    }

    private func directMoved(_ all: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let touches = fingers(all), live = liveTouches(event)
        if !resolved, downPoints.count == 2, live.count == 2 {
            // Two fingers: the wheel, 14 points of travel per notch, content following the fingers.
            let y = live.reduce(0) { $0 + $1.location(in: view).y } / 2
            if lastTwoY == 0 { lastTwoY = y }
            let dy = y - lastTwoY
            if abs(dy) > 2 { slopBroken = true }
            lastTwoY = y
            scrollAccum += dy
            let at = desktopPoint(midpoint(), in: view)
            while scrollAccum <= -14 { scrollAccum += 14; mid_post_pointer(Int32(at.x), Int32(at.y), Self.wheel, -120) }
            while scrollAccum >= 14 { scrollAccum -= 14; mid_post_pointer(Int32(at.x), Int32(at.y), Self.wheel, 120) }
            return
        }
        for t in touches {
            guard let down = downPoints[ObjectIdentifier(t)] else { continue }
            let p = t.location(in: view)
            if resolved {
                guard t === dragTouch else { continue }
                let d = desktopPoint(p, in: view)
                cursor = d
                mid_post_touch(1, Int32(d.x), Int32(d.y))
                continue
            }
            guard hypot(p.x - down.x, p.y - down.y) > Self.slop else { continue }
            slopBroken = true
            if downPoints.count == 1 {
                commitDrag(t, in: view)   // the button goes down where the finger first was
                let d = desktopPoint(p, in: view)
                cursor = d
                mid_post_touch(1, Int32(d.x), Int32(d.y))
            }
        }
    }

    private func directEnded(_ all: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let touches = fingers(all)
        if resolved, let d = dragTouch, touches.contains(d) {
            let p = desktopPoint(d.location(in: view), in: view)
            cursor = p
            mid_post_touch(2, Int32(p.x), Int32(p.y))
            resetGesture()
            return
        }
        guard !touches.isEmpty, !resolved, liveTouches(event).isEmpty else { return }   // wait for every finger
        let fingers = peak, at = desktopPoint(midpoint(), in: view), moved = slopBroken
        resetGesture()
        guard !moved else { return }
        cursor = at
        if fingers == 1 {
            mid_post_touch(0, Int32(at.x), Int32(at.y))
            mid_post_touch(2, Int32(at.x), Int32(at.y))
        } else if fingers == 2 {
            mid_post_pointer(Int32(at.x), Int32(at.y), Self.move | Self.absolute, 0)
            mid_post_pointer(Int32(at.x), Int32(at.y), Self.rightDown | Self.absolute, 0)
            mid_post_pointer(Int32(at.x), Int32(at.y), Self.rightUp | Self.absolute, 0)
        }
    }

    private func directCancelled(_ touches: Set<UITouch>, in view: UIView) {
        if resolved, let d = dragTouch, touches.contains(d) {
            let p = desktopPoint(d.location(in: view), in: view)
            mid_post_touch(2, Int32(p.x), Int32(p.y))   // never leave the button held
        }
        resetGesture()
    }

    // MARK: - hardware keyboard

    private var keysDown = Set<Int32>()   // posted down, not yet up

    private func attachKeyboard(_ kb: GCKeyboard?) {
        guard let kb, let input = kb.keyboardInput else { return }
        kb.handlerQueue = .main
        input.keyChangedHandler = { [weak self] _, _, code, pressed in
            let usage = code.rawValue
            MainActor.assumeIsolated { self?.key(usage, pressed) }
        }
        dlog("[input] keyboard connected: \(kb.vendorName ?? "keyboard")")
    }

    /// A hardware keyboard is attached: its keys reach the program raw, so UIKit's copy of
    /// them as text (the on-screen keyboard's path) must not be typed again.
    var hardwareKeyboard: Bool { GCKeyboard.coalesced?.keyboardInput != nil }

    private func key(_ usage: Int, _ pressed: Bool) {
        guard let vk = Self.vk(forHIDUsage: usage) else { return }
        if pressed {
            guard focused, !keysDown.contains(vk) else { return }
            keysDown.insert(vk)
            mid_post_key(vk, 1)
        } else if keysDown.remove(vk) != nil {
            mid_post_key(vk, 0)
        }
    }

    // MARK: - hardware mouse

    private var buttonsDown = Set<Int>()   // MOUSEEVENTF down flags posted, not yet up
    private var carryX = 0.0, carryY = 0.0
    private var wheelAccum = 0.0

    private func attachMouse(_ mouse: GCMouse?) {
        guard let mouse, let m = mouse.mouseInput else { return }
        mouse.handlerQueue = .main
        // GameController's y points up, Windows' down.
        m.mouseMovedHandler = { [weak self] _, dx, dy in
            MainActor.assumeIsolated { self?.mouseMoved(Double(dx), -Double(dy)) }
        }
        m.leftButton.pressedChangedHandler = { [weak self] _, _, p in
            MainActor.assumeIsolated { self?.mouseButton(0x0002, 0x0004, 0, p) }
        }
        m.rightButton?.pressedChangedHandler = { [weak self] _, _, p in
            MainActor.assumeIsolated { self?.mouseButton(0x0008, 0x0010, 0, p) }
        }
        m.middleButton?.pressedChangedHandler = { [weak self] _, _, p in
            MainActor.assumeIsolated { self?.mouseButton(0x0020, 0x0040, 0, p) }
        }
        m.scroll.valueChangedHandler = { [weak self] _, _, y in
            MainActor.assumeIsolated { self?.mouseWheel(Double(y)) }
        }
        dlog("[input] mouse connected: \(mouse.vendorName ?? "mouse")")
    }

    /// Device motion goes relative (mouse-look reads exact deltas; Wine moves its own cursor),
    /// and the drawn arrow follows our copy of the position.
    private func mouseMoved(_ dx: Double, _ dy: Double) {
        guard focused else { return }
        carryX += dx; carryY += dy
        let ix = Int32(max(-30000, min(30000, carryX))), iy = Int32(max(-30000, min(30000, carryY)))
        carryX -= Double(ix); carryY -= Double(iy)
        guard ix != 0 || iy != 0 else { return }
        mid_post_pointer(ix, iy, Self.move, 0)
        let d = Self.desktop()
        cursor = CGPoint(x: min(max(cursor.x + CGFloat(ix), 0), d.w - 1), y: min(max(cursor.y + CGFloat(iy), 0), d.h - 1))
        GameHostView.shared.showCursor(CGPoint(x: cursor.x / d.w, y: cursor.y / d.h))
    }

    private func mouseButton(_ down: UInt32, _ up: UInt32, _ data: Int32, _ pressed: Bool) {
        if pressed {
            guard focused, buttonsDown.insert(Int(down)).inserted else { return }
            mid_post_pointer(0, 0, down, data)
        } else if buttonsDown.remove(Int(down)) != nil {
            mid_post_pointer(0, 0, up, data)
        }
    }

    private func mouseWheel(_ y: Double) {
        guard focused, y.isFinite else { return }
        wheelAccum += y
        while wheelAccum >= 1 { wheelAccum -= 1; mid_post_pointer(0, 0, Self.wheel, 120) }
        while wheelAccum <= -1 { wheelAccum += 1; mid_post_pointer(0, 0, Self.wheel, -120) }
    }

    /// Everything the program was told is held goes up (focus lost, a device gone).
    func releaseAll(_ why: String) {
        for vk in keysDown { mid_post_key(vk, 0) }
        keysDown.removeAll()
        let ups: [Int: UInt32] = [0x0002: 0x0004, 0x0008: 0x0010, 0x0020: 0x0040]
        for b in buttonsDown { if let up = ups[b] { mid_post_pointer(0, 0, up, 0) } }
        buttonsDown.removeAll()
        carryX = 0; carryY = 0; wheelAccum = 0
        dlog("[input] released all keys and buttons (\(why))")
    }

    // MARK: - on-screen keyboard (GameHostView's UIKeyInput)

    func type(_ text: String) {
        guard !hardwareKeyboard else { return }
        for ch in text {
            guard let (vk, shift) = Self.vk(forChar: ch) else { continue }
            if shift { mid_post_key(0x10, 1) }
            mid_post_key(vk, 1)
            mid_post_key(vk, 0)
            if shift { mid_post_key(0x10, 0) }
        }
    }

    func backspace() {
        guard !hardwareKeyboard else { return }
        mid_post_key(0x08, 1)
        mid_post_key(0x08, 0)
    }

    /// The US-layout key and shift state that types a character (Madeira's vkForChar).
    static func vk(forChar ch: Character) -> (Int32, Bool)? {
        if ch == "\n" || ch == "\r" { return (0x0D, false) }
        if ch == "\t" { return (0x09, false) }
        if ch == " " { return (0x20, false) }
        if ch.isLetter, let up = ch.uppercased().first?.asciiValue, up >= 0x41, up <= 0x5A {
            return (Int32(up), ch.isUppercase)
        }
        if let a = ch.asciiValue, a >= 0x30, a <= 0x39 { return (Int32(a), false) }
        let table: [Character: (Int32, Bool)] = [
            "!": (0x31, true), "@": (0x32, true), "#": (0x33, true), "$": (0x34, true),
            "%": (0x35, true), "^": (0x36, true), "&": (0x37, true), "*": (0x38, true),
            "(": (0x39, true), ")": (0x30, true),
            "-": (0xBD, false), "_": (0xBD, true), "=": (0xBB, false), "+": (0xBB, true),
            "[": (0xDB, false), "{": (0xDB, true), "]": (0xDD, false), "}": (0xDD, true),
            "\\": (0xDC, false), "|": (0xDC, true), ";": (0xBA, false), ":": (0xBA, true),
            "'": (0xDE, false), "\"": (0xDE, true), ",": (0xBC, false), "<": (0xBC, true),
            ".": (0xBE, false), ">": (0xBE, true), "/": (0xBF, false), "?": (0xBF, true),
            "`": (0xC0, false), "~": (0xC0, true),
        ]
        return table[ch]
    }

    /// Windows virtual key for a USB HID keyboard usage (page 0x07), or nil for a key
    /// Windows has none for (Madeira's HardwareKeyMap). Modifiers keep left/right identity;
    /// Wine derives scan codes and the generic VK_SHIFT/VK_CONTROL/VK_MENU itself.
    static func vk(forHIDUsage u: Int) -> Int32? {
        switch u {
        case 0x04...0x1D: return Int32(0x41 + (u - 0x04))   // A-Z
        case 0x1E...0x26: return Int32(0x31 + (u - 0x1E))   // 1-9
        case 0x27: return 0x30                              // 0
        case 0x28: return 0x0D                              // Return
        case 0x29: return 0x1B                              // Escape
        case 0x2A: return 0x08                              // Backspace
        case 0x2B: return 0x09                              // Tab
        case 0x2C: return 0x20                              // Space
        case 0x2D: return 0xBD                              // -
        case 0x2E: return 0xBB                              // =
        case 0x2F: return 0xDB                              // [
        case 0x30: return 0xDD                              // ]
        case 0x31, 0x32: return 0xDC                        // backslash (and ISO #)
        case 0x33: return 0xBA                              // ;
        case 0x34: return 0xDE                              // '
        case 0x35: return 0xC0                              // `
        case 0x36: return 0xBC                              // ,
        case 0x37: return 0xBE                              // .
        case 0x38: return 0xBF                              // /
        case 0x39: return 0x14                              // Caps Lock
        case 0x3A...0x45: return Int32(0x70 + (u - 0x3A))   // F1-F12
        case 0x46: return 0x2C                              // Print Screen
        case 0x47: return 0x91                              // Scroll Lock
        case 0x48: return 0x13                              // Pause
        case 0x49: return 0x2D                              // Insert
        case 0x4A: return 0x24                              // Home
        case 0x4B: return 0x21                              // Page Up
        case 0x4C: return 0x2E                              // Delete
        case 0x4D: return 0x23                              // End
        case 0x4E: return 0x22                              // Page Down
        case 0x4F: return 0x27                              // Right
        case 0x50: return 0x25                              // Left
        case 0x51: return 0x28                              // Down
        case 0x52: return 0x26                              // Up
        case 0x53: return 0x90                              // Num Lock
        case 0x54: return 0x6F                              // keypad /
        case 0x55: return 0x6A                              // keypad *
        case 0x56: return 0x6D                              // keypad -
        case 0x57: return 0x6B                              // keypad +
        case 0x58: return 0x0D                              // keypad Enter
        case 0x59...0x61: return Int32(0x61 + (u - 0x59))   // keypad 1-9
        case 0x62: return 0x60                              // keypad 0
        case 0x63: return 0x6E                              // keypad .
        case 0x64: return 0xE2                              // ISO < >
        case 0x65: return 0x5D                              // Menu (Apps)
        case 0x68...0x73: return Int32(0x7C + (u - 0x68))   // F13-F24
        case 0xE0: return 0xA2                              // left Control
        case 0xE1: return 0xA0                              // left Shift
        case 0xE2: return 0xA4                              // left Alt
        case 0xE3: return 0x5B                              // left Windows (Command)
        case 0xE4: return 0xA3                              // right Control
        case 0xE5: return 0xA1                              // right Shift
        case 0xE6: return 0xA5                              // right Alt
        case 0xE7: return 0x5C                              // right Windows (Command)
        default: return nil
        }
    }
}
