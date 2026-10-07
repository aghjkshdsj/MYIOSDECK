// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The game library. Until the Wine and Steam stages land, it holds the
/// built-in x86-64 programs and shows exactly what each stage still needs.
struct LibraryView: View {
    @EnvironmentObject private var engine: EngineController
    @EnvironmentObject private var settings: Settings
    @EnvironmentObject private var wine: WineController
    @EnvironmentObject private var jit: JITController
    @State private var noJITAction: (() -> Void)?
    @State private var surfaceTitle = ""
    @State private var showSurface = false
    @State private var showSteamSignIn = false
    @ObservedObject private var steam = SteamSignInModel.shared
    @ObservedObject private var steamLibrary = SteamLibrary.shared

    private struct Stage: Identifiable {
        let id: Int
        let title: String
        let detail: String
        let done: Bool
    }

    private let stages = [
        Stage(id: 1, title: "JIT + FEX x86-64 translation", detail: "StikDebug JIT pool, FEXCore tuned to the A17 Pro, benchmarks, plus a no-JIT interpreter. Verified on iPhone 15 Pro Max / iOS 27.", done: true),
        Stage(id: 2, title: "Wine ARM64EC (Windows)", detail: "Wine running natively on ARM64 in-process, wineserver as a thread, FEX as xtajit64.dll for x86-64 code. Verified on device (build 20).", done: true),
        Stage(id: 3, title: "Direct3D 9/10/11/12 on Metal", detail: "DXMT for D3D9-11 and Madeira's D3D12 converter. D3D11 and D3D12 cubes render at 60 fps on device (builds 21-22).", done: true),
        Stage(id: 4, title: "Steam", detail: "Steam sign-in (QR or password + Steam Guard), owned library, depot downloads, cloud saves, launch through Valve's Windows client.", done: false),
        Stage(id: 5, title: "Deck input", detail: "Bluetooth controllers as XInput, touch controls, keyboard and mouse.", done: false),
    ]

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                DeckCard(title: "Installed (built-in x86-64 programs)", icon: "square.grid.2x2.fill") {
                    programRow(name: "x86-64 Hello", detail: "Prints the CPU the guest sees (CPUID) and times a loop") {
                        gated(jit: { engine.runHello(mode: .jit) }, noJIT: { engine.runHello(mode: .interpreter) })
                    }
                    Divider().overlay(Deck.panelHi)
                    programRow(name: "CPU benchmark suite", detail: "Native vs FEX vs no-JIT, five kernels (also on Performance)") {
                        let preset = settings.fexPreset.title
                        gated(jit: { engine.runBenchmarks(fex: true, interpreter: settings.benchInterpreter, presetName: preset) },
                              noJIT: { engine.runBenchmarks(fex: false, interpreter: true, presetName: preset) })
                    }
                }
                windowsCard
                steamCard
                DeckCard(title: "Road to Steam games", icon: "map.fill") {
                    ForEach(stages) { s in
                        StatusRow(label: "Stage \(s.id): \(s.title)", detail: s.detail, level: s.done ? .good : .idle)
                    }
                }
            }
            .padding(16)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
        .jitGate(pending: $noJITAction)
        .fullScreenCover(isPresented: $showSurface) { gameSurface }
    }

    /// Full-screen surface for a Windows program's Direct3D window. The bar sits
    /// outside the surface because the Metal host view is above all other views.
    private var gameSurface: some View {
        VStack(spacing: 0) {
            HStack {
                Text(surfaceTitle).font(.headline)
                Spacer()
                Button("Close") { showSurface = false }
                    .buttonStyle(DeckButtonStyle())
                    .frame(width: 90)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 8)
            .background(Deck.panel)
            GameSurfaceView()
                .ignoresSafeArea(edges: [.bottom, .horizontal])
        }
        .background(Color.black)
    }

    /// Stage 4: Steam account (SwiftSteam, from Madeira). The owned library and
    /// downloads build on this sign-in.
    private var steamCard: some View {
        DeckCard(title: "Steam", icon: "gamecontroller.fill") {
            if let name = steam.accountName {
                HStack {
                    StatusRow(label: "Signed in as \(name)", detail: "Downloads and Play come next in stage 4.", level: .good)
                    Button("Sign out") { steam.signOut() }
                        .buttonStyle(DeckButtonStyle())
                        .frame(width: 110)
                }
                Divider().overlay(Deck.panelHi)
                SteamGamesGrid(library: steamLibrary)
            } else {
                Text("Sign in with your Steam account name and password (Steam Guard supported) or a QR code from the Steam app. The sign-in token stays in this device's Keychain; the password is never stored.")
                    .font(.subheadline).foregroundStyle(Deck.dim)
                Button("Sign in to Steam") { showSteamSignIn = true }
                    .buttonStyle(DeckButtonStyle())
            }
        }
        .sheet(isPresented: $showSteamSignIn) { SteamSignInView() }
        .onAppear { steamLibrary.start() }
    }

    private var windowsCard: some View {
        DeckCard(title: "Windows programs (Wine + Direct3D)", icon: "macwindow") {
            if !wine.linked {
                Text("Wine is being built for iOS in CI (Stage 2). Once it links, Windows x64 programs run here through Wine ARM64EC + FEX.")
                    .font(.subheadline).foregroundStyle(Deck.dim)
            } else {
                ForEach(WineController.programs) { p in
                    HStack(spacing: 12) {
                        RoundedRectangle(cornerRadius: 8).fill(Deck.accent.opacity(0.25)).frame(width: 52, height: 52)
                            .overlay(Image(systemName: "macwindow").foregroundStyle(Deck.accent))
                        VStack(alignment: .leading, spacing: 2) {
                            Text(p.title).font(.body.weight(.semibold))
                            Text(p.detail).font(.caption).foregroundStyle(Deck.dim)
                        }
                        Spacer()
                        Button("Play") {
                            wine.run(p)
                            if p.graphics, case .booting = wine.state {
                                surfaceTitle = p.title
                                showSurface = true
                            }
                        }
                            .buttonStyle(DeckButtonStyle())
                            .frame(width: 90)
                            .disabled(!jit.isReady)
                    }
                }
                if !surfaceTitle.isEmpty, !showSurface {
                    Button("Show \(surfaceTitle)") { showSurface = true }
                        .buttonStyle(DeckButtonStyle())
                }
                switch wine.state {
                case .idle:
                    Text(jit.isReady ? "Output appears in the Logs tab ([stdio] lines)." : "Wine needs JIT: enable it on Home first.")
                        .font(.caption).foregroundStyle(Deck.dim)
                case .booting(let t): HStack { ProgressView(); Text("Starting Wine for \(t)…").foregroundStyle(Deck.dim) }
                case .running(let t): StatusRow(label: "Running \(t)", detail: "Watch the Logs tab for its output.", level: .good)
                case .finished(let t, let how): StatusRow(label: "\(t) finished", detail: "\(how). Its output is in the Logs tab. Restart MYIOSDECK to run another Windows program.", level: how.contains("error") ? .warn : .good)
                case .failed(let why): StatusRow(label: "Wine failed", detail: why, level: .bad)
                }
            }
        }
    }

    private func gated(jit: @escaping () -> Void, noJIT: @escaping () -> Void) {
        if engine.state == .ready { jit() } else { noJITAction = noJIT }
    }

    private func programRow(name: String, detail: String, action: @escaping () -> Void) -> some View {
        HStack(spacing: 12) {
            RoundedRectangle(cornerRadius: 8).fill(Deck.accent.opacity(0.25)).frame(width: 52, height: 52)
                .overlay(Image(systemName: "cpu").foregroundStyle(Deck.accent))
            VStack(alignment: .leading, spacing: 2) {
                Text(name).font(.body.weight(.semibold))
                Text(detail).font(.caption).foregroundStyle(Deck.dim)
            }
            Spacer()
            Button("Play", action: action)
                .buttonStyle(DeckButtonStyle())
                .frame(width: 90)
                .disabled(engine.busy != nil)
        }
    }
}
