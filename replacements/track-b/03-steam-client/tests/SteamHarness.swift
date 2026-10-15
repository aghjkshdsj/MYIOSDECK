// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation
// Standalone XCTest target only. Exclude when testing within the real app module.
enum LogStore {
    private static let lock = NSLock()
    private static var lines: [String] = []
    static func log(_ message: String) { lock.lock(); lines.append(message); lock.unlock() }
    static func captured() -> [String] { lock.lock(); let value = lines; lock.unlock(); return value }
}
enum MadeiraConfig { static func get(_ key: String) -> String? { nil } }
