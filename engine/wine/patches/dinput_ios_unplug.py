#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DirectInput's iOS joystick: an acquired pad that goes away leaves the acquired-device list.

Madeira's dlls/dinput/joystick_ios.c marks a pad that host_pad_state no longer reports
STATUS_UNPLUGGED, but leaves it in dinput_main.c's acquired_device_list. Wine's own unplug path
(dinput_device_internal_unacquire) removes the device as it changes the status. A game that then
re-Acquires (the documented answer to DIERR_INPUTLOST) runs dinput_device_Acquire, which sees a
device that is not STATUS_ACQUIRED and list_add_tail()s the same entry a second time: the list
becomes a cycle and DirectInput's input thread loops in it forever, while the game's thread waits
in SendMessageW for that thread. Build 190, Forager (32-bit, DirectInput pad): the on-screen
controller went away while the game menu was open and came back when it closed; from then on the
input thread ran x86 code in dinput8.dll at 100% of a core and the game froze in SendMessageW.
Build 197 keeps the on-screen controller connected behind the menu, but a real controller that
goes to sleep or disconnects takes the same path.

Here the unplug in the polling path leaves the list exactly as Unacquire does
(dinput_hooks_unacquire_device), once, under the device lock that decides it. The sample Acquire
makes to prime the state (queue == FALSE, device lock held, the device not on the list yet) no
longer changes the status: the next Poll sees the pad gone.

Applied to both farms' dinput/dinput8 (engine/wine/build-i386.sh, engine/wine/build-pe-extras.sh).
Usage: dinput_ios_unplug.py <wine tree>
"""
import sys

PATH = "dlls/dinput/joystick_ios.c"
FIXES = [
    ("        EnterCriticalSection( &impl->base.crit );\n"
     "        impl->base.status = STATUS_UNPLUGGED;\n"
     "        LeaveCriticalSection( &impl->base.crit );\n"
     "        return;\n",
     "        /* MYIOSDECK (engine/wine/patches/dinput_ios_unplug.py): leave the acquired list as\n"
     "         * dinput_main.c's own unplug does, or the next Acquire adds the entry twice. */\n"
     "        BOOL leave;\n"
     "        EnterCriticalSection( &impl->base.crit );\n"
     "        leave = queue && impl->base.status == STATUS_ACQUIRED;\n"
     "        if (queue) impl->base.status = STATUS_UNPLUGGED;\n"
     "        LeaveCriticalSection( &impl->base.crit );\n"
     "        if (leave) dinput_hooks_unacquire_device( iface );\n"
     "        return;\n"),
]


def main():
    path = "%s/%s" % (sys.argv[1], PATH)
    s = open(path).read()
    if "dinput_ios_unplug.py" in s:
        print("dinput_ios_unplug: already applied")
        return
    for old, new in FIXES:
        if s.count(old) != 1:
            sys.exit("dinput_ios_unplug.py: %s: expected 1 of %r, found %d" % (PATH, old[:60], s.count(old)))
        s = s.replace(old, new)
    open(path, "w").write(s)
    print("dinput_ios_unplug: patched %s" % PATH)


if __name__ == "__main__":
    main()
