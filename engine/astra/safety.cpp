// SPDX-License-Identifier: GPL-3.0-or-later
#include "astra.h"
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
template<class T> void put(std::vector<uint8_t> &v,size_t off,T value) {std::memcpy(v.data()+off,&value,sizeof(value));}
static std::vector<uint8_t> elf(std::initializer_list<uint8_t> code) {
    std::vector<uint8_t> v(4096+code.size());
    std::memcpy(v.data(),"\177ELF\2\1\1",7);
    put<uint16_t>(v,16,3);put<uint16_t>(v,18,62);put<uint32_t>(v,20,1);
    put<uint64_t>(v,24,4096);put<uint64_t>(v,32,64);put<uint16_t>(v,52,64);put<uint16_t>(v,54,56);put<uint16_t>(v,56,1);
    put<uint32_t>(v,64,1);put<uint32_t>(v,68,5);put<uint64_t>(v,72,4096);put<uint64_t>(v,80,4096);
    put<uint64_t>(v,96,code.size());put<uint64_t>(v,104,code.size());
    std::copy(code.begin(),code.end(),v.begin()+4096);return v;
}
static void check(std::vector<uint8_t> v,const char *reason) {
    astra_result r{};int ok=astra_run_elf(v.data(),v.size(),0,nullptr,&r);
    if(ok || r.ok || !std::strstr(r.error,reason)) { std::fprintf(stderr,"expected %s, got ok=%d %s\n",reason,ok,r.error);std::exit(1); }
    std::printf("clean failure: %s\n",r.error);
}
int main() {
    check({},"expected");
    check(elf({0x0f,0x0b}),"unimplemented");
    check(elf({0x48}),"truncated");
    check(elf({0x67,0x90}),"unimplemented");
    check(elf({0x0f,0xef,0xc0}),"unimplemented"); // MMX must not execute as XMM
    check(elf({0xf2,0x0f,0xd6,0xc0}),"unimplemented");
    // A null guest load followed by an exit; predecode must not mask the load.
    check(elf({0x31,0xc0,0x48,0x8b,0x00,0x0f,0x05}),"memory outside");
    // Guest stores to decoded instructions fail, never leave stale cache data.
    check(elf({0x48,0x8d,0x05,0xf9,0xff,0xff,0xff,0xc6,0x00,0x90,0x0f,0x05}),"self-modifying");
    check(elf({0x31,0xc9,0x48,0xf7,0xf1,0x0f,0x05}),"division by zero");
    auto bad=elf({0x0f,0x05});put<uint64_t>(bad,104,~uint64_t(0));check(bad,"invalid ELF");
    return 0;
}
