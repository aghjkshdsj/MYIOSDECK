// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import XCTest
import Foundation

@MainActor final class BSteamScriptedSession: BSteamSessionProtocol {
    var bSteamID: UInt64 = 0
    var bCellID: UInt32 = 0
    var requests: [UInt32] = []
    func bConnect() async throws {}
    func bLicenses() async throws -> [UInt32] { [123] }
    func bRequest(_ message: UInt32,body:Data,name:String?,timeout:Double,multipart:Bool) async throws -> [BSteamMessage] {
        requests.append(message); let request = try BSteamProto(body); var reply = BSteamProto()
        switch message {
        case 5438: reply.uint(1,1); reply.uint(2,request.number(1) ?? 0); reply.bytes(3,BSteamTestFixture.key)
        case 151:
            if name == "ContentServerDirectory.GetManifestRequestCode#1" { reply.fixed(1,123) }
            else if name == "ContentServerDirectory.GetCDNAuthToken#1" { reply.text(1,"") }
            else { throw SteamError.invalidMessage }
        case 8905:
            for (source,target) in [(1,1),(2,3)] {
                for id in try request.numbers(source) { var token = BSteamProto(); token.uint(1,id); token.uint(2,0); reply.bytes(target,token.encoded()) }
            }
        case 8903:
            for app in try request.messages(2) {
                var info = BSteamProto(); info.uint(1,app.number(1) ?? 0); info.bytes(5,Data(BSteamTestFixture.appInfo.utf8)); reply.bytes(1,info.encoded())
            }
            for package in try request.messages(1) {
                var binary = Data()
                for (name,id) in [("appids",UInt64(42)),("depotids",UInt64(100))] {
                    binary.append(0); binary.append(Data(name.utf8)); binary.append(0); binary.append(2); binary.append(contentsOf:[48,0]); binary.bAppendLE(id,count:4); binary.append(8)
                }
                binary.append(8)
                var info = BSteamProto(); info.uint(1,package.number(1) ?? 0); info.bytes(5,binary); reply.bytes(3,info.encoded())
            }
        default: throw SteamError.invalidMessage
        }
        return [BSteamMessage(id:message == 5438 ? 5439 : message == 8903 ? 8904 : message == 8905 ? 8906 : 147,body:reply.encoded())]
    }
}
actor BSteamScriptedContent: BSteamContentTransport {
    let manifest: Data
    let chunks: [String:Data]
    let cancelAt: Int?
    private(set) var fetched = 0
    private(set) var active = 0
    private(set) var peak = 0
    init(manifest:Data,chunks:[String:Data],cancelAt:Int? = nil) { self.manifest=manifest; self.chunks=chunks; self.cancelAt=cancelAt }
    func get(_ url:URL,limit:Int) async throws -> Data {
        if url.path.contains("GetServersForSteamPipe") { return Data(#"{"response":{"servers":[]}}"#.utf8) }
        if url.path.contains("/manifest/") { return manifest }
        guard url.path.contains("/chunk/"), let data = chunks[url.lastPathComponent], data.count <= limit else { throw SteamError.chunkFailed }
        fetched += 1; active += 1; peak = max(peak,active); defer { active -= 1 }
        if fetched == cancelAt { throw CancellationError() }
        try await Task.sleep(nanoseconds:10_000_000)
        return data
    }
}

final class SteamPipelineTests: XCTestCase {
    @MainActor func testScriptedLibraryAndInstallCancelResumeUpdate() async throws {
        let session = BSteamScriptedSession(), fetcher = SteamLibraryFetcher(session:session)
        let apps = try await fetcher.fetchOwnedApps(); XCTAssertEqual(apps.map(\.appID),[42])
        let depots = try await fetcher.ownedDepotIDs(); XCTAssertEqual(depots,[100])
        let requestCount = session.requests.count; _ = try await fetcher.ownedDepotIDs(); XCTAssertEqual(session.requests.count,requestCount)
        let optional = try await fetcher.fetchInstallInfo(appID:42); var app = try XCTUnwrap(optional)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("trackB-steam-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at:root) }
        let files = (0..<12).map { i -> (String,Data,UInt32) in
            (i == 0 ? "Bin/Game.exe" : "Assets/F\(i).bin",Data("payload-\(i)".utf8),i == 0 ? 0x80 : 0)
        }
        let (manifest,chunks) = try BSteamTestFixture.manifest(files)
        let interrupted = BSteamScriptedContent(manifest:manifest,chunks:chunks,cancelAt:9)
        let first = DepotDownloader(bSession:session,bContent:interrupted)
        do { _ = try await first.install(app,steamApps:root,ownedDepots:{ [100] },progress:{ _ in }); XCTFail("Expected cancellation") }
        catch is CancellationError {}
        XCTAssertFalse(FileManager.default.fileExists(atPath:root.appendingPathComponent("appmanifest_42.acf").path))
        let resume = root.appendingPathComponent("downloading/42/100_900.chunks")
        let valid = try Data(contentsOf:resume); XCTAssertGreaterThanOrEqual(valid.count,24)
        let handle = try FileHandle(forWritingTo:resume); _ = try handle.seekToEnd(); try handle.write(contentsOf:Data([1,2,3])); try handle.close()
        let stale = root.appendingPathComponent("downloading/42/100_1.chunks"); try Data([9]).write(to:stale)
        let transport = BSteamScriptedContent(manifest:manifest,chunks:chunks)
        let downloader = DepotDownloader(bSession:session,bContent:transport)
        var progress: [(Double,SteamDownloadProgress)] = []
        let folder = try await downloader.install(app,steamApps:root,ownedDepots:{ [100] },progress:{ progress.append((ProcessInfo.processInfo.systemUptime,$0)) })
        for (path,data,_) in files { XCTAssertEqual(try Data(contentsOf:folder.appendingPathComponent(path)),data) }
        let fetched = await transport.fetched, peak = await transport.peak
        XCTAssertLessThan(fetched,12); XCTAssertLessThanOrEqual(peak,8)
        XCTAssertEqual(SteamInstallFiles.buildID(appID:42,steamApps:root),1234)
        XCTAssertFalse(FileManager.default.fileExists(atPath:root.appendingPathComponent("downloading/42").path))
        XCTAssertTrue(FileManager.default.fileExists(atPath:root.appendingPathComponent("depotcache/100_900.manifest").path))
        XCTAssertEqual(progress.first?.1.phase,.preparing); XCTAssertEqual(progress.last?.1.phase,.finishing)
        for i in 1..<progress.count { XCTAssertGreaterThanOrEqual(progress[i].0-progress[i-1].0,0.249) }
        var updated = files; updated[0].1 = Data("changed".utf8)
        let (nextManifest,nextChunks) = try BSteamTestFixture.manifest(updated)
        app.bDepots = BSteamDepotSelection.select(app.bDepots); app.bDepots[0].manifests["public"] = 901; app.buildID = 1235
        let updateContent = BSteamScriptedContent(manifest:nextManifest,chunks:nextChunks)
        let update = DepotDownloader(bSession:session,bContent:updateContent)
        _ = try await update.install(app,steamApps:root,ownedDepots:{ [100] },progress:{ _ in })
        let updateFetches = await updateContent.fetched
        XCTAssertEqual(updateFetches,1); XCTAssertEqual(SteamInstallFiles.buildID(appID:42,steamApps:root),1235)
        XCTAssertEqual(try Data(contentsOf:folder.appendingPathComponent("Bin/Game.exe")),Data("changed".utf8))
        SteamInstallFiles.delete(appID:42,folderName:"Sample",steamApps:root)
        XCTAssertFalse(FileManager.default.fileExists(atPath:folder.path)); XCTAssertNil(SteamInstallFiles.buildID(appID:42,steamApps:root))
    }
    @MainActor func testSharedOwnerRecordMerge() async throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("trackB-owner-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at:root) }
        try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
        let old = BSteamInstallRecord.text(id:99,name:"Old",folder:"Sample",build:1,size:4,owner:0,depots:[BSteamInstalledDepot(id:777,gid:3,size:4,owner:nil)],custom:[],now:1)
        try Data(old.utf8).write(to:root.appendingPathComponent("appmanifest_99.acf"))
        let session = BSteamScriptedSession(); var app = try XCTUnwrap(BSteamAppParser.parse(Data(BSteamTestFixture.appInfo.utf8),appID:42))
        app.bDepots = BSteamDepotSelection.select(app.bDepots); app.bDepots[0].owner = 99; app.bOwners[99] = BSteamOwner(name:"Owner",folder:"sample",build:7)
        let (manifest,chunks) = try BSteamTestFixture.manifest([("Game.exe",Data("abc".utf8),0)])
        let downloader = DepotDownloader(bSession:session,bContent:BSteamScriptedContent(manifest:manifest,chunks:chunks))
        _ = try await downloader.install(app,steamApps:root,ownedDepots:{ [100] },progress:{ _ in })
        let record = try XCTUnwrap(SteamInstallFiles.bRecord(appID:99,steamApps:root))
        XCTAssertEqual(record["installeddepots"]?["777"]?["manifest"]?.string,"3")
        XCTAssertEqual(record["installeddepots"]?["100"]?["manifest"]?.string,"900")
        XCTAssertEqual(record["sizeondisk"]?.string,"7"); XCTAssertEqual(record["buildid"]?.string,"7")
    }
    func testDirectChoiceAndDeletionConfinement() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("trackB-launch-test-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at:root) }
        let install = root.appendingPathComponent("common/Title"), bin = install.appendingPathComponent("Bin")
        try FileManager.default.createDirectory(at:bin,withIntermediateDirectories:true); try Data([1]).write(to:bin.appendingPathComponent("Game.EXE"))
        let options = [SteamLaunchOption(executable:"missing.exe",type:"default",osarch:"64"),SteamLaunchOption(executable:"bin/game.exe",index:2,arguments:" -safe ",workingDir:".",type:"default",oslist:"windows",osarch:"64"),SteamLaunchOption(executable:"Bin/Game.EXE",type:"server")]
        let choice = try XCTUnwrap(SteamDirectStart.choose(options,installFolder:install))
        XCTAssertEqual(choice.program,"Bin/Game.EXE"); XCTAssertEqual(choice.folder,""); XCTAssertEqual(choice.arguments,"-safe"); XCTAssertEqual(choice.launchIndex,2)
        XCTAssertNil(SteamDirectStart.choose([SteamLaunchOption(executable:"../escape.exe")],installFolder:install))
        let outside = root.appendingPathComponent("outside"); try FileManager.default.createDirectory(at:outside,withIntermediateDirectories:true)
        let protected = outside.appendingPathComponent("keep"); try Data([7]).write(to:protected)
        let symlink = root.appendingPathComponent("common/Linked"); try FileManager.default.createSymbolicLink(at:symlink,withDestinationURL:outside)
        SteamInstallFiles.delete(appID:42,folderName:"Linked",steamApps:root)
        XCTAssertTrue(FileManager.default.fileExists(atPath:protected.path))
        let escaped = bin.appendingPathComponent("Escaped.exe"); try FileManager.default.createSymbolicLink(at:escaped,withDestinationURL:protected)
        XCTAssertNil(SteamDirectStart.choose([SteamLaunchOption(executable:"Bin/Escaped.exe")],installFolder:install))
    }
}
