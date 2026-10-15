// SPDX-License-Identifier: GPL-3.0-or-later
import UIKit
import SwiftUI
import GameController

/// The on-screen controller as a UIKit view: drawn controls (no blur over the game), multitouch,
/// and the layout editor's drag and resize. In play mode only the controls take touches: a touch
/// anywhere else falls through to the game view below (touch mouse, three-finger tap).
final class TouchControllerView: UIView {
    enum Mode { case play, edit }
    let mode: Mode

    private(set) var layout = TouchLayout.standard(landscape: true)
    /// Edit mode: the selected control (resize handles, the Size slider).
    var selected: PadControl? { didSet { if selected != oldValue { setNeedsLayout() } } }
    /// Play: the state the touches make, on every change.
    var onState: ((PadState) -> Void)?
    /// Edit: the layout after a drag or resize ended; the control a touch selected (or nil).
    var onLayoutChanged: ((TouchLayout) -> Void)?
    var onSelect: ((PadControl?) -> Void)?
    /// The layout for an orientation (play mode reloads it when the screen turns).
    var layoutProvider: ((Bool) -> TouchLayout)?
    var haptics = true

    private var views: [PadControl: PadControlView] = [:]
    private let selection = SelectionFrameView()
    private var landscape: Bool?
    private(set) var state = PadState()
    private let feedback = UIImpactFeedbackGenerator(style: .light)

    init(mode: Mode) {
        self.mode = mode
        super.init(frame: .zero)
        isMultipleTouchEnabled = true
        backgroundColor = .clear
        isOpaque = false
        for c in PadControl.allCases {
            let v = PadControlView(control: c)
            v.isUserInteractionEnabled = false
            views[c] = v
            addSubview(v)
        }
        selection.isUserInteractionEnabled = false
        selection.isHidden = true
        addSubview(selection)
        isAccessibilityElement = false
        accessibilityLabel = "On-screen controller"
    }
    required init?(coder: NSCoder) { fatalError() }

    func setLayout(_ l: TouchLayout) {
        guard l != layout else { return }
        layout = l
        setNeedsLayout()
    }

    /// Asks the provider again (a new game, or the editor saved).
    func reloadFromProvider() {
        landscape = nil
        setNeedsLayout()
    }

    var safeRect: CGRect {
        let r = bounds.inset(by: safeAreaInsets).insetBy(dx: 6, dy: 4)
        return r.width > 0 && r.height > 0 ? r : bounds
    }

