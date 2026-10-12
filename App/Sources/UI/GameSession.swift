// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// The running game's full-screen view: whether it is shown and what it is called. Shared, so it
/// outlives the Library tab that presents it: a game hidden with the game menu's Hide game keeps
/// running, and the top bar's Return button (RootView) brings its view back from any tab.
@MainActor
final class GameSession: ObservableObject {
    static let shared = GameSession()

    /// The game whose view the Library tab presents ("" when none).
    @Published var title = ""
    /// The view is on screen (LibraryView's full-screen cover).
    @Published var shown = false
}

extension WineController {
    /// A program is starting or running, so its view can be shown again.
    var isPlaying: Bool {
        switch state {
        case .booting, .running: return true
        default: return false
        }
    }
}
