// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for Steam Cloud saves' decisions (App/Sources/Steam/SteamCloudCore.swift): where a
// cloud path lands in the Wine prefix, how the two sides compare, what is copied which way, and
// which backups may go. No network, no app: tests/host/run.sh compiles this with the core file.

import Foundation

var failures = 0
var checks = 0
func check(_ ok: Bool, _ what: String, file: String = #file, line: Int = #line) {
    checks += 1
    if !ok { failures += 1; print("FAIL line \(line): \(what)") }
}

// A stand-in for SHA-1: any stable digest works, the core only compares them.
func digest(_ data: Data) -> Data {
    var h: UInt64 = 1469598103934665603
    for b in data { h = (h ^ UInt64(b)) &* 1099511628211 }
    return withUnsafeBytes(of: h.bigEndian) { Data($0) }
}
func digest(of url: URL) -> Data? { (try? Data(contentsOf: url)).map(digest) }
func hex(_ d: Data) -> String { SteamCloudRecord.hex(d) }

// MARK: - A Wine prefix in a temporary folder

let root = FileManager.default.temporaryDirectory.appendingPathComponent("steam-cloud-tests-\(getpid())", isDirectory: true)
try? FileManager.default.removeItem(at: root)
let drive = root.appendingPathComponent("drive_c", isDirectory: true)
let user = drive.appendingPathComponent("users/mobile", isDirectory: true)
let install = drive.appendingPathComponent("Program Files (x86)/Steam/steamapps/common/Game", isDirectory: true)
let remote = drive.appendingPathComponent("Program Files (x86)/Steam/userdata/123/480/remote", isDirectory: true)
for dir in [user, install, remote] { try! FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true) }
let steamID: UInt64 = 76561197960265851   // account 123
let paths = SteamCloudPaths(userFolder: user, installFolder: install, remoteFolder: remote, steamID: steamID, overrides: [])

func write(_ url: URL, _ text: String) {
    try! FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try! Data(text.utf8).write(to: url)
}

// MARK: - Paths

check(paths.url(cloudPath: "%WinAppDataLocalLow%Studio/Game/save1.dat")?.path ==
      user.appendingPathComponent("AppData/LocalLow/Studio/Game/save1.dat").path, "WinAppDataLocalLow maps under AppData/LocalLow")
check(paths.url(cloudPath: "%WinMyDocuments%My Games/G/s.sav")?.path ==
      user.appendingPathComponent("Documents/My Games/G/s.sav").path, "WinMyDocuments maps to Documents")
check(paths.url(cloudPath: "%WinAppDataRoaming%G/a")?.path == user.appendingPathComponent("AppData/Roaming/G/a").path, "Roaming")
check(paths.url(cloudPath: "%WinAppDataLocal%G/a")?.path == user.appendingPathComponent("AppData/Local/G/a").path, "Local")
check(paths.url(cloudPath: "%WinSavedGames%G/a")?.path == user.appendingPathComponent("Saved Games/G/a").path, "Saved Games")
check(paths.url(cloudPath: "%GameInstall%saves/x.sav")?.path == install.appendingPathComponent("saves/x.sav").path, "GameInstall")
check(paths.url(cloudPath: "%gameinstall%x")?.path == install.appendingPathComponent("x").path, "root names without regard to case")
check(paths.url(cloudPath: "remote_save.bin")?.path == remote.appendingPathComponent("remote_save.bin").path, "no placeholder: Steam's remote folder")
check(paths.url(cloudPath: "%WinAppDataLocal%{64BitSteamID}/s")?.path ==
      user.appendingPathComponent("AppData/Local/76561197960265851/s").path, "{64BitSteamID}")
check(paths.url(cloudPath: "%WinAppDataLocal%{Steam3AccountID}/s")?.path ==
      user.appendingPathComponent("AppData/Local/123/s").path, "{Steam3AccountID}")
