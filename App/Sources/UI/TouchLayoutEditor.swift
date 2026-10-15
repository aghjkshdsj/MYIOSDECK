// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The on-screen controller's layout editor (mockup 3): drag a control to move it, drag a corner
/// handle (or the Size slider) to resize it, Opacity for the whole controller, undo, reset, and
/// Save for this game or as the default. Over the game's artwork (the game itself is paused
/// behind it while it is open from the game menu). Each orientation has its own layout.
struct TouchLayoutEditor: View {
    /// The game whose layout this edits; nil edits the default.
    let gameKey: String?
    let title: String
    @Environment(\.dismiss) private var dismiss

    @State private var layout = TouchLayout.standard(landscape: true)
    @State private var landscape: Bool?
    @State private var selected: PadControl?
    @State private var undo: [TouchLayout] = []
    @State private var changed = false
    @State private var confirmDiscard = false
    @State private var savedText: String?

    private var artwork: ArtworkSource {
        guard let key = gameKey ?? UIState.shared.focusedGameID,
              let g = GameCatalog.find(key, steam: SteamLibrary.shared, custom: CustomGames.shared) else { return .blank }
        return g.artwork
    }

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width >= geo.size.height
            ZStack {
                ArtworkBackdrop(source: artwork, blurred: false)
                Color.black.opacity(0.15).ignoresSafeArea()
                TouchCanvas(layout: $layout, selected: $selected) { before in
                    undo.append(before)
                    changed = true
                    savedText = nil
                }
                .ignoresSafeArea()
                VStack {
                    topBar
                    Spacer()
                    bottomPanel
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
            }
            .onAppear { load(isLandscape) }
            .onChange(of: isLandscape) { _, l in load(l) }
        }
        .statusBarHidden()
        .confirmationDialog("Discard your changes?", isPresented: $confirmDiscard, titleVisibility: .visible) {
            Button("Discard changes", role: .destructive) { dismiss() }
            Button("Keep editing", role: .cancel) {}
        }
    }

    private func load(_ isLandscape: Bool) {
        if landscape != nil, changed, landscape != isLandscape {
            // The screen turned with unsaved changes: they belong to the other orientation's layout.
            saveLayout(scope: gameKey ?? TouchLayoutStore.defaultScope, for: landscape!)
        }
        landscape = isLandscape
        layout = TouchLayoutStore.layout(for: gameKey, landscape: isLandscape)
        undo = []
        selected = nil
        changed = false
    }

    // MARK: top bar

    private var topBar: some View {
        ZStack(alignment: .top) {
            HStack(alignment: .top) {
                Button {
                    if changed { confirmDiscard = true } else { dismiss() }
                } label: {
                    HStack(spacing: 8) { Image(systemName: "chevron.left"); Text("Controls") }
                        .font(.body).padding(.horizontal, 16).frame(height: 44).glassCapsule()
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close the editor")
                Spacer()
                Button {
                    if let last = undo.popLast() { layout = last; selected = nil; changed = !undo.isEmpty }
                } label: {
                    Image(systemName: "arrow.uturn.backward").font(.system(size: 17, weight: .semibold))
                        .frame(width: 44, height: 44).glassCircle()
                }
                .buttonStyle(.plain)
                .disabled(undo.isEmpty)
                .opacity(undo.isEmpty ? 0.5 : 1)
                .accessibilityLabel("Undo")
                saveButton
            }
            VStack(spacing: 4) {
                Text("Edit layout").font(.body.weight(.medium))
                    .padding(.horizontal, 22).frame(height: 40).glassCapsule()
                Text(savedText ?? (selected.map { "\($0.title): drag to move, a corner to resize" } ?? "Drag a control to move it"))
                    .font(.footnote).foregroundStyle(.white.opacity(0.85))
                    .shadow(color: .black.opacity(0.6), radius: 3)
            }
            .padding(.horizontal, 140)
        }
        .foregroundStyle(.white)
    }

    @ViewBuilder private var saveButton: some View {
        if let gameKey {
            Menu {
                Button("Save for \(title)") { save(scope: gameKey) }
                Button("Save as the default for all games") { save(scope: TouchLayoutStore.defaultScope) }
            } label: {
                Text("Save").font(.body.weight(.semibold)).foregroundStyle(.white)
                    .padding(.horizontal, 26).frame(height: 44)
                    .background(Capsule().fill(LinearGradient(colors: [Glass.primary, Glass.primaryDeep], startPoint: .top, endPoint: .bottom)))
                    .overlay(Capsule().strokeBorder(.white.opacity(0.35)))
                    .shadow(color: Glass.primary.opacity(0.6), radius: 10)
            }
        } else {
            Button("Save") { save(scope: TouchLayoutStore.defaultScope) }
                .buttonStyle(PrimaryButtonStyle(compact: true))
        }
    }

    private func saveLayout(scope: String, for isLandscape: Bool) {
        TouchLayoutStore.save(layout, scope: scope, landscape: isLandscape)
    }

    private func save(scope: String) {
        saveLayout(scope: scope, for: landscape ?? true)
        changed = false
        undo = []
        UINotificationFeedbackGenerator().notificationOccurred(.success)
        let whose = scope == TouchLayoutStore.defaultScope ? "the default layout" : title
        withGlassAnimation { savedText = "Saved for \(whose) (\(landscape == false ? "portrait" : "landscape")). Turn the phone to edit the other orientation." }
        if gameKey == nil || scope == gameKey { DispatchQueue.main.asyncAfter(deadline: .now() + 0.9) { dismiss() } }
    }

    // MARK: bottom panel

    private var scale: Binding<Double> {
        Binding(get: { selected.flatMap { layout.spec($0)?.scale } ?? 1 },
                set: { v in
                    guard let c = selected else { return }
                    layout.update(c) { $0.scale = v }
                    changed = true
                })
    }
    private var opacity: Binding<Double> {
        Binding(get: { layout.opacity }, set: { layout.opacity = $0; changed = true })
    }

    private var bottomPanel: some View {
        HStack(spacing: 14) {
            Text("Size").font(.subheadline)
            Slider(value: scale, in: PadControlSpec.scaleRange, onEditingChanged: { began in if began { undo.append(layout) } })
                .tint(Glass.primary).frame(minWidth: 90, maxWidth: 190)
                .disabled(selected == nil)
                .accessibilityLabel(selected.map { "Size of \($0.title)" } ?? "Size (select a control first)")
            Text("Opacity").font(.subheadline)
            Slider(value: opacity, in: TouchLayout.opacityRange, onEditingChanged: { began in if began { undo.append(layout) } })
                .tint(Glass.primary).frame(minWidth: 90, maxWidth: 170)
                .accessibilityLabel("Opacity of the controller")
            Rectangle().fill(Glass.separator).frame(width: 1, height: 26)
            Menu {
                ForEach(PadControl.allCases) { c in
                    let shown = !(layout.spec(c)?.hidden ?? true)
                    Button {
                        undo.append(layout)
                        layout.update(c) { $0.hidden = shown }
                        if shown, selected == c { selected = nil }
                        changed = true
                    } label: {
                        Label(c.title, systemImage: shown ? "checkmark.circle.fill" : "circle")
                    }
                }
            } label: {
                Text("Controls").font(.subheadline).foregroundStyle(Glass.primary)
            }
            .accessibilityLabel("Show or hide controls")
            Button("Reset layout") {
                undo.append(layout)
                layout = .standard(landscape: landscape ?? true)
                selected = nil
                changed = true
            }
            .font(.subheadline)
            .foregroundStyle(Glass.primary)
            .buttonStyle(.plain)
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 20).frame(height: 58)
        .glassCapsule()
        .environment(\.colorScheme, .dark)
        .frame(maxWidth: 760)
    }
}

/// The editor's canvas: the controller view in edit mode.
private struct TouchCanvas: UIViewRepresentable {
    @Binding var layout: TouchLayout
    @Binding var selected: PadControl?
    /// A drag or resize ended: the layout before it (for undo).
    let edited: (TouchLayout) -> Void

    func makeUIView(context: Context) -> TouchControllerView {
        let v = TouchControllerView(mode: .edit)
        v.setLayout(layout)
        v.onSelect = { c in DispatchQueue.main.async { selected = c } }
        v.onLayoutChanged = { [weak v] new in
            guard v != nil else { return }
            DispatchQueue.main.async {
                let before = layout
                guard new != before else { return }
                edited(before)
                layout = new
            }
        }
        return v
    }

    func updateUIView(_ v: TouchControllerView, context: Context) {
        if !v.isDragging { v.setLayout(layout) }
        v.selected = selected
        v.setNeedsLayout()
    }
}
