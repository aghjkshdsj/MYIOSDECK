// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

enum Deck {
    static let bg = Color(red: 0.055, green: 0.078, blue: 0.106)        // #0E141B
    static let panel = Color(red: 0.090, green: 0.122, blue: 0.161)     // #171F29
    static let panelHi = Color(red: 0.125, green: 0.165, blue: 0.212)   // #202A36
    static let accent = Color(red: 0.102, green: 0.624, blue: 1.0)      // #1A9FFF
    static let good = Color(red: 0.349, green: 0.749, blue: 0.251)      // #59BF40
    static let warn = Color(red: 0.957, green: 0.671, blue: 0.180)
    static let bad = Color(red: 0.902, green: 0.298, blue: 0.267)
    static let text = Color(white: 0.92)
    static let dim = Color(white: 0.58)
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
        .background(Deck.panel, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
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

struct DeckButtonStyle: ButtonStyle {
    var prominent = true
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.body.weight(.bold))
            .lineLimit(1).minimumScaleFactor(0.6)
            .padding(.horizontal, 12).padding(.vertical, 12)
            .frame(maxWidth: .infinity)
            .foregroundStyle(prominent ? Color.white : Deck.text)
            .background(prominent ? Deck.accent : Deck.panelHi, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            .opacity(configuration.isPressed ? 0.75 : 1)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
    }
}
