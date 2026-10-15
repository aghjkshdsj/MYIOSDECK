// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI

/// A Steam game's Steam Cloud saves (SteamCloudSync): how this iPhone and the cloud compare, the
/// saves that need a choice (both dates and sizes, one button per side), Sync now, and the
/// backups of replaced saves with Restore. Part of the game's sheet, and its own page when Play
/// is held by a choice.
struct SteamCloudSection: View {
    let appID: Int
    @ObservedObject private var cloud = SteamCloudSync.shared
    @State private var backups: [SteamCloudBackup] = []
    @State private var restoring: SteamCloudBackup?
    @State private var message: String?
    @State private var working = false
    @State private var showBackups = false

    private var state: SteamCloudSync.GameState { cloud.games[appID] ?? .init() }
    private var record: SteamCloudRecord { cloud.record(appID) }
    private var gameRunning: Bool { cloud.activeApp == appID }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Steam Cloud saves").font(.headline)
            if !SteamCloudSync.enabled {
                Text("Off in Settings › Steam. Nothing is synced.").font(.caption).foregroundStyle(Deck.dim)
            } else {
                status
                if !state.conflicts.isEmpty { conflicts }
                HStack(spacing: 10) {
                    Button("Sync now") { run { _ = await cloud.sync(appID, reason: "Sync now") } }
                        .buttonStyle(DeckButtonStyle())
                        .disabled(working || gameRunning || cloud.isBusy(appID))
                        .padFocus("cloud.sync", radius: 20) { if !(working || gameRunning || cloud.isBusy(appID)) { run { _ = await cloud.sync(appID, reason: "Sync now") } } }
                    if working || cloud.isBusy(appID) { ProgressView() }
                }
                HStack(spacing: 10) {
                    Button("Upload saves…") { prepare(upload: true) }
                        .padFocus("cloud.upload", radius: 10) { if !(working || gameRunning || cloud.isBusy(appID)) { prepare(upload: true) } }
                    Button("Download saves…") { prepare(upload: false) }
                        .padFocus("cloud.download", radius: 10) { if !(working || gameRunning || cloud.isBusy(appID)) { prepare(upload: false) } }
                }
                .buttonStyle(.bordered)
                .disabled(working || gameRunning || cloud.isBusy(appID))
                if gameRunning {
                    Text("The game is running: its saves sync when it ends.").font(.caption).foregroundStyle(Deck.dim)
                }
                files
            }
            backupList
            if let message { Text(message).font(.caption).foregroundStyle(Deck.dim) }
        }
        .task {
            reloadBackups()
            if SteamCloudSync.enabled, !gameRunning { await cloud.check(appID) }
            reloadBackups()
        }
        .sheet(item: $manual) { t in
            ManualTransferSheet(transfer: t) {
                manual = nil
                run { message = await cloud.runManual(t) }
            }
        }
        .glassDialog(item: $restoring, title: { "Restore the backup of \(Self.date($0.created))?" }, message: { _ in "The saved copies go back where the game reads them. This iPhone's current copies are backed up first, so this can be undone. Then the usual sync runs: the restored saves go up to Steam Cloud unless the cloud changed meanwhile (then you choose), and the cloud's copies they replace are backed up too." }) { backup in
            [DialogButton(title: "Restore \(backup.files.count) file\(backup.files.count == 1 ? "" : "s")") {
                run { message = await cloud.restore(appID, backup) }
             }, .cancel()]
        }
    }

    /// A manual upload or download waiting for the user's confirmation.
    @State private var manual: SteamCloudSync.ManualTransfer?

    private func prepare(upload: Bool) {
        run {
            switch await cloud.prepareManual(appID, upload: upload) {
            case .success(let t):
                if t.entries.isEmpty {
                    message = upload ? "Nothing to upload: Steam Cloud already has every save this iPhone has."
                                     : "Nothing to download: this iPhone already has every save Steam Cloud has."
                } else {
                    manual = t
                }
            case .failure(let e):
                message = e.localizedDescription
            }
        }
    }

    private func run(_ work: @escaping () async -> Void) {
        working = true
        Task { @MainActor in
            await work()
            working = false
            reloadBackups()
        }
    }

    private func reloadBackups() { backups = SteamCloudBackups.list(appID) }

    // MARK: Parts

    @ViewBuilder private var status: some View {
        switch state.phase {
        case .checking:
            HStack { ProgressView(); Text("Comparing with Steam Cloud…").font(.caption).foregroundStyle(Deck.dim) }
        case .downloading(let done, let of):
            HStack { ProgressView(); Text("Downloading saves (\(done) of \(of))…").font(.caption).foregroundStyle(Deck.dim) }
        case .uploading(let done, let of):
            HStack { ProgressView(); Text("Uploading saves (step \(done) of \(of))…").font(.caption).foregroundStyle(Deck.dim) }
        case .failed(let why):
            StatusRow(label: "Steam Cloud could not be synced", detail: why + " Nothing was changed; it is tried again before the game's next start.", level: .warn)
        case .idle:
            let a = state.audit
            if state.checked != nil {
                let inSync = a.count(.same)
                StatusRow(label: state.conflicts.isEmpty ? "Steam Cloud: \(a.cloudFiles) file\(a.cloudFiles == 1 ? "" : "s"), \(inSync) identical here" : "Some saves need your choice",
                          detail: detailLine, level: state.conflicts.isEmpty ? .good : .warn)
            } else {
                Text(detailLine).font(.caption).foregroundStyle(Deck.dim)
            }
        }
        if let problem = record.problem, state.phase == .idle {
            Text("Last sync stopped: \(problem) It is tried again before the next start.").font(.caption).foregroundStyle(.orange)
        }
        if record.unsyncedStart {
            Text("The last session started without syncing first. Saves it changed go up only if the cloud has not changed since the last sync; otherwise they wait for your choice here.")
                .font(.caption).foregroundStyle(Deck.dim)
        }
    }

    private var detailLine: String {
        var parts: [String] = []
        if let last = record.lastSync { parts.append("Last sync \(Self.date(last)).") } else { parts.append("Not synced yet on this iPhone.") }
        if let result = state.lastResult { parts.append(result) }
        if state.droppedInCloud > 0 { parts.append("\(state.droppedInCloud) save\(state.droppedInCloud == 1 ? " was" : "s were") removed from the cloud elsewhere and kept here (not sent back).") }
        if !state.audit.unmapped.isEmpty { parts.append("\(state.audit.unmapped.count) cloud file\(state.audit.unmapped.count == 1 ? " is" : "s are") for another platform.") }
        return parts.joined(separator: " ")
    }

    private var conflicts: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("This iPhone and Steam Cloud have different versions of these saves. Choose which to keep; the other version is kept in save-backups.")
                .font(.caption).foregroundStyle(Deck.dim)
            ForEach(state.conflicts) { c in
                VStack(alignment: .leading, spacing: 4) {
                    Text(c.entry.name).font(.subheadline.weight(.semibold)).lineLimit(1)
                    Text(Self.folder(c.entry.path)).font(.caption2).foregroundStyle(Deck.dim).lineLimit(1)
                    Text(Self.reason(c.reason)).font(.caption).foregroundStyle(Deck.dim)
                    HStack(alignment: .top) {
                        side("This iPhone", present: c.entry.kind != .cloudOnly, size: c.entry.localSize, time: c.entry.localTime)
                        Spacer()
                        side("Steam Cloud", present: c.entry.kind != .localOnly, size: c.entry.cloudSize, time: c.entry.cloudTime)
                    }
                    HStack {
                        Button(c.entry.kind == .cloudOnly ? "Restore from cloud" : "Keep cloud's") {
                            run { await cloud.resolve(appID, [c], useCloud: true) }
                        }
                        .padFocus("cloud.keepCloud.\(c.id)", radius: 10) { if !(working || gameRunning) { run { await cloud.resolve(appID, [c], useCloud: true) } } }
                        Spacer()
                        Button(c.entry.kind == .cloudOnly ? "Leave deleted here" : "Keep this iPhone's") {
                            run { await cloud.resolve(appID, [c], useCloud: false) }
                        }
                        .padFocus("cloud.keepLocal.\(c.id)", radius: 10) { if !(working || gameRunning) { run { await cloud.resolve(appID, [c], useCloud: false) } } }
                    }
                    .buttonStyle(.bordered)
                    .disabled(working || gameRunning)
                }
                .padding(10)
                .background(Deck.panelHi, in: RoundedRectangle(cornerRadius: 8))
            }
            if state.conflicts.count > 1 {
                HStack {
                    Button("Keep all from the cloud") { run { await cloud.resolve(appID, state.conflicts, useCloud: true) } }
                        .padFocus("cloud.allCloud", radius: 10) { if !(working || gameRunning) { run { await cloud.resolve(appID, state.conflicts, useCloud: true) } } }
                    Spacer()
                    Button("Keep all from this iPhone") { run { await cloud.resolve(appID, state.conflicts, useCloud: false) } }
                        .padFocus("cloud.allLocal", radius: 10) { if !(working || gameRunning) { run { await cloud.resolve(appID, state.conflicts, useCloud: false) } } }
                }
                .buttonStyle(.bordered)
                .disabled(working || gameRunning)
            }
        }
    }

    private func side(_ title: String, present: Bool, size: UInt64, time: UInt64) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.caption.weight(.semibold))
            if present {
                Text(ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file)).font(.caption2)
                Text(time > 0 ? Self.date(Date(timeIntervalSince1970: TimeInterval(time))) : "no date").font(.caption2)
            } else {
                Text("missing").font(.caption2).foregroundStyle(.orange)
            }
        }
    }

    @ViewBuilder private var files: some View {
        let entries = state.audit.entries
        if !entries.isEmpty {
            DisclosureGroup("Files (\(entries.count))") {
                ForEach(entries) { e in
                    HStack {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(e.name).font(.caption).lineLimit(1)
                            Text(Self.folder(e.path)).font(.caption2).foregroundStyle(Deck.dim).lineLimit(1)
                        }
                        Spacer()
                        Text(Self.kind(e.kind)).font(.caption2).foregroundStyle(Deck.dim)
                    }
                }
            }
            .font(.subheadline)
        }
    }

    @ViewBuilder private var backupList: some View {
        if !backups.isEmpty {
            DisclosureGroup(isExpanded: $showBackups) {
                ForEach(backups) { b in
                    HStack {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(Self.date(b.created)).font(.caption)
                            Text("\(b.reason) · \(b.files.count) file\(b.files.count == 1 ? "" : "s") · " +
                                 Self.sides(b)).font(.caption2).foregroundStyle(Deck.dim)
                        }
                        Spacer()
                        Button("Restore…") { restoring = b }
                            .buttonStyle(.bordered)
                            .disabled(working || gameRunning)
                            .padFocus("cloud.restore.\(b.id)", radius: 10) { if !(working || gameRunning) { restoring = b } }
                    }
                }
                Text("In the Files app: On My iPhone › MYIOSDECK › save-backups › \(String(appID)).")
                    .font(.caption2).foregroundStyle(Deck.dim)
            } label: {
                Text("Backups (\(backups.count))")
                    .padFocus("cloud.backups", radius: 8) { withGlassAnimation { showBackups.toggle() } }
            }
            .font(.subheadline)
        }
    }

    // MARK: Words

    static func date(_ d: Date) -> String { d.formatted(date: .abbreviated, time: .shortened) }

    static func folder(_ path: String) -> String {
        let (root, rest) = SteamCloudPaths.split(path)
        let parts = rest.split(separator: "/")
        let dir = parts.dropLast().joined(separator: "/")
        return (root.map { SteamCloudPaths.canonical(root: $0) } ?? "Steam Cloud folder") + (dir.isEmpty ? "" : " / " + dir)
    }

    static func reason(_ r: SteamCloudConflict.Reason) -> String {
        switch r {
        case .bothChanged: return "Changed on this iPhone and in the cloud since the last sync."
        case .noRecord: return "Different on the two sides, and there was no sync on this iPhone before."
        case .missingHere: return "Gone from this iPhone (the game may have deleted it) but still in the cloud."
        case .unsyncedSession: return "Changed here in a session that started without syncing, while the cloud changed too."
        case .emptyHere: return "This iPhone's copy is empty and the cloud's is not."
        }
    }

    static func kind(_ k: SteamCloudEntry.Kind) -> String {
        switch k {
        case .same: return "in sync"
        case .differ: return "differs"
        case .cloudOnly: return "cloud only"
        case .localOnly: return "this iPhone only"
        }
    }

    static func sides(_ b: SteamCloudBackup) -> String {
        let device = b.files.filter { $0.side == "device" }.count, cloud = b.files.count - device
        return [device > 0 ? "\(device) from this iPhone" : nil, cloud > 0 ? "\(cloud) from the cloud" : nil]
            .compactMap { $0 }.joined(separator: ", ")
    }
}