check(paths.url(cloudPath: "%WinAppDataLocal%a\\b\\c.sav")?.path == user.appendingPathComponent("AppData/Local/a/b/c.sav").path, "backslashes")
check(paths.url(cloudPath: "%WinAppDataLocal%../../../etc/x") == nil, "leaving the folder is refused")
check(paths.url(cloudPath: "%WinAppDataLocal%C:/x") == nil, "a drive is refused")
check(paths.url(cloudPath: "%WinAppDataLocal%") == nil, "an empty path is refused")
check(paths.url(cloudPath: "%MacAppSupport%x") == nil, "another platform's root has no folder here")
check(paths.url(cloudPath: "../x") == nil, "remote: leaving the folder is refused")
var overridden = paths
overridden.overrides = [.init(root: "WinMyDocuments", os: "Windows", useInstead: "WinAppDataLocal", addPath: "Moved")]
check(overridden.url(cloudPath: "%WinMyDocuments%G/s")?.path == user.appendingPathComponent("AppData/Local/Moved/G/s").path, "Windows root override")
overridden.overrides = [.init(root: "WinMyDocuments", os: "MacOS", useInstead: "WinAppDataLocal", addPath: "")]
check(overridden.url(cloudPath: "%WinMyDocuments%G/s")?.path == user.appendingPathComponent("Documents/G/s").path, "another OS's override is ignored")
// Wine is case-insensitive, iOS is not: an existing folder is found whatever its case.
write(user.appendingPathComponent("AppData/LocalLow/StudioName/Save.DAT"), "x")
check(paths.url(cloudPath: "%WinAppDataLocalLow%studioname/save.dat")?.lastPathComponent == "Save.DAT", "case-insensitive resolve")
try? FileManager.default.removeItem(at: user.appendingPathComponent("AppData/LocalLow/StudioName"))

check(SteamCloudAudit.wildcard("*.sav", "slot1.SAV"), "wildcard *.sav")
check(!SteamCloudAudit.wildcard("*.sav", "slot1.bak"), "wildcard *.sav excludes .bak")
check(SteamCloudAudit.wildcard("save?.dat", "save3.dat") && !SteamCloudAudit.wildcard("save?.dat", "save10.dat"), "wildcard ?")
check(SteamCloudAudit.wildcard("*", "anything") && SteamCloudAudit.wildcard("a*b*c", "aXXbYYc"), "wildcard *")

// MARK: - Comparison over real files

func file(_ path: String, _ content: String, time: UInt64 = 1_700_000_000, persist: UInt32 = 0) -> SteamCloudFile {
    let (r, rest) = SteamCloudPaths.split(path)
    let prefix = r.map { "%\($0)%" } ?? ""
    let slash = rest.lastIndex(of: "/")
    let dir = slash.map { String(rest[...$0]) } ?? ""
    let name = slash.map { String(rest[rest.index(after: $0)...]) } ?? rest
    return SteamCloudFile(prefix: prefix + dir, name: name, sha: digest(Data(content.utf8)), timestamp: time,
                          size: UInt64(content.utf8.count), persistState: persist)
}
let rules = [SteamCloudSaveRule(root: "WinAppDataLocalLow", path: "Studio/Game", pattern: "*.dat", recursive: false, platforms: ["windows"])]
write(user.appendingPathComponent("AppData/LocalLow/Studio/Game/same.dat"), "same")
write(user.appendingPathComponent("AppData/LocalLow/Studio/Game/differ.dat"), "local version")
write(user.appendingPathComponent("AppData/LocalLow/Studio/Game/new-here.dat"), "new")
write(user.appendingPathComponent("AppData/LocalLow/Studio/Game/notes.txt"), "not a save")
write(remote.appendingPathComponent("api.bin"), "api save")
var listing = SteamCloudListing(changeNumber: 7, files: [
    file("%WinAppDataLocalLow%Studio/Game/same.dat", "same"),
    file("%WinAppDataLocalLow%Studio/Game/differ.dat", "cloud version"),
    file("%WinAppDataLocalLow%Studio/Game/cloud-only.dat", "cloud only"),
    file("%WinAppDataLocalLow%Studio/Game/deleted.dat", "gone", persist: 2),
    file("%MacAppSupport%Game/mac.dat", "mac"),
])
let audit = SteamCloudAudit.run(listing: listing, rules: rules, paths: paths, sha1: digest(of:))
func kind(_ name: String) -> SteamCloudEntry.Kind? { audit.entries.first { $0.name == name }?.kind }
check(kind("same.dat") == .same, "identical file is same")
check(kind("differ.dat") == .differ, "different file differs")
check(kind("cloud-only.dat") == .cloudOnly, "cloud only")
check(kind("new-here.dat") == .localOnly, "a save matching the rule found locally")
check(audit.entries.first { $0.name == "new-here.dat" }?.path == "%WinAppDataLocalLow%Studio/Game/new-here.dat", "local-only file gets its cloud name")
check(kind("notes.txt") == nil, "a file the pattern excludes is not a save")
check(kind("api.bin") == .localOnly && audit.entries.first { $0.name == "api.bin" }?.path == "api.bin", "remote folder files")
check(kind("deleted.dat") == nil, "a deleted cloud file is not listed")
check(audit.unmapped == ["%MacAppSupport%Game/mac.dat"], "another platform's file is unmapped")
check(audit.cloudFiles == 4, "persisted cloud files counted")

