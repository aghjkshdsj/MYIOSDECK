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

// MARK: - Build 193 reports (Backbone Pro, landscape 932 x 430)

// 1. Home: the last recent card partly off the right edge; Left goes to the card before it.
let home: [FocusCandidate] = [
    FocusCandidate(id: "pill", frame: r(300, 10, 400, 44), section: "header"),
    FocusCandidate(id: "search", frame: r(770, 10, 44, 44), section: "header"),
    FocusCandidate(id: "play", frame: r(75, 186, 155, 56), preferred: true),
    FocusCandidate(id: "more", frame: r(245, 188, 52, 52)),
    FocusCandidate(id: "viewLibrary", frame: r(820, 262, 105, 24)),
    FocusCandidate(id: "dokimon", frame: r(76, 317, 214, 96)),
    FocusCandidate(id: "second", frame: r(306, 317, 214, 96)),
    FocusCandidate(id: "operator", frame: r(536, 317, 214, 96)),
    FocusCandidate(id: "deponia", frame: r(766, 317, 214, 96)),
]
func hm(_ from: String, _ d: PadDirection) -> String? {
    let c = home.first { $0.id == from }!
    return FocusMath.next(from: c.frame, section: c.section, direction: d, candidates: home)
}
check(hm("deponia", .left) == "operator", "Left from the card at the right edge: \(hm("deponia", .left) ?? "nil")")
check(hm("operator", .right) == "deponia", "Right to the card at the right edge")
check(hm("deponia", .up) == "viewLibrary", "Up from the last card to View library")
check(hm("pill", .down) == "play", "from the top bar down: the screen's main button: \(hm("pill", .down) ?? "nil")")
check(hm("play", .up) == "pill", "Play up to the top bar")

// 2. LB / RB: the top bar's destinations in order, LB the exact reverse.
check(TopDestination.home.next(1) == .library && TopDestination.library.next(1) == .downloads, "RB through the tabs")
check(TopDestination.downloads.next(1) == .search, "RB from Downloads: Search")
check(TopDestination.search.next(1) == .settings && TopDestination.settings.next(1) == .controllers, "then Settings, then Controllers")
check(TopDestination.controllers.next(1) == .home, "and round to Home")
for d in TopDestination.allCases { check(d.next(1).next(-1) == d && d.next(-1).next(1) == d, "LB undoes RB at \(d)") }
check(TopDestination.home.next(-1) == .controllers, "LB from Home: Controllers")

// 3/4/5. Settings in landscape: a sidebar and a panel. The panel goes on below the screen (Storage).
let sidebarNames = ["appearance", "performance", "controls", "steam", "storage", "logs", "about"]
var settingsUI: [FocusCandidate] = [FocusCandidate(id: "pill", frame: r(300, 10, 400, 44), section: "header"),
                                    FocusCandidate(id: "gear", frame: r(828, 10, 44, 44), section: "header")]
for (i, n) in sidebarNames.enumerated() {
    settingsUI.append(FocusCandidate(id: "side.\(n)", frame: r(75, Double(118 + i * 60), 255, 52), section: "sidebar",
                                     preferred: n == "storage"))
}
settingsUI.append(FocusCandidate(id: "sort", frame: r(778, 247, 147, 42), section: "panel"))
for i in 0..<5 { settingsUI.append(FocusCandidate(id: "game\(i)", frame: r(358, Double(310 + i * 90), 567, 80), section: "panel")) }
settingsUI.append(FocusCandidate(id: "clear", frame: r(838, 790, 90, 40), section: "panel"))
func st(_ from: String, _ d: PadDirection, _ list: [FocusCandidate] = settingsUI) -> String? {
    let c = list.first { $0.id == from }!
    return FocusMath.next(from: c.frame, section: c.section, direction: d, candidates: list)
}
check(st("sort", .down) == "game0", "Storage: the sort menu down to the first game")
var at = "game0", seen = ["game0"]
while let n = st(at, .down) { seen.append(n); at = n; if seen.count > 20 { break } }
check(seen == ["game0", "game1", "game2", "game3", "game4", "clear"], "Down goes through every game to Clear, below the fold too: \(seen)")
check(st("clear", .down) == nil, "the end of the panel stays put (no jump to the sidebar)")
check(st("game1", .left) == "side.storage", "Left from the panel: the sidebar's current section, not the item in line: \(st("game1", .left) ?? "nil")")
check(st("game4", .left) == "side.storage", "Left from far below the sidebar: still its current section")
check(st("side.storage", .right) == "game0" || st("side.storage", .right) == "game1", "Right from the sidebar into the panel, in line: \(st("side.storage", .right) ?? "nil")")
check(st("game0", .up) == "sort", "Up the panel to the sort menu")
check(st("side.about", .down) == nil, "the end of the sidebar stays put")
// Appearance: a short panel; Down from its last row must not reach Logs below-left.
let appearance: [FocusCandidate] = [
    FocusCandidate(id: "side.appearance", frame: r(75, 118, 255, 52), section: "sidebar", preferred: true),
    FocusCandidate(id: "side.logs", frame: r(75, 418, 255, 52), section: "sidebar"),
    FocusCandidate(id: "theme", frame: r(560, 150, 330, 42), section: "panel"),
    FocusCandidate(id: "glass", frame: r(560, 205, 330, 30), section: "panel"),
    FocusCandidate(id: "artwork", frame: r(358, 260, 567, 52), section: "panel"),
    FocusCandidate(id: "reduce", frame: r(358, 360, 567, 52), section: "panel"),
]
check(st("reduce", .down, appearance) == nil, "Appearance: Down from the last row stays in the panel: \(st("reduce", .down, appearance) ?? "nil")")
check(st("artwork", .left, appearance) == "side.appearance", "Left from a panel row: the current section")
check(st("theme", .up, settingsUI + appearance.filter { $0.section == "panel" }) == "pill" || st("theme", .up, settingsUI + appearance.filter { $0.section == "panel" }) == "gear",
      "Up from the panel's top row to the top bar")

