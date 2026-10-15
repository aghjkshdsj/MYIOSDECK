// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Storage (mockup 4): how much the games take and how much is free, the installed games by
/// size, and temporary files that can be cleared without touching saves or games.
struct StorageView: View {
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var recent = RecentlyPlayed.shared
    @EnvironmentObject private var wine: WineController

    enum Sort: String, CaseIterable, Identifiable {
        case largest = "Largest first", name = "Name", recent = "Recently played"
        var id: String { rawValue }
    }

    @State private var sort = Sort.largest
    @State private var sizes: [String: Int64] = [:]
    @State private var tempBytes: Int64?
    @State private var clearing = false
    @State private var cleared: String?

    private var games: [LibraryGame] {
        let list = GameCatalog.installed(steam: steam, custom: custom)
        switch sort {
        case .largest: return list.sorted { (sizes[$0.id] ?? 0) > (sizes[$1.id] ?? 0) }
        case .name: return list.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        case .recent: return recent.sorted(list.sorted { $0.name < $1.name }) { $0.id }
        }
    }

    private var total: Int64 { sizes.values.reduce(0, +) }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            usage
            HStack {
                Text("Installed games").font(.title3.weight(.semibold))
                Spacer()
                Menu {
                    Picker("Sort", selection: $sort) { ForEach(Sort.allCases) { Text($0.rawValue).tag($0) } }
                } label: {
                    HStack(spacing: 8) { Text(sort.rawValue); Image(systemName: "chevron.down").font(.footnote.weight(.semibold)) }
                        .font(.subheadline).foregroundStyle(Glass.text)
                        .padding(.horizontal, 16).frame(height: 40).glassCapsule()
                }
                .accessibilityLabel("Sort: \(sort.rawValue)")
            }
            let list = games
            if list.isEmpty {
                Text("No games installed.").foregroundStyle(Glass.secondary)
            } else {
                GlassCard(padding: 12) {
                    ForEach(Array(list.enumerated()), id: \.element.id) { i, game in row(game, last: i == list.count - 1) }
                }
            }
            temporary
        }
        .task(id: GameCatalog.installed(steam: steam, custom: custom).map(\.id)) { await measure() }
    }

    private var usage: some View {
        let free = StorageInfo.available ?? 0
        let fraction = total + free > 0 ? Double(total) / Double(total + free) : 0
        return VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                Text("Games · \(bytesText(total))").font(.title3.weight(.semibold))
                Spacer()
                Text("\(bytesText(free)) available").font(.subheadline).foregroundStyle(Glass.secondary)
            }
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Capsule().fill(Glass.separator)
                    Capsule().fill(LinearGradient(colors: [Glass.primary, Glass.primaryDeep], startPoint: .top, endPoint: .bottom))
                        .frame(width: max(14, geo.size.width * fraction))
                        .shadow(color: Glass.primary.opacity(0.5), radius: 6)
                }
            }
            .frame(height: 22)
            .accessibilityElement()
            .accessibilityLabel("Games use \(bytesText(total)), \(bytesText(free)) available")
        }
        .padding(18)
        .glassPanel()
    }

    private func row(_ game: LibraryGame, last: Bool) -> some View {
        Button { ui.open(.game(game.id)) } label: {
            GlassRow(last: last) {
                ArtworkView(source: game.artwork, kind: .portrait, name: game.name, compactPlaceholder: true)
                    .frame(width: 58, height: 72)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                VStack(alignment: .leading, spacing: 6) {
                    Text(game.name).font(.headline).lineLimit(2)
                    SourceBadge(source: game.source)
                }
                Spacer(minLength: 8)
                Text(sizes[game.id].map { bytesText($0) } ?? "…").font(.headline).monospacedDigit()
                Image(systemName: "chevron.right").foregroundStyle(Glass.secondary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
    }

    private var temporary: some View {
        HStack(spacing: 14) {
            Image(systemName: "doc").font(.title2).frame(width: 52, height: 52).glassCircle()
            VStack(alignment: .leading, spacing: 4) {
                Text("Temporary files").font(.headline)
                Text(cleared ?? (wine.programsThisRun > 0 ? "Clearing temporary files keeps your saves. Wine's own temporary files can be cleared after MYIOSDECK restarts." : "Clearing temporary files keeps your saves.")).font(.footnote).foregroundStyle(Glass.secondary)
            }
            Spacer(minLength: 8)
            VStack(alignment: .trailing, spacing: 8) {
                Text(tempBytes.map { bytesText($0) } ?? "…").font(.subheadline).monospacedDigit()
                Button(clearing ? "Clearing…" : "Clear") { clear() }
                    .buttonStyle(PrimaryButtonStyle(compact: true))
                    .disabled(clearing || (tempBytes ?? 0) == 0 || wine.isPlaying || !steam.downloads.isEmpty || GameLauncher.shared.dockProgress != nil)
            }
        }
        .padding(16)
        .glassPanel()
        .overlay(alignment: .bottomLeading) {
            if wine.isPlaying {
                Text("Quit the running game to clear them.").font(.caption2).foregroundStyle(Glass.secondary).padding(.leading, 82).padding(.bottom, 4)
            }
        }
    }

    private func measure() async {
        let list = GameCatalog.installed(steam: steam, custom: custom)
        var out: [String: Int64] = [:]
        for game in list {
            switch game.kind {
            case .steam(let id):
                if let dock = steam.installed.first(where: { $0.id == id }) {
                    out[game.id] = SteamInstallFiles.sizeOnDisk(appID: id, steamApps: SteamLibrary.drive.appendingPathComponent(dock.library, isDirectory: true)) ?? 0
                }
            case .custom(let g):
                let folder = g.folder
                out[game.id] = await Task.detached(priority: .utility) { CustomGames.sizeOnDisk(folder) }.value
            }
        }
        sizes = out
        let started = wine.programsThisRun > 0
        tempBytes = await Task.detached(priority: .utility) { TemporaryFiles.size(wineStarted: started) }.value
    }

    private func clear() {
        clearing = true
        let started = wine.programsThisRun > 0
        Task {
            let freed = await Task.detached(priority: .utility) { TemporaryFiles.clear(wineStarted: started) }.value
            dlog("[storage] cleared temporary files: \(freed) bytes")
            ArtworkStore.shared.forgetMemory()
            cleared = "Cleared \(bytesText(freed)). Your saves and games were not touched."
            tempBytes = await Task.detached(priority: .utility) { TemporaryFiles.size(wineStarted: started) }.value
            clearing = false
        }
    }
}

/// Files that are safe to delete: the app's caches (downloaded artwork, URL caches, shader and
/// other caches iOS may purge anyway), its temporary folder, and Windows' temporary folders in
/// the Wine prefix. Never Documents (games, saves, save-backups), never the prefix's other folders.
enum TemporaryFiles {
    /// With `wineStarted` (Wine ran in this app run and keeps its server files, mappings and swap in
    /// the app's tmp and caches until MYIOSDECK closes) only the downloaded artwork.
    static func folders(wineStarted: Bool) -> [URL] {
        let fm = FileManager.default
        if wineStarted { return [ArtworkStore.cacheDir] }
        var out = [fm.urls(for: .cachesDirectory, in: .userDomainMask)[0], fm.temporaryDirectory]
        let driveC = fm.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("wine-prefix/drive_c", isDirectory: true)
        out.append(driveC.appendingPathComponent("windows/temp", isDirectory: true))
        let users = driveC.appendingPathComponent("users", isDirectory: true)
        for user in (try? fm.contentsOfDirectory(atPath: users.path)) ?? [] {
            out.append(users.appendingPathComponent(user).appendingPathComponent("AppData/Local/Temp", isDirectory: true))
            out.append(users.appendingPathComponent(user).appendingPathComponent("Temp", isDirectory: true))
        }
        return out
    }

    private static func walk(_ folder: URL, _ each: (URL, Int64) -> Void) {
        let keys: [URLResourceKey] = [.totalFileAllocatedSizeKey, .isRegularFileKey, .isSymbolicLinkKey]
        // Symbolic links are not followed (C:\Games links into Documents).
        guard let walker = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: keys, options: []) else { return }
        for case let url as URL in walker {
            let v = try? url.resourceValues(forKeys: Set(keys))
            if v?.isSymbolicLink == true { walker.skipDescendants(); continue }
            if v?.isRegularFile == true { each(url, Int64(v?.totalFileAllocatedSize ?? 0)) }
        }
    }

    static func size(wineStarted: Bool) -> Int64 {
        var total: Int64 = 0
        for f in folders(wineStarted: wineStarted) { walk(f) { _, n in total += n } }
        return total
    }

    /// Deletes what is inside each folder (the folders stay); returns the bytes freed.
    static func clear(wineStarted: Bool) -> Int64 {
        let fm = FileManager.default
        var freed: Int64 = 0
        for folder in folders(wineStarted: wineStarted) {
            for name in (try? fm.contentsOfDirectory(atPath: folder.path)) ?? [] {
                let item = folder.appendingPathComponent(name)
                var size: Int64 = 0
                var isDir: ObjCBool = false
                if fm.fileExists(atPath: item.path, isDirectory: &isDir), isDir.boolValue {
                    walk(item) { _, n in size += n }
                } else {
                    size = Int64((try? item.resourceValues(forKeys: [.totalFileAllocatedSizeKey]))?.totalFileAllocatedSize ?? 0)
                }
                if (try? fm.removeItem(at: item)) != nil { freed += size }
            }
        }
        return freed
    }
}
