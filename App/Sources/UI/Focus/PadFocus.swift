// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit
import UIKit.UIGestureRecognizerSubclass
import GameController

/// Controller navigation of the app's UI (iOS gives SwiftUI no controller-driven focus on iPhone):
/// views mark themselves `.padFocus(...)` and report their frames; the D-pad or left stick moves a
/// glass focus ring between them (FocusMath), A activates, B goes back or closes, LB/RB switch tabs,
/// Y searches, X is the focused item's second action, Menu opens Settings or the screen's own menu.
/// Only the top scope's items take part (a sheet, a dialog, the editor over the rest). While a game
/// is on screen the controller is the game's (ControllerBridge) and only the menu chord is watched.
@MainActor
final class PadFocus: ObservableObject {
    static let shared = PadFocus()

    /// The focused item, and whether its ring shows (controller in use, not after a touch).
    @Published private(set) var focusedID: String?
    @Published private(set) var ringVisible = false

    /// An item's live actions (updated by its view on every update, so they never go stale).
    final class Actions {
        var activate: () -> Void = {}
        /// Left / right change the value; false at the first / last value (focus then moves on).
        var adjust: ((Int) -> Bool)?
        var secondary: (() -> Void)?
        var onFocus: (() -> Void)?
        var preferred = false
    }

    private struct Item {
        var scope: String
        var section: String
        var frame: CGRect
        let actions: Actions
    }
    private var items: [String: Item] = [:]

    /// Open scopes, the last on top, each with what B does there.
    private var scopes: [(name: String, token: UUID, back: (() -> Void)?)] = []
    var topScope: String { scopes.last?.name ?? "root" }

    /// The Menu button's action on the current screen (Home: the game's "…"), the last set wins.
    private var menuHandlers: [(token: UUID, run: () -> Void)] = []

    // MARK: registration (PadFocusModifier)

    func register(_ id: String, scope: String, section: String, frame: CGRect, actions: Actions) {
        items[id] = Item(scope: scope, section: section, frame: frame, actions: actions)
    }

