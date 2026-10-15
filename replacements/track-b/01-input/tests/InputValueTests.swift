// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

import XCTest
import CoreGraphics
// Add this file and both production sources to the SAME iOS XCTest target,
// together with the app interfaces and logging/GameHostView test doubles.

final class InputValueTests: XCTestCase {
    func testHIDRangesAndExceptions() {
        for i in 0..<26 { XCTAssertEqual(BInputKeys.virtualKey(4 + i), Int32(65 + i)) }
        for i in 0..<9 { XCTAssertEqual(BInputKeys.virtualKey(0x1e + i), Int32(0x31 + i)) }
        XCTAssertEqual(BInputKeys.virtualKey(0x27), 0x30)
        for i in 0..<12 {
            XCTAssertEqual(BInputKeys.virtualKey(0x3a + i), Int32(0x70 + i))
            XCTAssertEqual(BInputKeys.virtualKey(0x68 + i), Int32(0x7c + i))
        }
        let special: [(Int, Int32)] = [
            (0x28,0x0d),(0x29,0x1b),(0x2a,0x08),(0x2b,0x09),(0x2c,0x20),
            (0x2d,0xbd),(0x2e,0xbb),(0x2f,0xdb),(0x30,0xdd),(0x31,0xdc),(0x32,0xdc),
            (0x33,0xba),(0x34,0xde),(0x35,0xc0),(0x36,0xbc),(0x37,0xbe),(0x38,0xbf),
            (0x39,0x14),(0x46,0x2c),(0x47,0x91),(0x48,0x13),(0x49,0x2d),(0x4a,0x24),
            (0x4b,0x21),(0x4c,0x2e),(0x4d,0x23),(0x4e,0x22),(0x4f,0x27),(0x50,0x25),
            (0x51,0x28),(0x52,0x26),(0x53,0x90),(0x54,0x6f),(0x55,0x6a),(0x56,0x6d),
            (0x57,0x6b),(0x58,0x0d),(0x62,0x60),(0x63,0x6e),(0x64,0xe2),(0x65,0x5d),
            (0xe0,0xa2),(0xe1,0xa0),(0xe2,0xa4),(0xe3,0x5b),(0xe4,0xa3),(0xe5,0xa1),
            (0xe6,0xa5),(0xe7,0x5c)
        ]
        for (hid,vk) in special { XCTAssertEqual(BInputKeys.virtualKey(hid), vk) }
        for i in 0..<9 { XCTAssertEqual(BInputKeys.virtualKey(0x59+i), Int32(0x61+i)) }
        for hid in [0,1,2,3,0x66,0x67,0x76,0x7f,0x80,0x81,0x85,0x86,0x87,0x90,0xe8,0xffff] {
            XCTAssertNil(BInputKeys.virtualKey(hid))
        }
    }

    func testUSCharactersAndSmartPunctuation() {
        for scalar in UInt32(32)...126 {
            XCTAssertNotNil(BInputKeys.textKey(Character(UnicodeScalar(scalar)!)))
        }
        XCTAssertEqual(BInputKeys.textKey("A")?.key, 0x41)
        XCTAssertEqual(BInputKeys.textKey("A")?.shift, true)
        XCTAssertEqual(BInputKeys.textKey("a")?.shift, false)
        XCTAssertEqual(BInputKeys.textKey("?")?.key, 0xbf)
        XCTAssertEqual(BInputKeys.textKey("?")?.shift, true)
        XCTAssertEqual(BInputKeys.textKey("\r\n")?.key, 0x0d)
        XCTAssertNil(BInputKeys.textKey("é"))
        XCTAssertNil(BInputKeys.textKey("🎮"))
        XCTAssertEqual(BInputKeys.normalized("‘’‚′“”„″–—−…\u{00a0}"), "''''\"\"\"\"---... ")
    }

    func testControllerBoundsAndCurve() {
        XCTAssertEqual(BPadMath.axis(-1), -32768)
        XCTAssertEqual(BPadMath.axis(1), 32767)
        XCTAssertEqual(BPadMath.axis(-0.5), -16384)
        XCTAssertEqual(BPadMath.axis(0.5), 16384)
        XCTAssertEqual(BPadMath.axis(.nan), 0)
        XCTAssertEqual(BPadMath.axis(.infinity), 0)
        XCTAssertEqual(BPadMath.trigger(0.5), 128)
        XCTAssertEqual(BPadMath.trigger(-1), 0)
        XCTAssertEqual(BPadMath.trigger(2), 255)
        XCTAssertEqual(BPadMath.trigger(.nan), 0)
        XCTAssertEqual(BPadMath.curve(0), 0)
        XCTAssertEqual(BPadMath.curve(4915), 0)
        XCTAssertEqual(BPadMath.curve(-4915), 0)
        XCTAssertEqual(BPadMath.curve(32767), 1, accuracy: 1e-10)
        XCTAssertEqual(BPadMath.curve(-32768), -1, accuracy: 1e-10)
        let half = BPadMath.curve(BPadMath.axis(0.575))
        XCTAssertEqual(half, 0.25, accuracy: 0.0001)
        XCTAssertEqual(BPadMath.curve(32767) * 1400 * 0.004, 5.6, accuracy: 1e-10)
        XCTAssertEqual(BPadMath.keys(buttons: 0x0400, lx: 16384, ly: -16384), [])
        XCTAssertEqual(BPadMath.keys(buttons: 0x1000, lx: -16385, ly: 16385), [0x20,0x41,0x57])
    }

    func testPictureMappingAndClamping() {
        let g = BInputGeometry(width: 1024, height: 768, picture: CGRect(x: -20, y: 40, width: 800, height: 600))
        XCTAssertEqual(g.reach, CGRect(x: 0, y: 40, width: 780, height: 600))
        let bounds = CGRect(x: 10, y: 20, width: 400, height: 300)
        XCTAssertEqual(g.map(CGPoint(x: 210.9, y: 170.9), bounds: bounds), CGPoint(x: 381, y: 341))
        XCTAssertEqual(g.map(CGPoint(x: 500, y: 500), bounds: bounds), CGPoint(x: 779, y: 639))
        XCTAssertEqual(g.map(CGPoint(x: -100, y: -100), bounds: bounds), CGPoint(x: 0, y: 40))
        let tiny = BInputGeometry(width: 0, height: -10, picture: nil)
        XCTAssertEqual(tiny.reach, CGRect(x: 0, y: 0, width: 1, height: 1))
        XCTAssertEqual(BInputGeometry.clamp(CGPoint(x: 99, y: .nan), to: tiny.reach), .zero)
        let outside = BInputGeometry(width: 100, height: 100, picture: CGRect(x: 200, y: 200, width: 10, height: 10))
        XCTAssertEqual(outside.reach, outside.desktop)
        XCTAssertEqual(outside.unit(.zero), CGPoint(x: -20, y: -20))
    }
}
