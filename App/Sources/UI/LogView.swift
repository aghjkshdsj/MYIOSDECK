// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

struct LogView: View {
    @EnvironmentObject private var logs: LogStore
    @State private var filter = ""

    private var shown: [String] {
        filter.isEmpty ? logs.lines : logs.lines.filter { $0.localizedCaseInsensitiveContains(filter) }
    }

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                TextField("Filter (jit, fex, bench…)", text: $filter)
                    .textFieldStyle(.roundedBorder)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                ShareLink(item: logs.fileURL) { Image(systemName: "square.and.arrow.up") }
                Button { logs.clear() } label: { Image(systemName: "trash") }
            }
            .padding(.horizontal, 16).padding(.top, 10)

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(shown.enumerated()), id: \.offset) { i, line in
                            Text(line).font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(color(for: line))
                                .textSelection(.enabled)
                                .id(i)
                        }
                    }
                    .padding(.horizontal, 16)
                }
                .onChange(of: logs.lines.count) { _, _ in
                    if filter.isEmpty, let last = shown.indices.last { proxy.scrollTo(last, anchor: .bottom) }
                }
            }
            Text("Full log: Files › On My iPhone › MYIOSDECK › myiosdeck-log.txt")
                .font(.caption2).foregroundStyle(Deck.dim).padding(.bottom, 6)
        }
    }

    private func color(for line: String) -> Color {
        if line.contains("FAIL") || line.contains("THROW") || line.contains(":E]") { return Deck.bad }
        if line.contains("[jit]") { return Deck.accent }
        if line.contains("[bench]") { return Deck.good }
        return Deck.text.opacity(0.85)
    }
}
