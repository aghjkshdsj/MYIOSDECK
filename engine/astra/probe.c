// SPDX-License-Identifier: GPL-3.0-or-later
// Differential instruction and fusion tests. This is an x86 guest, never part
// of the portable engine. All expected output is obtained by native execution.
#include "../guest/guest_rt.h"
static unsigned long long digest;
static void number(unsigned long long n) { digest^=n+0x9e3779b97f4a7c15ull+(digest<<6)+(digest>>2); }
#define PROBE(OP) do { \
    unsigned long long a=inputs[i],b=inputs[j]; \
    unsigned char cf,of,zf,sf,pf; \
    __asm__ volatile(OP " %6,%0; setc %1; seto %2; setz %3; sets %4; setp %5" \
       : "+r"(a),"=qm"(cf),"=qm"(of),"=qm"(zf),"=qm"(sf),"=qm"(pf):"r"(b):"cc"); \
    number(a);number(cf | of*2u | zf*4u | sf*8u | pf*16u); \
} while(0)
static unsigned long long inputs[]={0,1,~0ull,0x8000000000000000ull,0x7fffffffffffffffull,255,256};
static unsigned long long a[128] __attribute__((aligned(16))), b[128] __attribute__((aligned(16)));
static unsigned char bytes[256];
int guest_main(int argc,char **argv) {
    (void)argc;(void)argv;
    for(unsigned i=0;i<7;i++) for(unsigned j=0;j<7;j++) {
        PROBE("addq");PROBE("subq");PROBE("xorq");
        // CMP sets carry before ADC/SBB and has no externally visible result.
        PROBE("cmpq $1,%0; adcq");PROBE("cmpq $1,%0; sbbq");
    }
    // Different registers, trip counts, data and counter steps from benchmarks.
    for(unsigned i=0;i<128;i++) { a[i]=i*1234567ull;b[i]=19; }
    for(unsigned count=1;count<=31;count+=5) {
        unsigned long long increment[2]={count,~(unsigned long long)count};
        unsigned long long index=0,limit=count*16;
        __asm__ volatile("movdqu (%3),%%xmm5\n1: movdqu (%1,%0),%%xmm7\n"
                         "paddq %%xmm5,%%xmm7\nmovdqu %%xmm7,(%2,%0)\n"
                         "addq $16,%0\ncmpq %4,%0\njne 1b"
                         :"+r"(index):"r"(a),"r"(b),"r"(increment),"r"(limit):"xmm5","xmm7","cc","memory");
        number(index);for(unsigned i=0;i<count*2;i++)number(b[i]);
    }
    // Forward-overlapping buffers must observe stores from prior iterations.
    unsigned long long index=0,limit=256,increment[2]={13,29};
    __asm__ volatile("movdqu (%3),%%xmm5\n1: movdqu (%1,%0),%%xmm7\n"
                     "paddq %%xmm5,%%xmm7\nmovdqu %%xmm7,(%2,%0)\n"
                     "addq $16,%0\ncmpq %4,%0\njne 1b"
                     :"+r"(index):"r"(a),"r"(a+2),"r"(increment),"r"(limit):"xmm5","xmm7","cc","memory");
    for(unsigned i=0;i<40;i++)number(a[i]);
    for(unsigned step=2;step<=17;step+=3) {
        unsigned idx=0,old;
        __asm__ volatile("1: movl %0,%1\naddl %3,%0\nmovb $93,(%2,%q1)\n"
                         "cmpl $199,%0\njbe 1b"
                         :"+r"(idx),"=&r"(old):"r"(bytes),"r"(step):"cc","memory");
        number(idx);number(old);
    }
    for(unsigned i=0;i<256;i++)number(bytes[i]);
    // COMISD equal/unordered flag results (including ZF as seen by JLE).
    unsigned long long nan=0x7ff8000000000001ull;
    for(unsigned i=0;i<2;i++) {
        unsigned char cf,zf,pf,le;
        __asm__ volatile("pxor %%xmm0,%%xmm0; movq %4,%%xmm1; comisd %%xmm1,%%xmm0; setc %0; setz %1; setp %2; setle %3"
                         :"=qm"(cf),"=qm"(zf),"=qm"(pf),"=qm"(le):"r"(i?nan:0):"xmm0","xmm1","cc");
        number(cf|zf*2u|pf*4u|le*8u);
    }
    g_puts("probe digest=");g_putu(digest);g_puts("\n");return 0;
}
