// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Portrait cover art that falls through the candidate URLs until one loads,
/// then shows the title on a plain tile if none does.
struct SteamCover: View {
    let urls: [URL]
    let title: String
    @State private var index = 0

    var body: some View {
        // The 2:3 frame sets the size; the image only fills and is cropped to
        // it, so a wide header-image fallback cannot spill into the next tile.
        Deck.panelHi
            .aspectRatio(2 / 3, contentMode: .fit)
            .overlay {
                if index < urls.count {
                    AsyncImage(url: urls[index]) { phase in
                        switch phase {
                        case .success(let image): image.resizable().scaledToFill()
                        case .failure: Color.clear.onAppear { index += 1 }
                        default: ProgressView()
                        }
                    }
                    .id(index)
                } else {
                    Text(title).font(.caption.weight(.semibold)).multilineTextAlignment(.center).padding(6)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8))
    }
}

/// Stage 4b: the signed-in account's Windows games.
struct SteamGamesGrid: View {
    @ObservedObject var library: SteamLibrary
    @State private var filter = ""

    private var shown: [OwnedSteamGame] {
        let f = filter.trimmingCharacters(in: .whitespaces)
        return f.isEmpty ? library.games : library.games.filter { $0.name.localizedCaseInsensitiveContains(f) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(library.games.isEmpty ? "Owned games" : "Owned games (\(library.games.count))")
                    .font(.headline)
                Spacer()
                if library.refreshing { ProgressView() }
                Button("Refresh") { Task { await library.refresh() } }
                    .buttonStyle(DeckButtonStyle())
                    .frame(width: 110)
                    .disabled(library.refreshing)
            }
            if let error = library.error {
                StatusRow(label: "Could not load the library", detail: error, level: .bad)
            }
            if library.games.count > 12 {
                TextField("Search your games", text: $filter)
                    .textFieldStyle(.roundedBorder)
            }
            if library.games.isEmpty, !library.refreshing, library.error == nil {
                Text("No Windows games loaded yet. Tap Refresh.").font(.caption).foregroundStyle(Deck.dim)
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 96), spacing: 10)], spacing: 12) {
                ForEach(shown) { game in
                    VStack(alignment: .leading, spacing: 4) {
                        SteamCover(urls: library.artwork(game.id), title: game.name)
                        Text(game.name).font(.caption2).lineLimit(2).foregroundStyle(Deck.dim)
                    }
                }
            }
        }
    }
}
