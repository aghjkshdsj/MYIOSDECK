// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UniformTypeIdentifiers

/// "My games": DRM-free Windows games the user added (CustomGames). A folder dropped into
/// Files › On My iPhone › MYIOSDECK › Games appears by itself; "Add a game folder" copies one
/// from anywhere else in Files. Play goes back to the library, which owns the game surface.
struct CustomGamesCard: View {
    @ObservedObject private var store = CustomGames.shared
    let prefix: URL
    let playBlocker: String?
    let onPlay: (CustomGame) -> Void

    @State private var showImporter = false
    @State private var choosingProgram: CustomGame?
    @State private var deleting: CustomGame?

    var body: some View {
        DeckCard(title: "My games (DRM-free)", icon: "folder.fill") {
            Text("Put an installed game's folder (GOG, itch.io, or copied from a PC) into Files › On My iPhone › MYIOSDECK › Games, or add one from anywhere in Files. Wine sees it as C:\\Games. 64-bit games run with or without JIT; 32-bit games without JIT are experimental.")
                .font(.subheadline).foregroundStyle(Deck.dim)
            HStack {
                Button("Add a game folder…") { showImporter = true }
                    .buttonStyle(DeckButtonStyle())
                Button("Refresh") { store.refresh(prefix: prefix) }
                    .buttonStyle(DeckButtonStyle())
                    .frame(width: 110)
            }
            if let copying = store.copying {
                HStack {
                    if copying.hasPrefix("Copying") { ProgressView() }
                    Text(copying).font(.subheadline).foregroundStyle(Deck.dim)
                }
            }
            ForEach(store.games) { game in
                Divider().overlay(Deck.panelHi)
                row(game)
            }
            ForEach(store.unusable.keys.sorted(), id: \.self) { folder in
                Divider().overlay(Deck.panelHi)
                StatusRow(label: folder, detail: store.unusable[folder] ?? "", level: .warn)
            }
            if let playBlocker, !store.games.isEmpty {
                Text(playBlocker).font(.caption).foregroundStyle(Deck.dim)
            }
        }
        .onAppear { store.refresh(prefix: prefix) }
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result { store.importFolder(url, prefix: prefix) }
        }
        .sheet(item: $choosingProgram) { game in programPicker(game) }
        .confirmationDialog(deleting.map { "Delete \($0.name)?" } ?? "", isPresented: deletingPresented,
                            titleVisibility: .visible, presenting: deleting) { game in
            Button("Delete the game's files", role: .destructive) { store.remove(game, files: true) }
            Button("Cancel", role: .cancel) {}
        } message: { game in
            Text("Removes Games/\(game.folder) from this iPhone. This cannot be undone.")
        }
    }

    private var deletingPresented: Binding<Bool> {
        Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })
    }

    private func row(_ game: CustomGame) -> some View {
        HStack(spacing: 12) {
            RoundedRectangle(cornerRadius: 8).fill(Deck.accent.opacity(0.25)).frame(width: 52, height: 52)
                .overlay(Image(systemName: "gamecontroller").foregroundStyle(Deck.accent))
            VStack(alignment: .leading, spacing: 2) {
                Text(game.name).font(.body.weight(.semibold))
                Text(game.exe).font(.caption).foregroundStyle(Deck.dim).lineLimit(1).truncationMode(.middle)
            }
            Spacer()
            Menu {
                Button("Choose the program…") { choosingProgram = game }
                Button("Remove from the list (keep files)") { store.remove(game, files: false) }
                Button("Delete the game's files…", role: .destructive) { deleting = game }
            } label: {
                Image(systemName: "ellipsis.circle").font(.title3).foregroundStyle(Deck.accent)
            }
            Button("Play") { onPlay(game) }
                .buttonStyle(DeckButtonStyle())
                .frame(width: 90)
                .disabled(playBlocker != nil)
        }
    }

    /// Every program in the folder, likeliest first, with its CPU: pick the one that starts the game.
    private func programPicker(_ game: CustomGame) -> some View {
        let programs = CustomGames.programs(in: game.folder)
        return NavigationStack {
            List(programs) { p in
                Button {
                    var g = game
                    g.exe = p.path
                    store.update(g)
                    choosingProgram = nil
                } label: {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(p.path).font(.body)
                            Text("\(p.kind), \(ByteCountFormatter.string(fromByteCount: Int64(p.size), countStyle: .file))")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if p.path == game.exe { Image(systemName: "checkmark") }
                    }
                }
            }
            .navigationTitle(game.name)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Close") { choosingProgram = nil } } }
        }
    }
}
