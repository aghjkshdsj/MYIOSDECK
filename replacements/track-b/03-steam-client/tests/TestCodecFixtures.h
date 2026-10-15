// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#ifndef BSTEAM_TEST_FIXTURES_H
#define BSTEAM_TEST_FIXTURES_H
#include <stddef.h>
size_t bsteam_test_encrypt(const void *source, size_t length, const void *key, void *out, size_t capacity);
size_t bsteam_test_vz(const void *source, size_t length, void *out, size_t capacity);
#endif
