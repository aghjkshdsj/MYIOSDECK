// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import UIKit

/// How x86-64 code is executed.
enum RunMode: String {
    case jit = "FEX JIT"            // translated to ARM64 once, runs at near-native speed
    case interpreter = "Interpreter" // Blink, no executable memory needed (no JIT)
}

struct BenchResult: Identifiable {
    let id = UUID()
    let name: String
    let what: String
    let nativeNs: UInt64
    var fexNs: UInt64?
    var fexMatch: Bool?
    /// The interpreter runs a smaller workload; native time at that same size.
    var interpScale: UInt32?
    var nativeAtInterpScaleNs: UInt64?
    var interpNs: UInt64?
    var interpMatch: Bool?

    /// Share of native speed (100% = as fast as native ARM64).
    var fexEfficiency: Double? {
        guard let f = fexNs, f > 0 else { return nil }
        return Double(nativeNs) / Double(f) * 100
    }
    var interpEfficiency: Double? {
        guard let i = interpNs, i > 0, let n = nativeAtInterpScaleNs else { return nil }
        return Double(n) / Double(i) * 100
    }
}

struct GuestRun {
    let mode: RunMode
    let ok: Bool
    let exitCode: Int64
    let seconds: Double
    let syscalls: UInt64
    let poolUsedKB: Int
    let output: String
    let error: String
}

/// Owns the x86-64 engines: FEXCore (JIT) and Blink (interpreter). Runs the
/// built-in programs and the benchmarks, and writes shareable reports.
final class EngineController: ObservableObject, @unchecked Sendable {
    enum State: Equatable { case notStarted, starting, ready, failed(String) }

    @Published private(set) var state: State = .notStarted
    @Published private(set) var lastRun: GuestRun?
    @Published private(set) var bench: [BenchResult] = []
    @Published private(set) var busy: String?
    @Published private(set) var lastReportURL: URL?

    let linked = mid_engine_linked()
    let interpreterLinked = mid_interp_linked()
    let fexVersion = String(cString: mid_engine_version())
    let interpVersion = String(cString: mid_interp_version())

    /// The interpreter runs this fraction of each kernel's normal workload.
    static let interpreterScaleDivisor: UInt32 = 20

    private let queue = DispatchQueue(label: "myiosdeck.engine", qos: .userInitiated)

