// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit

/// The "Glass" look: a dark navy base (or a light one), the selected game's artwork behind
/// everything, frosted panels with thin light borders, and a bright blue primary button.
/// Colours are dynamic (they follow the app's Theme setting), so every view, old or new, adapts.
enum Glass {
    static func dynamic(dark: UIColor, light: UIColor) -> Color {
        Color(UIColor { $0.userInterfaceStyle == .light ? light : dark })
    }

    /// The base under the artwork.
    static let base = dynamic(dark: UIColor(red: 0.039, green: 0.075, blue: 0.137, alpha: 1),     // #0A1323
                              light: UIColor(red: 0.906, green: 0.929, blue: 0.965, alpha: 1))   // #E7EDF6
    static let baseDeep = dynamic(dark: UIColor(red: 0.020, green: 0.039, blue: 0.082, alpha: 1),
                                  light: UIColor(red: 0.835, green: 0.871, blue: 0.925, alpha: 1))
    /// Panel tint over the material, and a solid panel when transparency is reduced.
    static let tint = dynamic(dark: UIColor(white: 1, alpha: 0.07), light: UIColor(white: 1, alpha: 0.35))
    static let solidPanel = dynamic(dark: UIColor(red: 0.090, green: 0.122, blue: 0.180, alpha: 1),
                                    light: UIColor(white: 0.985, alpha: 1))
    static let border = dynamic(dark: UIColor(white: 1, alpha: 0.20), light: UIColor(white: 0, alpha: 0.10))
    static let separator = dynamic(dark: UIColor(white: 1, alpha: 0.12), light: UIColor(white: 0, alpha: 0.08))
    static let text = dynamic(dark: UIColor(white: 1, alpha: 0.96), light: UIColor(red: 0.055, green: 0.090, blue: 0.149, alpha: 1))
    static let secondary = dynamic(dark: UIColor(white: 1, alpha: 0.68), light: UIColor(red: 0.055, green: 0.090, blue: 0.149, alpha: 0.62))
    /// The primary blue (buttons, selection, toggles).
    static let primary = Color(red: 0.204, green: 0.502, blue: 1.0)            // #3480FF
    static let primaryDeep = Color(red: 0.090, green: 0.345, blue: 0.851)      // #1758D9
    static let selectedFill = dynamic(dark: UIColor(red: 0.204, green: 0.502, blue: 1, alpha: 0.38),
                                      light: UIColor(red: 0.204, green: 0.502, blue: 1, alpha: 0.22))
    static let selectedBorder = Color(red: 0.42, green: 0.65, blue: 1.0).opacity(0.85)
    /// The tab pill's selected segment (white glass, not blue: mockups 1 and 2).
    static let tabFill = dynamic(dark: UIColor(white: 1, alpha: 0.22), light: UIColor(white: 1, alpha: 0.85))
    static let good = Color(red: 0.36, green: 0.80, blue: 0.45)
    static let warn = Color(red: 0.98, green: 0.72, blue: 0.24)
    static let bad = Color(red: 0.96, green: 0.36, blue: 0.33)
}

/// Appearance settings (Settings › Appearance), with the system's Reduce Transparency and Reduce
/// Motion folded in: either one turned on in iOS also turns the app's own setting on.
@MainActor
final class Appearance: ObservableObject {
    static let shared = Appearance()
    private let d = UserDefaults.standard

    enum Theme: String, CaseIterable, Identifiable {
        case system, dark, light
        var id: String { rawValue }
        var title: String { rawValue.capitalized }
    }

    @Published var theme: Theme { didSet { d.set(theme.rawValue, forKey: "ui.theme") } }
    /// 0 (clear glass) ... 1 (heavily frosted).
    @Published var glassIntensity: Double { didSet { d.set(glassIntensity, forKey: "ui.glassIntensity") } }
    @Published var artworkBackground: Bool { didSet { d.set(artworkBackground, forKey: "ui.artworkBackground") } }
    @Published var animatedTransitions: Bool { didSet { d.set(animatedTransitions, forKey: "ui.animatedTransitions") } }
    @Published var reduceTransparencySetting: Bool { didSet { d.set(reduceTransparencySetting, forKey: "ui.reduceTransparency") } }
    /// The iOS accessibility settings, kept current.
    @Published private(set) var systemReduceTransparency = UIAccessibility.isReduceTransparencyEnabled
    @Published private(set) var systemReduceMotion = UIAccessibility.isReduceMotionEnabled

