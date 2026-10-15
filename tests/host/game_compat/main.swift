// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for the launch-time compatibility checks (App/Sources/Engine/GameCompatCore.swift):
// steam_appid.txt for a Steam game started without Steam, the engine named in the log, an Adobe AIR
// game's runtime, and Windows paths on drive C:.

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String, line: Int = #line) {
    checks += 1
    if !ok { failures += 1; print("FAIL line \(line): \(what)") }
}

let fm = FileManager.default
let root = fm.temporaryDirectory.appendingPathComponent("game-compat-\(getpid())")
try? fm.removeItem(at: root)
func mk(_ rel: String, _ contents: Data = Data()) {
    let url = root.appendingPathComponent(rel)
    try! fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try! contents.write(to: url)
}
func dir(_ rel: String) -> URL { root.appendingPathComponent(rel, isDirectory: true) }
// A PE header with this machine (MZ, e_lfanew = 0x80, "PE\0\0", machine).
func pe(_ machine: UInt16) -> Data {
    var d = Data(count: 0x100)
    d[0] = 0x4d; d[1] = 0x5a; d[0x3c] = 0x80
    d[0x80] = 0x50; d[0x81] = 0x45; d[0x84] = UInt8(machine & 0xff); d[0x85] = UInt8(machine >> 8)
    return d
}

// MARK: - steam_appid.txt

// 911 Operator: Unity, steam_api in <Game>_Data/Plugins, program and working folder the install folder.
mk("op/911.exe", pe(0x14c))
mk("op/911_Data/Plugins/steam_api.dll")
var folders = GameCompat.steamAppIDFolders(installFolder: dir("op"), programFolder: dir("op"), workingFolder: dir("op"))
check(folders.map(\.lastPathComponent) == ["op"], "one folder when program and working folder are the same: \(folders)")
let written = GameCompat.writeSteamAppID(503560, folders: folders)
check(written.count == 1 && (try? String(contentsOf: written[0], encoding: .utf8)) == "503560\n", "the App ID and a newline")
folders = GameCompat.steamAppIDFolders(installFolder: dir("op"), programFolder: dir("op"), workingFolder: dir("op"))
check(folders.isEmpty, "nothing once the file is there")

// The game's own file is never replaced, whatever its case.
mk("own/Game.exe", pe(0x8664)); mk("own/steam_api64.dll"); mk("own/Steam_AppID.TXT", Data("480".utf8))
check(GameCompat.steamAppIDFolders(installFolder: dir("own"), programFolder: dir("own"), workingFolder: dir("own")).isEmpty,
      "a steam_appid.txt in any case is kept")
check((try? String(contentsOf: root.appendingPathComponent("own/Steam_AppID.TXT"), encoding: .utf8)) == "480", "untouched")

// Program in bin/, working folder the install folder: both get one.
mk("two/bin/game.exe", pe(0x8664)); mk("two/bin/steam_api64.dll")
folders = GameCompat.steamAppIDFolders(installFolder: dir("two"), programFolder: dir("two/bin"), workingFolder: dir("two"))
check(folders.map(\.lastPathComponent).sorted() == ["bin", "two"], "program and working folder: \(folders)")

// No Steamworks (a DRM-free build, Forager): nothing written.
mk("drmfree/Forager.exe", pe(0x14c)); mk("drmfree/data.win")
check(GameCompat.steamAppIDFolders(installFolder: dir("drmfree"), programFolder: dir("drmfree"), workingFolder: dir("drmfree")).isEmpty,
      "no steam_api: no file")
// steam_api deeper than the search: not found (the search stays small).
mk("deep/a/b/c/d/e/steam_api.dll"); mk("deep/game.exe")
check(!GameCompat.shipsSteamAPI(dir("deep")), "five levels down is past the search")
check(GameCompat.shipsSteamAPI(dir("op")), "Unity plugin folder found")
// A working folder that does not exist is skipped.
check(GameCompat.steamAppIDFolders(installFolder: dir("op2"), programFolder: dir("op2"), workingFolder: dir("op2")).isEmpty, "missing install folder")

// MARK: - Engine