    func start(settings: Settings) {
        mid_engine_set_verbose(settings.verboseFEXLog)
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

    func runHello(mode: RunMode) {
        let (ptr, len) = guest { mid_guest_hello($0) }
        guard let ptr else { return }
        work("Running x86-64 hello (\(mode.rawValue))") {
            let r = self.run(ptr, len, ["hello"], mode: mode)
            DispatchQueue.main.async { self.lastRun = r }
        }
    }

    /// - fex: run each kernel through FEX (needs JIT)
    /// - interpreter: also run each kernel in the interpreter at a reduced size
    func runBenchmarks(fex: Bool, interpreter: Bool, presetName: String) {
        let (ptr, len) = guest { mid_guest_bench($0) }
        let (iptr, ilen) = guest { mid_guest_bench_sse2($0) }
        work("Benchmarking") {
            var results: [BenchResult] = []
            for i in 0..<Int(mid_bench_count()) {
                let name = String(cString: mid_bench_name(Int32(i)))
                let what = String(cString: mid_bench_what(Int32(i)))
                let fullScale = mid_bench_default_scale(Int32(i))
                self.setBusy("Native ARM64: \(name)")
                var nativeSum: UInt64 = 0
                let nativeNs = mid_bench_native(Int32(i), 0, &nativeSum)
                var r = BenchResult(name: name, what: what, nativeNs: nativeNs)

                if fex, let ptr, self.state == .ready {
                    self.setBusy("x86-64 via FEX JIT: \(name)")
                    let run = self.run(ptr, len, ["bench", name], mode: .jit)
                    (r.fexNs, r.fexMatch) = Self.parseResult(run, expectedSum: nativeSum)
                }
                if interpreter, let iptr, self.interpreterLinked {
                    let scale = max(1, fullScale / Self.interpreterScaleDivisor)
                    var smallSum: UInt64 = 0
                    r.interpScale = scale
                    r.nativeAtInterpScaleNs = mid_bench_native(Int32(i), scale, &smallSum)
                    self.setBusy("x86-64 via interpreter (no JIT): \(name)")
                    let run = self.run(iptr, ilen, ["bench", name, String(scale)], mode: .interpreter)
                    (r.interpNs, r.interpMatch) = Self.parseResult(run, expectedSum: smallSum)
                }
                results.append(r)
                let snapshot = results
                DispatchQueue.main.async { self.bench = snapshot }
            }
            self.writeReport(results, presetName: presetName)
        }
    }

    // MARK: - report

    private func writeReport(_ results: [BenchResult], presetName: String) {
        let date = ISO8601DateFormatter().string(from: Date())
        let info = Bundle.main.infoDictionary
        let d = Self.interpreterScaleDivisor
        var lines: [String] = []
        lines.append("==== MYIOSDECK BENCHMARK REPORT ====")
        lines.append("date: \(date)")
        lines.append("device: \(DeviceInfo.describeDevice()) | iOS \(UIDevice.current.systemVersion)")
        lines.append("app: \(info?["CFBundleShortVersionString"] ?? "?") build \(info?["CFBundleVersion"] ?? "?")")
        lines.append("jit: \(mid_jit_pool_ready() ? "on (\(mid_jit_pool_size() >> 20) MB pool)" : "OFF")" +
                     " | fex: \(fexVersion.prefix(10)) preset \(presetName) (x86-64-v2 build)" +
                     " | interpreter: blink \(interpVersion.prefix(10)) (SSE2 build)")
        lines.append("thermal: \(ProcessInfo.processInfo.thermalState.rawValue) | low power: \(ProcessInfo.processInfo.isLowPowerModeEnabled)")
        lines.append(Self.row(["kernel", "native", "fex", "fex%", "native/\(d)", "interp/\(d)", "interp%"]))
        for r in results {
            lines.append(Self.row([r.name, Self.ms(r.nativeNs), r.fexNs.map(Self.ms) ?? "-",
                                   r.fexEfficiency.map { String(format: "%.0f%%", $0) } ?? "-",
                                   r.nativeAtInterpScaleNs.map(Self.ms) ?? "-", r.interpNs.map(Self.ms) ?? "-",
                                   r.interpEfficiency.map { String(format: "%.1f%%", $0) } ?? "-"]))
            if r.fexMatch == false || r.interpMatch == false {
                lines.append("  ! checksum mismatch (fex=\(String(describing: r.fexMatch)) interp=\(String(describing: r.interpMatch)))")
            }
        }
        if let a = Self.average(results.compactMap(\.fexEfficiency)) {
            lines.append(String(format: "average FEX JIT: %.0f%% of native", a))
        }
        if let a = Self.average(results.compactMap(\.interpEfficiency)) {
            lines.append(String(format: "average interpreter (no JIT): %.1f%% of native, about %.0fx slower", a, 100 / max(a, 0.001)))
        }
        lines.append("==== END REPORT ====")
        lines.forEach { dlog("[report] \($0)") }

        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("benchmarks")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("benchmark-\(date.replacingOccurrences(of: ":", with: "-")).txt")
        try? (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
        DispatchQueue.main.async { self.lastReportURL = url }
    }

    private static func row(_ cols: [String]) -> String {
        let widths = [8, 10, 10, 6, 11, 11, 8]
        return zip(cols, widths).map { c, w in c.count >= w ? c : c + String(repeating: " ", count: w - c.count) }
            .joined(separator: " ")
    }

    // MARK: - plumbing

    private static func parseResult(_ run: GuestRun, expectedSum: UInt64) -> (UInt64?, Bool?) {
        guard let line = run.output.split(separator: "\n").first(where: { $0.hasPrefix("RESULT") }) else {
            dlog("[bench] \(run.mode.rawValue) run failed: \(run.error) exit=\(run.exitCode) output=\(run.output.prefix(300))")
            return (nil, nil)
        }
        let f = parseFields(String(line))
        let ns = f["ns"].flatMap { UInt64($0) }
        let match = f["sum"].flatMap { UInt64($0) }.map { $0 == expectedSum }
        return (ns, match)
    }

    private func guest(_ get: (UnsafeMutablePointer<Int>) -> UnsafePointer<UInt8>?) -> (UnsafePointer<UInt8>?, Int) {
        var len = 0
        let p = get(&len)
        if p == nil || len == 0 { dlog("[engine] guest programs are missing from this build") }
        return (len > 0 ? p : nil, len)
    }

    private func run(_ elf: UnsafePointer<UInt8>, _ len: Int, _ args: [String], mode: RunMode) -> GuestRun {
        let cArgs = args.map { strdup($0) }
        defer { cArgs.forEach { free($0) } }
        var argv = cArgs.map { UnsafePointer<CChar>($0) }
        let result = UnsafeMutablePointer<mid_run_result>.allocate(capacity: 1)
        defer { result.deallocate() }
        let ok = argv.withUnsafeMutableBufferPointer { buf -> Bool in
            switch mode {
            case .jit: return mid_engine_run_elf(elf, len, Int32(args.count), buf.baseAddress, result)
            case .interpreter: return mid_interp_run_elf(elf, len, Int32(args.count), buf.baseAddress, result)
            }
        }
        let r = result.pointee
        let run = GuestRun(mode: mode, ok: ok, exitCode: r.exit_code, seconds: r.seconds, syscalls: r.syscalls,
                           poolUsedKB: Int(r.pool_used_after &- r.pool_used_before) >> 10,
                           output: String(cString: mid_run_result_output(result)),
                           error: String(cString: mid_run_result_error(result)))
        dlog("[engine] \(mode.rawValue) \(args.joined(separator: " ")): ok=\(ok) exit=\(run.exitCode) " +
             String(format: "%.3f s", run.seconds) +
             (mode == .jit ? ", \(run.syscalls) syscalls, +\(run.poolUsedKB) KB JIT code" : "") +
             (run.error.isEmpty ? "" : " error=\(run.error)"))
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

    static func ms(_ ns: UInt64) -> String { String(format: "%.1f ms", Double(ns) / 1e6) }
    static func average(_ v: [Double]) -> Double? { v.isEmpty ? nil : v.reduce(0, +) / Double(v.count) }

    static func parseFields(_ line: String) -> [String: String] {
        var out: [String: String] = [:]
        for part in line.split(separator: " ") {
            let kv = part.split(separator: "=", maxSplits: 1)
            if kv.count == 2 { out[String(kv[0])] = String(kv[1]) }
        }
        return out
    }
}
