// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

// Application log forwarding is integration glue, independent of the bridge package.
private final class TrackBLogTail: @unchecked Sendable {
    static let shared = TrackBLogTail()
    private let queue = DispatchQueue(label: "myiosdeck.trackB.log")
    private var timer: DispatchSourceTimer?
    private var reader: FileHandle?
    private var pending = Data()
    private var path = ""
    private var idle = 0
    func start(_ path: String) {
        queue.async { [self] in
            guard timer == nil else { return }
            self.path = path
            let source = DispatchSource.makeTimerSource(queue: queue)
            source.schedule(deadline: .now(), repeating: .milliseconds(400), leeway: .milliseconds(100))
            source.setEventHandler { [self] in tick() }
            timer = source; source.resume()
        }
    }
    private func tick() {
        if reader == nil { reader = try? FileHandle(forReadingFrom: URL(fileURLWithPath: path)) }
        let bytes = try? reader?.read(upToCount: 32768)
        if let bytes, !bytes.isEmpty {
            idle = 0; pending.append(bytes)
            while let newline = pending.firstIndex(of: 10) {
                let line = String(decoding: pending[..<newline], as: UTF8.self)
                pending.removeSubrange(...newline)
                if !line.isEmpty { dlog("[wine] " + line) }
            }
            if pending.count > 65536 {
                dlog("[wine] " + String(decoding: pending.prefix(65536), as: UTF8.self))
                pending.removeAll(keepingCapacity: true)
            }
        } else {
            idle += 1
            if idle > 25 && mid_wine_running() == 0 {
                timer?.cancel(); timer = nil
                try? reader?.close(); reader = nil
            }
        }
    }
}
@_cdecl("mid_track_b_start_log_tail")
func midTrackBStartLogTail(_ path: UnsafePointer<CChar>?) {
    guard let path else { return }
    TrackBLogTail.shared.start(String(cString: path))
}
