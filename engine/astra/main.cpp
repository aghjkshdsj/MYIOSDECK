// SPDX-License-Identifier: GPL-3.0-or-later
#include "astra.h"
#include <cstdio>
#include <vector>
int main(int argc,char **argv) {
    if(argc<2){std::fprintf(stderr,"usage: astra program.elf [arguments...]\n");return 2;}
    FILE *f=std::fopen(argv[1],"rb");if(!f){std::perror(argv[1]);return 2;}
    std::fseek(f,0,SEEK_END);long n=std::ftell(f);std::rewind(f);
    if(n<0 || n>256*1024*1024){std::fclose(f);return 2;}
    std::vector<uint8_t> bytes(n);
    if(std::fread(bytes.data(),1,bytes.size(),f)!=bytes.size()){std::fclose(f);return 2;}
    std::fclose(f);astra_result result{};
    astra_run_elf(bytes.data(),bytes.size(),argc-1,(const char *const *)(argv+1),&result);
    std::fwrite(result.output,1,result.output_len,stdout);
    std::fprintf(stderr,"%s: ok=%d exit=%lld seconds=%.6f blocks=%llu syscalls=%llu %s\n",
        astra_version(),result.ok,result.exit_code,result.seconds,result.blocks,result.syscalls,result.error);
    return result.ok?(int)result.exit_code:1;
}
