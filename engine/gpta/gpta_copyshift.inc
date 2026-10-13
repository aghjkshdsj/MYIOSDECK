// SPDX-License-Identifier: GPL-3.0-or-later
// No flags or memory effects: mov D,S; shift/rotate D,imm with dead flags.
// Each width, nonzero masked count and register pair is compiled statically.
#include "gpta_pin.h"
#include "gpta_pin_copyshift.h"
#define CSH31(M, ...) M(1,__VA_ARGS__) M(2,__VA_ARGS__) M(3,__VA_ARGS__) M(4,__VA_ARGS__) M(5,__VA_ARGS__) M(6,__VA_ARGS__) M(7,__VA_ARGS__) M(8,__VA_ARGS__) M(9,__VA_ARGS__) M(10,__VA_ARGS__) M(11,__VA_ARGS__) M(12,__VA_ARGS__) M(13,__VA_ARGS__) M(14,__VA_ARGS__) M(15,__VA_ARGS__) M(16,__VA_ARGS__) M(17,__VA_ARGS__) M(18,__VA_ARGS__) M(19,__VA_ARGS__) M(20,__VA_ARGS__) M(21,__VA_ARGS__) M(22,__VA_ARGS__) M(23,__VA_ARGS__) M(24,__VA_ARGS__) M(25,__VA_ARGS__) M(26,__VA_ARGS__) M(27,__VA_ARGS__) M(28,__VA_ARGS__) M(29,__VA_ARGS__) M(30,__VA_ARGS__) M(31,__VA_ARGS__)
#define CSH63(M, ...) CSH31(M,__VA_ARGS__) M(32,__VA_ARGS__) M(33,__VA_ARGS__) M(34,__VA_ARGS__) M(35,__VA_ARGS__) M(36,__VA_ARGS__) M(37,__VA_ARGS__) M(38,__VA_ARGS__) M(39,__VA_ARGS__) M(40,__VA_ARGS__) M(41,__VA_ARGS__) M(42,__VA_ARGS__) M(43,__VA_ARGS__) M(44,__VA_ARGS__) M(45,__VA_ARGS__) M(46,__VA_ARGS__) M(47,__VA_ARGS__) M(48,__VA_ARGS__) M(49,__VA_ARGS__) M(50,__VA_ARGS__) M(51,__VA_ARGS__) M(52,__VA_ARGS__) M(53,__VA_ARGS__) M(54,__VA_ARGS__) M(55,__VA_ARGS__) M(56,__VA_ARGS__) M(57,__VA_ARGS__) M(58,__VA_ARGS__) M(59,__VA_ARGS__) M(60,__VA_ARGS__) M(61,__VA_ARGS__) M(62,__VA_ARGS__) M(63,__VA_ARGS__)
#define CSH_shl(a,N,SZ) ((a) << (N))
#define CSH_shr(a,N,SZ) ((a) >> (N))
#define CSH_sar(a,N,SZ) ((uint64_t)((int64_t)((a) << (64-SZ)) >> (64-SZ) >> (N)))
#define CSH_rol(a,N,SZ) (((a) << (N)) | ((a) >> (SZ-(N))))
#define CSH_ror(a,N,SZ) (((a) >> (N)) | ((a) << (SZ-(N))))
#define DEF_CSH(S,D,N,OP,SZ) \
    PH p_csh_##OP##_##SZ##_##N##_##D##_##S(GPTA_PARAMS) { \
        uint64_t a = g##S & M##SZ; g##D = CSH_##OP(a,N,SZ) & M##SZ; PNEXT(); }
#define DEF_CSH_D(D,N,OP,SZ) R16B(DEF_CSH,D,N,OP,SZ)
#define DEF_CSH_N(N,OP,SZ) R16(DEF_CSH_D,N,OP,SZ)
#define CSH_DEFS(OP) CSH31(DEF_CSH_N,OP,32) CSH63(DEF_CSH_N,OP,64)
CSH_DEFS(CSH_OP)
#define E_CSH(S,D,N,OP,SZ) p_csh_##OP##_##SZ##_##N##_##D##_##S,
#define ROW_CSH_D(D,N,OP,SZ) { R16B(E_CSH,D,N,OP,SZ) },
#define ROW_CSH_N(N,OP,SZ) [N] = { R16(ROW_CSH_D,N,OP,SZ) },
#define CSH_ROWS(OP) { { CSH31(ROW_CSH_N,OP,32) }, { CSH63(ROW_CSH_N,OP,64) } }
const PFn CSH_TABLE[2][64][16][16] = CSH_ROWS(CSH_OP);
