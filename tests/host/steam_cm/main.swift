// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the CM list handed to Valve's client (App/Sources/Steam/SteamCMList.swift): the
// directory answer's UDP/TCP servers, and the edit of config.vdf that keeps everything else.

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String, line: Int = #line) {
    checks += 1
    if !ok { failures += 1; print("FAIL line \(line): \(what)") }
}

// MARK: - The directory's answer

let answer = """
{"response":{"serverlist":[
 {"endpoint":"162.254.199.181:27017","legacy_endpoint":"162.254.199.181:27017","type":"netfilter","dc":"atl3","realm":"steamglobal","load":14,"wtd_load":8.88},
 {"endpoint":"cmp1-iad1.steamserver.net:27018","type":"websockets","dc":"iad1","realm":"steamglobal","load":10,"wtd_load":5.0},
 {"endpoint":"162.254.192.101:27017","type":"netfilter","dc":"iad1","realm":"steamglobal","load":15,"wtd_load":3},
 {"endpoint":"155.133.1.2:27017","type":"netfilter","dc":"sha","realm":"steamchina","load":1,"wtd_load":1},
 {"endpoint":"162.254.199.181:27017","type":"netfilter","realm":"steamglobal","load":14,"wtd_load":8.88},
 {"endpoint":"1.2.3:27017","type":"netfilter","realm":"steamglobal"},
 {"endpoint":"162.254.193.74:27017","dc":"ord1"}
],"success":true,"message":""}}
"""
let endpoints = SteamCMList.netfilterEndpoints(directoryJSON: Data(answer.utf8))
check(endpoints == ["162.254.193.74:27017", "162.254.192.101:27017", "162.254.199.181:27017"],
      "netfilter, public realm, IPv4, once each, least loaded first: \(endpoints)")
check(SteamCMList.netfilterEndpoints(directoryJSON: Data("{}".utf8)).isEmpty, "no list")
check(SteamCMList.netfilterEndpoints(directoryJSON: Data("not json".utf8)).isEmpty, "not JSON")
check(SteamCMList.isIPEndpoint("10.0.0.1:27017") && !SteamCMList.isIPEndpoint("10.0.0.256:1") &&
      !SteamCMList.isIPEndpoint("host:27017") && !SteamCMList.isIPEndpoint("1.2.3.4") && !SteamCMList.isIPEndpoint("1.2.3.4:0"),
      "endpoint check")

// MARK: - config.vdf

let path = ["InstallConfigStore", "Software", "Valve", "Steam", "CM"]
let original = """
"InstallConfigStore"
{
\t"Software"
\t{
\t\t"valve"
\t\t{
\t\t\t"Steam"
\t\t\t{
\t\t\t\t"AutoUpdateWindowEnabled"\t\t"0"
\t\t\t\t"BaseInstallFolder_1"\t\t"D:\\\\SteamLibrary"
\t\t\t\t"CM"\t\t"1.1.1.1:27017"
\t\t\t\t"CMWebSocket"
\t\t\t\t{
\t\t\t\t\t"cmp1-iad1.steamserver.net:443"
\t\t\t\t\t{
\t\t\t\t\t\t"LastPingValue"\t\t"35"
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t}
\t// a comment
\t"Music"
\t{
\t\t"LocalLibrary"\t\t"C:\\\\Music \\"x\\""
\t}
}
"""
let edited = ValveVDF.setting(original, path: path, value: "2.2.2.2:27017;3.3.3.3:27017")!
let tree = ValveVDF.parse(edited)!
check(ValveVDF.value(tree, path: path) == "2.2.2.2:27017;3.3.3.3:27017", "CM replaced")
check(ValveVDF.value(tree, path: ["InstallConfigStore", "Software", "Valve", "Steam", "BaseInstallFolder_1"]) == "D:\\\\SteamLibrary",
      "an escaped value is kept exactly")
check(ValveVDF.value(tree, path: ["InstallConfigStore", "Music", "LocalLibrary"]) == "C:\\\\Music \\\"x\\\"", "escaped quotes kept")
check(ValveVDF.value(tree, path: ["InstallConfigStore", "Software", "valve", "Steam", "CMWebSocket", "cmp1-iad1.steamserver.net:443", "LastPingValue"]) == "35",
      "nested sections kept")
check(edited.contains("\"valve\""), "the key's own spelling is kept (matched without regard to case)")
check(edited.components(separatedBy: "\"CM\"").count == 2, "CM appears once")
check(ValveVDF.setting(edited, path: path, value: "2.2.2.2:27017;3.3.3.3:27017") == edited, "setting the same value again changes nothing")

let fresh = ValveVDF.setting(nil, path: path, value: "4.4.4.4:27017")!
check(ValveVDF.value(ValveVDF.parse(fresh)!, path: path) == "4.4.4.4:27017", "a missing file gets the sections")
let partial = ValveVDF.setting("\"InstallConfigStore\"\n{\n\t\"Software\"\n\t{\n\t}\n}\n", path: path, value: "5.5.5.5:27017")!
check(ValveVDF.value(ValveVDF.parse(partial)!, path: path) == "5.5.5.5:27017", "missing inner sections are created")
check(ValveVDF.setting("\"InstallConfigStore\"\n{\n\t\"Software\"\n", path: path, value: "x") == nil, "a broken file is left alone")
check(ValveVDF.setting("\"a\" \"b\" }", path: path, value: "x") == nil, "a stray brace is not taken")
check(ValveVDF.setting(original, path: path, value: "bad\"value") == nil, "a value with a quote is refused")
check(ValveVDF.parse("bare value // c\n\"k\" {\n}\n").map { $0.count } == 2, "bare tokens and comments")

print("steam cm host tests: \(checks - failures)/\(checks) passed")
exit(failures == 0 ? 0 : 1)
