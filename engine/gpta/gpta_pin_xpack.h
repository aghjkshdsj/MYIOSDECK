// SPDX-License-Identifier: GPL-3.0-or-later
// Eight vector arguments hold sixteen low 64-bit XMM lanes. Upper lanes
// remain in CPU state. The layout is uniform for every guest instruction.
#ifndef GPTA_PIN_XPACK_H
#define GPTA_PIN_XPACK_H
#undef SPILL_X
#undef RELOAD_X
#define XP_SPILL(R) c->xmm[R].q[0] = x##R[0]; c->xmm[(R)+8].q[0] = x##R[1];
#define XP_LOAD(R) x##R = (GptaV){ c->xmm[R].q[0], c->xmm[(R)+8].q[0] };
#define XP8(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)
#define SPILL_X() do { XP8(XP_SPILL) } while (0)
#define RELOAD_X() do { XP8(XP_LOAD) } while (0)
// Fixed operands let scalar operations discard the unused upper-lane access.
#define XP_GET(R,S,L) ((GptaV){ x##S[L], c->xmm[R].q[1] })
#define XP_PUT(R,S,L,V) do { GptaV xp_v_ = (V); x##S[L] = xp_v_[0]; c->xmm[R].q[1] = xp_v_[1]; } while (0)
#define XD_0 XP_GET(0,0,0)
#define XS_0 XD_0
#define XW_0(V) XP_PUT(0,0,0,V)
#define XD_1 XP_GET(1,1,0)
#define XS_1 XD_1
#define XW_1(V) XP_PUT(1,1,0,V)
#define XD_2 XP_GET(2,2,0)
#define XS_2 XD_2
#define XW_2(V) XP_PUT(2,2,0,V)
#define XD_3 XP_GET(3,3,0)
#define XS_3 XD_3
#define XW_3(V) XP_PUT(3,3,0,V)
#define XD_4 XP_GET(4,4,0)
#define XS_4 XD_4
#define XW_4(V) XP_PUT(4,4,0,V)
#define XD_5 XP_GET(5,5,0)
#define XS_5 XD_5
#define XW_5(V) XP_PUT(5,5,0,V)
#define XD_6 XP_GET(6,6,0)
#define XS_6 XD_6
#define XW_6(V) XP_PUT(6,6,0,V)
#define XD_7 XP_GET(7,7,0)
#define XS_7 XD_7
#define XW_7(V) XP_PUT(7,7,0,V)
#define XD_8 XP_GET(8,0,1)
#define XS_8 XD_8
#define XW_8(V) XP_PUT(8,0,1,V)
#define XD_9 XP_GET(9,1,1)
#define XS_9 XD_9
#define XW_9(V) XP_PUT(9,1,1,V)
#define XD_10 XP_GET(10,2,1)
#define XS_10 XD_10
#define XW_10(V) XP_PUT(10,2,1,V)
#define XD_11 XP_GET(11,3,1)
#define XS_11 XD_11
#define XW_11(V) XP_PUT(11,3,1,V)
#define XD_12 XP_GET(12,4,1)
#define XS_12 XD_12
#define XW_12(V) XP_PUT(12,4,1,V)
#define XD_13 XP_GET(13,5,1)
#define XS_13 XD_13
#define XW_13(V) XP_PUT(13,5,1,V)
#define XD_14 XP_GET(14,6,1)
#define XS_14 XD_14
#define XW_14(V) XP_PUT(14,6,1,V)
#define XD_15 XP_GET(15,7,1)
#define XS_15 XD_15
#define XW_15(V) XP_PUT(15,7,1,V)
// The inherited M class always denotes architectural XMM8..15. Its index
// comes from the uop; select a packed lane without forming a stack array.
#define XP_SELECT_CASE(R) case R: xp_lo_ = x##R[1]; break;
#define XP_READ_M(I) ({ unsigned xp_i_ = (I); uint64_t xp_lo_ = 0; \
    switch (xp_i_ & 7) { XP8(XP_SELECT_CASE) } \
    (GptaV){ xp_lo_, c->xmm[xp_i_].q[1] }; })
#define XD_M XP_READ_M(u->dst)
#define XS_M XP_READ_M(u->src)
#define XP_WRITE_CASE(R) case R: x##R[1] = xp_v_[0]; break;
#define XW_M(V) do { GptaV xp_v_ = (V); unsigned xp_i_ = u->dst; \
    switch (xp_i_ & 7) { XP8(XP_WRITE_CASE) } c->xmm[xp_i_].q[1] = xp_v_[1]; } while (0)
#endif
