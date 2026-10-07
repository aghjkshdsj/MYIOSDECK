// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.hpp"
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
template<int Code,bool ExtraAdd> ASTRA_CC void vector_map(Cpu *c,Op *p) {
    constexpr int store=ExtraAdd?3:2,inc=store+1,cmp=inc+1,jcc=cmp+1;
    unsigned reg=p[inc].a.reg;
    U counter=c->r[reg], step=p[inc].b.disp, limit=read_compare(c,p[cmp].b);
    U src=ea(c,p[0].b),dst=ea(c,p[store].b),other=ExtraAdd?ea(c,p[2].b):0;
    U srcstep=step_address(p[0].b,reg,step),dststep=step_address(p[store].b,reg,step);
    U otherstep=ExtraAdd?step_address(p[2].b,reg,step):0;
    Xmm rhs=c->x[p[1].b.reg],v{};
    do {
        std::memcpy(&v,checked(c,src,16,p[0].pc),16);
        v=arithmetic<Code>(v,rhs);
        if constexpr(ExtraAdd) {
            Xmm add;std::memcpy(&add,checked(c,other,16,p[2].pc),16);v.f+=add.f;
        }
        std::memcpy(checked(c,dst,16,p[store].pc,true),&v,16);
        counter+=step;src+=srcstep;dst+=dststep;other+=otherstep;
    } while(counter!=limit);
    c->r[reg]=counter;c->x[p[0].a.reg]=v;
    setflags<CMP,8>(c,counter,limit,counter-limit);
    GO(target(c,p+jcc,p[jcc].end,0));
}
template<int Code> Handler vector_map_form(bool extra) {
    return extra?vector_map<Code,true>:vector_map<Code,false>;
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
    do {
        value=0;std::memcpy(&value,checked(c,src,LoadWidth,p[0].pc),LoadWidth);
        counter+=step;
        if constexpr(WithXor) {U rhs;std::memcpy(&rhs,checked(c,other,8,p[2].pc),8);value^=rhs;other+=otherstep;}
        sum+=value;src+=srcstep;
    } while(counter!=limit);
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
    do {
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
void optimize_block(Cpu *,Op *p,size_t n) {
    if(n>=2 && p[n-1].kind==JCC && (p[n-2].kind==CMP || p[n-2].kind==TEST))
        p[n-2].fn=p[n-2].kind==CMP?compare_size<CMP>(p[n-2]):compare_size<TEST>(p[n-2]);
    if(sum_pattern(p,n))return;
    if((n==6 || n==7) && counted(p,n,n-3)) {
        bool extra=n==7;size_t store=extra?3:2;
        unsigned xmm=p[0].a.reg;
        if(packed_load(p[0]) && packed_store(p[store]) && p[store].a.reg==xmm &&
           p[1].kind==SSE && p[1].a.reg==xmm && p[1].b.mode==REG && p[1].b.reg!=xmm &&
           (!extra || (p[2].kind==SSE && p[2].aux==0x58 && p[2].a.reg==xmm && p[2].b.mode==MEM))) {
            switch(p[1].aux) {
            case 0x1d4:p[0].fn=vector_map_form<0xd4>(extra);return;
            case 0x1fe:p[0].fn=vector_map_form<0xfe>(extra);return;
            case 0x1ef:p[0].fn=vector_map_form<0xef>(extra);return;
            case 0x58:p[0].fn=vector_map_form<0x58>(extra);return;
            case 0x59:p[0].fn=vector_map_form<0x59>(extra);return;
            }
        }
    }
    if(n==5 && simple(p[0],MOV,4,REG,REG) && simple(p[1],ADD,4,REG,REG) &&
       simple(p[2],MOV,1,MEM,IMM) && simple(p[3],CMP,4,REG,IMM) &&
       p[4].kind==JCC && p[4].aux==6 && p[4].imm==p[0].pc &&
       p[0].b.reg==p[1].a.reg && p[3].a.reg==p[1].a.reg &&
       p[0].a.reg!=p[1].a.reg && p[1].b.reg!=p[0].a.reg && p[1].b.reg!=p[1].a.reg)
        p[0].fn=strided_bytes;
}
}
