// SPDX-License-Identifier: GPL-3.0-or-later
//
// Steam's connection servers (CMs) for Valve's client inside Madeira Dock (docs/STEAM_DOCK.md).
// Without JIT Valve's client cannot finish a TLS handshake in time, so it never gets the CM list
// from the HTTPS CM directory, and with no list it has no UDP/TCP ("NetFilter") server to use
// (build 180: "No NetFilter CMs, defaulting to WebSockets"). It also reads a cached list from its
// install config store, config\config.vdf: InstallConfigStore/Software/Valve/Steam/CM, addresses
// separated by ";" (steamclient64.dll: CCMInterface's list setup reads "Software\Valve\Steam\" +
// "CM" from config store 1 and splits it at ";"). MYIOSDECK fetches the same directory over iOS's
// own HTTPS and writes that value before Dock starts; everything else in the file stays as it is.
// Foundation only, so tests/host runs the parsing and the file edit.

import Foundation

enum SteamCMList {
    /// UDP/TCP endpoints ("ip:port") of the public realm from a GetCMListForConnect answer, least
    /// loaded first. WebSocket entries (host names) and other realms are left out.
    static func netfilterEndpoints(directoryJSON data: Data, limit: Int = 24) -> [String] {
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let response = json["response"] as? [String: Any],
              let list = response["serverlist"] as? [[String: Any]] else { return [] }
        var found: [(endpoint: String, load: Double)] = []
        for entry in list {
            guard let endpoint = entry["endpoint"] as? String, isIPEndpoint(endpoint) else { continue }
            if let type = entry["type"] as? String, type.lowercased() != "netfilter" { continue }
            if let realm = entry["realm"] as? String, !realm.isEmpty, realm.lowercased() != "steamglobal" { continue }
            let load = ((entry["wtd_load"] ?? entry["load"]) as? NSNumber)?.doubleValue ?? 0
            if !found.contains(where: { $0.endpoint == endpoint }) { found.append((endpoint, load)) }
        }
        return Array(found.sorted { $0.load < $1.load }.map(\.endpoint).prefix(limit))
    }

    /// "a.b.c.d:port" with an IPv4 address and a port.
    static func isIPEndpoint(_ s: String) -> Bool {
        let parts = s.split(separator: ":")
        guard parts.count == 2, let port = Int(parts[1]), port > 0, port < 65536 else { return false }
        let octets = parts[0].split(separator: ".", omittingEmptySubsequences: false)
        return octets.count == 4 && octets.allSatisfy { o in Int(o).map { (0...255).contains($0) && o.count <= 3 } ?? false }
    }
}

/// A Valve KeyValues text file (config.vdf), edited without losing anything: tokens are kept as
/// written (escapes included), only the value set is new.
enum ValveVDF {
    indirect enum Value { case string(String), node([Item]) }
    struct Item { var key: String; var value: Value }

    /// Parses quoted or bare tokens, braces and // comments. nil when the text is not well formed.
    static func parse(_ text: String) -> [Item]? {
        var tokens: [(text: String, quoted: Bool)] = []
        let chars = Array(text.unicodeScalars)
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if c == " " || c == "\t" || c == "\n" || c == "\r" { i += 1; continue }
            if c == "/", i + 1 < chars.count, chars[i + 1] == "/" {
                while i < chars.count, chars[i] != "\n" { i += 1 }
                continue
            }
            if c == "{" || c == "}" { tokens.append((String(c), false)); i += 1; continue }
            if c == "\"" {
                var s = String.UnicodeScalarView()
                i += 1
                while i < chars.count, chars[i] != "\"" {
                    if chars[i] == "\\", i + 1 < chars.count { s.append(chars[i]); i += 1 }   // kept escaped
                    s.append(chars[i]); i += 1
                }
                guard i < chars.count else { return nil }
                i += 1
                tokens.append((String(s), true))
                continue
            }
            var s = String.UnicodeScalarView()
            while i < chars.count, !" \t\r\n{}\"".unicodeScalars.contains(chars[i]) { s.append(chars[i]); i += 1 }
            tokens.append((String(s), false))
        }
        var at = 0
        func items(top: Bool) -> [Item]? {
            var out: [Item] = []
            while at < tokens.count {
                let t = tokens[at]
                if !t.quoted, t.text == "}" { return top ? nil : out }
                if !t.quoted, t.text == "{" { return nil }
                at += 1
                guard at < tokens.count else { return nil }
                let v = tokens[at]
                if !v.quoted, v.text == "{" {
                    at += 1
                    guard let sub = items(top: false), at < tokens.count else { return nil }
                    at += 1   // the closing brace
                    out.append(Item(key: t.text, value: .node(sub)))
                } else if !v.quoted, v.text == "}" {
                    return nil
                } else {
                    at += 1
                    out.append(Item(key: t.text, value: .string(v.text)))
                }
            }
            return top ? out : nil
        }
        return items(top: true)
    }

    static func serialize(_ items: [Item], depth: Int = 0) -> String {
        let pad = String(repeating: "\t", count: depth)
        var out = ""
        for item in items {
            switch item.value {
            case .string(let s): out += "\(pad)\"\(item.key)\"\t\t\"\(s)\"\n"
            case .node(let sub): out += "\(pad)\"\(item.key)\"\n\(pad){\n" + serialize(sub, depth: depth + 1) + "\(pad)}\n"
            }
        }
        return out
    }

    /// The text with `path`'s last component set to `value` (an unescaped plain value: no quote or
    /// backslash), sections created as needed, keys matched without regard to case. nil when the
    /// existing text cannot be parsed (then it is left alone).
    static func setting(_ text: String?, path: [String], value: String) -> String? {
        guard !path.isEmpty, !value.contains("\""), !value.contains("\\") else { return nil }
        var root: [Item]
        if let text, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            guard let parsed = parse(text) else { return nil }
            root = parsed
        } else {
            root = []
        }
        func set(_ items: inout [Item], _ rest: ArraySlice<String>) {
            let key = rest.first!
            let index = items.firstIndex { $0.key.caseInsensitiveCompare(key) == .orderedSame }
            if rest.count == 1 {
                if let index { items[index].value = .string(value) } else { items.append(Item(key: key, value: .string(value))) }
                return
            }
            var sub: [Item] = []
            if let index, case .node(let existing) = items[index].value { sub = existing }
            set(&sub, rest.dropFirst())
            if let index { items[index].value = .node(sub) } else { items.append(Item(key: key, value: .node(sub))) }
        }
        set(&root, path[...])
        return serialize(root)
    }

    /// The string at `path`, or nil.
    static func value(_ items: [Item], path: [String]) -> String? {
        guard let key = path.first, let item = items.first(where: { $0.key.caseInsensitiveCompare(key) == .orderedSame }) else { return nil }
        switch item.value {
        case .string(let s): return path.count == 1 ? s : nil
        case .node(let sub): return path.count == 1 ? nil : value(sub, path: Array(path.dropFirst()))
        }
    }
}
