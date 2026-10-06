// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController

/// What the hardware is and what it gives us: the CPU features FEX targets,
/// the memory budget, and connected controllers.
final class DeviceInfo: ObservableObject {
    struct Feature: Identifiable { let id: String; let on: Bool; let why: String }

    let info: mid_sysinfo
    @Published private(set) var controllers: [String] = []
    @Published private(set) var availableMB: Int = 0
    @Published private(set) var footprintMB: Int = 0
    @Published private(set) var thermal: ProcessInfo.ThermalState = .nominal

    init() {
        var s = mid_sysinfo()
        mid_sysinfo_read(&s)
        info = s
        refresh()
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in self?.refresh() }
        }
        NotificationCenter.default.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil,
                                               queue: .main) { [weak self] _ in self?.refresh() }
    }

    func refresh() {
        controllers = GCController.controllers().map { $0.vendorName ?? "Controller" }
        availableMB = Int(mid_available_memory() >> 20)
        footprintMB = Int(mid_phys_footprint() >> 20)
        thermal = ProcessInfo.processInfo.thermalState
    }

    var machine: String { Self.string(info.machine) }
    var cpuBrand: String {
        let b = Self.string(info.cpu_brand)
        return b.isEmpty ? machine : b
    }
    var memoryGB: Double { Double(info.memsize) / 1_073_741_824 }
    var cores: String { "\(info.ncpu) cores (\(info.perf_cores) performance + \(info.eff_cores) efficiency)" }
    var hasIncreasedMemoryLimit: Bool { mid_available_memory() > 4_000_000_000 }

    var features: [Feature] {
        [
            Feature(id: "LSE atomics", on: info.lse, why: "x86 LOCK instructions become single ARM atomics"),
            Feature(id: "LRCPC", on: info.rcpc, why: "cheap acquire loads for x86 memory ordering"),
            Feature(id: "LRCPC2", on: info.rcpc2, why: "ordered loads/stores with offsets (TSO fast path)"),
            Feature(id: "FlagM / FlagM2", on: info.flagm && info.flagm2, why: "x86 EFLAGS without extra instructions"),
            Feature(id: "FRINTTS", on: info.frintts, why: "fast float to int conversions"),
            Feature(id: "AFP", on: info.afp, why: "x86-exact SSE min/max and denormals"),
            Feature(id: "AES / PMULL", on: info.aes && info.pmull, why: "AES-NI and PCLMULQDQ in hardware"),
            Feature(id: "SHA1 / SHA256", on: info.sha1 && info.sha256, why: "SHA-NI in hardware"),
            Feature(id: "CRC32", on: info.crc32, why: "SSE4.2 CRC32 in hardware"),
            Feature(id: "CSSC", on: info.cssc, why: "single-instruction min/max/popcount"),
            Feature(id: "ECV / WFxT", on: info.ecv && info.wfxt, why: "precise timers and waits"),
        ]
    }

    static func string<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }
}
