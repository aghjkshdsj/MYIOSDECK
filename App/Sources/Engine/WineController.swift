// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import GameController
import UIKit
import os

/// Stage 2: runs Windows x86-64 programs through Wine ARM64EC + FEX (xtajit64)
/// in-process, using a slice of the JIT pool. One Wine session per app run; with session reuse
/// (Settings, docs/SESSION_REUSE.md) later games start in that session.
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
        /// Runs on the aarch64 DLL farm: a plain ARM64 program, or a 32-bit one (WoW64, whose
        /// 64-bit half is aarch64; docs/NO_JIT_WOW64.md).
        var aarch64 = false
    }

    /// The signed Wine dylibs (engine/pedylib), present when CI converted the DLL farm.
    static let peWineDir = Bundle.main.bundlePath + "/PE/wine"
    static var noJITWineAvailable: Bool {
        FileManager.default.fileExists(atPath: peWineDir + "/libntdll.dll.dylib")
    }
    /// The aarch64 farm as signed dylibs: Wine maps aarch64 images from here (nojit_dylib.py).
    static let peWineA64Dir = Bundle.main.bundlePath + "/PE/wine-a64"
    /// 32-bit programs without JIT (docs/NO_JIT_WOW64.md): the i386 farm, the aarch64 farm's
    /// dylibs and the WoW64 CPU module (FXI32's front end) are all in this build.
    static var wow64NoJITAvailable: Bool {
        let fm = FileManager.default
        return fm.fileExists(atPath: Bundle.main.bundlePath + "/i386-windows/ntdll.dll") &&
            fm.fileExists(atPath: peWineA64Dir + "/libntdll.dll.dylib") &&
            fm.fileExists(atPath: peWineA64Dir + "/libxtajit.dll.dylib") &&
            fm.fileExists(atPath: peWineA64Dir + "/xtajit.dll")
    }
    static let wow64Refusal = "This build cannot run 32-bit games without JIT (its WoW64 files are missing): enable JIT to play it."

    /// Madeira's test programs, shipped in the ARM64EC DLL farm.
    static let programs = [
        Program(id: "hello-arm64ec.exe", title: "Windows Hello (ARM64EC, no JIT)",
                detail: "Wine from signed dylibs, no JIT needed: native Windows ARM code only", noJIT: true),
        Program(id: "hello-aarch64.exe", title: "Windows Hello (ARM64, no JIT)",
                detail: "An aarch64 Wine session from signed dylibs: the 64-bit half of 32-bit games", noJIT: true,
                aarch64: true),
        Program(id: "hello-i386.exe", title: "Windows Hello (x86 32-bit, no JIT)",
                detail: "A 32-bit program through WoW64: its x86 code in FXI32, Wine's 64-bit half native", noJIT: true,
                aarch64: true),
        Program(id: "suite-x86.exe", title: "x86 test suite (32-bit, no JIT)",
                detail: "FXI32 in WoW64: window callbacks, threads, exceptions, C++ throw, longjmp, child process",
                noJIT: true, aarch64: true),
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
        Program(id: "hello-x64.exe", title: "Windows Hello (x64)", detail: "Console hello world through Wine + FEX ARM64EC"),
        Program(id: "fib-x64.exe", title: "Fibonacci (x64)", detail: "Recursive CPU test: x86-64 call/ret through FEX"),
        Program(id: "clocktest-x64.exe", title: "Clock test (x64)", detail: "Windows timers and QueryPerformanceCounter"),
        Program(id: "cube-x64.exe", title: "Direct3D 11 cube (x64)", detail: "Spinning cube: D3D11 through DXMT to Metal", graphics: true),
        Program(id: "d3d12-cube-x64.exe", title: "Direct3D 12 cube (x64)", detail: "Spinning cube: D3D12 through Madeira's converter to Metal", graphics: true),
    ]

    @Published private(set) var state: State = .idle {
        // A game (or the long Steam sign-in without JIT) gets no touches: keep iOS from
        // locking the screen and suspending the app while Wine runs.
        didSet {
            switch state {
            case .booting, .running: ScreenAwake.set("wine", true)
            default: ScreenAwake.set("wine", false)
            }
            // A Steam game's end, however it ended (its own quit, Quit game, Force quit, a crash,
            // a start that failed): its changed saves go up to Steam Cloud (SteamCloudSync).
            switch state {
            case .finished, .failed: Task { @MainActor in SteamCloudSync.shared.gameEnded() }
            default: break
            }
        }
    }
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

    /// The program this session started (its controller choice is remembered per program).
    private(set) var sessionProgramID = ""
    /// The library game this session started ("steam.<App ID>" / "custom.<UUID>"; "" for a test
    /// program): its controller and touch choices, and its Game settings, are kept under it.
    private(set) var sessionGameKey = ""
    /// The next start's game settings (GameSettingsStore.prepareLaunch), used once by run().
    var pendingOverrides: LaunchOverrides?
    /// The running (or last) game's settings, nil for a test program.
    @Published private(set) var sessionOverrides: LaunchOverrides?

    /// Modes whose device exists only if the session started with it: the HID gamepads, and
    /// the DirectInput joystick (MADEIRA_DINPUT_PAD, read at session start).
    static let sessionStartAPIs: Set<String> = ["hid", "dualsense", "dinput"]

    /// The controller API a program starts with: its own last choice (games differ: a
    /// GameMaker game wants XInput, Steins;Gate DirectInput), else Settings › Controller.
    static func controllerAPI(for programID: String, gameKey: String? = nil) -> String {
        if let gameKey, !gameKey.isEmpty, let own = UserDefaults.standard.string(forKey: "controllerAPI.\(gameKey)") { return own }
        return UserDefaults.standard.string(forKey: "controllerAPI.\(programID)")
            ?? UserDefaults.standard.string(forKey: "controllerAPI") ?? "xinput"
    }

    /// The running session's controller API as the game menu shows it.
    var currentControllerAPI: String { Self.controllerAPI(for: sessionProgramID, gameKey: sessionGameKey) }

    /// In-game switch (the game menu). Saved as this program's own choice for its next launch
    /// (Settings › Controller stays the default of every other game: Steins;Gate's DirectInput
    /// started Crab Game with it); applied now unless the mode needs a device this session was
    /// not started with. Returns whether it took effect now.
    @discardableResult
    func switchController(to api: String) -> Bool {
        if !sessionGameKey.isEmpty { UserDefaults.standard.set(api, forKey: "controllerAPI.\(sessionGameKey)") }
        else if sessionProgramID.isEmpty { UserDefaults.standard.set(api, forKey: "controllerAPI") }
        else { UserDefaults.standard.set(api, forKey: "controllerAPI.\(sessionProgramID)") }
        let needsStart = Self.sessionStartAPIs.contains(api) && api != sessionAPI
        dlog("[xinput] in-game switch to \(api)\(needsStart ? " (applies at next launch)" : "")")
        guard !needsStart else { return false }
        ControllerBridge.shared.setMode(Self.bridgeMode(api))
        return true
    }
    let linked = mid_wine_linked()

    // ---- Session reuse (docs/SESSION_REUSE.md) ----

    /// Settings › Without JIT › Play another game without restarting (experimental): when a game
    /// ends, its Wine session stays, and the next game starts in it as a new Windows process.
    static var reuseEnabled: Bool { UserDefaults.standard.bool(forKey: "sessionReuse") }

    /// What Wine sets up once, for the session's first program: a later game can start in the
    /// session only when it needs the same.
    struct SessionKind: Equatable {
        var noJIT: Bool       // Wine's DLLs from the signed dylibs (WINE_IOS_NOJIT)
        var aarch64: Bool     // the aarch64 farm (32-bit games through WoW64), else ARM64EC (64-bit)
        var hidPad: String    // the HID gamepad the wineserver makes when it starts ("" for none)
        var fxr: Bool         // FXR as the x64 CPU of a 64-bit game (Settings › Windows games: FXR)
    }
    /// The x64 CPU of this app run's 64-bit games (true FXR, false FXI), fixed by the first one:
    /// the CPU is one global of the Wine session (fxi_win_host.c), and threads an ended game left
    /// running still use the CPU they were made for. nil before the first 64-bit game.
    private(set) var fxrThisRun: Bool?
    /// The running session's kind (nil before the first program).
    private(set) var sessionKind: SessionKind?
    /// Windows programs started in this app run.
    private(set) var programsThisRun = 0
    /// The running (or last) session stays when its game ends: Quit game can then leave
    /// MYIOSDECK open.
    @Published private(set) var sessionKept = false
    /// Force quit was asked for the running game (its end is reported as that, not as a crash).
    private let forceQuitAsked = OSAllocatedUnfairLock(initialState: false)
    /// No limit on the number of games: an ended 32-bit game's window comes back when the next
    /// one starts (docs/SESSION_REUSE.md). A 64-bit game's memory still stays in use until
    /// MYIOSDECK closes, so the next game needs this much free (iOS ends an app that runs out).
    static let minFreeMB = 1536

    /// Force quit (a kept session's game that does not close after Quit game): the Wine server
    /// terminates the session's programs as TerminateProcess does, and the session stays for the
    /// next game (docs/SESSION_REUSE.md). False when there is nothing to end.
    func forceQuit() -> Bool {
        guard sessionKept, mid_wine_running() != 0 else { return false }
        forceQuitAsked.withLock { $0 = true }
        return mid_wine_force_quit() != 0
    }

    static func hidPad(_ api: String) -> String {
        api == "hid" ? "generic" : api == "dualsense" ? "dualsense" : ""
    }

    /// Why another game cannot start now (nil: it can, as far as is known without the game;
    /// `kind` adds the game's own checks).
    func nextGameRefusal(_ kind: SessionKind? = nil) -> String? {
        let restart = "To play another, swipe MYIOSDECK away in the app switcher and open it again."
        guard Self.reuseEnabled else {
            return "MYIOSDECK runs one Windows game per launch. \(restart) (Settings › Without JIT › Play another game without restarting lets the next game start without that.)"
        }
        guard let session = sessionKind, session.noJIT, sessionKept else {
            return "This Wine session was not kept for another game (it started with JIT, or before Play another game without restarting was on). \(restart)"
        }
        guard mid_wine_session_reusable() != 0 else {
            return "The last game's Wine session is no longer running. \(restart)"
        }
        if let kind {
            if !kind.noJIT {
                return "Another game can start in the running Wine session only without JIT. \(restart)"
            }
            // A 64-bit game after a 32-bit one, or the reverse, runs as Madeira's cross-architecture
            // processes do (the bridge keeps the session's system32; docs/SESSION_REUSE.md).
            if kind.hidPad != session.hidPad {
                return "This game's controller mode (\(kind.hidPad.isEmpty ? "not a HID gamepad" : "HID gamepad")) differs from the running Wine session's, which Wine sets up when it starts. Choose the same mode for it, or: \(restart)"
            }
        }
        // The x64 CPU is not a reason to refuse: every 64-bit game of a run takes the first one's (run()).
        let freeMB = os_proc_available_memory() >> 20
        if freeMB < Self.minFreeMB {
            return "Only \(freeMB) MB of memory is free: an earlier game's memory stays in use until MYIOSDECK closes. \(restart)"
        }
        return nil
    }

    /// What kind of start run() is doing (set by the run* helpers just before it).
    private enum Launch { case program, steam, custom, dock }
    private var nextLaunch = Launch.program

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
        var is32 = false
        if noJIT {
            let machine = peMachine(windowsPath: plan.exe)
            is32 = machine == 0x14c
            dlog("[wine] no-JIT Steam launch: \(plan.exe) machine=0x\(String(machine, radix: 16))\(is32 ? " (32-bit: WoW64 with FXI32, experimental)" : "")")
            if is32 && !Self.wow64NoJITAvailable {
                state = .failed(Self.wow64Refusal)
                return
            }
        }
        setenv("MADEIRA_STEAM_APPID", String(appID), 1)
        setenv("MADEIRA_STEAM_APPPATH", plan.appPath, 1)
        setenv("MADEIRA_WORKDIR", plan.workingFolder, 1)
        nextLaunch = .steam
        run(Program(id: plan.exe, title: title, detail: "", graphics: true, noJIT: noJIT, aarch64: is32), args: plan.arguments)
    }

    /// A game the user added (CustomGames: a DRM-free folder in Documents/Games, C:\Games in
    /// Wine), started in its program's folder. Not a Steam game: like a Dock session, the bridge
    /// publishes no Steam identity for it (MADEIRA_DOCK_SESSION), so no game sees Madeira's
    /// fixed fallback App ID.
    func runCustomGame(_ game: CustomGame) {
        let noJIT = !mid_jit_pool_ready()
        CustomGames.prepare(prefix: prefixURL)                       // C:\Games -> Documents/Games
        let machine = peMachine(windowsPath: game.windowsExe)
        let is32 = noJIT && machine == 0x14c
        dlog("[games] start \(game.name): \(game.windowsExe) machine=0x\(String(machine, radix: 16)) noJIT=\(noJIT ? 1 : 0)\(is32 ? " (32-bit: WoW64 with FXI32, experimental)" : "")")
        if is32 && !Self.wow64NoJITAvailable {
            state = .failed(Self.wow64Refusal)
            return
        }
        for k in ["MADEIRA_STEAM_APPID", "MADEIRA_STEAM_APPPATH"] { unsetenv(k) }
        setenv("MADEIRA_WORKDIR", game.windowsWorkdir, 1)
        nextLaunch = .custom
        run(Program(id: game.windowsExe, title: game.name, detail: "", graphics: true, noJIT: noJIT, aarch64: is32), args: game.arguments)
    }

    /// Set while an added game runs (no Steam identity for it).
    private(set) var customActive = false

    /// A Steam game started through Madeira Dock (App/Sources/Steam/MadeiraDock.swift): Wine runs
    /// dockhost.exe, which loads Valve's own steamclient64.dll, signs in with the user's token
    /// (the one-use transfer the caller wrote), asks Valve's client whether the account owns the
    /// game and has it start the game with Valve's LaunchApp. The game then finds a running,
    /// signed-in Steam client, as on a PC. Without JIT the host, Valve's client and the game are
    /// x64 code in FXI; the game is the host's child process.
    func runDockGame(_ game: DockGame, launchOption: UInt32, title: String) {
        let noJIT = !mid_jit_pool_ready()
        for k in ["MADEIRA_STEAM_APPID", "MADEIRA_STEAM_APPPATH", "MADEIRA_WORKDIR"] { unsetenv(k) }   // no direct start
        MadeiraDock.configure(game, launchOption: launchOption)
        // Without JIT Valve's client runs interpreted: in build 105 it was still in its HTTPS
        // setup (OpenSSL in CHTTPClientThreadPool) when the host's 90 s sign-in wait ran out.
        // Our dockhost (engine/pedylib/madeira-dock-signin-wait.patch) reads a longer wait here.
        if noJIT { setenv("MADEIRA_DOCK_SIGNIN_MS", "480000", 1) } else { unsetenv("MADEIRA_DOCK_SIGNIN_MS") }
        // Without JIT Valve's client never finished a TLS handshake in time (build 178: its CM
        // pings over WebSocket have 1 s each and failed at once, the HTTPS CM directory timed out
        // after 27 s and its second try never returned, with the client's HTTP thread in OpenSSL's
        // bignum code throughout). Its other transport to Steam, UDP/TCP with Valve's own channel
        // encryption, needs no TLS: the host has the client prefer it
        // (engine/pedylib/madeira-dock-cm-transport.patch). With JIT the client decides as usual.
        if noJIT { setenv("MADEIRA_DOCK_CM_TRANSPORT", "netfilter", 1) } else { unsetenv("MADEIRA_DOCK_CM_TRANSPORT") }
        // Build 181: the client still asks its HTTPS CM directory first and waited on it for over
        // 4 minutes. Its address then resolves to nothing at once (engine/wine/patches/dns_refuse.py),
        // so the request fails and the client takes the UDP/TCP servers MYIOSDECK wrote into its
        // config.vdf (DockCMCache). Cleared for every other launch (run()).
        if noJIT { setenv("MYIOSDECK_DNS_REFUSE", "api.steampowered.com", 1) } else { unsetenv("MYIOSDECK_DNS_REFUSE") }
        dlog("[dock-launch] app=\(game.id) launch-option=\(launchOption) noJIT=\(noJIT ? 1 : 0) signin-wait=\(noJIT ? 480 : 90)s cm-transport=\(noJIT ? "netfilter" : "client's choice")")
        nextLaunch = .dock
        run(Program(id: MadeiraDock.executable, title: title, detail: "", graphics: true, noJIT: noJIT))
    }

    /// Set while a Dock game runs: watch() polls the host's report (C:\madeira-dock.txt).
    private(set) var dockActive = false
    /// The host's failure, in words, when its report ended with a nonzero result.
    @Published private(set) var dockFailure: String?

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
        let launch = nextLaunch
        nextLaunch = .program
        let overrides = pendingOverrides
        pendingOverrides = nil
        // Per-launch variables (runSteamGame) must not outlive a launch that never boots.
        func refuse(_ why: String) {
            for k in ["MADEIRA_STEAM_APPID", "MADEIRA_STEAM_APPPATH", "MADEIRA_WORKDIR"] { unsetenv(k) }
            state = .failed(why)
        }
        guard linked else { return refuse("This build does not include Wine yet.") }
        switch state {
        case .booting, .running:
            // A game is still running, maybe with its view hidden: its state stays as it is (a
            // .failed here closed its view and hid the Return bar). Play says why (LibraryView).
            dlog("[wine] start of \(program.title) refused: a program is still running")
            return
        default: break
        }
        // Madeira's wineserver cannot start twice in one process (init_registry aborts on
        // "\Registry"), so a later game either joins the running session (session reuse,
        // docs/SESSION_REUSE.md) or needs a restart (nextGameRefusal says which, and why).
        let api = Self.controllerAPI(for: program.id, gameKey: overrides?.gameKey)
        let noJITSession = program.noJIT && !mid_jit_pool_ready()
        // The x64 CPU (Settings › Windows games: FXR). FXI and FXR both drop the blocks of an
        // earlier game before a later one starts (wine_host.c), so every 64-bit game of a run may
        // use either; but all of them use the first one's (fxrThisRun). 32-bit games never use it (FXI32).
        let wantFXR = noJITSession && !program.aarch64 && (overrides?.fxr ?? UserDefaults.standard.bool(forKey: "winFXR"))
        let useFXR = noJITSession && !program.aarch64 && (fxrThisRun ?? wantFXR)
        let kind = SessionKind(noJIT: noJITSession, aarch64: program.aarch64, hidPad: Self.hidPad(api), fxr: useFXR)
        if programsThisRun > 0, let why = nextGameRefusal(kind) {
            dlog("[session-reuse] \(program.title) not started: \(why)")
            return refuse(why)
        }
        dockActive = launch == .dock
        if launch != .dock { unsetenv("MYIOSDECK_DNS_REFUSE") }   // only a Dock session without JIT (runDockGame)
        customActive = launch == .custom
        dockFailure = nil
        forceQuitAsked.withLock { $0 = false }
        // The bridge keeps the wineserver when this program ends (WineProcessBridge.m as patched by
        // engine/wine/patches/session_reuse.py), so the next game can start in the session.
        let keep = noJITSession && Self.reuseEnabled
        if keep { setenv("MYIOSDECK_SESSION_REUSE", "1", 1) } else { unsetenv("MYIOSDECK_SESSION_REUSE") }
        // No JIT: Wine maps its DLLs from the signed dylibs and loads the stub x64 emulator.
        // With JIT, every program (this one too) takes the usual JIT-pool path.
        if program.noJIT && !mid_jit_pool_ready() {
            guard Self.noJITWineAvailable else {
                return refuse("This build has no signed Wine DLLs (no-JIT Wine).")
            }
            if program.aarch64 && !FileManager.default.fileExists(atPath: Self.peWineA64Dir + "/libntdll.dll.dylib") {
                return refuse("This build has no signed aarch64 Wine DLLs.")
            }
            setenv("WINE_IOS_NOJIT", "1", 1)
            setenv("MYIOSDECK_PE_DIR", Self.peWineDir, 1)
            setenv("MYIOSDECK_PE_DIR_A64", Self.peWineA64Dir, 1)
            setenv("MYIOSDECK_NOJIT_EMULATOR", Self.peWineDir + "/xtajit64.dll", 1)
            // 32-bit programs: wow64.dll's CPU module is FXI32's front end (system32\xtajit.dll).
            if FileManager.default.fileExists(atPath: Self.peWineA64Dir + "/xtajit.dll") {
                setenv("MYIOSDECK_NOJIT_WOW_CPU", Self.peWineA64Dir + "/xtajit.dll", 1)
            } else {
                unsetenv("MYIOSDECK_NOJIT_WOW_CPU")
            }
            // An aarch64 program gets Madeira's plain aarch64 session (its exe name picks it).
            if program.aarch64 { unsetenv("MADEIRA_USE_ARM64EC") } else { setenv("MADEIRA_USE_ARM64EC", "1", 1) }
            setenv("MYIOSDECK_NOJIT_TRACE", CrashReporter.nojitTraceURL.path, 1)
            dlog("[wine] no-JIT session: DLLs from \(Self.peWineDir)\(program.aarch64 ? " (aarch64: \(Self.peWineA64Dir))" : "")")
            // Settings › Without JIT › game audio off: no audio endpoints, so the game's audio
            // engine (decoding and mixing in the interpreter) does not run.
            // Game settings › Audio overrides it per game.
            if !(overrides?.audio ?? !UserDefaults.standard.bool(forKey: "noJITMuteAudio")) {
                setenv("WINEDLLOVERRIDES", "mmdevapi=d", 1)
                dlog("[wine] no-JIT session without audio (mmdevapi disabled)")
            } else {
                unsetenv("WINEDLLOVERRIDES")
            }
            // Settings › Without JIT › Windows games: FXR (fxi_win_host.c picks the x64 CPU). A 32-bit
            // game keeps the run's x64 CPU too: x64 code may still run in the session.
            if fxrThisRun ?? kind.fxr {
                setenv("MYIOSDECK_WIN_CPU", "fxr", 1)
                if !program.aarch64 { dlog("[wine] no-JIT session with FXR as the x64 CPU") }
            } else {
                unsetenv("MYIOSDECK_WIN_CPU")
            }
            if !program.aarch64, let first = fxrThisRun, first != wantFXR {
                dlog("[session-reuse] \(program.title) runs with \(first ? "FXR" : "FXI") like the first 64-bit game of this app run (Settings › Windows games: FXR changed since; it applies after MYIOSDECK restarts)")
            }
        } else {
            for k in ["WINE_IOS_NOJIT", "MYIOSDECK_PE_DIR", "MYIOSDECK_PE_DIR_A64", "MYIOSDECK_NOJIT_EMULATOR",
                      "MYIOSDECK_NOJIT_WOW_CPU", "WINEDLLOVERRIDES", "MYIOSDECK_WIN_CPU"] { unsetenv(k) }
            if program.aarch64 { unsetenv("MADEIRA_USE_ARM64EC") } else if program.noJIT { setenv("MADEIRA_USE_ARM64EC", "1", 1) }
        }
        // A 32-bit game's launcher starts the game and exits (Steins;Gate: Launcher.exe ->
        // Game.exe): the session waits for the children it started instead of ending with the
        // launcher and killing the game (Madeira's MADEIRA_WAIT_CHILDREN; opt-in there because a
        // child that ends from a worker thread can keep the session waiting, which the game menu's
        // Quit ends). 64-bit sessions keep Madeira's default.
        if program.aarch64 { setenv("MADEIRA_WAIT_CHILDREN", "1", 1) } else { unsetenv("MADEIRA_WAIT_CHILDREN") }
        // A Dock session publishes no fixed Steam game identity: Madeira's bridge otherwise sets
        // SteamAppId/SteamGameId/SteamAppPath (its Thumper fallback) for every launch, and in
        // build 105 that reached Valve's client inside the host, which then never finished
        // signing in (result 34). Valve's client gives the game it starts its own identity.
        // An added (non-Steam) game gets none either.
        if dockActive || customActive { setenv("MADEIRA_DOCK_SESSION", "1", 1) } else { unsetenv("MADEIRA_DOCK_SESSION") }
        state = .booting(program.title)
        // Controller API for this session (Settings › Controller, `api` above). Must be in the
        // environment before the wineserver starts: the HID device and the
        // DirectInput joystick are created at session start.
        sessionProgramID = program.id
        sessionGameKey = overrides?.gameKey ?? ""
        sessionOverrides = overrides
        let touchProgram = program.id, touchGame = overrides?.gameKey
        Task { @MainActor in GameInput.shared.load(for: touchProgram, gameKey: touchGame) }   // its touch mode: off unless chosen
        // Game settings › Frame rate: DXMT paces a Direct3D 10/11 game's presents to it.
        if let line = overrides?.dxmtConfig {
            setenv("DXMT_CONFIG", line, 1)
            dlog("[game-settings] DXMT_CONFIG=\(line)")
        } else {
            unsetenv("DXMT_CONFIG")
        }
        // Game settings › Show performance overlay: Metal's HUD on the game's layer.
        let hud = overrides?.performanceHUD ?? false
        Task { @MainActor in GameHostView.shared.setPerformanceHUD(hud) }
        unsetenv("MADEIRA_DINPUT_PAD"); unsetenv("MADEIRA_HIDPAD"); unsetenv("MADEIRA_HIDPAD_NAME")
        // DirectInput view of the pad; its trace (Madeira's joystick_ios.c, rate-limited: what the
        // game reads once it has acquired the pad) shows in the log whether it ever does.
        unsetenv("MADEIRA_DINPUT_TRACE")
        if api == "dinput" { setenv("MADEIRA_DINPUT_PAD", "1", 1); setenv("MADEIRA_DINPUT_TRACE", "1", 1) }
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
            if programsThisRun > 0 { GameHostView.shared.clearPicture() }   // not the last game's frame
            // The virtual monitor takes the phone's landscape shape, so a game that
            // runs at the desktop resolution fills the screen (win32u reads these
            // once per session; the default 1024x768 is letterboxed).
            let (w, h) = Self.monitorSize(height: overrides?.resolution ?? UserDefaults.standard.integer(forKey: "monitorHeight"))
            if programsThisRun > 0, let first = Self.firstMonitorHeight, h != first {
                dlog("[display] \(program.title) asks for a \(h)-line monitor; Wine keeps the session's first (\(first) lines) until MYIOSDECK restarts")
            }
            if programsThisRun == 0 { Self.firstMonitorHeight = h }
            setenv("MADEIRA_SCREEN_W", String(w), 1)
            setenv("MADEIRA_SCREEN_H", String(h), 1)
            dlog("[display] virtual monitor \(w)x\(h) for \(program.title)")
        }
        let prefix = prefixURL.path
        dlog("[wine] booting \(program.exe) in \(prefix)\(programsThisRun > 0 ? " (in the running session, program \(programsThisRun + 1) of this app run)" : "")")
        queue.async {
            var err = [CChar](repeating: 0, count: 512)
            let ok = mid_wine_boot(prefix, program.exe, args, &err, err.count)
            let msg = String(cString: err)
            DispatchQueue.main.async {
                self.state = ok ? .running(program.title) : .failed(msg)
                if ok {
                    if self.programsThisRun == 0 { self.sessionKind = kind }
                    if kind.noJIT && !kind.aarch64 && self.fxrThisRun == nil { self.fxrThisRun = kind.fxr }
                    self.programsThisRun += 1
                    self.sessionKept = keep
                } else {
                    dlog("[wine] boot failed: \(msg)")
                }
            }
            if ok { self.watch(program.title) }
        }
    }

    /// The first game's monitor height: win32u reads the size once per Wine session.
    private static var firstMonitorHeight: Int?

    /// `height` lines high (720 by default, light enough for the GPU at 60 fps; Settings ›
    /// Performance and Game settings › Display change it), as wide as the screen's landscape
    /// aspect (a multiple of 8): 1560x720 on an iPhone 15 Pro Max.
    static func monitorSize(height: Int = 720) -> (Int, Int) {
        let b = UIScreen.main.nativeBounds
        let long = max(b.width, b.height), short = min(b.width, b.height)
        let h = LaunchOverrides.validResolution(height)
        guard short > 0 else { return (h * 16 / 9 / 8 * 8, h) }
        let w = Int((CGFloat(h) * long / short / 8).rounded()) * 8
        return (w, h)
    }

    /// Poll until the Windows program exits, then report how it ended.
    private func watch(_ title: String) {
        let started = Date()
        var ticks = 0, clientLogsDumped = false
        while mid_wine_running() != 0 {
            usleep(250_000)
            ticks += 1
            // Madeira Dock: log the host's report fields as they change ([dock-report]), and
            // show its progress over the (still black) game surface.
            if dockActive, ticks % 4 == 0 {
                let seconds = Int(Date().timeIntervalSince(started))
                // Valve's client logs once about 4 min in too, in case the session is cut short.
                let dumpLogs = !clientLogsDumped && seconds >= 240
                if dumpLogs { clientLogsDumped = true }
                Task { @MainActor in
                    let report = MadeiraDock.pollReport()
                    GameHostView.shared.setStatus(MadeiraDock.progressText(report, seconds: seconds))
                    if dumpLogs { MadeiraDock.logClientLogs() }
                }
            }
        }
        if dockActive { Task { @MainActor in GameHostView.shared.setStatus(nil) } }
        if dockActive {
            let done = DispatchSemaphore(value: 0)
            Task { @MainActor in
                let report = MadeiraDock.pollReport()
                self.dockFailure = report.failure
                if let failure = report.failure { dlog("[dock-launch] result=\(report.result ?? -1): \(failure)") }
                MadeiraDock.logClientLogs()
                MadeiraDock.cleanup()
                unsetenv("MYIOSDECK_DNS_REFUSE")   // the Dock session ended: names resolve again
                done.signal()
            }
            done.wait()
        }
        var status: UInt32 = 0
        let forced = forceQuitAsked.withLock { $0 }
        let crashed = !forced && wine_crash_exit_status(&status) != 0
        let secs = String(format: "%.1f s", Date().timeIntervalSince(started))
        let how = forced ? "was force-quit after \(secs)"
            : crashed ? String(format: "ended with error 0x%08X after %@", status, secs) : "exited normally after \(secs)"
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

/// Keeps the screen on while anything needs it (a running game, a Steam download): one
/// switch for the whole app, so one holder ending does not turn it off for another.
enum ScreenAwake {
    @MainActor private static var holders = Set<String>()
    /// Callable from any thread; applied on the main actor in call order.
    static func set(_ holder: String, _ on: Bool) {
        Task { @MainActor in
            if on { holders.insert(holder) } else { holders.remove(holder) }
            UIApplication.shared.isIdleTimerDisabled = !holders.isEmpty
        }
    }
}