    func unregister(_ id: String, actions: Actions) {
        guard items[id]?.actions === actions else { return }
        items[id] = nil
        if focusedID == id {
            focusedID = nil
            // The page changed under the focus (a push, a tab): focus moves to the new page.
            if ringVisible { DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.ensureFocus() } }
        }
    }

    func pushScope(_ name: String, token: UUID, back: (() -> Void)?) {
        scopes.removeAll { $0.token == token }
        scopes.append((name, token, back))
        focusedID = nil
        if ringVisible { DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in self?.ensureFocus() } }
    }

    func popScope(token: UUID) {
        let wasTop = scopes.last?.token == token
        scopes.removeAll { $0.token == token }
        if wasTop {
            focusedID = nil
            if ringVisible { DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.ensureFocus() } }
        }
    }

    func setMenuHandler(_ token: UUID, _ run: @escaping () -> Void) {
        menuHandlers.removeAll { $0.token == token }
        menuHandlers.append((token, run))
    }
    func removeMenuHandler(_ token: UUID) { menuHandlers.removeAll { $0.token == token } }

    // MARK: focus

    /// The items that can take focus: the top scope's, on screen across, and within two screen
    /// heights up and down (lists scroll to them). Items of pages pushed under the current one are
    /// off to the side, so they are left out.
    private var candidates: [(id: String, item: Item)] {
        let screen = UIScreen.main.bounds
        let reach = screen.insetBy(dx: 0, dy: -2 * screen.height)
        return items.filter { $0.value.scope == topScope && $0.value.frame.width > 0 && reach.intersects($0.value.frame)
                && $0.value.frame.maxX > screen.minX + 2 && $0.value.frame.minX < screen.maxX - 2 }
            .map { (id: $0.key, item: $0.value) }
    }

    private func ensureFocus() {
        if let id = focusedID, let it = items[id], it.scope == topScope { return }
        let list = candidates
        let onScreen = list.filter { UIScreen.main.bounds.intersects($0.item.frame) }
        let pool = onScreen.isEmpty ? list : onScreen
        if let id = FocusMath.first(pool.map { (id: $0.id, frame: $0.item.frame, preferred: $0.item.actions.preferred) }) { focus(id) }
    }

    func focus(_ id: String) {
        guard focusedID != id else { return }
        focusedID = id
        if let f = items[id]?.frame { ScrollIntoView.reveal(f) }
        items[id]?.actions.onFocus?()
    }

    private func move(_ d: PadDirection) {
        guard let id = focusedID, let current = items[id], current.scope == topScope else { ensureFocus(); return }
        if let a = current.actions.adjust, d == .left || d == .right, a(d == .left ? -1 : 1) {
            UISelectionFeedbackGenerator().selectionChanged()
            return
        }
        let list = candidates.filter { $0.id != id }.map {
            FocusCandidate(id: $0.id, frame: $0.item.frame, section: $0.item.section, preferred: $0.item.actions.preferred)
        }
        if let next = FocusMath.next(from: current.frame, section: current.section, direction: d, candidates: list) {
            focus(next)
            UISelectionFeedbackGenerator().selectionChanged()
        } else {
            ScrollIntoView.reveal(current.frame)   // at an edge: at least show all of the focused item
        }
    }

    /// A touch: the ring hides until the controller is used again.
    func touched() {
        if ringVisible { ringVisible = false }
    }

    // MARK: input (PadUIInput)

    func handle(_ key: PadKey) {
        let wasVisible = ringVisible
        if !ringVisible { withGlassAnimation { ringVisible = true } }
        if let d = key.direction {
            if !wasVisible, let id = focusedID, items[id]?.scope == topScope {   // first press shows where focus is
                if let f = items[id]?.frame { ScrollIntoView.reveal(f) }
                return
            }
            move(d)
            return
        }
        switch key {
        case .a:
            guard let id = focusedID, let it = items[id], it.scope == topScope else { ensureFocus(); return }
            it.actions.activate()
        case .b:
            back()
        case .x:
            if let id = focusedID, let s = items[id]?.actions.secondary, items[id]?.scope == topScope { s() }
        case .y:
            if topScope == "root" { UIState.shared.showSearch = true }
        case .lb, .rb:
            cycle(key == .rb ? 1 : -1)
        case .menu:
            if topScope == "root" {
                if let h = menuHandlers.last { h.run() } else { UIState.shared.show(.settings) }
            }
        default: break
        }
    }

    private func back() {
        if let top = scopes.last {
            top.back?()
            return
        }
        let ui = UIState.shared
        if !ui.path.isEmpty { ui.back() }
        else if ui.tab != .home { ui.show(.home) }
    }

    /// LB / RB: the top bar's destinations in their order on screen (TopDestination), from the app
    /// itself or from the Search and Controllers panels; LB is the exact reverse of RB.
    private func cycle(_ step: Int) {
        let ui = UIState.shared
        let current: TopDestination
        switch topScope {
        case "search": current = .search
        case "controllers": current = .controllers
        case "root":
            switch ui.tab {
            case .home: current = .home
            case .library: current = .library
            case .downloads: current = .downloads
            case .settings: current = .settings
            }
        default: return   // a dialog, the editor, a game's sheet: LB/RB do nothing there
        }
        let next = current.next(step)
        let sheetUp = ui.showSearch || ui.showControllers
        ui.showSearch = false
        ui.showControllers = false
        focusedID = nil
        DispatchQueue.main.asyncAfter(deadline: .now() + (sheetUp ? 0.45 : 0)) { [weak self] in
            switch next {
            case .home: ui.show(.home)
            case .library: ui.show(.library)
            case .downloads: ui.show(.downloads)
            case .settings: ui.show(.settings)
            case .search: ui.showSearch = true
            case .controllers: ui.showControllers = true
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { self?.ensureFocus() }
        }
    }

    func isFocused(_ id: String) -> Bool { ringVisible && focusedID == id }

    /// What the hint bar says about the focused item.
    var focusedAdjustable: Bool { focusedID.flatMap { items[$0]?.actions.adjust } != nil }
    var focusedHasSecondary: Bool { focusedID.flatMap { items[$0]?.actions.secondary } != nil }
    var inDialog: Bool { topScope.hasPrefix("dialog.") }
}

