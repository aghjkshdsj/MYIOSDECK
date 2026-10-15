// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
#if canImport(CoreGraphics)
import CoreGraphics
#endif

/// Driving the app's UI with a game controller, without UIKit (tests/host/pad_nav checks it):
/// which item a direction moves to, repeat on hold, the stick as four directions, and the chord
/// that opens the game menu while a game has the controller.

enum PadDirection: CaseIterable { case up, down, left, right }

/// The controller's inputs as the UI uses them.
enum PadKey: Hashable, CaseIterable {
    case up, down, left, right
    case a, b, x, y, lb, rb, menu, view, home

    var direction: PadDirection? {
        switch self {
        case .up: return .up
        case .down: return .down
        case .left: return .left
        case .right: return .right
        default: return nil
        }
    }
}

enum FocusMath {
    /// The item a direction moves to from `from`: the nearest one lying that way, preferring the
    /// ones in line with it (overlapping across the direction), nil when there is none.
    static func next(from: CGRect, direction: PadDirection, candidates: [(id: String, frame: CGRect)]) -> String? {
        var best: (id: String, score: Double)?
        for c in candidates {
            let f = c.frame
            guard f != from else { continue }
            let primary: Double, along: (Double, Double, Double, Double)
            switch direction {
            case .right:
                guard f.midX >= from.maxX, Double(f.minX) > Double(from.minX) + 1 else { continue }
                primary = max(0, Double(f.minX - from.maxX))
                along = (Double(f.minY), Double(f.maxY), Double(from.minY), Double(from.maxY))
            case .left:
                guard f.midX <= from.minX, Double(f.maxX) < Double(from.maxX) - 1 else { continue }
                primary = max(0, Double(from.minX - f.maxX))
                along = (Double(f.minY), Double(f.maxY), Double(from.minY), Double(from.maxY))
            case .down:
                guard f.midY >= from.maxY, Double(f.minY) > Double(from.minY) + 1 else { continue }
                primary = max(0, Double(f.minY - from.maxY))
                along = (Double(f.minX), Double(f.maxX), Double(from.minX), Double(from.maxX))
            case .up:
                guard f.midY <= from.minY, Double(f.maxY) < Double(from.maxY) - 1 else { continue }
                primary = max(0, Double(from.minY - f.maxY))
                along = (Double(f.minX), Double(f.maxX), Double(from.minX), Double(from.maxX))
            }
            // Distance across the direction: 0 when the two overlap that way.
            let gap = max(0, max(along.0, along.2) - min(along.1, along.3))
            // Centre offset across the direction breaks ties between overlapping items.
            let centre = abs((along.0 + along.1) / 2 - (along.2 + along.3) / 2)
            let score = primary + gap * 4 + (gap == 0 ? centre * 0.05 : centre * 0.5)
            if best == nil || score < best!.score { best = (c.id, score) }
        }
        return best?.id
    }

    /// Where focus starts in a scope: the preferred item, else the top-most (then left-most).
    static func first(_ candidates: [(id: String, frame: CGRect, preferred: Bool)]) -> String? {
        let preferred = candidates.filter { $0.preferred }
        return (preferred.isEmpty ? candidates : preferred).min { a, b in
            abs(Double(a.frame.minY - b.frame.minY)) > 8 ? a.frame.minY < b.frame.minY : a.frame.minX < b.frame.minX
        }?.id
    }
}

/// The stick as directions, with hysteresis so a stick resting near the threshold does not flicker.
struct StickDirections {
    private(set) var held: PadDirection?
    static let on = 0.55, off = 0.35

    /// x right, y up, -1...1.
    mutating func update(x: Double, y: Double) -> PadDirection? {
        let ax = abs(x), ay = abs(y)
        if let h = held {
            let still: Bool
            switch h {
            case .left: still = x < -Self.off
            case .right: still = x > Self.off
            case .up: still = y > Self.off
            case .down: still = y < -Self.off
            }
            if still && max(ax, ay) < Self.on { return h }   // held, nothing stronger elsewhere
        }
        if max(ax, ay) < Self.on {
            held = held.flatMap { h in
                switch h {
                case .left: return x < -Self.off ? h : nil
                case .right: return x > Self.off ? h : nil
                case .up: return y > Self.off ? h : nil
                case .down: return y < -Self.off ? h : nil
                }
            }
            return held
        }
        held = ax >= ay ? (x > 0 ? .right : .left) : (y > 0 ? .up : .down)
        return held
    }
}

/// Presses from held keys: every key fires when it goes down; a direction held fires again after
/// `delay`, then every `interval` (a list scrolls while the D-pad is held).
struct PadRepeater {
    var delay = 0.38
    var interval = 0.085
    private var downAt: [PadKey: Double] = [:]
    private var lastFire: [PadKey: Double] = [:]

    init(delay: Double = 0.38, interval: Double = 0.085) {
        self.delay = delay
        self.interval = interval
    }

    mutating func step(held: Set<PadKey>, now: Double) -> [PadKey] {
        var fired: [PadKey] = []
        for k in downAt.keys where !held.contains(k) { downAt[k] = nil; lastFire[k] = nil }
        for k in PadKey.allCases where held.contains(k) {
            guard let start = downAt[k] else {
                downAt[k] = now; lastFire[k] = now
                fired.append(k)
                continue
            }
            guard k.direction != nil, now - start >= delay, now - (lastFire[k] ?? start) >= (lastFire[k] == start ? delay : interval) else { continue }
            lastFire[k] = now
            fired.append(k)
        }
        return fired
    }
}

/// The game menu while a game has the controller: View and Menu held together for `hold` seconds
/// (or the Home / guide button, reported by the caller as a press). Fires once per hold.
struct MenuChord {
    var hold = 0.6
    private var since: Double?
    private var fired = false

    init(hold: Double = 0.6) { self.hold = hold }

    mutating func step(view: Bool, menu: Bool, now: Double) -> Bool {
        guard view && menu else { since = nil; fired = false; return false }
        if since == nil { since = now }
        guard !fired, now - since! >= hold else { return false }
        fired = true
        return true
    }
}
