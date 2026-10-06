# Running without JIT

MYIOSDECK has two ways to execute x86-64 code:

| | FEX JIT | Interpreter (no JIT) |
|---|---|---|
| How | translates each block of x86 code to ARM64 once, then runs it natively | decodes and executes every x86 instruction, every time |
| Needs | StikDebug (a debugger must prepare executable memory) | nothing: no executable memory is created |
| Speed (iPhone 15 Pro Max) | ~80–100% of native ARM64 | measured by the Performance tab; expect a few percent of native |
| Engine | [FEX](https://github.com/FEX-Emu/FEX) (Madeira's iOS fork) | [Blink](https://github.com/jart/blink) by Justine Tunney (ISC), built with its JIT disabled |

When you start x86 code with JIT off, MYIOSDECK asks first:

> **You are running this without JIT** — Enable JIT / Continue anyway / Cancel

**Continue anyway** runs it in the interpreter. Results are written to the log as a
`MYIOSDECK BENCHMARK REPORT` block and saved to Files › On My iPhone › MYIOSDECK › benchmarks,
so you can share them. The interpreter runs 1/20 of each benchmark's workload and compares it with
native ARM64 running the same 1/20.

## What can run without JIT

The interpreter runs Linux x86-64 programs (like the built-in tests). Windows games need Wine, and
Wine's own ARM64 libraries are loaded from files at run time; iOS only lets that code execute
through the same debugger-prepared memory, so **Wine and Steam games require JIT**.

### Visual novels

Most visual novels are built on a handful of engines that already have native iOS or ARM ports, so
the fast way to play them without JIT is to run the *engine* natively and only load the game's data:

| Engine | Native route |
|---|---|
| Ren'Py | Ren'Py builds for iOS; the game's `.rpy`/`.rpa` files run on the native runtime |
| KiriKiri / KAG (krkr2, krkrz) | open-source ports (krkrsdl2) run on ARM |
| NScripter | ONScripter (open source) runs on iOS |
| TyranoScript | HTML5: runs in WKWebView |

This is a possible future "VN mode": pick a game folder, detect the engine, run it natively at full
speed with no JIT at all, which is also the only route that could ever be App Store–compatible.