/// The section as a page of its own (from the Play hold).
struct SteamCloudPage: View {
    let appID: Int
    let name: String
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    Text(name).font(.title3.weight(.bold))
                    SteamCloudSection(appID: appID)
                }
                .padding(16)
            }
            .background(Deck.bg)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
        .padScope("cloudPage") { dismiss() }
    }
}

/// The confirmation of a manual upload or download: every save it copies with both sides' size
/// and date, the ones that replace a copy changed since the last sync marked, and what happens to
/// the replaced copies.
struct ManualTransferSheet: View {
    let transfer: SteamCloudSync.ManualTransfer
    let confirm: () -> Void
    @Environment(\.dismiss) private var dismiss

    private var upload: Bool { transfer.upload }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button(upload ? "Upload \(transfer.entries.count)" : "Download \(transfer.entries.count)") { confirm() }
                        .padFocus("transfer.confirm", radius: 10, preferred: true) { confirm() }
                    Button("Cancel", role: .cancel) { dismiss() }
                        .padFocus("transfer.cancel", radius: 10) { dismiss() }
                }
                Section {
                    ForEach(transfer.entries) { e in
                        VStack(alignment: .leading, spacing: 4) {
                            Text(e.name).font(.subheadline.weight(.semibold)).lineLimit(1)
                            Text(SteamCloudSection.folder(e.path)).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                            HStack {
                                side("This iPhone", present: e.kind != .cloudOnly, size: e.localSize, time: e.localTime)
                                Spacer()
                                side("Steam Cloud", present: e.kind != .localOnly, size: e.cloudSize, time: e.cloudTime)
                            }
                            if transfer.replacesNewer.contains(e.key) {
                                Text(upload ? "The cloud's copy changed since the last sync on this iPhone (another device, or never synced here): it will be replaced; a copy is kept in save-backups."
                                            : "This iPhone's copy changed since the last sync: it will be replaced; a copy is kept in save-backups.")
                                    .font(.caption).foregroundStyle(.orange)
                            }
                        }
                    }
                } footer: {
                    Text(upload
                         ? "These saves on this iPhone go up to Steam Cloud, where every device signed in to this account (a PC too) gets them. The cloud's copies they replace are downloaded into save-backups first. Nothing is deleted in the cloud."
                         : "These saves come down from Steam Cloud to where the game reads them. This iPhone's copies they replace go to save-backups first. Nothing is deleted on this iPhone.")
                }
            }
            .navigationTitle(upload ? "Upload to Steam Cloud" : "Download from Steam Cloud")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button(upload ? "Upload \(transfer.entries.count)" : "Download \(transfer.entries.count)") { confirm() }
                }
            }
        }
        .padScope("transfer") { dismiss() }
    }

    private func side(_ title: String, present: Bool, size: UInt64, time: UInt64) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.caption.weight(.semibold))
            if present {
                Text(ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file)).font(.caption2)
                Text(time > 0 ? SteamCloudSection.date(Date(timeIntervalSince1970: TimeInterval(time))) : "no date").font(.caption2)
            } else {
                Text("not there").font(.caption2).foregroundStyle(.secondary)
            }
        }
    }
}
