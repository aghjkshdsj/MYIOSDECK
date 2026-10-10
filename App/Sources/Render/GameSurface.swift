// SPDX-License-Identifier: GPL-3.0-or-later
import SwiftUI
import UIKit
import QuartzCore
import Metal

/// Stage 3: the CAMetalLayer DXMT presents Wine's Direct3D windows into.
///
/// Like Madeira's MetalHostView, the layer lives in a raw UIView added straight
/// to the UIWindow: a SwiftUI-hosted backing layer can have direct Metal
/// presents silently dropped on iOS 26/27. One host, one layer, for the whole
/// process (DXMT keeps the layer it was given), so only its frame changes.
final class GameHostView: UIView, UIKeyInput {
    static let shared = GameHostView(frame: CGRect(x: 0, y: 0, width: 800, height: 600))
    private static var registered = false

    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    /// The in-game menu button lives on this view because nothing SwiftUI draws
    /// can be above it; the app sets what it does.
    static var onMenu: (() -> Void)?
    private let menuButton = UIButton(type: .system)
    /// Opens the iOS keyboard; what is typed reaches the program as key presses.
    private let keyboardButton = UIButton(type: .system)
    /// The program's mouse cursor while the right stick or a mouse moves it (a finger is
    /// its own pointer, so a touch hides it). Its tip is the image's top left.
    private let cursorView = UIImageView(image: UIImage(systemName: "cursorarrow",
                                                        withConfiguration: UIImage.SymbolConfiguration(pointSize: 18)))
    private var cursorUnit: CGPoint?

    override init(frame: CGRect) {
        super.init(frame: frame)
        isHidden = true
        isMultipleTouchEnabled = true
        menuButton.setImage(UIImage(systemName: "ellipsis.circle.fill",
                                    withConfiguration: UIImage.SymbolConfiguration(pointSize: 26)), for: .normal)
        menuButton.tintColor = UIColor.white.withAlphaComponent(0.55)
        menuButton.frame = CGRect(x: 6, y: 6, width: 44, height: 44)
        menuButton.addAction(UIAction { _ in GameHostView.onMenu?() }, for: .touchUpInside)
        addSubview(menuButton)
        keyboardButton.setImage(UIImage(systemName: "keyboard",
                                        withConfiguration: UIImage.SymbolConfiguration(pointSize: 22)), for: .normal)
        keyboardButton.tintColor = UIColor.white.withAlphaComponent(0.55)
        keyboardButton.frame = CGRect(x: 52, y: 6, width: 44, height: 44)
        keyboardButton.addAction(UIAction { _ in GameHostView.shared.toggleKeyboard() }, for: .touchUpInside)
        addSubview(keyboardButton)
        cursorView.tintColor = .white
        cursorView.layer.shadowColor = UIColor.black.cgColor
        cursorView.layer.shadowOpacity = 0.9
        cursorView.layer.shadowRadius = 1.5
        cursorView.layer.shadowOffset = .zero
        cursorView.isUserInteractionEnabled = false
        cursorView.isHidden = true
        addSubview(cursorView)
        statusLabel.font = .preferredFont(forTextStyle: .subheadline)
        statusLabel.textColor = UIColor.white.withAlphaComponent(0.8)
        statusLabel.backgroundColor = UIColor.black.withAlphaComponent(0.55)
        statusLabel.textAlignment = .center
        statusLabel.numberOfLines = 2
        statusLabel.layer.cornerRadius = 10
        statusLabel.layer.masksToBounds = true
        statusLabel.isHidden = true
        addSubview(statusLabel)
        backgroundColor = .black
        contentScaleFactor = UIScreen.main.scale
        metalLayer.device = MTLCreateSystemDefaultDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        // Private on iOS (public on macOS): keeps presents out of display-sync
        // scheduling, which drops them at low present rates (Madeira, MeloNX).
        let sync = NSSelectorFromString("setDisplaySyncEnabled:")
        if metalLayer.responds(to: sync) { metalLayer.perform(sync, with: NSNumber(value: false)) }
        // Seed once so DXMT's swapchain never waits on a zero-sized layer; after
        // this DXMT is the only drawableSize writer.
        metalLayer.drawableSize = CGSize(width: 800, height: 600)
    }
    required init?(coder: NSCoder) { fatalError() }

