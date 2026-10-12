// SPDX-License-Identifier: GPL-3.0-or-later
// Immediate shifts/rotates with dead flags. Every legal nonzero count has a
// statically compiled handler; zero counts retain the ordinary lowering.
#include "gpta_pin.h"
#include "gpta_pin_shconst.h"

#define SC31(M, ...) \
    M(1,__VA_ARGS__) M(2,__VA_ARGS__) M(3,__VA_ARGS__) M(4,__VA_ARGS__) \
    M(5,__VA_ARGS__) M(6,__VA_ARGS__) M(7,__VA_ARGS__) M(8,__VA_ARGS__) \
    M(9,__VA_ARGS__) M(10,__VA_ARGS__) M(11,__VA_ARGS__) M(12,__VA_ARGS__) \
    M(13,__VA_ARGS__) M(14,__VA_ARGS__) M(15,__VA_ARGS__) M(16,__VA_ARGS__) \
    M(17,__VA_ARGS__) M(18,__VA_ARGS__) M(19,__VA_ARGS__) M(20,__VA_ARGS__) \
    M(21,__VA_ARGS__) M(22,__VA_ARGS__) M(23,__VA_ARGS__) M(24,__VA_ARGS__) \
    M(25,__VA_ARGS__) M(26,__VA_ARGS__) M(27,__VA_ARGS__) M(28,__VA_ARGS__) \
    M(29,__VA_ARGS__) M(30,__VA_ARGS__) M(31,__VA_ARGS__)
#define SC63(M, ...) SC31(M,__VA_ARGS__) \
    M(32,__VA_ARGS__) M(33,__VA_ARGS__) M(34,__VA_ARGS__) M(35,__VA_ARGS__) \
    M(36,__VA_ARGS__) M(37,__VA_ARGS__) M(38,__VA_ARGS__) M(39,__VA_ARGS__) \
    M(40,__VA_ARGS__) M(41,__VA_ARGS__) M(42,__VA_ARGS__) M(43,__VA_ARGS__) \
    M(44,__VA_ARGS__) M(45,__VA_ARGS__) M(46,__VA_ARGS__) M(47,__VA_ARGS__) \
    M(48,__VA_ARGS__) M(49,__VA_ARGS__) M(50,__VA_ARGS__) M(51,__VA_ARGS__) \
    M(52,__VA_ARGS__) M(53,__VA_ARGS__) M(54,__VA_ARGS__) M(55,__VA_ARGS__) \
    M(56,__VA_ARGS__) M(57,__VA_ARGS__) M(58,__VA_ARGS__) M(59,__VA_ARGS__) \
    M(60,__VA_ARGS__) M(61,__VA_ARGS__) M(62,__VA_ARGS__) M(63,__VA_ARGS__)
#define SC_shl(a,N,SZ) ((a) << (N))
#define SC_shr(a,N,SZ) ((a) >> (N))
#define SC_sar(a,N,SZ) ((uint64_t)((int64_t)((a) << (64-SZ)) >> (64-SZ) >> (N)))
#define SC_rol(a,N,SZ) (((a) << (N)) | ((a) >> (SZ-(N))))
#define SC_ror(a,N,SZ) (((a) >> (N)) | ((a) << (SZ-(N))))
#define DEF_SC(D,N,OP,SZ) \
    PH p_shconst_##OP##_##SZ##_##N##_##D(GPTA_PARAMS) { \
        uint64_t a = g##D & M##SZ; g##D = SC_##OP(a,N,SZ) & M##SZ; PNEXT(); }
#define DEF_SC_N(N,OP,SZ) R16(DEF_SC,N,OP,SZ)
#define DEF_SC_OP(OP) SC31(DEF_SC_N,OP,32) SC63(DEF_SC_N,OP,64)
DEF_SC_OP(shl) DEF_SC_OP(shr) DEF_SC_OP(sar) DEF_SC_OP(rol) DEF_SC_OP(ror)
#define E_SC(D,N,OP,SZ) p_shconst_##OP##_##SZ##_##N##_##D,
#define ROW_SC(N,OP,SZ) [N] = { R16(E_SC,N,OP,SZ) },
#define TABLE_SC(OP) { { SC31(ROW_SC,OP,32) }, { SC63(ROW_SC,OP,64) } },
const PFn t_shconst[5][2][64][16] = {
    TABLE_SC(shl) TABLE_SC(shr) TABLE_SC(sar) TABLE_SC(rol) TABLE_SC(ror)
};
