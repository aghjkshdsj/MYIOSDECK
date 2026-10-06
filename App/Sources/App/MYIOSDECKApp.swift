// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

@main
struct MYIOSDECKApp: App {
    @StateObject private var settings = Settings()
    @StateObject private var jit = JITController()
    @StateObject private var engine = EngineController()
    @StateObject private var device = DeviceInfo()
    @StateObject private var logs = LogStore.shared
    @Environment(\.scenePhase) private var phase

    init() {
        let info = Bundle.main.infoDictionary
        dlog("MYIOSDECK \(info?["CFBundleShortVersionString"] ?? "?") (\(info?["CFBundleVersion"] ?? "?")) " +
             "fex=\(String(cString: mid_engine_version())) pid=\(getpid()) " +
             "debuggable=\(mid_has_get_task_allow()) cs_flags=0x\(String(mid_cs_flags(), radix: 16))")
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(settings)
                .environmentObject(jit)
                .environmentObject(engine)
                .environmentObject(device)
                .environmentObject(logs)
                .preferredColorScheme(.dark)
                .onOpenURL { url in dlog("[app] opened by \(url.absoluteString)") }
                .onChange(of: phase) { _, newPhase in
                    guard newPhase == .active else { return }
                    device.refresh()
                    jit.resumeIfDebugged(poolMB: settings.jitPoolMB) { engine.start(settings: settings) }
                }
                .onAppear {
                    jit.resumeIfDebugged(poolMB: settings.jitPoolMB) { engine.start(settings: settings) }
                }
        }
    }
}
