// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Before Madeira Dock starts: Steam's UDP/TCP connection servers go into Valve's client's own
/// cache (config\config.vdf, InstallConfigStore/Software/Valve/Steam/CM; SteamCMList.swift), so
/// the client can connect without its HTTPS CM directory, which does not complete without JIT.
/// The list comes from the same public directory, fetched over iOS's HTTPS; the last good list is
/// kept for when it cannot be fetched. Logged as [dock-cm] (counts only).
enum DockCMCache {
    /// The directory asked for NetFilter servers, then for every kind (filtered here).
    private static let directories = [
        "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=netfilter",
        "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0",
    ]
    private static let keptKey = "dockNetfilterCMs"
    static let path = ["InstallConfigStore", "Software", "Valve", "Steam", "CM"]

    static func prepare(drive: URL) async {
        var endpoints: [String] = []
        var source = "directory"
        for address in directories where endpoints.isEmpty {
            guard let url = URL(string: address) else { continue }
            var request = URLRequest(url: url)
            request.timeoutInterval = 15
            if let (data, response) = try? await URLSession.shared.data(for: request),
               (response as? HTTPURLResponse)?.statusCode == 200 {
                endpoints = SteamCMList.netfilterEndpoints(directoryJSON: data)
            }
        }
        if endpoints.isEmpty {
            endpoints = UserDefaults.standard.stringArray(forKey: keptKey)?.filter(SteamCMList.isIPEndpoint) ?? []
            source = "kept from an earlier start"
        } else {
            UserDefaults.standard.set(endpoints, forKey: keptKey)
        }
        guard !endpoints.isEmpty else {
            SteamLog.event("[dock-cm] no UDP/TCP CM list (directory unreachable, none kept): Valve's client asks its own directory")
            return
        }
        let file = drive.appendingPathComponent(SteamRuntimeFiles.relativeRoot + "/config/config.vdf")
        let current = try? String(contentsOf: file, encoding: .utf8)
        guard let text = ValveVDF.setting(current, path: path, value: endpoints.joined(separator: ";")) else {
            SteamLog.event("[dock-cm] config.vdf could not be read as KeyValues: left unchanged")
            return
        }
        do {
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try text.write(to: file, atomically: true, encoding: .utf8)
            SteamLog.event("[dock-cm] netfilter CMs=\(endpoints.count) (\(source)) written to Valve's client's config")
        } catch {
            SteamLog.event("[dock-cm] config.vdf not written: \(SteamSignIn.reason(error))")
        }
    }
}
