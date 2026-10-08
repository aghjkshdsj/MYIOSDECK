#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Game-mode dialogs a player can see and a log can read (Madeira's win32u unix side).

Build 97: Stick Fight with -force-glcore showed a black screen. Unity had opened its "Error"
dialog and waited on it, but the dialog sat at x = 32767, outside the 1560x720 virtual screen
(game-input vis={32767,-2,33157,227}), so nothing visible was drawn and nobody could press
OK. The log named the window ("Error", "Details:", "OK") but not the message: Unity fills the
text in after creating the controls, with WM_SETTEXT.

Two changes:
  build/win32u-unix/driver_ios.c   in game mode, a visible top-level window with a title bar
                                   whose rectangle lies entirely outside the virtual screen
                                   is moved to the screen's centre ([game-dialog] line).
  build/win32u-unix/message_ios.c  WM_SETTEXT sent to any window is logged ([win-text], first
                                   200): the message and details of error dialogs, window
                                   titles.

Usage: game_dialogs.py <madeira checkout>. Idempotent (marker lines).
"""
import sys

MARK = "/* MYIOSDECK game-dialogs */"

ONSCREEN = MARK + r'''
/* A game-mode dialog placed entirely off the virtual screen (Unity's error box at x = 32767)
 * is drawn where nobody sees it and waits forever: move it to the screen's centre. Only
 * visible, non-minimised top-level windows with a title bar; helper windows parked off
 * screen have none. engine/wine/patches/game_dialogs.py */
static void myiosdeck_onscreen_dialog( HWND hwnd, const RECT *win )
{
    static int busy;
    RECT screen, inter;
    DWORD style = get_window_long( hwnd, GWL_STYLE );
    int w = win->right - win->left, h = win->bottom - win->top, x, y;

    if (busy || (style & (WS_MINIMIZE | WS_CHILD)) || !(style & WS_VISIBLE) ||
        (style & WS_CAPTION) != WS_CAPTION || w <= 0 || h <= 0) return;
    screen = get_virtual_screen_rect( 0, MDT_DEFAULT );
    if (intersect_rect( &inter, win, &screen )) return;
    x = screen.left + max( 0, (int)(screen.right - screen.left - w) / 2 );
    y = screen.top + max( 0, (int)(screen.bottom - screen.top - h) / 2 );
    dprintf( 2, "[game-dialog] hwnd=%p at {%d,%d %dx%d} is outside the %dx%d screen: moved to %d,%d\n",
             hwnd, (int)win->left, (int)win->top, w, h, (int)(screen.right - screen.left),
             (int)(screen.bottom - screen.top), x, y );
    busy = 1;
    NtUserSetWindowPos( hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    busy = 0;
}

'''

SETTEXT = MARK + r'''
#include <stdio.h>
/* Text sent with WM_SETTEXT, for the log: what a dialog says (an error box's message and
 * details) is otherwise invisible when nothing on screen shows it.
 * engine/wine/patches/game_dialogs.py */
static void myiosdeck_log_settext( HWND hwnd, LPARAM lparam, BOOL ansi )
{
    static int count;
    char txt[600];
    int j = 0;

    if (!lparam || count >= 200) return;
    if (ansi)
    {
        const unsigned char *s = (const unsigned char *)lparam;
        for (; s[j] && j < (int)sizeof(txt) - 1; j++)
            txt[j] = s[j] == '\r' || s[j] == '\n' || s[j] == '\t' ? ' ' : s[j] >= 32 && s[j] < 127 ? s[j] : '?';
    }
    else
    {
        const WCHAR *s = (const WCHAR *)lparam;
        for (; s[j] && j < (int)sizeof(txt) - 1; j++)
            txt[j] = s[j] == '\r' || s[j] == '\n' || s[j] == '\t' ? ' ' : s[j] >= 32 && s[j] < 127 ? (char)s[j] : '?';
    }
    txt[j] = 0;
    if (!j) return;
    count++;
    fprintf( stderr, "[win-text] hwnd=%p \"%s\"\n", hwnd, txt );
}

'''


def sub(s, old, new, what, path):
    n = s.count(old)
    if n != 1:
        sys.exit("game_dialogs.py: %s: anchor for %s found %d times" % (path, what, n))
    return s.replace(old, new)


def main():
    root = sys.argv[1]

    path = root + "/build/win32u-unix/driver_ios.c"
    s = open(path).read()
    if MARK not in s:
        head = "/* pWindowPosChanged wrapper: dereference window_rects HERE"
        s = sub(s, head, ONSCREEN + head, "pWindowPosChanged wrapper", path)
        call = "        if (visible && surface && winios_game_windows()) winios_note_dialog_thread( hwnd, v );\n"
        s = sub(s, call, call +
                "        if (visible && winios_game_windows() && !winios_desktop_mode())\n"
                "            myiosdeck_onscreen_dialog( hwnd, &new_rects->window );\n",
                "game-mode dialog note", path)
        open(path, "w").write(s)
        print("game_dialogs: applied to %s" % path)

    path = root + "/build/win32u-unix/message_ios.c"
    s = open(path).read()
    if MARK not in s:
        head = ("LRESULT WINAPI NtUserMessageCall( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,\n"
                "                                  void *result_info, DWORD type, BOOL ansi )\n{\n")
        s = sub(s, head, SETTEXT + head +
                "    if (msg == WM_SETTEXT && (type == NtUserSendMessage || type == NtUserSendMessageTimeout))\n"
                "        myiosdeck_log_settext( hwnd, lparam, ansi );\n",
                "NtUserMessageCall", path)
        open(path, "w").write(s)
        print("game_dialogs: applied to %s" % path)


if __name__ == "__main__":
    main()
