// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

struct PerformanceView: View {
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var device: DeviceInfo
    @State private var gpuLoad: Double = 48
    @State private var stats = FrameStats()
    @State private var stageOn = false

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                cpuCard
                gpuCard
                featuresCard
            }
            .padding(16)
            .frame(maxWidth: 900)
            .frame(maxWidth: .infinity)
        }
    }

    // MARK: CPU

    private var cpuCard: some View {
        DeckCard(title: "CPU: native ARM64 vs x86-64 through FEX", icon: "cpu") {
            Text("The same C kernels compiled twice: once for ARM64 (native) and once for x86-64, run through FEX. Efficiency = native time ÷ FEX time.")
                .font(.footnote).foregroundStyle(Deck.dim)
            HStack(spacing: 10) {
                Button("Run native + FEX") { engine.runBenchmarks(includeGuest: true) }
                    .buttonStyle(DeckButtonStyle())
                    .disabled(engine.busy != nil || engine.state != .ready)
                Button("Native only") { engine.runBenchmarks(includeGuest: false) }
                    .buttonStyle(DeckButtonStyle(prominent: false))
                    .disabled(engine.busy != nil)
            }
            if engine.state != .ready {
                Text("Enable JIT on Home to include FEX results.").font(.caption).foregroundStyle(Deck.warn)
            }
            if let busy = engine.busy { HStack { ProgressView(); Text(busy).foregroundStyle(Deck.dim) } }
            if !engine.bench.isEmpty {
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 8) {
                    GridRow {
                        Text("Kernel"); Text("Native"); Text("FEX"); Text("Efficiency")
                    }
                    .font(.caption.weight(.bold)).foregroundStyle(Deck.dim)
                    ForEach(engine.bench) { r in
                        GridRow {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(r.name).font(.body.weight(.semibold))
                                Text(r.what).font(.caption2).foregroundStyle(Deck.dim)
                            }
                            Text(ms(r.nativeNs)).monospacedDigit()
                            Text(r.guestNs.map(ms) ?? "–").monospacedDigit()
                            efficiencyLabel(r)
                        }
                    }
                }
                if let avg = averageEfficiency {
                    Text("Average: FEX runs x86-64 code at \(Int(avg.rounded()))% of native speed with the \(settings.fexPreset.title) preset.")
                        .font(.subheadline.weight(.semibold)).foregroundStyle(Deck.accent)
                }
            }
        }
    }

    private func efficiencyLabel(_ r: BenchResult) -> some View {
        HStack(spacing: 4) {
            if let e = r.efficiency {
                Text("\(Int(e.rounded()))%").monospacedDigit()
                    .foregroundStyle(e >= 70 ? Deck.good : e >= 40 ? Deck.warn : Deck.bad)
            } else {
                Text("–")
            }
            if r.checksumsMatch == false {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Deck.warn)
                    .accessibilityLabel("Checksum differs")
            }
        }
    }

    private var averageEfficiency: Double? {
        let e = engine.bench.compactMap(\.efficiency)
        return e.isEmpty ? nil : e.reduce(0, +) / Double(e.count)
    }

    private func ms(_ ns: UInt64) -> String { String(format: "%.0f ms", Double(ns) / 1e6) }

    // MARK: GPU

    private var gpuCard: some View {
        DeckCard(title: "GPU: Metal stage at \(settings.frameCap) Hz", icon: "display") {
            Text("A full-screen Metal scene with adjustable per-pixel cost, presented the way game frames will be: triple buffered, paced with ProMotion. Watch frame time and GPU time as you raise the load.")
                .font(.footnote).foregroundStyle(Deck.dim)
            Toggle("Run the stage", isOn: $stageOn).tint(Deck.accent)
            if stageOn {
                ZStack(alignment: .topLeading) {
                    MetalStage(iterations: UInt32(gpuLoad), frameCap: settings.frameCap) { stats = $0 }
                        .frame(height: 260)
                        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                    if settings.showHUD { hud }
                }
                HStack {
                    Text("GPU load").foregroundStyle(Deck.dim)
                    Slider(value: $gpuLoad, in: 4...256, step: 4).tint(Deck.accent)
                    Text("\(Int(gpuLoad))").monospacedDigit().frame(width: 40)
                }
            }
        }
    }

    private var hud: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(String(format: "%.0f FPS", stats.fps)).font(.headline.monospacedDigit())
            Text(String(format: "frame %.2f ms · worst %.1f", stats.frameMs, stats.worstMs))
            Text(String(format: "GPU %.2f ms", stats.gpuMs))
            Text("\(Int(stats.drawableSize.width))×\(Int(stats.drawableSize.height))")
        }
        .font(.caption.monospacedDigit())
        .padding(8)
        .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 8))
        .padding(8)
    }

    // MARK: features

    private var featuresCard: some View {
        DeckCard(title: "\(device.cpuBrand) features FEX uses", icon: "memorychip") {
            Text("\(device.machine) · \(device.cores) · \(String(format: "%.0f", device.memoryGB)) GB")
                .font(.footnote).foregroundStyle(Deck.dim)
            ForEach(device.features) { f in
                StatusRow(label: f.id, detail: f.why, level: f.on ? .good : .idle)
            }
        }
    }
}
