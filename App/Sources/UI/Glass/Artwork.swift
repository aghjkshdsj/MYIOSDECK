// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit
import ImageIO
import CoreImage

/// Game artwork for the Glass look, from the user's own library at runtime (nothing is bundled):
/// Steam's public store images for Steam games, the program's own icon for added games.
/// Kept in memory and in Caches/artwork (Storage › Temporary files clears it; it comes back).
@MainActor
final class ArtworkStore {
    static let shared = ArtworkStore()

    enum Kind: String { case hero, wide, portrait }

    private let memory = NSCache<NSString, UIImage>()
    private var failed = Set<URL>()
    private var inflight: [String: Task<UIImage?, Never>] = [:]

    nonisolated static var cacheDir: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("artwork", isDirectory: true)
    }

    private init() {
        memory.totalCostLimit = 96 << 20
    }

    /// After Storage cleared the files: the images in memory stay usable, failures are retried.
    func forgetMemory() { failed.removeAll() }

    /// An image already in memory (no waiting): the first frame of a view shows it at once.
    func cached(_ key: String) -> UIImage? { memory.object(forKey: key as NSString) }

    private func remember(_ image: UIImage, _ key: String) {
        let cost = Int(image.size.width * image.size.height * image.scale * image.scale * 4)
        memory.setObject(image, forKey: key as NSString, cost: cost)
    }

    /// The first candidate URL that loads (disk cache, then network), decoded at most `maxPixels` wide.
    func image(key: String, urls: [URL], maxPixels: CGFloat) async -> UIImage? {
        if let hit = cached(key) { return hit }
        if let running = inflight[key] { return await running.value }
        let candidates = urls.filter { !failed.contains($0) }
        let task = Task<UIImage?, Never> { [weak self] in
            for url in candidates {
                let file = Self.cacheDir.appendingPathComponent(Self.fileName(url))
                if let image = await Self.decode(file: file, maxPixels: maxPixels) { return image }
                let fetched = try? await URLSession.shared.data(from: url)
                guard let data = fetched?.0, (fetched?.1 as? HTTPURLResponse)?.statusCode == 200, data.count > 0 else {
                    self?.failed.insert(url)
                    continue
                }
                try? FileManager.default.createDirectory(at: Self.cacheDir, withIntermediateDirectories: true)
                try? data.write(to: file, options: .atomic)
                if let image = await Self.decode(file: file, maxPixels: maxPixels) { return image }
                self?.failed.insert(url)
            }
            return nil
        }
        inflight[key] = task
        let image = await task.value
        inflight[key] = nil
        if let image { remember(image, key) }
        return image
    }

    /// A blurred copy for the backdrop (small: it is blurred anyway), cached.
    func blurred(key: String, base: UIImage, radius: CGFloat) async -> UIImage? {
        let bkey = "\(key)|blur\(Int(radius))"
        if let hit = cached(bkey) { return hit }
        guard let cg = base.cgImage else { return nil }
        let out = await Task.detached(priority: .utility) { () -> UIImage? in
            let input = CIImage(cgImage: cg)
            let scale = min(1, 480 / max(input.extent.width, 1))
            let small = input.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
            let blur = small.clampedToExtent().applyingGaussianBlur(sigma: Double(radius) * scale * 1.6)
                .cropped(to: small.extent)
            guard let made = CIContext(options: [.useSoftwareRenderer: false]).createCGImage(blur, from: small.extent) else { return nil }
            return UIImage(cgImage: made)
        }.value
        if let out { remember(out, bkey) }
        return out
    }

    nonisolated private static func fileName(_ url: URL) -> String {
        var h: UInt64 = 0xcbf29ce484222325
        for b in url.absoluteString.utf8 { h = (h ^ UInt64(b)) &* 0x100000001b3 }
        return String(h, radix: 16) + "." + (url.pathExtension.isEmpty ? "img" : url.pathExtension)
    }

    nonisolated private static func decode(file: URL, maxPixels: CGFloat) async -> UIImage? {
        await Task.detached(priority: .utility) { () -> UIImage? in
            guard FileManager.default.fileExists(atPath: file.path),
                  let src = CGImageSourceCreateWithURL(file as CFURL, nil) else { return nil }
            let opts: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                         kCGImageSourceThumbnailMaxPixelSize: maxPixels,
                                         kCGImageSourceCreateThumbnailWithTransform: true]
            guard let cg = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) else { return nil }
            return UIImage(cgImage: cg)
        }.value
    }

    // MARK: added games: the program's icon

    /// The largest icon in an added game's program (PE resources), cached as a file.
    func icon(for game: CustomGame) async -> UIImage? {
        let key = "icon.\(game.id.uuidString).\(game.exe)"
        if let hit = cached(key) { return hit }
        let exe = CustomGames.root.appendingPathComponent(game.folder).appendingPathComponent(game.exe)
        let file = Self.cacheDir.appendingPathComponent(Self.fileName(URL(fileURLWithPath: "/icon/\(game.id.uuidString)/\(game.exe)")) + ".ico")
        let image = await Task.detached(priority: .utility) { () -> UIImage? in
            if let data = try? Data(contentsOf: file), let img = UIImage(data: data) { return img }
            guard let data = PEIcon.extract(from: exe), let img = UIImage(data: data) else { return nil }
            try? FileManager.default.createDirectory(at: Self.cacheDir, withIntermediateDirectories: true)
            try? data.write(to: file, options: .atomic)
            return img
        }.value
        if let image { remember(image, key) }
        return image
    }

    // MARK: Steam artwork candidates (Steam's public store images, no account data)

    private static let assetBase = "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/"
    private static let legacyBase = "https://cdn.cloudflare.steamstatic.com/steam/apps/"

    private static func safeAssetName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= 256 && !name.hasPrefix("/") && !name.contains("..") &&
            name.unicodeScalars.allSatisfy { $0.isASCII && (CharacterSet.alphanumerics.contains($0) || "._-/".unicodeScalars.contains($0)) }
    }

    static func steamURLs(_ appID: Int, kind: Kind, owned: OwnedSteamGame?, library: SteamLibrary) -> [URL] {
        var urls: [URL] = []
        func add(_ s: String?) { if let s, let u = URL(string: s), !urls.contains(u) { urls.append(u) } }
        func asset(_ name: String?) -> String? {
            guard let name, safeAssetName(name) else { return nil }
            return assetBase + "\(appID)/" + name
        }
        switch kind {
        case .hero:
            add(asset(owned?.libraryHero))
            add(assetBase + "\(appID)/library_hero.jpg")
            add(legacyBase + "\(appID)/library_hero.jpg")
            add(legacyBase + "\(appID)/capsule_616x353.jpg")
            add(asset(owned?.headerImage))
            add(legacyBase + "\(appID)/header.jpg")
        case .wide:
            add(legacyBase + "\(appID)/capsule_616x353.jpg")
            add(asset(owned?.headerImage))
            add(legacyBase + "\(appID)/header.jpg")
        case .portrait:
            for u in library.artwork(appID) where !urls.contains(u) { urls.append(u) }
        }
        return urls
    }
}

