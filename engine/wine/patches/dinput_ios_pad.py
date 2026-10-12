#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""DirectInput's iOS joystick (32-bit farm): an Rz axis on the right stick, and the triggers as buttons.

Madeira's dlls/dinput/joystick_ios.c reports Windows' XUSB layout: X/Y left stick, Rx/Ry right
stick, Z both triggers, ten buttons, no Rz. A game's DIJOYSTATE still has an lRz field, and an
axis the device lacks comes back as 0 (device.c GetDeviceState zero-fills it), the far end of
the default 0..65535 range. A game made for PlayStation-style DirectInput pads, whose right stick
is Z/Rz (Steins;Gate: its stock preset puts confirm on button 3, the circle), reads that 0 as the
stick held up and scrolls its menus forever. Here Rz is the right stick's vertical axis, centred
at rest like every other axis, and buttons 11 and 12 are the left and right triggers (past
XInput's threshold), so such a game's trigger actions (Steins;Gate's phone) can be assigned.

Applied to the i386 farm only (engine/wine/build-i386.sh): 64-bit games keep Madeira's
prebuilt dinput.
Usage: dinput_ios_pad.py <wine tree>
"""
import sys

PATH = "dlls/dinput/joystick_ios.c"
FIXES = [
    ("enum ios_object_index\n{\n",
     "/* MYIOSDECK (engine/wine/patches/dinput_ios_pad.py): A B X Y LB RB Back Start LThumb RThumb,\n"
     " * then the left and right triggers as buttons. */\n"
     "#define IOS_BUTTON_COUNT 12\n"
     "\n"
     "enum ios_object_index\n{\n"),
    ("    IOS_OBJ_RY,\n    IOS_OBJ_POV,\n    IOS_OBJ_BUTTON_0,\n    IOS_OBJ_COUNT = IOS_OBJ_BUTTON_0 + 10,\n",
     "    IOS_OBJ_RY,\n    IOS_OBJ_RZ,\n    IOS_OBJ_POV,\n    IOS_OBJ_BUTTON_0,\n"
     "    IOS_OBJ_COUNT = IOS_OBJ_BUTTON_0 + IOS_BUTTON_COUNT,\n"),
    ("    IOS_AXIS( IOS_OBJ_RY, 4, &GUID_RyAxis, HID_USAGE_GENERIC_RY, L\"Y Rotation\" ),\n",
     "    IOS_AXIS( IOS_OBJ_RY, 4, &GUID_RyAxis, HID_USAGE_GENERIC_RY, L\"Y Rotation\" ),\n"
     "    IOS_AXIS( IOS_OBJ_RZ, 5, &GUID_RzAxis, HID_USAGE_GENERIC_RZ, L\"Z Rotation\" ),\n"),
    ("    IOS_BUTTON( 9, L\"Button 9\" ),\n",
     "    IOS_BUTTON( 9, L\"Button 9\" ), IOS_BUTTON( 10, L\"Button 10\" ), IOS_BUTTON( 11, L\"Button 11\" ),\n"),
    ("static void ios_logical_state( const XINPUT_GAMEPAD *pad, LONG values[IOS_VALUE_COUNT], BYTE buttons[10] )\n",
     "static void ios_logical_state( const XINPUT_GAMEPAD *pad, LONG values[IOS_VALUE_COUNT], BYTE buttons[IOS_BUTTON_COUNT] )\n"),
    ("    values[IOS_OBJ_POV] = ios_logical_pov( pad->wButtons );\n"
     "\n"
     "    for (i = 0; i < ARRAY_SIZE(ios_button_mask); ++i)\n"
     "        buttons[i] = (pad->wButtons & ios_button_mask[i]) ? 0x80 : 0x00;\n",
     "    /* MYIOSDECK: an absent Rz reads 0, a range end (see dinput_ios_pad.py); this one is the\n"
     "     * right stick's vertical, where a PlayStation-style layout keeps it. */\n"
     "    values[IOS_OBJ_RZ]  = ios_logical_stick_inverted( pad->sThumbRY );\n"
     "    values[IOS_OBJ_POV] = ios_logical_pov( pad->wButtons );\n"
     "\n"
     "    for (i = 0; i < ARRAY_SIZE(ios_button_mask); ++i)\n"
     "        buttons[i] = (pad->wButtons & ios_button_mask[i]) ? 0x80 : 0x00;\n"
     "    buttons[10] = pad->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x80 : 0x00;\n"
     "    buttons[11] = pad->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x80 : 0x00;\n"),
    ("ios_trace_axis_name[IOS_VALUE_COUNT] = { \"X\", \"Y\", \"Z\", \"RX\", \"RY\", \"POV\" };",
     "ios_trace_axis_name[IOS_VALUE_COUNT] = { \"X\", \"Y\", \"Z\", \"RX\", \"RY\", \"RZ\", \"POV\" };"),
    ("const LONG scaled[IOS_VALUE_COUNT], const BYTE buttons[10] )",
     "const LONG scaled[IOS_VALUE_COUNT], const BYTE buttons[IOS_BUTTON_COUNT] )"),
    ("    for (i = 0; i < ARRAY_SIZE(ios_button_mask); ++i) if (buttons[i]) btn |= 1u << i;\n",
     "    for (i = 0; i < IOS_BUTTON_COUNT; ++i) if (buttons[i]) btn |= 1u << i;\n"),
    ("    BYTE buttons[10];\n",
     "    BYTE buttons[IOS_BUTTON_COUNT];\n"),
    ("pad-env=MADEIRA_DINPUT_PAD trace=state+raw+range+format@1Hz\";",
     "pad-env=MADEIRA_DINPUT_PAD trace=state+raw+range+format@1Hz \"\n"
     "    \"myiosdeck=Rz(right-stick-Y)+buttons11-12(LT,RT)\";"),
]


def main():
    path = "%s/%s" % (sys.argv[1], PATH)
    s = open(path).read()
    if "dinput_ios_pad.py" in s:
        print("dinput_ios_pad: already applied")
        return
    for old, new in FIXES:
        if s.count(old) != 1:
            sys.exit("dinput_ios_pad.py: %s: expected 1 of %r, found %d" % (PATH, old[:60], s.count(old)))
        s = s.replace(old, new)
    open(path, "w").write(s)
    print("dinput_ios_pad: patched %s" % PATH)


if __name__ == "__main__":
    main()