    override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        setNeedsLayout()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let isLandscape = bounds.width >= bounds.height
        if landscape != isLandscape, bounds.width > 0, let provider = layoutProvider {
            landscape = isLandscape
            releaseAll()
            layout = provider(isLandscape)
        }
        let safe = safeRect
        for spec in layout.controls {
            guard let v = views[spec.control] else { continue }
            v.isHidden = spec.hidden
            v.frame = TouchLayout.frame(spec, in: safe)
            v.selectedLook = mode == .edit && spec.control == selected
            v.setNeedsDisplay()
        }
        alpha = 1
        for v in views.values { v.alpha = CGFloat(layout.opacity) }
        if mode == .edit, let sel = selected, let spec = layout.spec(sel), !spec.hidden {
            selection.frame = TouchLayout.frame(spec, in: safe).insetBy(dx: -10, dy: -10)
            selection.isHidden = false
            bringSubviewToFront(selection)
        } else {
            selection.isHidden = true
        }
    }

    // MARK: hit testing

    /// The control under a point (a little larger than drawn), the nearest when several are.
    private func control(at p: CGPoint) -> PadControl? {
        var best: (PadControl, CGFloat)?
        for spec in layout.controls where !spec.hidden {
            let f = TouchLayout.frame(spec, in: safeRect)
            guard f.insetBy(dx: -14, dy: -14).contains(p) else { continue }
            let d = hypot(p.x - f.midX, p.y - f.midY)
            if best == nil || d < best!.1 { best = (spec.control, d) }
        }
        return best?.0
    }

    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        guard !isHidden, alpha > 0.01, isUserInteractionEnabled else { return nil }
        if mode == .edit { return bounds.contains(point) ? self : nil }
        return control(at: point) != nil ? self : nil
    }

    // MARK: play

    private struct Hold { let control: PadControl; var point: CGPoint; let click: Bool }
    private var holds: [ObjectIdentifier: Hold] = [:]
    private var lastStickTap: [PadControl: (end: TimeInterval, short: Bool)] = [:]
    private var touchStart: [ObjectIdentifier: TimeInterval] = [:]

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if mode == .edit { editBegan(touches); return }
        let now = ProcessInfo.processInfo.systemUptime
        for t in touches {
            let p = t.location(in: self)
            guard let c = control(at: p) else { continue }
            // A double tap that stays down on a stick clicks it (L3 / R3) while held.
            var click = false
            if c == .leftStick || c == .rightStick, let last = lastStickTap[c], last.short, now - last.end < 0.3 { click = true }
            holds[ObjectIdentifier(t)] = Hold(control: c, point: p, click: click)
            touchStart[ObjectIdentifier(t)] = now
        }
        apply()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        if mode == .edit { editMoved(touches); return }
        for t in touches {
            guard var h = holds[ObjectIdentifier(t)] else { continue }
            h.point = t.location(in: self)
            holds[ObjectIdentifier(t)] = h
        }
        apply()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if mode == .edit { editEnded(); return }
        let now = ProcessInfo.processInfo.systemUptime
        for t in touches {
            let id = ObjectIdentifier(t)
            if let h = holds[id], h.control == .leftStick || h.control == .rightStick {
                lastStickTap[h.control] = (now, now - (touchStart[id] ?? 0) < 0.2 && !h.click)
            }
            holds[id] = nil
            touchStart[id] = nil
        }
        apply()
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        if mode == .edit { editEnded(); return }
        for t in touches { holds[ObjectIdentifier(t)] = nil; touchStart[ObjectIdentifier(t)] = nil }
        apply()
    }

    /// Everything up (the controller hides, the game menu opens, the app goes inactive).
    func releaseAll() {
        holds.removeAll()
        touchStart.removeAll()
        apply()
    }

    private func apply() {
        var s = PadState()
        var knobs: [PadControl: CGPoint] = [:]
        var pressed: [PadControl: UInt16] = [:]
        for h in holds.values {
            guard let spec = layout.spec(h.control) else { continue }
            let f = TouchLayout.frame(spec, in: safeRect)
            let dx = Double(h.point.x - f.midX), dy = Double(h.point.y - f.midY)
            switch h.control {
            case .leftStick, .rightStick:
                let v = PadMath.stick(dx: dx, dy: dy, radius: Double(f.width) * 0.42)
                if h.control == .leftStick {
                    s.lx = PadState.axis(v.x); s.ly = PadState.axis(v.y)
                    if h.click { s.buttons |= PadState.l3 }
                } else {
                    s.rx = PadState.axis(v.x); s.ry = PadState.axis(v.y)
                    if h.click { s.buttons |= PadState.r3 }
                }
                knobs[h.control] = CGPoint(x: v.x, y: -v.y)
            case .dpad:
                let bits = PadMath.dpad(dx: dx, dy: dy, radius: Double(f.width) / 2)
                s.buttons |= bits
                pressed[.dpad, default: 0] |= bits
            case .face:
                let bit = PadMath.face(dx: dx, dy: dy)
                s.buttons |= bit
                pressed[.face, default: 0] |= bit
            case .lt: s.lt = 255; pressed[.lt] = 1
            case .rt: s.rt = 255; pressed[.rt] = 1
            default:
                if let b = h.control.button { s.buttons |= b; pressed[h.control] = b }
            }
        }
        for (c, v) in views {
            let k = knobs[c] ?? .zero, p = pressed[c] ?? 0
            if v.knob != k || v.pressed != p { v.knob = k; v.pressed = p; v.setNeedsDisplay() }
        }
        let newPress = (s.buttons & ~state.buttons) != 0 || (s.lt > 0 && state.lt == 0) || (s.rt > 0 && state.rt == 0)
        if newPress, haptics { feedback.impactOccurred(intensity: 0.6) }
        guard s != state else { return }
        state = s
        onState?(s)
    }

    // MARK: edit

    private enum Drag { case move(PadControl, offset: CGPoint), resize(PadControl, startDistance: CGFloat, startScale: Double) }
    private var drag: Drag?
    private var dragTouch: UITouch?
    var isDragging: Bool { drag != nil }

    private func editBegan(_ touches: Set<UITouch>) {
        guard dragTouch == nil, let t = touches.first else { return }
        let p = t.location(in: self)
        // A corner handle of the selected control: resize.
        if let sel = selected, let spec = layout.spec(sel), !selection.isHidden {
            let f = selection.frame
            let corners = [CGPoint(x: f.minX, y: f.minY), CGPoint(x: f.maxX, y: f.minY), CGPoint(x: f.minX, y: f.maxY), CGPoint(x: f.maxX, y: f.maxY)]
            if corners.contains(where: { hypot($0.x - p.x, $0.y - p.y) < 26 }) {
                let center = CGPoint(x: f.midX, y: f.midY)
                drag = .resize(sel, startDistance: max(hypot(p.x - center.x, p.y - center.y), 10), startScale: spec.scale)
                dragTouch = t
                return
            }
        }
        if let c = control(at: p), let spec = layout.spec(c) {
            let f = TouchLayout.frame(spec, in: safeRect)
            selected = c
            onSelect?(c)
            drag = .move(c, offset: CGPoint(x: p.x - f.midX, y: p.y - f.midY))
            dragTouch = t
            feedback.impactOccurred(intensity: 0.4)
        } else {
            selected = nil
            onSelect?(nil)
        }
    }

    private func editMoved(_ touches: Set<UITouch>) {
        guard let t = dragTouch, touches.contains(t), let drag else { return }
        let p = t.location(in: self), safe = safeRect
        switch drag {
        case .move(let c, let offset):
            layout.update(c) { s in
                s.x = Double((p.x - offset.x - safe.minX) / max(safe.width, 1))
                s.y = Double((p.y - offset.y - safe.minY) / max(safe.height, 1))
            }
        case .resize(let c, let start, let scale):
            guard let spec = layout.spec(c) else { return }
            let f = TouchLayout.frame(spec, in: safe)
            let d = hypot(p.x - f.midX, p.y - f.midY)
            layout.update(c) { s in s.scale = scale * Double(d / start) }
        }
        setNeedsLayout()
    }

    private func editEnded() {
        guard drag != nil else { return }
        drag = nil
        dragTouch = nil
        onLayoutChanged?(layout)
    }
}

