// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
import Foundation

struct BSteamInstalledDepot: Sendable { var id: UInt32; var gid: UInt64; var size: UInt64; var owner: UInt32? }
enum BSteamInstallRecord {
    static func quote(_ value: String) -> String { "\"" + value.replacingOccurrences(of:"\\",with:"\\\\").replacingOccurrences(of:"\"",with:"\\\"") + "\"" }
    static func text(id: UInt32, name: String, folder: String, build: UInt32, size: UInt64, owner: UInt64,
                     depots: [BSteamInstalledDepot], custom: [String], now: UInt64) -> String {
        var out = "\"AppState\"\n{\n"
        func field(_ key: String, _ value: String, _ depth: Int = 1) { out += String(repeating:"\t",count:depth) + quote(key) + "\t\t" + quote(value) + "\n" }
        let fields = [("appid",String(id)),("universe","1"),("name",name),("StateFlags","4"),("installdir",folder),("LastUpdated",String(now)),("LastPlayed","0"),("SizeOnDisk",String(size)),("StagingSize","0"),("buildid",String(build)),("LastOwner",String(owner)),("DownloadType","1"),("UpdateResult","0"),("BytesToDownload","0"),("BytesDownloaded","0"),("BytesToStage","0"),("BytesStaged","0"),("TargetBuildID",String(build)),("AutoUpdateBehavior","0"),("AllowOtherDownloadsWhileRunning","0"),("ScheduledAutoUpdate","0")]
        for (key,value) in fields { field(key,value) }
        out += "\t\"InstalledDepots\"\n\t{\n"
        for d in depots.filter({ $0.owner == nil || $0.owner == id }).sorted(by:{ $0.id < $1.id }) {
            out += "\t\t\"\(d.id)\"\n\t\t{\n"; field("manifest",String(d.gid),3); field("size",String(d.size),3); out += "\t\t}\n"
        }
        out += "\t}\n"
        let shared = depots.filter { $0.owner != nil && $0.owner != id }.sorted { $0.id < $1.id }
        if !shared.isEmpty {
            out += "\t\"SharedDepots\"\n\t{\n"
            for d in shared { if let owner = d.owner { field(String(d.id),String(owner),2) } }; out += "\t}\n"
        }
        if !custom.isEmpty {
            out += "\t\"CheckGuid\"\n\t{\n"
            for (i,path) in custom.prefix(256).enumerated() { field(String(i),path.replacingOccurrences(of:"/",with:"\\"),2) }; out += "\t}\n"
        }
        out += "\t\"UserConfig\"\n\t{\n"; field("language","english",2); out += "\t}\n\t\"MountedConfig\"\n\t{\n"; field("language","english",2); out += "\t}\n}\n"
        return out
    }
}
