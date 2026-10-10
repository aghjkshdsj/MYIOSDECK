#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""wow64win: a 32-bit value below 64 KB is an atom or a resource ID, never a guest pointer.

Win32 passes MAKEINTATOM / MAKEINTRESOURCE numbers where a string pointer may go (a window class
such as the dialog class #32770 = 0x8002, a menu or icon resource ID), and the callee tells them
apart with IS_INTRESOURCE(), i.e. value >> 16 == 0. Upstream's guest_ptr32() is the identity, so
the number survives every thunk. Madeira's adds the guest window's base B, so 0x8002 reaches
win32u as B + 0x8002: IS_INTRESOURCE() is false and the callee reads a string from the
never-mapped first 64 KB of the window (Steins;Gate's launcher: NtUserCreateWindowEx ->
wine_dbgstr_wn faults on B + 0x8002 creating its dialog, and the process dies). Madeira guards
three property calls (get_str_or_atom); every class name, window name, menu name and icon
resource that goes through unicode_str_32to64, WNDCLASSEXW or client_menu_name has the same
problem, in both directions (host_ptr32 turned a returned resource ID into ID - B).

Below 64 KB nothing is mapped in a Windows process, so the two converters keep such values as
they are, as upstream does: atoms and resource IDs work in every thunk, and a small invalid
pointer still faults (iOS maps nothing below 4 GB of a 64-bit process). wow64win.dll only runs
in 32-bit (WoW64) processes. Applies to Madeira's Wine fork. Usage: wow64win_atoms.py <wine tree>
"""
import sys

PRIVATE = "dlls/wow64win/wow64win_private.h"

FIXES = {
    PRIVATE: [
        ("static inline void *guest_ptr32( ULONG addr )\n"
         "{\n"
         "    return addr ? (void *)(wow64win_guest_base() + addr) : NULL;\n"
         "}\n",
         "static inline void *guest_ptr32( ULONG addr )\n"
         "{\n"
         "    /* MYIOSDECK: below 64 KB an atom or resource ID, never a pointer: kept as upstream keeps it */\n"
         "    if (!(addr >> 16)) return (void *)(ULONG_PTR)addr;\n"
         "    return (void *)(wow64win_guest_base() + addr);\n"
         "}\n", 1),
        ("static inline ULONG host_ptr32( const void *addr )\n"
         "{\n"
         "    return addr ? (ULONG)((ULONG_PTR)addr - wow64win_guest_base()) : 0;\n"
         "}\n",
         "static inline ULONG host_ptr32( const void *addr )\n"
         "{\n"
         "    /* MYIOSDECK: an atom or resource ID (below 64 KB) goes back unchanged */\n"
         "    if (!((ULONG_PTR)addr >> 16)) return (ULONG)(ULONG_PTR)addr;\n"
         "    return (ULONG)((ULONG_PTR)addr - wow64win_guest_base());\n"
         "}\n", 1),
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
                sys.exit("wow64win_atoms.py: %s: expected %d of %r, found %d" % (rel, count, old[:48], n))
            s = s.replace(old, new)
        open(path, "w").write(s)
        print("wow64win_atoms: patched %s" % rel)


if __name__ == "__main__":
    main()