/// One drawn control.
final class PadControlView: UIView {
    let control: PadControl
    var pressed: UInt16 = 0
    var knob = CGPoint.zero          // stick deflection, -1...1, y down
    var selectedLook = false

    init(control: PadControl) {
        self.control = control
        super.init(frame: .zero)
        isOpaque = false
        backgroundColor = .clear
        contentMode = .redraw
    }
    required init?(coder: NSCoder) { fatalError() }

    private static let fill = UIColor(white: 0.08, alpha: 0.42)
    private static let fillPressed = UIColor(white: 1, alpha: 0.38)
    private static let stroke = UIColor(white: 1, alpha: 0.55)
    private static let glow = UIColor(red: 0.30, green: 0.58, blue: 1, alpha: 1)

    override func draw(_ rect: CGRect) {
        let b = bounds.insetBy(dx: 2, dy: 2)
        switch control {
        case .leftStick, .rightStick: drawStick(b)
        case .dpad: drawDpad(b)
        case .face: drawFace(b)
        case .lb, .rb, .lt, .rt: drawShoulder(b)
        case .view, .menu: drawRound(b)
        }
    }

    private func stroked(_ path: UIBezierPath, fill: UIColor, width: CGFloat = 1.5) {
        fill.setFill(); path.fill()
        Self.stroke.setStroke(); path.lineWidth = width; path.stroke()
    }

