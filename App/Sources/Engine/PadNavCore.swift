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

/// An item focus can move to: its frame, the section it belongs to (a sidebar, a panel, the top
/// bar: "" for the page itself) and whether it is its section's current item (a sidebar's selected
/// section, the screen's main button).
struct FocusCandidate {
    var id: String
    var frame: CGRect
    var section = ""
    var preferred = false
}

enum FocusMath {
    /// The item a direction moves to from `from` (in `section`), nil when there is none:
    /// 1. items in line with it (overlapping across the direction), the nearest;
    /// 2. else items of its own section that way (a grid's shorter last row);
    /// 3. else, going up or down, the top bar from the page or the page from the top bar; going left
    ///    or right, another section's current item (from a settings panel to the sidebar's selected section).
    /// Entering another section lands on that section's current item when it lies that way.
    /// Never a jump to another section's item out of line (the end of a panel stays put).
    static func next(from: CGRect, section: String = "", direction: PadDirection, candidates: [FocusCandidate]) -> String? {
        struct Scored { let c: FocusCandidate; let score: Double; let inline: Bool }
        var all: [Scored] = []
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
            all.append(Scored(c: c, score: primary + gap * 4 + (gap == 0 ? centre * 0.05 : centre * 0.5), inline: gap == 0))
        }
        func best(_ list: [Scored]) -> FocusCandidate? { list.min { $0.score < $1.score }?.c }
        let horizontal = direction == .left || direction == .right
        var pick = best(all.filter { $0.inline })
            ?? (horizontal ? nil : best(all.filter { $0.c.section == section }))
            ?? (horizontal ? nil : best(all.filter { $0.c.section == "header" || section == "header" }))
        if let p = pick, p.section != section,
           let current = best(all.filter { $0.c.section == p.section && $0.c.preferred }) {
            pick = current
        }
        if pick == nil, horizontal {
            // Into another section beside: its current item, else its nearest one that way (a
            // sidebar row level with nothing in the panel still enters the panel).
            pick = best(all.filter { $0.c.section != section && $0.c.preferred })
                ?? best(all.filter { $0.c.section != section && $0.c.section != "header" })
        }
        return pick?.id
    }

    /// The same without sections (every item in one).
    static func next(from: CGRect, direction: PadDirection, candidates: [(id: String, frame: CGRect)]) -> String? {
        next(from: from, direction: direction, candidates: candidates.map { FocusCandidate(id: $0.id, frame: $0.frame) })
    }

    /// Where focus starts in a scope: the top-most preferred item, else the top-most (then left-most).
    static func first(_ candidates: [(id: String, frame: CGRect, preferred: Bool)]) -> String? {
        let preferred = candidates.filter { $0.preferred }
        return (preferred.isEmpty ? candidates : preferred).min { a, b in
            abs(Double(a.frame.minY - b.frame.minY)) > 8 ? a.frame.minY < b.frame.minY : a.frame.minX < b.frame.minX
        }?.id
    }
}

/// The stick as directions: the dominant axis only when it clearly dominates (a push between the
/// two, around 40° to 50°, is no direction), the held axis kept until the other one clearly takes
/// over, and hysteresis so a stick resting near the threshold does not flicker.
struct StickDirections {
    private(set) var held: PadDirection?
    static let on = 0.55, off = 0.35
    /// The dominant axis must be this many times the other to count (about 39° from the axis).
    static let dominance = 1.25
    /// A held direction gives way to the other axis only past this ratio.
    static let switchRatio = 1.6

    /// x right, y up, -1...1.
    mutating func update(x: Double, y: Double) -> PadDirection? {
        let ax = abs(x), ay = abs(y)
        func dir(horizontal: Bool) -> PadDirection { horizontal ? (x > 0 ? .right : .left) : (y > 0 ? .up : .down) }
        if let h = held {
            let horizontal = h == .left || h == .right
            let along = horizontal ? x : y, across = horizontal ? ay : ax
            let sameSide = (h == .right || h == .up) ? along > Self.off : along < -Self.off
            if sameSide {
                // Still pushed that way: keep it unless the other axis clearly takes over.
                if across >= Self.on, across > abs(along) * Self.switchRatio {
                    held = dir(horizontal: !horizontal)
                }
                return held
            }
            held = nil
        }
        guard max(ax, ay) >= Self.on else { return nil }
        if ax >= ay * Self.dominance { held = dir(horizontal: true) }
        else if ay >= ax * Self.dominance { held = dir(horizontal: false) }
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

/// LB / RB: the top bar's destinations in their order on screen (the three tabs, then Search,
/// Settings and the Controllers panel); LB goes the exact reverse way.
enum TopDestination: CaseIterable {
    case home, library, downloads, search, settings, controllers

    func next(_ step: Int) -> TopDestination {
        let all = Self.allCases
        let i = all.firstIndex(of: self) ?? 0
        let n = all.count
        return all[((i + step) % n + n) % n]
    }
}

/// Left / right on a choice (segmented options, a picker, a stepper): the next index, or nil at the
/// first / last one, where the press moves focus out instead (so a control never traps the focus).
enum ChoiceStep {
    static func index(_ i: Int, count: Int, delta: Int) -> Int? {
        let n = i + delta
        guard count > 0, n >= 0, n < count, n != i else { return nil }
        return n
    }
}
