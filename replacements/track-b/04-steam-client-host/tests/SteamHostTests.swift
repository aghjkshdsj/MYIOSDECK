// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import XCTest
import Foundation

final class SteamHostTests: XCTestCase {
    private func prefix() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("trackB-host-test-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true); return root
    }
    private func addClient(_ root: URL) throws {
        let exe = BSteamHostPolicy.client(root)
        try FileManager.default.createDirectory(at:exe.deletingLastPathComponent(),withIntermediateDirectories:true)
        try Data([1]).write(to:exe)
    }
    func testLaunchBuildingAndRecord() throws {
        let root = try prefix(); defer { try? FileManager.default.removeItem(at:root); UserDefaults.standard.removeObject(forKey:BSteamHostPolicy.noticeKey(root)) }
        let defaults = UserDefaults.standard, original = defaults.object(forKey:"steamhost.gpu")
        defer { if let original = original { defaults.set(original,forKey:"steamhost.gpu") } else { defaults.removeObject(forKey:"steamhost.gpu") } }
        XCTAssertFalse(SteamHost.clientInstalled(prefix:root)); try addClient(root); XCTAssertTrue(SteamHost.clientInstalled(prefix:root))
        defaults.removeObject(forKey:"steamhost.gpu")
        let base = try SteamHost.openClientLaunch(prefix:root)
        XCTAssertEqual(base.exe,"C:\\Program Files (x86)\\Steam\\steam.exe"); XCTAssertEqual(base.workingFolder,SteamHost.windowsRoot)
        XCTAssertEqual(base.arguments,"-no-cef-sandbox -cef-disable-gpu -cef-disable-gpu-compositing")
        XCTAssertEqual(base.environment,["MADEIRA_DOCK_SESSION":"1"])
        defaults.set("1",forKey:"steamhost.gpu"); XCTAssertEqual(try SteamHost.openClientLaunch(prefix:root).arguments,"-no-cef-sandbox")
        defaults.set(1,forKey:"steamhost.gpu"); XCTAssertEqual(try SteamHost.openClientLaunch(prefix:root).arguments,"-no-cef-sandbox")
        for id in [0,-1,4_294_967_295,Int.max] { XCTAssertThrowsError(try SteamHost.playLaunch(appID:id,prefix:root)) { XCTAssertEqual($0 as? SteamHostError,.invalidGame) } }
        XCTAssertThrowsError(try SteamHost.playLaunch(appID:42,prefix:root)) { XCTAssertEqual($0 as? SteamHostError,.notInstalledForClient) }
        let apps = SteamInstallPaths.steamApps(drive:root.appendingPathComponent("drive_c"))
        try FileManager.default.createDirectory(at:apps,withIntermediateDirectories:true)
        for id in [1,42,4_294_967_294] {
            let text = "\"AppState\" { \"appid\" \"\(id)\" \"buildid\" \"7\" }"
            try Data(text.utf8).write(to:apps.appendingPathComponent("appmanifest_\(id).acf"))
            let launch = try SteamHost.playLaunch(appID:id,prefix:root)
            XCTAssertEqual(launch.arguments,"-no-cef-sandbox -applaunch \(id)"); XCTAssertEqual(launch.environment,base.environment)
        }
        try Data("\"AppState\" { \"appid\" \"99\" \"buildid\" \"7\" }".utf8).write(to:apps.appendingPathComponent("appmanifest_42.acf"))
        XCTAssertThrowsError(try SteamHost.playLaunch(appID:42,prefix:root))
    }
    func testInstallerValidationAndRedirectPolicy() throws {
        XCTAssertFalse(BSteamHostPolicy.validPE(Data("not PE".utf8)))
        var minimal = Data(count:88); minimal[0]=0x4d; minimal[1]=0x5a; minimal[0x3c]=64
        minimal[64]=0x50; minimal[65]=0x45; minimal[68]=0x4c; minimal[69]=1
        XCTAssertTrue(BSteamHostPolicy.validPE(minimal)); XCTAssertThrowsError(try BSteamHostPolicy.validate(minimal))
        var installer = minimal; installer.append(Data(count:BSteamHostPolicy.minimum-minimal.count))
        XCTAssertNoThrow(try BSteamHostPolicy.validate(installer))
        XCTAssertThrowsError(try BSteamHostPolicy.validateSize(BSteamHostPolicy.minimum-1))
        XCTAssertThrowsError(try BSteamHostPolicy.validateSize(BSteamHostPolicy.maximum+1))
        installer[0x3f]=255; XCTAssertFalse(BSteamHostPolicy.validPE(installer))
        for string in ["https://cdn.akamai.steamstatic.com/a","https://a.steampowered.com/x","https://a.steamstatic.com:8443/x"] {
            XCTAssertTrue(BSteamHostPolicy.redirectAllowed(try XCTUnwrap(URL(string:string))))
        }
        for string in ["http://a.steamstatic.com/x","https://a.steamstatic.com.evil.test/x","https://evil.test/x","https://user:pass@a.steamstatic.com/x"] {
            XCTAssertFalse(BSteamHostPolicy.redirectAllowed(try XCTUnwrap(URL(string:string))))
        }
    }
    func testFinishAndFirstUpdateFlag() throws {
        let root = try prefix(), key = BSteamHostPolicy.noticeKey(root)
        let priorNote = UserDefaults.standard.object(forKey:"steamhost.noticeText")
        defer {
            try? FileManager.default.removeItem(at:root); UserDefaults.standard.removeObject(forKey:key)
            if let priorNote = priorNote { UserDefaults.standard.set(priorNote,forKey:"steamhost.noticeText") }
            else { UserDefaults.standard.removeObject(forKey:"steamhost.noticeText") }
        }
        let installer = BSteamHostPolicy.installer(root)
        try FileManager.default.createDirectory(at:installer.deletingLastPathComponent(),withIntermediateDirectories:true)
        try Data([1]).write(to:installer)
        XCTAssertThrowsError(try SteamHost.finishInstall(prefix:root)) { XCTAssertEqual($0 as? SteamHostError,.installationDidNotFinish) }
        XCTAssertFalse(FileManager.default.fileExists(atPath:installer.path))
        try addClient(root); try SteamHost.finishInstall(prefix:root); XCTAssertTrue(UserDefaults.standard.bool(forKey:key))
        _ = try SteamHost.openClientLaunch(prefix:root); XCTAssertFalse(UserDefaults.standard.bool(forKey:key))
        XCTAssertEqual(UserDefaults.standard.string(forKey:"steamhost.noticeText"),BSteamHostPolicy.firstUpdateNote)
    }
    func testPrepareRejectsBeforeNetwork() async throws {
        let root = try prefix(); defer { try? FileManager.default.removeItem(at:root) }
        try addClient(root)
        do { _ = try await SteamHost.prepareInstall(prefix:root,progress:{ _ in }); XCTFail("Expected already installed") }
        catch { XCTAssertEqual(error as? SteamHostError,.alreadyInstalled) }
        do { _ = try await SteamHost.prepareInstall(prefix:root.appendingPathComponent("missing"),progress:{ _ in }); XCTFail("Expected prefix missing") }
        catch { XCTAssertEqual(error as? SteamHostError,.prefixMissing) }
    }
}
