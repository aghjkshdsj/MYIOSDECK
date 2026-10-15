// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Search (the top bar's magnifier): a page like the tabs, every game of the library by name; a
/// result opens its page.
struct SearchView: View {
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var recent = RecentlyPlayed.shared
    @Environment(\.isLandscape) private var landscape
    @State private var query = ""
    @FocusState private var focused: Bool

    private var results: [LibraryGame] {
        let all = GameCatalog.all(steam: steam, custom: custom)
        let q = query.trimmingCharacters(in: .whitespaces)
        let list = q.isEmpty ? all : all.filter { $0.name.localizedCaseInsensitiveContains(q) }
        return recent.sorted(list.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }) { $0.id }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass").foregroundStyle(Glass.secondary)
                TextField("Search games", text: $query)
                    .focused($focused)
                    .textInputAutocapitalization(.never).autocorrectionDisabled()
                    .submitLabel(.search)
                if !query.isEmpty {
                    Button { query = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(Glass.secondary) }
                        .buttonStyle(.plain).accessibilityLabel("Clear search")
                }
            }
            .padding(.horizontal, 16).frame(height: 46).glassCapsule()
            .padFocus("search.field", radius: 23) { focused = true }
            PadScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: landscape ? 360 : 300), spacing: 10)], spacing: 10) {
                    ForEach(results) { game in
                        Button { ui.open(.game(game.id)) } label: {
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
                        .padFocus("search.\(game.id)", radius: 14, secondary: { ui.open(.gameSettings(game.id)) }) {
                            ui.open(.game(game.id))
                        }
                    }
                }
                if results.isEmpty {
                    Text(query.isEmpty ? "Your library is empty." : "No game matches \"\(query)\".")
                        .foregroundStyle(Glass.secondary).padding(.top, 30)
                }
            }
            .scrollIndicators(.hidden)
        }
        .padding(.horizontal, landscape ? 10 : 20)
        .onAppear { if !PadFocus.shared.ringVisible { focused = true } }
    }
}

/// The top bar's controller button: a page like the tabs with the connected controllers, the
/// controller games see, and the on-screen controller.
struct ControllersSheet: View {
    @EnvironmentObject private var device: DeviceInfo
    @EnvironmentObject private var settings: Settings
    @Environment(\.isLandscape) private var landscape
    @State private var editing = false

    private var padMode: Binding<TouchPadMode> {
        Binding(get: { TouchPadMode(rawValue: settings.touchPadMode) ?? .auto }, set: { settings.touchPadMode = $0.rawValue })
    }

    var body: some View {
        PadScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("Controllers").font(.largeTitle.weight(.bold))
                GlassCard {
                    if device.controllers.isEmpty {
                        GlassRow(last: true) {
                            Image(systemName: "gamecontroller").font(.title2).frame(width: 34)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("No controller connected")
                                Text("Pair one in iOS Settings › Bluetooth: Xbox, DualSense, DualShock 4, Backbone and MFi controllers work.")
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
                Text("Controller games see").font(.headline)
                GlassSegmented(options: PadStyle.allCases, selection: Binding(get: { PadStyle(rawValue: settings.padStyle) ?? .xbox },
                                                                               set: { settings.padStyle = $0.rawValue }),
                               compact: true, focusID: "controllers.style") { $0.title }
                    .frame(maxWidth: 420)
                Text("For games without their own choice (Game settings, or the game menu while playing). PlayStation also gives games a DualSense, so games with PlayStation support show its buttons.")
                    .font(.footnote).foregroundStyle(Glass.secondary)
                Text("On-screen controller").font(.headline)
                GlassSegmented(options: TouchPadMode.allCases, selection: padMode, compact: true, focusID: "controllers.mode") { $0.title }
                    .frame(maxWidth: 420)
                Text("Automatic shows it over games while no controller is connected. Games can choose their own in Game settings.")
                    .font(.footnote).foregroundStyle(Glass.secondary)
                Button { editing = true } label: { Label("Edit default layout", systemImage: "square.and.pencil") }
                    .buttonStyle(GlassButtonStyle(compact: true))
                    .padFocus("controllers.edit", radius: 18) { editing = true }
            }
            .frame(maxWidth: 900, alignment: .leading)
            .padding(.horizontal, landscape ? 10 : 20)
            .padding(.bottom, 24)
        }
        .scrollIndicators(.hidden)
        .fullScreenCover(isPresented: $editing) { TouchLayoutEditor(gameKey: nil, title: "Default layout") }
    }
}