/// Brings an item into view by scrolling the UIKit scroll views that hold it (a page, a sidebar, a
/// row of cards), innermost first. Works from its frame alone, so it needs nothing from SwiftUI's
/// scroll views (ScrollViewReader did not scroll them reliably, build 193).
@MainActor
enum ScrollIntoView {
    static func reveal(_ frame: CGRect) {
        guard frame.width > 0, frame.height > 0 else { return }
        // A dialog's own window while one shows, else the app's.
        let windows = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap(\.windows)
        guard let window = (Dialogs.shared.showing ? Dialogs.shared.overlayWindow : nil)
                ?? windows.first(where: { $0.isKeyWindow }) ?? windows.first else { return }
        let centre = CGPoint(x: frame.midX, y: frame.midY)
        var chain: [UIScrollView] = []
        func visit(_ v: UIView) {
            if v.isHidden || v.alpha < 0.01 { return }
            if let s = v as? UIScrollView, s.isScrollEnabled {
                let onScreen = s.convert(s.bounds, to: nil)
                let vertical = s.contentSize.height > s.bounds.height + 1
                let horizontal = s.contentSize.width > s.bounds.width + 1
                let inContent = CGRect(origin: .zero, size: s.contentSize).insetBy(dx: -1, dy: -1).contains(s.convert(centre, from: nil))
                // It scrolls the way the item is out of view, and the item lies across it.
                let across = (vertical && centre.x >= onScreen.minX && centre.x <= onScreen.maxX)
                    || (horizontal && centre.y >= onScreen.minY && centre.y <= onScreen.maxY)
                if (vertical || horizontal), inContent, across { chain.append(s) }
            }
            for sub in v.subviews { visit(sub) }
        }
        visit(window)
        for s in chain.reversed() {
            let r = s.convert(frame, from: nil).insetBy(dx: -14, dy: -18)
            s.scrollRectToVisible(r, animated: Appearance.shared.animations)
        }
    }
}

// MARK: - scope and item modifiers

struct PadScopeKey: EnvironmentKey { static let defaultValue = "root" }
struct PadSectionKey: EnvironmentKey { static let defaultValue = "" }
extension EnvironmentValues {
    var padScope: String {
        get { self[PadScopeKey.self] }
        set { self[PadScopeKey.self] = newValue }
    }
    /// The part of the screen an item belongs to (FocusMath: a sidebar, a panel, the top bar).
    var padSection: String {
        get { self[PadSectionKey.self] }
        set { self[PadSectionKey.self] = newValue }
    }
}

/// A sheet, dialog or full-screen page whose items alone take focus while it is up; B runs `back`.
private struct PadScopeModifier: ViewModifier {
    let name: String
    let back: (() -> Void)?
    @State private var token = UUID()

    func body(content: Content) -> some View {
        content
            .environment(\.padScope, name)
            .onAppear { PadFocus.shared.pushScope(name, token: token, back: back) }
            .onDisappear { PadFocus.shared.popScope(token: token) }
    }
}

/// One focusable item: registers its frame and actions, and draws the ring when focused.
private struct PadFocusModifier: ViewModifier {
    let id: String
    let radius: CGFloat
    let preferred: Bool
    let activate: () -> Void
    let adjust: ((Int) -> Bool)?
    let secondary: (() -> Void)?
    let onFocus: (() -> Void)?
    @Environment(\.padScope) private var scope
    @Environment(\.padSection) private var section
    @ObservedObject private var focus = PadFocus.shared
    @State private var actions = PadFocus.Actions()
    @State private var frame = CGRect.zero