// MARK: - Decisions

func entry(_ name: String, _ kind: SteamCloudEntry.Kind, local: String? = nil, cloud: String? = nil) -> SteamCloudEntry {
    var e = SteamCloudEntry(path: "%WinAppDataLocal%G/\(name)", kind: kind)
    if let local { e.localSHA = digest(Data(local.utf8)); e.localSize = UInt64(local.utf8.count); e.localTime = 10 }
    if let cloud { e.cloudSHA = digest(Data(cloud.utf8)); e.cloudSize = UInt64(cloud.utf8.count); e.cloudTime = 20 }
    return e
}
func synced(_ content: String) -> SteamCloudRecord.FileState { .init(state: .synced, sha: hex(digest(Data(content.utf8))), size: UInt64(content.utf8.count)) }
func plan(_ entries: [SteamCloudEntry], _ record: SteamCloudRecord = SteamCloudRecord(), change: UInt64 = 5) -> SteamCloudPlan {
    SteamCloudPlan.make(audit: SteamCloudAudit(changeNumber: change, cloudFiles: entries.count, entries: entries, unmapped: []), record: record)
}
func key(_ name: String) -> String { "%winappdatalocal%g/\(name)" }

// First sync on this device (no record).
var p = plan([entry("a", .same, local: "1", cloud: "1"), entry("b", .differ, local: "x", cloud: "y"),
              entry("c", .cloudOnly, cloud: "c"), entry("d", .localOnly, local: "d")])
check(p.settled[key("a")]?.state == .synced, "same: recorded as synced")
check(p.conflicts.map(\.entry.name) == ["b"] && p.conflicts[0].reason == .noRecord, "differ without a record: the user chooses")
check(p.download.map(\.name) == ["c"], "new in the cloud: downloaded")
check(p.upload.map(\.name) == ["d"], "new here: uploaded")

// Changed on one side since the last sync.
var record = SteamCloudRecord()
record.changeNumber = 5
record.files = [key("here"): synced("old"), key("there"): synced("old"), key("both"): synced("old")]
p = plan([entry("here", .differ, local: "new", cloud: "old"), entry("there", .differ, local: "old", cloud: "new"),
          entry("both", .differ, local: "mine", cloud: "theirs")], record)
check(p.upload.map(\.name) == ["here"], "changed here only: uploaded")
check(p.download.map(\.name) == ["there"], "changed in the cloud only: downloaded")
check(p.conflicts.map(\.entry.name) == ["both"] && p.conflicts[0].reason == .bothChanged, "changed on both sides: the user chooses")

// An empty save never replaces a cloud one unasked.
record.files = [key("e"): synced("old")]
p = plan([entry("e", .differ, local: "", cloud: "old")], record)
check(p.upload.isEmpty && p.conflicts.first?.reason == .emptyHere, "empty local save: the user chooses")

// A session that started without syncing.
record = SteamCloudRecord()
record.changeNumber = 5
record.unsyncedStart = true
record.files = [key("s"): synced("old")]
p = plan([entry("s", .differ, local: "new", cloud: "old")], record, change: 5)
check(p.upload.map(\.name) == ["s"], "unsynced start, cloud unchanged since the last sync: uploaded")
p = plan([entry("s", .differ, local: "new", cloud: "old")], record, change: 6)
check(p.upload.isEmpty && p.conflicts.first?.reason == .unsyncedSession, "unsynced start, cloud moved: the user chooses")
record.changeNumber = 0
p = plan([entry("s", .differ, local: "new", cloud: "old")], record, change: 6)
check(p.upload.isEmpty, "unsynced start and never a complete sync: nothing uploaded unasked")

