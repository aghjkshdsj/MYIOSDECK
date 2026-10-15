// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit

/// The app's questions and menus as glass dialogs (iOS's alerts and confirmation dialogs cannot be
/// driven by a controller). They show in a window of their own above everything, the game view
/// included, so the game menu, a sheet's question and a launch question all look and work alike.
struct DialogButton: Identifiable {
    enum Role { case normal, cancel, destructive }
    let id = UUID()
    let title: String
    var role = Role.normal
    var action: () -> Void = {}

    static func cancel(_ title: String = "Cancel", _ action: @escaping () -> Void = {}) -> DialogButton {
        DialogButton(title: title, role: .cancel, action: action)
    }
}

struct GlassDialogModel: Identifiable {
    let id = UUID()
    let title: String
    var message: String?
    var buttons: [DialogButton]
    /// Runs when the dialog closes without a button (B, a tap beside it).
    var onDismiss: () -> Void = {}
}

@MainActor
final class Dialogs: ObservableObject {
    static let shared = Dialogs()
    @Published private(set) var stack: [GlassDialogModel] = []
    var showing: Bool { !stack.isEmpty }
    private var window: UIWindow?
    /// The dialogs' window (ScrollIntoView scrolls in it while one shows).
    var overlayWindow: UIWindow? { window }

    func show(_ d: GlassDialogModel) {
        stack.append(d)
        // Its focus scope is kept here, not by the panel's appear / disappear: the window is hidden
        // as the last dialog closes, and a hidden window's views may never report that they went
        // (a stale scope then took every move, build 193).
        let id = d.id
        PadFocus.shared.pushScope("dialog.\(id)", token: id) { [weak self] in self?.close(id, button: nil) }
        updateWindow()
    }

    /// Closes it; `button` ran (or nil: dismissed, which also runs the cancel button's action).
    func close(_ id: UUID, button: DialogButton?) {
        guard let d = stack.first(where: { $0.id == id }) else { return }
        stack.removeAll { $0.id == id }
        PadFocus.shared.popScope(token: id)
        updateWindow()
        if let button {
            button.action()
        } else {
            d.buttons.first(where: { $0.role == .cancel })?.action()
            d.onDismiss()
        }
    }

    /// The dialog with this id closes without running anything (its presenter stopped asking).
    func withdraw(_ id: UUID) {
        stack.removeAll { $0.id == id }
        PadFocus.shared.popScope(token: id)
        updateWindow()
    }

    private func updateWindow() {
        if stack.isEmpty {
            window?.isHidden = true
            return
        }
        if window == nil {
            guard let scene = UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene })
                .first(where: { $0.activationState == .foregroundActive }) ?? UIApplication.shared.connectedScenes.first as? UIWindowScene
            else { return }
            let w = UIWindow(windowScene: scene)
            w.windowLevel = .alert + 1
            let host = UIHostingController(rootView: DialogHost())
            host.view.backgroundColor = .clear
            w.rootViewController = host
            w.backgroundColor = .clear
            TouchWatcher.attach(to: w)
            window = w
        }
        let theme = Appearance.shared.theme
        window?.overrideUserInterfaceStyle = theme == .dark ? .dark : theme == .light ? .light : .unspecified
        window?.isHidden = false
    }
}

/// The top dialog: a dimmed screen, a glass panel, the title, the message, one button per row.
private struct DialogHost: View {
    @ObservedObject private var dialogs = Dialogs.shared

    var body: some View {
        ZStack {
            if let d = dialogs.stack.last {
                Color.black.opacity(0.45).ignoresSafeArea()
                    .onTapGesture { dialogs.close(d.id, button: nil) }
                DialogPanel(dialog: d)
                    .id(d.id)
                    .transition(.scale(scale: 0.94).combined(with: .opacity))
            }
        }
        .animation(Appearance.shared.animation(.spring(response: 0.3, dampingFraction: 0.85)), value: dialogs.stack.last?.id)
        .tint(Glass.primary)
    }
}

private struct DialogPanel: View {
    let dialog: GlassDialogModel
    @ObservedObject private var dialogs = Dialogs.shared