    func body(content: Content) -> some View {
        // Fresh closures on every update (the box is registered once).
        let _ = {
            actions.activate = activate
            actions.adjust = adjust
            actions.secondary = secondary
            actions.onFocus = onFocus
            actions.preferred = preferred
        }()
        let on = focus.isFocused(id)
        return content
            .overlay {
                if on {
                    RoundedRectangle(cornerRadius: radius + 4, style: .continuous)
                        .inset(by: -5)
                        .strokeBorder(Color.white, lineWidth: 2.5)
                        .shadow(color: Glass.primary.opacity(0.9), radius: 8)
                        .shadow(color: Glass.primary.opacity(0.6), radius: 16)
                        .allowsHitTesting(false)
                        .transition(.opacity)
                }
            }
            .scaleEffect(on ? 1.03 : 1)
            .animation(Appearance.shared.animation(.easeOut(duration: 0.14)), value: on)
            .zIndex(on ? 1 : 0)
            .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { f in
                frame = f
                PadFocus.shared.register(id, scope: scope, section: section, frame: f, actions: actions)
            }
            .background {
                // The first frame, in case the geometry callback only reports changes.
                GeometryReader { g in
                    Color.clear.onAppear {
                        let f = g.frame(in: .global)
                        if frame == .zero, f.width > 0 {
                            frame = f
                            PadFocus.shared.register(id, scope: scope, section: section, frame: f, actions: actions)
                        }
                    }
                }
            }
            .onAppear { if frame != .zero { PadFocus.shared.register(id, scope: scope, section: section, frame: frame, actions: actions) } }
            .onDisappear { PadFocus.shared.unregister(id, actions: actions) }
            .id(id)
    }
}

/// The Menu button's action while this view is on screen.
private struct PadMenuModifier: ViewModifier {
    let run: () -> Void
    @State private var token = UUID()
    func body(content: Content) -> some View {
        content
            .onAppear { PadFocus.shared.setMenuHandler(token, run) }
            .onDisappear { PadFocus.shared.removeMenuHandler(token) }
    }
}

extension View {
    /// A controller-focusable item. `adjust` takes left/right (-1 / +1) instead of moving focus
    /// (segmented choices, sliders); `secondary` is X; `onFocus` runs when focus arrives.
    func padFocus(_ id: String, radius: CGFloat = 14, preferred: Bool = false, adjust: ((Int) -> Bool)? = nil,
                  secondary: (() -> Void)? = nil, onFocus: (() -> Void)? = nil, activate: @escaping () -> Void) -> some View {
        modifier(PadFocusModifier(id: id, radius: radius, preferred: preferred, activate: activate, adjust: adjust,
                                  secondary: secondary, onFocus: onFocus))
    }

    /// A toggle: A flips it.
    func padToggle(_ id: String, isOn: Binding<Bool>, radius: CGFloat = 12) -> some View {
        padFocus(id, radius: radius) { isOn.wrappedValue.toggle() }
    }

    /// A choice among options: left/right step through them, A takes the next one.
    func padChoice<T: Hashable>(_ id: String, options: [T], selection: Binding<T>, radius: CGFloat = 22) -> some View {
        func step(_ d: Int) -> Bool {
            let i = options.firstIndex(of: selection.wrappedValue) ?? 0
            guard let n = ChoiceStep.index(i, count: options.count, delta: d) else { return false }
            withGlassAnimation { selection.wrappedValue = options[n] }
            return true
        }
        return padFocus(id, radius: radius, adjust: step) {
            guard !options.isEmpty else { return }
            let i = options.firstIndex(of: selection.wrappedValue) ?? 0
            withGlassAnimation { selection.wrappedValue = options[(i + 1) % options.count] }
        }
    }

    /// A slider: left/right move it by `step`.
    func padSlider(_ id: String, value: Binding<Double>, range: ClosedRange<Double>, step: Double, radius: CGFloat = 12) -> some View {
        padFocus(id, radius: radius, adjust: { d in
            let v = min(max(value.wrappedValue + Double(d) * step, range.lowerBound), range.upperBound)
            guard abs(v - value.wrappedValue) > 1e-9 else { return false }   // at an end: focus moves on
            value.wrappedValue = v
            return true
        }) {}
    }

