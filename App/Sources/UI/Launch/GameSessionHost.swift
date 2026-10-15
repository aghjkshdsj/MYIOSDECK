// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The running game's full-screen view and its menu (Resume, Controller, Touch mouse, the
/// on-screen controller, Hide game, Quit game / Force quit, Quit and close MYIOSDECK). On the root
/// view, so a game started anywhere shows here, and a hidden game comes back from any screen.
struct GameSessionHost: ViewModifier {
    @EnvironmentObject private var wine: WineController
    @ObservedObject private var session = GameSession.shared
    @ObservedObject private var pad = TouchPadOverlay.shared

    @State private var showGameMenu = false
    @State private var showControllerMenu = false
    @State private var showTouchMenu = false
    @State private var showPadMenu = false
    @State private var openingSubmenu = false   // the game menu closes before a submenu opens
    @State private var quitStuck = false        // Quit game did not end the game: Force quit is offered
    @State private var quitAttempt = 0          // the latest Quit game / Force quit (its watchdog's)
    @State private var quitPending = false      // Quit game was asked and the game still runs

    func body(content: Content) -> some View {
        content
            .fullScreenCover(isPresented: $session.shown) { gameSurface }
            .onChange(of: wine.state) { _, state in
                // The session ended (the game's own Quit, a crash, or a start that failed): close the
                // surface and its menus and go back to the app, where the status shows (and a crash
                // report after it). A launcher that starts its game and exits is not an end: 32-bit
                // sessions wait for the game.
                switch state {
                case .finished, .failed:
                    dlog("[ui] session ended: game view closed")
                    showGameMenu = false; showControllerMenu = false; showTouchMenu = false; showPadMenu = false
                    session.shown = false
                    session.title = ""
                    GameHostView.shared.setStatus(nil)   // Quit game's "Closing…"
                    TouchPadOverlay.shared.sessionEnded()
                    quitStuck = false
                    quitPending = false
                    quitAttempt += 1
                default: break
                }
            }
    }

