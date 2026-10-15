// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "TestCodecFixtures.h"
#include <CommonCrypto/CommonCryptor.h>
#include <lzma.h>
#include <stdint.h>
#include <string.h>
size_t bsteam_test_encrypt(const void *source,size_t length,const void *key,void *out,size_t capacity) {
    if (capacity < length+32) return 0;
    uint8_t iv[16]; for (int i=0;i<16;++i) iv[i]=(uint8_t)i;
    size_t n=0;
    if (CCCrypt(kCCEncrypt,kCCAlgorithmAES,kCCOptionECBMode,key,32,NULL,iv,16,out,16,&n)!=kCCSuccess || n!=16) return 0;
    if (CCCrypt(kCCEncrypt,kCCAlgorithmAES,kCCOptionPKCS7Padding,key,32,iv,source,length,(uint8_t *)out+16,capacity-16,&n)!=kCCSuccess) return 0;
    return n+16;
}
size_t bsteam_test_vz(const void *source,size_t length,void *out,size_t capacity) {
    if (capacity < 22 || length > UINT32_MAX) return 0;
    lzma_options_lzma options;
    if (lzma_lzma_preset(&options,6)) return 0;
    lzma_filter filters[2]={{LZMA_FILTER_LZMA1,&options},{LZMA_VLI_UNKNOWN,NULL}};
    uint8_t *bytes=out; memset(bytes,0,12); bytes[0]='V'; bytes[1]='Z';
    if (lzma_properties_encode(&filters[0],bytes+7)!=LZMA_OK) return 0;
    size_t used=0;
    if (lzma_raw_buffer_encode(filters,NULL,source,length,bytes+12,&used,capacity-22)!=LZMA_OK) return 0;
    size_t total=12+used+10; memset(bytes+12+used,0,10);
    for (int i=0;i<4;++i) bytes[total-6+i]=(uint8_t)(length>>(8*i));
    return total;
}