    /// Its own focus scope (a sheet, dialog, full-screen page); B runs `back`.
    func padScope(_ name: String, back: (() -> Void)? = nil) -> some View {
        modifier(PadScopeModifier(name: name, back: back))
    }

    /// The part of the screen its items belong to (a sidebar, a panel, the top bar).
    func padSection(_ name: String) -> some View { environment(\.padSection, name) }

    /// What the Menu button does while this view shows.
    func padMenu(_ run: @escaping () -> Void) -> some View { modifier(PadMenuModifier(run: run)) }
}

/// A ScrollView for focusable content (PadFocus scrolls the focused item into view with UIKit:
/// ScrollIntoView).
struct PadScrollView<Content: View>: View {
    var axes: Axis.Set = .vertical
    @ViewBuilder var content: Content

    /// Room for the focus ring and its glow: the scroll view reaches this far past its place and
    /// its content starts as far in, so the ring of an item at an edge is not clipped.
    static var ringRoom: CGFloat { 12 }

    var body: some View {
        ScrollView(axes) { content.padding(Self.ringRoom) }
            .padding(-Self.ringRoom)
    }
}

// MARK: - controller input

/// Reads every connected controller about 60 times a second while the app is active. With the
/// UI in front the presses go to PadFocus and the game gets a neutral pad (ControllerBridge); with
/// a game on screen only the game menu's chord is watched: Home / guide, or View + Menu held 0.6 s.
@MainActor
final class PadUIInput {
    static let shared = PadUIInput()

    private var timer: Timer?
    private var stick = StickDirections()
    private var repeater = PadRepeater()
    private var chord = MenuChord()
    private var homeWasDown = false
    private var started = false

    func start() {
        guard !started else { return }
        started = true
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect,
                     UIApplication.didBecomeActiveNotification, UIApplication.willResignActiveNotification] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.update() }
            }
        }
        update()
        TouchWatcher.install()
    }

    private func update() {
        let needed = !GCController.controllers().filter { $0.extendedGamepad != nil }.isEmpty
            && UIApplication.shared.applicationState != .background
        if needed, timer == nil {
            let t = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.tick() }
            }
            RunLoop.main.add(t, forMode: .common)
            timer = t
            dlog("[pad-ui] controller drives the app's UI when no game is on screen")
        } else if !needed, let t = timer {
            t.invalidate()
            timer = nil
            ControllerBridge.shared.setUIOwnsPads(false)
        }
    }

    /// A game is on screen (its view shown, no dialog over it): the controller is the game's.
    private var gameInFront: Bool {
        let host = GameHostView.shared
        return host.window != nil && !host.isHidden && !Dialogs.shared.showing && !TouchPadOverlay.shared.editing
    }

    private func tick() {
        guard UIApplication.shared.applicationState == .active else { return }
        var held = Set<PadKey>()
        var home = false, sx = 0.0, sy = 0.0
        for c in GCController.controllers() {
            guard let p = c.extendedGamepad else { continue }
            if p.dpad.up.isPressed { held.insert(.up) }
            if p.dpad.down.isPressed { held.insert(.down) }
            if p.dpad.left.isPressed { held.insert(.left) }
            if p.dpad.right.isPressed { held.insert(.right) }
            if p.buttonA.isPressed { held.insert(.a) }
            if p.buttonB.isPressed { held.insert(.b) }
            if p.buttonX.isPressed { held.insert(.x) }
            if p.buttonY.isPressed { held.insert(.y) }
            if p.leftShoulder.isPressed { held.insert(.lb) }
            if p.rightShoulder.isPressed { held.insert(.rb) }
            if p.buttonMenu.isPressed { held.insert(.menu) }
            if p.buttonOptions?.isPressed == true { held.insert(.view) }
            if p.buttonHome?.isPressed == true { home = true }
            let x = Double(p.leftThumbstick.xAxis.value), y = Double(p.leftThumbstick.yAxis.value)
            if abs(x) + abs(y) > abs(sx) + abs(sy) { sx = x; sy = y }
        }
        let now = ProcessInfo.processInfo.systemUptime
        let homePressed = home && !homeWasDown
        homeWasDown = home
        if gameInFront {
            // Back to the game (Resume): the button that chose it is still down; the game gets the
            // controller once everything is released, so it never sees that press.
            if uiHadPads {
                if !held.isEmpty || abs(sx) + abs(sy) > 0.3 { return }
                uiHadPads = false
            }
            ControllerBridge.shared.setUIOwnsPads(false)
            // Reset so nothing held in the game fires in the UI when it takes over.
            _ = repeater.step(held: [], now: now)
            stick = StickDirections()
            if homePressed || chord.step(view: held.contains(.view), menu: held.contains(.menu), now: now) {
                dlog("[pad-ui] game menu from the controller (\(homePressed ? "Home button" : "View + Menu"))")
                GameHostView.onMenu?()
                swallowUntilReleased = true
            }
            return
        }
        ControllerBridge.shared.setUIOwnsPads(true)
        uiHadPads = true
        _ = chord.step(view: false, menu: false, now: now)
        if let d = stick.update(x: sx, y: sy) {
            switch d {
            case .up: held.insert(.up)
            case .down: held.insert(.down)
            case .left: held.insert(.left)
            case .right: held.insert(.right)
            }
        }
        if homePressed { held.insert(.home) }
        // The buttons of the chord that opened the menu are still down: they act only once released.
        if swallowUntilReleased {
            if held.isEmpty { swallowUntilReleased = false } else { _ = repeater.step(held: held, now: now); return }
        }
        for key in repeater.step(held: held, now: now) where key != .view && key != .home {
            PadFocus.shared.handle(key)
        }
    }
    private var swallowUntilReleased = false
    private var uiHadPads = false
}

