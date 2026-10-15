// SPDX-License-Identifier: GPL-3.0-or-later
// Host tests for driving the UI with a controller (App/Sources/Engine/PadNavCore.swift): where a
// direction moves focus, where it starts, the stick as directions, repeat on hold, the menu chord.

import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: String, line: Int = #line) {
    checks += 1
    if !ok { failures += 1; print("FAIL line \(line): \(what)") }
}

func r(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> CGRect {
    CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
}

// MARK: - A Library-like grid: 3 columns of 100x150 tiles, 20 apart, under a header row

var items: [(id: String, frame: CGRect)] = []
for row in 0..<3 { for col in 0..<3 {
    items.append(("tile\(row)\(col)", r(Double(20 + col * 120), Double(100 + row * 170), 100, 150)))
} }
items.append(("search", r(20, 20, 260, 44)))
items.append(("add", r(300, 20, 44, 44)))
func frame(_ id: String) -> CGRect { items.first { $0.id == id }!.frame }
func go(_ from: String, _ d: PadDirection) -> String? { FocusMath.next(from: frame(from), direction: d, candidates: items) }

check(go("tile00", .right) == "tile01", "right in a row")
check(go("tile01", .left) == "tile00", "left in a row")
check(go("tile02", .right) == nil, "nothing right of the last column")
check(go("tile00", .down) == "tile10", "down a column")
check(go("tile11", .down) == "tile21", "down keeps the column")
check(go("tile21", .up) == "tile11", "up keeps the column")
check(go("tile00", .up) == "search", "up from the first row to the search field above it")
check(go("tile02", .up) == "add", "up from the right column to the button above it")
check(go("search", .down) == "tile00" || go("search", .down) == "tile01", "down from the wide search field into the grid: \(go("search", .down) ?? "nil")")
check(go("search", .right) == "add", "right from search to +")
check(go("add", .down) == "tile02", "down from + to the column below it")
check(go("tile22", .down) == nil, "nothing below the last row")

// MARK: - A settings layout: a sidebar on the left, rows on the right

var settings: [(id: String, frame: CGRect)] = [
    ("side.appearance", r(10, 80, 240, 52)), ("side.performance", r(10, 140, 240, 52)), ("side.controls", r(10, 200, 240, 52)),
    ("theme", r(500, 90, 330, 42)), ("glass", r(500, 150, 330, 30)), ("toggle.artwork", r(290, 230, 560, 52)),
    ("toggle.anim", r(290, 290, 560, 52)),
]
func s(_ from: String, _ d: PadDirection) -> String? {
    FocusMath.next(from: settings.first { $0.id == from }!.frame, direction: d, candidates: settings)
}
check(s("side.appearance", .right) == "theme", "from the sidebar into the panel, the row in line")
check(s("side.controls", .right) == "toggle.artwork", "from a lower sidebar item into the row nearest in line")
check(s("toggle.artwork", .left) == "side.controls", "back left to the sidebar item in line")
check(s("theme", .down) == "glass", "down the panel")
check(s("glass", .down) == "toggle.artwork", "down to the wide toggle row")
check(s("side.appearance", .down) == "side.performance", "down the sidebar")
settings.append(("far", r(290, 2000, 560, 52)))
check(s("toggle.anim", .down) == "far", "a far item below is still reachable (the engine limits the range)")

// MARK: - Overlapping rows prefer the one in line; a header row of round buttons

let header: [(id: String, frame: CGRect)] = [
    ("pill", r(300, 10, 380, 44)), ("search", r(760, 10, 44, 44)), ("settings", r(816, 10, 44, 44)), ("controller", r(872, 10, 44, 44)),
    ("play", r(10, 250, 150, 52)), ("more", r(174, 250, 52, 52)), ("card0", r(10, 330, 200, 90)), ("card1", r(224, 330, 200, 90)),
]
func h(_ from: String, _ d: PadDirection) -> String? {
    FocusMath.next(from: header.first { $0.id == from }!.frame, direction: d, candidates: header)
}
check(h("pill", .right) == "search", "pill to search")
check(h("search", .right) == "settings" && h("settings", .right) == "controller", "along the round buttons")
check(h("play", .right) == "more", "Play to …")
check(h("play", .down) == "card0", "Play down to the first card")
check(h("more", .down) == "card0" || h("more", .down) == "card1", "… down to a card")
check(h("card1", .left) == "card0", "cards left")
check(h("play", .up) == "pill", "Play up to the header: \(h("play", .up) ?? "nil")")

// MARK: - Where focus starts

check(FocusMath.first([("a", r(0, 100, 10, 10), false), ("b", r(0, 10, 10, 10), false)]) == "b", "top-most first")
check(FocusMath.first([("a", r(50, 10, 10, 10), false), ("b", r(0, 12, 10, 10), false)]) == "b", "same row: left-most")
check(FocusMath.first([("a", r(0, 10, 10, 10), false), ("play", r(0, 300, 10, 10), true)]) == "play", "the preferred item wins")
check(FocusMath.first([]) == nil, "nothing to focus")
check(FocusMath.first([("b", r(0, 300, 10, 10), true), ("x", r(0, 0, 10, 10), false), ("a", r(0, 100, 10, 10), true)]) == "a",
      "several preferred: the top-most of them")

// MARK: - Stick

var stick = StickDirections()
check(stick.update(x: 0.2, y: 0.1) == nil, "dead zone")
check(stick.update(x: 0.8, y: 0.1) == .right, "right")
check(stick.update(x: 0.45, y: 0.1) == .right, "still right above the release threshold (hysteresis)")
check(stick.update(x: 0.2, y: 0) == nil, "released")
check(stick.update(x: 0.3, y: -0.9) == .down, "down (y up is positive)")
check(stick.update(x: 0.9, y: -0.5) == .right, "a stronger push the other way takes over")
check(stick.update(x: -0.1, y: 0.7) == .up, "up")

// MARK: - Repeat on hold

var rep = PadRepeater(delay: 0.4, interval: 0.1)
check(rep.step(held: [.down], now: 0) == [.down], "a press fires at once")
check(rep.step(held: [.down], now: 0.2) == [], "no repeat before the delay")
check(rep.step(held: [.down], now: 0.41) == [.down], "repeat after the delay")
check(rep.step(held: [.down], now: 0.45) == [], "not again before the interval")
check(rep.step(held: [.down], now: 0.52) == [.down], "then every interval")
check(rep.step(held: [], now: 0.6) == [], "release")
check(rep.step(held: [.down], now: 0.61) == [.down], "a new press fires at once")
var buttons = PadRepeater()
check(buttons.step(held: [.a], now: 0) == [.a], "A fires on press")
check(buttons.step(held: [.a], now: 2) == [], "buttons do not repeat")
check(buttons.step(held: [.a, .b], now: 2.1) == [.b], "a second button while the first is held")
var fired = 0
var hold = PadRepeater(delay: 0.38, interval: 0.085)
var t = 0.0
while t < 1.0 { fired += hold.step(held: [.right], now: t).count; t += 1.0 / 60 }
check(fired >= 7 && fired <= 10, "about 9 moves in a second of holding: \(fired)")

// MARK: - The game menu chord

var chord = MenuChord(hold: 0.6)
check(!chord.step(view: true, menu: false, now: 0), "View alone does nothing")
check(!chord.step(view: true, menu: true, now: 0.1), "both, not long enough")
check(!chord.step(view: true, menu: true, now: 0.5), "still not")
check(chord.step(view: true, menu: true, now: 0.75), "held 0.65 s: the menu")
check(!chord.step(view: true, menu: true, now: 1.5), "once per hold")
check(!chord.step(view: false, menu: true, now: 1.6), "released")
check(!chord.step(view: true, menu: true, now: 1.7), "a new hold starts over")
check(chord.step(view: true, menu: true, now: 2.4), "and fires again")

print("pad-nav tests: \(checks - failures)/\(checks) passed")
exit(failures == 0 ? 0 : 1)
