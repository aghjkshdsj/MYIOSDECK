// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// The settings file the Steam client reads switches from (`env.NAME` keys, as
/// interfaces/steam_api.md describes). MYIOSDECK has no such file yet, so every lookup
/// answers nil and the Steam client falls back to the process environment.
enum MadeiraConfig {
    static func get(_ key: String) -> String? { nil }
}
