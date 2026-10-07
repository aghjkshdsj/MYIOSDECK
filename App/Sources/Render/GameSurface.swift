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
final class GameHostView: UIView {
    static let shared = GameHostView(frame: CGRect(x: 0, y: 0, width: 800, height: 600))
    private static var registered = false

    override class var layerClass: AnyClass { CAMetalLayer.self }
    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    /// The in-game menu button lives on this view because nothing SwiftUI draws
    /// can be above it; the app sets what it does.
    static var onMenu: (() -> Void)?
    private let menuButton = UIButton(type: .system)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isHidden = true
        menuButton.setImage(UIImage(systemName: "ellipsis.circle.fill",
                                    withConfiguration: UIImage.SymbolConfiguration(pointSize: 26)), for: .normal)
        menuButton.tintColor = UIColor.white.withAlphaComponent(0.55)
        menuButton.frame = CGRect(x: 6, y: 6, width: 44, height: 44)
        menuButton.addAction(UIAction { _ in GameHostView.onMenu?() }, for: .touchUpInside)
        addSubview(menuButton)
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

    /// Only the menu button takes touches; the rest fall through to the views below.
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        let hit = super.hitTest(point, with: event)
        return hit === self ? nil : hit
    }

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
            return
        }
        if host.superview !== w {
            host.removeFromSuperview()
            w.addSubview(host)
        }
        host.isHidden = false
        GameHostView.register()
        applyLayout()
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
    }
}