    var reduceTransparency: Bool { reduceTransparencySetting || systemReduceTransparency }
    var animations: Bool { animatedTransitions && !systemReduceMotion }
    var colorScheme: ColorScheme? {
        switch theme {
        case .system: return nil
        case .dark: return .dark
        case .light: return .light
        }
    }

    private init() {
        d.register(defaults: ["ui.theme": Theme.dark.rawValue, "ui.glassIntensity": 0.6, "ui.artworkBackground": true,
                              "ui.animatedTransitions": true, "ui.reduceTransparency": false])
        theme = Theme(rawValue: d.string(forKey: "ui.theme") ?? "") ?? .dark
        glassIntensity = d.double(forKey: "ui.glassIntensity")
        artworkBackground = d.bool(forKey: "ui.artworkBackground")
        animatedTransitions = d.bool(forKey: "ui.animatedTransitions")
        reduceTransparencySetting = d.bool(forKey: "ui.reduceTransparency")
        let center = NotificationCenter.default
        center.addObserver(forName: UIAccessibility.reduceTransparencyStatusDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.systemReduceTransparency = UIAccessibility.isReduceTransparencyEnabled }
        }
        center.addObserver(forName: UIAccessibility.reduceMotionStatusDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.systemReduceMotion = UIAccessibility.isReduceMotionEnabled }
        }
    }

    /// The animation for a transition, or none (Animated transitions off, or Reduce Motion).
    func animation(_ a: Animation = .spring(response: 0.38, dampingFraction: 0.86)) -> Animation? { animations ? a : nil }

    var material: Material {
        switch glassIntensity {
        case ..<0.25: return .ultraThinMaterial
        case ..<0.55: return .thinMaterial
        case ..<0.85: return .regularMaterial
        default: return .thickMaterial
        }
    }
    /// The backdrop's blur radius for blurred pages.
    var backdropBlur: CGFloat { 18 + 30 * glassIntensity }
}

/// Runs a state change with the app's transition animation (or none).
@MainActor func withGlassAnimation(_ body: () -> Void) {
    if let a = Appearance.shared.animation() { withAnimation(a, body) } else { body() }
}

// MARK: - panels

/// A frosted panel: material, a faint tint and a thin light border. Solid when transparency is reduced.
struct GlassPanel<S: InsettableShape>: ViewModifier {
    var shape: S
    var highlighted = false
    @ObservedObject private var look = Appearance.shared

    init(shape: S, highlighted: Bool = false) {
        self.shape = shape
        self.highlighted = highlighted
    }

    func body(content: Content) -> some View {
        content
            .background {
                if look.reduceTransparency {
                    shape.fill(highlighted ? AnyShapeStyle(Glass.primaryDeep) : AnyShapeStyle(Glass.solidPanel))
                } else {
                    shape.fill(look.material)
                        .overlay(shape.fill(highlighted ? Glass.selectedFill : Glass.tint))
                }
            }
            .overlay(shape.strokeBorder(highlighted ? Glass.selectedBorder : Glass.border, lineWidth: 1))
    }
}

extension View {
    func glassPanel(cornerRadius: CGFloat = 20, highlighted: Bool = false) -> some View {
        modifier(GlassPanel(shape: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous), highlighted: highlighted))
    }
    func glassCapsule(highlighted: Bool = false) -> some View {
        modifier(GlassPanel(shape: Capsule(), highlighted: highlighted))
    }
    func glassCircle(highlighted: Bool = false) -> some View {
        modifier(GlassPanel(shape: Circle(), highlighted: highlighted))
    }
}

/// A list card: rows separated by thin lines, inside a glass panel.
struct GlassCard<Content: View>: View {
    var padding: CGFloat = 16
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 0) { content }
            .padding(.horizontal, padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassPanel(cornerRadius: 18)
    }
}

/// A row inside a GlassCard (with a separator below unless it is the last).
struct GlassRow<Content: View>: View {
    var last = false
    @ViewBuilder var content: Content
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) { content }
                .frame(minHeight: 52)
                .padding(.vertical, 6)
            if !last { Rectangle().fill(Glass.separator).frame(height: 1) }
        }
    }
}

