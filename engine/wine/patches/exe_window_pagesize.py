#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A fixed-base main image keeps its address across games: compare the claim in host pages.

Madeira's virtual_ios.c gives a 64-bit program whose relocations are stripped (it can only run at
its ImageBase, e.g. Dokimon.exe at 0x140000000) the executable window, and hands that address on
when the program exits so the next copy can run there (ml988: ios_exe_win_claim records the
pending interval, map_image_view confirms it with ios_exe_win_commit_claim, the exiting process
retires it in ios_retire_own_fixed_base_image). The claim is recorded inside anon_mmap_tryfixed
with the size map_fixed_area rounded up to the host page (16 KB on iOS); the confirmation gets
the image view's own size, a multiple of 4 KB. For an image whose size is not a multiple of 16 KB
they differ, the confirmation is taken for someone else's and does nothing: the image is never
OWNED, its exit retires nothing, and the same program started again in a kept session (session
reuse, docs/SESSION_REUSE.md) cannot map at its address and fails with STATUS_CONFLICTING_ADDRESSES
(build 183: Dokimon through Madeira Dock, second start, c0000018).

The confirmation now rounds the size to the host page, as the claim did, so the held and retired
interval is the one the next claim asks for. Usage: exe_window_pagesize.py <madeira checkout>
"""
import sys

REL = "build/ntdll-unix/virtual_ios.c"
ANCHOR = ("void ios_exe_win_commit_claim( void *base, size_t size, int mapped )\n"
          "{\n"
          "    unsigned gen = 0;\n"
          "    int rolled_back = 0, lost = 0;\n")
ADDED = ("\n    /* MYIOSDECK (engine/wine/patches/exe_window_pagesize.py): the claim was recorded in host\n"
         "     * pages (map_fixed_area rounds to them); compare and keep the same size. */\n"
         "    size = (size + host_page_mask) & ~(size_t)host_page_mask;\n")


def main():
    path = "%s/%s" % (sys.argv[1], REL)
    s = open(path).read()
    if "exe_window_pagesize.py" in s:
        print("exe_window_pagesize: already applied")
        return
    n = s.count(ANCHOR)
    if n != 1:
        sys.exit("exe_window_pagesize.py: %s: expected 1 of the commit_claim head, found %d" % (REL, n))
    s = s.replace(ANCHOR, ANCHOR + ADDED)
    open(path, "w").write(s)
    print("exe_window_pagesize: patched %s" % REL)


if __name__ == "__main__":
    main()
