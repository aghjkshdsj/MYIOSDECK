// SPDX-License-Identifier: GPL-3.0-or-later
// A second (third, ...) instance of a signed PE dylib (docs/NO_JIT_WINDOWS.md, step 4: one
// set of Wine DLLs per pseudo-process). dyld loads a dylib once per app, but each Windows
// process needs its own copy of every DLL's data. So we do what dyld does, from the same
// process: register the file's code signature (F_ADDFILESIGS_RETURN), map its signed __TEXT
// from the file r-x at a fresh address, and give it a fresh __DATA read from the file (the
// pristine, unrelocated data). Every RVA keeps its place, so the PE image inside is complete.
// pe2dylib's dylibs carry no dyld fixups (no pointers, no imports), so nothing else is needed.

#include "pe_instance.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jit_core.h"

#define PAGE16K 0x4000ull
#define ROUND16K(x) (((x) + PAGE16K - 1) & ~(PAGE16K - 1))

typedef struct { uint64_t vmaddr, vmsize, fileoff, filesize; int prot; } seg_t;

bool mid_pe_dylib_instance(void *handle, const char *path, mid_pe_instance *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    uint8_t *img = dlsym(handle, "myiosdeck_pe_image"), *data = dlsym(handle, "myiosdeck_pe_data");
    uint8_t *end = dlsym(handle, "myiosdeck_pe_image_end"), *teb = dlsym(handle, "myiosdeck_teb_offset");
    Dl_info info;
    if (!img || !data || !end || !teb) { snprintf(err, errlen, "missing myiosdeck_pe_* symbols"); return false; }
    if (!dladdr(img, &info) || !info.dli_fbase) { snprintf(err, errlen, "dladdr failed"); return false; }
    const struct mach_header_64 *mh = info.dli_fbase;
    if (mh->magic != MH_MAGIC_64) { snprintf(err, errlen, "not a 64-bit Mach-O"); return false; }

    seg_t segs[8];
    int nseg = 0, text = -1;
    uint32_t cs_off = 0, cs_size = 0;
    uint64_t lo = UINT64_MAX, hi = 0;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *c = (const void *)lc;
        if (c->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)c;
            if (strcmp(s->segname, "__PAGEZERO") && strcmp(s->segname, "__LINKEDIT") && s->vmsize && nseg < 8) {
                segs[nseg] = (seg_t){ s->vmaddr, s->vmsize, s->fileoff, s->filesize, s->initprot };
                if (s->fileoff == 0 && (s->initprot & VM_PROT_EXECUTE)) text = nseg;
                if (s->vmaddr < lo) lo = s->vmaddr;
                if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
                nseg++;
            }
        } else if (c->cmd == LC_CODE_SIGNATURE) {
            const struct linkedit_data_command *l = (const void *)c;
            cs_off = l->dataoff; cs_size = l->datasize;
        }
        lc += c->cmdsize;
    }
    if (text < 0) { snprintf(err, errlen, "no executable segment at file offset 0"); return false; }

    size_t span = (size_t)ROUND16K(hi - lo);
    uint8_t *r = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r == MAP_FAILED) { snprintf(err, errlen, "reserve 0x%zx: %s", span, strerror(errno)); return false; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errlen, "open: %s", strerror(errno)); munmap(r, span); return false; }

#ifdef F_ADDFILESIGS_RETURN
    // What dyld does before mapping code. Already registered by dyld's own load of this
    // file, so this is normally a no-op; logged either way for the record.
    if (cs_size) {
        fsignatures_t fs = { .fs_file_start = 0, .fs_blob_start = (void *)(uintptr_t)cs_off, .fs_blob_size = cs_size };
        int rc = fcntl(fd, F_ADDFILESIGS_RETURN, &fs);
        mid_log("[pe-instance] F_ADDFILESIGS_RETURN: %s (signature 0x%x+0x%x, covers to 0x%llx)", rc ? strerror(errno) : "ok",
                cs_off, cs_size, rc ? 0ull : (unsigned long long)fs.fs_file_start);
    }
#endif
    for (int i = 0; i < nseg; i++) {
        uint8_t *dst = r + (segs[i].vmaddr - lo);
        if (segs[i].prot & VM_PROT_EXECUTE) {
            void *m = mmap(dst, (size_t)ROUND16K(segs[i].filesize), PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE, fd,
                           (off_t)segs[i].fileoff);
            if (m == MAP_FAILED) {
                snprintf(err, errlen, "mapping signed code r-x from the file was refused: %s", strerror(errno));
                close(fd); munmap(r, span); return false;
            }
        } else {
            if (mprotect(dst, (size_t)ROUND16K(segs[i].vmsize), PROT_READ | PROT_WRITE)) {
                snprintf(err, errlen, "data mprotect: %s", strerror(errno)); close(fd); munmap(r, span); return false;
            }
            for (uint64_t done = 0; done < segs[i].filesize;) {
                ssize_t n = pread(fd, dst + done, (size_t)(segs[i].filesize - done), (off_t)(segs[i].fileoff + done));
                if (n <= 0) { snprintf(err, errlen, "data read: %s", strerror(errno)); close(fd); munmap(r, span); return false; }
                done += (uint64_t)n;
            }
        }
    }
    close(fd);

    // The first instance's addresses -> this instance's.
    uint8_t *first = (uint8_t *)mh - (segs[text].vmaddr - lo);
    out->image = r + (img - first);
    out->data = r + (data - first);
    out->end = r + (end - first);
    out->teb_offset = (uint64_t *)(void *)(r + (teb - first));
    out->tramps = dlsym(handle, "myiosdeck_x18_tramps") ? r + ((uint8_t *)dlsym(handle, "myiosdeck_x18_tramps") - first) : NULL;
    out->reserve = r;
    out->reserve_size = span;
    mid_log("[pe-instance] %s: new instance at %p (image %p, first instance image %p)", path, (void *)r, (void *)out->image,
            (void *)img);
    return true;
}
