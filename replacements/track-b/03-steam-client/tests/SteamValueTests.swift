// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import XCTest
import Foundation

enum BSteamTestFixture {
    static let key = Data(repeating:7,count:32)
    static func encrypted(_ data: Data) throws -> Data {
        var out = Data(count:data.count+64)
        let capacity = out.count
        let count = out.withUnsafeMutableBytes { destination in data.withUnsafeBytes { source in key.withUnsafeBytes { k in
            bsteam_test_encrypt(source.baseAddress,data.count,k.baseAddress,destination.baseAddress,capacity)
        } } }
        guard count > 0 else { throw SteamError.invalidMessage }; out.count = count; return out
    }
    static func manifest(_ files: [(String,Data,UInt32)]) throws -> (Data,[String:Data]) {
        var payload = BSteamProto(), chunks: [String:Data] = [:]
        for (name,data,flags) in files {
            let encrypted = try encrypted(data), sha = BSteamCodec.sha1(data)
            var chunk = BSteamProto(); chunk.bytes(1,sha); chunk.fields[2] = [.fixed32(BSteamCodec.adler(data))]
            chunk.uint(3,0); chunk.uint(4,UInt64(data.count)); chunk.uint(5,UInt64(encrypted.count))
            var file = BSteamProto(); file.text(1,name); file.uint(2,UInt64(data.count)); file.uint(3,UInt64(flags)); file.bytes(6,chunk.encoded())
            payload.bytes(1,file.encoded()); chunks[sha.map { String(format:"%02x",$0) }.joined()] = encrypted
        }
        return (payload.encoded(),chunks)
    }
    static let appInfo = #"""
    "42" { "COMMON" { "name" "Sample Game" "type" "Game" "oslist" "windows,linux" "parent" "99"
      "library_assets_full" { "library_capsule" { "image2x" { "french" "fr.png" "english" "en.png" } } }
      "header_image" { "english" "header.jpg" } }
      "config" { "installdir" "Sample" "launch" {
        "2" { "executable" "Game32.exe" "config" { "osarch" "32" } }
        "0" { "executable" "Bin\\Game.exe" "arguments" " -safe " "workingdir" "." "type" "default" "config" { "oslist" "windows" "osarch" "64" "ownsdlc" "0" } }
      } }
      "depots" {
        "100" { "config" { "oslist" "windows" "osarch" "64" "language" "english" } "manifests" { "public" { "gid" "900" "size" "12" "download" "32" } } }
        "101" { "config" { "oslist" "windows" "osarch" "32" } "manifests" { "public" "901" } }
        "branches" { "public" { "buildid" "1234" } }
      }
    }
    """#
}

final class SteamValueTests: XCTestCase {
    func testTextKVAndLimits() throws {
        var p = try SteamKeyValues(Data([0xef,0xbb,0xbf])+Data(#"""
        // comment
        ROOT { "Path" "C:\Games\file.exe" braces "{" quote "a\"b" slash "a\\b" }
        """#.utf8))
        let value = try p.read()
        XCTAssertEqual(value["root"]?["PATH"]?.string,#"C:\Games\file.exe"#)
        XCTAssertEqual(value["root"]?["braces"]?.string,"{")
        XCTAssertEqual(value["root"]?["quote"]?.string,"a\"b")
        XCTAssertEqual(value["root"]?["slash"]?.string,#"a\b"#)
        for text in ["}","key { a b", "key", "key \"unterminated", "key \""+String(repeating:"x",count:16_385)+"\""] {
            var parser = try SteamKeyValues(Data(text.utf8)); XCTAssertThrowsError(try parser.read())
        }
        var deep = try SteamKeyValues(Data((String(repeating:"a { ",count:33)+String(repeating:"}",count:33)).utf8))
        XCTAssertThrowsError(try deep.read())
        XCTAssertThrowsError(try SteamKeyValues(Data(count:4*1024*1024+1)))
    }
    func testBinaryKVAndProtobuf() throws {
        var binary = Data([0]); binary.append(Data("appids".utf8)); binary.append(0); binary.append(2); binary.append(Data("0".utf8)); binary.append(0); binary.bAppendLE(42,count:4); binary.append(contentsOf:[8,8])
        var reader = try BSteamBinaryKV(binary); let value = try reader.read()
        XCTAssertEqual(BSteamAppParser.integers("appids",in:value),[42])
        var broken = try BSteamBinaryKV(Data([0,97,0,2,98,0,1])); XCTAssertThrowsError(try broken.read())
        var proto = BSteamProto(); proto.uint(1,UInt64.max); proto.fixed(2,123); proto.text(3,"hello"); proto.fields[4] = [.fixed32(0xabcdef01)]
        let decoded = try BSteamProto(proto.encoded())
        XCTAssertEqual(decoded.number(1),UInt64.max); XCTAssertEqual(decoded.number(2),123); XCTAssertEqual(decoded.string(3),"hello"); XCTAssertEqual(decoded.number(4),0xabcdef01)
        XCTAssertThrowsError(try BSteamProto(Data([8])+Data(repeating:255,count:10)))
        XCTAssertThrowsError(try BSteamProto(Data([0])))
        XCTAssertThrowsError(try BSteamProto(Data([10,127,1])))
    }
    func testAppMetadataLaunchesAndDepotChoice() throws {
        let optional = try BSteamAppParser.parse(Data(BSteamTestFixture.appInfo.utf8),appID:42)
        let app = try XCTUnwrap(optional)
        XCTAssertEqual(app.name,"Sample Game"); XCTAssertEqual(app.buildID,1234); XCTAssertEqual(app.parentID,99)
        XCTAssertEqual(app.libraryCapsule,"en.png"); XCTAssertEqual(app.headerImage,"header.jpg"); XCTAssertTrue(app.installableOnWindows)
        XCTAssertEqual(app.launches.map(\.index),[0,2]); XCTAssertEqual(app.launches.first?.requiredDLC,"0")
        XCTAssertEqual(BSteamDepotSelection.select(app.bDepots).map(\.id),[100])
        var depots = app.bDepots; depots[0].language = "french"
        XCTAssertEqual(BSteamDepotSelection.select(depots).map(\.id),[101])
        depots[1].lowViolence = true; XCTAssertTrue(BSteamDepotSelection.select(depots).isEmpty)
        let old = try JSONDecoder().decode(SteamLaunchOption.self,from:Data(#"{"executable":"a.exe"}"#.utf8))
        XCTAssertEqual(old.arguments,""); XCTAssertNil(old.index); XCTAssertEqual(old.osarch,"")
        XCTAssertEqual(try JSONDecoder().decode(SteamLaunchOption.self,from:JSONEncoder().encode(old)),old)
    }
    func testManifestCryptoAndCodecs() throws {
        let plain = Data("abc".utf8), encrypted = try BSteamTestFixture.encrypted(plain)
        XCTAssertEqual(try BSteamCodec.decrypt(encrypted,key:BSteamTestFixture.key),plain)
        XCTAssertEqual(try BSteamCodec.decode(plain,expected:3),plain)
        XCTAssertEqual(BSteamCodec.adler(plain),0x024a0126)
        XCTAssertEqual(BSteamCodec.sha1(plain).map { String(format:"%02x",$0) }.joined(),"a9993e364706816aba3e25717850c26c9cd0d89d")
        XCTAssertThrowsError(try BSteamCodec.decrypt(Data([1]),key:BSteamTestFixture.key))
        let frame = Data([0x28,0xb5,0x2f,0xfd,0x20,3,0x19,0,0])+plain
        let vsza = Data([0x56,0x53,0x5a,0x61,0,0,0,0])+frame+Data(count:15)
        XCTAssertEqual(try BSteamCodec.decode(vsza,expected:3),plain)
        var corrupt = vsza; corrupt[8] = 0; XCTAssertThrowsError(try BSteamCodec.decode(corrupt,expected:3))
        var vz = Data(count:4096)
        let count = vz.withUnsafeMutableBytes { dst in plain.withUnsafeBytes { src in bsteam_test_vz(src.baseAddress,plain.count,dst.baseAddress,4096) } }
        XCTAssertGreaterThan(count,0); vz.count = count
        XCTAssertEqual(try BSteamCodec.decode(vz,expected:3),plain)
        XCTAssertThrowsError(try BSteamCodec.decode(vz,expected:4))
        let (data,_) = try BSteamTestFixture.manifest([("Assets\\a.bin",plain,0)])
        let parsed = try BSteamManifest.parse(data,key:BSteamTestFixture.key,depot:BSteamDepot(id:100),gid:900)
        XCTAssertEqual(parsed.files.first?.name,"Assets/a.bin"); XCTAssertEqual(parsed.files.first?.chunks.first?.crc,0x024a0126)
        var envelope = Data(); envelope.bAppendLE(0x71f617d0,count:4); envelope.bAppendLE(UInt64(data.count),count:4); envelope.append(data); envelope.append(contentsOf:[1,2,3])
        XCTAssertEqual(try BSteamManifest.parse(envelope,key:BSteamTestFixture.key,depot:BSteamDepot(id:100),gid:900).files.count,1)
    }
    func testPathsRecordsAndJWT() throws {
        for invalid in ["/x","a//b","a/../b","a/./b","C:/x","a\u{0}b",String(repeating:"x",count:1025)] { XCTAssertNil(BSteamDownloadPath.parts(invalid)) }
        var known: [String:String] = [:]
        XCTAssertEqual(BSteamDownloadPath.fold(["Assets","Textures","a"],directory:false,known:&known),["Assets","Textures","a"])
        XCTAssertEqual(BSteamDownloadPath.fold(["assets","textures","b"],directory:false,known:&known),["Assets","Textures","b"])
        XCTAssertEqual(SteamInstallFiles.safeFolderName("C:\\games\\Title"),"Title"); XCTAssertEqual(SteamInstallFiles.safeFolderName("x/.."),"app")
        let depots = [BSteamInstalledDepot(id:10,gid:20,size:3,owner:nil),BSteamInstalledDepot(id:12,gid:21,size:4,owner:99)]
        let text = BSteamInstallRecord.text(id:42,name:"A \"Game\"",folder:"Title",build:3,size:7,owner:123,depots:depots,custom:["Bin/Game.exe"],now:100)
        XCTAssertTrue(text.contains("\t\"universe\"\t\t\"1\"\n")); XCTAssertFalse(text.contains("\"Universe\""))
        var parser = try SteamKeyValues(Data(text.utf8)); let record = try parser.read()["appstate"]
        XCTAssertEqual(record?["bytesdownloaded"]?.string,"0"); XCTAssertEqual(record?["installeddepots"]?["10"]?["manifest"]?.string,"20")
        XCTAssertEqual(record?["shareddepots"]?["12"]?.string,"99"); XCTAssertEqual(record?["checkguid"]?["0"]?.string,"Bin\\Game.exe")
        func jwt(_ value: String) -> String { "header."+Data(value.utf8).base64EncodedString().replacingOccurrences(of:"=",with:"").replacingOccurrences(of:"+",with:"-").replacingOccurrences(of:"/",with:"_")+".signature" }
        XCTAssertTrue(BSteamTokenStore.expired(jwt(#"{"exp":100}"#),now:Date(timeIntervalSince1970:100)))
        XCTAssertFalse(BSteamTokenStore.expired(jwt(#"{"exp":101}"#),now:Date(timeIntervalSince1970:100)))
        XCTAssertFalse(BSteamTokenStore.expired(jwt(#"{"exp":true}"#),now:Date(timeIntervalSince1970:100)))
        XCTAssertFalse(BSteamTokenStore.expired("opaque"))
        var progress = SteamDownloadProgress(); XCTAssertEqual(progress.fraction,0); progress.totalBytes=2; progress.doneBytes=9; XCTAssertEqual(progress.fraction,1)
    }
    func testWireAndResolveOnce() async throws {
        var header = BSteamProto(); header.fixed(10,123); header.fixed(11,456)
        let encoded = BSteamMessage(id:147,header:header,body:Data([1,2])).encoded(), decoded = try BSteamMessage(encoded)
        XCTAssertEqual(decoded.id,147); XCTAssertEqual(decoded.header.number(11),456)
        var simple = Data(); simple.bAppendLE(751,count:4); simple.bAppendLE(456,count:8); simple.bAppendLE(123,count:8); simple.bAppendLE(1,count:4)
        XCTAssertEqual(try BSteamMessage(simple).body.bLE(0,4),1)
        var payload = Data(); payload.bAppendLE(UInt64(encoded.count),count:4); payload.append(encoded)
        var multi = BSteamProto(); multi.bytes(2,payload)
        XCTAssertEqual(try BSteamWire.messages(BSteamMessage(id:1,body:multi.encoded()).encoded()).count,1)
        let promise = BSteamPromise<Int>(); async let a = promise.value(); async let b = promise.value()
        promise.resolve(.success(7)); promise.resolve(.failure(SteamError.timeout))
        let values = try await (a,b); XCTAssertEqual(values.0,7); XCTAssertEqual(values.1,7)
        let never = BSteamPromise<Int>()
        do { _ = try await bSteamTimeout(0.01) { try await never.value() }; XCTFail("Expected timeout") }
        catch { XCTAssertEqual(error as? SteamError,.timeout) }
        never.resolve(.success(1)) // Release the deliberately pending operation.
    }
    func testContentServerValidation() throws {
        for host in ["x.steamcontent.com","a.akamaized.net","cdn.steamstatic.com"] { XCTAssertTrue(BSteamContentServers.valid(host)) }
        for host in ["steamcontent.com.evil.test","a.steamcontent.com:443","a/steamcontent.com","*.steamcontent.com","lancache.steamcontent.com"] { XCTAssertFalse(BSteamContentServers.valid(host)) }
        let data = Data(#"{"response":{"servers":[{"vhost":"a.steamcontent.com","https_support":"mandatory"},{"host":"b.steamstatic.com","https_support":"optional","allowed_app_ids":[9]},{"host":"evil.test","https_support":"mandatory"}]}}"#.utf8)
        XCTAssertEqual(try BSteamContentServers.parse(data,appID:42),["https://a.steamcontent.com",BSteamContentServers.fallback])
    }
}
