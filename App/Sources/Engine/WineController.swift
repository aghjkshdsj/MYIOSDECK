// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController
import UIKit

/// Stage 2: runs Windows x86-64 programs through Wine ARM64EC + FEX (xtajit64)
/// in-process, using a slice of the JIT pool. One Wine session per app run.
final class WineController: ObservableObject, @unchecked Sendable {
    enum State: Equatable { case idle, booting(String), running(String), finished(String, String), failed(String) }

    struct Program: Identifiable {
        let id: String      // file name in the DLL farm (C:\windows\system32)
        let title: String
        let detail: String
        var graphics = false  // opens a window: shown on the game surface (stage 3)
        /// Runs without JIT: Wine's DLLs from signed dylibs, x64 code through FXI (docs/NO_JIT_WINDOWS.md).
        var noJIT = false
        /// The program file when it differs from id (one exe, two list entries).
        var exeName: String? = nil
        var exe: String { exeName ?? id }
    }

    /// The signed Wine dylibs (engine/pedylib), present when CI converted the DLL farm.
    static let peWineDir = Bundle.main.bundlePath + "/PE/wine"
    static var noJITWineAvailable: Bool {
        FileManager.default.fileExists(atPath: peWineDir + "/libntdll.dll.dylib")
    }

    /// Madeira's test programs, shipped in the ARM64EC DLL farm.
    static let programs = [
        Program(id: "hello-arm64ec.exe", title: "Windows Hello (ARM64EC, no JIT)",
                detail: "Wine from signed dylibs, no JIT needed: native Windows ARM code only", noJIT: true),
        Program(id: "hello-x64-nojit", title: "Windows Hello (x64, no JIT)",
                detail: "x64 code interpreted by FXI inside Wine: the App Store path", noJIT: true,
                exeName: "hello-x64.exe"),
        Program(id: "suite-x64.exe", title: "x64 test suite (no JIT)",
                detail: "FXI inside Wine: callbacks, threads, exceptions, C++ throw, longjmp, child process", noJIT: true),
        Program(id: "cube-x64-nojit", title: "Direct3D 11 cube (x64, no JIT)",
                detail: "D3D11 through DXMT to Metal from signed DLLs, the program's code in FXI",
                graphics: true, noJIT: true, exeName: "cube-x64.exe"),
        Program(id: "d3d12-cube-x64-nojit", title: "Direct3D 12 cube (x64, no JIT)",
                detail: "D3D12 through Madeira's converter to Metal from signed DLLs, the program's code in FXI",
                graphics: true, noJIT: true, exeName: "d3d12-cube-x64.exe"),
        Program(id: "gl-triangle-x64-nojit", title: "OpenGL triangle (x64, no JIT)",
                detail: "Desktop OpenGL 3.3 through Mesa Zink and MoltenVK to Metal, the program's code in FXI",
                graphics: true, noJIT: true, exeName: "gl-triangle-x64.exe"),
        Program(id: "hello-x64.exe", title: "Windows Hello (x64)", detail: "Console hello world through Wine + FEX ARM64EC"),
        Program(id: "fib-x64.exe", title: "Fibonacci (x64)", detail: "Recursive CPU test: x86-64 call/ret through FEX"),
        Program(id: "clocktest-x64.exe", title: "Clock test (x64)", detail: "Windows timers and QueryPerformanceCounter"),
        Program(id: "cube-x64.exe", title: "Direct3D 11 cube (x64)", detail: "Spinning cube: D3D11 through DXMT to Metal", graphics: true),
        Program(id: "d3d12-cube-x64.exe", title: "Direct3D 12 cube (x64)", detail: "Spinning cube: D3D12 through Madeira's converter to Metal", graphics: true),
        Program(id: "gl-triangle-x64.exe", title: "OpenGL triangle (x64)", detail: "Spinning triangle: OpenGL 3.3 through Mesa Zink and MoltenVK to Metal", graphics: true),
    ]

    @Published private(set) var state: State = .idle
    /// The Controller API this Wine session started with (its HID / DirectInput
    /// devices exist only if it started with them).
    @Published private(set) var sessionAPI = "xinput"

    static func bridgeMode(_ api: String) -> ControllerBridge.Mode {
        switch api {
        case "hid", "dualsense": return .hid
        case "keyboard": return .keyboard
        default: return .xinput
        }
    }

