// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Zstandard decoding for Steam chunks (spec 03 §D6), using Meta's zstd educational decoder
// (v1.5.6), vendored unchanged in ../Vendor/zstd/ under its BSD licence.
//
// That decoder calls exit(1) on bad input ("a production library should propagate error
// codes", its own comment says). The spec forbids trapping on network data, so this file
// compiles the unchanged decoder with four macros in front of it:
//   - exit(code) jumps back to steamcodec_zstd() with longjmp, which returns an error;
//   - malloc/calloc/free go through a per-thread list, so everything the decoder had
//     allocated when it bailed out is freed;
//   - its public names are renamed, so they can't clash with any other zstd in the app;
//   - ZDEC_NO_MESSAGE silences its stderr messages.
// Do NOT add Vendor/zstd/zstd_decompress.c to the target's sources; this file includes it.

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "SteamCodecs.h"

// MARK: allocation tracking and the bail-out jump (one decode per thread at a time)

typedef union steamzstd_block {
    struct {
        union steamzstd_block *prev;
        union steamzstd_block *next;
    } link;
    max_align_t align;   // keeps the memory after the header aligned like malloc's
} steamzstd_block;

static _Thread_local jmp_buf *steamzstd_jump;
static _Thread_local steamzstd_block *steamzstd_live;

static void *steamzstd_malloc(size_t n)
{
    if (n > SIZE_MAX - sizeof(steamzstd_block))
        return NULL;
    steamzstd_block *b = malloc(sizeof(steamzstd_block) + n);
    if (b == NULL)
        return NULL;
    b->link.prev = NULL;
    b->link.next = steamzstd_live;
    if (steamzstd_live != NULL)
        steamzstd_live->link.prev = b;
    steamzstd_live = b;
    return b + 1;
}

static void *steamzstd_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
        return NULL;
    void *p = steamzstd_malloc(count * size);
    if (p != NULL)
        memset(p, 0, count * size);
    return p;
}

static void steamzstd_free(void *p)
{
    if (p == NULL)
        return;
    steamzstd_block *b = (steamzstd_block *)p - 1;
    if (b->link.prev != NULL)
        b->link.prev->link.next = b->link.next;
    else
        steamzstd_live = b->link.next;
    if (b->link.next != NULL)
        b->link.next->link.prev = b->link.prev;
    free(b);
}

static void steamzstd_release_all(void)
{
    steamzstd_block *b = steamzstd_live;
    while (b != NULL) {
        steamzstd_block *next = b->link.next;
        free(b);
        b = next;
    }
    steamzstd_live = NULL;
}

__attribute__((noreturn)) static void steamzstd_fail(void)
{
    if (steamzstd_jump != NULL)
        longjmp(*steamzstd_jump, 1);
    abort();   // unreachable: the decoder only runs inside steamcodec_zstd*()
}

// MARK: the unchanged decoder

#define ZDEC_NO_MESSAGE 1
#define malloc(n) steamzstd_malloc(n)
#define calloc(n, s) steamzstd_calloc((n), (s))
#define free(p) steamzstd_free(p)
#define exit(code) steamzstd_fail()
#define ZSTD_decompress steamzstd_impl_decompress
#define ZSTD_decompress_with_dict steamzstd_impl_decompress_with_dict
#define ZSTD_get_decompressed_size steamzstd_impl_get_decompressed_size
#define create_dictionary steamzstd_impl_create_dictionary
#define parse_dictionary steamzstd_impl_parse_dictionary
#define free_dictionary steamzstd_impl_free_dictionary

#include "../Vendor/zstd/zstd_decompress.c"

#undef malloc
#undef calloc
#undef free
#undef exit

// MARK: entry points

int steamcodec_zstd(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (out_len == NULL || src == NULL || dst == NULL)
        return STEAMCODEC_ERROR;
    *out_len = 0;

    jmp_buf env;
    steamzstd_live = NULL;
    steamzstd_jump = &env;
    if (setjmp(env) != 0) {
        steamzstd_jump = NULL;
        steamzstd_release_all();
        return STEAMCODEC_ERROR;
    }
    size_t n = steamzstd_impl_decompress(dst, dst_cap, src, src_len);
    steamzstd_jump = NULL;
    steamzstd_release_all();
    if (n > dst_cap)
        return STEAMCODEC_ERROR;
    *out_len = n;
    return STEAMCODEC_OK;
}

int steamcodec_zstd_content_size(const uint8_t *src, size_t src_len, uint64_t *size)
{
    if (size == NULL || src == NULL)
        return STEAMCODEC_ERROR;
    *size = 0;

    jmp_buf env;
    steamzstd_live = NULL;
    steamzstd_jump = &env;
    if (setjmp(env) != 0) {
        steamzstd_jump = NULL;
        steamzstd_release_all();
        return STEAMCODEC_ERROR;
    }
    size_t n = steamzstd_impl_get_decompressed_size(src, src_len);
    steamzstd_jump = NULL;
    steamzstd_release_all();
    if (n == (size_t)-1)
        return STEAMCODEC_ERROR;
    *size = (uint64_t)n;
    return STEAMCODEC_OK;
}
