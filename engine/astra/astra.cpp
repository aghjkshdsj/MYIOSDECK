// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.hpp"
namespace astra {
template<int W> U operand(Cpu *c,const Arg &a,U pc) {
    if(a.mode==MEM)return read<W,MEM>(c,a,pc);
    if(a.mode==IMM)return read<W,IMM>(c,a,pc);
    return read<W,REG>(c,a,pc);
}
template<int W> void put(Cpu *c,const Arg &a,U v,U pc) {
    if(a.mode==MEM)write<W,MEM>(c,a,v,pc); else write<W,REG>(c,a,v,pc);
}
U operand(Cpu *c,const Arg &a,int w,U pc) {
    switch(w) { case 1:return operand<1>(c,a,pc);case 2:return operand<2>(c,a,pc);case 4:return operand<4>(c,a,pc);default:return operand<8>(c,a,pc); }
}
void put(Cpu *c,const Arg &a,U v,int w,U pc) {
    switch(w) {case 1:put<1>(c,a,v,pc);break;case 2:put<2>(c,a,v,pc);break;case 4:put<4>(c,a,v,pc);break;default:put<8>(c,a,v,pc);break;}
}
template<Kind K> ASTRA_CC void control(Cpu *c,Op *p) {
    if constexpr(K==NOP) { NEXT(); }
    if constexpr(K==JCC) { bool take=condition(c,p->aux); GO(target(c,p,take?p->imm:p->end,take)); }
    if constexpr(K==JMP || K==CALL) {
        U pc=p->a.mode==IMM?p->imm:operand<8>(c,p->a,p->pc);
        if constexpr(K==CALL) { c->r[4]-=8; std::memcpy(checked(c,c->r[4],8,p->pc,true),&p->end,8); }
        GO(target(c,p,pc,0));
    }
    if constexpr(K==RET) {
        U pc; std::memcpy(&pc,checked(c,c->r[4],8,p->pc),8); c->r[4]+=8+p->imm;
        GO(target(c,p,pc,0));
    }
    if constexpr(K==LEAVE) {
        c->r[4]=c->r[5]; std::memcpy(&c->r[5],checked(c,c->r[4],8,p->pc),8); c->r[4]+=8; NEXT();
    }
    if constexpr(K==SIGNEXT) {
        if(p->aux==0x98) {
            if(p->width==8)c->r[0]=(I)(int32_t)c->r[0];
            else if(p->width==4)c->r[0]=(uint32_t)(int32_t)(int16_t)c->r[0];
            else c->r[0]=(c->r[0]&~U(65535))|(uint16_t)(int16_t)(int8_t)c->r[0];
        } else {
            if(p->width==8)c->r[2]=(I)c->r[0]>>63;
            else if(p->width==4)c->r[2]=(uint32_t)((int32_t)c->r[0]>>31);
            else c->r[2]=(c->r[2]&~U(65535))|(uint16_t)((int16_t)c->r[0]>>15);
        }
        NEXT();
    }
    if constexpr(K==MOVSX || K==MOVZX) {
        U v=operand(c,p->b,p->source_width,p->pc);
        if constexpr(K==MOVSX) { if(p->source_width==1)v=(I)(int8_t)v; else if(p->source_width==2)v=(I)(int16_t)v; else v=(I)(int32_t)v; }
        put(c,p->a,v,p->width,p->pc); NEXT();
    }
    if constexpr(K==IMUL) {
        U a=operand(c,p->b,p->width,p->pc), b=p->imm;
        __int128 prod=p->width==8?(__int128)(I)a*(I)b:p->width==4?(__int128)(int32_t)a*(int32_t)b:(__int128)(int16_t)a*(int16_t)b;
        put(c,p->a,(U)prod,p->width,p->pc);
        I result=p->width==8?(I)prod:p->width==4?(int32_t)prod:(int16_t)prod;
        c->f.explicit_co=true;c->f.cf=c->f.of=prod!=result; NEXT();
    }
    if constexpr(K==CPUID) {
        uint32_t leaf=c->r[0], a=0,b=0,cc=0,d=0;
        if(leaf==0) { a=1; std::memcpy(&b,"AstraCPU",4); std::memcpy(&d,"aCPU",4); std::memcpy(&cc,"    ",4); }
        if(leaf==1) { a=0x600; d=(1u<<26)|(1u<<25)|(1u<<15)|(1u<<8)|(1u<<0); }
        if(leaf==0x80000000u)a=0x80000004u;
        if(leaf>=0x80000002u && leaf<=0x80000004u) {
            char brand[49]="Astra portable no-JIT x86-64 interpreter";
            const char *s=brand+(leaf-0x80000002u)*16;
            std::memcpy(&a,s,4);std::memcpy(&b,s+4,4);std::memcpy(&cc,s+8,4);std::memcpy(&d,s+12,4);
        }
        c->r[0]=a;c->r[3]=b;c->r[1]=cc;c->r[2]=d; NEXT();
    }
    if constexpr(K==SYSCALL) {
        ++c->out->syscalls; c->r[1]=p->end;
        c->r[11]=0x202|U(condition(c,2))|U(condition(c,10))*4|
            (c->f.kind==SSE?0:((c->f.a^c->f.b^c->f.r)&16))|U(condition(c,4))*64|
            U(condition(c,8))*128|U(condition(c,0))*2048;
        if(c->r[0]==60 || c->r[0]==231) { c->out->ok=1;c->out->exit_code=(int)c->r[7];return; }
        if(c->r[0]==1) {
            if(c->r[7]!=1 && c->r[7]!=2)c->r[0]=(U)-9;
            else {
                const void *src=checked(c,c->r[6],c->r[2],p->pc);
                size_t n=std::min<size_t>(c->r[2],sizeof(c->out->output)-1-c->out->output_len);
                std::memcpy(c->out->output+c->out->output_len,src,n); c->out->output_len+=n;
                c->out->output[c->out->output_len]=0; c->r[0]=c->r[2];
            }
        } else if(c->r[0]==228) {
            if(c->r[7]!=1) c->r[0]=(U)-22;
            else { timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts); I value[2]={(I)ts.tv_sec,(I)ts.tv_nsec}; std::memcpy(checked(c,c->r[6],16,p->pc,true),value,16);c->r[0]=0; }
        } else fault(c,p->pc,"unimplemented syscall");
        GO(target(c,p,p->end,0));
    }
}
template<Kind K,int W> ASTRA_CC void wide(Cpu *c,Op *p) {
    U v=operand<W>(c,p->a,p->pc), lo=c->r[0]&mask<W>();
    if constexpr(K==MUL || K==IMUL) {
        unsigned __int128 prod;
        if constexpr(K==MUL)prod=(unsigned __int128)lo*v;
        else prod=(unsigned __int128)((__int128)signedval<W>(lo)*signedval<W>(v));
        if constexpr(W==1)c->r[0]=(c->r[0]&~U(65535))|((U)prod&65535);
        else { Arg a;a.reg=0;write<W,REG>(c,a,(U)prod,p->pc);a.reg=2;write<W,REG>(c,a,(U)(prod>>(W*8)),p->pc); }
        c->f.explicit_co=true;
        if constexpr(K==MUL)c->f.cf=c->f.of=(prod>>(W*8))!=0;
        else c->f.cf=c->f.of=(__int128)prod!=signedval<W>((U)prod);
    } else {
        if(!v)fault(c,p->pc,"integer division by zero");
        unsigned __int128 dividend=W==1?(c->r[0]&65535):((unsigned __int128)(c->r[2]&mask<W>())<<(W*8))|lo;
        U quotient,rem;
        if constexpr(K==DIV) { auto q=dividend/v; if(q>mask<W>())fault(c,p->pc,"integer division overflow"); quotient=(U)q;rem=(U)(dividend%v); }
        else {
            __int128 s;
            if constexpr(W==8)s=(__int128)dividend;
            else s=(__int128)(dividend<<(128-W*16))>>(128-W*16);
            I divisor=signedval<W>(v);
            if(s==(__int128)((unsigned __int128)1<<127) && divisor==-1)fault(c,p->pc,"integer division overflow");
            __int128 q=s/divisor; I min=W==8?INT64_MIN:-(I(1)<<(W*8-1)),max=W==8?INT64_MAX:(I(1)<<(W*8-1))-1;
            if(q<min||q>max)fault(c,p->pc,"integer division overflow");quotient=(U)q;rem=(U)(s%divisor);
        }
        if constexpr(W==1)c->r[0]=(c->r[0]&~U(65535))|(quotient&255)|((rem&255)<<8);
        else {Arg a;a.reg=0;write<W,REG>(c,a,quotient,p->pc);a.reg=2;write<W,REG>(c,a,rem,p->pc);}
    }
    NEXT();
}
template<Kind K> Handler wide_size(int w) {
    switch(w){case 1:return wide<K,1>;case 2:return wide<K,2>;case 4:return wide<K,4>;default:return wide<K,8>;}
}
Handler select(const Op &p) {
    if(p.kind==SSE)return select_sse(p);
    if(p.kind==IMUL && p.aux==1)return control<IMUL>;
    if(p.kind==IMUL && p.aux==2)return wide_size<IMUL>(p.width);
    if(p.kind==MUL)return wide_size<MUL>(p.width);
    if(p.kind==DIV)return wide_size<DIV>(p.width);
    if(p.kind==IDIV)return wide_size<IDIV>(p.width);
    switch(p.kind) {
#define S(k) case k:return sized<k>(p)
    S(MOV);S(ADD);S(OR);S(ADC);S(SBB);S(AND);S(SUB);S(XOR);S(CMP);S(TEST);S(LEA);
    S(SHL);S(SHR);S(SAR);S(ROL);S(ROR);S(IMUL);S(NEG);S(NOT);S(INC);S(DEC);S(PUSH);S(POP);S(SETCC);S(CMOV);
#undef S
#define C(k) case k:return control<k>
    C(CALL);C(JMP);C(JCC);C(RET);C(NOP);C(SYSCALL);C(CPUID);C(SIGNEXT);C(LEAVE);C(MOVZX);C(MOVSX);
#undef C
    default:return nullptr;
    }
}
// ELF parsing uses byte copies, avoiding alignment/aliasing assumptions and
// platform-specific elf.h (not available in the iPhone SDK).
template<class T> T field(const uint8_t *p,size_t n,size_t off) {
    if(off>n || sizeof(T)>n-off)throw std::runtime_error("truncated ELF header");
    T v;std::memcpy(&v,p+off,sizeof(v));return v;
}
double now() { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9; }
}
extern "C" const char *astra_version() { return "astra 0.1"; }
extern "C" int astra_run_elf(const uint8_t *elf,size_t len,int argc,const char *const *argv,astra_result *out) {
    using namespace astra;
    if(!out)return 0;
    std::memset(out,0,sizeof(*out)); double start=now();
    try {
        if(!elf || len<64 || std::memcmp(elf,"\177ELF\2\1\1",7) || field<uint16_t>(elf,len,16)!=3 || field<uint16_t>(elf,len,18)!=62)
            throw std::runtime_error("expected little-endian ELF64 x86-64 static PIE");
        if(argc<0 || argc>4096 || (argc && !argv))throw std::runtime_error("invalid guest argv");
        U phoff=field<U>(elf,len,32),entry=field<U>(elf,len,24);
        unsigned phsize=field<uint16_t>(elf,len,54),phnum=field<uint16_t>(elf,len,56);
        if(phsize<56 || phoff>len || phnum>(len-phoff)/phsize)throw std::runtime_error("invalid ELF program headers");
        U image_size=0;Cpu c;c.out=out;
        for(unsigned i=0;i<phnum;i++) {
            const uint8_t *p=elf+phoff+i*phsize;
            unsigned type=field<uint32_t>(p,phsize,0);
            if(type==3)throw std::runtime_error("dynamic ELF interpreter unsupported");
            if(type!=1)continue;
            U off=field<U>(p,phsize,8),va=field<U>(p,phsize,16),fs=field<U>(p,phsize,32),ms=field<U>(p,phsize,40);
            if(off>len||fs>len-off||ms<fs||va>(1u<<28)||ms>(1u<<28)-va)throw std::runtime_error("invalid ELF load segment");
            image_size=std::max(image_size,va+ms);
        }
        if(!image_size)throw std::runtime_error("empty ELF image");
        U rounded=(image_size+16383)&~U(16383);c.memsize=rounded+8*1024*1024;
        void *arena=nullptr;if(posix_memalign(&arena,16384,c.memsize))throw std::bad_alloc();
        c.mem=(uint8_t *)arena;std::memset(c.mem,0,c.memsize);
        U base=(U)c.mem;
        for(unsigned i=0;i<phnum;i++) {
            const uint8_t *p=elf+phoff+i*phsize;if(field<uint32_t>(p,phsize,0)!=1)continue;
            U off=field<U>(p,phsize,8),va=field<U>(p,phsize,16),fs=field<U>(p,phsize,32),ms=field<U>(p,phsize,40);
            std::memcpy(c.mem+va,elf+off,fs);
            if(field<uint32_t>(p,phsize,4)&1) {
                if(c.code_hi)throw std::runtime_error("multiple executable segments unsupported");
                c.code_lo=base+va;c.code_hi=base+va+ms;
            }
        }
        if(base+entry<c.code_lo || base+entry>=c.code_hi)throw std::runtime_error("ELF entry outside code segment");
        U sp=base+c.memsize;std::vector<U> ptrs;
        for(int i=0;i<argc;i++) {
            if(!argv[i])throw std::runtime_error("null argument");
            size_t n=strnlen(argv[i],65536)+1;
            if(n>65536 || sp-base-rounded<n+65536)throw std::runtime_error("guest arguments too large");
            sp-=n;std::memcpy((void *)sp,argv[i],n);ptrs.push_back(sp);
        }
        sp=(sp-U(argc+5)*8)&~U(15);U *stack=(U *)sp;stack[0]=argc;
        for(int i=0;i<argc;i++)stack[i+1]=ptrs[i];
        stack[argc+1]=0;stack[argc+2]=0;stack[argc+3]=0;stack[argc+4]=0;
        c.r[4]=sp;
        Op *first=block(&c,base+entry);first->fn(&c,first);
    } catch(const std::exception &e) {
        if(!out->error[0])std::snprintf(out->error,sizeof(out->error),"%s",e.what());
        out->ok=0;
    }
    out->seconds=now()-start;return out->ok;
}
