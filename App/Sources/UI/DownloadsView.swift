// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// Downloads: Steam installs (running, waiting, failed), Valve's Steam components for With Steam,
/// and game folders being copied in. Installs go on while the rest of the app is used; iOS pauses
/// them in the background, and Install picks up where it stopped.
struct DownloadsView: View {
    @ObservedObject private var ui = UIState.shared
    @ObservedObject private var steam = SteamLibrary.shared
    @ObservedObject private var custom = CustomGames.shared
    @ObservedObject private var launcher = GameLauncher.shared
    @Environment(\.isLandscape) private var landscape

    private var entries: [(id: Int, state: SteamLibrary.DownloadState)] {
        steam.downloads.map { ($0.key, $0.value) }.sorted { a, b in
            func rank(_ s: SteamLibrary.DownloadState) -> Int {
                switch s { case .active: return 0; case .queued: return 1; case .failed: return 2 }
            }
            return rank(a.state) != rank(b.state) ? rank(a.state) < rank(b.state) : a.id < b.id
        }
    }

    var body: some View {
        PadScrollView {
            VStack(alignment: .leading, spacing: 14) {
                if let dock = launcher.dockProgress {
                    other(icon: "cloud.fill", title: "Steam components", detail: dock)
                }
                if let copying = custom.copying {
                    other(icon: "folder.fill", title: "Game folder", detail: copying, busy: copying.hasPrefix("Copying"))
                }
                if entries.isEmpty && launcher.dockProgress == nil && custom.copying == nil {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("No downloads").font(.title3.weight(.semibold))
                        Text("Games you install from your Steam library appear here. You can keep browsing while they download; keep MYIOSDECK open, as iOS pauses downloads in the background (Install resumes them).")
                            .font(.footnote).foregroundStyle(Glass.secondary)
                        Button { ui.show(.library) } label: { Label("Browse your library", systemImage: "square.grid.2x2.fill") }
                            .buttonStyle(GlassButtonStyle(compact: true))
                            .padFocus("downloads.browse", radius: 18, preferred: true) { ui.show(.library) }
                            .padding(.top, 4)
                    }
                    .padding(18)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .glassPanel()
                }
                ForEach(entries, id: \.id) { entry in row(entry.id, entry.state) }
            }
            .padding(.horizontal, landscape ? 10 : 20)
            .padding(.bottom, 24)
            .frame(maxWidth: 900)
            .frame(maxWidth: .infinity)
        }
        .scrollIndicators(.hidden)
    }

    private func name(_ id: Int) -> String { steam.game(id)?.name ?? "App \(id)" }

    private func row(_ id: Int, _ state: SteamLibrary.DownloadState) -> some View {
        HStack(spacing: 14) {
            Button { ui.open(.game(RecentlyPlayed.steam(id))) } label: {
                ArtworkView(source: .steam(id), kind: .wide, name: name(id), compactPlaceholder: true)
                    .frame(width: landscape ? 150 : 104, height: landscape ? 70 : 49)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("\(name(id)) page")
            .padFocus("downloads.art.\(id)", radius: 10) { ui.open(.game(RecentlyPlayed.steam(id))) }
            VStack(alignment: .leading, spacing: 6) {
                Text(name(id)).font(.headline).lineLimit(1)
                switch state {
                case .queued:
                    Text("Waiting: another download is running").font(.footnote).foregroundStyle(Glass.secondary)
                case .active(let p):
                    ProgressView(value: p.fraction).tint(Glass.primary)
                    Text(Self.progressText(p)).font(.footnote).foregroundStyle(Glass.secondary).monospacedDigit()
                case .failed(let why):
                    Text(why).font(.footnote).foregroundStyle(Glass.bad).lineLimit(3)
                }
            }
            Spacer(minLength: 8)
            switch state {
            case .failed:
                Button("Retry") { steam.install(id) }.buttonStyle(PrimaryButtonStyle(compact: true))
                    .padFocus("downloads.retry.\(id)", radius: 20) { steam.install(id) }
            default:
                Button("Cancel") { steam.cancel(id) }.buttonStyle(GlassButtonStyle(compact: true))
                    .padFocus("downloads.cancel.\(id)", radius: 18) { steam.cancel(id) }
            }
        }
        .padding(14)
        .glassPanel()
    }

    private func other(icon: String, title: String, detail: String, busy: Bool = true) -> some View {
        HStack(spacing: 14) {
            Image(systemName: icon).font(.title2).frame(width: 44)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                Text(detail).font(.footnote).foregroundStyle(Glass.secondary)
            }
            Spacer()
            if busy { ProgressView() }
        }
        .padding(14)
        .glassPanel()
    }

    static func progressText(_ p: SteamDownloadProgress) -> String {
        switch p.phase {
        case .preparing: return "Preparing (reading Steam's manifests)…"
        case .finishing: return "Finishing…"
        case .downloading:
            let speed = p.bytesPerSecond > 0 ? " · \(bytesText(UInt64(p.bytesPerSecond)))/s" : ""
            return "\(bytesText(p.doneBytes)) of \(bytesText(p.totalBytes))\(speed)"
        }
    }
}
