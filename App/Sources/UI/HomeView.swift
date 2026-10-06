// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

struct HomeView: View {
    @Binding var tab: DeckTab
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var jit: JITController
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var device: DeviceInfo

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                hero
                readiness
                if let run = engine.lastRun { lastRunCard(run) }
                aboutCard
            }
            .padding(16)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(headline).font(.title.weight(.heavy))
            Text(subline).foregroundStyle(Deck.dim)
            HStack(spacing: 10) {
                if !jit.isReady {
                    Button(jitButtonTitle) {
                        jit.enableWithStikDebug(poolMB: settings.jitPoolMB) { engine.start(settings: settings) }
                    }
                    .buttonStyle(DeckButtonStyle())
                    .disabled(jit.state == .waitingForDebugger || jit.state == .preparing)
                } else if engine.state == .ready {
                    Button("Run x86-64 test") { engine.runHello() }.buttonStyle(DeckButtonStyle())
                        .disabled(engine.busy != nil)
                    Button("Benchmark") { tab = .performance }.buttonStyle(DeckButtonStyle(prominent: false))
                } else {
                    Button("Start engine") { engine.start(settings: settings) }.buttonStyle(DeckButtonStyle())
                        .disabled(engine.state == .starting)
                }
            }
            if let busy = engine.busy {
                HStack { ProgressView(); Text(busy).foregroundStyle(Deck.dim) }
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            LinearGradient(colors: [Deck.accent.opacity(0.35), Deck.panel], startPoint: .topLeading, endPoint: .bottomTrailing),
            in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private var headline: String {
        if engine.state == .ready { return "Ready to translate" }
        if jit.isReady { return "JIT is on" }
        return "Welcome to MYIOSDECK"
    }

    private var subline: String {
        if engine.state == .ready {
            return "FEX is translating x86-64 to ARM64 on your \(device.cpuBrand). Wine and Steam come next (see Library)."
        }
        if jit.isReady { return "Starting FEXCore…" }
        return "Turn on JIT through StikDebug to start the x86 → ARM64 engine."
    }

    private var jitButtonTitle: String {
        switch jit.state {
        case .waitingForDebugger: return "Waiting for StikDebug…"
        case .preparing: return "Preparing JIT memory…"
        default: return "Enable JIT"
        }
    }

    private var readiness: some View {
        DeckCard(title: "Ready to play checklist", icon: "checklist") {
            StatusRow(label: "Debuggable signature",
                      detail: jit.hasGetTaskAllow ? "get-task-allow present: StikDebug can attach"
                                                  : "Missing: re-sideload the IPA with iloader / SideStore",
                      level: jit.hasGetTaskAllow ? .good : .bad)
            StatusRow(label: "JIT", detail: jitDetail, level: jitLevel)
            StatusRow(label: "Memory+", detail: memoryDetail, level: device.hasIncreasedMemoryLimit ? .good : .warn)
            StatusRow(label: "FEX engine", detail: engineDetail, level: engineLevel)
            StatusRow(label: "Controller",
                      detail: device.controllers.isEmpty ? "None connected (Bluetooth Xbox / DualSense recommended)"
                                                         : device.controllers.joined(separator: ", "),
                      level: device.controllers.isEmpty ? .idle : .good)
        }
    }

    private var jitDetail: String {
        switch jit.state {
        case .off: return "Off. Needs StikDebug + LocalDevVPN (see docs/INSTALL.md)."
        case .waitingForDebugger: return "Waiting for StikDebug to attach…"
        case .preparing: return "Asking the debugger for executable memory…"
        case .ready(let mb): return "\(mb) MB JIT pool ready, self-test passed (\(jit.selfTest))"
        case .failed(let why): return why
        }
    }

    private var jitLevel: StatusRow.Level {
        switch jit.state {
        case .ready: return .good
        case .failed: return .bad
        case .off: return .idle
        default: return .warn
        }
    }

    private var memoryDetail: String {
        "\(device.availableMB) MB available to the app of \(String(format: "%.0f", device.memoryGB)) GB" +
            (device.hasIncreasedMemoryLimit ? " (increased limit active)" : " (increased-memory-limit not granted)")
    }

    private var engineDetail: String {
        if !engine.linked { return "Not in this build (UI-only)" }
        switch engine.state {
        case .notStarted: return "Waiting for JIT. FEX \(engine.fexVersion.prefix(10))"
        case .starting: return "Starting FEXCore…"
        case .ready: return "FEXCore \(engine.fexVersion.prefix(10)), preset \(settings.fexPreset.title)"
        case .failed(let why): return why
        }
    }

    private var engineLevel: StatusRow.Level {
        switch engine.state {
        case .ready: return .good
        case .failed: return .bad
        case .starting: return .warn
        case .notStarted: return .idle
        }
    }

    private func lastRunCard(_ run: GuestRun) -> some View {
        DeckCard(title: "Last x86-64 program", icon: "terminal.fill") {
            Text(run.output.isEmpty ? run.error : run.output)
                .font(.system(.footnote, design: .monospaced))
                .textSelection(.enabled)
            Text("exit \(run.exitCode) · \(String(format: "%.1f", run.seconds * 1000)) ms incl. translation · \(run.syscalls) syscalls · \(run.poolUsedKB) KB of ARM64 code generated")
                .font(.caption).foregroundStyle(Deck.dim)
        }
    }

    private var aboutCard: some View {
        DeckCard(title: "How it works", icon: "cpu") {
            Text("iOS has no Linux kernel and cannot start other programs, so SteamOS itself cannot run here. MYIOSDECK runs the same stack Valve's ARM64 Proton uses, rebuilt for iOS in one process: FEX translates x86 game code to ARM64, Wine ARM64EC provides Windows natively, and Direct3D draws straight to Metal.")
                .foregroundStyle(Deck.dim).font(.subheadline)
        }
    }
}
