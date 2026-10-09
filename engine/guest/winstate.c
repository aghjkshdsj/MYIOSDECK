// SPDX-License-Identifier: GPL-3.0-or-later
// Test guest for FXR's Windows-mode state recovery (engine/fxr-probe/winstate_test.c runs it):
// what FXR rebuilds from the host's registers when the thread is stopped or faults
// (fxr_win_host_state) must be the exact x64 state.
//   winstate.elf loop <n>   n iterations of a loop whose registers are known at every
//                           instruction boundary: r15 = the loop's start, r14 = a table of its
//                           instruction offsets (winstate_test.c holds the expected values)
//   winstate.elf fault <s>  every register set to a known value, then fault site s faults
//                           (r15 = the faulting instruction, rbx = an unmapped address)
#include "guest_rt.h"

static unsigned long long g_buf[4] __attribute__((aligned(16)));

// Iteration i (rax = i in the body). Instruction k sets one register from i; before instruction
// k, a register holds the value of its latest setter before k in this iteration, else its value
// from the end of the previous iteration (0 before the first). winstate_test.c, k_loop[].
static void loop(unsigned long long n) {
    __asm__ volatile(
        "lea Lws_s(%%rip), %%r15\n\t"
        "lea Lws_t(%%rip), %%r14\n\t"
        "mov %[buf], %%r13\n\t"
        "xor %%eax, %%eax\n\t" "xor %%ebx, %%ebx\n\t" "xor %%ecx, %%ecx\n\t" "xor %%edx, %%edx\n\t"
        "xor %%esi, %%esi\n\t" "xor %%r8d, %%r8d\n\t" "xor %%r9d, %%r9d\n\t" "xor %%r10d, %%r10d\n\t"
        "xor %%r11d, %%r11d\n\t" "xor %%r12d, %%r12d\n\t" "pxor %%xmm1, %%xmm1\n\t"
        "Lws_s:\n"
        "Lws_0: lea (%%rax,%%rax), %%rbx\n\t"      // rbx = 2i
        "Lws_1: lea (%%rbx,%%rax), %%rcx\n\t"      // rcx = 3i
        "Lws_2: mov %%rcx, (%%r13)\n\t"            // buf = 3i
        "Lws_3: mov (%%r13), %%rdx\n\t"            // rdx = 3i
        "Lws_4: add %%rax, %%rdx\n\t"              // rdx = 4i
        "Lws_5: mov %%rdx, %%rsi\n\t"              // rsi = 4i
        "Lws_6: shl $1, %%rsi\n\t"                 // rsi = 8i
        "Lws_7: movq %%rsi, %%xmm1\n\t"            // xmm1 = 8i
        "Lws_8: paddq %%xmm1, %%xmm1\n\t"          // xmm1 = 16i
        "Lws_9: movq %%xmm1, %%r8\n\t"             // r8 = 16i
        "Lws_10: lea 7(%%r8), %%r9\n\t"            // r9 = 16i + 7
        "Lws_11: mov %%r9, %%r10\n\t"              // r10 = 16i + 7
        "Lws_12: xor %%rax, %%r10\n\t"             // r10 = (16i + 7) ^ i
        "Lws_13: imul $5, %%rax, %%r11\n\t"        // r11 = 5i
        "Lws_14: push %%r11\n\t"                   // rsp - 8
        "Lws_15: pop %%r12\n\t"                    // r12 = 5i
        "Lws_16: add $1, %%rax\n\t"                // rax = i + 1
        "Lws_17: cmp %%rdi, %%rax\n\t"
        "Lws_18: jne Lws_s\n\t"
        ".pushsection .rodata\n\t"
        ".balign 8\n"
        "Lws_t: .quad Lws_0-Lws_s, Lws_1-Lws_s, Lws_2-Lws_s, Lws_3-Lws_s, Lws_4-Lws_s, Lws_5-Lws_s\n\t"
        ".quad Lws_6-Lws_s, Lws_7-Lws_s, Lws_8-Lws_s, Lws_9-Lws_s, Lws_10-Lws_s, Lws_11-Lws_s\n\t"
        ".quad Lws_12-Lws_s, Lws_13-Lws_s, Lws_14-Lws_s, Lws_15-Lws_s, Lws_16-Lws_s, Lws_17-Lws_s, Lws_18-Lws_s\n\t"
        ".popsection"
        : : "D"(n), [buf] "r"(g_buf)
        : "rax", "rbx", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "xmm1", "memory", "cc");
}