/// The source of a game's artwork, resolved once per view.
enum ArtworkSource: Hashable {
    case steam(Int)
    case custom(CustomGame)
    case blank

    func key(_ kind: ArtworkStore.Kind) -> String {
        switch self {
        case .steam(let id): return "steam.\(id).\(kind.rawValue)"
        case .custom(let g): return "icon.\(g.id.uuidString).\(g.exe)"
        case .blank: return ""
        }
    }
}

/// Loads a game's image of one kind; nil until it is there (or when there is none).
@MainActor
final class ArtworkLoader: ObservableObject {
    @Published private(set) var image: UIImage?
    @Published private(set) var blurredImage: UIImage?
    @Published private(set) var isIcon = false
    private var loadedKey = ""

    func load(_ source: ArtworkSource, kind: ArtworkStore.Kind, blurRadius: CGFloat? = nil) {
        let key = source.key(kind) + (blurRadius.map { "|\(Int($0))" } ?? "")
        guard key != loadedKey else { return }
        loadedKey = key
        let store = ArtworkStore.shared
        image = store.cached(source.key(kind))
        isIcon = { if case .custom = source { return true } else { return false } }()
        if let r = blurRadius, image != nil { blurredImage = store.cached("\(source.key(kind))|blur\(Int(r))") } else { blurredImage = nil }
        Task {
            let img: UIImage?
            switch source {
            case .steam(let id):
                let lib = SteamLibrary.shared
                let urls = ArtworkStore.steamURLs(id, kind: kind, owned: lib.game(id), library: lib)
                img = await store.image(key: source.key(kind), urls: urls, maxPixels: kind == .hero ? 1800 : kind == .wide ? 900 : 700)
            case .custom(let g):
                img = await store.icon(for: g)
            case .blank:
                img = nil
            }
            guard key == loadedKey else { return }
            image = img
            if let r = blurRadius, let img {
                let b = await store.blurred(key: source.key(kind), base: img, radius: r)
                guard key == loadedKey else { return }
                blurredImage = b
            }
        }
    }
}

