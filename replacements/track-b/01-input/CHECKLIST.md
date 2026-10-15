<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

## Design

GameInput.swift owns main-actor gesture and hardware state, with internal pure geometry and key-table helpers. ControllerBridge.swift owns four slots and a serial user-interactive sampler. Keyboard-mode samples enqueue one main-actor transaction: cursor move, releases, presses. No custom locks. XCTest exercises value helpers separately from UIKit event delivery.

Status `done` means implemented and reviewed by reading; compilation and device results remain unverified. Ownership questions B01-1 and B01-2 are recorded in track B QUESTIONS.md.

| Spec ref | Requirement | File | Function | Status |
|---|---|---|---|---|
| A/API | Singleton, modes, default XInput, queue | ControllerBridge.swift | ControllerBridge | done |
| A/setMode; clarification 4 | Outgoing HID disconnect or keyboard release, immediate ordered sample | ControllerBridge.swift | setMode | done |
| A/start | Main-actor idempotent observers | ControllerBridge.swift | start | done |
| A/1; clarifications 1,2,5 | Persistent lowest free slots, refill waiting pads, once-per-wait log, zero-based logs | ControllerBridge.swift | reconcile | done |
| A/2 | Active 4 ms timer, 1 ms leeway, stopped without slots | ControllerBridge.swift | updateTimer | done |
| A/3 | Inactive connected neutral and disconnected empty slots | ControllerBridge.swift | sample | done |
| A/4 buttons | All d-pad, face, shoulder, thumb, menu/options/home bits | ControllerBridge.swift | read | done |
| A/4 triggers | Finite rounded clamped 0...255 | ControllerBridge.swift | trigger | done |
| A/4 sticks | Asymmetric full signed rounding, finite, no dead zone, y up | ControllerBridge.swift | axis | done |
| A/5 | Player 1 route, other slots XInput | ControllerBridge.swift | publish | done |
| A/6; clarification 7 | First button per connection lowercase unpadded hex and route log | ControllerBridge.swift | sample | done |
| A/7 | Trace no-button to button transitions | ControllerBridge.swift | sample | done |
| A/keyboard buttons; clarification 6 | Specified VK map, GUIDE ignored | ControllerBridge.swift | keys | done |
| A/keyboard left stick | Strict half-deflection WASD | ControllerBridge.swift | keys | done |
| A/keyboard triggers | Strict >128 mouse threshold | ControllerBridge.swift | publish | done |
| A/keyboard differences | Move then releases then presses, trace new presses; clarification27 union ownership | GameInput.swift | bApplyController | done |
| A/keyboard cursor | 15% squared rescaling, full scale 1400px/s, fractional 4ms carry, up inversion | ControllerBridge.swift | cursorStep | done |
| B/API | Exact public surface and initial cursor/mode | GameInput.swift | GameInput | done |
| B/start | Current devices, observers, release on disconnect/inactive | GameInput.swift | start | done |
| B/load; clarification 12 | Per-program mode, unknown off, recenter per session | GameInput.swift | load | done |
| B/setTouchMode | Save, log, reset gestures, hide arrow, surfaceShown | GameInput.swift | setTouchMode | done |
| B/surfaceShown | Trackpad initial reach center and absolute placement | GameInput.swift | surfaceShown | done |
| B/geometry desktop | Minimum 1 dimensions | GameInput.swift | geometry | done |
| B/geometry picture | Drawable-size query, positive-size fallback | GameInput.swift | geometry | done |
| B/geometry reach | Intersection, empty desktop fallback, max-1 clamp | GameInput.swift | InputGeometry | done |
| B/desktopPoint | Proportional floor, reach clamp | GameInput.swift | desktopPoint | done |
| B/moveCursor | Clamp/store absolute MOVE, draw fraction or hide | GameInput.swift | moveCursor | done |
| B/focus; clarification 11 | Window/visibility/activity gates hardware, text and stick; releases pass | GameInput.swift | focused | done |
| B/indirectPointer | Ignore duplicate mouse touch | GameInput.swift | accepted | done |
| B/off | Only stationary >=3-finger menu under 0.6s | GameInput.swift | finishGesture | done |
| B/trackpad begin | First time placement, strict <0.35 tap-hold, max count | GameInput.swift | touchesBegan | done |
| B/trackpad movement | 1.3 picture/view width gain, >10 displacement | GameInput.swift | touchesMoved | done |
| B/trackpad wheel | Average live y,14pt/notch, up negative120 | GameInput.swift | scroll | done |
| B/trackpad end | Drag up/menu/<0.3s click, one-finger timestamp, double click | GameInput.swift | finishGesture | done |
| B/trackpad cancel | Release drag, reset | GameInput.swift | touchesCancelled | done |
| B/direct begin | Hide cursor, ignore joining drag fingers, record landings | GameInput.swift | touchesBegan | done |
| B/direct drag; clarification 17 | Generation hold timer, exactly one throughout, >10 motion, phases0/1/2 | GameInput.swift | beginDirectDrag | done |
| B/direct wheel; clarification 18 | First-notch midpoint move then positionless wheel | GameInput.swift | scroll | done |
| B/direct end | Landing midpoint touch/right/menu, cursor every point | GameInput.swift | finishGesture | done |
| Clarifications 13,14,15,16 | Strict times, per-finger displacement, wheel travel, count rebase | GameInput.swift | gesture helpers | done |
| B/keyboard mapping; clarification 21 | Complete specified US HID table and explicit ignored keys | GameInput.swift | virtualKey | done |
| B/keyboard events; clarification 22 | Main queue, per-VK down, focus, no repeat, release, trace, vendor log | GameInput.swift | keyboardChanged | done |
| B/mouse motion; clarification 23 | Main queue, invert y, double carry, whole +/-30000, desktop clamp, raw arrow fraction | GameInput.swift | mouseMoved | done |
| B/mouse buttons; clarification 8 | Per-button held, focus, unconditional release, coordinate-free flags | GameInput.swift | mouseButton | done |
| B/mouse wheel; clarification 9 | Signed units, individual 120 notches,max100, retained remainder | GameInput.swift | mouseWheel | done |
| B/type; clarifications 19,20,28 | Focus, trace, hardware suppression, US mapping, smart punctuation, preserve held keys/Shift | GameInput.swift | type | done |
| B/backspace | Focus, trace, hardware suppression, VK_BACK pulse | GameInput.swift | backspace | done |
| B/releaseAll; clarification 10 | Hardware and controller only, touch holds independent, carries reset, reason log | GameInput.swift | releaseAll | done |
| B/traceInput | Trace new click, tap, key, button | GameInput.swift | traceInput | done |
| interfaces/app-api.md | Every caller's signature and GameHostView usage | both Swift files | public API | done |
| Done means | Pure XCTest key/stick/geometry and on-device plan | tests/, TESTS.md | tests | done |
| Process | Provenance, questions, source review, commit, no local compile claim | PROVENANCE.md | review | done |
| Clarification 24 | Waiting controllers follow GCController.controllers order | ControllerBridge.swift | reconcile | done |
| Clarification 25 | One scroll position move per first-to-last-finger gesture | GameInput.swift | scroll/resetGesture | done |
| Clarification 26 | Mode application releases existing touch hold before reset | GameInput.swift | applyMode/cancelGesture | done |
| Clarification 27 | Per-source union ownership; only first down and last up | GameInput.swift | bApplyController | done |
| Clarification 28 | Typed pulses preserve held keys/Shift | GameInput.swift | type/pulse | done |