    /// A line of progress over the game (Madeira Dock while Valve's client signs in and starts
    /// the game, which takes minutes without JIT and shows no window); nil hides it.
    private let statusLabel = UILabel()
    func setStatus(_ text: String?) {
        guard statusLabel.text != text else { return }
        statusLabel.text = text.map { "  \($0)  " }
        statusLabel.isHidden = text == nil
        setNeedsLayout()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        placeCursor()
        guard !statusLabel.isHidden else { return }
        let size = statusLabel.sizeThatFits(CGSize(width: bounds.width - 40, height: 80))
        statusLabel.frame = CGRect(x: (bounds.width - min(size.width, bounds.width - 40)) / 2, y: bounds.height - size.height - 24,
                                   width: min(size.width, bounds.width - 40), height: size.height + 8)
    }

    override var isHidden: Bool {
        didSet { if isHidden { _ = resignFirstResponder() } }   // the game menu took over
    }

    // MARK: - touches: the program's mouse (GameInput); the buttons keep theirs

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        GameInput.shared.touchesBegan(touches, in: self)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        GameInput.shared.touchesMoved(touches, event, in: self)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        GameInput.shared.touchesEnded(touches, event, in: self)
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        GameInput.shared.touchesCancelled(touches, in: self)
    }

    /// The drawn cursor at a position given as a fraction of the desktop; nil hides it.
    func showCursor(_ unit: CGPoint?) {
        cursorUnit = unit
        placeCursor()
    }
    private func placeCursor() {
        guard let u = cursorUnit else { cursorView.isHidden = true; return }
        let size = cursorView.intrinsicContentSize
        cursorView.frame = CGRect(x: u.x * bounds.width - 2, y: u.y * bounds.height - 2, width: size.width, height: size.height)
        cursorView.isHidden = false
    }

    // MARK: - on-screen keyboard (UIKeyInput): typed text becomes key presses

    func toggleKeyboard() {
        if isFirstResponder { _ = resignFirstResponder() } else { _ = becomeFirstResponder() }
    }
    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }
    func insertText(_ text: String) { GameInput.shared.type(text) }
    func deleteBackward() { GameInput.shared.backspace() }
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    var keyboardType: UIKeyboardType = .asciiCapable

    /// Hand the layer to DXMT. Must happen before a Direct3D program starts.
    static func register() {
        guard !registered else { return }
        registered = true
        mid_display_set_layer(Unmanaged.passUnretained(shared.metalLayer).toOpaque())
        dlog("[display] Metal layer registered with DXMT")
    }
}

/// Layout placeholder: puts the window-level host over its own area, letterboxed
/// to the guest monitor's aspect, and tells winios where that is.
final class GameSurfacePlaceholder: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
    }
    required init?(coder: NSCoder) { fatalError() }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        let host = GameHostView.shared
        guard let w = window else {
            host.isHidden = true
            mid_game_overlay_show(0, nil)
            return
        }
        if host.superview !== w {
            host.removeFromSuperview()
            w.addSubview(host)
        } else {
            w.bringSubviewToFront(host)   // above a full-screen cover presented after it was added
        }
        host.isHidden = false
        GameHostView.register()
        applyLayout()
        // A launcher's or dialog's GDI windows (Winios's overlay) go above the game view.
        mid_game_overlay_show(1, Unmanaged.passUnretained(w).toOpaque())
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        applyLayout()
    }

    private func applyLayout() {
        guard let w = window, bounds.width > 0, bounds.height > 0 else { return }
        var gw: Int32 = 0, gh: Int32 = 0
        mid_display_screen_size(&gw, &gh)
        let guest = CGSize(width: CGFloat(max(gw, 1)), height: CGFloat(max(gh, 1)))
        let scale = min(bounds.width / guest.width, bounds.height / guest.height)
        let size = CGSize(width: guest.width * scale, height: guest.height * scale)
        let rect = CGRect(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2,
                          width: size.width, height: size.height)
        GameHostView.shared.frame = convert(rect, to: w)
        let full = convert(bounds, to: w)
        mid_display_layout(full.minX, full.minY, full.width, full.height,
                           rect.minX, rect.minY, rect.width, rect.height)
    }
}

struct GameSurfaceView: UIViewRepresentable {
    func makeUIView(context: Context) -> GameSurfacePlaceholder { GameSurfacePlaceholder() }
    func updateUIView(_ view: GameSurfacePlaceholder, context: Context) { view.setNeedsLayout() }
    static func dismantleUIView(_ view: GameSurfacePlaceholder, coordinator: ()) {
        GameHostView.shared.isHidden = true
        mid_game_overlay_show(0, nil)
    }
}
