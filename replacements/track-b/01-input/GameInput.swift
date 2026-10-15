// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

import UIKit
import GameController
import QuartzCore

struct BInputGeometry {
    let desktop: CGRect
    let picture: CGRect
    let reach: CGRect

    init(width: Int32, height: Int32, picture proposed: CGRect?) {
        desktop = CGRect(x: 0, y: 0, width: CGFloat(max(1, width)), height: CGFloat(max(1, height)))
        if let p = proposed, p.width > 0, p.height > 0, !p.isInfinite, !p.isNull {
            picture = p
        } else { picture = desktop }
        let intersection = picture.intersection(desktop)
        reach = intersection.isEmpty || intersection.isNull ? desktop : intersection
    }

    static func clamp(_ p: CGPoint, to r: CGRect) -> CGPoint {
        let x = p.x.isFinite ? p.x : r.minX
        let y = p.y.isFinite ? p.y : r.minY
        return CGPoint(x: min(max(x, r.minX), max(r.minX, r.maxX - 1)),
                       y: min(max(y, r.minY), max(r.minY, r.maxY - 1)))
    }

    func map(_ p: CGPoint, bounds: CGRect) -> CGPoint {
        guard bounds.width > 0, bounds.height > 0 else { return Self.clamp(picture.origin, to: reach) }
        return Self.clamp(CGPoint(x: floor(picture.minX + (p.x - bounds.minX) * picture.width / bounds.width),
                                  y: floor(picture.minY + (p.y - bounds.minY) * picture.height / bounds.height)), to: reach)
    }

    func unit(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x - picture.minX) / picture.width, y: (p.y - picture.minY) / picture.height)
    }
}

enum BInputKeys {
    static func virtualKey(_ usage: Int) -> Int32? {
        switch usage {
        case 0x04...0x1d: return Int32(0x41 + usage - 0x04)
        case 0x1e...0x26: return Int32(0x31 + usage - 0x1e)
        case 0x27: return 0x30
        case 0x3a...0x45: return Int32(0x70 + usage - 0x3a)
        case 0x59...0x61: return Int32(0x61 + usage - 0x59)
        case 0x68...0x73: return Int32(0x7c + usage - 0x68)
        default: break
        }
        let fixed: [Int: Int32] = [
            0x28: 0x0d, 0x29: 0x1b, 0x2a: 0x08, 0x2b: 0x09, 0x2c: 0x20,
            0x2d: 0xbd, 0x2e: 0xbb, 0x2f: 0xdb, 0x30: 0xdd, 0x31: 0xdc, 0x32: 0xdc,
            0x33: 0xba, 0x34: 0xde, 0x35: 0xc0, 0x36: 0xbc, 0x37: 0xbe, 0x38: 0xbf,
            0x39: 0x14, 0x46: 0x2c, 0x47: 0x91, 0x48: 0x13,
            0x49: 0x2d, 0x4a: 0x24, 0x4b: 0x21, 0x4c: 0x2e, 0x4d: 0x23, 0x4e: 0x22,
            0x4f: 0x27, 0x50: 0x25, 0x51: 0x28, 0x52: 0x26,
            0x53: 0x90, 0x54: 0x6f, 0x55: 0x6a, 0x56: 0x6d, 0x57: 0x6b, 0x58: 0x0d,
            0x62: 0x60, 0x63: 0x6e, 0x64: 0xe2, 0x65: 0x5d,
            0xe0: 0xa2, 0xe1: 0xa0, 0xe2: 0xa4, 0xe3: 0x5b,
            0xe4: 0xa3, 0xe5: 0xa1, 0xe6: 0xa5, 0xe7: 0x5c
        ]
        return fixed[usage]
    }

    static func normalized(_ text: String) -> String {
        var result = ""
        for c in text {
            switch c {
            case "‘", "’", "‚", "′": result += "'"
            case "“", "”", "„", "″": result += "\""
            case "–", "—", "−": result += "-"
            case "…": result += "..."
            case "\u{00a0}": result += " "
            default: result.append(c)
            }
        }
        return result
    }