/// A tile with the game's name on a colour of its own, for games without artwork.
struct GamePlaceholder: View {
    let name: String
    var compact = false

    private var hue: Double {
        var h: UInt32 = 2166136261
        for b in name.utf8 { h = (h ^ UInt32(b)) &* 16777619 }
        return Double(h % 360) / 360
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(hue: hue, saturation: 0.55, brightness: 0.42),
                                    Color(hue: (hue + 0.08).truncatingRemainder(dividingBy: 1), saturation: 0.7, brightness: 0.18)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
            Text(name)
                .font(compact ? .caption.weight(.bold) : .headline.weight(.heavy))
                .fontWidth(.condensed)
                .multilineTextAlignment(.center)
                .foregroundStyle(.white.opacity(0.92))
                .minimumScaleFactor(0.5)
                .padding(compact ? 6 : 12)
        }
    }
}

/// A game's artwork filling its frame (cropped), its icon on a coloured tile, or the placeholder.
struct ArtworkView: View {
    let source: ArtworkSource
    let kind: ArtworkStore.Kind
    let name: String
    var compactPlaceholder = false
    @StateObject private var loader = ArtworkLoader()

    var body: some View {
        GeometryReader { geo in
            ZStack {
                if let image = loader.image {
                    if loader.isIcon {
                        GamePlaceholder(name: "", compact: true)
                        Image(uiImage: image).resizable().interpolation(.high).scaledToFit()
                            .frame(width: min(geo.size.width, geo.size.height) * 0.55)
                            .shadow(color: .black.opacity(0.4), radius: 8, y: 4)
                    } else {
                        Image(uiImage: image).resizable().scaledToFill()
                            .frame(width: geo.size.width, height: geo.size.height)
                    }
                } else {
                    GamePlaceholder(name: name, compact: compactPlaceholder)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            .clipped()
        }
        .onAppear { loader.load(source, kind: kind) }
        .onChange(of: source) { _, s in loader.load(s, kind: kind) }
        .accessibilityHidden(true)
    }
}

/// The full-screen background: the game's artwork (sharp on Home, blurred elsewhere) under a dark
/// (or light) scrim, or the plain base when Game artwork background is off or there is no game.
struct ArtworkBackdrop: View {
    let source: ArtworkSource
    var blurred = true
    @ObservedObject private var look = Appearance.shared
    @StateObject private var loader = ArtworkLoader()
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        ZStack {
            LinearGradient(colors: [Glass.base, Glass.baseDeep], startPoint: .top, endPoint: .bottom)
            if look.artworkBackground, let image = blurred ? loader.blurredImage : loader.image {
                GeometryReader { geo in
                    Image(uiImage: image).resizable().scaledToFill()
                        .frame(width: geo.size.width, height: geo.size.height)
                        .clipped()
                }
                .transition(.opacity)
                scrim
            }
        }
        .animation(look.animation(.easeInOut(duration: 0.45)), value: loader.image)
        .animation(look.animation(.easeInOut(duration: 0.45)), value: loader.blurredImage)
        .ignoresSafeArea()
        .onAppear { reload() }
        .onChange(of: source) { _, _ in reload() }
        .onChange(of: look.glassIntensity) { _, _ in reload() }
        .accessibilityHidden(true)
    }

    private func reload() {
        // An added game has an icon, not a scene: blurred it tints the page, sharp it would not do.
        var s = source
        if case .custom = source, !blurred { s = .blank }
        loader.load(s, kind: .hero, blurRadius: blurred ? look.backdropBlur : nil)
    }

    private var scrim: some View {
        let base = scheme == .light ? Color.white : Color(red: 0.02, green: 0.04, blue: 0.09)
        return ZStack {
            base.opacity(blurred ? (scheme == .light ? 0.45 : 0.42) : (scheme == .light ? 0.25 : 0.12))
            LinearGradient(colors: [base.opacity(scheme == .light ? 0.6 : 0.72), base.opacity(0)],
                           startPoint: .leading, endPoint: .init(x: 0.7, y: 0.5))
            LinearGradient(colors: [base.opacity(0), base.opacity(scheme == .light ? 0.7 : 0.85)],
                           startPoint: .init(x: 0.5, y: 0.45), endPoint: .bottom)
        }
    }
}

/// The largest icon of a Windows program (its .exe's RT_GROUP_ICON / RT_ICON resources), as an
/// .ico file (or the PNG inside it), which ImageIO decodes. Reads only the headers and the
/// resource section, so a big program costs little.
enum PEIcon {
    static func extract(from url: URL) -> Data? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        func read(_ offset: UInt64, _ count: Int) -> Data? {
            guard (try? h.seek(toOffset: offset)) != nil, let d = try? h.read(upToCount: count), d.count == count else { return nil }
            return d
        }
        guard let head = read(0, 4096), head.count >= 0x40, head[0] == 0x4d, head[1] == 0x5a else { return nil }
        func u16(_ d: Data, _ o: Int) -> Int { o + 2 <= d.count ? Int(d[d.startIndex + o]) | Int(d[d.startIndex + o + 1]) << 8 : 0 }
        func u32(_ d: Data, _ o: Int) -> Int { o + 4 <= d.count ? u16(d, o) | u16(d, o + 2) << 16 : 0 }
        let pe = u32(head, 0x3c)
        guard pe > 0, pe + 24 <= head.count, head[pe] == 0x50, head[pe + 1] == 0x45 else { return nil }
        let sections = u16(head, pe + 6), optSize = u16(head, pe + 20), opt = pe + 24
        let magic = u16(head, opt)
        let dirs = opt + (magic == 0x20b ? 112 : 96)
        let resRVA = u32(head, dirs + 2 * 8), resSize = u32(head, dirs + 2 * 8 + 4)
        guard resRVA > 0, resSize > 0, resSize < 64 << 20 else { return nil }
        // The section holding the resources.
        let table = opt + optSize
        var fileOffset = -1
        for i in 0..<sections {
            let s = table + i * 40
            guard s + 40 <= head.count else { break }
            let va = u32(head, s + 12), vsize = max(u32(head, s + 8), u32(head, s + 16)), raw = u32(head, s + 20)
            if resRVA >= va && resRVA < va + vsize { fileOffset = raw + (resRVA - va); break }
        }
        guard fileOffset >= 0, let res = read(UInt64(fileOffset), resSize) else { return nil }
        // Resource tree: type -> name/id -> language -> data entry (offsets from the section start).
        func entries(_ dir: Int) -> [(id: Int, offset: Int, isDir: Bool)] {
            guard dir + 16 <= res.count else { return [] }
            let n = u16(res, dir + 12) + u16(res, dir + 14)
            return (0..<min(n, 4096)).compactMap { i in
                let e = dir + 16 + i * 8
                guard e + 8 <= res.count else { return nil }
                let name = u32(res, e), off = u32(res, e + 4)
                return (id: name & 0x7fff_ffff, offset: off & 0x7fff_ffff, isDir: off & 0x8000_0000 != 0)
            }
        }
        func firstData(_ entry: (id: Int, offset: Int, isDir: Bool)) -> Data? {
            var e = entry
            while e.isDir { guard let next = entries(e.offset).first else { return nil }; e = next }
            let rva = u32(res, e.offset), size = u32(res, e.offset + 4)
            let start = rva - resRVA
            guard start >= 0, size > 0, start + size <= res.count else { return nil }
            return res.subdata(in: (res.startIndex + start)..<(res.startIndex + start + size))
        }
        let types = entries(0)
        guard let groupType = types.first(where: { $0.id == 14 && $0.isDir }),
              let iconType = types.first(where: { $0.id == 3 && $0.isDir }),
              let group = entries(groupType.offset).first.flatMap(firstData) else { return nil }
        let icons = entries(iconType.offset)
        // GRPICONDIR: the biggest, deepest icon.
        let count = u16(group, 4)
        var best: (w: Int, bits: Int, id: Int, entry: Data)?
        for i in 0..<min(count, 64) {
            let e = 6 + i * 14
            guard e + 14 <= group.count else { break }
            let w = group[group.startIndex + e] == 0 ? 256 : Int(group[group.startIndex + e])
            let bits = u16(group, e + 6), id = u16(group, e + 12)
            if best == nil || w > best!.w || (w == best!.w && bits > best!.bits) {
                best = (w, bits, id, group.subdata(in: (group.startIndex + e)..<(group.startIndex + e + 12)))
            }
        }
        guard let best, let iconEntry = icons.first(where: { $0.id == best.id }), let image = firstData(iconEntry) else { return nil }
        if image.starts(with: [0x89, 0x50, 0x4e, 0x47]) { return image }   // a PNG icon
        // An .ico of the one image: ICONDIR, then its entry with the image right after.
        var ico = Data([0, 0, 1, 0, 1, 0])
        ico.append(best.entry.prefix(8))                    // width, height, colours, reserved, planes, bits
        var size = UInt32(image.count).littleEndian
        ico.append(Data(bytes: &size, count: 4))
        var offset = UInt32(22).littleEndian
        ico.append(Data(bytes: &offset, count: 4))
        ico.append(image)
        return ico
    }
}
