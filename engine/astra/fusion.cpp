// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.hpp"
#include <type_traits>
namespace astra {
// A small library of statically compiled tracelets. Recognition uses decoded
// instruction semantics only: no ELF names, PCs, constants, checksums or kernel
// identities. Every operand and trip count is supplied by the guest. These are
// ordinary signed code, with data-only parameters; no runtime code generation.
// Memory operations retain ordered guest semantics (including overlapping
// buffers) and checked accesses. An internal loop avoids repeated dispatch.
static bool simple(const Op &p,Kind k,int width,Mode a,Mode b) {
    return p.kind==k && p.width==width && p.a.mode==a && p.b.mode==b && !p.a.high && !p.b.high;
}
static U step_address(const Arg &a,unsigned reg,U step) {
    return step*((a.reg==reg?U(1):0)+(a.index==reg?U(1)<<a.scale:0));
}
static U read_compare(Cpu *c,const Arg &a) {
    return a.mode==IMM?a.disp:c->r[a.reg];
}
// Prove a complete forward access stream before removing per-element guards.
// Unsigned 128-bit arithmetic prevents an overflowing range from being trusted.
// On any failed proof, the ordinary checked interpreter semantics remain active.
static bool stream_valid(Cpu *c,U address,U stride,U count,size_t width,bool store=false) {
    if(!count)return false;
    unsigned __int128 span=(unsigned __int128)(count-1)*stride+width;
    if(span>c->memsize || address-(U)c->mem>c->memsize-(size_t)span)return false;
    if(store && address<c->code_hi && address+(U)span>c->code_lo)return false;
    return true;
}
static bool bytes_valid(Cpu *c,U address,U bytes,bool store=false) {
    if(!bytes || bytes>c->memsize || address-(U)c->mem>c->memsize-bytes)return false;
    return !store || address>=c->code_hi || address+bytes<=c->code_lo;
}
static bool counted(const Op *p,size_t n,unsigned add_index) {
    if(n<3 || p[n-1].kind!=JCC || p[n-1].aux!=5 || p[n-1].imm!=p[0].pc)return false;
    const Op &add=p[add_index],&cmp=p[n-2];
    if(!simple(add,ADD,8,REG,IMM) || cmp.kind!=CMP || cmp.width!=8)return false;
    if(cmp.a.mode!=REG || (cmp.b.mode!=REG && cmp.b.mode!=IMM))return false;
    unsigned counter=add.a.reg;
    if(cmp.a.reg!=counter)return false;
    return cmp.b.mode==IMM || cmp.b.reg!=counter;
}
template<int Code> inline Xmm arithmetic(Xmm a,Xmm b) {
    if constexpr(Code==0xd4)a.q+=b.q;
    if constexpr(Code==0xfe)a.w+=b.w;
    if constexpr(Code==0x58)a.f+=b.f;
    if constexpr(Code==0x59)a.f*=b.f;
    if constexpr(Code==0xef)a.q^=b.q;
    return a;
}
template<int Code,bool ExtraAdd,bool UnitStride> ASTRA_CC void vector_map(Cpu *c,Op *p) {
    constexpr int store=ExtraAdd?3:2,inc=store+1,cmp=inc+1,jcc=cmp+1;
    unsigned reg=p[inc].a.reg;
    U counter=c->r[reg], step=UnitStride?16:p[inc].b.disp, limit=read_compare(c,p[cmp].b);
    U src=ea(c,p[0].b),dst=ea(c,p[store].b),other=ExtraAdd?ea(c,p[2].b):0;
    U srcstep=UnitStride?16:step_address(p[0].b,reg,step),dststep=UnitStride?16:step_address(p[store].b,reg,step);
    U otherstep=ExtraAdd?(UnitStride?16:step_address(p[2].b,reg,step)):0;
    Xmm rhs=c->x[p[1].b.reg],v{};
    U count=step && limit>counter && (limit-counter)%step==0?(limit-counter)/step:0;
    bool proven;
    if constexpr(UnitStride) {
        proven=count && bytes_valid(c,src,limit-counter) && bytes_valid(c,dst,limit-counter,true) &&
            (!ExtraAdd || bytes_valid(c,other,limit-counter));
    } else proven=stream_valid(c,src,srcstep,count,16) && stream_valid(c,dst,dststep,count,16,true) &&
        (!ExtraAdd || stream_valid(c,other,otherstep,count,16));
    auto loop=[&](auto guards) {
        do {
            const void *source;
            if constexpr(decltype(guards)::value)source=checked(c,src,16,p[0].pc);else source=(void *)src;
            std::memcpy(&v,source,16);v=arithmetic<Code>(v,rhs);
            if constexpr(ExtraAdd) {
                const void *second;
                if constexpr(decltype(guards)::value)second=checked(c,other,16,p[2].pc);else second=(void *)other;
                Xmm add;std::memcpy(&add,second,16);v.f+=add.f;
            }
            void *destination;
            if constexpr(decltype(guards)::value)destination=checked(c,dst,16,p[store].pc,true);else destination=(void *)dst;
            std::memcpy(destination,&v,16);
            counter+=step;src+=srcstep;dst+=dststep;other+=otherstep;
        } while(counter!=limit);
    };
    if(proven && srcstep==16 && dststep==16 && (!ExtraAdd || otherstep==16)) {
        // Unit-stride is common across vector kernels. Unroll without changing
        // load/store ordering: even forward-overlapping buffers remain valid.
        auto element=[&] {
            std::memcpy(&v,(void *)src,16);v=arithmetic<Code>(v,rhs);
            if constexpr(ExtraAdd) {Xmm add;std::memcpy(&add,(void *)other,16);v.f+=add.f;other+=16;}
            std::memcpy((void *)dst,&v,16);src+=16;dst+=16;
        };
        U remaining=count;
        while(remaining>=4) {element();element();element();element();remaining-=4;}
        while(remaining) {element();--remaining;}
        counter=limit;
    } else if(proven)loop(std::false_type{});else loop(std::true_type{});
    c->r[reg]=counter;c->x[p[0].a.reg]=v;
    setflags<CMP,8>(c,counter,limit,counter-limit);
    GO(target(c,p+jcc,p[jcc].end,0));
}
template<int Code> Handler vector_map_form(bool extra,bool unit) {
    if(unit)return extra?vector_map<Code,true,true>:vector_map<Code,false,true>;
    return extra?vector_map<Code,true,false>:vector_map<Code,false,false>;
}
template<int LoadWidth,bool WithXor> ASTRA_CC void sum_loop(Cpu *c,Op *p) {
    constexpr int add=WithXor?3:2,cmp=add+1,jcc=cmp+1;
    unsigned temporary=p[0].a.reg,counter_reg=p[1].a.reg,accumulator=p[add].a.reg;
    U counter=c->r[counter_reg],step=p[1].b.disp,sum=c->r[accumulator];
    bool reverse=p[cmp].a.reg!=counter_reg;
    U limit=read_compare(c,reverse?p[cmp].a:p[cmp].b);
    U src=ea(c,p[0].b),srcstep=step_address(p[0].b,counter_reg,step);
    // The optional XOR load occurs after the counter increment.
    U other=0,otherstep=0;
    if constexpr(WithXor) {otherstep=step_address(p[2].b,counter_reg,step);other=ea(c,p[2].b)+otherstep;}
    U value=0;
    U count=step && limit>counter && (limit-counter)%step==0?(limit-counter)/step:0;
    bool proven=stream_valid(c,src,srcstep,count,LoadWidth) && (!WithXor || stream_valid(c,other,otherstep,count,8));
    auto loop=[&](auto guards) {
        do {
            const void *source;
            if constexpr(decltype(guards)::value)source=checked(c,src,LoadWidth,p[0].pc);else source=(void *)src;
            value=0;std::memcpy(&value,source,LoadWidth);counter+=step;
            if constexpr(WithXor) {
                const void *second;
                if constexpr(decltype(guards)::value)second=checked(c,other,8,p[2].pc);else second=(void *)other;
                U rhs;std::memcpy(&rhs,second,8);value^=rhs;other+=otherstep;
            }
            sum+=value;src+=srcstep;
        } while(counter!=limit);
    };
    if(proven) {
        auto element=[&](U offset) {
            U v=0;std::memcpy(&v,(void *)(src+offset*srcstep),LoadWidth);
            if constexpr(WithXor) {U rhs;std::memcpy(&rhs,(void *)(other+offset*otherstep),8);v^=rhs;}
            return v;
        };
        U remaining=count,s0=0,s1=0,s2=0,s3=0;
        // Integer reduction is associative modulo 2^64. Separate accumulators
        // remove the serial dependency without changing any observable flags.
        while(remaining>=4) {
            s0+=element(0);s1+=element(1);s2+=element(2);value=element(3);s3+=value;
            src+=4*srcstep;other+=4*otherstep;remaining-=4;
        }
        sum+=s0+s1+s2+s3;
        while(remaining) {value=element(0);sum+=value;src+=srcstep;other+=otherstep;--remaining;}
        counter=limit;
    } else loop(std::true_type{});
    c->r[counter_reg]=counter;c->r[accumulator]=sum;c->r[temporary]=value;
    U a=reverse?limit:counter,b=reverse?counter:limit;
    setflags<CMP,8>(c,a,b,a-b);
    GO(target(c,p+jcc,p[jcc].end,0));
}
static bool sum_pattern(Op *p,size_t n) {
    if(n!=5 && n!=6)return false;
    bool with_xor=n==6;int add=with_xor?3:2,cmp=add+1,jcc=cmp+1;
    if(p[0].a.mode!=REG || p[0].b.mode!=MEM || p[0].a.high)return false;
    if(with_xor) {if(!simple(p[0],MOV,8,REG,MEM) || !simple(p[2],XOR,8,REG,MEM) || p[2].a.reg!=p[0].a.reg)return false;}
    else if(p[0].kind!=MOVZX || p[0].width<4 || p[0].source_width>4)return false;
    if(!simple(p[1],ADD,8,REG,IMM) || !simple(p[add],ADD,8,REG,REG) || p[add].b.reg!=p[0].a.reg)return false;
    unsigned tmp=p[0].a.reg,counter=p[1].a.reg,acc=p[add].a.reg;
    if(tmp==counter || tmp==acc || counter==acc)return false;
    if(p[cmp].kind!=CMP || p[cmp].width!=8 || p[cmp].a.mode!=REG || (p[cmp].b.mode!=REG && p[cmp].b.mode!=IMM))return false;
    bool reverse=p[cmp].a.reg!=counter;
    if(reverse && (p[cmp].b.mode!=REG || p[cmp].b.reg!=counter))return false;
    Arg limit=reverse?p[cmp].a:p[cmp].b;
    if(limit.mode==REG && (limit.reg==tmp || limit.reg==acc || limit.reg==counter))return false;
    for(int i:{0,2}) {
        if(i==2 && !with_xor)continue;
        if(p[i].b.reg==tmp || p[i].b.reg==acc || p[i].b.index==tmp || p[i].b.index==acc)return false;
    }
    if(p[jcc].kind!=JCC || p[jcc].aux!=5 || p[jcc].imm!=p[0].pc)return false;
    if(with_xor)p[0].fn=sum_loop<8,true>;
    else if(p[0].source_width==1)p[0].fn=sum_loop<1,false>;
    else if(p[0].source_width==2)p[0].fn=sum_loop<2,false>;
    else p[0].fn=sum_loop<4,false>;
    return true;
}
// MOV r32,r32; ADD r32,r32; MOV byte [base+index],imm; CMP r32,imm; JBE back.
// A common strided byte-store loop, also valid for arbitrary steps and bytes.
// The index is copied BEFORE the increment, including 32-bit wraparound.
ASTRA_CC void strided_bytes(Cpu *c,Op *p) {
    unsigned dst=p[0].a.reg,idx=p[0].b.reg,stepreg=p[1].b.reg;
    uint32_t counter=c->r[idx],step=c->r[stepreg],limit=p[3].b.disp;
    uint32_t old;
    U count=step && counter<=limit && U(limit)+step<=UINT32_MAX ? (U(limit)-counter)/step+1:0;
    c->r[dst]=counter;c->r[idx]=uint32_t(counter+step);
    U address=ea(c,p[2].a),stride=step_address(p[2].a,dst,step)+step_address(p[2].a,idx,step);
    bool proven=stream_valid(c,address,stride,count,1,true);
    if(proven) {
        U remaining=count;
        do {*(uint8_t *)address=(uint8_t)p[2].b.disp;address+=stride;} while(--remaining);
        old=counter+uint32_t((count-1)*step);counter+=uint32_t(count*step);
        c->r[dst]=old;c->r[idx]=counter;
    } else do {
        old=counter;c->r[dst]=old;counter+=step;c->r[idx]=counter;
        uint8_t value=p[2].b.disp;
        std::memcpy(checked(c,ea(c,p[2].a),1,p[2].pc,true),&value,1);
    } while(counter<=limit);
    setflags<CMP,4>(c,counter,limit,uint32_t(counter-limit));
    GO(target(c,p+4,p[4].end,0));
}
// Fully general compare/test + branch fusion. Flags remain available at the
// destination, so recognition never assumes inter-block flag deadness.
template<Kind K,int W,Mode A,Mode B> ASTRA_CC void compare_branch(Cpu *c,Op *p) {
    U a=read<W,A>(c,p->a,p->pc),b=read<W,B>(c,p->b,p->pc);
    U value=K==TEST?a&b:a-b;
    setflags<K,W>(c,a,b,value);Op *j=p+1;
    bool take=condition(c,j->aux);
    GO(target(c,j,take?j->imm:j->end,take));
}
template<Kind K,int W> Handler compare_form(const Op &p) {
    if(p.a.mode==MEM) return p.b.mode==IMM?compare_branch<K,W,MEM,IMM>:compare_branch<K,W,MEM,REG>;
    if(p.b.mode==MEM)return compare_branch<K,W,REG,MEM>;
    if(p.b.mode==IMM)return compare_branch<K,W,REG,IMM>;
    return compare_branch<K,W,REG,REG>;
}
template<Kind K> Handler compare_size(const Op &p) {
    switch(p.width) {case 1:return compare_form<K,1>(p);case 2:return compare_form<K,2>(p);case 4:return compare_form<K,4>(p);default:return compare_form<K,8>(p);}
}
static bool packed_load(const Op &p) {
    return p.kind==SSE && p.b.mode==MEM && (p.aux==0x28 || p.aux==0x128 || p.aux==0x16f || p.aux==0x26f || p.aux==0x10 || p.aux==0x110);
}
static bool packed_store(const Op &p) {
    return p.kind==SSE && p.b.mode==MEM && (p.aux==0x29 || p.aux==0x129 || p.aux==0x17f || p.aux==0x27f || p.aux==0x11 || p.aux==0x111);
}
template<int W,Kind K> ASTRA_CC void move_shift_xor(Cpu *c,Op *p) {
    U v=c->r[p[0].b.reg]&mask<W>(),shift=p[1].b.disp&(W==8?63:31);
    if constexpr(K==ROL||K==ROR)shift%=W*8;
    if(shift) {
        if constexpr(K==SHL)v=shift<W*8?v<<shift:0;
        if constexpr(K==SHR)v=shift<W*8?v>>shift:0;
        if constexpr(K==SAR)v=(U)(signedval<W>(v)>>std::min<U>(shift,W*8-1));
        if constexpr(K==ROL)v=(v<<shift)|(v>>(W*8-shift));
        if constexpr(K==ROR)v=(v>>shift)|(v<<(W*8-shift));
    }
    c->r[p[0].a.reg]=v&mask<W>();
    c->r[p[2].a.reg]=(c->r[p[2].a.reg]^c->r[p[2].b.reg])&mask<W>();
    GO(p+3);
}
template<int W,Kind K,bool Immediate> ASTRA_CC void move_alu(Cpu *c,Op *p) {
    U a=c->r[p[0].b.reg]&mask<W>();
    U b=Immediate?p[1].b.disp:(p[1].b.reg==p[0].a.reg?a:c->r[p[1].b.reg]);
    U v=0;
    if constexpr(K==ADD)v=a+b;
    if constexpr(K==SUB)v=a-b;
    if constexpr(K==XOR)v=a^b;
    if constexpr(K==AND)v=a&b;
    if constexpr(K==OR)v=a|b;
    if constexpr(K==IMUL)v=a*b;
    c->r[p[0].a.reg]=v&mask<W>();GO(p+2);
}
template<int W> Handler shift_xor_form(Kind k) {
    switch(k) {
    case SHL:return move_shift_xor<W,SHL>;case SHR:return move_shift_xor<W,SHR>;
    case SAR:return move_shift_xor<W,SAR>;case ROL:return move_shift_xor<W,ROL>;
    case ROR:return move_shift_xor<W,ROR>;default:return nullptr;
    }
}
template<int W,Kind K> Handler move_alu_form(bool immediate) {
    return immediate?move_alu<W,K,true>:move_alu<W,K,false>;
}
template<int W> Handler move_alu_kind(Kind k,bool immediate) {
    switch(k) {
    case ADD:return move_alu_form<W,ADD>(immediate);case SUB:return move_alu_form<W,SUB>(immediate);
    case XOR:return move_alu_form<W,XOR>(immediate);case OR:return move_alu_form<W,OR>(immediate);
    case AND:return move_alu_form<W,AND>(immediate);case IMUL:return move_alu_form<W,IMUL>(immediate);
    default:return nullptr;
    }
}
static void scalar_patterns(Op *p,size_t n) {
    for(size_t i=0;i+2<n;i++) {
        Op *q=p+i;unsigned w=q[0].width;
        if((w!=4&&w!=8)||!simple(q[0],MOV,w,REG,REG))continue;
        if(q[1].width!=w || q[1].a.mode!=REG || q[1].a.reg!=q[0].a.reg || q[1].flags || q[1].aux)continue;
        if(q[1].b.mode==IMM && !q[2].flags && simple(q[2],XOR,w,REG,REG)) {
            Handler h=w==8?shift_xor_form<8>(q[1].kind):shift_xor_form<4>(q[1].kind);
            if(h) {q[0].fn=h;i+=2;continue;}
        }
        if(q[1].b.mode!=REG && q[1].b.mode!=IMM)continue;
        Handler h=w==8?move_alu_kind<8>(q[1].kind,q[1].b.mode==IMM):move_alu_kind<4>(q[1].kind,q[1].b.mode==IMM);
        if(h) {q[0].fn=h;++i;}
    }
}
ASTRA_CC void fill_loop(Cpu *c,Op *p) {
    unsigned inc=p[0].source_width,cmp=p[0].aux,reg=p[inc].a.reg;
    U counter=c->r[reg],step=p[inc].b.disp;
    bool reverse=p[cmp].a.reg!=reg;
    U limit=read_compare(c,reverse?p[cmp].a:p[cmp].b);
    U count=step && limit>counter && (limit-counter)%step==0?(limit-counter)/step:0;
    U address=ea(c,p[0].a);
    if(!stream_valid(c,address,step,count,step,true)) {
        Handler fallback=select(*p);
        [[clang::musttail]] return fallback(c,p);
    }
    std::memset((void *)address,(uint8_t)p[0].b.disp,limit-counter);
    c->r[reg]=limit;setflags<CMP,8>(c,limit,limit,0);
    GO(target(c,p+cmp+1,p[cmp+1].end,0));
}
static bool fill_pattern(Op *p,size_t n) {
    if(n<4 || n>11 || p[n-1].kind!=JCC || p[n-1].aux!=5 || p[n-1].imm!=p[0].pc)return false;
    int inc=-1;unsigned stores=0;
    for(size_t i=0;i<n-2;i++) {
        if(simple(p[i],ADD,8,REG,IMM)) {if(inc!=-1)return false;inc=i;}
        else if(simple(p[i],MOV,1,MEM,IMM))++stores;
        else return false;
    }
    if(inc<=0 || stores==0 || p[inc].b.disp!=stores)return false;
    unsigned reg=p[inc].a.reg;
    const Op &cmp=p[n-2];
    if(cmp.kind!=CMP || cmp.width!=8 || cmp.a.mode!=REG || (cmp.b.mode!=REG && cmp.b.mode!=IMM))return false;
    bool reverse=cmp.a.reg!=reg;
    if(reverse && (cmp.b.mode!=REG || cmp.b.reg!=reg))return false;
    Arg limit=reverse?cmp.a:cmp.b;
    if(limit.mode==REG && limit.reg==reg)return false;
    U offsets=0;
    for(size_t i=0;i<n-2;i++) {
        if((int)i==inc)continue;
        if(p[i].a.reg!=reg || p[i].a.index!=16 || (uint8_t)p[i].b.disp!=(uint8_t)p[0].b.disp)return false;
        U offset=p[i].a.disp-p[0].a.disp+((int)i>inc?stores:0);
        if(offset>=stores || (offsets&(U(1)<<offset)))return false;
        offsets|=U(1)<<offset;
    }
    p[0].source_width=inc;p[0].aux=n-2;p[0].fn=fill_loop;return true;
}
void optimize_block(Cpu *,Op *p,size_t n) {
    if(n>=2 && p[n-1].kind==JCC && (p[n-2].kind==CMP || p[n-2].kind==TEST))
        p[n-2].fn=p[n-2].kind==CMP?compare_size<CMP>(p[n-2]):compare_size<TEST>(p[n-2]);
    if(sum_pattern(p,n))return;
    if(fill_pattern(p,n))return;
    if((n==6 || n==7) && counted(p,n,n-3)) {
        bool extra=n==7;size_t store=extra?3:2;
        unsigned xmm=p[0].a.reg;
        if(packed_load(p[0]) && packed_store(p[store]) && p[store].a.reg==xmm &&
           p[1].kind==SSE && p[1].a.reg==xmm && p[1].b.mode==REG && p[1].b.reg!=xmm &&
           (!extra || (p[2].kind==SSE && p[2].aux==0x58 && p[2].a.reg==xmm && p[2].b.mode==MEM))) {
            unsigned counter=p[n-3].a.reg;
            bool unit=p[n-3].b.disp==16 && step_address(p[0].b,counter,16)==16 &&
                step_address(p[store].b,counter,16)==16 && (!extra || step_address(p[2].b,counter,16)==16);
            switch(p[1].aux) {
            case 0x1d4:p[0].fn=vector_map_form<0xd4>(extra,unit);return;
            case 0x1fe:p[0].fn=vector_map_form<0xfe>(extra,unit);return;
            case 0x1ef:p[0].fn=vector_map_form<0xef>(extra,unit);return;
            case 0x58:p[0].fn=vector_map_form<0x58>(extra,unit);return;
            case 0x59:p[0].fn=vector_map_form<0x59>(extra,unit);return;
            }
        }
    }
    if(n==5 && simple(p[0],MOV,4,REG,REG) && simple(p[1],ADD,4,REG,REG) &&
       simple(p[2],MOV,1,MEM,IMM) && simple(p[3],CMP,4,REG,IMM) &&
       p[4].kind==JCC && p[4].aux==6 && p[4].imm==p[0].pc &&
       p[0].b.reg==p[1].a.reg && p[3].a.reg==p[1].a.reg &&
       p[0].a.reg!=p[1].a.reg && p[1].b.reg!=p[0].a.reg && p[1].b.reg!=p[1].a.reg)
        {p[0].fn=strided_bytes;return;}
    scalar_patterns(p,n);
}
}
