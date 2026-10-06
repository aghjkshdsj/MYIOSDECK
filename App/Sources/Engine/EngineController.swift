// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

struct BenchResult: Identifiable {
    let id = UUID()
    let name: String
    let what: String
    let nativeNs: UInt64
    let guestNs: UInt64?
    let checksumsMatch: Bool?

    /// Guest speed as a share of native speed (100% = as fast as native ARM64).
    var efficiency: Double? {
        guard let g = guestNs, g > 0 else { return nil }
        return Double(nativeNs) / Double(g) * 100
    }
}

struct GuestRun {
    let ok: Bool
    let exitCode: Int64
    let seconds: Double
    let syscalls: UInt64
    let poolUsedKB: Int
    let output: String
    let error: String
}

/// Owns FEXCore: start/stop with the chosen preset, run the built-in x86-64
/// programs, and the native-vs-FEX benchmarks.
final class EngineController: ObservableObject, @unchecked Sendable {
    enum State: Equatable { case notStarted, starting, ready, failed(String) }

    @Published private(set) var state: State = .notStarted
    @Published private(set) var lastRun: GuestRun?
    @Published private(set) var bench: [BenchResult] = []
    @Published private(set) var busy: String?

    let linked = mid_engine_linked()
    let fexVersion = String(cString: mid_engine_version())

    private let queue = DispatchQueue(label: "myiosdeck.engine", qos: .userInitiated)

    func start(settings: Settings) {
        guard linked else {
            state = .failed("This build has no FEXCore (UI-only build).")
            return
        }
        var cfg = mid_engine_config(preset: mid_preset(rawValue: numericCast(settings.fexPreset.rawValue)),
                                    multiblock: settings.multiblock,
                                    max_inst: Int32(settings.maxInst))
        state = .starting
        queue.async {
            var err = [CChar](repeating: 0, count: 512)
            let ok = mid_engine_init(&cfg, &err, err.count)
            let msg = String(cString: err)
            DispatchQueue.main.async { self.state = ok ? .ready : .failed(msg) }
        }
    }

    func runHello() {
        let (ptr, len) = guest { mid_guest_hello($0) }
        guard let ptr else { return }
        work("Running x86-64 hello") {
            let r = self.run(ptr, len, ["hello"])
            DispatchQueue.main.async { self.lastRun = r }
        }
    }

    func runBenchmarks(includeGuest: Bool) {
        let (ptr, len) = guest { mid_guest_bench($0) }
        work("Benchmarking") {
            var results: [BenchResult] = []
            for i in 0..<Int(mid_bench_count()) {
                let name = String(cString: mid_bench_name(Int32(i)))
                let what = String(cString: mid_bench_what(Int32(i)))
                self.setBusy("Native ARM64: \(name)")
                var nativeSum: UInt64 = 0
                let nativeNs = mid_bench_native(Int32(i), 0, &nativeSum)
                dlog("[bench] native \(name): \(nativeNs / 1_000_000) ms sum=\(nativeSum)")

                var guestNs: UInt64?
                var match: Bool?
                if includeGuest, let ptr, self.state == .ready {
                    self.setBusy("x86-64 via FEX: \(name)")
                    let r = self.run(ptr, len, ["bench", name])
                    if let line = r.output.split(separator: "\n").first(where: { $0.hasPrefix("RESULT") }) {
                        let fields = Self.parseFields(String(line))
                        guestNs = fields["ns"].flatMap { UInt64($0) }
                        match = fields["sum"].flatMap { UInt64($0) }.map { $0 == nativeSum }
                    } else {
                        dlog("[bench] guest \(name) failed: \(r.error) exit=\(r.exitCode) output=\(r.output)")
                    }
                    if let g = guestNs { dlog("[bench] fex \(name): \(g / 1_000_000) ms match=\(match ?? false)") }
                }
                results.append(BenchResult(name: name, what: what, nativeNs: nativeNs, guestNs: guestNs, checksumsMatch: match))
                let snapshot = results
                DispatchQueue.main.async { self.bench = snapshot }
            }
        }
    }

    // MARK: - plumbing

    private func guest(_ get: (UnsafeMutablePointer<Int>) -> UnsafePointer<UInt8>?) -> (UnsafePointer<UInt8>?, Int) {
        var len = 0
        let p = get(&len)
        if p == nil || len == 0 { dlog("[engine] guest programs are missing from this build") }
        return (len > 0 ? p : nil, len)
    }

    private func run(_ elf: UnsafePointer<UInt8>, _ len: Int, _ args: [String]) -> GuestRun {
        let cArgs = args.map { strdup($0) }
        defer { cArgs.forEach { free($0) } }
        var argv = cArgs.map { UnsafePointer<CChar>($0) }
        let result = UnsafeMutablePointer<mid_run_result>.allocate(capacity: 1)
        defer { result.deallocate() }
        let ok = argv.withUnsafeMutableBufferPointer { buf in
            mid_engine_run_elf(elf, len, Int32(args.count), buf.baseAddress, result)
        }
        let r = result.pointee
        let output = String(cString: mid_run_result_output(result))
        let error = String(cString: mid_run_result_error(result))
        let run = GuestRun(ok: ok, exitCode: r.exit_code, seconds: r.seconds, syscalls: r.syscalls,
                           poolUsedKB: Int((r.pool_used_after - r.pool_used_before) >> 10),
                           output: output, error: error)
        dlog("[engine] \(args.joined(separator: " ")): ok=\(ok) exit=\(run.exitCode) \(String(format: "%.3f", run.seconds)) s, \(run.syscalls) syscalls, +\(run.poolUsedKB) KB JIT code")
        return run
    }

    private func work(_ label: String, _ body: @escaping () -> Void) {
        guard busy == nil else { return }
        busy = label
        queue.async {
            body()
            DispatchQueue.main.async { self.busy = nil }
        }
    }

    private func setBusy(_ s: String) { DispatchQueue.main.async { self.busy = s } }

    static func parseFields(_ line: String) -> [String: String] {
        var out: [String: String] = [:]
        for part in line.split(separator: " ") {
            let kv = part.split(separator: "=", maxSplits: 1)
            if kv.count == 2 { out[String(kv[0])] = String(kv[1]) }
        }
        return out
    }
}