// Every register a known value (RAX..R15 except RSP/RBX/R15: 0x1000 * (n + 1) + 0x11; XMM0-7:
// their GPR's), flags from cmp $3, rdx (ZF set), rbx = 0x10 (unmapped), r15 = the faulting
// instruction, then the fault. For the stack sites rsp = 0x1008 (unmapped) instead.
#define SET_ALL                                                                            \
    "mov $0x1011, %%rax\n\t" "mov $0x2011, %%rcx\n\t" "mov $0x3, %%rdx\n\t" "mov $0x10, %%rbx\n\t" \
    "mov $0x7011, %%rsi\n\t" "mov $0x8011, %%rdi\n\t" "mov $0x9011, %%r8\n\t" "mov $0xa011, %%r9\n\t" \
    "mov $0xb011, %%r10\n\t" "mov $0xc011, %%r11\n\t" "mov $0xd011, %%r12\n\t" "mov $0xe011, %%r13\n\t" \
    "mov $0xf011, %%r14\n\t" "mov $0x6011, %%rbp\n\t"                                        \
    "movq %%rax, %%xmm0\n\t" "movq %%rcx, %%xmm1\n\t" "movq %%rdx, %%xmm2\n\t" "movq %%rsi, %%xmm3\n\t" \
    "movq %%rdi, %%xmm4\n\t" "movq %%r8, %%xmm5\n\t" "movq %%r9, %%xmm6\n\t" "movq %%r10, %%xmm7\n\t" \
    "cmp $3, %%rdx\n\t"
#define FAULT(NAME, PRE, INSN)                                                             \
    static void NAME(void) {                                                               \
        __asm__ volatile(SET_ALL PRE "lea 1f(%%rip), %%r15\n\t" "1: " INSN "\n\t"                \
                         : : : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", \
                           "r13", "r14", "r15", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", \
                           "xmm7", "memory", "cc");                                        \
    }
FAULT(f_load, "", "mov (%%rbx), %%rcx")                       // 0: a load
FAULT(f_store, "", "mov %%rcx, (%%rbx)")                      // 1: a store
FAULT(f_xload, "", "movdqu (%%rbx,%%rsi), %%xmm0")            // 2: a 16-byte load with an index
FAULT(f_push, "mov $0x1008, %%rsp\n\t", "push %%rcx")         // 3: push (rsp must stay)
FAULT(f_call, "mov $0x1008, %%rsp\n\t", "call .")             // 4: call
FAULT(f_cmpj, "", "cmpl $5, (%%rbx)\n\tje 2f\n2:")            // 5: cmp memory + jcc (fused)
FAULT(f_ltj, "", "mov 8(%%rbx), %%rcx\n\ttest %%rcx, %%rcx\n\tje 3f\n3:")   // 6: load + test + jcc
FAULT(f_pop, "mov $0x1008, %%rsp\n\t", "pop %%rcx")           // 7: pop
FAULT(f_ret, "mov $0x1008, %%rsp\n\t", "ret")                 // 8: ret
FAULT(f_rmw, "", "addl $1, (%%rbx)")                          // 9: read-modify-write
FAULT(f_callm, "", "call *(%%rbx)")                           // 10: call through memory
FAULT(f_xstore, "", "movaps %%xmm3, 16(%%rbx)")               // 11: a 16-byte store
FAULT(f_xarith_index, "", "addps (%%rbx,%%rsi), %%xmm0")      // 12: indexed SSE arithmetic
FAULT(f_xarith_base, "", "mulsd 16(%%rbx), %%xmm7")           // 13: scalar SSE arithmetic
static void (*const k_faults[])(void) = { f_load, f_store, f_xload, f_push, f_call, f_cmpj, f_ltj, f_pop, f_ret,
                                          f_rmw, f_callm, f_xstore, f_xarith_index, f_xarith_base };

int guest_main(int argc, char **argv) {
    if (argc >= 3 && g_streq(argv[1], "loop")) { loop(g_atou(argv[2])); g_puts("loop done\n"); return 0; }
    if (argc >= 3 && g_streq(argv[1], "fault")) {
        unsigned s = g_atou(argv[2]);
        if (s < sizeof k_faults / sizeof k_faults[0]) k_faults[s]();
        g_puts("no fault\n");
        return 1;
    }
    g_puts("usage: winstate loop <n> | winstate fault <site>\n");
    return 2;
}
