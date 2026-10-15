// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation
import Darwin

private final class BSteamDisk {
    let root: URL
    let fd: Int32
    init(_ root: URL) throws {
        self.root = root
        try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
        fd = open(root.path,O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
        guard fd >= 0 else { throw SteamError.files("Could not open the Steam library folder.") }
    }
    deinit { close(fd) }
    func directory(_ parts: [String], create: Bool = true) throws -> Int32 {
        var current = dup(fd)
        guard current >= 0 else { throw SteamError.files("Could not open a download folder.") }
        do {
            for part in parts {
                guard !part.isEmpty, part != ".", part != "..", !part.contains("/") else { throw SteamError.invalidMessage }
                if create && mkdirat(current,part,0o755) != 0 && errno != EEXIST { throw SteamError.files("Could not create a download folder.") }
                let next = openat(current,part,O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
                guard next >= 0 else { throw SteamError.files("A download folder is not a safe directory.") }
                close(current); current = next
            }
            return current
        } catch { close(current); throw error }
    }
    func withFile<T>(_ path: [String], flags: Int32, createParents: Bool = false, _ body: (Int32) throws -> T) throws -> T {
        guard let leaf = path.last else { throw SteamError.invalidMessage }
        let parent = try directory(Array(path.dropLast()),create:createParents); defer { close(parent) }
        let file = openat(parent,leaf,flags | O_NOFOLLOW,0o644)
        guard file >= 0 else { throw SteamError.files("Could not open a game file.") }; defer { close(file) }
        var st = stat(); guard fstat(file,&st) == 0, st.st_mode & S_IFMT == S_IFREG else { throw SteamError.files("A game file is not a regular file.") }
        return try body(file)
    }
    func existingSize(_ path: [String]) throws -> UInt64? {
        let parent = try directory(Array(path.dropLast())); defer { close(parent) }
        guard let name = path.last else { throw SteamError.invalidMessage }
        var st = stat()
        if fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW) != 0 { if errno == ENOENT { return nil }; throw SteamError.files("Could not inspect a game file.") }
        guard st.st_mode & S_IFMT == S_IFREG, st.st_size >= 0 else { throw SteamError.files("A game file is not a regular file.") }
        return UInt64(st.st_size)
    }
    func write(_ data: Data, file: Int32, offset: UInt64 = 0) throws {
        guard offset <= UInt64(Int64.max), UInt64(data.count) <= UInt64(Int64.max)-offset else { throw SteamError.invalidMessage }
        try data.withUnsafeBytes { (buffer: UnsafeRawBufferPointer) in
            var count = 0
            while count < data.count {
                guard let base = buffer.baseAddress else { throw SteamError.invalidMessage }
                let n = pwrite(file,base.advanced(by:count),data.count-count,off_t(offset+UInt64(count)))
                if n < 0 && errno == EINTR { continue }; guard n > 0 else { throw SteamError.files("Could not write a game file.") }; count += n
            }
        }
    }
    func read(_ file: Int32, offset: UInt64, size: Int) throws -> Data {
        guard size >= 0, size <= 64*1024*1024, offset <= UInt64(Int64.max), UInt64(size) <= UInt64(Int64.max)-offset else { throw SteamError.invalidMessage }
        var data = Data(count:size)
        try data.withUnsafeMutableBytes { (buffer: UnsafeMutableRawBufferPointer) in
            var count = 0
            while count < size {
                guard let base = buffer.baseAddress else { throw SteamError.invalidMessage }
                let n = pread(file,base.advanced(by:count),size-count,off_t(offset+UInt64(count)))
                if n < 0 && errno == EINTR { continue }; guard n > 0 else { throw SteamError.files("Could not read a game file.") }; count += n
            }
        }
        return data
    }
    func atomic(_ data: Data, path: [String]) throws {
        guard let leaf = path.last else { throw SteamError.invalidMessage }
        let parent = try directory(Array(path.dropLast())); defer { close(parent) }
        let temporary = ".trackB-\(UUID().uuidString)"
        let file = openat(parent,temporary,O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW,0o644)
        guard file >= 0 else { throw SteamError.files("Could not create the Steam install record.") }
        defer { close(file); unlinkat(parent,temporary,0) }
        try write(data,file:file)
        guard fsync(file) == 0, renameat(parent,temporary,parent,leaf) == 0, fsync(parent) == 0 else { throw SteamError.files("Could not save the Steam install record.") }
    }
}

struct BSteamChunkJob: Sendable {
    var depot: UInt32; var gid: UInt64; var path: [String]; var relative: String; var chunk: BSteamChunk
    var hadContent: Bool; var index: Int
    var recordID: Data {
        var data = Data(relative.utf8); data.append(0); data.bAppendLE(chunk.offset,count:8); data.bAppendLE(UInt64(chunk.original),count:4); data.append(chunk.sha)
        return BSteamCodec.sha1(data)
    }
}
struct BSteamPrepared: Sendable { var folder: URL; var jobs: [BSteamChunkJob]; var total: UInt64; var installed: [BSteamInstalledDepot]; var custom: [String]; var size: UInt64 }
actor BSteamInstallWorker {
    private struct FilePlan { var path: [String]; var file: BSteamManifestFile; var depot: UInt32; var old: UInt64 }
    private let steamApps: URL
    private let appID: UInt32
    private let folder: String
    private var disk: BSteamDisk?
    private var plans: [FilePlan] = []
    private var resume: [UInt32:Set<Data>] = [:]
    private var resumePaths: [UInt32:[String]] = [:]
    init(steamApps: URL, appID: UInt32, folder: String) { self.steamApps = steamApps; self.appID = appID; self.folder = folder }
    private func storage() throws -> BSteamDisk {
        if let disk = disk { return disk }; let value = try BSteamDisk(steamApps); disk = value; return value
    }
    private func resumePath(_ depot: UInt32, _ gid: UInt64) -> [String] { ["downloading",String(appID),"\(depot)_\(gid).chunks"] }
    private func loadResume(_ depot: UInt32, _ gid: UInt64) throws {
        let disk = try storage(), path = resumePath(depot,gid)
        let parent = try disk.directory(Array(path.dropLast())); defer { close(parent) }
        let directoryURL = steamApps.appendingPathComponent("downloading/\(appID)")
        for name in (try? FileManager.default.contentsOfDirectory(atPath:directoryURL.path)) ?? [] {
            if name.hasPrefix("\(depot)_"), name.hasSuffix(".chunks"), name != path.last, !name.contains("/") { unlinkat(parent,name,0) }
        }
        var finished: Set<Data> = []
        try disk.withFile(path,flags:O_RDWR | O_CREAT) { fd in
            var st = stat(); guard fstat(fd,&st) == 0, st.st_size >= 0, st.st_size <= 32*1024*1024 else { throw SteamError.invalidMessage }
            let data = try disk.read(fd,offset:0,size:Int(st.st_size))
            var p = 0
            while data.count-p >= 24 {
                let key = data.subdata(in:p..<(p+20)), check = UInt32(try data.bLE(p+20,4))
                if BSteamCodec.adler(key) != check { break }; finished.insert(key); p += 24
            }
            // Remove a torn tail before appending future records.
            if p != data.count && ftruncate(fd,off_t(p)) != 0 { throw SteamError.files("Could not repair download resume data.") }
        }
        resume[depot] = finished; resumePaths[depot] = path
    }
    func prepare(_ manifests: [BSteamManifest]) throws -> BSteamPrepared {
        let disk = try storage()
        let root = ["common",folder]
        let directory = try disk.directory(root); close(directory)
        var spellings: [String:String] = [:], jobs: [BSteamChunkJob] = [], total: UInt64 = 0, growth: UInt64 = 0, size: UInt64 = 0
        var invalid = 0, installed: [BSteamInstalledDepot] = [], custom: [String] = []
        for manifest in manifests {
            try Task.checkCancellation(); try loadResume(manifest.depot.id,manifest.gid)
            var depotSize: UInt64 = 0
            for file in manifest.files {
                if file.flags & 0x200 != 0 { continue }
                guard let parsed = BSteamDownloadPath.parts(file.name) else { invalid += 1; continue }
                let directory = file.flags & 0x40 != 0
                let parts = BSteamDownloadPath.fold(parsed,directory:directory,known:&spellings), path = root + parts
                if directory { let fd = try disk.directory(path); close(fd); continue }
                let old = try disk.existingSize(path) ?? 0
                let addition = file.size > old ? file.size-old : 0
                let g = growth.addingReportingOverflow(addition), s = size.addingReportingOverflow(file.size), ds = depotSize.addingReportingOverflow(file.size)
                guard !g.overflow, !s.overflow, !ds.overflow else { throw SteamError.invalidMessage }
                growth = g.partialValue; size = s.partialValue; depotSize = ds.partialValue
                plans.append(FilePlan(path:path,file:file,depot:manifest.depot.id,old:old))
                if file.flags & 0x80 != 0 && custom.count < 256 { custom.append(parts.joined(separator:"/")) }
                for chunk in file.chunks {
                    let sum = total.addingReportingOverflow(chunk.compressed); guard !sum.overflow else { throw SteamError.invalidMessage }; total = sum.partialValue
                    jobs.append(BSteamChunkJob(depot:manifest.depot.id,gid:manifest.gid,path:path,relative:parts.joined(separator:"/"),chunk:chunk,
                        hadContent:old >= chunk.offset+UInt64(chunk.original) && old > 0,index:jobs.count))
                    guard jobs.count <= 1_000_000 else { throw SteamError.invalidMessage }
                }
            }
            installed.append(BSteamInstalledDepot(id:manifest.depot.id,gid:manifest.gid,size:depotSize,owner:manifest.depot.owner))
            if manifest.files.contains(where:{ $0.flags & 0x80 != 0 }) {
                let cache = ["depotcache","\(manifest.depot.id)_\(manifest.gid).manifest"]
                if (try disk.existingSize(cache)) != UInt64(manifest.bytes.count) { try disk.atomic(manifest.bytes,path:cache) }
            }
        }
        let available = try? steamApps.resourceValues(forKeys:[.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage
        if let available = available, available >= 0, UInt64(available) < growth { throw SteamError.notEnoughSpace(needed:growth,available:UInt64(available)) }
        for plan in plans {
            try disk.withFile(plan.path,flags:O_RDWR | O_CREAT,createParents:true) { fd in
                guard ftruncate(fd,off_t(plan.file.size)) == 0 else { throw SteamError.files("Could not prepare a game file.") }
            }
        }
        if invalid > 0 { SteamLog.event("[steam-depot] skipped invalid paths count=\(invalid)") }
        return BSteamPrepared(folder:steamApps.appendingPathComponent("common/\(folder)"),jobs:jobs,total:total,installed:installed,custom:custom,size:size)
    }
    func beginDepot(_ id: UInt32) throws {
        let disk = try storage()
        // Deterministic depot order also handles manifests that overlay a common file.
        for plan in plans where plan.depot == id {
            try disk.withFile(plan.path,flags:O_RDWR) { fd in guard ftruncate(fd,off_t(plan.file.size)) == 0 else { throw SteamError.files("Could not prepare a game file.") } }
        }
    }
    func isFinished(_ job: BSteamChunkJob) throws -> Bool {
        guard job.hadContent || resume[job.depot]?.contains(job.recordID) == true else { return false }
        let disk = try storage()
        return (try? disk.withFile(job.path,flags:O_RDONLY) { fd in
            let bytes = try disk.read(fd,offset:job.chunk.offset,size:job.chunk.original); return BSteamCodec.sha1(bytes) == job.chunk.sha
        }) ?? false
    }
    func complete(_ job: BSteamChunkJob, bytes: Data?) throws {
        let disk = try storage()
        try disk.withFile(job.path,flags:O_RDWR) { fd in
            if let bytes = bytes { guard bytes.count == job.chunk.original else { throw SteamError.invalidMessage }; try disk.write(bytes,file:fd,offset:job.chunk.offset) }
            guard fsync(fd) == 0 else { throw SteamError.files("Could not save a game file.") }
        }
        let key = job.recordID
        if resume[job.depot]?.contains(key) == true { return }
        guard let path = resumePaths[job.depot] else { throw SteamError.invalidMessage }
        var record = key; record.bAppendLE(UInt64(BSteamCodec.adler(key)),count:4)
        try disk.withFile(path,flags:O_RDWR) { fd in
            var st = stat(); guard fstat(fd,&st) == 0, st.st_size >= 0, st.st_size <= 32*1024*1024-24 else { throw SteamError.invalidMessage }
            try disk.write(record,file:fd,offset:UInt64(st.st_size)); guard fsync(fd) == 0 else { throw SteamError.files("Could not save download resume data.") }
        }
        resume[job.depot,default:[]].insert(key)
    }
    func finish(_ prepared: BSteamPrepared, app: SteamAppInfo, owner: UInt64) throws {
        let disk = try storage(), now = UInt64(max(0,Date().timeIntervalSince1970))
        var written = 0, skipped = 0
        for id in Set(prepared.installed.compactMap(\.owner)).sorted() {
            guard let metadata = app.bOwners[id] else { skipped += 1; continue }
            let sourceFolder = metadata.folder.isEmpty ? "app_\(id)" : SteamInstallFiles.safeFolderName(metadata.folder)
            guard sourceFolder.caseInsensitiveCompare(folder) == .orderedSame else { skipped += 1; continue }
            var depots: [UInt32:BSteamInstalledDepot] = [:]
            if let old = SteamInstallFiles.bRecord(appID:Int(id),steamApps:steamApps) {
                for (key,value) in old["installeddepots"]?.fields ?? [:] {
                    if let depot = UInt32(key), let gid = value["manifest"]?.string.flatMap(UInt64.init), let size = value["size"]?.string.flatMap(UInt64.init) {
                        depots[depot] = BSteamInstalledDepot(id:depot,gid:gid,size:size,owner:nil)
                    }
                }
            }
            for d in prepared.installed where d.owner == id { depots[d.id] = BSteamInstalledDepot(id:d.id,gid:d.gid,size:d.size,owner:nil) }
            var total: UInt64 = 0
            for d in depots.values { let sum = total.addingReportingOverflow(d.size); guard !sum.overflow else { throw SteamError.invalidMessage }; total = sum.partialValue }
            let text = BSteamInstallRecord.text(id:id,name:metadata.name.isEmpty ? "App \(id)" : metadata.name,folder:folder,build:metadata.build,size:total,owner:owner,depots:Array(depots.values),custom:[],now:now)
            try disk.atomic(Data(text.utf8),path:["appmanifest_\(id).acf"]); written += 1
        }
        let text = BSteamInstallRecord.text(id:app.appID,name:app.name,folder:folder,build:app.buildID,size:prepared.size,owner:owner,depots:prepared.installed,custom:prepared.custom,now:now)
        try disk.atomic(Data(text.utf8),path:["appmanifest_\(app.appID).acf"])
        SteamLog.event("[steam-depot] shared owner records written=\(written) skipped=\(skipped)")
        // Fixed app-ID path, verified not to be a symlink before recursive removal.
        let parent = try disk.directory(["downloading"]); defer { close(parent) }
        var st = stat()
        if fstatat(parent,String(appID),&st,AT_SYMLINK_NOFOLLOW) == 0, st.st_mode & S_IFMT == S_IFDIR {
            try FileManager.default.removeItem(at:steamApps.appendingPathComponent("downloading/\(appID)"))
        }
    }
}
