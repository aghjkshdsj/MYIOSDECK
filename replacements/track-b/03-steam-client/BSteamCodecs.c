// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "BSteamCodecs.h"
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <compression.h>
#include <zlib.h>
#include <lzma.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include <limits.h>

int bsteam_aes(const void *src, size_t length, const void *key, void *dst, size_t cap, size_t *written) {
    if (!src || !key || !dst || length < 32 || (length - 16) % 16 || cap < length - 16) return -1;
    unsigned char iv[16]; size_t count = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, NULL, src, 16, iv, sizeof iv, &count) != kCCSuccess || count != 16) return -1;
    count = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key, 32, iv, (const unsigned char *)src + 16, length - 16, dst, cap, &count) != kCCSuccess) return -1;
    if (written) *written = count; return 0;
}
int bsteam_inflate(const void *src, size_t length, void *dst, size_t cap, int gzip, size_t *written) {
    if (length > UINT_MAX || cap > UINT_MAX || !src || !dst) return -1;
    z_stream stream = {0};
    stream.next_in = (Bytef *)src; stream.avail_in = (uInt)length; stream.next_out = dst; stream.avail_out = (uInt)cap;
    if (inflateInit2(&stream, gzip ? MAX_WBITS + 16 : -MAX_WBITS) != Z_OK) return -1;
    int result = inflate(&stream, Z_FINISH);
    if (written) *written = stream.total_out;
    inflateEnd(&stream);
    return result == Z_STREAM_END ? 0 : -1;
}
int bsteam_lzma(const void *src, size_t length, const void *props, void *dst, size_t cap, size_t *written) {
    if (!src || !props || !dst || cap > 64u * 1024u * 1024u) return -1;
    lzma_filter filters[2] = {{LZMA_FILTER_LZMA1, NULL}, {LZMA_VLI_UNKNOWN, NULL}};
    if (lzma_properties_decode(&filters[0], NULL, props, 5) != LZMA_OK) return -1;
    lzma_options_lzma *options = filters[0].options;
    int result = -1;
    if (options && options->dict_size <= 64u * 1024u * 1024u) {
        size_t input = 0, output = 0;
        lzma_ret status = lzma_raw_buffer_decode(filters, NULL, src, &input, length, dst, &output, cap);
        if (status == LZMA_OK) { result = 0; if (written) *written = output; }
    }
    lzma_filters_free(filters, NULL); return result;
}
int bsteam_apple_lzma(const void *src, size_t length, void *dst, size_t cap, size_t *written) {
    size_t count = compression_decode_buffer(dst, cap, src, length, NULL, COMPRESSION_LZMA);
    if (!count) return -1;
    if (written) *written = count; return 0;
}
void bsteam_sha1(const void *src, size_t length, uint8_t dst[20]) {
    CC_SHA1_CTX ctx; CC_SHA1_Init(&ctx);
    const unsigned char *bytes = src;
    while (length) { CC_LONG n = length > UINT_MAX ? UINT_MAX : (CC_LONG)length; CC_SHA1_Update(&ctx, bytes, n); bytes += n; length -= n; }
    CC_SHA1_Final(dst, &ctx);
}
uint32_t bsteam_adler(const void *src, size_t length) {
    const unsigned char *bytes = src; uLong value = 0;
    while (length) { uInt n = length > UINT_MAX ? UINT_MAX : (uInt)length; value = adler32(value, bytes, n); bytes += n; length -= n; }
    return (uint32_t)value;
}

// The vendor files remain byte-for-byte unchanged. Contain their documented
// exit-on-error behavior and reclaim every scratch allocation after a failure.
struct bsteam_allocation { void *pointer; size_t size; struct bsteam_allocation *next; };
static _Thread_local struct bsteam_allocation *bsteam_allocations;
static _Thread_local size_t bsteam_allocated, bsteam_allocation_count;
static _Thread_local jmp_buf bsteam_decode_error;
static void *bsteam_zalloc(size_t size) {
    if (size > 256u * 1024u * 1024u - bsteam_allocated || bsteam_allocation_count >= 65536) return NULL;
    void *pointer = malloc(size ? size : 1);
    if (!pointer) return NULL;
    struct bsteam_allocation *node = malloc(sizeof *node);
    if (!node) { free(pointer); return NULL; }
    *node = (struct bsteam_allocation){pointer, size, bsteam_allocations}; bsteam_allocations = node;
    bsteam_allocated += size; ++bsteam_allocation_count; return pointer;
}
static void *bsteam_zcalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *p = bsteam_zalloc(count * size); if (p) memset(p, 0, count * size); return p;
}
static void bsteam_zfree(void *pointer) {
    if (!pointer) return;
    struct bsteam_allocation **link = &bsteam_allocations;
    while (*link && (*link)->pointer != pointer) link = &(*link)->next;
    if (!*link) return;
    struct bsteam_allocation *node = *link; *link = node->next;
    bsteam_allocated -= node->size; --bsteam_allocation_count; free(node->pointer); free(node);
}
static _Noreturn void bsteam_zerror(int status) { (void)status; longjmp(bsteam_decode_error, 1); }
#define ZDEC_NO_MESSAGE
#define ZDEC_NO_DICTIONARY
#define malloc bsteam_zalloc
#define calloc bsteam_zcalloc
#define free bsteam_zfree
#define exit bsteam_zerror
#define ZSTD_decompress bsteam_zstd_impl
#define ZSTD_decompress_with_dict bsteam_zstd_dict_impl
#define ZSTD_get_decompressed_size bsteam_zstd_size_impl
#define create_dictionary bsteam_zstd_create_dictionary
#define parse_dictionary bsteam_zstd_parse_dictionary
#define free_dictionary bsteam_zstd_free_dictionary
#include "vendor/zstd_decompress.c"
#undef malloc
#undef calloc
#undef free
#undef exit

int bsteam_zstd(const void *src, size_t length, void *dst, size_t cap, size_t *written) {
    if (!src || !dst || cap > 64u * 1024u * 1024u || bsteam_allocations) return -1;
    int result = -1;
    if (!setjmp(bsteam_decode_error)) {
        size_t count = bsteam_zstd_impl(dst, cap, src, length);
        if (count <= cap) { if (written) *written = count; result = 0; }
    }
    while (bsteam_allocations) bsteam_zfree(bsteam_allocations->pointer);
    return result;
}
