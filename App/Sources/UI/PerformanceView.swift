// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

struct PerformanceView: View {
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var device: DeviceInfo
    @State private var gpuLoad: Double = 48
    @State private var stats = FrameStats()
    @State private var stageOn = false
    @State private var noJITAction: (() -> Void)?

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
        .jitGate(pending: $noJITAction)
    }

    private func runBench() {
        let preset = settings.fexPreset.title
        if engine.state == .ready {
            engine.runBenchmarks(fex: true, interpreter: settings.benchInterpreter, presetName: preset)
        } else {
            noJITAction = { engine.runBenchmarks(fex: false, interpreter: true, presetName: preset) }
        }
    }

    // MARK: CPU

    private var cpuCard: some View {
        DeckCard(title: "CPU: native ARM64 vs x86-64", icon: "cpu") {
            Text("The same C kernels compiled twice: once for ARM64 (native) and once for x86-64. The x86-64 build runs through FEX (JIT) and, optionally, the no-JIT interpreter. % = share of native speed.")
                .font(.footnote).foregroundStyle(Deck.dim)
            Toggle("Also measure without JIT (interpreter, 1/\(EngineController.interpreterScaleDivisor) workload)",
                   isOn: $settings.benchInterpreter)
                .tint(Deck.accent).font(.subheadline)
                .disabled(!engine.interpreterLinked)
            HStack(spacing: 10) {
                Button("Run benchmark", action: runBench)
                    .buttonStyle(DeckButtonStyle())
                    .disabled(engine.busy != nil)
                if let url = engine.lastReportURL {
                    ShareLink(item: url) { Label("Share report", systemImage: "square.and.arrow.up") }
                        .buttonStyle(DeckButtonStyle(prominent: false))
                }
            }
            if engine.state != .ready {
                Text("JIT is off: FEX results need JIT. You can still measure the interpreter.")
                    .font(.caption).foregroundStyle(Deck.warn)
            }
            if let busy = engine.busy { HStack { ProgressView(); Text(busy).foregroundStyle(Deck.dim) } }
            if !engine.bench.isEmpty {
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 8) {
                    GridRow {
                        Text("Kernel"); Text("Native"); Text("FEX JIT"); Text("No JIT")
                    }
                    .font(.caption.weight(.bold)).foregroundStyle(Deck.dim)
                    ForEach(engine.bench) { r in
                        GridRow {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(r.name).font(.body.weight(.semibold))
                                Text(r.what).font(.caption2).foregroundStyle(Deck.dim)
                            }
                            Text(ms(r.nativeNs)).monospacedDigit()
                            pct(r.fexEfficiency, time: r.fexNs, match: r.fexMatch, digits: 0)
                            pct(r.interpEfficiency, time: r.interpNs, match: r.interpMatch, digits: 1)
                        }
                    }
                }
                if let avg = EngineController.average(engine.bench.compactMap(\.fexEfficiency)) {
                    Text("FEX JIT: \(Int(avg.rounded()))% of native speed (\(settings.fexPreset.title) preset).")
                        .font(.subheadline.weight(.semibold)).foregroundStyle(Deck.accent)
                }
                if let avg = EngineController.average(engine.bench.compactMap(\.interpEfficiency)) {
                    Text(String(format: "No JIT (interpreter): %.1f%% of native, about %.0f× slower.", avg, 100 / max(avg, 0.001)))
                        .font(.subheadline.weight(.semibold)).foregroundStyle(Deck.warn)
                }
                Text("The full report is in the Logs tab and in Files › MYIOSDECK › benchmarks.")
                    .font(.caption).foregroundStyle(Deck.dim)
            }
        }
    }

    private func pct(_ e: Double?, time: UInt64?, match: Bool?, digits: Int) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            if let e {
                HStack(spacing: 3) {
                    Text(String(format: "%.\(digits)f%%", e)).monospacedDigit()
                        .foregroundStyle(e >= 70 ? Deck.good : e >= 20 ? Deck.warn : Deck.bad)
                    if match == false {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Deck.warn)
                            .accessibilityLabel("Checksum differs")
                    }
                }
                if let time { Text(ms(time)).font(.caption2).foregroundStyle(Deck.dim).monospacedDigit() }
            } else {
                Text("–").foregroundStyle(Deck.dim)
            }
        }
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
