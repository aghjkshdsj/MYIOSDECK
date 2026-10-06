// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

enum DeckTab: String, CaseIterable, Identifiable {
    case home = "Home", library = "Library", performance = "Performance", settings = "Settings", logs = "Logs"
    var id: String { rawValue }
    var icon: String {
        switch self {
        case .home: return "house.fill"
        case .library: return "square.grid.2x2.fill"
        case .performance: return "speedometer"
        case .settings: return "gearshape.fill"
        case .logs: return "text.alignleft"
        }
    }
}

struct RootView: View {
    @State private var tab: DeckTab = .home

    var body: some View {
        VStack(spacing: 0) {
            topBar
            Group {
                switch tab {
                case .home: HomeView(tab: $tab)
                case .library: LibraryView()
                case .performance: PerformanceView()
                case .settings: SettingsView()
                case .logs: LogView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Deck.bg.ignoresSafeArea())
        .foregroundStyle(Deck.text)
    }

    private var topBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                Text("MYIOSDECK").font(.headline.weight(.black)).tracking(1.5).foregroundStyle(Deck.accent)
                    .padding(.trailing, 10)
                ForEach(DeckTab.allCases) { t in
                    Button {
                        tab = t
                    } label: {
                        Label(t.rawValue, systemImage: t.icon)
                            .font(.subheadline.weight(.semibold))
                            .padding(.horizontal, 12).padding(.vertical, 8)
                            .background(tab == t ? Deck.accent.opacity(0.22) : .clear, in: Capsule())
                            .foregroundStyle(tab == t ? Deck.accent : Deck.dim)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 16).padding(.vertical, 10)
        }
        .background(Deck.panel.opacity(0.6))
    }
}
