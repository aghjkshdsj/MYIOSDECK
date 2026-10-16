// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

#ifndef WB_HOST_STUBS_H
#define WB_HOST_STUBS_H

#define STUB_MAX_CALLS 1024

/* kind 'K': a = vk; kind 'M': a = x, b = y. */
struct stub_input_call { char kind; int a, b; unsigned int flags, data; };

extern struct stub_input_call stub_calls[STUB_MAX_CALLS];
extern int stub_call_count;

#endif