    private func drawStick(_ b: CGRect) {
        let base = UIBezierPath(ovalIn: b)
        stroked(base, fill: Self.fill, width: selectedLook ? 2.5 : 1.5)
        if selectedLook {
            Self.glow.withAlphaComponent(0.9).setStroke()
            let ring = UIBezierPath(ovalIn: b.insetBy(dx: 3, dy: 3)); ring.lineWidth = 3; ring.stroke()
        }
        let r = b.width * 0.27, travel = b.width / 2 - r - 1   // the knob stays inside the base (drawing stops at the view's edge)
        let c = CGPoint(x: b.midX + knob.x * travel, y: b.midY + knob.y * travel)
        let k = UIBezierPath(ovalIn: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r))
        stroked(k, fill: knob == .zero ? UIColor(white: 0.55, alpha: 0.55) : UIColor(white: 0.85, alpha: 0.7))
    }

    private func drawDpad(_ b: CGRect) {
        let arm = b.width / 3
        let cross = UIBezierPath()
        cross.append(UIBezierPath(roundedRect: CGRect(x: b.minX + arm, y: b.minY, width: arm, height: b.height), cornerRadius: arm * 0.22))
        cross.append(UIBezierPath(roundedRect: CGRect(x: b.minX, y: b.minY + arm, width: b.width, height: arm), cornerRadius: arm * 0.22))
        cross.usesEvenOddFillRule = false
        Self.fill.setFill(); cross.fill()
        // Pressed arms.
        let arms: [(UInt16, CGRect)] = [
            (PadState.up, CGRect(x: b.minX + arm, y: b.minY, width: arm, height: arm)),
            (PadState.down, CGRect(x: b.minX + arm, y: b.maxY - arm, width: arm, height: arm)),
            (PadState.left, CGRect(x: b.minX, y: b.minY + arm, width: arm, height: arm)),
            (PadState.right, CGRect(x: b.maxX - arm, y: b.minY + arm, width: arm, height: arm)),
        ]
        for (bit, r) in arms where pressed & bit != 0 {
            Self.fillPressed.setFill(); UIBezierPath(roundedRect: r, cornerRadius: arm * 0.22).fill()
        }
        // The outline of the cross.
        let outline = UIBezierPath()
        let x0 = b.minX, x1 = b.minX + arm, x2 = b.maxX - arm, x3 = b.maxX
        let y0 = b.minY, y1 = b.minY + arm, y2 = b.maxY - arm, y3 = b.maxY
        for (i, p) in [CGPoint(x: x1, y: y0), CGPoint(x: x2, y: y0), CGPoint(x: x2, y: y1), CGPoint(x: x3, y: y1), CGPoint(x: x3, y: y2),
                       CGPoint(x: x2, y: y2), CGPoint(x: x2, y: y3), CGPoint(x: x1, y: y3), CGPoint(x: x1, y: y2), CGPoint(x: x0, y: y2),
                       CGPoint(x: x0, y: y1), CGPoint(x: x1, y: y1)].enumerated() {
            if i == 0 { outline.move(to: p) } else { outline.addLine(to: p) }
        }
        outline.close()
        outline.lineJoinStyle = .round
        Self.stroke.setStroke(); outline.lineWidth = 1.5; outline.stroke()
        // Arrows.
        UIColor(white: 1, alpha: 0.85).setFill()
        let s = arm * 0.2
        func tri(_ c: CGPoint, _ dir: CGFloat) {
            let p = UIBezierPath()
            for i in 0..<3 {
                let a = dir + CGFloat(i) * 2 * .pi / 3
                let q = CGPoint(x: c.x + cos(a) * s, y: c.y - sin(a) * s)
                if i == 0 { p.move(to: q) } else { p.addLine(to: q) }
            }
            p.close(); p.fill()
        }
        tri(CGPoint(x: b.midX, y: b.minY + arm / 2), .pi / 2)
        tri(CGPoint(x: b.midX, y: b.maxY - arm / 2), -.pi / 2)
        tri(CGPoint(x: b.minX + arm / 2, y: b.midY), .pi)
        tri(CGPoint(x: b.maxX - arm / 2, y: b.midY), 0)
    }

    private func drawFace(_ b: CGRect) {
        let d = b.width * 0.38
        let buttons: [(String, UInt16, CGPoint, UIColor)] = [
            ("Y", PadState.y, CGPoint(x: b.midX, y: b.minY + d / 2), UIColor(red: 0.98, green: 0.80, blue: 0.20, alpha: 1)),
            ("X", PadState.x, CGPoint(x: b.minX + d / 2, y: b.midY), UIColor(red: 0.30, green: 0.56, blue: 1.0, alpha: 1)),
            ("B", PadState.b, CGPoint(x: b.maxX - d / 2, y: b.midY), UIColor(red: 0.95, green: 0.30, blue: 0.30, alpha: 1)),
            ("A", PadState.a, CGPoint(x: b.midX, y: b.maxY - d / 2), UIColor(red: 0.35, green: 0.82, blue: 0.40, alpha: 1)),
        ]
        for (label, bit, c, color) in buttons {
            let r = CGRect(x: c.x - d / 2, y: c.y - d / 2, width: d, height: d)
            let on = pressed & bit != 0
            stroked(UIBezierPath(ovalIn: r), fill: on ? color.withAlphaComponent(0.55) : Self.fill)
            drawLabel(label, in: r, size: d * 0.5, color: on ? .white : color, weight: .heavy)
        }
    }

    private func drawShoulder(_ b: CGRect) {
        let on = pressed != 0
        let path = UIBezierPath(roundedRect: b, cornerRadius: min(b.height * 0.45, 22))
        stroked(path, fill: on ? Self.fillPressed : Self.fill)
        let text: String
        switch control {
        case .lb: text = "L"
        case .rb: text = "R"
        case .lt: text = "LT"
        default: text = "RT"
        }
        drawLabel(text, in: b, size: b.height * 0.42, color: .white, weight: .semibold)
    }

    private func drawRound(_ b: CGRect) {
        let on = pressed != 0
        stroked(UIBezierPath(ovalIn: b), fill: on ? Self.fillPressed : Self.fill)
        let name = control == .menu ? "line.3.horizontal" : "rectangle.on.rectangle"
        let cfg = UIImage.SymbolConfiguration(pointSize: b.width * 0.36, weight: .semibold)
        if let img = UIImage(systemName: name, withConfiguration: cfg)?.withTintColor(.white, renderingMode: .alwaysOriginal) {
            img.draw(at: CGPoint(x: b.midX - img.size.width / 2, y: b.midY - img.size.height / 2))
        }
    }

    private func drawLabel(_ text: String, in r: CGRect, size: CGFloat, color: UIColor, weight: UIFont.Weight) {
        let attrs: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: size, weight: weight), .foregroundColor: color]
        let s = (text as NSString).size(withAttributes: attrs)
        (text as NSString).draw(at: CGPoint(x: r.midX - s.width / 2, y: r.midY - s.height / 2), withAttributes: attrs)
    }
}

