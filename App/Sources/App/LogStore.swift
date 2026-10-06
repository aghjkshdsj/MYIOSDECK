// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// One log for every layer (Swift, the JIT core, FEXCore). Lines also go to
/// Documents/myiosdeck-log.txt so they can be shared from the Files app.
/// `lines` is only mutated on the main queue; `log` is callable from any thread.
final class LogStore: ObservableObject, @unchecked Sendable {
    static let shared = LogStore()

    @Published private(set) var lines: [String] = []
    let fileURL: URL
    private let maxLines = 4000
    private let fileQueue = DispatchQueue(label: "myiosdeck.log.file")
    private var handle: FileHandle?

    private init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        fileURL = docs.appendingPathComponent("myiosdeck-log.txt")
        FileManager.default.createFile(atPath: fileURL.path, contents: nil)
        handle = try? FileHandle(forWritingTo: fileURL)
        mid_set_log_sink { cLine in
            guard let cLine else { return }
            LogStore.log(String(cString: cLine))
        }
    }

    static func log(_ message: String) {
        let line = "\(stamp()) \(message)"
        let store = shared
        store.fileQueue.async {
            if let data = (line + "\n").data(using: .utf8) { store.handle?.write(data) }
        }
        DispatchQueue.main.async { store.append(line) }
    }

    private func append(_ line: String) {
        lines.append(line)
        if lines.count > maxLines { lines.removeFirst(lines.count - maxLines) }
    }

    func clear() { lines.removeAll() }

    private static func stamp() -> String {
        var t = timespec()
        clock_gettime(CLOCK_REALTIME, &t)
        var tmv = tm()
        var secs = t.tv_sec
        localtime_r(&secs, &tmv)
        return String(format: "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, Int(t.tv_nsec / 1_000_000))
    }
}

func dlog(_ message: String) { LogStore.log(message) }