    /// Full-screen surface for a Windows program's Direct3D window, edge to edge.
    /// The menu button is drawn by GameHostView (the Metal host is above every
    /// SwiftUI view); while the menu is open the host is hidden so it shows.
    private var gameSurface: some View {
        GameSurfaceView()
            .ignoresSafeArea()
            .background(Color.black)
            .statusBarHidden()
            .persistentSystemOverlays(.hidden)
            .onAppear {
                GameHostView.onMenu = {
                    dlog("[ui] game menu opening (surface shown \(session.shown), menu already open \(showGameMenu))")
                    // The menu is opened when something is wrong (a game that hangs while starting
                    // or quitting, build 162): where every thread of the 32-bit programs is, each time.
                    if wine.sessionKept, case .running = wine.state { mid_wow_thread_report("game menu opened") }
                    GameHostView.shared.isHidden = true
                    showGameMenu = true
                }
                TouchPadOverlay.shared.sessionShown(gameKey: session.gameID, mode: wine.sessionOverrides?.touchPad)
            }
            .confirmationDialog(session.title, isPresented: $showGameMenu, titleVisibility: .visible) {
                Button("Resume") {}
                Button("Controller: \(Self.controllerNames[wine.currentControllerAPI] ?? "XInput")…") {
                    submenu { showControllerMenu = true }
                }
                Button("Touch mouse: \(Self.touchNames[GameInput.shared.touchMode] ?? "Off")…") {
                    submenu { showTouchMenu = true }
                }
                Button("On-screen controller: \(pad.visible ? "shown" : "hidden")…") {
                    submenu { showPadMenu = true }
                }
                Button("Hide game (keeps running)") { session.shown = false }
                if wine.sessionKept {
                    Button("Quit game") { requestQuit() }
                    if quitStuck {
                        Button("Force quit", role: .destructive) { forceQuit() }
                    }
                }
                Button("Quit game and close MYIOSDECK", role: .destructive) { CrashReporter.shared.quitApp() }
            } message: {
                if wine.sessionKept && quitStuck {
                    Text("\(session.title) has not closed. Force quit ends it at once (unsaved progress is lost) and MYIOSDECK stays open for the next game.")
                } else if wine.sessionKept {
                    Text("Quit game asks the game to close, as its window's close button does: confirm in the game if it asks. A game still running 10 s later is force quit (unsaved progress is lost). MYIOSDECK stays open for the next game. Quit game and close MYIOSDECK stops it at once.")
                } else {
                    Text("A Windows game can only be stopped by closing MYIOSDECK; unsaved progress is lost.")
                }
            }
            .confirmationDialog("Controller", isPresented: $showControllerMenu, titleVisibility: .visible) {
                ForEach(Self.controllerOrder, id: \.self) { api in
                    let current = api == wine.currentControllerAPI
                    let later = WineController.sessionStartAPIs.contains(api) && api != wine.sessionAPI
                    Button((current ? "✓ " : "") + (Self.controllerNames[api] ?? api) + (later ? " (next launch)" : "")) {
                        wine.switchController(to: api)
                    }
                }
            } message: {
                Text("XInput and Keyboard and mouse switch at once. DirectInput and HID modes need the game to start with them, so they apply at the next launch. The choice is remembered for this game only; Settings › Controls is the default for the others.")
            }
            .confirmationDialog("Touch mouse", isPresented: $showTouchMenu, titleVisibility: .visible) {
                ForEach(GameInput.TouchMode.allCases, id: \.self) { mode in
                    Button((mode == GameInput.shared.touchMode ? "✓ " : "") + (Self.touchNames[mode] ?? mode.rawValue)) {
                        GameInput.shared.setTouchMode(mode)
                    }
                }
            } message: {
                Text("Off: touching the game does nothing, so holding the phone never clicks. A three-finger tap opens this menu in every mode. On-screen controller buttons keep working in every mode. The choice is remembered for this game.")
            }
            .confirmationDialog("On-screen controller", isPresented: $showPadMenu, titleVisibility: .visible) {
                ForEach(TouchPadMode.allCases) { mode in
                    Button((mode == pad.mode ? "✓ " : "") + Self.padNames[mode]!) { pad.setMode(mode, remember: true) }
                }
                Button("Edit layout…") {
                    // The editor is full screen over the hidden game view.
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { pad.editing = true }
                }
            } message: {
                Text("Automatic shows it while no controller is connected and hides it when one connects. The choice is remembered for this game.")
            }
            .fullScreenCover(isPresented: $pad.editing) {
                TouchLayoutEditor(gameKey: session.gameID.isEmpty ? nil : session.gameID, title: session.title)
            }
            .onChange(of: showGameMenu) { _, open in
                dlog("[ui] game menu \(open ? "shown" : "closed")")
                if !open, session.shown, !openingSubmenu, !pad.editing { GameHostView.shared.isHidden = false }
            }
            .onChange(of: showControllerMenu) { _, open in closedSubmenu(open) }
            .onChange(of: showTouchMenu) { _, open in closedSubmenu(open) }
            .onChange(of: showPadMenu) { _, open in
                if open { openingSubmenu = false }
                if !open, session.shown, !pad.editing {
                    // "Edit layout…" opens the editor right after this closes: stay hidden for it.
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        if session.shown, !pad.editing, !showGameMenu { GameHostView.shared.isHidden = false }
                    }
                }
            }
            .onChange(of: pad.editing) { _, editing in
                if !editing, session.shown {
                    pad.reloadLayout()
                    GameHostView.shared.isHidden = false
                }
            }
    }

    private func submenu(_ open: @escaping () -> Void) {
        openingSubmenu = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35, execute: open)
    }

    private func closedSubmenu(_ open: Bool) {
        if open { openingSubmenu = false }
        if !open, session.shown { GameHostView.shared.isHidden = false }
    }

    /// Quit game with session reuse: asks the game to close with Alt+F4 through its normal input,
    /// as Madeira's Quit game does (Library.swift requestQuit), so it can save and run its own exit
    /// dialog. Its view stays up until the session ends; the Wine session stays for the next game.
    private func requestQuit() {
        dlog("[ui] quit requested: Alt+F4 to \(session.title)")
        mid_post_key(0x12, 1); mid_post_key(0x73, 1)   // VK_MENU, VK_F4
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { mid_post_key(0x73, 0); mid_post_key(0x12, 0) }
        GameHostView.shared.setStatus("Closing \(session.title)… confirm in the game if it asks")
        let title = session.title
        // Quit game again while the first is pending: the game is not closing. Say where its
        // threads are now (build 161: the second press came before the first one's report, and
        // the log was taken before either), and end it.
        if quitPending {
            dlog("[ui] Quit game again: \(title) is still running")
            mid_wow_thread_report("Quit game again: \(title) still runs")
            forceQuit()
            return
        }
        // Still running 6 s later (a game can hang on its way out, or wait in a dialog): say where
        // its threads are in the log, and force quit it 4 s after that unless it ends meanwhile.
        quitPending = true
        quitAttempt += 1
        let attempt = quitAttempt
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) {
            guard attempt == quitAttempt, session.title == title, case .running = wine.state, !quitStuck else { return }
            dlog("[ui] \(title) is still running 6 s after Quit game")
            mid_wow_thread_report("\(title) still runs 6 s after Quit game")
            quitStuck = true
            GameHostView.shared.setStatus("\(title) has not closed: force quitting it in 4 s.")
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                guard attempt == quitAttempt, session.title == title, case .running = wine.state else { return }
                dlog("[ui] \(title) is still running 10 s after Quit game: force quit")
                forceQuit()
            }
        }
    }

    /// Force quit (kept sessions): the Wine server ends the game's processes as TerminateProcess
    /// does; the session stays for the next game (docs/SESSION_REUSE.md).
    private func forceQuit() {
        dlog("[ui] force quit requested for \(session.title)")
        quitAttempt += 1
        guard wine.forceQuit() else { quitStuck = false; return }
        GameHostView.shared.setStatus("Ending \(session.title)…")
        let attempt = quitAttempt, title = session.title
        DispatchQueue.main.asyncAfter(deadline: .now() + 8) {
            guard attempt == quitAttempt, session.title == title, case .running = wine.state else { return }
            dlog("[ui] \(title) is still running 8 s after Force quit")
            mid_wow_thread_report("\(title) still runs 8 s after Force quit")
            GameHostView.shared.setStatus("\(title) could not be ended. Quit game and close MYIOSDECK stops it.")
        }
    }

    static let controllerOrder = ["xinput", "dinput", "hid", "dualsense", "keyboard"]
    static let controllerNames = [
        "xinput": "Xbox (XInput)", "dinput": "XInput + DirectInput", "hid": "HID gamepad",
        "dualsense": "DualSense (PS5)", "keyboard": "Keyboard and mouse",
    ]
    static let touchNames: [GameInput.TouchMode: String] = [
        .off: "Off", .trackpad: "Trackpad with cursor", .direct: "Tap where you click",
    ]
    static let padNames: [TouchPadMode: String] = [
        .auto: "Automatic (hidden with a controller)", .always: "Always shown", .off: "Off",
    ]
}

/// "Return to <game>": a game hidden with the game menu's Hide game is still running. Shown at the
/// top of every page while that is so.
struct ReturnToGameBar: View {
    @EnvironmentObject private var wine: WineController
    @ObservedObject private var session = GameSession.shared

    var body: some View {
        if wine.isPlaying, !session.shown, !session.title.isEmpty {
            Button {
                dlog("[ui] return to \(session.title)")
                session.shown = true
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "play.fill")
                    Text("Return to \(session.title)").lineLimit(1)
                    Spacer(minLength: 0)
                    Text("Running").font(.caption.weight(.semibold)).foregroundStyle(Glass.text.opacity(0.7))
                }
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Glass.text)
                .padding(.horizontal, 18).frame(height: 44)
                .glassCapsule(highlighted: true)
            }
            .buttonStyle(.plain)
            .accessibilityHint("Shows the running game")
        }
    }
}