    static func textKey(_ c: Character) -> (key: Int32, shift: Bool)? {
        if c == "\r\n" || c == "\n" || c == "\r" { return (0x0d, false) }
        if c == "\t" { return (0x09, false) }
        if c == " " { return (0x20, false) }
        guard c.unicodeScalars.count == 1, let v = c.unicodeScalars.first?.value else { return nil }
        if (65...90).contains(v) { return (Int32(v), true) }
        if (97...122).contains(v) { return (Int32(v - 32), false) }
        if (48...57).contains(v) { return (Int32(v), false) }
        let pairs: [(String, Int32)] = [
            ("1!",0x31), ("2@",0x32), ("3#",0x33), ("4$",0x34), ("5%",0x35),
            ("6^",0x36), ("7&",0x37), ("8*",0x38), ("9(",0x39), ("0)",0x30),
            ("-_",0xbd), ("=+",0xbb), ("[{",0xdb), ("]}",0xdd), ("\\|",0xdc),
            (";:",0xba), ("'\"",0xde), ("`~",0xc0), (",<",0xbc), (".>",0xbe), ("/?",0xbf)
        ]
        for (pair, key) in pairs {
            let chars = Array(pair)
            if c == chars[0] { return (key, false) }
            if c == chars[1] { return (key, true) }
        }
        return nil
    }
}

@MainActor final class GameInput {
    static let shared = GameInput()
    enum TouchMode: String, CaseIterable { case off, trackpad, direct }
    private(set) var cursor = CGPoint(x: 512, y: 384)
    private(set) var touchMode: TouchMode = .off
    var hardwareKeyboard: Bool { GCKeyboard.coalesced != nil }

    private struct Finger {
        let touch: UITouch
        let start: CGPoint
        var point: CGPoint
    }
    private var started = false
    private var observers: [NSObjectProtocol] = []
    private var keyboard: GCKeyboard?
    private var mice: [ObjectIdentifier: GCMouse] = [:]
    private var programID: String?
    private var placed = false
    private var hardwareKeys = Set<Int32>()
    private var controllerKeys = Set<Int32>()
    private var hardwareButtons = Set<UInt32>()
    private var controllerButtons = Set<UInt32>()
    private var motionX = 0.0
    private var motionY = 0.0
    private var mouseWheelCarry = 0.0
    private var fingers: [ObjectIdentifier: Finger] = [:]
    private var landings: [CGPoint] = []
    private var beganAt = 0.0
    private var maxFingers = 0
    private var moved = false
    private var dragging = false
    private var dragFinger: ObjectIdentifier?
    private var generation: UInt64 = 0
    private var lastTap: TimeInterval?
    private var wheelBase: CGFloat?
    private var wheelCarry: CGFloat = 0
    private var wheelTravel: CGFloat = 0
    private var wheelPositioned = false

    private init() {}
    private var focused: Bool {
        let view = GameHostView.shared
        return view.window != nil && !view.isHidden && UIApplication.shared.applicationState == .active
    }