    /// In-game switch (the game menu). Saved for the next launch either way;
    /// applied now unless the mode needs a device this session was not started
    /// with. Returns whether it took effect now.
    @discardableResult
    func switchController(to api: String) -> Bool {
        UserDefaults.standard.set(api, forKey: "controllerAPI")
        let needsStart = (api == "hid" || api == "dualsense") && api != sessionAPI
        dlog("[xinput] in-game switch to \(api)\(needsStart ? " (applies at next launch)" : "")")
        guard !needsStart else { return false }
        ControllerBridge.shared.setMode(Self.bridgeMode(api))
        return true
    }
    let linked = mid_wine_linked()

    private let queue = DispatchQueue(label: "myiosdeck.wine", qos: .userInitiated)

    var prefixURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("wine-prefix")
    }

    /// Stage 4d: a Steam game started as its own program (no Steam client). The
    /// bridge publishes its identity (SteamAppId / SteamGameId / SteamAppPath)
    /// and starts it in Steam's working folder; it reads and clears these.
    /// Without JIT the game takes the no-JIT session (signed Wine DLLs, x64 code in FXI), as
    /// the built-in no-JIT programs do; Madeira picks ARM64EC for it from the full path.
    func runSteamGame(appID: Int, title: String, plan: SteamLaunchPlan) {
        let noJIT = !mid_jit_pool_ready()
        if noJIT {
            let machine = peMachine(windowsPath: plan.exe)
            dlog("[wine] no-JIT Steam launch: \(plan.exe) machine=0x\(String(machine, radix: 16))")
            if machine == 0x14c {
                state = .failed("This is a 32-bit game. Without JIT only 64-bit (x64) games can run: enable JIT to play it.")
                return
            }
        }
        setenv("MADEIRA_STEAM_APPID", String(appID), 1)
        setenv("MADEIRA_STEAM_APPPATH", plan.appPath, 1)
        setenv("MADEIRA_WORKDIR", plan.workingFolder, 1)
        for (key, value) in plan.environment { setenv(key, value, 1) }   // GameGraphics (Wine copies it to Windows)
        run(Program(id: plan.exe, title: title, detail: "", graphics: true, noJIT: noJIT), args: plan.arguments)
    }

    /// IMAGE_FILE_HEADER.Machine of a C:\ path in the prefix (0x8664 x64, 0x14c i386; 0 unreadable).
    func peMachine(windowsPath: String) -> UInt16 {
        guard windowsPath.count > 3, windowsPath.dropFirst().hasPrefix(":\\") else { return 0 }
        let rel = windowsPath.dropFirst(3).replacingOccurrences(of: "\\", with: "/")
        let url = prefixURL.appendingPathComponent("drive_c").appendingPathComponent(rel)
        guard let h = try? FileHandle(forReadingFrom: url) else { return 0 }
        defer { try? h.close() }
        guard let d = try? h.read(upToCount: 4096), d.count >= 0x40, d[0] == 0x4d, d[1] == 0x5a else { return 0 }
        let pe = Int(d[0x3c]) | Int(d[0x3d]) << 8 | Int(d[0x3e]) << 16 | Int(d[0x3f]) << 24
        guard pe > 0, pe + 6 <= d.count, d[pe] == 0x50, d[pe + 1] == 0x45 else { return 0 }
        return UInt16(d[pe + 4]) | UInt16(d[pe + 5]) << 8
    }

    func run(_ program: Program, args: String = "") {
        // Per-launch variables (runSteamGame) must not outlive a launch that never boots.
        func refuse(_ why: String) {
            for k in ["MADEIRA_STEAM_APPID", "MADEIRA_STEAM_APPPATH", "MADEIRA_WORKDIR"] { unsetenv(k) }
            state = .failed(why)
        }
        guard linked else { return refuse("This build does not include Wine yet.") }
        switch state {
        case .booting, .running, .finished:
            return refuse("Wine already ran a program in this session. Restart MYIOSDECK to run another.")
        default: break
        }
        // No JIT: Wine maps its DLLs from the signed dylibs and loads the stub x64 emulator.
        // With JIT, every program (this one too) takes the usual JIT-pool path.
        if program.noJIT && !mid_jit_pool_ready() {
            guard Self.noJITWineAvailable else {
                return refuse("This build has no signed Wine DLLs (no-JIT Wine).")
            }
            setenv("WINE_IOS_NOJIT", "1", 1)
            setenv("MYIOSDECK_PE_DIR", Self.peWineDir, 1)
            setenv("MYIOSDECK_NOJIT_EMULATOR", Self.peWineDir + "/xtajit64.dll", 1)
            setenv("MADEIRA_USE_ARM64EC", "1", 1)
            setenv("MYIOSDECK_NOJIT_TRACE", CrashReporter.nojitTraceURL.path, 1)
            dlog("[wine] no-JIT session: DLLs from \(Self.peWineDir)")
            // Settings › Without JIT › game audio off: no audio endpoints, so the game's audio
            // engine (decoding and mixing in the interpreter) does not run.
            if UserDefaults.standard.bool(forKey: "noJITMuteAudio") {
                setenv("WINEDLLOVERRIDES", "mmdevapi=d", 1)
                dlog("[wine] no-JIT session without audio (mmdevapi disabled)")
            } else {
                unsetenv("WINEDLLOVERRIDES")
            }
        } else {
            for k in ["WINE_IOS_NOJIT", "MYIOSDECK_PE_DIR", "MYIOSDECK_NOJIT_EMULATOR", "WINEDLLOVERRIDES"] { unsetenv(k) }
            if program.noJIT { setenv("MADEIRA_USE_ARM64EC", "1", 1) }
        }
        state = .booting(program.title)
        // Controller API for this session (Settings › Controller). Must be in the
        // environment before the wineserver starts: the HID device and the
        // DirectInput joystick are created at session start.
        let api = UserDefaults.standard.string(forKey: "controllerAPI") ?? "xinput"
        unsetenv("MADEIRA_DINPUT_PAD"); unsetenv("MADEIRA_HIDPAD"); unsetenv("MADEIRA_HIDPAD_NAME")
        if api == "dinput" { setenv("MADEIRA_DINPUT_PAD", "1", 1) }
        if api == "hid" || api == "dualsense" {
            setenv("MADEIRA_HIDPAD", api == "hid" ? "generic" : "dualsense", 1)
            if api == "hid", let name = GCController.controllers().first?.vendorName { setenv("MADEIRA_HIDPAD_NAME", name, 1) }
        }
        sessionAPI = api
        ControllerBridge.shared.setMode(Self.bridgeMode(api))
        dlog("[xinput] controller API for this session: \(api)")
        // DXMT takes the layer when the program creates its swapchain.
        if program.graphics {
            GameHostView.register()
            // The virtual monitor takes the phone's landscape shape, so a game that
            // runs at the desktop resolution fills the screen (win32u reads these
            // once per session; the default 1024x768 is letterboxed).
            let (w, h) = Self.monitorSize()
            setenv("MADEIRA_SCREEN_W", String(w), 1)
            setenv("MADEIRA_SCREEN_H", String(h), 1)
            dlog("[display] virtual monitor \(w)x\(h) for \(program.title)")
        }
        let prefix = prefixURL.path
        dlog("[wine] booting \(program.exe) in \(prefix)")
        queue.async {
            var err = [CChar](repeating: 0, count: 512)
            let ok = mid_wine_boot(prefix, program.exe, args, &err, err.count)
            let msg = String(cString: err)
            DispatchQueue.main.async {
                self.state = ok ? .running(program.title) : .failed(msg)
                if !ok { dlog("[wine] boot failed: \(msg)") }
            }
            if ok { self.watch(program.title) }
        }
    }

    /// 720 lines high, as wide as the screen's landscape aspect (a multiple of 8):
    /// 1560x720 on an iPhone 15 Pro Max. Light enough for the GPU at 60 fps.
    static func monitorSize() -> (Int, Int) {
        let b = UIScreen.main.nativeBounds
        let long = max(b.width, b.height), short = min(b.width, b.height)
        guard short > 0 else { return (1280, 720) }
        let h = 720
        let w = Int((CGFloat(h) * long / short / 8).rounded()) * 8
        return (w, h)
    }

    /// Poll until the Windows program exits, then report how it ended.
    private func watch(_ title: String) {
        let started = Date()
        while mid_wine_running() != 0 { usleep(250_000) }
        var status: UInt32 = 0
        let crashed = wine_crash_exit_status(&status) != 0
        let secs = String(format: "%.1f s", Date().timeIntervalSince(started))
        let how = crashed ? String(format: "ended with error 0x%08X after %@", status, secs) : "exited normally after \(secs)"
        dlog("[wine] \(title) \(how)")
        // Test programs write their report to C:\myiosdeck-output.txt: proof that does not
        // depend on where the console's output goes.
        let report = prefixURL.appendingPathComponent("drive_c/myiosdeck-output.txt")
        if let text = try? String(contentsOf: report, encoding: .utf8) {
            for line in text.split(whereSeparator: \.isNewline) { dlog("[program] \(line)") }
            try? FileManager.default.removeItem(at: report)
        }
        DispatchQueue.main.async {
            self.state = .finished(title, how)
            // After the game surface has closed (LibraryView closes it on an error).
            if crashed {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { CrashReporter.shared.programCrashed(title, how: how) }
            }
        }
    }
}
