// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The app's frame (mockups 1, 2 and 5): the selected game's artwork behind everything, the
/// MYIOSDECK wordmark, the Home / Library / Downloads pill and the round Search, Settings and
/// controller buttons; pages (a game, its settings, Storage…) are pushed over it.
struct RootView: View {
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var jit: JITController
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var wine: WineController
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var look = Appearance.shared

    var body: some View {
        GeometryReader { geo in
            NavigationStack(path: $ui.path) {
                TabShell()
                    .navigationDestination(for: UIState.Route.self) { route in
                        RouteView(route: route)
                    }
                    .toolbar(.hidden, for: .navigationBar)
            }
            .environment(\.isLandscape, geo.size.width > geo.size.height)
        }
        .tint(Glass.primary)
        .foregroundStyle(Glass.text)
        .modifier(LaunchAlerts())
        .modifier(GameSessionHost())
        .sheet(isPresented: $ui.showSearch) { SearchView() }
        .sheet(isPresented: $ui.showControllers) { ControllersSheet() }
        .onAppear {
            GameLauncher.shared.attach(settings: settings, jit: jit, engine: engine, wine: wine)
            SteamLibrary.shared.start()
            CustomGames.shared.refresh(prefix: wine.prefixURL)
            look.applyTheme()
        }
        .onChange(of: look.theme) { _, _ in look.applyTheme() }
    }
}

/// A pushed page.
private struct RouteView: View {
    let route: UIState.Route
    var body: some View {
        switch route {
        case .game(let id): GameDetailView(gameID: id)
        case .gameSettings(let id): GameSettingsView(gameID: id)
        case .settings(let section): SettingsPage(section: section)
        case .storage: GlassPage(back: "Settings", title: "Storage") { StorageView() }
        case .logs: GlassPage(back: "Settings", title: "Logs", scrolls: false) { LogView() }
        case .benchmarks: GlassPage(back: "Settings", title: "Benchmarks", scrolls: false) { PerformanceView() }
        case .developer: GlassPage(back: "About", title: "Test programs") { DeveloperView() }
        }
    }
}

/// The tabs under the header.
struct TabShell: View {
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @Environment(\.isLandscape) private var landscape

    private var focused: ArtworkSource {
        guard let id = ui.focusedGameID, let g = GameCatalog.find(id, steam: steam, custom: custom) else { return .blank }
        return g.artwork
    }

    var body: some View {
        ZStack {
            ArtworkBackdrop(source: focused, blurred: !(ui.tab == .home && landscape))
            VStack(spacing: landscape ? 6 : 12) {
                ShellHeader()
                ReturnToGameBar().padding(.horizontal, landscape ? 0 : 20)
                Group {
                    switch ui.tab {
                    case .home: HomeView()
                    case .library: LibraryView()
                    case .downloads: DownloadsView()
                    case .settings: SettingsView()
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                .transition(.opacity)
                .id(ui.tab)
            }
        }
    }
}

/// Wordmark, tab pill and round buttons.
struct ShellHeader: View {
    @ObservedObject private var ui = UIState.shared
    @EnvironmentObject private var device: DeviceInfo
    @Environment(\.isLandscape) private var landscape

    private var pill: some View {
        GlassSegmented(options: [UIState.Tab.home, .library, .downloads],
                       selection: Binding(get: { ui.tab }, set: { ui.show($0) }),
                       style: .tabs, compact: landscape) { $0.rawValue }
    }

    var body: some View {
        if landscape {
            ZStack {
                HStack(spacing: 10) {
                    Wordmark()
                    Spacer()
                    buttons
                }
                pill.frame(maxWidth: 380)
            }
            .padding(.top, 8)
            .padding(.horizontal, 4)
        } else {
            VStack(spacing: 14) {
                HStack(spacing: 10) {
                    Wordmark()
                    Spacer()
                    buttons
                }
                pill
            }
            .padding(.horizontal, 20)
            .padding(.top, 6)
        }
    }

    @ViewBuilder private var buttons: some View {
        GlassIconButton(systemImage: "magnifyingglass", label: "Search") { ui.showSearch = true }
        GlassIconButton(systemImage: "gearshape.fill", label: "Settings", highlighted: ui.tab == .settings) { ui.show(.settings) }
        if landscape {
            GlassIconButton(systemImage: "gamecontroller.fill", label: "Controllers", badge: !device.controllers.isEmpty) {
                ui.showControllers = true
            }
        }
    }
}

/// A pushed page: the artwork behind it, a back button, an optional large title, its content.
struct GlassPage<Content: View>: View {
    enum Back { case capsule(String), titled(String) }
    let back: Back
    var title: String?
    var artwork: ArtworkSource = .blank
    var scrolls = true
    var trailing: AnyView? = nil
    @ViewBuilder var content: Content
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @Environment(\.isLandscape) private var landscape

    init(back: String, title: String? = nil, artwork: ArtworkSource? = nil, scrolls: Bool = true, @ViewBuilder content: () -> Content) {
        self.back = .capsule(back)
        self.title = title
        self.artwork = artwork ?? .blank
        self.scrolls = scrolls
        self.content = content()
    }

    init(titledBack: String, title: String? = nil, artwork: ArtworkSource, scrolls: Bool = true, trailing: AnyView? = nil,
         @ViewBuilder content: () -> Content) {
        self.back = .titled(titledBack)
        self.title = title
        self.artwork = artwork
        self.scrolls = scrolls
        self.trailing = trailing
        self.content = content()
    }

    /// Pages without a game of their own keep the selected game's artwork.
    private var backdrop: ArtworkSource {
        if case .blank = artwork, let id = ui.focusedGameID, let g = GameCatalog.find(id, steam: steam, custom: custom) { return g.artwork }
        return artwork
    }

    var body: some View {
        ZStack {
            ArtworkBackdrop(source: backdrop, blurred: true)
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    switch back {
                    case .capsule(let t): GlassBackButton(style: .capsule(t)) { ui.back() }
                    case .titled(let t): GlassBackButton(style: .titled(t)) { ui.back() }
                    }
                    Spacer()
                    if let trailing { trailing }
                }
                ReturnToGameBar()
                if scrolls {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 16) {
                            if let title { Text(title).font(.largeTitle.weight(.bold)).foregroundStyle(Glass.text) }
                            content
                        }
                        .padding(.bottom, 24)
                        .frame(maxWidth: 900, alignment: .leading)
                        .frame(maxWidth: .infinity, alignment: landscape ? .leading : .center)
                    }
                    .scrollIndicators(.hidden)
                } else {
                    if let title { Text(title).font(.largeTitle.weight(.bold)).foregroundStyle(Glass.text) }
                    content
                }
            }
            .padding(.horizontal, landscape ? 8 : 20)
            .padding(.top, 6)
        }
        .toolbar(.hidden, for: .navigationBar)
    }
}

extension Appearance {
    /// The Theme setting on every window (UIKit too: the game's views, alerts, sheets). System
    /// clears the override, which SwiftUI's preferredColorScheme(nil) does not reliably do.
    func applyTheme() {
        let style: UIUserInterfaceStyle = theme == .dark ? .dark : theme == .light ? .light : .unspecified
        for scene in UIApplication.shared.connectedScenes {
            for window in (scene as? UIWindowScene)?.windows ?? [] { window.overrideUserInterfaceStyle = style }
        }
    }
}
