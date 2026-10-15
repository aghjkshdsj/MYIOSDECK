// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Home (mockups 1 and 2): the last played (or selected) game large, Play and "…", and the
/// recently played games. Landscape: the game's artwork fills the screen and a card selects the
/// hero; portrait: "Jump back in" with the game's art, then the others as tall cards.
struct HomeView: View {
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var recent = RecentlyPlayed.shared
    @ObservedObject private var launcher = GameLauncher.shared
    @EnvironmentObject private var wine: WineController
    @EnvironmentObject private var device: DeviceInfo
    @Environment(\.isLandscape) private var landscape
    @State private var selectedID: String?

    private var games: [LibraryGame] { GameCatalog.recent(steam: steam, custom: custom, recent: recent) }

    private var hero: LibraryGame? {
        let list = games
        if let selectedID, let g = list.first(where: { $0.id == selectedID }) { return g }
        return list.first
    }

    var body: some View {
        Group {
            if let hero {
                if landscape { landscapeHome(hero) } else { portraitHome(hero) }
            } else {
                welcome
            }
        }
        .onAppear { ui.focusedGameID = hero?.id }
        .onChange(of: hero?.id) { _, id in ui.focusedGameID = id }
        .padMenu { if let hero { showActions(hero.name, moreActions(hero)) } else { ui.show(.settings) } }
    }

    // MARK: landscape (mockup 1)

    private func landscapeHome(_ game: LibraryGame) -> some View {
        GeometryReader { geo in
            VStack(alignment: .leading, spacing: 0) {
                Spacer(minLength: 4)
                heroText(game, titleSize: min(72, max(40, geo.size.height * 0.17)))
                Spacer(minLength: 10)
                recentRow(wide: true)
            }
            .padding(.horizontal, 10)
            .padding(.bottom, 4)
        }
    }

