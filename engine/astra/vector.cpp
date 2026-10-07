// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.hpp"
namespace astra {
// aux: low byte = opcode, upper byte = mandatory prefix (0, 1=66, 2=f3, 3=f2).
// Compiler vectors lower to NEON on ARM64 and SSE on x86; no x86 intrinsics.
template<int Code,int Prefix,bool Memory> ASTRA_CC void vector_op(Cpu *c,Op *p) {
    Xmm a=c->x[p->a.reg], b{}, v=a;
    constexpr bool scalar_fp=Prefix>=2;
    constexpr bool dbl=Prefix==1 || Prefix==3;
    constexpr int lane=dbl?8:4;
    constexpr bool store=Code==0x11 || Code==0x29 || Code==0x7f || Code==0xd6 || (Code==0x7e && Prefix!=2) || Code==0x17;
    constexpr bool gpr=Code==0x2a || Code==0x6e || Code==0x122;
    constexpr bool shiftimm=Code==0x71 || Code==0x72 || Code==0x73;
    if constexpr(!store && !shiftimm && !gpr && Code!=0x116 && Code!=0x50 && Code!=0xd7) {
        if constexpr(Memory) {
            constexpr int n=(Code==0x2e || Code==0x2f)?lane:
                Code==0x12 || Code==0x16 || (Code==0x7e && Prefix==2) || (Code==0x5a && Prefix==0)?8 : scalar_fp && (Code==0x10 || Code==0x51 || (Code>=0x58 && Code<=0x5f) || Code==0x2c || Code==0x2d)?lane:16;
            std::memcpy(&b,checked(c,ea(c,p->b),n,p->pc),n);
        } else b=c->x[p->b.reg];
    }
    if constexpr(Code==0x10 || Code==0x28 || Code==0x6f) {
        if constexpr(Code==0x10 && scalar_fp) {
            if constexpr(Memory) v={};
            if constexpr(dbl) v.sd[0]=b.sd[0]; else v.sf[0]=b.sf[0];
        } else v=b;
    }
    if constexpr(Code==0x7e && Prefix==2) { v={};v.uq[0]=b.uq[0]; }
    if constexpr(store) {
        constexpr int n=(Code==0xd6 || Code==0x17)?8:Code==0x7e?0:(Code==0x11 && scalar_fp)?lane:16;
        if constexpr(Code==0x7e) {
            if(p->width==8) write<8,Memory?MEM:REG>(c,p->b,a.uq[0],p->pc);
            else write<4,Memory?MEM:REG>(c,p->b,a.uw[0],p->pc);
        } else if constexpr(Memory) std::memcpy(checked(c,ea(c,p->b),n,p->pc,true),Code==0x17?(void *)&a.uq[1]:(void *)&a,n);
        else if constexpr(n==16) c->x[p->b.reg]=a;
        else {
            if constexpr(Code==0xd6) { c->x[p->b.reg]={}; c->x[p->b.reg].uq[0]=a.uq[0]; }
            else std::memcpy(&c->x[p->b.reg],&a,n);
        }
        NEXT();
    }
    if constexpr(Code==0x6e || Code==0x2a || Code==0x122) {
        U value=p->width==8?read<8,Memory?MEM:REG>(c,p->b,p->pc):read<4,Memory?MEM:REG>(c,p->b,p->pc);
        if constexpr(Code==0x6e) { v={}; v.uq[0]=value; }
        if constexpr(Code==0x2a) {
            I s=p->width==8?(I)value:(int32_t)value;
            if constexpr(dbl) v.sd[0]=(double)s; else v.sf[0]=(float)s;
        }
        if constexpr(Code==0x122) { if(p->width==8) v.uq[p->imm&1]=value; else v.uw[p->imm&3]=value; }
    }
    if constexpr(Code==0x116) {
        U value=p->width==8?a.uq[p->imm&1]:a.uw[p->imm&3];
        if(p->width==8) write<8,Memory?MEM:REG>(c,p->b,value,p->pc);
        else write<4,Memory?MEM:REG>(c,p->b,value,p->pc);
        NEXT();
    }
    if constexpr(Code==0x2c || Code==0x2d) {
        double f=dbl?b.sd[0]:(double)b.sf[0];
        if constexpr(Code==0x2d) f=std::nearbyint(f);
        if(p->width==8) c->r[p->a.reg]=!std::isfinite(f)||f>=0x1p63||f< -0x1p63?U(1)<<63:(U)(I)f;
        else c->r[p->a.reg]=!std::isfinite(f)||f>=0x1p31||f< -0x1p31?U(1)<<31:(uint32_t)(int32_t)f;
        NEXT();
    }
    if constexpr(Code==0x2e || Code==0x2f) {
        double x=dbl?a.sd[0]:a.sf[0], y=dbl?b.sd[0]:b.sf[0];
        bool un=std::isnan(x)||std::isnan(y),eq=x==y;
        c->f={0,0,un||eq?0u:1u,U(un||x<y),0,64,AND,true};
        // parity encodes unordered; equality also needs even parity but is ordered.
        if(!un) c->f.r=eq?0x100u:1u;
        // flags with independent Z/P are represented by a dedicated marker.
        c->f.a=eq||un; c->f.b=un; c->f.kind=SSE;
        NEXT();
    }
    if constexpr(Code==0x54 || Code==0xdb) v.q=a.q&b.q;
    if constexpr(Code==0x55 || Code==0xdf) v.q=(~a.q)&b.q;
    if constexpr(Code==0x56 || Code==0xeb) v.q=a.q|b.q;
    if constexpr(Code==0x57 || Code==0xef) v.q=a.q^b.q;
    if constexpr(Code==0x58 || Code==0x59 || Code==0x5c || Code==0x5e) {
        if constexpr(scalar_fp) {
            if constexpr(dbl) {
                if constexpr(Code==0x58) v.sd[0]=a.sd[0]+b.sd[0];
                if constexpr(Code==0x59) v.sd[0]=a.sd[0]*b.sd[0];
                if constexpr(Code==0x5c) v.sd[0]=a.sd[0]-b.sd[0];
                if constexpr(Code==0x5e) v.sd[0]=a.sd[0]/b.sd[0];
            } else {
                if constexpr(Code==0x58) v.sf[0]=a.sf[0]+b.sf[0];
                if constexpr(Code==0x59) v.sf[0]=a.sf[0]*b.sf[0];
                if constexpr(Code==0x5c) v.sf[0]=a.sf[0]-b.sf[0];
                if constexpr(Code==0x5e) v.sf[0]=a.sf[0]/b.sf[0];
            }
        } else if constexpr(dbl) {
            if constexpr(Code==0x58) v.d=a.d+b.d;
            if constexpr(Code==0x59) v.d=a.d*b.d;
            if constexpr(Code==0x5c) v.d=a.d-b.d;
            if constexpr(Code==0x5e) v.d=a.d/b.d;
        } else {
            if constexpr(Code==0x58) v.f=a.f+b.f;
            if constexpr(Code==0x59) v.f=a.f*b.f;
            if constexpr(Code==0x5c) v.f=a.f-b.f;
            if constexpr(Code==0x5e) v.f=a.f/b.f;
        }
    }
    if constexpr(Code==0x5a) {
        if constexpr(Prefix==3) v.sf[0]=(float)b.sd[0];
        if constexpr(Prefix==2) v.sd[0]=(double)b.sf[0];
        if constexpr(Prefix==0) { v.sd[0]=b.sf[0]; v.sd[1]=b.sf[1]; }
        if constexpr(Prefix==1) { v={}; v.sf[0]=(float)b.sd[0]; v.sf[1]=(float)b.sd[1]; }
    }
    if constexpr(Code==0x5b) {
        if constexpr(Prefix==0) for(int i=0;i<4;i++) v.sf[i]=(float)(int32_t)b.uw[i];
        else for(int i=0;i<4;i++) { double d=Prefix==2?b.sf[i]:std::nearbyint(b.sf[i]); v.uw[i]=!std::isfinite(d)||d>=0x1p31||d< -0x1p31?0x80000000u:(uint32_t)(int32_t)d; }
    }
    if constexpr(Code==0xfc) v.b=a.b+b.b;
    if constexpr(Code==0xfd) v.h=a.h+b.h;
    if constexpr(Code==0xfe) v.w=a.w+b.w;
    if constexpr(Code==0xd4) v.q=a.q+b.q;
    if constexpr(Code==0xf8) v.b=a.b-b.b;
    if constexpr(Code==0xf9) v.h=a.h-b.h;
    if constexpr(Code==0xfa) v.w=a.w-b.w;
    if constexpr(Code==0xfb) v.q=a.q-b.q;
    if constexpr(Code==0xd5) v.h=a.h*b.h;
    if constexpr(Code==0xf4) { v.uq[0]=(U)a.uw[0]*b.uw[0]; v.uq[1]=(U)a.uw[2]*b.uw[2]; }
    if constexpr(Code==0x76) v.w=(V4U)(a.w==b.w);
    if constexpr(Code==0x74) v.b=(V16U)(a.b==b.b);
    if constexpr(Code==0x75) v.h=(V8U)(a.h==b.h);
    if constexpr(Code==0x66) for(int i=0;i<4;i++) v.uw[i]=(int32_t)a.uw[i]>(int32_t)b.uw[i]?~0u:0;
    if constexpr(Code==0x65) for(int i=0;i<8;i++) v.uh[i]=(int16_t)a.uh[i]>(int16_t)b.uh[i]?0xffff:0;
    if constexpr(Code==0x64) for(int i=0;i<16;i++) v.ub[i]=(int8_t)a.ub[i]>(int8_t)b.ub[i]?0xff:0;
    if constexpr(Code==0xc6) {
        if constexpr(Prefix==1) { v.uq[0]=a.uq[p->imm&1]; v.uq[1]=b.uq[(p->imm>>1)&1]; }
        else { v.uw[0]=a.uw[p->imm&3]; v.uw[1]=a.uw[(p->imm>>2)&3]; v.uw[2]=b.uw[(p->imm>>4)&3]; v.uw[3]=b.uw[(p->imm>>6)&3]; }
    }
    if constexpr(Code==0x70) {
        if constexpr(Prefix==1) for(int i=0;i<4;i++) v.uw[i]=b.uw[(p->imm>>(2*i))&3];
        else { v=b; int start=Prefix==2?4:0; for(int i=0;i<4;i++) v.uh[start+i]=b.uh[start+((p->imm>>(2*i))&3)]; }
    }
    if constexpr(Code==0x14 || Code==0x15) {
        constexpr int s=Code==0x15?(dbl?1:2):0;
        if constexpr(dbl) { v.uq[0]=a.uq[s]; v.uq[1]=b.uq[s]; }
        else { v.uw[0]=a.uw[s]; v.uw[1]=b.uw[s]; v.uw[2]=a.uw[s+1]; v.uw[3]=b.uw[s+1]; }
    }
    if constexpr(Code==0x12) { if constexpr(Memory) v.uq[0]=b.uq[0]; else v.uq[0]=b.uq[1]; }
    if constexpr(Code==0x16) v.uq[1]=b.uq[0];
    if constexpr(Code==0x60 || Code==0x61 || Code==0x62 || Code==0x68 || Code==0x69 || Code==0x6a || Code==0x6c || Code==0x6d) {
        constexpr int sz=(Code==0x60 || Code==0x68)?1:(Code==0x61 || Code==0x69)?2:(Code==0x62 || Code==0x6a)?4:8;
        constexpr int off=(Code==0x68||Code==0x69||Code==0x6a||Code==0x6d)?8:0;
        for(int i=0;i<8/sz;i++) { std::memcpy(v.ub+2*i*sz,a.ub+off+i*sz,sz); std::memcpy(v.ub+(2*i+1)*sz,b.ub+off+i*sz,sz); }
    }
    if constexpr(shiftimm) {
        unsigned n=p->imm, sub=p->source_width;
        if constexpr(Code==0x73) {
            if(sub==3 || sub==7) { v={}; if(n<16) { if(sub==3) std::memcpy(v.ub,a.ub+n,16-n); else std::memcpy(v.ub+n,a.ub,16-n); } }
            else { if(n>=64) v={}; else v.q=sub==6?a.q<<n:a.q>>n; }
        }
        if constexpr(Code==0x72) {
            if(sub==4) for(int i=0;i<4;i++) v.uw[i]=(int32_t)a.uw[i]>>std::min(n,31u);
            else { if(n>=32) v={}; else v.w=sub==6?a.w<<n:a.w>>n; }
        }
        if constexpr(Code==0x71) {
            if(sub==4) for(int i=0;i<8;i++) v.uh[i]=(int16_t)a.uh[i]>>std::min(n,15u);
            else { if(n>=16) v={}; else v.h=sub==6?a.h<<n:a.h>>n; }
        }
    }
    c->x[p->a.reg]=v;
    NEXT();
}
template<int C> Handler pref(const Op &p) {
    bool m=p.b.mode==MEM;
    switch(p.aux>>8) {
    case 0:return m?vector_op<C,0,true>:vector_op<C,0,false>;
    case 1:return m?vector_op<C,1,true>:vector_op<C,1,false>;
    case 2:return m?vector_op<C,2,true>:vector_op<C,2,false>;
    default:return m?vector_op<C,3,true>:vector_op<C,3,false>;
    }
}
Handler select_sse(const Op &p) {
    unsigned code=p.aux&255,prefix=p.aux>>8;
    bool valid=false;
    switch(code) {
    case 0x10:case 0x11:case 0x58:case 0x59:case 0x5a:case 0x5c:case 0x5e:valid=true;break;
    case 0x12:case 0x14:case 0x15:case 0x16:case 0x17:case 0x28:case 0x29:
    case 0x2e:case 0x2f:case 0x54:case 0x55:case 0x56:case 0x57:case 0xc6:valid=prefix<=1;break;
    case 0x2a:case 0x2c:case 0x2d:valid=prefix>=2;break;
    case 0x5b:valid=prefix<=2;break;
    case 0x6f:case 0x7e:case 0x7f:valid=prefix==1||prefix==2;break;
    case 0x70:valid=prefix!=0;break;
    default:valid=prefix==1;break;
    }
    if(!valid)return nullptr;
    switch(p.aux&255) {
#define V(code) case code:return pref<code>(p)
    V(0x10);V(0x11);V(0x12);V(0x14);V(0x15);V(0x16);V(0x17);V(0x28);V(0x29);
    V(0x2a);V(0x2c);V(0x2d);V(0x2e);V(0x2f);
    V(0x54);V(0x55);V(0x56);V(0x57);V(0x58);V(0x59);V(0x5a);V(0x5b);V(0x5c);V(0x5e);
    V(0x60);V(0x61);V(0x62);V(0x64);V(0x65);V(0x66);V(0x68);V(0x69);V(0x6a);V(0x6c);V(0x6d);V(0x6e);V(0x6f);
    V(0x70);V(0x71);V(0x72);V(0x73);V(0x74);V(0x75);V(0x76);V(0x7e);V(0x7f);
    V(0xc6);V(0xd4);V(0xd5);V(0xd6);V(0xdb);V(0xdf);V(0xeb);V(0xef);V(0xf4);
    V(0xf8);V(0xf9);V(0xfa);V(0xfb);V(0xfc);V(0xfd);V(0xfe);
    case 0x22:return pref<0x122>(p);
    case 0x36:return pref<0x116>(p);
#undef V
    default:return nullptr;
    }
}
}
