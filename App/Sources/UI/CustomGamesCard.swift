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
    @State private var deletingInstaller: InstallerCleanup?

    var body: some View {
        DeckCard(title: "My games (DRM-free)", icon: "folder.fill") {
            Text("Put an installed game's folder (GOG, itch.io, or copied from a PC) into Files › On My iPhone › MYIOSDECK › Games, or add one from anywhere in Files. Wine sees it as C:\\Games. 64-bit games run with or without JIT; 32-bit games need JIT.")
                .font(.subheadline).foregroundStyle(Deck.dim)
            Text("GOG offline installer? Put setup_<game>.exe and all its setup_<game>-1.bin, -2.bin… files into one folder in Games and tap Unpack. Add-ons and patches go into the game's own folder.")
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
            if let message = store.unpackMessage {
                Text(message).font(.subheadline).foregroundStyle(Deck.warn)
            }
            // Its own container: the installer dialog must not share a view with the delete-game one.
            VStack(alignment: .leading, spacing: 12) {
                ForEach(store.installers) { installer in
                    Divider().overlay(Deck.panelHi)
                    installerRow(installer)
                }
            }
            .confirmationDialog("Delete the installer files?", isPresented: cleanupPresented, titleVisibility: .visible,
                                presenting: store.cleanup ?? deletingInstaller) { offer in
                Button("Delete \(offer.files.count) installer file\(offer.files.count == 1 ? "" : "s")", role: .destructive) {
                    store.deleteInstaller(offer.installer, prefix: prefix)
                    store.cleanup = nil
                    deletingInstaller = nil
                }
                Button("Keep them", role: .cancel) {
                    store.cleanup = nil
                    deletingInstaller = nil
                }
            } message: { offer in
                Text("The game is in Games/\(offer.destination). The installer (\(offer.files.joined(separator: ", "))) takes \(CustomGames.size(offer.bytes)) in Games/\(offer.installer.folder) and is not needed to play. This cannot be undone.")
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

    private var cleanupPresented: Binding<Bool> {
        Binding(get: { store.cleanup != nil || deletingInstaller != nil },
                set: { if !$0 { store.cleanup = nil; deletingInstaller = nil } })
    }

    /// A GOG installer in Games: what it is, whether it can be unpacked, and the unpack itself.
    private func installerRow(_ installer: GameInstaller) -> some View {
        let info = store.installerInfo[installer.id]
        let running = store.unpacking == installer.id
        let done = store.unpacked[installer.id]
        let dest = store.destination(for: installer)
        let problem = info?.problem
        let ready = info != nil && problem == nil && dest != nil
        let files = CustomGames.installerFiles(installer, info: info)
        let detail: String
        if let done {
            detail = "Unpacked into Games/\(done)."
        } else if info == nil {
            detail = "Reading the installer…"
        } else if let problem {
            detail = problem
        } else if let dest {
            let size = CustomGames.size(info?.appSize ?? 0)
            detail = dest == installer.folder && store.games.contains(where: { $0.folder == installer.folder })
                ? "Add-on or patch: unpacks \(info?.appFiles ?? 0) files (\(size)) into this game's folder."
                : "GOG installer: unpacks \(info?.appFiles ?? 0) files (\(size)) into Games/\(dest). Keep MYIOSDECK open while it runs."
        } else {
            detail = ""
        }
        return VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                RoundedRectangle(cornerRadius: 8).fill(Deck.panelHi).frame(width: 52, height: 52)
                    .overlay(Image(systemName: "shippingbox").foregroundStyle(Deck.accent))
                VStack(alignment: .leading, spacing: 2) {
                    Text(info?.appName ?? installer.exe).font(.body.weight(.semibold))
                    Text("\(installer.folder)/\(installer.exe)").font(.caption).foregroundStyle(Deck.dim)
                        .lineLimit(1).truncationMode(.middle)
                }
                Spacer()
                Menu {
                    if done != nil && ready {
                        Button("Unpack again") { store.unpack(installer, prefix: prefix) }
                    }
                    Button("Delete installer files…", role: .destructive) {
                        deletingInstaller = InstallerCleanup(
                            installer: installer, destination: done ?? dest ?? installer.folder, files: files,
                            bytes: files.reduce(0) { $0 + CustomGames.fileSize(installer.folder, $1) })
                    }
                } label: {
                    Image(systemName: "ellipsis.circle").font(.title3).foregroundStyle(Deck.accent)
                }
                .disabled(running)
                if done == nil && !running {
                    Button("Unpack") { store.unpack(installer, prefix: prefix) }
                        .buttonStyle(DeckButtonStyle())
                        .frame(width: 100)
                        .disabled(!ready || store.unpacking != nil)
                }
            }
            if running {
                ProgressView(value: store.unpackFraction)
                HStack {
                    Text(store.unpackDetail).font(.caption).foregroundStyle(Deck.dim)
                    Spacer()
                    Button("Cancel") { store.cancelUnpack() }.font(.caption)
                }
            } else if !detail.isEmpty {
                Text(detail).font(.caption).foregroundStyle(problem != nil && done == nil ? Deck.warn : Deck.dim)
            }
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