mk("vs/VampireSurvivors.exe", pe(0x8664)); mk("vs/GameAssembly.dll"); mk("vs/UnityPlayer.dll")
check(GameCompat.engine(programFolder: dir("vs")) == "Unity (IL2CPP)", "IL2CPP: \(GameCompat.engine(programFolder: dir("vs")))")
mk("hk/hollow_knight.exe", pe(0x8664)); mk("hk/UnityPlayer.dll"); mk("hk/hollow_knight_Data/Managed/Assembly-CSharp.dll")
check(GameCompat.engine(programFolder: dir("hk")) == "Unity (Mono)", "Mono: \(GameCompat.engine(programFolder: dir("hk")))")
mk("old/game.exe", pe(0x14c)); mk("old/game_Data/il2cpp_data/Metadata/global-metadata.dat")
check(GameCompat.engine(programFolder: dir("old")) == "Unity (IL2CPP)", "IL2CPP by its data folder")
check(GameCompat.engine(programFolder: dir("drmfree")) == "GameMaker", "GameMaker: data.win")
mk("fna/Game.exe", pe(0x14c)); mk("fna/FNA.dll")
check(GameCompat.engine(programFolder: dir("fna")) == "FNA", "FNA")
mk("xna/Game.exe", pe(0x14c)); mk("xna/Microsoft.Xna.Framework.Game.dll")
check(GameCompat.engine(programFolder: dir("xna")) == "XNA", "XNA")
mk("ue/MyGame/Binaries/Win64/MyGame-Win64-Shipping.exe", pe(0x8664))
check(GameCompat.engine(programFolder: dir("ue/MyGame/Binaries/Win64")) == "Unreal", "Unreal")
mk("godot/game.exe", pe(0x8664)); mk("godot/game.pck")
check(GameCompat.engine(programFolder: dir("godot")) == "Godot", "Godot")
mk("sdl/game.exe", pe(0x8664)); mk("sdl/SDL2.dll")
check(GameCompat.engine(programFolder: dir("sdl")) == "SDL", "SDL")
check(GameCompat.engine(programFolder: dir("deep")) == "unknown", "unknown")

// MARK: - Adobe AIR

mk("air/Transformice.exe", pe(0x14c)); mk("air/META-INF/AIR/application.xml")
check(GameCompat.engine(programFolder: dir("air")) == "Adobe AIR", "AIR engine")
check(GameCompat.airRuntime(programFolder: dir("air"))?.hasPrefix("no captive runtime") == true,
      "no runtime: \(GameCompat.airRuntime(programFolder: dir("air")) ?? "nil")")
mk("air/Adobe AIR/Versions/1.0/Adobe AIR.dll", pe(0x14c)); mk("air/Adobe AIR/Versions/1.0/Resources/x.txt")
check(GameCompat.airRuntime(programFolder: dir("air")) == "captive runtime Adobe AIR.dll (32-bit, with Resources)",
      "captive 32-bit: \(GameCompat.airRuntime(programFolder: dir("air")) ?? "nil")")
mk("air64/game.exe", pe(0x8664)); mk("air64/meta-inf/air/APPLICATION.XML"); mk("air64/adobe air/versions/1.0/adobe air.dll", pe(0x8664))
check(GameCompat.airRuntime(programFolder: dir("air64")) == "captive runtime Adobe AIR.dll (64-bit, no Resources folder)",
      "names without case: \(GameCompat.airRuntime(programFolder: dir("air64")) ?? "nil")")
mk("air2/game.exe"); mk("air2/META-INF/AIR/application.xml"); mk("air2/Adobe AIR/Versions/1.0/other.dll")
check(GameCompat.airRuntime(programFolder: dir("air2")) == "captive runtime folder without Adobe AIR.dll", "folder without the DLL")
check(GameCompat.airRuntime(programFolder: dir("hk")) == nil, "not AIR")
check(GameCompat.peMachine(root.appendingPathComponent("vs/VampireSurvivors.exe")) == 0x8664, "PE machine")
check(GameCompat.peMachine(root.appendingPathComponent("vs/GameAssembly.dll")) == 0, "empty file: not PE")

// MARK: - Windows paths

let c = URL(fileURLWithPath: "/prefix/drive_c")
check(GameCompat.unixURL(windowsPath: "C:\\Program Files (x86)\\Steam\\steamapps\\common\\911 Operator\\911.exe", driveC: c)?.path
      == "/prefix/drive_c/Program Files (x86)/Steam/steamapps/common/911 Operator/911.exe", "C:\\ path")
check(GameCompat.unixURL(windowsPath: "c:/Games/x.exe", driveC: c)?.path == "/prefix/drive_c/Games/x.exe", "lower case, slashes")
check(GameCompat.unixURL(windowsPath: "D:\\x.exe", driveC: c) == nil, "other drive")

try? fm.removeItem(at: root)
print("game compat: \(checks - failures)/\(checks) checks passed")
exit(failures == 0 ? 0 : 1)
