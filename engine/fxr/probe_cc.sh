#!/bin/bash
# Can preserve_none + musttail carry the 23 GPR values AND 8 vector values (XMM0-7) in registers?
# Prints the code of two handlers: a scalar double add on pinned vectors, and a pass-through.
#   engine/fxr/probe_cc.sh "<compiler and target flags>"
set -uo pipefail
CC="$1"
cat > /tmp/probe_cc.c <<'EOF'
#include <stdint.h>
typedef double V2d __attribute__((vector_size(16)));
typedef struct U { void *p; uint64_t imm; } U;
#define CC __attribute__((preserve_none))
#define P U *u, uint64_t c, uint64_t g0, uint64_t g1, uint64_t g2, uint64_t g3, uint64_t g4, uint64_t g5, \
    uint64_t g6, uint64_t g7, uint64_t g8, uint64_t g9, uint64_t g10, uint64_t g11, uint64_t g12, uint64_t g13, \
    uint64_t g14, uint64_t g15, uint64_t T, uint64_t F0, uint64_t F1, uint64_t F2, uint64_t F3, \
    V2d x0, V2d x1, V2d x2, V2d x3, V2d x4, V2d x5, V2d x6, V2d x7
#define A u, c, g0, g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11, g12, g13, g14, g15, T, F0, F1, F2, F3, \
    x0, x1, x2, x3, x4, x5, x6, x7
typedef CC void (*F)(P);
CC void h_addsd_0_1(P) { x0[0] = x0[0] + x1[0]; U *n = u + 1; __attribute__((musttail)) return ((F)n->p)(n, c, g0, g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11, g12, g13, g14, g15, T, F0, F1, F2, F3, x0, x1, x2, x3, x4, x5, x6, x7); }
CC void h_add_0_3(P) { g0 += g3; U *n = u + 1; __attribute__((musttail)) return ((F)n->p)(n, c, g0, g1, g2, g3, g4, g5, g6, g7, g8, g9, g10, g11, g12, g13, g14, g15, T, F0, F1, F2, F3, x0, x1, x2, x3, x4, x5, x6, x7); }
EOF
echo "== probe: $CC"
if $CC -O2 -S -o /tmp/probe_cc.s /tmp/probe_cc.c; then
    grep -vE '^\s*(\.|//|;|$)' /tmp/probe_cc.s | grep -vE '^Lfunc|^Ltmp|^\.L' | head -40
else
    echo "probe: compile FAILED"
fi
