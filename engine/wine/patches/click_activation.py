#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A click on a dialog's button must reach the button (Steins;Gate's launcher: no button worked).

Before a WM_LBUTTONDOWN reaches a window that is not the active one, win32u activates its top
window (process_mouse_message, Madeira's message_ios.c): NtUserGetAncestor(button, GA_ROOT),
WM_MOUSEACTIVATE, then set_foreground_window(top); when that fails the button-down is eaten.
In a game-mode session GA_ROOT came back 0 for the launcher's buttons (the trace showed
WM_MOUSEACTIVATE with wparam 0, then WM_LBUTTONUP without its WM_LBUTTONDOWN), so every click
lost its button-down and no button ever fired, Quit included.

- dlls/win32u/window.c: when list_window_parents() fails, GA_ROOT walks the window's parents
  itself (GA_PARENT while the window has WS_CHILD) instead of returning 0.
- build/win32u-unix/message_ios.c: a click whose top window cannot be activated is delivered
  anyway (Windows gives a clicked window the foreground), with one log line.
Both change only cases that lost the click; every lookup and activation that works is as before.
Usage: click_activation.py <madeira checkout>
"""
import sys

FIXES = {
    "wine/dlls/win32u/window.c": [
        ("    case GA_ROOT:\n"
         "        if (!(list = list_window_parents( hwnd ))) return 0;\n",
         "    case GA_ROOT:\n"
         "        if (!(list = list_window_parents( hwnd )))\n"
         "        {\n"
         "            /* MYIOSDECK (engine/wine/patches/click_activation.py): the parent list could not\n"
         "             * be read; walk up while the window is a child, as Windows' GA_ROOT ends there. */\n"
         "            HWND cur = hwnd, parent;\n"
         "            static int logged;\n"
         "            if (!is_window( hwnd )) return 0;\n"
         "            while ((get_window_long( cur, GWL_STYLE ) & WS_CHILD) && (parent = NtUserGetAncestor( cur, GA_PARENT )))\n"
         "                cur = parent;\n"
         "            if (!logged++) ERR( \"[myiosdeck] GA_ROOT of %p from its parents: %p\\n\", hwnd, cur );\n"
         "            return get_full_window_handle( cur );\n"
         "        }\n", 1),
    ],
    "build/win32u-unix/message_ios.c": [
        ("                    if (!set_foreground_window( hwndTop, TRUE, FALSE )) eat_msg = TRUE;\n",
         "                    if (!set_foreground_window( hwndTop, TRUE, FALSE ))\n"
         "                    {\n"
         "                        /* MYIOSDECK (engine/wine/patches/click_activation.py): delivered anyway;\n"
         "                         * Windows gives a clicked window the foreground. */\n"
         "                        static int warned;\n"
         "                        if (!warned++) ERR( \"[myiosdeck] could not activate %p for a click; delivering it\\n\", hwndTop );\n"
         "                    }\n", 1),
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
                sys.exit("click_activation.py: %s: expected %d of %r, found %d" % (rel, count, old[:48], n))
            s = s.replace(old, new)
        open(path, "w").write(s)
        print("click_activation: patched %s" % rel)


if __name__ == "__main__":
    main()
