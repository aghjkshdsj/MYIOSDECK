// SPDX-License-Identifier: GPL-3.0-or-later
// Independently authored decoded interpreter. Handler pointers always name
// compiled functions; decoded blocks contain data, never executable code.
#pragma once
#include "astra.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <limits>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>

namespace astra {
using U = uint64_t;
using I = int64_t;
using V4F = float __attribute__((vector_size(16)));
using V2D = double __attribute__((vector_size(16)));
using V2U = uint64_t __attribute__((vector_size(16)));
using V4U = uint32_t __attribute__((vector_size(16)));
using V8U = uint16_t __attribute__((vector_size(16)));
using V16U = uint8_t __attribute__((vector_size(16)));
union alignas(16) Xmm {
    V4F f; V2D d; V2U q; V4U w; V8U h; V16U b;
    float sf[4]; double sd[2]; U uq[2]; uint32_t uw[4];
    uint16_t uh[8]; uint8_t ub[16];
};
struct Cpu;
struct Op;
#if __has_attribute(preserve_none)
#define ASTRA_CC __attribute__((preserve_none))
#else
#define ASTRA_CC
#endif
using Handler = void (ASTRA_CC *)(Cpu *, Op *);
#define NEXT() do { ++p; [[clang::musttail]] return p->fn(c,p); } while (0)
#define GO(q) do { Op *dest_ = (q); [[clang::musttail]] return dest_->fn(c,dest_); } while (0)
enum Kind {
    MOV, ADD, OR, ADC, SBB, AND, SUB, XOR, CMP, TEST, LEA,
    SHL, SHR, SAR, ROL, ROR, IMUL, MUL, DIV, IDIV, NEG, NOT,
    INC, DEC, MOVZX, MOVSX, PUSH, POP, CALL, JMP, JCC, RET,
    SETCC, CMOV, NOP, SYSCALL, CPUID, SIGNEXT, LEAVE, SSE, STOP
};
enum Mode { REG, IMM, MEM };
struct Arg {
    U disp = 0;
    uint8_t reg = 16, index = 16, scale = 0, high = 0;
    Mode mode = REG;
};
struct Op {
    Handler fn = nullptr;
    Arg a{}, b{};
    U pc = 0, end = 0, imm = 0;
    Op *link[2]{};
    Kind kind = STOP;
    uint16_t aux = 0;
    uint8_t width = 8, source_width = 8;
    bool flags = true;
};
struct Lazy {
    U a=0, b=0, r=0, cf=0, of=0;
    int bits=64;
    Kind kind=AND;
    bool explicit_co=false;
};
struct Cpu {
    U r[17]{};
    Xmm x[16]{};
    Lazy f{};
    uint8_t *mem=nullptr;
    size_t memsize=0;
    U code_lo=0, code_hi=0;
    astra_result *out=nullptr;
    std::unordered_map<U,Op *> cache;
    std::vector<Op *> allocations;
    U branches=0;
    ~Cpu() { for (Op *p: allocations) delete[] p; free(mem); }
};
[[noreturn]] inline void fault(Cpu *c, U pc, const char *why) {
    char bytes[49]{};
    size_t n=0;
    for (; n<12 && pc+n-(U)c->mem < c->memsize; ++n)
        std::snprintf(bytes+n*3,4,"%02x ",*(uint8_t *)(pc+n));
    std::snprintf(c->out->error,sizeof(c->out->error),
        "%s at 0x%llx (image+0x%llx): %s",why,
        (unsigned long long)pc,(unsigned long long)(pc-(U)c->mem),bytes);
    throw std::runtime_error(c->out->error);
}
inline void *checked(Cpu *c,U addr,size_t n,U pc,bool store=false) {
    if (n>c->memsize || addr-(U)c->mem > c->memsize-n)
        fault(c,pc,"guest memory outside arena");
    if (store && addr<c->code_hi && addr+n>c->code_lo)
        fault(c,pc,"self-modifying code unsupported");
    return (void *)addr;
}
inline U ea(Cpu *c,const Arg &a) {
    return a.disp+c->r[a.reg]+(c->r[a.index]<<a.scale);
}
template<int W> constexpr U mask() { if constexpr(W==8) return ~U(0); else return (U(1)<<(W*8))-1; }
template<int W> inline I signedval(U v) {
    if constexpr(W==8) return (I)v;
    else return (I)(v<<(64-W*8))>>(64-W*8);
}
template<int W,Mode M> inline U read(Cpu *c,const Arg &a,U pc) {
    if constexpr(M==IMM) return a.disp & mask<W>();
    if constexpr(M==REG) return (c->r[a.reg]>>(a.high*8)) & mask<W>();
    U v=0; std::memcpy(&v,checked(c,ea(c,a),W,pc),W); return v;
}
template<int W,Mode M> inline void write(Cpu *c,const Arg &a,U v,U pc) {
    if constexpr(M==REG) {
        if constexpr(W>=4) c->r[a.reg]=v & mask<W>();
        else { U m=mask<W>()<<(a.high*8); c->r[a.reg]=(c->r[a.reg]&~m)|((v&mask<W>())<<(a.high*8)); }
    } else std::memcpy(checked(c,ea(c,a),W,pc,true),&v,W);
}
inline bool carry(const Lazy &f) {
    if(f.explicit_co) return f.cf;
    if(f.kind==SUB || f.kind==CMP) return f.a<f.b;
    if(f.kind==ADD) return f.r<f.a;
    return false;
}
inline bool overflow(const Lazy &f) {
    if(f.explicit_co) return f.of;
    U v=f.kind==SUB || f.kind==CMP ? (f.a^f.b)&(f.a^f.r) :
        f.kind==ADD ? ~(f.a^f.b)&(f.a^f.r) : 0;
    return (v>>(f.bits-1))&1;
}
inline bool condition(Cpu *c,int cc) {
    const Lazy &f=c->f;
    bool b;
    switch(cc>>1) {
    case 0: b=overflow(f); break;
    case 1: b=carry(f); break;
    case 2: b=f.kind==SSE?f.a:f.r==0; break;
    case 3: b=carry(f)||(f.kind==SSE?bool(f.a):f.r==0); break;
    case 4: b=(f.r>>(f.bits-1))&1; break;
    case 5: b=f.kind==SSE?f.b:!__builtin_parity((unsigned)(uint8_t)f.r); break;
    case 6: b=bool((f.r>>(f.bits-1))&1)!=overflow(f); break;
    default: b=(f.r==0)||(bool((f.r>>(f.bits-1))&1)!=overflow(f)); break;
    }
    return b ^ (cc&1);
}
template<Kind K,int W> inline void setflags(Cpu *c,U a,U b,U v) {
    c->f={a&mask<W>(),b&mask<W>(),v&mask<W>(),0,0,W*8,K,false};
}
Op *block(Cpu *c,U pc);
inline Op *target(Cpu *c,Op *p,U pc,int slot) {
    if(p->link[slot] && p->link[slot]->pc==pc) return p->link[slot];
    return p->link[slot]=block(c,pc);
}

template<Kind K,int W,Mode A,Mode B,bool F> ASTRA_CC void scalar(Cpu *c,Op *p) {
    U a=0,b=0,v=0;
    if constexpr(K!=MOV && K!=LEA && K!=POP && K!=MOVZX && K!=MOVSX && K!=SETCC) a=read<W,A>(c,p->a,p->pc);
    if constexpr(K!=LEA && K!=POP && K!=PUSH && K!=NEG && K!=NOT && K!=INC && K!=DEC && K!=SETCC) b=read<W,B>(c,p->b,p->pc);
    if constexpr(K==MOV) v=b;
    if constexpr(K==ADD) v=a+b;
    if constexpr(K==OR) v=a|b;
    if constexpr(K==AND || K==TEST) v=a&b;
    if constexpr(K==SUB || K==CMP) v=a-b;
    if constexpr(K==XOR) v=a^b;
    if constexpr(K==NEG) { b=a; a=0; v=a-b; }
    if constexpr(K==NOT) v=~a;
    if constexpr(K==INC) { b=1; v=a+1; }
    if constexpr(K==DEC) { b=1; v=a-1; }
    if constexpr(K==ADC || K==SBB) {
        U cf=carry(c->f); v=K==ADC?a+b+cf:a-b-cf;
        if constexpr(F) {
            setflags<K,W>(c,a,b,v); c->f.explicit_co=true;
            if constexpr(K==ADC) { c->f.cf=(unsigned __int128)a+b+cf>mask<W>(); c->f.of=(~(a^b)&(a^v)>>(0))>>(W*8-1)&1; }
            else { c->f.cf=(unsigned __int128)b+cf>a; c->f.of=((a^b)&(a^v))>>(W*8-1)&1; }
        }
    }
    if constexpr(K==IMUL) v=(U)(signedval<W>(a)*(__int128)signedval<W>(b));
    if constexpr(K==LEA) v=ea(c,p->b);
    if constexpr(K==SHL || K==SHR || K==SAR || K==ROL || K==ROR) {
        b &= W==8?63:31;
        if constexpr(K==ROL || K==ROR) b%=W*8;
        v=a;
        if(b) {
            if constexpr(K==SHL) v=b<W*8?a<<b:0;
            if constexpr(K==SHR) v=b<W*8?a>>b:0;
            if constexpr(K==SAR) v=(U)(signedval<W>(a)>>std::min<U>(b,W*8-1));
            if constexpr(K==ROL) v=(a<<b)|(a>>(W*8-b));
            if constexpr(K==ROR) v=(a>>b)|(a<<(W*8-b));
            if constexpr(F) {
                bool cf=false,of=false;
                if constexpr(K==SHL) { cf=b<=W*8 && ((a>>(W*8-b))&1); of=((v>>(W*8-1))&1)^cf; }
                if constexpr(K==SHR || K==SAR) { cf=b<=W*8 && ((a>>(b-1))&1); of=K==SHR && (a>>(W*8-1)); }
                if constexpr(K==ROL) { cf=v&1; of=((v>>(W*8-1))&1)^cf; }
                if constexpr(K==ROR) { cf=(v>>(W*8-1))&1; of=((v>>(W*8-1))^(v>>(W*8-2)))&1; }
                if constexpr(K!=ROL && K!=ROR) setflags<K,W>(c,a,b,v);
                c->f.explicit_co=true; c->f.cf=cf; c->f.of=of;
            }
        }
    }
    if constexpr(K==PUSH) { c->r[4]-=W; std::memcpy(checked(c,c->r[4],W,p->pc,true),&a,W); NEXT(); }
    if constexpr(K==POP) { std::memcpy(&v,checked(c,c->r[4],W,p->pc),W); c->r[4]+=W; }
    if constexpr(K==SETCC) v=condition(c,p->aux);
    if constexpr(K==CMOV) { if(condition(c,p->aux)) write<W,A>(c,p->a,b,p->pc); NEXT(); }
    if constexpr(F && (K==ADD || K==SUB || K==CMP || K==AND || K==OR || K==XOR || K==TEST)) setflags<K,W>(c,a,b,v);
    if constexpr(F && K==NEG) setflags<SUB,W>(c,a,b,v);
    if constexpr(F && (K==INC || K==DEC)) {
        bool cf=carry(c->f); setflags<K==INC?ADD:SUB,W>(c,a,b,v);
        bool of=overflow(c->f); c->f.explicit_co=true; c->f.cf=cf; c->f.of=of;
    }
    if constexpr(F && K==IMUL) {
        __int128 full=(__int128)signedval<W>(a)*signedval<W>(b);
        c->f.explicit_co=true; c->f.cf=c->f.of=full!=signedval<W>(v);
    }
    if constexpr(K!=CMP && K!=TEST) write<W,A>(c,p->a,v,p->pc);
    NEXT();
}
template<Kind K,int W,Mode A,Mode B> Handler flaghandler(bool f) {
    return f ? scalar<K,W,A,B,true> : scalar<K,W,A,B,false>;
}
template<Kind K,int W> Handler form(const Op &p) {
    if(p.a.mode==MEM) {
        if(p.b.mode==IMM) return flaghandler<K,W,MEM,IMM>(p.flags);
        return flaghandler<K,W,MEM,REG>(p.flags);
    }
    if(p.a.mode==IMM) return flaghandler<K,W,IMM,REG>(p.flags);
    if(p.b.mode==MEM) return flaghandler<K,W,REG,MEM>(p.flags);
    if(p.b.mode==IMM) return flaghandler<K,W,REG,IMM>(p.flags);
    return flaghandler<K,W,REG,REG>(p.flags);
}
template<Kind K> Handler sized(const Op &p) {
    switch(p.width) { case 1:return form<K,1>(p); case 2:return form<K,2>(p); case 4:return form<K,4>(p); default:return form<K,8>(p); }
}
Handler select(const Op &p);
Handler select_sse(const Op &p);
Op decode(Cpu *c,U &pc);
}