/// A toggle row as in the mockups (label left, switch right).
struct GlassToggleRow: View {
    let title: String
    var detail: String? = nil
    @Binding var isOn: Bool
    var last = false
    var disabled = false

    var body: some View {
        GlassRow(last: last) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).foregroundStyle(Glass.text)
                if let detail { Text(detail).font(.footnote).foregroundStyle(Glass.secondary).fixedSize(horizontal: false, vertical: true) }
            }
            Spacer(minLength: 8)
            Toggle(title, isOn: $isOn).labelsHidden().tint(Glass.primary).disabled(disabled)
        }
        .opacity(disabled ? 0.55 : 1)
    }
}

// MARK: - buttons

/// The blue primary button with its glow (Play, Install game, Save).
struct PrimaryButtonStyle: ButtonStyle {
    var fullWidth = false
    var compact = false
    func makeBody(configuration: Configuration) -> some View {
        PrimaryButtonBody(configuration: configuration, fullWidth: fullWidth, compact: compact)
    }
}

struct PrimaryButtonBody: View {
    let configuration: ButtonStyleConfiguration
    var fullWidth = false
    var compact = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        configuration.label
            .font((compact ? Font.subheadline : Font.title3).weight(.semibold))
            .lineLimit(1).minimumScaleFactor(0.7)
            .foregroundStyle(.white.opacity(enabled ? 1 : 0.6))
            .padding(.horizontal, compact ? 18 : 28)
            .frame(minHeight: compact ? 40 : 56)
            .frame(maxWidth: fullWidth ? .infinity : nil)
            .background {
                Capsule().fill(enabled
                               ? AnyShapeStyle(LinearGradient(colors: [Glass.primary, Glass.primaryDeep], startPoint: .top, endPoint: .bottom))
                               : AnyShapeStyle(Color.gray.opacity(0.45)))
            }
            .overlay(Capsule().strokeBorder(.white.opacity(enabled ? 0.35 : 0.15), lineWidth: 1))
            .shadow(color: enabled ? Glass.primary.opacity(configuration.isPressed ? 0.3 : 0.6) : .clear, radius: compact ? 8 : 14)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .contentShape(Capsule())
    }
}

/// A glass capsule button (Cancel, secondary actions).
struct GlassButtonStyle: ButtonStyle {
    var fullWidth = false
    var compact = false
    var destructive = false
    func makeBody(configuration: Configuration) -> some View {
        GlassButtonBody(configuration: configuration, fullWidth: fullWidth, compact: compact, destructive: destructive)
    }
}

struct GlassButtonBody: View {
    let configuration: ButtonStyleConfiguration
    var fullWidth = false
    var compact = false
    var destructive = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        configuration.label
            .font((compact ? Font.subheadline : Font.body).weight(.medium))
            .lineLimit(1).minimumScaleFactor(0.7)
            .foregroundStyle(destructive ? Glass.bad : Glass.text)
            .padding(.horizontal, compact ? 14 : 22)
            .frame(minHeight: compact ? 36 : 50)
            .frame(maxWidth: fullWidth ? .infinity : nil)
            .glassCapsule()
            .opacity(enabled ? (configuration.isPressed ? 0.7 : 1) : 0.45)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .contentShape(Capsule())
    }
}

/// A round glass icon button (Search, Settings, the controller, "…").
struct GlassIconButton: View {
    let systemImage: String
    let label: String
    var size: CGFloat = 44
    var highlighted = false
    var badge = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: size * 0.4, weight: .semibold))
                .foregroundStyle(Glass.text)
                .frame(width: size, height: size)
                .glassCircle(highlighted: highlighted)
                .overlay(alignment: .topTrailing) {
                    if badge { Circle().fill(Glass.good).frame(width: 10, height: 10).offset(x: -2, y: 2) }
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }
}

/// The round glass "…" look as a label (for Menus).
struct GlassMoreLabel: View {
    var size: CGFloat = 56
    var body: some View {
        Image(systemName: "ellipsis")
            .font(.system(size: size * 0.36, weight: .bold))
            .foregroundStyle(Glass.text)
            .frame(width: size, height: size)
            .glassCircle()
            .contentShape(Circle())
    }
}

