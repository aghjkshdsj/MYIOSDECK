// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The two Madeira app types SwiftSteam (vendored from Madeira, see
/// SwiftSteam/README.md) reaches outside its own folder.

/// Madeira reads `env.NAME` switches from Documents/madeira.cfg. MYIOSDECK has
/// no such file yet; SwiftSteam then falls back to the process environment.
enum MadeiraConfig {
    static func get(_ key: String) -> String? { nil }
}

extension LogStore {
    /// SwiftSteam logs through `LogStore.shared.log(_:)`.
    func log(_ message: String) { LogStore.log(message) }
}
