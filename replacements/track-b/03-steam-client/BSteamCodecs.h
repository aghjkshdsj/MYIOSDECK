// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#ifndef BSTEAM_CODECS_H
#define BSTEAM_CODECS_H
#include <stddef.h>
#include <stdint.h>
int bsteam_aes(const void *source, size_t length, const void *key, void *destination, size_t capacity, size_t *written);
int bsteam_inflate(const void *source, size_t length, void *destination, size_t capacity, int gzip, size_t *written);
int bsteam_lzma(const void *source, size_t length, const void *properties, void *destination, size_t capacity, size_t *written);
int bsteam_apple_lzma(const void *source, size_t length, void *destination, size_t capacity, size_t *written);
int bsteam_zstd(const void *source, size_t length, void *destination, size_t capacity, size_t *written);
void bsteam_sha1(const void *source, size_t length, uint8_t destination[20]);
uint32_t bsteam_adler(const void *source, size_t length);
#endif
