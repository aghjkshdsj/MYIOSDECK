#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Process-wide strings must outlive the Wine thread (putenv, setprogname).

putenv() keeps the caller's buffer as part of the process environment. Wine's unix side
passes it stack buffers (exec_wineloader) and a heap string that is freed later (winedebug):
harmless on Linux, where exec() replaces the process right after, but on iOS there is no
exec, so the environment ends up pointing into a dead thread's stack. iOS's own code scans
the environment later (build 52: the keyboard's dictation network check, ~10 s after a
Windows program exited) and crashes reading freed memory.

Give putenv a heap copy that is never freed instead. Applies to Madeira's
build/ntdll-unix/loader_ios.c and process_ios.c. Usage: putenv_lifetime.py <madeira checkout>
"""
import sys

FIXES = {
    "build/ntdll-unix/loader_ios.c": [
        ("    putenv( preloader_reserve );\n    putenv( socket_env );\n",
         "    putenv( strdup( preloader_reserve ) );   /* MYIOSDECK: no exec on iOS, buffer must outlive us */\n"
         "    putenv( strdup( socket_env ) );\n", 1),
    ],
    "build/ntdll-unix/process_ios.c": [
        ("if (winedebug) putenv( winedebug );",
         "if (winedebug) putenv( strdup( winedebug ) );   /* MYIOSDECK: winedebug is freed later */", 2),
    ],
    # setprogname() keeps the pointer too, and set_process_name hands it the exe path from a
    # buffer on the bridge's Wine thread stack. Once that thread exits, getprogname() dangles
    # and iOS frameworks that format the process name crash (build 53: the share sheet's
    # CTMessageCenter init; build 52: the keyboard's network-availability check).
    "build/ntdll-unix/env_ios.c": [
        ("    setprogname( name );\n",
         "    setprogname( strdup( name ) );   /* MYIOSDECK: name lives on a thread stack that ends */\n", 1),
    ],
}


def main():
    root = sys.argv[1]
    for rel, subs in FIXES.items():
        path = "%s/%s" % (root, rel)
        s = open(path).read()
        for old, new, count in subs:
            if new in s:
                continue
            n = s.count(old)
            if n != count:
                sys.exit("putenv_lifetime.py: %s: expected %d of %r, found %d" % (rel, count, old[:40], n))
            s = s.replace(old, new)
        open(path, "w").write(s)
        print("putenv_lifetime: patched %s" % rel)


if __name__ == "__main__":
    main()
