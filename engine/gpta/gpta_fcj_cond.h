// SPDX-License-Identifier: GPL-3.0-or-later
// Conditions for x86 COMIS/UCOMIS, including unordered operands.
FXI_INLINE int fcj_cond(unsigned cc, double a, double b) {
    switch (cc) {
    case 0: case 8: case 12: return 0;
    case 1: case 9: case 13: return 1;
    case 2: return !(a >= b);
    case 3: return a >= b;
    case 4: case 14: return !(a < b || a > b);
    case 5: case 15: return a < b || a > b;
    case 6: return !(a > b);
    case 7: return a > b;
    case 10: return (a != a) | (b != b);
    default: return (a == a) & (b == b);
    }
}