    var body: some View {
        GeometryReader { geo in
            let landscape = geo.size.width > geo.size.height
            VStack(spacing: 0) {
                VStack(spacing: 6) {
                    Text(dialog.title).font(.headline).multilineTextAlignment(.center)
                    if let m = dialog.message, !m.isEmpty {
                        ScrollView {
                            Text(m).font(.footnote).foregroundStyle(Glass.secondary).multilineTextAlignment(.center)
                                .frame(maxWidth: .infinity)
                        }
                        .frame(maxHeight: landscape ? 110 : 220)
                        .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .padding(.horizontal, 20).padding(.top, 20).padding(.bottom, 14)
                PadScrollView {
                    VStack(spacing: 8) {
                        ForEach(Array(dialog.buttons.enumerated()), id: \.element.id) { i, b in
                            Button { dialogs.close(dialog.id, button: b) } label: {
                                Text(b.title)
                                    .font(.body.weight(b.role == .cancel ? .semibold : .regular))
                                    .foregroundStyle(b.role == .destructive ? Glass.bad : Glass.text)
                                    .multilineTextAlignment(.center)
                                    .frame(maxWidth: .infinity, minHeight: 46)
                                    .padding(.horizontal, 12)
                                    .glassPanel(cornerRadius: 14, highlighted: false)
                            }
                            .buttonStyle(.plain)
                            .padFocus("dialog.\(dialog.id).\(i)", radius: 14, preferred: i == 0) {
                                dialogs.close(dialog.id, button: b)
                            }
                        }
                    }
                    .padding(.horizontal, 16).padding(.vertical, 6)
                }
                .frame(maxHeight: landscape ? max(120, geo.size.height - 190) : 420)
                .fixedSize(horizontal: false, vertical: dialog.buttons.count <= 4)
                .padding(.bottom, 12)
            }
            .frame(width: min(geo.size.width - 40, 400))
            .glassPanel(cornerRadius: 24)
            .shadow(color: .black.opacity(0.35), radius: 30)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .foregroundStyle(Glass.text)
        .environment(\.padScope, "dialog.\(dialog.id)")
        .accessibilityAddTraits(.isModal)
    }
}

// MARK: - presenting from a view, like confirmationDialog / alert

private struct GlassDialogModifier: ViewModifier {
    @Binding var isPresented: Bool
    let title: () -> String
    let message: () -> String?
    let buttons: () -> [DialogButton]
    @State private var shown: UUID?

    func body(content: Content) -> some View {
        content
            .onChange(of: isPresented) { _, on in on ? present() : withdraw() }
            .onAppear { if isPresented { present() } }
            .onDisappear { withdraw() }
    }

    private func present() {
        guard shown == nil else { return }
        let binding = $isPresented
        let model = GlassDialogModel(
            title: title(), message: message(),
            buttons: buttons().map { b in
                DialogButton(title: b.title, role: b.role) { binding.wrappedValue = false; b.action() }
            },
            onDismiss: { binding.wrappedValue = false })
        shown = model.id
        Dialogs.shared.show(model)
    }

    private func withdraw() {
        guard let id = shown else { return }
        shown = nil
        Dialogs.shared.withdraw(id)
    }
}

extension View {
    /// A glass dialog while `isPresented` (the controller-friendly confirmationDialog / alert).
    func glassDialog(_ title: @escaping @autoclosure () -> String, isPresented: Binding<Bool>,
                     message: @escaping @autoclosure () -> String? = nil,
                     buttons: @escaping () -> [DialogButton]) -> some View {
        modifier(GlassDialogModifier(isPresented: isPresented, title: title, message: message, buttons: buttons))
    }

    /// A glass dialog for an item while it is set.
    func glassDialog<T>(item: Binding<T?>, title: @escaping (T) -> String, message: @escaping (T) -> String? = { _ in nil },
                        buttons: @escaping (T) -> [DialogButton]) -> some View {
        let shown = Binding<Bool>(get: { item.wrappedValue != nil }, set: { if !$0 { item.wrappedValue = nil } })
        return modifier(GlassDialogModifier(
            isPresented: shown,
            title: { item.wrappedValue.map(title) ?? "" },
            message: { item.wrappedValue.flatMap(message) },
            buttons: { item.wrappedValue.map(buttons) ?? [] }))
    }
}

/// A list of actions as a dialog (what a Menu shows, for the controller).
@MainActor func showActions(_ title: String, message: String? = nil, _ buttons: [DialogButton]) {
    Dialogs.shared.show(GlassDialogModel(title: title, message: message, buttons: buttons + [.cancel()]))
}

/// iOS's share sheet for a file, from code (ShareLink needs a touch): the controller's way to
/// share a log.
@MainActor func presentShare(_ url: URL) {
    let windows = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap(\.windows)
    guard var top = (windows.first { $0.isKeyWindow } ?? windows.first)?.rootViewController else { return }
    while let p = top.presentedViewController { top = p }
    let vc = UIActivityViewController(activityItems: [url], applicationActivities: nil)
    vc.popoverPresentationController?.sourceView = top.view
    vc.popoverPresentationController?.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 1, height: 1)
    top.present(vc, animated: true)
}
