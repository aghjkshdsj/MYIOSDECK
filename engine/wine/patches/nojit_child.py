#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No-JIT step 4 (docs/NO_JIT_WINDOWS.md): child pseudo-processes get their own ntdll.

With JIT, Madeira gives a child a private copy of ntdll at the SAME addresses, redirected per
process through the JIT pool's owner-aware translation (ios_jit_copy_module_for_child). Without
JIT there is no pool: the copy fails and the child falls back to the parent's ntdll, whose
module list and loader lock then collide (build 64: the child crashed in ntdll's loader and
the parent hung at exit).

Madeira already has a second path: ios_load_child_ec_ntdll loads a private ntdll image through
the normal map pipeline and registers it for the child's PEB (ios_set_proc_ntdll), for
cross-architecture children. Without JIT, every child takes that path; the no-JIT map hook
(nojit_dylib.py) answers the second mapping of ntdll.dll (and of every other DLL the child
loads) with a new instance of the signed dylib (App/Sources/Native/pe_instance.c).

That path loads the ARM64EC ntdll, the 64-bit half of an x64 child. A 32-bit (WoW64) child's
64-bit half is the session's aarch64 ntdll, as the main process's (Steins;Gate's launcher
starting Game.exe: "FAILED to load .../x86_64-windows/ntdll.dll", the child never ran), so for an
i386 child the function loads that one instead (the map hook answers it with a new instance of
the aarch64 dylib, PE/wine-a64).

Usage: nojit_child.py <madeira checkout>. Idempotent (marker line).
"""
import sys

MARK = "/* MYIOSDECK nojit-child */"
WOW_MARK = "/* MYIOSDECK nojit-child wow64 */"
WOW_FIXES = [
    ("    const char *pe_dir = get_pe_dir( IMAGE_FILE_MACHINE_AMD64 );  /* owner-aware → /arm64ec-windows */\n",
     "    " + WOW_MARK + "\n"
     "    WORD nt_machine = is_machine_64bit( ios_cur_image_info()->Machine ) ? IMAGE_FILE_MACHINE_AMD64\n"
     "                                                                         : IMAGE_FILE_MACHINE_ARM64;\n"
     "    const char *pe_dir = get_pe_dir( nt_machine );  /* AMD64: arm64ec-windows; i386 child: aarch64-windows */\n"),
    ("    status = open_builtin_pe_file( name, &attr, &module, &size, &info, 0, 0,\n"
     "                                   IMAGE_FILE_MACHINE_AMD64, FALSE, 0 );\n",
     "    status = open_builtin_pe_file( name, &attr, &module, &size, &info, 0, 0,\n"
     "                                   nt_machine, FALSE, 0 );\n"),
]


def wow_child(s):
    if WOW_MARK in s:
        return s
    for old, new in WOW_FIXES:
        if s.count(old) != 1:
            sys.exit("nojit_child.py: wow64 anchor found %d times: %r" % (s.count(old), old[:60]))
        s = s.replace(old, new)
    print("nojit_child: a 32-bit child loads the aarch64 ntdll")
    return s


def main():
    path = sys.argv[1] + "/build/ntdll-unix/loader_ios.c"
    s = open(path).read()
    s = wow_child(s)
    if MARK in s:
        open(path, "w").write(s)
        print("nojit_child: already applied")
        return
    old = "        if (ios_cur_image_info()->Machine == IMAGE_FILE_MACHINE_AMD64 && !is_arm64ec())\n"
    new = ("        " + MARK + "\n"
           "        if ((ios_cur_image_info()->Machine == IMAGE_FILE_MACHINE_AMD64 && !is_arm64ec()) ||\n"
           "            (getenv( \"WINE_IOS_NOJIT\" ) && *getenv( \"WINE_IOS_NOJIT\" ) == '1'))\n")
    if s.count(old) != 1:
        sys.exit("nojit_child.py: anchor found %d times" % s.count(old))
    s = s.replace(old, new)
    open(path, "w").write(s)
    print("nojit_child: applied to %s" % path)


if __name__ == "__main__":
    main()