// Gone from this device.
record = SteamCloudRecord()
record.changeNumber = 5
record.files = [key("kept"): synced("k"), key("gone"): synced("g")]
p = plan([entry("kept", .same, local: "k", cloud: "k"), entry("gone", .cloudOnly, cloud: "g")], record)
check(p.download.isEmpty && p.conflicts.first?.reason == .missingHere, "one save gone while others are here: the user chooses")
check(p.settled[key("gone")]?.state == .missingHere, "and it is marked so a new file of that name is not uploaded over it")
p = plan([entry("kept", .cloudOnly, cloud: "k"), entry("gone", .cloudOnly, cloud: "g")], record)
check(Set(p.download.map(\.name)) == ["kept", "gone"] && p.conflicts.isEmpty, "every save gone (new or reset drive): restored from the cloud")
p = plan([entry("kept", .same, local: "k", cloud: "k"), entry("gone", .cloudOnly, cloud: "g2")], record)
check(p.download.map(\.name) == ["gone"], "gone here but changed in the cloud since: downloaded (nothing here to lose)")
record.files[key("gone")] = .init(state: .missingHere, sha: hex(digest(Data("g".utf8))))
p = plan([entry("gone", .differ, local: "fresh", cloud: "g")], record)
check(p.upload.isEmpty && p.conflicts.first?.reason == .bothChanged, "a new save where one was missing is never uploaded over the cloud's unasked")
record.files[key("gone")] = .init(state: .leftDeleted, sha: hex(digest(Data("g".utf8))))
p = plan([entry("gone", .cloudOnly, cloud: "g")], record)
check(p.isClear, "left deleted by choice: not downloaded again")
p = plan([entry("gone", .cloudOnly, cloud: "g3")], record)
check(p.download.map(\.name) == ["gone"], "left deleted, then changed in the cloud: downloaded")

// Gone from the cloud (deleted elsewhere): kept here, not sent back, nothing deleted.
record.files = [key("dropped"): synced("d")]
p = plan([entry("dropped", .localOnly, local: "d")], record)
check(p.upload.isEmpty && p.download.isEmpty && p.conflicts.isEmpty && p.droppedInCloud.map(\.name) == ["dropped"],
      "deleted in the cloud elsewhere: left on this device, not uploaded")

// An empty or older answer is not the cloud's state.
record = SteamCloudRecord()
record.changeNumber = 9
record.files = [key("a"): synced("1")]
check(record.implausible(SteamCloudListing(changeNumber: 9, files: [])), "no files although saves were synced: implausible")
check(record.implausible(SteamCloudListing(changeNumber: 8, files: [file("%WinAppDataLocal%G/a", "1")])), "change number went back: implausible")
check(!record.implausible(SteamCloudListing(changeNumber: 10, files: [file("%WinAppDataLocal%G/a", "1")])), "a normal answer is plausible")
check(!SteamCloudRecord().implausible(SteamCloudListing(changeNumber: 0, files: [])), "no saves anywhere yet: plausible")

// A record survives a round trip through JSON.
record.unsyncedStart = true
record.rules = rules
let decoded = try! JSONDecoder().decode(SteamCloudRecord.self, from: try! JSONEncoder().encode(record))
check(decoded == record, "record JSON round trip")

// MARK: - Backups

let now = Date(timeIntervalSince1970: 2_000_000_000)
func backups(_ ages: [Double]) -> [(name: String, created: Date)] {
    ages.enumerated().map { ("b\($0.offset)", now.addingTimeInterval(-$0.element * 86_400)) }
}
check(SteamCloudBackupRetention.expired(backups(Array(repeating: 1, count: 100)), now: now).isEmpty, "recent backups all stay")
let old = backups((0..<40).map { 100 + Double($0) })   // 40 backups, all older than 60 days
let dropped = Set(SteamCloudBackupRetention.expired(old, now: now))
check(dropped.count == 10, "beyond 30, the oldest go (\(dropped.count))")
check(dropped == Set((30..<40).map { "b\($0)" }), "only the oldest ones")
let mixed = backups((0..<12).map { 200 + Double($0) })   // 12 old: under the limit
check(SteamCloudBackupRetention.expired(mixed, now: now).isEmpty, "under the limit nothing goes, however old")
let many = backups((0..<45).map { 70 + Double($0) })
let gone = SteamCloudBackupRetention.expired(many, now: now)
check(!gone.contains { ["b0", "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "b9"].contains($0) }, "the newest ten never go")

try? FileManager.default.removeItem(at: root)
print("steam cloud host tests: \(checks - failures)/\(checks) passed")
exit(failures == 0 ? 0 : 1)
