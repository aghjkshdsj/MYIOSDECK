// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var jit: JITController

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                DeckCard(title: "FEX preset", icon: "dial.high.fill") {
                    Picker("Preset", selection: $settings.fexPreset) {
                        ForEach(FEXPreset.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text(settings.fexPreset.detail).font(.footnote).foregroundStyle(Deck.dim)
                    Toggle("Multiblock compilation", isOn: $settings.multiblock).tint(Deck.accent)
                    Stepper("Max instructions per block: \(settings.maxInst)", value: $settings.maxInst, in: 500...10000, step: 500)
                    Button("Apply (restart engine)") { engine.start(settings: settings) }
                        .buttonStyle(DeckButtonStyle(prominent: false))
                        .disabled(!jit.isReady || engine.state == .starting)
                    Text("Each restart uses a little of the JIT pool; restart MYIOSDECK if it runs low.")
                        .font(.caption).foregroundStyle(Deck.dim)
                }

                DeckCard(title: "JIT", icon: "bolt.fill") {
                    Picker("JIT pool", selection: $settings.jitPoolMB) {
                        Text("256 MB").tag(256)
                        Text("512 MB").tag(512)
                        Text("1 GB").tag(1024)
                    }
                    .pickerStyle(.segmented)
                    Text("Memory the debugger makes executable at JIT time. Bigger fits bigger games' translated code; it applies the next time JIT is enabled.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                }

                DeckCard(title: "Display", icon: "display") {
                    Picker("Frame cap", selection: $settings.frameCap) {
                        Text("30").tag(30)
                        Text("40").tag(40)
                        Text("60").tag(60)
                        Text("120").tag(120)
                    }
                    .pickerStyle(.segmented)
                    Text("ProMotion paces presents evenly at the cap. 40 Hz is a good battery/smoothness middle ground, as on the Steam Deck.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                    Toggle("Performance overlay", isOn: $settings.showHUD).tint(Deck.accent)
                }

                DeckCard(title: "About", icon: "info.circle") {
                    let info = Bundle.main.infoDictionary
                    Text("MYIOSDECK \(info?["CFBundleShortVersionString"] as? String ?? "?") (build \(info?["CFBundleVersion"] as? String ?? "?"))")
                    Text("FEX: \(engine.fexVersion)").font(.footnote.monospaced()).foregroundStyle(Deck.dim)
                    Text("GPL-3.0-or-later. Built on FEX-Emu, Madeira's iOS ports of FEX, Wine and DXMT, StikDebug's JIT protocol, and the DroidDeck design.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                }
            }
            .padding(16)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
    }
}