    private func heroText(_ game: LibraryGame, titleSize: CGFloat) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Eyebrow(text: recent.started[game.id] == nil ? "Ready to play" : "Last played")
            Text(game.name)
                .font(.system(size: titleSize, weight: .heavy))
                .fontWidth(.condensed)
                .lineLimit(2).minimumScaleFactor(0.5)
                .foregroundStyle(Glass.text)
                .shadow(color: .black.opacity(0.35), radius: 10, y: 3)
                .frame(maxWidth: 560, alignment: .leading)
                .accessibilityAddTraits(.isHeader)
            Text(GameCatalog.lastPlayedText(game, recent: recent))
                .font(.title3).foregroundStyle(Glass.text.opacity(0.9))
            playRow(game)
                .padding(.top, 8)
            statusLine
        }
    }

    private func playRow(_ game: LibraryGame) -> some View {
        HStack(spacing: 14) {
            Button {
                launcher.play(game)
            } label: {
                Label("Play", systemImage: "play.fill").padding(.horizontal, 12)
            }
            .buttonStyle(PrimaryButtonStyle())
            .disabled(launcher.blocker(for: game) != nil || launcher.busyText != nil)
            .accessibilityHint(launcher.blocker(for: game) ?? "Starts \(game.name)")
            .padFocus("home.play", radius: 28, preferred: true, secondary: { ui.open(.gameSettings(game.id)) }) { play(game) }
            moreMenu(game)
        }
    }

    private func play(_ game: LibraryGame) {
        if launcher.blocker(for: game) == nil, launcher.busyText == nil { launcher.play(game) }
    }

    /// The "…" actions (the Menu, and the same list as a dialog for the controller).
    private func moreActions(_ game: LibraryGame) -> [DialogButton] {
        let fav = Favorites.shared.contains(game.id)
        return [DialogButton(title: "Game details") { ui.open(.game(game.id)) },
                DialogButton(title: "Game settings") { ui.open(.gameSettings(game.id)) },
                DialogButton(title: fav ? "Remove from Favorites" : "Add to Favorites") { Favorites.shared.set(game.id, !fav) }]
    }

    private func moreMenu(_ game: LibraryGame) -> some View {
        Menu {
            Button { ui.open(.game(game.id)) } label: { Label("Game details", systemImage: "info.circle") }
            Button { ui.open(.gameSettings(game.id)) } label: { Label("Game settings", systemImage: "slider.horizontal.3") }
            let fav = Favorites.shared.contains(game.id)
            Button { Favorites.shared.set(game.id, !fav) } label: {
                Label(fav ? "Remove from Favorites" : "Add to Favorites", systemImage: fav ? "star.slash" : "star")
            }
        } label: {
            GlassMoreLabel(size: landscape ? 52 : 56)
        }
        .accessibilityLabel("More for \(game.name)")
        .padFocus("home.more", radius: 28) { showActions(game.name, moreActions(game)) }
    }

    /// Why Play waits (Steam Cloud, Valve's components), why the last start failed, or why Play is off.
    @ViewBuilder private var statusLine: some View {
        if let busy = launcher.busyText {
            HStack(spacing: 8) { ProgressView(); Text(busy) }
                .font(.subheadline).foregroundStyle(Glass.text.opacity(0.85))
        } else if let failure = wine.dockFailure {
            Label("Steam did not start the game: \(failure)", systemImage: "exclamationmark.triangle.fill")
                .font(.footnote).foregroundStyle(Glass.warn).lineLimit(2)
        } else if let hero, let why = launcher.blocker(for: hero) {
            Text(why).font(.footnote).foregroundStyle(Glass.secondary).lineLimit(2)
        }
    }

    private func recentRow(wide: Bool) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionHeader(title: "Recent games") {
                Button { ui.show(.library) } label: {
                    HStack(spacing: 6) { Text("View library"); Image(systemName: "chevron.right") }
                        .font(.subheadline).foregroundStyle(Glass.text)
                }
                .buttonStyle(.plain)
                .padFocus("home.viewLibrary", radius: 10) { ui.show(.library) }
            }
            PadScrollView(axes: .horizontal) {
                LazyHStack(spacing: 14) {
                    ForEach(rowGames(wide: wide)) { game in
                        card(game, wide: wide)
                    }
                }
                .padding(.vertical, 4)
                .padding(.horizontal, 2)
            }
            .scrollIndicators(.hidden)
        }
    }

    /// Landscape: every recent game (the hero among them, selected); portrait: the others.
    private func rowGames(wide: Bool) -> [LibraryGame] {
        let list = Array(games.prefix(12))
        return wide ? list : list.filter { $0.id != hero?.id }
    }

    private func card(_ game: LibraryGame, wide: Bool) -> some View {
        let selected = wide && game.id == hero?.id
        let size = wide ? CGSize(width: 200, height: 90) : CGSize(width: 116, height: 174)
        return Button {
            if wide && !selected {
                withGlassAnimation { selectedID = game.id }
            } else {
                ui.open(.game(game.id))
            }
        } label: {
            ArtworkView(source: game.artwork, kind: wide ? .wide : .portrait, name: game.name, compactPlaceholder: true)
                .frame(width: size.width, height: size.height)
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(selected ? Color.white : Glass.border, lineWidth: selected ? 2.5 : 1))
                .shadow(color: selected ? .white.opacity(0.25) : .clear, radius: 8)
                .overlay(alignment: .topTrailing) {
                    if Favorites.shared.contains(game.id) {
                        Image(systemName: "star.fill").font(.caption).foregroundStyle(.yellow).padding(6)
                    }
                }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(game.name)
        .accessibilityHint(wide && !selected ? "Shows it above" : "Opens its page")
        .accessibilityAddTraits(selected ? .isSelected : [])
        .padFocus("home.card.\(game.id)", radius: 14, secondary: { ui.open(.gameSettings(game.id)) },
                  onFocus: { if wide { withGlassAnimation { selectedID = game.id } } }) {
            ui.open(.game(game.id))
        }
    }

    // MARK: portrait (mockup 2)

    private func portraitHome(_ game: LibraryGame) -> some View {
        PadScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text(recent.started[game.id] == nil ? "Ready to play" : "Jump back in")
                    .font(.title2.weight(.semibold))
                Button { ui.open(.game(game.id)) } label: {
                    ArtworkView(source: game.artwork, kind: .hero, name: game.name)
                        .aspectRatio(1.72, contentMode: .fit)
                        .overlay(alignment: .bottomLeading) {
                            Text(game.name)
                                .font(.system(size: 46, weight: .heavy)).fontWidth(.condensed)
                                .lineLimit(1).minimumScaleFactor(0.4)
                                .foregroundStyle(.white)
                                .shadow(color: .black.opacity(0.6), radius: 8, y: 2)
                                .padding(16)
                        }
                        .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
                        .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).strokeBorder(.white.opacity(0.5), lineWidth: 1.5))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(game.name), opens its page")
                .padFocus("home.heroArt", radius: 22) { ui.open(.game(game.id)) }
                VStack(alignment: .leading, spacing: 2) {
                    Text(game.name).font(.title2.weight(.semibold))
                    Text(GameCatalog.lastPlayedText(game, recent: recent)).font(.body).foregroundStyle(Glass.secondary)
                }
                HStack(spacing: 14) {
                    Button { launcher.play(game) } label: {
                        Label("Play", systemImage: "play.fill").frame(maxWidth: 200)
                    }
                    .buttonStyle(PrimaryButtonStyle())
                    .disabled(launcher.blocker(for: game) != nil || launcher.busyText != nil)
                    .padFocus("home.play", radius: 28, preferred: true, secondary: { ui.open(.gameSettings(game.id)) }) { play(game) }
                    moreMenu(game)
                }
                .frame(maxWidth: .infinity)
                statusLine
                if rowGames(wide: false).isEmpty == false {
                    recentRow(wide: false).padding(.top, 6)
                }
                controllerStatus.padding(.top, 8)
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 20)
        }
        .scrollIndicators(.hidden)
    }

    private var controllerStatus: some View {
        HStack(spacing: 10) {
            Image(systemName: "gamecontroller.fill").font(.title3)
            Text(device.controllers.isEmpty ? "No controller connected" : "Controller connected")
            if let name = device.controllers.first { Text("· \(name)").foregroundStyle(Glass.secondary).lineLimit(1) }
        }
        .font(.subheadline)
        .foregroundStyle(Glass.text.opacity(0.9))
        .onTapGesture { ui.show(.controllers) }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .padFocus("home.controllers", radius: 10) { ui.show(.controllers) }
    }

    // MARK: nothing installed yet

    private var welcome: some View {
        PadScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Eyebrow(text: "Welcome")
                Text("Your games, here")
                    .font(.system(size: landscape ? 54 : 44, weight: .heavy)).fontWidth(.condensed)
                    .lineLimit(2).minimumScaleFactor(0.6)
                Text(steam.signedIn
                     ? "Install a game from your Steam library, or add a DRM-free game folder (GOG, itch.io)."
                     : "Sign in to Steam to install your games, or add a DRM-free game folder (GOG, itch.io).")
                    .font(.body).foregroundStyle(Glass.secondary).frame(maxWidth: 520, alignment: .leading)
                HStack(spacing: 12) {
                    Button { ui.show(.library) } label: { Label("Open library", systemImage: "square.grid.2x2.fill") }
                        .buttonStyle(PrimaryButtonStyle(compact: true))
                        .padFocus("home.openLibrary", radius: 20, preferred: true) { ui.show(.library) }
                    if !steam.signedIn {
                        Button { ui.open(.settings(.steam)) } label: { Text("Sign in to Steam") }
                            .buttonStyle(GlassButtonStyle(compact: true))
                            .padFocus("home.signIn", radius: 18) { ui.open(.settings(.steam)) }
                    }
                }
                .padding(.top, 6)
                controllerStatus.padding(.top, 20)
            }
            .padding(.horizontal, landscape ? 10 : 20)
            .padding(.top, landscape ? 24 : 12)
        }
    }
}
