// SPDX-License-Identifier: GPL-3.0-or-later
import Metal
import QuartzCore
import SwiftUI
import UIKit

/// Frame statistics published to the HUD a few times a second.
struct FrameStats: Equatable {
    var fps: Double = 0
    var frameMs: Double = 0      // display-link interval actually achieved
    var gpuMs: Double = 0        // GPU busy time per frame
    var worstMs: Double = 0      // worst frame in the last window (stutter)
    var drawableSize: CGSize = .zero
}

/// CAMetalLayer surface driven by CADisplayLink at up to the panel's maximum
/// (120 Hz ProMotion on iPhone 15 Pro Max). Triple-buffered, never blocks the
/// main thread on the GPU, and paces with presentDrawable(afterMinimumDuration:)
/// so a 30/40/60 cap is even rather than juddery.
final class MetalStageView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    var iterations: UInt32 = 48
    var frameCap: Int = 120 { didSet { applyFrameRate() } }
    var onStats: ((FrameStats) -> Void)?

    private let device = MTLCreateSystemDefaultDevice()
    private var queue: MTLCommandQueue?
    private var pipeline: MTLRenderPipelineState?
    private var link: CADisplayLink?
    private let inFlight = DispatchSemaphore(value: 3)
    private let start = CACurrentMediaTime()

    private var lastTimestamp: CFTimeInterval = 0
    private var windowStart: CFTimeInterval = 0
    private var windowFrames = 0
    private var windowWorst: Double = 0
    private var gpuAccum: Double = 0
    private var gpuSamples = 0
    private let statsLock = NSLock()

    override init(frame: CGRect) {
        super.init(frame: frame)
        setUp()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setUp()
    }

    private func setUp() {
        guard let device else { return }
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.maximumDrawableCount = 3
        metalLayer.presentsWithTransaction = false
        queue = device.makeCommandQueue()
        do {
            let lib = try device.makeDefaultLibrary(bundle: .main)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = lib.makeFunction(name: "stage_vertex")
            desc.fragmentFunction = lib.makeFunction(name: "stage_fragment")
            desc.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipeline = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            dlog("[metal] pipeline failed: \(error)")
        }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        link?.invalidate()
        link = nil
        guard window != nil else { return }
        let l = CADisplayLink(target: self, selector: #selector(step(_:)))
        link = l
        applyFrameRate()
        l.add(to: .main, forMode: .common)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let scale = window?.screen.nativeScale ?? UIScreen.main.nativeScale
        metalLayer.drawableSize = CGSize(width: bounds.width * scale, height: bounds.height * scale)
    }

    private func applyFrameRate() {
        let maxHz = Float(window?.screen.maximumFramesPerSecond ?? 120)
        let cap = min(Float(frameCap), maxHz)
        link?.preferredFrameRateRange = CAFrameRateRange(minimum: min(30, cap), maximum: cap, preferred: cap)
    }

    @objc private func step(_ link: CADisplayLink) {
        guard let queue, let pipeline else { return }
        if inFlight.wait(timeout: .now()) == .timedOut { return } // GPU behind: skip, never block main
        guard let drawable = metalLayer.nextDrawable(), let cmd = queue.makeCommandBuffer() else {
            inFlight.signal()
            return
        }
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = drawable.texture
        rp.colorAttachments[0].loadAction = .dontCare
        rp.colorAttachments[0].storeAction = .store
        if let enc = cmd.makeRenderCommandEncoder(descriptor: rp) {
            var u = (SIMD2<Float>(Float(metalLayer.drawableSize.width), Float(metalLayer.drawableSize.height)),
                     Float(link.targetTimestamp - start), iterations)
            enc.setRenderPipelineState(pipeline)
            enc.setFragmentBytes(&u, length: MemoryLayout.size(ofValue: u), index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            enc.endEncoding()
        }
        let interval = 1.0 / Double(max(1, min(frameCap, window?.screen.maximumFramesPerSecond ?? 120)))
        cmd.present(drawable, afterMinimumDuration: interval)
        cmd.addCompletedHandler { [weak self] cb in
            guard let self else { return }
            let gpu = cb.gpuEndTime - cb.gpuStartTime
            self.statsLock.lock()
            if gpu > 0 { self.gpuAccum += gpu; self.gpuSamples += 1 }
            self.statsLock.unlock()
            self.inFlight.signal()
        }
        cmd.commit()
        account(link.timestamp)
    }

    private func account(_ ts: CFTimeInterval) {
        if lastTimestamp > 0 { windowWorst = max(windowWorst, (ts - lastTimestamp) * 1000) }
        lastTimestamp = ts
        if windowStart == 0 { windowStart = ts }
        windowFrames += 1
        let elapsed = ts - windowStart
        guard elapsed >= 0.5 else { return }
        statsLock.lock()
        let gpuMs = gpuSamples > 0 ? gpuAccum / Double(gpuSamples) * 1000 : 0
        gpuAccum = 0; gpuSamples = 0
        statsLock.unlock()
        let stats = FrameStats(fps: Double(windowFrames) / elapsed, frameMs: elapsed / Double(windowFrames) * 1000,
                               gpuMs: gpuMs, worstMs: windowWorst, drawableSize: metalLayer.drawableSize)
        windowStart = ts; windowFrames = 0; windowWorst = 0
        onStats?(stats)
    }

    deinit { link?.invalidate() }
}

struct MetalStage: UIViewRepresentable {
    var iterations: UInt32
    var frameCap: Int
    var onStats: (FrameStats) -> Void

    func makeUIView(context: Context) -> MetalStageView {
        let v = MetalStageView(frame: .zero)
        v.onStats = onStats
        return v
    }

    func updateUIView(_ v: MetalStageView, context: Context) {
        v.iterations = iterations
        v.frameCap = frameCap
        v.onStats = onStats
    }
}
