// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.hpp"
namespace astra {
struct Decoder {
    Cpu *c; U start, pos; unsigned rex=0, prefix=0; bool word=false;
    uint8_t byte() {
        if(pos-start>=15 || pos<c->code_lo || pos>=c->code_hi) fault(c,start,"truncated instruction");
        return *(uint8_t *)pos++;
    }
    U imm(int n,bool sign=false) {
        U x=0; for(int i=0;i<n;i++) x|=U(byte())<<(i*8);
        if(sign && n<8) x=(U)((I)(x<<(64-n*8))>>(64-n*8));
        return x;
    }
    Arg reg(int n,int width) {
        Arg a; a.mode=REG; a.reg=n;
        if(width==1 && !rex && n>=4 && n<8) { a.reg=n-4; a.high=1; }
        return a;
    }
    Arg immediate(U v) { Arg a; a.mode=IMM; a.disp=v; return a; }
    Arg rm(unsigned m,int width) {
        int mod=m>>6, r=m&7;
        if(mod==3) return reg(r+((rex&1)?8:0),width);
        Arg a; a.mode=MEM; a.reg=r+((rex&1)?8:0);
        if(r==4) {
            unsigned sib=byte(); a.scale=sib>>6;
            a.index=((sib>>3)&7)+((rex&2)?8:0);
            if(a.index==4) a.index=16;
            a.reg=(sib&7)+((rex&1)?8:0);
            if(mod==0 && (sib&7)==5) { a.reg=16; a.disp=imm(4,true); }
        } else if(mod==0 && r==5) { a.reg=16; a.high=2; a.disp=imm(4,true); }
        if(mod==1) a.disp+=imm(1,true);
        if(mod==2) a.disp+=imm(4,true);
        return a;
    }
};
Op decode(Cpu *c,U &pc) {
    Decoder d{c,pc,pc}; Op p; p.pc=pc;
    unsigned b=d.byte();
    for(;;) {
        if(b>=0x40 && b<=0x4f) d.rex=b;
        else if(b==0x66) { d.word=true; d.prefix=1; d.rex=0; }
        else if(b==0xf3) { d.prefix=2; d.rex=0; }
        else if(b==0xf2) { d.prefix=3; d.rex=0; }
        else if(b==0x2e || b==0x36 || b==0x3e || b==0x26) { d.rex=0; }
        else break;
        b=d.byte();
    }
    p.width=d.rex&8?8:d.word?2:4;
    auto modrm=[&](int width,bool reverse=false) {
        unsigned m=d.byte(); Arg reg=d.reg(((m>>3)&7)+((d.rex&4)?8:0),width), rm=d.rm(m,width);
        p.a=reverse?reg:rm; p.b=reverse?rm:reg; return (m>>3)&7;
    };
    if(b<0x40 && (b&7)<=5) {
        static const Kind kinds[]={ADD,OR,ADC,SBB,AND,SUB,XOR,CMP};
        p.kind=kinds[b>>3];
        if(!(b&1)) p.width=1;
        if((b&7)<4) modrm(p.width,b&2);
        else { p.a=d.reg(0,p.width); p.b=d.immediate(d.imm(p.width==8?4:p.width,true)); }
    } else if(b>=0xb0 && b<=0xbf) {
        if(b<0xb8) p.width=1;
        p.kind=MOV; p.a=d.reg((b&7)+((d.rex&1)?8:0),p.width); p.b=d.immediate(d.imm(p.width));
    } else if(b>=0x50 && b<=0x5f) {
        p.width=d.word?2:8; p.kind=b<0x58?PUSH:POP; p.a=d.reg((b&7)+((d.rex&1)?8:0),p.width);
    } else if(b>=0x70 && b<=0x7f) {
        p.kind=JCC; p.aux=b&15; I rel=(int8_t)d.byte(); p.imm=d.pos+rel;
    } else switch(b) {
    case 0x63: p.kind=MOVSX; modrm(p.width,true); p.source_width=4; break;
    case 0x68:case 0x6a: p.kind=PUSH; p.width=d.word?2:8; p.a=d.immediate(d.imm(b==0x6a?1:d.word?2:4,true)); break;
    case 0x69:case 0x6b:
        p.kind=IMUL; modrm(p.width,true); p.aux=1; p.imm=d.imm(b==0x6b?1:p.width==8?4:p.width,true); break;
    case 0x80:case 0x81:case 0x83: {
        if(b==0x80) p.width=1;
        static const Kind kinds[]={ADD,OR,ADC,SBB,AND,SUB,XOR,CMP};
        p.kind=kinds[modrm(p.width)]; p.b=d.immediate(d.imm(b==0x81?(p.width==8?4:p.width):1,b==0x83||p.width==8)); break;
    }
    case 0x84:case 0x85: p.kind=TEST; if(b==0x84)p.width=1; modrm(p.width); break;
    case 0x88:case 0x89:case 0x8a:case 0x8b:
        p.kind=MOV; if(!(b&1))p.width=1; modrm(p.width,b&2); break;
    case 0x8d: p.kind=LEA; modrm(p.width,true); if(p.b.mode!=MEM)fault(c,pc,"invalid lea"); break;
    case 0x8f: p.kind=POP; p.width=d.word?2:8; if(modrm(p.width)!=0)fault(c,pc,"unsupported pop"); break;
    case 0x90: if(d.rex&1)fault(c,pc,"unsupported xchg form");p.kind=NOP; break;
    case 0x98:case 0x99: p.kind=SIGNEXT; p.aux=b; break;
    case 0xa8:case 0xa9: p.kind=TEST; if(b==0xa8)p.width=1; p.a=d.reg(0,p.width); p.b=d.immediate(d.imm(p.width==8?4:p.width,true)); break;
    case 0xc0:case 0xc1:case 0xd0:case 0xd1:case 0xd2:case 0xd3: {
        if(!(b&1))p.width=1;
        static const Kind kinds[]={ROL,ROR,STOP,STOP,SHL,SHR,SHL,SAR};
        p.kind=kinds[modrm(p.width)];
        p.b=(b==0xd2||b==0xd3)?d.reg(1,1):d.immediate(b==0xc0||b==0xc1?d.byte():1); break;
    }
    case 0xc2:case 0xc3: p.kind=RET; if(b==0xc2)p.imm=d.imm(2); break;
    case 0xc6:case 0xc7: p.kind=MOV; if(b==0xc6)p.width=1; if(modrm(p.width)!=0)fault(c,pc,"unsupported mov group"); p.b=d.immediate(d.imm(p.width==8?4:p.width,true)); break;
    case 0xc9: p.kind=LEAVE; break;
    case 0xe8:case 0xe9:case 0xeb: {
        p.kind=b==0xe8?CALL:JMP; I rel=(I)d.imm(b==0xeb?1:4,true); p.imm=d.pos+rel; p.a.mode=IMM; break;
    }
    case 0xf6:case 0xf7: {
        if(b==0xf6)p.width=1;
        unsigned sub=modrm(p.width);
        static const Kind kinds[]={TEST,STOP,NOT,NEG,MUL,IMUL,DIV,IDIV}; p.kind=kinds[sub];
        if(sub==0)p.b=d.immediate(d.imm(p.width==8?4:p.width,true));
        if(sub==5)p.aux=2;
        break;
    }
    case 0xfe:case 0xff: {
        if(b==0xfe)p.width=1;
        unsigned sub=modrm(p.width); p.kind=sub==0?INC:sub==1?DEC:sub==2?CALL:sub==4?JMP:sub==6?PUSH:STOP;
        if(sub>=2)p.width=d.word?2:8;
        break;
    }
    case 0x0f: {
        unsigned op=d.byte();
        if(op==0x05) { p.kind=SYSCALL; break; }
        if(op==0xa2) { p.kind=CPUID; break; }
        if(op==0x1f) { if(modrm(p.width)!=0)fault(c,pc,"invalid nop form");p.kind=NOP;break; }
        if(op==0x1e && d.prefix==2) {
            unsigned tail=d.byte();if(tail!=0xfa && tail!=0xfb)fault(c,pc,"unimplemented cet instruction");p.kind=NOP;break;
        }
        if(op>=0x80 && op<=0x8f) { p.kind=JCC; p.aux=op&15; I rel=(I)d.imm(4,true); p.imm=d.pos+rel; break; }
        if(op>=0x90 && op<=0x9f) { p.kind=SETCC; p.width=1; p.aux=op&15; modrm(1); break; }
        if(op>=0x40 && op<=0x4f) { p.kind=CMOV; p.aux=op&15; modrm(p.width,true); break; }
        if(op==0xaf) { p.kind=IMUL; modrm(p.width,true); break; }
        if(op==0xb6 || op==0xb7 || op==0xbe || op==0xbf) {
            p.kind=op&8?MOVSX:MOVZX; p.source_width=op&1?2:1;
            unsigned m=d.byte(); p.a=d.reg(((m>>3)&7)+((d.rex&4)?8:0),p.width); p.b=d.rm(m,p.source_width); break;
        }
        if(op==0x3a) {
            op=d.byte();
            if(d.prefix!=1 || (op!=0x16 && op!=0x22)) fault(c,pc,"unsupported 0f3a opcode");
            unsigned m=d.byte(); p.a=d.reg(((m>>3)&7)+((d.rex&4)?8:0),8); p.b=d.rm(m,p.width);
            p.kind=SSE; p.aux=(1<<8)|(op==0x16?0x36:0x22); p.imm=d.byte(); break;
        }
        p.kind=SSE; p.aux=(d.prefix<<8)|op;
        if(!select_sse(p) || op==0x22 || op==0x36)fault(c,pc,"unimplemented opcode");
        unsigned m=d.byte(); p.a=d.reg(((m>>3)&7)+((d.rex&4)?8:0),8); p.b=d.rm(m,8);
        if((op==0x17 || ((op==0x12||op==0x16) && d.prefix==1)) && p.b.mode!=MEM)
            fault(c,pc,"invalid vector memory form");
        if(op==0x70 || op==0xc6) p.imm=d.byte();
        if(op==0x71 || op==0x72 || op==0x73) {
            if(p.b.mode!=REG)fault(c,pc,"invalid vector shift");
            p.source_width=(m>>3)&7; p.a=p.b; p.imm=d.byte();
            if(p.source_width!=2 && p.source_width!=4 && p.source_width!=6 && !(op==0x73 && (p.source_width==3||p.source_width==7))) fault(c,pc,"unsupported vector shift");
        }
        break;
    }
    default: break;
    }
    p.end=d.pos;
    for(Arg *a:{&p.a,&p.b}) if(a->mode==MEM && a->high==2) { a->disp+=d.pos; a->high=0; }
    if(p.kind==STOP) fault(c,pc,"unimplemented opcode");
    pc=d.pos; return p;
}
static bool reads_flags(const Op &p) {
    return p.kind==JCC||p.kind==SETCC||p.kind==CMOV||p.kind==ADC||p.kind==SBB||p.kind==INC||p.kind==DEC;
}
static bool full_flags(const Op &p) {
    if((p.kind==SHL||p.kind==SHR||p.kind==SAR) && p.b.mode==IMM && (p.b.disp&(p.width==8?63:31)))return true;
    return p.kind==ADD||p.kind==SUB||p.kind==CMP||p.kind==TEST||p.kind==AND||p.kind==OR||p.kind==XOR||p.kind==NEG;
}
Op *block(Cpu *c,U pc) {
    auto found=c->cache.find(pc); if(found!=c->cache.end())return found->second;
    if(c->cache.size()>=100000)fault(c,pc,"decoded block budget exceeded");
    std::vector<Op> ops;
    U cur=pc;
    for(int i=0;i<128;i++) {
        Op p=decode(c,cur); ops.push_back(p);
        if(p.kind==CALL||p.kind==JMP||p.kind==JCC||p.kind==RET||p.kind==SYSCALL)break;
        if(i==127) { Op jump; jump.kind=JMP; jump.a.mode=IMM; jump.imm=cur; jump.pc=cur; jump.end=cur; ops.push_back(jump); }
    }
    // A backward edge can reveal a loop leader inside this block. Split the
    // prefix now: the first iteration must enter the same optimized loop block
    // as later iterations, rather than execute a duplicated scalar prefix.
    if(ops.back().kind==JCC && ops.back().imm>pc && ops.back().imm<ops.back().pc) {
        U leader=ops.back().imm;
        for(size_t i=1;i<ops.size();i++) if(ops[i].pc==leader) {
            ops.resize(i);Op jump;jump.kind=JMP;jump.a.mode=IMM;
            jump.pc=leader;jump.end=leader;jump.imm=leader;ops.push_back(jump);break;
        }
    }
    // Only discard flags when a later full writer dominates every read.
    bool live=true;
    for(auto it=ops.rbegin();it!=ops.rend();++it) {
        it->flags=live;
        if(full_flags(*it)) { it->flags=live; live=false; }
        if(reads_flags(*it))live=true;
        // Variable/zero-count shifts and rotations leave incoming flags live
        // only if a later consumer needs them. They are not full flag writers.
    }
    Op *result=new Op[ops.size()]; c->allocations.push_back(result);
    for(size_t i=0;i<ops.size();i++) {
        result[i]=ops[i]; result[i].fn=select(result[i]);
        if(!result[i].fn)fault(c,result[i].pc,"unimplemented instruction form");
    }
    if(!std::getenv("ASTRA_NO_FUSION"))optimize_block(c,result,ops.size());
    c->cache.emplace(pc,result); ++c->out->blocks; return result;
}
}
