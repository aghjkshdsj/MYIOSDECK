// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// When each game was last started, so the library lists the recently played ones first.
/// Keys: "steam.<App ID>" and "custom.<id>" (CustomGame). Kept in UserDefaults.
@MainActor final class RecentlyPlayed: ObservableObject {
    static let shared = RecentlyPlayed()
    private static let defaultsKey = "recentlyPlayed"

    @Published private(set) var started: [String: Double]

    private init() {
        started = UserDefaults.standard.dictionary(forKey: Self.defaultsKey) as? [String: Double] ?? [:]
    }

    static func steam(_ appID: Int) -> String { "steam.\(appID)" }
    static func custom(_ id: UUID) -> String { "custom.\(id.uuidString)" }

    /// A game just started.
    func note(_ key: String) {
        started[key] = Date().timeIntervalSince1970
        UserDefaults.standard.set(started, forKey: Self.defaultsKey)
    }

    /// A game that is gone (deleted).
    func forget(_ key: String) {
        guard started.removeValue(forKey: key) != nil else { return }
        UserDefaults.standard.set(started, forKey: Self.defaultsKey)
    }

    /// The played games first, the latest first; the others after them in their own order.
    func sorted<T>(_ items: [T], key: (T) -> String) -> [T] {
        items.enumerated()
            .map { (index: $0.offset, item: $0.element, at: started[key($0.element)] ?? 0) }
            .sorted { $0.at != $1.at ? $0.at > $1.at : $0.index < $1.index }
            .map { $0.item }
    }
}
