// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Search (the header's magnifier): every game of the library by name; a result opens its page.
struct SearchView: View {
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var recent = RecentlyPlayed.shared
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @FocusState private var focused: Bool

    private var results: [LibraryGame] {
        let all = GameCatalog.all(steam: steam, custom: custom)
        let q = query.trimmingCharacters(in: .whitespaces)
        let list = q.isEmpty ? all : all.filter { $0.name.localizedCaseInsensitiveContains(q) }
        return recent.sorted(list.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }) { $0.id }
    }

    var body: some View {
        VStack(spacing: 14) {
            HStack(spacing: 10) {
                HStack(spacing: 8) {
                    Image(systemName: "magnifyingglass").foregroundStyle(Glass.secondary)
                    TextField("Search games", text: $query)
                        .focused($focused)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                        .submitLabel(.search)
                }
                .padding(.horizontal, 16).frame(height: 46).glassCapsule()
                Button("Cancel") { dismiss() }.buttonStyle(.plain)
                    .padFocus("search.cancel", radius: 10) { dismiss() }
            }
            PadScrollView {
                LazyVStack(spacing: 8) {
                    ForEach(results) { game in
                        Button {
                            dismiss()
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { UIState.shared.open(.game(game.id)) }
                        } label: {
                            HStack(spacing: 14) {
                                ArtworkView(source: game.artwork, kind: .wide, name: game.name, compactPlaceholder: true)
                                    .frame(width: 92, height: 43)
                                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(game.name).font(.body.weight(.semibold)).lineLimit(1)
                                    Text(GameCatalog.isInstalled(game, steam: steam) ? "Installed · \(game.source.title)" : game.source.title)
                                        .font(.caption).foregroundStyle(Glass.secondary)
                                }
                                Spacer()
                                Image(systemName: "chevron.right").foregroundStyle(Glass.secondary)
                            }
                            .padding(10)
                            .glassPanel(cornerRadius: 14)
                        }
                        .buttonStyle(.plain)
                        .padFocus("search.\(game.id)", radius: 14, preferred: game.id == results.first?.id,
                                  secondary: { dismiss(); DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { UIState.shared.open(.gameSettings(game.id)) } }) {
                            dismiss()
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { UIState.shared.open(.game(game.id)) }
                        }
                    }
                    if results.isEmpty {
                        Text(query.isEmpty ? "Your library is empty." : "No game matches \"\(query)\".")
                            .foregroundStyle(Glass.secondary).padding(.top, 30)
                    }
                }
            }
            .scrollIndicators(.hidden)
        }
        .padding(16)
        .foregroundStyle(Glass.text)
        .background { ArtworkBackdrop(source: .blank) }
        .onAppear { if !PadFocus.shared.ringVisible { focused = true } }
        .presentationBackground(.clear)
        .padScope("search") { dismiss() }
    }
}

/// The header's controller button: connected controllers and the on-screen controller.
struct ControllersSheet: View {
    @EnvironmentObject private var device: DeviceInfo
    @EnvironmentObject private var settings: Settings
    @Environment(\.dismiss) private var dismiss
    @State private var editing = false

    private var padMode: Binding<TouchPadMode> {
        Binding(get: { TouchPadMode(rawValue: settings.touchPadMode) ?? .auto }, set: { settings.touchPadMode = $0.rawValue })
    }

    var body: some View {
        PadScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text("Controllers").font(.title2.weight(.bold))
                    Spacer()
                    Button("Done") { dismiss() }.buttonStyle(GlassButtonStyle(compact: true))
                        .padFocus("controllers.done", radius: 18) { dismiss() }
                }
                GlassCard {
                    if device.controllers.isEmpty {
                        GlassRow(last: true) {
                            Image(systemName: "gamecontroller").font(.title2).frame(width: 34)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("No controller connected")
                                Text("Pair one in iOS Settings › Bluetooth: Xbox, DualSense, Backbone and MFi controllers work.")
                                    .font(.footnote).foregroundStyle(Glass.secondary)
                            }
                        }
                    } else {
                        ForEach(Array(device.controllers.enumerated()), id: \.offset) { i, name in
                            GlassRow(last: i == device.controllers.count - 1) {
                                Image(systemName: "gamecontroller.fill").font(.title2).foregroundStyle(Glass.good).frame(width: 34)
                                Text(name)
                                Spacer()
                                Text("Player \(i + 1)").foregroundStyle(Glass.secondary)
                            }
                        }
                    }
                }
                Text("On-screen controller").font(.headline)
                GlassSegmented(options: TouchPadMode.allCases, selection: padMode, compact: true, focusID: "controllers.mode") { $0.title }
                Text("Automatic shows it over games while no controller is connected. Games can choose their own in Game settings.")
                    .font(.footnote).foregroundStyle(Glass.secondary)
                Button { editing = true } label: { Label("Edit default layout", systemImage: "square.and.pencil") }
                    .buttonStyle(GlassButtonStyle(compact: true))
                    .padFocus("controllers.edit", radius: 18) { editing = true }
            }
            .padding(20)
        }
        .foregroundStyle(Glass.text)
        .background { ArtworkBackdrop(source: .blank) }
        .presentationDetents([.medium, .large])
        .presentationBackground(.clear)
        .padScope("controllers") { dismiss() }
        .fullScreenCover(isPresented: $editing) { TouchLayoutEditor(gameKey: nil, title: "Default layout") }
    }
}
