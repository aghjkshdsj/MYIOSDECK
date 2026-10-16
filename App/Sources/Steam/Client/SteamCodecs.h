// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// C codecs for the Steam client (spec 03 §C5, §D4, §D6): raw deflate (zlib), raw LZMA1
// (liblzma) and Zstandard (the vendored educational decoder, wrapped so bad data returns an
// error). Add this header to the app's Swift bridging header:  #include "SteamCodecs.h"

#ifndef STEAM_CODECS_H
#define STEAM_CODECS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STEAMCODEC_OK 0
#define STEAMCODEC_ERROR (-1)
#define STEAMCODEC_TOO_BIG 1

/// Inflates raw deflate data (RFC 1951, no zlib or gzip wrapper) into dst.
/// Returns STEAMCODEC_OK with *out_len set, STEAMCODEC_TOO_BIG when dst_cap is too small,
/// or STEAMCODEC_ERROR for bad data. src_len and dst_cap must each be below 4 GiB.
int steamcodec_inflate_raw(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap, size_t *out_len);

/// Decodes a raw LZMA1 stream whose 5 property bytes are `props5` and whose decoded size is
/// exactly dst_len. Returns STEAMCODEC_OK only when exactly dst_len bytes were produced.
int steamcodec_lzma_raw(const uint8_t *props5, const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_len);

/// Decodes one Zstandard frame into dst (at most dst_cap bytes). Returns STEAMCODEC_OK with
/// *out_len set, or STEAMCODEC_ERROR for bad data or a too small dst. Thread-safe.
int steamcodec_zstd(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap, size_t *out_len);

/// The content size a Zstandard frame header declares. STEAMCODEC_ERROR when unknown or bad.
int steamcodec_zstd_content_size(const uint8_t *src, size_t src_len, uint64_t *size);

#ifdef __cplusplus
}
#endif

#endif /* STEAM_CODECS_H */
