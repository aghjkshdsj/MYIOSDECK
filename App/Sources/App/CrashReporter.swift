// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import SwiftUI

/// Offers the log for sharing after something went wrong:
/// - the app itself ended while in the foreground (a crash, or iOS killing it
///   for memory): detected at the next launch by a marker file that is only
///   removed when the app goes to the background; that session's log is kept
///   as myiosdeck-crash-log.txt before the new session truncates the log;
/// - a Windows program ended with an error (WineController.watch).
final class CrashReporter: ObservableObject {
    static let shared = CrashReporter()

    struct Report: Identifiable {
        let id = UUID()
        let title: String
        let detail: String
        let file: URL
    }

    @Published var report: Report?

    private static var docs: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    private static var marker: URL { docs.appendingPathComponent(".myiosdeck-session-active") }
    static var crashLogURL: URL { docs.appendingPathComponent("myiosdeck-crash-log.txt") }

    /// Called by LogStore before it truncates the log. Returns true when the
    /// previous session ended unexpectedly (its log is then preserved).
    static func preservePreviousLogIfCrashed(log: URL) -> Bool {
        let fm = FileManager.default
        guard fm.fileExists(atPath: marker.path) else { return false }
        try? fm.removeItem(at: crashLogURL)
        try? fm.copyItem(at: log, to: crashLogURL)
        try? fm.removeItem(at: marker)
        return true
    }

    private var crashedLastTime = false
    func noteCrashedLastTime() { crashedLastTime = true }

    /// After launch: offer the previous session's log once.
    func offerPreviousCrash() {
        guard crashedLastTime else { return }
        crashedLastTime = false
        report = Report(title: "MYIOSDECK closed unexpectedly",
                        detail: "The last session ended while the app was open (a crash, or iOS closing it for memory). Share its log so the bug can be fixed.",
                        file: Self.crashLogURL)
    }

    /// A Windows program ended with an error.
    func programCrashed(_ title: String, how: String) {
        report = Report(title: "\(title) crashed",
                        detail: "It \(how). Share the log so the bug can be fixed.",
                        file: LogStore.shared.fileURL)
    }

    /// Quit game: Wine cannot stop a running Windows program (one session per
    /// app run), so ending the game ends the app. Not reported as a crash.
    func quitApp() {
        dlog("[app] quit requested by the user")
        sceneActive(false)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { exit(0) }   // let the log line reach the file
    }

    /// Foreground: arm the marker. Background: a later exit is not a crash.
    func sceneActive(_ active: Bool) {
        if active {
            FileManager.default.createFile(atPath: Self.marker.path, contents: Data())
        } else {
            try? FileManager.default.removeItem(at: Self.marker)
        }
    }
}

struct CrashReportSheet: View {
    let report: CrashReporter.Report
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label(report.title, systemImage: "exclamationmark.triangle.fill")
                .font(.title3.weight(.bold))
                .foregroundStyle(.orange)
            Text(report.detail).foregroundStyle(Deck.dim)
            ShareLink(item: report.file) {
                Label("Share log", systemImage: "square.and.arrow.up")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(DeckButtonStyle())
            Button("Not now") { dismiss() }
                .frame(maxWidth: .infinity)
            Spacer()
        }
        .padding(24)
        .background(Deck.bg)
        .presentationDetents([.medium])
    }
}
