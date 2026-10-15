#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The DualSense HID pad can be there for every session and still be absent in Xbox mode.

Madeira's wineserver creates its HID controller (build/wineserver/hidpad_ios.c, ml2101) once,
when the session starts, from MADEIRA_HIDPAD; a game finds it through setupapi and opens it.
MYIOSDECK lets the player switch between an Xbox and a PlayStation controller at any time,
including between the games of one kept session (session reuse: the wineserver outlives a game),
so it now always creates the DualSense, and the app says whether it is there: while the app
publishes no HID state (Xbox mode: winios_hidpad_set_state(NULL)), opening the device fails with
STATUS_NO_SUCH_DEVICE, as for a pad that is not plugged in. setupapi still lists its path, so a
game that enumerates sees a device it cannot open and skips it (SDL's HIDAPI, DirectInput's HID
joystick and hid.dll users all open before they use a device). A handle opened while the pad was
there keeps working, at rest when the app stops publishing.

`myiosdeck_hidpad_gate` tells the app this build has the gate (it then always asks for the
DualSense; without it, it asks only in PlayStation mode, as before).
Usage: hidpad_gate.py <madeira checkout>
"""
import sys

REL = "build/wineserver/hidpad_ios.c"
ANCHOR = ("static struct object *hidpad_device_open_file( struct object *obj, unsigned int access,\n"
          "                                               unsigned int sharing, unsigned int options )\n"
          "{\n"
          "    struct hidpad_device *device = (struct hidpad_device *)obj;\n"
          "    struct hidpad_file *file;\n")
GATE = ("\n    /* MYIOSDECK (engine/wine/patches/hidpad_gate.py): absent while the app publishes no pad. */\n"
        "    {\n"
        "        struct winios_hidpad pad;\n"
        "        if (!winios_hidpad_get_state( &pad ) || !pad.connected)\n"
        "        {\n"
        "            static unsigned int refused;\n"
        "            if (refused++ < 4)\n"
        "                fprintf( stderr, \"[hid-pad] open refused: the app shows no PlayStation pad (Xbox mode)\\n\" );\n"
        "            set_error( STATUS_NO_SUCH_DEVICE );\n"
        "            return NULL;\n"
        "        }\n"
        "    }\n")
MARKER = ("/* MYIOSDECK (engine/wine/patches/hidpad_gate.py): the app checks for this symbol. */\n"
          "int myiosdeck_hidpad_gate = 1;\n\n")


def main():
    path = "%s/%s" % (sys.argv[1], REL)
    s = open(path).read()
    if "hidpad_gate.py" in s:
        print("hidpad_gate: already applied")
        return
    n = s.count(ANCHOR)
    if n != 1:
        sys.exit("hidpad_gate.py: %s: expected 1 of the open_file head, found %d" % (REL, n))
    s = s.replace(ANCHOR, MARKER + ANCHOR + GATE)
    open(path, "w").write(s)
    print("hidpad_gate: patched %s" % REL)


if __name__ == "__main__":
    main()
