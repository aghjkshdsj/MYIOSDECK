#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DirectInput's iOS joystick signals the game's event, from an 8 ms read timer.

Madeira's dlls/dinput/joystick_ios.c samples the host pad only when the game polls it
(Poll/GetDeviceState/GetDeviceData) and has no read_event, so dinput's input thread never
services it and nothing ever signals the event a game hands SetEventNotification. A game that
waits on that event before reading (Steins;Gate: it acquired the pad, read it once, then only
waited) never sees a press. Wine's HID joystick wakes the game from its read() after a report
changes the state.

Here the device gets a periodic waitable timer as its read_event while it is acquired: the input
thread calls ios_joystick_read every 8 ms, which samples (queueing buffered events as polling
does) and signals the game's event when the state changed. Applied to the i386 farm only
(engine/wine/build-i386.sh): 64-bit games keep Madeira's prebuilt dinput.
Usage: dinput_ios_events.py <wine tree>
"""
import sys

PATH = "dlls/dinput/joystick_ios.c"
FIXES = [
    ("static HRESULT ios_joystick_poll( IDirectInputDevice8W *iface )\n"
     "{\n"
     "    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );\n"
     "    ios_joystick_sample( impl, TRUE );\n"
     "    return DI_OK;\n"
     "}\n",
     "static HRESULT ios_joystick_poll( IDirectInputDevice8W *iface )\n"
     "{\n"
     "    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );\n"
     "    ios_joystick_sample( impl, TRUE );\n"
     "    return DI_OK;\n"
     "}\n"
     "\n"
     "/* MYIOSDECK (engine/wine/patches/dinput_ios_events.py): while acquired, read_event is a\n"
     " * periodic 8 ms timer and dinput's input thread calls this, so a game waiting on its\n"
     " * SetEventNotification event is woken when the pad changes, as the HID joystick does. */\n"
     "#define IOS_STATE_SIZE IOS_BUTTON_OFS( 10 )\n"
     "static HRESULT ios_joystick_read( IDirectInputDevice8W *iface )\n"
     "{\n"
     "    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );\n"
     "    BYTE old_state[IOS_STATE_SIZE];\n"
     "    BOOL changed;\n"
     "\n"
     "    EnterCriticalSection( &impl->base.crit );\n"
     "    memcpy( old_state, impl->base.device_state, IOS_STATE_SIZE );\n"
     "    LeaveCriticalSection( &impl->base.crit );\n"
     "    ios_joystick_sample( impl, TRUE );\n"
     "    EnterCriticalSection( &impl->base.crit );\n"
     "    changed = memcmp( old_state, impl->base.device_state, IOS_STATE_SIZE ) != 0;\n"
     "    if (changed && impl->base.hEvent) SetEvent( impl->base.hEvent );\n"
     "    LeaveCriticalSection( &impl->base.crit );\n"
     "    return DI_OK;\n"
     "}\n"
     "\n"
     "static void ios_joystick_destroy( IDirectInputDevice8W *iface )\n"
     "{\n"
     "    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );\n"
     "    if (impl->base.read_event) CloseHandle( impl->base.read_event );\n"
     "}\n"),
    ("    ios_joystick_sample( impl, FALSE );\n"
     "    return DI_OK;\n"
     "}\n"
     "\n"
     "static HRESULT ios_joystick_unacquire( IDirectInputDevice8W *iface )\n"
     "{\n"
     "    return DI_OK;\n"
     "}\n",
     "    ios_joystick_sample( impl, FALSE );\n"
     "    if (impl->base.read_event)   /* MYIOSDECK: the input thread samples every 8 ms */\n"
     "    {\n"
     "        LARGE_INTEGER due = {.QuadPart = -80000};\n"
     "        SetWaitableTimer( impl->base.read_event, &due, 8, NULL, NULL, FALSE );\n"
     "    }\n"
     "    return DI_OK;\n"
     "}\n"
     "\n"
     "static HRESULT ios_joystick_unacquire( IDirectInputDevice8W *iface )\n"
     "{\n"
     "    struct ios_joystick *impl = impl_from_IDirectInputDevice8W( iface );\n"
     "    if (impl->base.read_event) CancelWaitableTimer( impl->base.read_event );\n"
     "    return DI_OK;\n"
     "}\n"),
    ("    impl->user_index = IOS_JOYSTICK_USER;\n",
     "    impl->user_index = IOS_JOYSTICK_USER;\n"
     "    impl->base.read_event = CreateWaitableTimerW( NULL, FALSE, NULL );   /* MYIOSDECK */\n"),
    ("    NULL,                       /* destroy: nothing owned beyond what the core frees\n",
     "    ios_joystick_destroy,       /* MYIOSDECK: closes the read timer; nothing else owned beyond what the core frees\n"),
    ("    NULL,                       /* read: no report thread, nothing to read from */\n",
     "    ios_joystick_read,          /* MYIOSDECK: the 8 ms read timer (dinput_ios_events.py) */\n"),
]


def main():
    path = "%s/%s" % (sys.argv[1], PATH)
    s = open(path).read()
    if "dinput_ios_events.py" in s:
        print("dinput_ios_events: already applied")
        return
    for old, new in FIXES:
        if s.count(old) != 1:
            sys.exit("dinput_ios_events.py: %s: expected 1 of %r, found %d" % (PATH, old[:50], s.count(old)))
        s = s.replace(old, new)
    open(path, "w").write(s)
    print("dinput_ios_events: patched %s" % PATH)


if __name__ == "__main__":
    main()