/// Sees every touch without taking it (the focus ring hides when the screen is touched).
final class TouchWatcher: UIGestureRecognizer, UIGestureRecognizerDelegate {
    @MainActor static func install() {
        for scene in UIApplication.shared.connectedScenes {
            for w in (scene as? UIWindowScene)?.windows ?? [] { attach(to: w) }
        }
    }

    @MainActor static func attach(to window: UIWindow) {
        guard !(window.gestureRecognizers ?? []).contains(where: { $0 is TouchWatcher }) else { return }
        let g = TouchWatcher(target: nil, action: nil)
        g.cancelsTouchesInView = false
        g.delaysTouchesBegan = false
        g.delaysTouchesEnded = false
        g.delegate = g
        window.addGestureRecognizer(g)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        MainActor.assumeIsolated { PadFocus.shared.touched() }
        state = .failed
    }

    func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }
}

/// What the controller's buttons do here, shown while the focus ring is (bottom of the screen).
struct PadHintBar: View {
    @ObservedObject private var focus = PadFocus.shared
    /// The one in the dialogs' window shows while a dialog is up, the app's otherwise.
    var dialogWindow = false

    private var hints: [(String, String)] {
        if focus.inDialog { return [("A", "Choose"), ("B", "Cancel")] }
        var h: [(String, String)] = []
        if focus.focusedAdjustable { h.append(("◀ ▶", "Change · past the end: move on")) }
        h.append(("A", "Select"))
        h.append(("B", "Back"))
        if focus.focusedHasSecondary { h.append(("X", "Game settings")) }
        if focus.topScope == "root" { h.append(("LB RB", "Switch")); h.append(("Y", "Search")) }
        return h
    }

    var body: some View {
        if focus.ringVisible, focus.inDialog == dialogWindow {
            HStack(spacing: 14) {
                ForEach(hints, id: \.0) { key, text in
                    HStack(spacing: 5) {
                        Text(key).font(.caption2.weight(.bold))
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(Glass.text.opacity(0.85), in: Capsule())
                            .foregroundStyle(Glass.base)
                        Text(text).font(.caption).foregroundStyle(Glass.text)
                    }
                }
            }
            .padding(.horizontal, 14).padding(.vertical, 7)
            .glassCapsule()
            .padding(.bottom, 6)
            .allowsHitTesting(false)
            .transition(.opacity)
            .accessibilityHidden(true)
        }
    }
}