// 5. The stick: a push mostly left with some up is Left, never Up; a 45° push is nothing.
var s5 = StickDirections()
check(s5.update(x: -0.8, y: 0.5) == .left, "mostly left with some up: Left")
check(s5.update(x: -0.7, y: 0.6) == .left, "held left while the push drifts up: still Left")
check(s5.update(x: -0.5, y: 0.75) == .left, "drifting further up without clearly taking over: still Left")
check(s5.update(x: -0.2, y: 0.9) == .up, "clearly up now: Up")
var s6 = StickDirections()
check(s6.update(x: -0.65, y: 0.6) == nil, "a 45-degree push from rest: no direction")
check(s6.update(x: -0.9, y: 0.4) == .left, "then mostly left: Left")

// MARK: - Build 194 reports

// 2. Right from a sidebar row level with nothing in the panel (Controls: the panel's rows are above
//    and below it) still enters the panel.
let controls: [FocusCandidate] = [
    FocusCandidate(id: "side.appearance", frame: r(75, 118, 255, 52), section: "sidebar"),
    FocusCandidate(id: "side.controls", frame: r(75, 238, 255, 52), section: "sidebar", preferred: true),
    FocusCandidate(id: "pill", frame: r(300, 10, 400, 44), section: "header"),
    FocusCandidate(id: "show", frame: r(556, 172, 352, 44), section: "panel"),
    FocusCandidate(id: "haptics", frame: r(375, 325, 535, 32), section: "panel"),
    FocusCandidate(id: "edit", frame: r(375, 370, 260, 40), section: "panel"),
]
check(st("side.controls", .right, controls) == "show" || st("side.controls", .right, controls) == "haptics",
      "Right from Controls enters its panel: \(st("side.controls", .right, controls) ?? "nil")")
check(st("haptics", .left, controls) == "side.controls", "and Left comes back to Controls")
check(st("edit", .right, controls) == nil, "nothing beside the panel's last button: stays (not the top bar)")

// 3. A choice never traps the focus: past its first / last value the press moves on.
check(ChoiceStep.index(0, count: 4, delta: -1) == nil, "Left at the first value: move on")
check(ChoiceStep.index(0, count: 4, delta: 1) == 1, "Right from the first value: the next value")
check(ChoiceStep.index(3, count: 4, delta: 1) == nil, "Right at the last value: move on")
check(ChoiceStep.index(2, count: 4, delta: -1) == 1, "Left in the middle: the value before")
check(ChoiceStep.index(0, count: 0, delta: 1) == nil, "no values")
check(ChoiceStep.index(0, count: 1, delta: 1) == nil && ChoiceStep.index(0, count: 1, delta: -1) == nil, "a single value never takes the press")
// The Resolution control (540p selected) in a game's Display panel: Left leaves it for the sidebar.
let display: [FocusCandidate] = [
    FocusCandidate(id: "side.performance", frame: r(72, 125, 245, 52), section: "sidebar"),
    FocusCandidate(id: "side.controls", frame: r(72, 193, 245, 52), section: "sidebar"),
    FocusCandidate(id: "side.display", frame: r(72, 261, 245, 52), section: "sidebar", preferred: true),
    FocusCandidate(id: "resolution", frame: r(515, 212, 405, 52), section: "panel"),
    FocusCandidate(id: "save", frame: r(785, 390, 142, 40), section: "panel"),
]
check(st("resolution", .left, display) == "side.display", "Left from the resolution control (after its first value): the sidebar's Display")
check(st("resolution", .down, display) == "save", "Down from it: Save changes")

print("pad-nav tests: \(checks - failures)/\(checks) passed")
exit(failures == 0 ? 0 : 1)