/// A back button: a glass circle with a chevron, then the page's name in spaced capitals (mockups 6, 7)
/// or a glass capsule with the previous page's name (mockup 4).
struct GlassBackButton: View {
    enum Style { case titled(String), capsule(String) }
    let style: Style
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            switch style {
            case .titled(let title):
                HStack(spacing: 14) {
                    Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold))
                        .frame(width: 44, height: 44).glassCircle()
                    Text(title.uppercased()).font(.subheadline.weight(.medium)).tracking(3).lineLimit(1)
                }
            case .capsule(let title):
                HStack(spacing: 8) {
                    Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold))
                    Text(title).font(.body)
                }
                .padding(.horizontal, 16).frame(height: 44).glassCapsule()
            }
        }
        .buttonStyle(.plain)
        .foregroundStyle(Glass.text)
        .accessibilityLabel("Back")
    }
}

// MARK: - segmented

/// A glass segmented control. `.tabs`: the app's tab pill (white selected segment);
/// `.accent`: settings choices (blue selected segment).
struct GlassSegmented<T: Hashable>: View {
    enum Style { case tabs, accent }
    let options: [T]
    @Binding var selection: T
    var style = Style.accent
    var compact = false
    let title: (T) -> String
    @Namespace private var ns
    @ObservedObject private var look = Appearance.shared

    var body: some View {
        HStack(spacing: 2) {
            ForEach(options, id: \.self) { option in
                let selected = option == selection
                Button {
                    withGlassAnimation { selection = option }
                } label: {
                    Text(title(option))
                        .font((compact ? Font.subheadline : Font.body).weight(selected ? .semibold : .regular))
                        .lineLimit(1).minimumScaleFactor(0.7)
                        .foregroundStyle(Glass.text.opacity(selected ? 1 : 0.85))
                        .padding(.horizontal, compact ? 12 : 18)
                        .frame(maxWidth: .infinity, minHeight: compact ? 34 : 42)
                        .background {
                            if selected {
                                Capsule()
                                    .fill(style == .tabs ? Glass.tabFill : Glass.selectedFill)
                                    .overlay(Capsule().strokeBorder(style == .tabs ? Glass.border : Glass.selectedBorder, lineWidth: 1))
                                    .matchedGeometryEffect(id: "seg", in: ns)
                            }
                        }
                        .contentShape(Capsule())
                }
                .buttonStyle(.plain)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
        .padding(4)
        .glassCapsule()
    }
}

// MARK: - text

struct Wordmark: View {
    var body: some View {
        Text("MYIOSDECK")
            .font(.system(size: 15, weight: .medium))
            .tracking(5.5)
            .foregroundStyle(Glass.text)
            .lineLimit(1)
            .fixedSize()
            .accessibilityLabel("MYIOSDECK")
    }
}

/// "LAST PLAYED"-style label.
struct Eyebrow: View {
    let text: String
    var body: some View {
        Text(text.uppercased()).font(.caption.weight(.semibold)).tracking(2.4).foregroundStyle(Glass.text.opacity(0.85))
    }
}

/// A section title with an optional trailing action ("Recent games … View library >").
struct SectionHeader<Trailing: View>: View {
    let title: String
    @ViewBuilder var trailing: Trailing
    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).font(.title3.weight(.semibold)).foregroundStyle(Glass.text)
            Spacer()
            trailing
        }
    }
}

extension SectionHeader where Trailing == EmptyView {
    init(title: String) { self.init(title: title) { EmptyView() } }
}

/// A small capsule with an icon and a word ("Steam", "GOG", "Installed").
struct SourceBadge: View {
    let source: GameSource
    var big = false
    var body: some View {
        HStack(spacing: big ? 10 : 6) {
            Image(systemName: source.symbol).font(big ? .title3 : .footnote)
            Text(source.title).font(big ? .body : .subheadline)
        }
        .foregroundStyle(Glass.text.opacity(0.9))
    }
}

/// Byte counts as Files shows them.
func bytesText(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: n, countStyle: .file) }
func bytesText(_ n: UInt64) -> String { bytesText(Int64(clamping: n)) }

/// Landscape when wider than tall (the mockups' two layouts).
struct IsLandscapeKey: EnvironmentKey { static let defaultValue = false }
extension EnvironmentValues {
    var isLandscape: Bool {
        get { self[IsLandscapeKey.self] }
        set { self[IsLandscapeKey.self] = newValue }
    }
}
