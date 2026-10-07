// SPDX-License-Identifier: GPL-3.0-or-later
// Another instance of a signed PE dylib (see pe_instance.c): one Wine DLL set per process.

#ifndef MYIOSDECK_PE_INSTANCE_H
#define MYIOSDECK_PE_INSTANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *image, *data, *end;   // as myiosdeck_pe_image / _data / _image_end, in this instance
    uint64_t *teb_offset;          // this instance's TEB TSD-offset word
    uint8_t *tramps;               // its x18 trampolines
    void *reserve;                 // the whole mapping
    size_t reserve_size;
} mid_pe_instance;

/// Map a new instance of the PE dylib at `path`, already loaded once with dlopen (`handle`):
/// signed code mapped r-x from the file, fresh data from the file (unrelocated, no imports
/// resolved). false with `err` when iOS refuses the mapping.
bool mid_pe_dylib_instance(void *handle, const char *path, mid_pe_instance *out, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
