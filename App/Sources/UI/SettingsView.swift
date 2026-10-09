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
                    Toggle("Verbose FEX log", isOn: $settings.verboseFEXLog).tint(Deck.accent)
                }

                DeckCard(title: "Without JIT", icon: "lock.shield") {
                    Toggle("Use FXR for x86-64 Hello (experimental)", isOn: $settings.useFXR).tint(Deck.accent)
                    Text("FXR is FXI with the x86 registers kept in ARM registers (\(engine.fxrPinned ? "pinned in this build" : "NOT pinned in this build")), about 2-3× FXI's speed. The Performance tab benchmark measures both.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                    Toggle("Windows games: FXR (experimental)", isOn: $settings.winFXR).tint(Deck.accent)
                    Text("Runs a Windows game's x64 code with FXR instead of FXI. Faults (x64 exceptions) are exact; a debugger or garbage collector reading another thread's registers is not supported yet. Applies to the next game you start without JIT.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                    Toggle("Windows games without audio", isOn: $settings.noJITMuteAudio).tint(Deck.accent)
                    Text("Without JIT a game's audio engine decodes and mixes sound in the interpreter, and that can take a whole CPU core (Stick Fight's menu music did). Off: the game starts with no audio device. Applies to the next game you start without JIT.")
                        .font(.footnote).foregroundStyle(Deck.dim)
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

                DeckCard(title: "Controller", icon: "gamecontroller.fill") {
                    Picker("Controller API", selection: $settings.controllerAPI) {
                        Text("XInput (Xbox controller)").tag("xinput")
                        Text("XInput + DirectInput").tag("dinput")
                        Text("HID gamepad").tag("hid")
                        Text("HID DualSense (PS5)").tag("dualsense")
                        Text("Keyboard and mouse").tag("keyboard")
                    }
                    .pickerStyle(.menu)
                    Text("How Windows games see your controller; takes effect the next time a game starts. XInput suits most games. If a game ignores the controller, try DirectInput (older games), HID or DualSense (engines that look for devices, such as Unity's old input system), or Keyboard and mouse, which works with any game that takes keys: A Space, B Ctrl, X E, Y R, LB Q, RB F, L3 Shift, R3 C, Menu Esc, Options Enter, D-pad arrows, left stick WASD, RT left click, LT right click.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                }

                DeckCard(title: "About", icon: "info.circle") {
                    let info = Bundle.main.infoDictionary
                    Text("MYIOSDECK \(info?["CFBundleShortVersionString"] as? String ?? "?") (build \(info?["CFBundleVersion"] as? String ?? "?"))")
                    Text("FEX: \(engine.fexVersion)").font(.footnote.monospaced()).foregroundStyle(Deck.dim)
                    Text("Interpreter (Blink): \(engine.interpVersion)").font(.footnote.monospaced()).foregroundStyle(Deck.dim)
                    Text("GPL-3.0-or-later. Built on FEX-Emu, Madeira's iOS ports of FEX, Wine and DXMT, Blink (no-JIT interpreter), StikDebug's JIT protocol, and the DroidDeck design.")
                        .font(.footnote).foregroundStyle(Deck.dim)
                }
            }
            .padding(16)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
    }
}