/// The editor's selection: a thin frame with round handles at its corners (mockup 3).
final class SelectionFrameView: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        isOpaque = false
        backgroundColor = .clear
        contentMode = .redraw
    }
    required init?(coder: NSCoder) { fatalError() }

    override func draw(_ rect: CGRect) {
        let r = bounds.insetBy(dx: 6, dy: 6)
        UIColor.white.setStroke()
        let frame = UIBezierPath(rect: r); frame.lineWidth = 1.5; frame.stroke()
        UIColor.white.setFill()
        for p in [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.maxX, y: r.maxY)] {
            UIBezierPath(ovalIn: CGRect(x: p.x - 6, y: p.y - 6, width: 12, height: 12)).fill()
        }
    }
}

// MARK: - layouts on disk

/// Layouts by game ("steam.<App ID>" / "custom.<UUID>") and the default, per orientation.
enum TouchLayoutStore {
    static let defaultScope = "default"

    private static func key(_ scope: String, _ landscape: Bool) -> String {
        "touchLayout.\(scope).\(landscape ? "landscape" : "portrait")"
    }

    static func stored(_ scope: String, landscape: Bool) -> TouchLayout? {
        guard let data = UserDefaults.standard.data(forKey: key(scope, landscape)),
              let l = try? JSONDecoder().decode(TouchLayout.self, from: data) else { return nil }
        return l.completed(landscape: landscape)
    }

    /// The default layout (the user's, else MYIOSDECK's).
    static func defaultLayout(landscape: Bool) -> TouchLayout {
        stored(defaultScope, landscape: landscape) ?? .standard(landscape: landscape)
    }

