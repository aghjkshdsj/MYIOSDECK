// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The colours of the older views (Steam Cloud, Performance, sign-in, crash reports), mapped onto
/// the Glass theme (GlassTheme.swift) so they follow the app's light or dark Theme.
enum Deck {
    static let bg = Glass.base
    static let panel = Glass.solidPanel
    static let panelHi = Glass.dynamic(dark: UIColor(white: 1, alpha: 0.12), light: UIColor(white: 0, alpha: 0.07))
    static let accent = Glass.primary
    static let good = Glass.good
    static let warn = Glass.warn
    static let bad = Glass.bad
    static let text = Glass.text
    static let dim = Glass.secondary
}

struct DeckCard<Content: View>: View {
    var title: String?
    var icon: String?
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let title {
                HStack(spacing: 8) {
                    if let icon { Image(systemName: icon).foregroundStyle(Deck.accent) }
                    Text(title.uppercased()).font(.caption.weight(.bold)).tracking(1.2).foregroundStyle(Deck.dim)
                }
            }
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassPanel(cornerRadius: 18)
    }
}

struct StatusRow: View {
    enum Level { case good, warn, bad, idle }
    let label: String
    let detail: String
    let level: Level

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: symbol).foregroundStyle(color).font(.body.weight(.semibold))
            VStack(alignment: .leading, spacing: 2) {
                Text(label).foregroundStyle(Deck.text).font(.body.weight(.semibold))
                Text(detail).foregroundStyle(Deck.dim).font(.footnote).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
    }

    private var symbol: String {
        switch level {
        case .good: return "checkmark.circle.fill"
        case .warn: return "exclamationmark.triangle.fill"
        case .bad: return "xmark.octagon.fill"
        case .idle: return "circle.dashed"
        }
    }

    private var color: Color {
        switch level {
        case .good: return Deck.good
        case .warn: return Deck.warn
        case .bad: return Deck.bad
        case .idle: return Deck.dim
        }
    }
}

/// The older views' buttons, drawn as the Glass primary (blue) or glass capsule buttons.
struct DeckButtonStyle: ButtonStyle {
    var prominent = true
    @ViewBuilder func makeBody(configuration: Configuration) -> some View {
        if prominent {
            PrimaryButtonBody(configuration: configuration, fullWidth: true, compact: true)
        } else {
            GlassButtonBody(configuration: configuration, fullWidth: true, compact: true)
        }
    }
}
