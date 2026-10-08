// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../guest/bench_kernels.h"
static unsigned long long ns(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;
}
int main(int argc,char **argv) {
    if(argc<2)return 2;
    for(unsigned i=0;i<BK_KERNEL_COUNT;i++) {
        const bk_kernel *k=&bk_kernels[i];if(strcmp(argv[1],k->name))continue;
        unsigned scale=argc>2?(unsigned)strtoul(argv[2],0,10):k->scale;
        if(!scale)scale=k->scale;
        // Same warm-up and timing boundaries as the unmodified guest driver.
        volatile bk_u64 warm=k->fn(scale/8?scale/8:1);(void)warm;
        unsigned long long t0=ns();bk_u64 sum=k->fn(scale);unsigned long long t1=ns();
        printf("RESULT kernel=%s ns=%llu sum=%llu\n",k->name,t1-t0,sum);return 0;
    }
    return 3;
}