    /// A game's layout: its own, else the default.
    static func layout(for game: String?, landscape: Bool) -> TouchLayout {
        if let game, !game.isEmpty, let own = stored(game, landscape: landscape) { return own }
        return defaultLayout(landscape: landscape)
    }

    static func save(_ l: TouchLayout, scope: String, landscape: Bool) {
        if let data = try? JSONEncoder().encode(l) { UserDefaults.standard.set(data, forKey: key(scope, landscape)) }
        dlog("[touch-pad] layout saved for \(scope) (\(landscape ? "landscape" : "portrait"))")
    }

    static func hasOwnLayout(_ game: String) -> Bool {
        stored(game, landscape: true) != nil || stored(game, landscape: false) != nil
    }

    static func removeOwnLayout(_ game: String) {
        for l in [true, false] { UserDefaults.standard.removeObject(forKey: key(game, l)) }
    }

    static func resetDefault() { removeOwnLayout(defaultScope) }
}

// MARK: - in game

/// The on-screen controller over the running game: a TouchControllerView in the window above the
/// game view (the game view is a raw UIView above SwiftUI, so this has to be one too). Shown while
/// the game view is (not behind the game menu), as the game's mode says; its state is player 1's
/// pad through ControllerBridge, combined with a physical controller there.
@MainActor
final class TouchPadOverlay: ObservableObject {
    static let shared = TouchPadOverlay()

    @Published private(set) var mode: TouchPadMode = .auto
    @Published private(set) var visible = false
    /// The layout editor is open over the game.
    @Published var editing = false {
        didSet { if editing != oldValue { refresh() } }
    }

    private var gameKey = ""
    private var active = false
    private var hostHidden = true
    let view = TouchControllerView(mode: .play)

    private init() {
        view.onState = { ControllerBridge.shared.setTouchPad(connected: true, $0) }
        view.isHidden = true
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            }
        }
        center.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.view.releaseAll() }
        }
    }

    /// The game view appeared for a game (its settings' mode, else the app default).
    func sessionShown(gameKey: String, mode: TouchPadMode?) {
        if !active || gameKey != self.gameKey {
            self.gameKey = gameKey
            self.mode = mode ?? GameDefaults.current.touchPad
            reloadLayout()
        }
        active = true
        view.haptics = UserDefaults.standard.bool(forKey: "touchPadHaptics")
        attach()
        refresh()
    }

    /// The game ended: nothing shows, player 1's on-screen pad is gone.
    func sessionEnded() {
        active = false
        refresh()
        view.removeFromSuperview()
    }

    /// GameHostView shown or hidden (the game menu, Hide game, the editor).
    func hostVisibility(hidden: Bool) {
        hostHidden = hidden
        if !hidden { attach() }
        refresh()
    }

    func setMode(_ m: TouchPadMode, remember: Bool) {
        mode = m
        if remember, !gameKey.isEmpty {
            var s = GameSettingsStore.load(gameKey)
            if s.useDefaults { s = GameSettings(); s.useDefaults = false }
            s.touchPad = m
            GameSettingsStore.save(s, for: gameKey)
        }
        dlog("[touch-pad] \(m.rawValue) for \(gameKey.isEmpty ? "this program" : gameKey)")
        refresh()
    }

    func reloadLayout() {
        let key = gameKey
        view.layoutProvider = { landscape in TouchLayoutStore.layout(for: key, landscape: landscape) }
        view.reloadFromProvider()
    }

    private func attach() {
        guard let w = GameHostView.shared.window else { return }
        if view.superview !== w {
            view.removeFromSuperview()
            w.addSubview(view)
        }
        view.frame = w.bounds
        view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        w.bringSubviewToFront(view)
    }

    private func refresh() {
        let physical = GCController.controllers().contains { $0.extendedGamepad != nil }
        let show = active && !hostHidden && !editing && (mode == .always || (mode == .auto && !physical))
        if !show { view.releaseAll() }
        view.isHidden = !show
        if show != visible {
            visible = show
            dlog("[touch-pad] \(show ? "shown" : "hidden") (mode \(mode.rawValue), controller \(physical ? "connected" : "none"))")
        }
        ControllerBridge.shared.setTouchPad(connected: show, view.state)
    }
}
