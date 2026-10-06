// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Stage 2: runs Windows x86-64 programs through Wine ARM64EC + FEX (xtajit64)
/// in-process, using a slice of the JIT pool. One Wine session per app run.
final class WineController: ObservableObject, @unchecked Sendable {
    enum State: Equatable { case idle, booting(String), running(String), failed(String) }

    struct Program: Identifiable {
        let id: String      // file name in the DLL farm (C:\windows\system32)
        let title: String
        let detail: String
    }

    /// Madeira's test programs, shipped in the ARM64EC DLL farm.
    static let programs = [
        Program(id: "hello-x64.exe", title: "Windows Hello (x64)", detail: "Console hello world through Wine + FEX ARM64EC"),
        Program(id: "fib-x64.exe", title: "Fibonacci (x64)", detail: "Recursive CPU test: x86-64 call/ret through FEX"),
        Program(id: "clocktest-x64.exe", title: "Clock test (x64)", detail: "Windows timers and QueryPerformanceCounter"),
    ]

    @Published private(set) var state: State = .idle
    let linked = mid_wine_linked()

    private let queue = DispatchQueue(label: "myiosdeck.wine", qos: .userInitiated)

    var prefixURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("wine-prefix")
    }

    func run(_ program: Program) {
        guard linked else {
            state = .failed("This build does not include Wine yet.")
            return
        }
        if case .running = state {
            state = .failed("Wine is already running a program. Restart MYIOSDECK to run another.")
            return
        }
        state = .booting(program.title)
        let prefix = prefixURL.path
        dlog("[wine] booting \(program.id) in \(prefix)")
        queue.async {
            var err = [CChar](repeating: 0, count: 512)
            let ok = mid_wine_boot(prefix, program.id, "", &err, err.count)
            let msg = String(cString: err)
            DispatchQueue.main.async {
                self.state = ok ? .running(program.title) : .failed(msg)
                if !ok { dlog("[wine] boot failed: \(msg)") }
            }
        }
    }
}
