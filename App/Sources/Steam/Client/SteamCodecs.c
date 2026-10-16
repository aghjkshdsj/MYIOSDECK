// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Raw deflate and raw LZMA1 decoding for the Steam client (spec 03 §C5, §D4, §D6).
// Written from the public zlib manual (inflateInit2 with negative window bits for raw
// deflate) and the liblzma API documentation (lzma_alone_decoder, lzma_code). The app links
// -lz and -llzma.

#include "SteamCodecs.h"

#include <lzma.h>
#include <string.h>
#include <zlib.h>

int steamcodec_inflate_raw(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (out_len == NULL || src == NULL || dst == NULL)
        return STEAMCODEC_ERROR;
    *out_len = 0;
    if (src_len > 0xFFFFFFFFu || dst_cap > 0xFFFFFFFFu)
        return STEAMCODEC_ERROR;

    z_stream s;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK)
        return STEAMCODEC_ERROR;
    s.next_in = (Bytef *)src;
    s.avail_in = (uInt)src_len;
    s.next_out = (Bytef *)dst;
    s.avail_out = (uInt)dst_cap;

    int rc = inflate(&s, Z_FINISH);
    size_t produced = dst_cap - (size_t)s.avail_out;
    uInt room_left = s.avail_out;
    inflateEnd(&s);

    if (rc == Z_STREAM_END) {
        *out_len = produced;
        return STEAMCODEC_OK;
    }
    if ((rc == Z_BUF_ERROR || rc == Z_OK) && room_left == 0)
        return STEAMCODEC_TOO_BIG;
    return STEAMCODEC_ERROR;
}

int steamcodec_lzma_raw(const uint8_t *props5, const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len)
{
    if (props5 == NULL || src == NULL || dst == NULL || dst_len == 0)
        return STEAMCODEC_ERROR;

    // An .lzma ("alone") header: the 5 property bytes, then the decoded size as a u64 LE.
    // With the size known, the decoder stops there whether or not the stream has an end mark.
    uint8_t header[13];
    memcpy(header, props5, 5);
    uint64_t n = (uint64_t)dst_len;
    for (int i = 0; i < 8; i++)
        header[5 + i] = (uint8_t)(n >> (8 * i));

    lzma_stream s = LZMA_STREAM_INIT;
    // 256 MiB bounds the dictionary an untrusted header can ask for.
    if (lzma_alone_decoder(&s, (uint64_t)256 << 20) != LZMA_OK)
        return STEAMCODEC_ERROR;

    s.next_in = header;
    s.avail_in = sizeof(header);
    s.next_out = dst;
    s.avail_out = dst_len;
    lzma_ret rc = lzma_code(&s, LZMA_RUN);
    if (rc != LZMA_OK || s.avail_in != 0) {
        lzma_end(&s);
        return STEAMCODEC_ERROR;
    }

    s.next_in = src;
    s.avail_in = src_len;
    rc = lzma_code(&s, LZMA_FINISH);
    size_t produced = dst_len - s.avail_out;
    lzma_end(&s);

    if (rc == LZMA_STREAM_END && produced == dst_len)
        return STEAMCODEC_OK;
    return STEAMCODEC_ERROR;
}
