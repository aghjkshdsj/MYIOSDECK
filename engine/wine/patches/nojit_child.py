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

Usage: nojit_child.py <madeira checkout>. Idempotent (marker line).
"""
import sys

MARK = "/* MYIOSDECK nojit-child */"


def main():
    path = sys.argv[1] + "/build/ntdll-unix/loader_ios.c"
    s = open(path).read()
    if MARK in s:
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