    func start() {
        guard !started else { return }
        started = true
        attachKeyboard(GCKeyboard.coalesced)
        for mouse in GCMouse.mice() { attachMouse(mouse) }
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: .GCKeyboardDidConnect, object: nil, queue: .main) { [weak self] note in
            MainActor.assumeIsolated { self?.attachKeyboard(note.object as? GCKeyboard ?? GCKeyboard.coalesced) }
        })
        observers.append(center.addObserver(forName: .GCKeyboardDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.keyboard?.keyboardInput?.keyChangedHandler = nil
                self?.keyboard = nil
                self?.releaseAll("keyboard disconnected")
            }
        })
        observers.append(center.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] note in
            MainActor.assumeIsolated { if let m = note.object as? GCMouse { self?.attachMouse(m) } }
        })
        observers.append(center.addObserver(forName: .GCMouseDidDisconnect, object: nil, queue: .main) { [weak self] note in
            MainActor.assumeIsolated {
                guard let self = self else { return }
                if let mouse = note.object as? GCMouse {
                    self.detachMouse(mouse)
                    self.mice.removeValue(forKey: ObjectIdentifier(mouse))
                }
                self.releaseAll("mouse disconnected")
            }
        })
        observers.append(center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseAll("app inactive") }
        })
    }

    private func attachKeyboard(_ device: GCKeyboard?) {
        guard let device = device else { return }
        if keyboard === device { return }
        keyboard?.keyboardInput?.keyChangedHandler = nil
        keyboard = device
        device.handlerQueue = .main
        device.keyboardInput?.keyChangedHandler = { [weak self] _, _, code, pressed in
            MainActor.assumeIsolated { self?.keyboardChanged(Int(code.rawValue), pressed: pressed) }
        }
        dlog("[input] keyboard connected: \(device.vendorName ?? "unknown")")
    }

    private func attachMouse(_ mouse: GCMouse) {
        let id = ObjectIdentifier(mouse)
        guard mice[id] == nil else { return }
        mice[id] = mouse
        mouse.handlerQueue = .main
        guard let input = mouse.mouseInput else { return }
        input.mouseMovedHandler = { [weak self] _, x, y in
            MainActor.assumeIsolated { self?.mouseMoved(Double(x), Double(-y)) }
        }
        for (button, flag) in [(input.leftButton as GCControllerButtonInput?, UInt32(2)),
                               (input.rightButton, UInt32(8)), (input.middleButton, UInt32(32))] {
            button?.pressedChangedHandler = { [weak self] _, _, pressed in
                MainActor.assumeIsolated { self?.mouseButton(flag, pressed: pressed) }
            }
        }
        input.scroll.valueChangedHandler = { [weak self] _, _, y in
            MainActor.assumeIsolated { self?.mouseWheel(Double(y)) }
        }
        dlog("[input] mouse connected: \(mouse.vendorName ?? "unknown")")
    }

    private func detachMouse(_ mouse: GCMouse) {
        guard let input = mouse.mouseInput else { return }
        input.mouseMovedHandler = nil
        input.leftButton.pressedChangedHandler = nil
        input.rightButton?.pressedChangedHandler = nil
        input.middleButton?.pressedChangedHandler = nil
        input.scroll.valueChangedHandler = nil
    }

    func load(for programID: String) {
        self.programID = programID
        placed = false
        let saved = UserDefaults.standard.string(forKey: "touchMode.\(programID)") ?? ""
        applyMode(TouchMode(rawValue: saved) ?? .off)
    }

    func setTouchMode(_ mode: TouchMode) {
        if let id = programID { UserDefaults.standard.set(mode.rawValue, forKey: "touchMode.\(id)") }
        dlog("[input] touch for this game: \(mode.rawValue)")
        applyMode(mode)
    }

    private func applyMode(_ mode: TouchMode) {
        cancelGesture()
        lastTap = nil
        touchMode = mode
        GameHostView.shared.showCursor(nil)
        surfaceShown()
    }

    func surfaceShown() {
        guard touchMode == .trackpad else { return }
        if !placed {
            let r = geometry(in: GameHostView.shared).reach
            cursor = CGPoint(x: r.midX, y: r.midY)
            placed = true
        }
        moveCursor(to: cursor, showArrow: true)
    }

    private func geometry(in view: UIView) -> BInputGeometry {
        var w: Int32 = 1, h: Int32 = 1
        mid_display_screen_size(&w, &h)
        let size = (view as? GameHostView)?.metalLayer.drawableSize ?? GameHostView.shared.metalLayer.drawableSize
        var x: Int32 = 0, y: Int32 = 0, pw: Int32 = 0, ph: Int32 = 0
        let valid = mid_game_picture_rect(Int32(clamping: Int(size.width)), Int32(clamping: Int(size.height)), &x, &y, &pw, &ph)
        let p = valid != 0 && pw > 0 && ph > 0 ? CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(pw), height: CGFloat(ph)) : nil
        return BInputGeometry(width: w, height: h, picture: p)
    }

    func desktopPoint(_ p: CGPoint, in view: UIView) -> CGPoint { geometry(in: view).map(p, bounds: view.bounds) }

    func moveCursor(to p: CGPoint, showArrow: Bool) {
        let g = geometry(in: GameHostView.shared)
        cursor = BInputGeometry.clamp(p, to: g.reach)
        mid_post_pointer(pixel(cursor.x), pixel(cursor.y), 0x8001, 0)
        GameHostView.shared.showCursor(showArrow ? g.unit(cursor) : nil)
    }

    private func pixel(_ value: CGFloat) -> Int32 {
        Int32(min(CGFloat(Int32.max), max(CGFloat(Int32.min), value.rounded(.towardZero))))
    }

    func stickMoved(dx: CGFloat, dy: CGFloat) {
        guard focused else { return }
        moveCursor(to: CGPoint(x: cursor.x + dx, y: cursor.y + dy), showArrow: true)
    }

    static func traceInput() { mid_wow_trace_calls(3) }

    private var allKeys: Set<Int32> { hardwareKeys.union(controllerKeys) }
    private var allButtons: Set<UInt32> { hardwareButtons.union(controllerButtons) }
    private func buttonUp(_ down: UInt32) -> UInt32 { down << 1 }

    private func keyboardChanged(_ usage: Int, pressed: Bool) {
        guard let vk = BInputKeys.virtualKey(usage) else { return }
        if pressed {
            guard focused, !hardwareKeys.contains(vk) else { return }
            let already = allKeys.contains(vk)
            hardwareKeys.insert(vk)
            Self.traceInput()
            if !already { mid_post_key(vk, 1) }
        } else if hardwareKeys.remove(vk) != nil, !allKeys.contains(vk) { mid_post_key(vk, 0) }
    }

    private func mouseButton(_ flag: UInt32, pressed: Bool) {
        if pressed {
            guard focused, !hardwareButtons.contains(flag) else { return }
            let already = allButtons.contains(flag)
            hardwareButtons.insert(flag)
            Self.traceInput()
            if !already { mid_post_pointer(0, 0, flag, 0) }
        } else if hardwareButtons.remove(flag) != nil, !allButtons.contains(flag) {
            mid_post_pointer(0, 0, buttonUp(flag), 0)
        }
    }

    private func mouseMoved(_ x: Double, _ y: Double) {
        guard focused, x.isFinite, y.isFinite else { return }
        motionX += x; motionY += y
        let dx = min(30000, max(-30000, motionX.rounded(.towardZero)))
        let dy = min(30000, max(-30000, motionY.rounded(.towardZero)))
        motionX -= dx; motionY -= dy
        guard dx != 0 || dy != 0 else { return }
        mid_post_pointer(Int32(dx), Int32(dy), 0x0001, 0)
        let g = geometry(in: GameHostView.shared)
        cursor = BInputGeometry.clamp(CGPoint(x: cursor.x + CGFloat(dx), y: cursor.y + CGFloat(dy)), to: g.desktop)
        GameHostView.shared.showCursor(g.unit(cursor))
    }

    private func mouseWheel(_ value: Double) {
        guard focused, value.isFinite else { return }
        mouseWheelCarry += value
        let n = Int(min(100, max(-100, mouseWheelCarry.rounded(.towardZero))))
        mouseWheelCarry -= Double(n)
        for _ in 0..<abs(n) { mid_post_pointer(0, 0, 0x0800, n > 0 ? 120 : -120) }
    }

    // One queue-to-main transaction per controller sample. Union ownership avoids
    // releasing a physical key when its keyboard-mode counterpart changes.
    func bApplyController(dx: CGFloat, dy: CGFloat, keys: Set<Int32>, buttons: Set<UInt32>) {
        if dx != 0 || dy != 0 { stickMoved(dx: dx, dy: dy) }
        let nextKeys = focused ? keys : []
        let nextButtons = focused ? buttons : []
        let oldKeys = allKeys
        let oldButtons = allButtons
        controllerKeys = nextKeys
        controllerButtons = nextButtons
        for key in oldKeys.subtracting(allKeys).sorted() { mid_post_key(key, 0) }
        for flag in oldButtons.subtracting(allButtons).sorted() { mid_post_mouse_button(buttonUp(flag)) }
        for key in allKeys.subtracting(oldKeys).sorted() { Self.traceInput(); mid_post_key(key, 1) }
        for flag in allButtons.subtracting(oldButtons).sorted() { Self.traceInput(); mid_post_mouse_button(flag) }
    }

    func bClearController() { bApplyController(dx: 0, dy: 0, keys: [], buttons: []) }

    func releaseAll(_ why: String) {
        let keys = allKeys
        let buttons = allButtons
        let physicalButtons = hardwareButtons
        hardwareKeys.removeAll(); controllerKeys.removeAll()
        hardwareButtons.removeAll(); controllerButtons.removeAll()
        for key in keys.sorted() { mid_post_key(key, 0) }
        for flag in buttons.sorted() {
            if physicalButtons.contains(flag) { mid_post_pointer(0, 0, buttonUp(flag), 0) }
            else { mid_post_mouse_button(buttonUp(flag)) }
        }
        motionX = 0; motionY = 0; mouseWheelCarry = 0; wheelCarry = 0
        ControllerBridge.shared.bResetCursorCarry()
        dlog("[input] released all keys and buttons (\(why))")
    }

    private func pulse(_ vk: Int32) {
        // Text owns only its transient press; preserve another route's held state.
        guard !allKeys.contains(vk) else { return }
        Self.traceInput(); mid_post_key(vk, 1); mid_post_key(vk, 0)
    }

    func type(_ text: String) {
        guard focused else { return }
        Self.traceInput()
        guard !hardwareKeyboard else { return }
        for c in BInputKeys.normalized(text) {
            guard let entry = BInputKeys.textKey(c) else { continue }
            let shift = entry.shift && !allKeys.contains(0x10)
            if shift { Self.traceInput(); mid_post_key(0x10, 1) }
            pulse(entry.key)
            if shift { mid_post_key(0x10, 0) }
        }
    }

    func backspace() {
        guard focused else { return }
        Self.traceInput()
        if !hardwareKeyboard { pulse(0x08) }
    }

    private func accepted(_ touches: Set<UITouch>) -> [UITouch] {
        touches.filter { mice.isEmpty || $0.type != .indirectPointer }
    }

    func touchesBegan(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let incoming = accepted(touches)
        guard !incoming.isEmpty else { return }
        if touchMode == .direct && dragging { return }
        if fingers.isEmpty {
            resetGesture()
            beganAt = incoming.map { $0.timestamp }.min() ?? ProcessInfo.processInfo.systemUptime
            if touchMode == .trackpad {
                if !placed { surfaceShown() }
                if let last = lastTap, beganAt - last >= 0, beganAt - last < 0.35 {
                    dragging = true
                    postClickFlag(0x0002)
                }
                lastTap = nil
            }
        }
        if touchMode == .direct { GameHostView.shared.showCursor(nil) }
        for touch in incoming {
            let id = ObjectIdentifier(touch)
            let point = touch.location(in: view)
            fingers[id] = Finger(touch: touch, start: point, point: point)
            landings.append(point)
        }
        maxFingers = max(maxFingers, fingers.count)
        rebaseWheel()
        if touchMode == .direct && maxFingers == 1 && fingers.count == 1 {
            let ticket = generation
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self, weak view] in
                guard let self = self, let view = view, self.generation == ticket,
                      self.touchMode == .direct, !self.dragging, !self.moved,
                      self.maxFingers == 1, self.fingers.count == 1,
                      let id = self.fingers.keys.first else { return }
                self.beginDirectDrag(id, in: view)
            }
        }
    }

    func touchesMoved(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let oldSingle = fingers.count == 1 ? fingers.values.first?.point : nil
        updateFingers(accepted(touches), in: view)
        guard !fingers.isEmpty else { return }
        if touchMode == .direct {
            if !dragging && maxFingers == 1 && moved, let id = fingers.keys.first { beginDirectDrag(id, in: view) }
            if dragging, let id = dragFinger, let f = fingers[id] { postTouch(1, desktopPoint(f.point, in: view)) }
            else if fingers.count >= 2 { scroll(in: view) }
        } else if touchMode == .trackpad {
            if fingers.count == 1, let old = oldSingle, let next = fingers.values.first?.point {
                let gain = geometry(in: view).picture.width / max(1, view.bounds.width) * 1.3
                moveCursor(to: CGPoint(x: cursor.x + (next.x - old.x) * gain, y: cursor.y + (next.y - old.y) * gain), showArrow: true)
            } else if !dragging && fingers.count >= 2 { scroll(in: view) }
        }
    }

    private func updateFingers(_ touches: [UITouch], in view: UIView) {
        for touch in touches {
            let id = ObjectIdentifier(touch)
            guard var finger = fingers[id] else { continue }
            finger.point = touch.location(in: view)
            if hypot(finger.point.x - finger.start.x, finger.point.y - finger.start.y) > 10 { moved = true }
            fingers[id] = finger
        }
    }

    private func beginDirectDrag(_ id: ObjectIdentifier, in view: UIView) {
        guard let f = fingers[id], !dragging else { return }
        dragging = true
        dragFinger = id
        Self.traceInput()
        postTouch(0, desktopPoint(f.start, in: view))
    }

    private func postTouch(_ phase: Int32, _ p: CGPoint) {
        cursor = p
        mid_post_touch(phase, pixel(p.x), pixel(p.y))
    }

    private func postClickFlag(_ flag: UInt32) {
        if flag == 0x0002 || flag == 0x0008 { Self.traceInput() }
        mid_post_pointer(pixel(cursor.x), pixel(cursor.y), flag | 0x8000, 0)
    }

    private func midpoint(_ points: [CGPoint]) -> CGPoint {
        guard !points.isEmpty else { return .zero }
        return CGPoint(x: points.reduce(0) { $0 + $1.x } / CGFloat(points.count),
                       y: points.reduce(0) { $0 + $1.y } / CGFloat(points.count))
    }

    private func rebaseWheel() {
        wheelBase = fingers.count >= 2 ? midpoint(fingers.values.map { $0.point }).y : nil
    }

    private func scroll(in view: UIView) {
        let mid = midpoint(fingers.values.map { $0.point })
        guard let base = wheelBase else { wheelBase = mid.y; return }
        let delta = mid.y - base
        wheelBase = mid.y
        wheelTravel += abs(delta)
        if wheelTravel > 2 { moved = true }
        wheelCarry += delta
        let n = Int(min(100, max(-100, (wheelCarry / 14).rounded(.towardZero))))
        guard n != 0 else { return }
        wheelCarry -= CGFloat(n) * 14
        if touchMode == .direct && !wheelPositioned {
            moveCursor(to: desktopPoint(mid, in: view), showArrow: false)
            wheelPositioned = true
        }
        for _ in 0..<abs(n) { mid_post_pointer(0, 0, 0x0800, n > 0 ? 120 : -120) }
    }

    func touchesEnded(_ touches: Set<UITouch>, _ event: UIEvent?, in view: UIView) {
        let ending = accepted(touches).filter { fingers[ObjectIdentifier($0)] != nil }
        guard !ending.isEmpty else { return }
        updateFingers(ending, in: view)
        let time = ending.map { $0.timestamp }.max() ?? ProcessInfo.processInfo.systemUptime
        if touchMode == .direct && dragging, let id = dragFinger,
           ending.contains(where: { ObjectIdentifier($0) == id }), let f = fingers[id] {
            postTouch(2, desktopPoint(f.point, in: view))
            resetGesture()
            return
        }
        for touch in ending { fingers.removeValue(forKey: ObjectIdentifier(touch)) }
        if fingers.isEmpty { finishGesture(at: time, in: view) }
        else { rebaseWheel() }
    }

    private func finishGesture(at time: TimeInterval, in view: UIView) {
        let duration = time - beganAt
        if touchMode == .trackpad && dragging { postClickFlag(0x0004) }
        else if !moved {
            if maxFingers >= 3 && duration < 0.6 { GameHostView.onMenu?() }
            else if touchMode == .trackpad && duration < 0.3 {
                if maxFingers == 1 {
                    postClickFlag(0x0002); postClickFlag(0x0004); lastTap = time
                } else if maxFingers == 2 { postClickFlag(0x0008); postClickFlag(0x0010) }
            } else if touchMode == .direct && maxFingers < 3 {
                let point = desktopPoint(midpoint(landings), in: view)
                if maxFingers == 1 { Self.traceInput(); postTouch(0, point); postTouch(2, point) }
                else if maxFingers == 2 {
                    moveCursor(to: point, showArrow: false)
                    postClickFlag(0x0008); postClickFlag(0x0010)
                }
            }
        }
        resetGesture()
    }

    func touchesCancelled(_ touches: Set<UITouch>, in view: UIView) {
        guard accepted(touches).contains(where: { fingers[ObjectIdentifier($0)] != nil }) else { return }
        updateFingers(accepted(touches), in: view)
        if touchMode == .direct && dragging, let id = dragFinger, let f = fingers[id] {
            postTouch(2, desktopPoint(f.point, in: view))
            dragging = false
        }
        cancelGesture()
        lastTap = nil
    }

    private func cancelGesture() {
        if dragging {
            if touchMode == .trackpad { postClickFlag(0x0004) }
            else if touchMode == .direct { postTouch(2, cursor) }
        }
        resetGesture()
    }

    private func resetGesture() {
        generation &+= 1
        fingers.removeAll(); landings.removeAll()
        maxFingers = 0; moved = false; dragging = false; dragFinger = nil
        wheelBase = nil; wheelCarry = 0; wheelTravel = 0; wheelPositioned = false
    }
}
