#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No-JIT: Madeira's Mach exception server asks FXI first about a fault nothing else claimed.

FXI (MYIOSDECK's no-JIT x64 CPU) runs the program's x64 code on the thread's ARM64EC emulator
stack. A wild guest pointer faults inside an FXI handler; Madeira's last-resort delivery
(ios_mach_deliver_guest_exception) then looks for the thread's TEB by stack containment, finds
none (the emulator stack is not the TEB's stack, and x18 can be 0), and falls back to a
best-effort delivery: it worked once (build 76) and left the thread stuck at the fault the next
time (build 77, same fault).

So before that delivery the server calls mid_fxi_mach_fault (App/Sources/Native/fxi_win_host.c)
with the suspended thread's state. FXI takes the fault only when the thread is in simulation
and the address is not mapped at all; it then rewrites the state to enter its own raise path
(the x64 access violation goes to KiUserExceptionDispatcher like every other FXI exception),
and the server writes the state back as for any handled fault. With JIT, FXI is not the CPU:
the hook declines (no thread is registered).

Usage: nojit_fault_hook.py <madeira checkout>. Idempotent (marker line).
"""
import sys

MARK = "/* MYIOSDECK nojit-fault-hook */"


def main():
    path = sys.argv[1] + "/build/ntdll-unix/signal_arm64_ios.c"
    s = open(path).read()
    if MARK in s:
        print("nojit_fault_hook: already applied")
        return
    old = ("            if (!handled && (req->exception == EXC_BAD_ACCESS ||\n"
           "                             req->exception == EXC_BAD_INSTRUCTION))\n")
    new = ("            " + MARK + "\n"
           "            if (!handled && req->exception == EXC_BAD_ACCESS)\n"
           "            {\n"
           "                extern int mid_fxi_mach_fault( mach_port_t thread, arm_thread_state64_t *state,\n"
           "                                               uint64_t fault_addr );\n"
           "                if (mid_fxi_mach_fault( thread, &state, (uint64_t)fault_addr )) handled = 1;\n"
           "            }\n"
           + old)
    if s.count(old) != 1:
        sys.exit("nojit_fault_hook.py: anchor found %d times" % s.count(old))
    s = s.replace(old, new)
    open(path, "w").write(s)
    print("nojit_fault_hook: applied to %s" % path)


if __name__ == "__main__":
    main()
