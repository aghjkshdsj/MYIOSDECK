// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import UIKit

/// Gets executable memory on iOS 26/27 through StikDebug.
///
/// 1. MYIOSDECK opens stikdebug://enable-jit with its PID and our script.
/// 2. StikDebug attaches (CS_DEBUGGED turns on) and runs the script.
/// 3. We ask the script, via BRK #0xf00d, for one large RX region (the JIT pool),
///    alias it RW, then tell the script to detach. The pool stays executable.
final class JITController: ObservableObject, @unchecked Sendable {
    enum State: Equatable {
        case off
        case waitingForDebugger
        case preparing
        case ready(poolMB: Int)
        case failed(String)
    }

    @Published private(set) var state: State = .off
    @Published private(set) var selfTest: Int64 = 0

    var isReady: Bool { if case .ready = state { return true } else { return false } }
    var hasGetTaskAllow: Bool { mid_has_get_task_allow() }
    var stikDebugInstalled: Bool {
        guard let url = URL(string: "stikdebug://") else { return false }
        return UIApplication.shared.canOpenURL(url)
    }

    private let worker = DispatchQueue(label: "myiosdeck.jit", qos: .userInitiated)
    private var waiting = false

    /// Called at launch and whenever the app becomes active: if a debugger is
    /// already attached (JIT was started from StikDebug), finish the setup.
    func resumeIfDebugged(poolMB: Int, onReady: @escaping () -> Void) {
        guard !isReady, !waiting, mid_jit_debugged() else { return }
        dlog("[jit] CS_DEBUGGED is set; creating the JIT pool")
        worker.async { self.createPool(poolMB: poolMB, onReady: onReady) }
    }

    func enableWithStikDebug(poolMB: Int, onReady: @escaping () -> Void) {
        guard !isReady else { return }
        if mid_jit_debugged() {
            worker.async { self.createPool(poolMB: poolMB, onReady: onReady) }
            return
        }
        guard hasGetTaskAllow else {
            set(.failed("This install is not debuggable (no get-task-allow). Sideload the IPA with a development signature: iloader, SideStore and AltStore all do this."))
            return
        }
        guard let url = stikDebugURL() else {
            set(.failed("Could not build the StikDebug link"))
            return
        }
        dlog("[jit] opening StikDebug for pid \(getpid())")
        set(.waitingForDebugger)
        UIApplication.shared.open(url) { opened in
            if !opened {
                self.set(.failed("StikDebug is not installed. Install StikDebug, import your pairing file, connect LocalDevVPN, then try again."))
                return
            }
            self.waitForDebugger(poolMB: poolMB, onReady: onReady)
        }
    }

    private func stikDebugURL() -> URL? {
        guard let scriptURL = Bundle.main.url(forResource: "myiosdeck-jit", withExtension: "js"),
              let script = try? Data(contentsOf: scriptURL) else { return nil }
        var c = URLComponents()
        c.scheme = "stikdebug"
        c.host = "enable-jit"
        c.queryItems = [
            URLQueryItem(name: "bundle-id", value: Bundle.main.bundleIdentifier ?? ""),
            URLQueryItem(name: "pid", value: String(getpid())),
            URLQueryItem(name: "script-data", value: script.base64EncodedString()),
        ]
        // URLComponents leaves '+' alone, but a query decoder reads it as a space.
        c.percentEncodedQuery = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B")
        return c.url
    }

    /// Polls CS_DEBUGGED for up to 120 s. A background task keeps us running
    /// while StikDebug is in front. Called on the main thread (open's completion).
    private func waitForDebugger(poolMB: Int, onReady: @escaping () -> Void) {
        waiting = true
        var task: UIBackgroundTaskIdentifier = .invalid
        task = UIApplication.shared.beginBackgroundTask(withName: "myiosdeck.jit") {
            UIApplication.shared.endBackgroundTask(task)
            task = .invalid
        }
        worker.async {
            defer {
                self.waiting = false
                DispatchQueue.main.async { UIApplication.shared.endBackgroundTask(task) }
            }
            let deadline = Date().addingTimeInterval(120)
            while !mid_jit_debugged() {
                if Date() > deadline {
                    self.set(.failed("StikDebug did not attach within 2 minutes. Check that LocalDevVPN is connected and the pairing file is current."))
                    return
                }
                usleep(200_000)
            }
            dlog("[jit] debugger attached")
            self.createPool(poolMB: poolMB, onReady: onReady)
        }
    }

    private func createPool(poolMB: Int, onReady: @escaping () -> Void) {
        set(.preparing)
        var err = [CChar](repeating: 0, count: 512)
        let ok = mid_jit_pool_create(size_t(poolMB) << 20, &err, err.count)
        guard ok else {
            set(.failed(String(cString: err)))
            return
        }
        mid_jit26_detach()
        let result = mid_jit_selftest()
        dlog("[jit] self-test returned \(result) (expect 42)")
        DispatchQueue.main.async {
            self.selfTest = result
            if result == 42 {
                self.state = .ready(poolMB: Int(mid_jit_pool_size() >> 20))
                onReady()
            } else {
                self.state = .failed("JIT pool was created but code did not execute (self-test \(result)).")
            }
        }
    }

    private func set(_ s: State) {
        if case .failed(let why) = s { dlog("[jit] FAILED: \(why)") }
        DispatchQueue.main.async { self.state = s }
    }
}
