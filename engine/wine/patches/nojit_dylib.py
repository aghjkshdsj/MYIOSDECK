#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""No-JIT step B (docs/NO_JIT_WINDOWS.md): Madeira's ntdll unix side maps PE images from the
signed dylibs engine/pedylib built, instead of copying them into the JIT pool.

Active only when WINE_IOS_NOJIT=1 (the app sets it, with MYIOSDECK_PE_DIR, when it boots
Wine without JIT); with JIT, nothing changes. Three hooks in build/ntdll-unix/virtual_ios.c:

  virtual_map_image   an image whose file name has <PE dir>/lib<name>.dylib is dlopen'ed
                      and registered as a view at the dylib's address (like Wine's own
                      virtual_create_builtin_view), with section protections and ARM64EC
                      code ranges set as map_image_into_view would. Relocation is left to
                      Wine (the server reports STATUS_IMAGE_NOT_AT_BASE): virtual_relocate_module
                      for ntdll, perform_relocations for the rest, once each, since the
                      read-only header keeps its preferred ImageBase.
  mprotect_exec       inside those images, code pages (signed __TEXT) are already r-x and
                      cannot change: requests for them are no-ops. Data pages (__DATA) get
                      the request without PROT_EXEC. Nothing reaches the JIT pool.
  eager JIT copy      skipped for these images.

Usage: nojit_dylib.py <madeira checkout>. Idempotent (marker line).
"""
import sys

MARK = "/* MYIOSDECK nojit-dylib */"

HELPERS = MARK + r'''
/* MYIOSDECK no-JIT: PE images from signed dylibs (engine/wine/patches/nojit_dylib.py). */
#include <dlfcn.h>

struct ios_nojit_image
{
    char  name[64];
    char *base;   /* PE image (headers + code in signed __TEXT) */
    char *data;   /* first __DATA byte: everything below is code/headers, never written */
    size_t size;
    int   mapped; /* a view was created for it */
};
static struct ios_nojit_image ios_nojit_images[512];
static int ios_nojit_count;
static pthread_mutex_t ios_nojit_lock = PTHREAD_MUTEX_INITIALIZER;

static int ios_nojit_enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *e = getenv( "WINE_IOS_NOJIT" );
        on = e && *e == '1';
    }
    return on;
}

/* Every [nojit] line goes to stderr AND is appended, unbuffered, to MYIOSDECK_NOJIT_TRACE
 * (Documents), so it survives a crash: stderr reaches the app's log through a pipe whose
 * tail is lost when the process dies (build 47). */
static void __attribute__((format(printf, 1, 2))) ios_nojit_trace( const char *fmt, ... )
{
    static int fd = -2;
    char line[512];
    va_list ap;
    int n;

    va_start( ap, fmt );
    n = vsnprintf( line, sizeof(line), fmt, ap );
    va_end( ap );
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    write( 2, line, n );
    if (fd == -2)
    {
        const char *path = getenv( "MYIOSDECK_NOJIT_TRACE" );
        fd = path ? open( path, O_WRONLY | O_CREAT | O_APPEND, 0644 ) : -1;
    }
    if (fd >= 0) write( fd, line, n );
}

/* The no-JIT image containing addr, or NULL. Entries are only appended (lock-free read). */
static const struct ios_nojit_image *ios_nojit_find( const void *addr )
{
    int i, n = __atomic_load_n( &ios_nojit_count, __ATOMIC_ACQUIRE );
    for (i = 0; i < n; i++)
    {
        const struct ios_nojit_image *im = &ios_nojit_images[i];
        if ((const char *)addr >= im->base && (const char *)addr < im->base + im->size) return im;
    }
    return NULL;
}

/* mprotect_exec hook: returns 1 (and the result in *ret) when the range is in a no-JIT image. */
static int ios_nojit_mprotect( void *base, size_t size, int unix_prot, int *ret )
{
    const struct ios_nojit_image *im;
    char *p = base, *end = p + size;

    if (!ios_nojit_enabled()) return 0;
    {
        static int announced;
        if (!announced++) ios_nojit_trace( "[nojit] ntdll unix side: no-JIT mode active (first protection change)\n" );
    }
    if (!(im = ios_nojit_find( base )))
    {
        /* Executable memory outside the signed images would need the JIT pool, which a
         * no-JIT session does not have: hand out the memory without PROT_EXEC and record
         * where, so a later crash at that address is explained (step C: no runtime code). */
        static int n;
        if (!(unix_prot & PROT_EXEC)) return 0;
        if (n++ < 64)
            ios_nojit_trace( "[nojit] EXEC requested outside the signed images at %p+0x%lx (prot %d): "
                             "given without execute permission\n", base, (unsigned long)size, unix_prot );
        *ret = mprotect( base, size, unix_prot & ~PROT_EXEC );
        return 1;
    }
    *ret = 0;
    if (end > im->data)
    {
        char *from = p > im->data ? p : im->data;
        *ret = mprotect( from, end - from, unix_prot & ~PROT_EXEC );
    }
    return 1;
}

/* Find (and dlopen once) the signed dylib for an image. Called outside virtual_mutex. */
static struct ios_nojit_image *ios_nojit_load( const UNICODE_STRING *nt_name )
{
    extern int ios_teb_tls_slot_offset;
    const char *dir = getenv( "MYIOSDECK_PE_DIR" );
    char name[64], path[1024];
    unsigned int i, start = 0, len = nt_name->Length / sizeof(WCHAR), n = 0;
    struct ios_nojit_image *im = NULL;
    void *h;
    char *img, *data, *end;
    uint64_t *teb_off;

    if (!dir) return NULL;
    for (i = 0; i < len; i++) if (nt_name->Buffer[i] == '\\' || nt_name->Buffer[i] == '/') start = i + 1;
    for (i = start; i < len && n < sizeof(name) - 1; i++)
    {
        WCHAR c = nt_name->Buffer[i];
        if (c >= 0x80) return NULL;
        name[n++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    name[n] = 0;
    if (!n) return NULL;

    pthread_mutex_lock( &ios_nojit_lock );
    for (i = 0; i < (unsigned int)ios_nojit_count; i++)
        if (!strcmp( ios_nojit_images[i].name, name )) { im = &ios_nojit_images[i]; break; }
    if (im)
    {
        pthread_mutex_unlock( &ios_nojit_lock );
        if (im->mapped)
        {
            ios_nojit_trace( "[nojit] %s: mapped again; one signed image per DLL so far -- not supported yet\n", name );
            return NULL;
        }
        return im;
    }
    snprintf( path, sizeof(path), "%s/lib%s.dylib", dir, name );
    if (access( path, R_OK ) || ios_nojit_count >= (int)(sizeof(ios_nojit_images) / sizeof(ios_nojit_images[0])))
    {
        pthread_mutex_unlock( &ios_nojit_lock );
        ios_nojit_trace( "[nojit] %s: no signed dylib (it would need the JIT pool)\n", name );
        return NULL;
    }
    h = dlopen( path, RTLD_NOW | RTLD_LOCAL );
    img = h ? dlsym( h, "myiosdeck_pe_image" ) : NULL;
    data = h ? dlsym( h, "myiosdeck_pe_data" ) : NULL;
    end = h ? dlsym( h, "myiosdeck_pe_image_end" ) : NULL;
    teb_off = h ? dlsym( h, "myiosdeck_teb_offset" ) : NULL;
    if (!img || !data || !end || !teb_off)
    {
        pthread_mutex_unlock( &ios_nojit_lock );
        ios_nojit_trace( "[nojit] %s: dlopen %s failed: %s\n", name, path, h ? "missing symbols" : dlerror() );
        return NULL;
    }
    *teb_off = ios_teb_tls_slot_offset;   /* x18 trampolines read the TEB from this TSD slot */
    im = &ios_nojit_images[ios_nojit_count];
    snprintf( im->name, sizeof(im->name), "%s", name );
    im->base = img;
    im->data = data;
    im->size = end - img;
    __atomic_store_n( &ios_nojit_count, ios_nojit_count + 1, __ATOMIC_RELEASE );
    pthread_mutex_unlock( &ios_nojit_lock );
    ios_nojit_trace( "[nojit] %s: signed image %p+0x%lx (code+headers 0x%lx), TEB slot offset 0x%x\n",
             name, img, (unsigned long)im->size, (unsigned long)(data - img), ios_teb_tls_slot_offset );
    if (!ios_teb_tls_slot_offset) ios_nojit_trace( "[nojit] WARNING: TEB TSD slot not known yet\n" );
    return im;
}

'''

MAP_VIEW = MARK + r'''
/* MYIOSDECK no-JIT: register a view over a signed dylib image (see ios_nojit_load). */
static NTSTATUS ios_nojit_map_view( struct file_view **view_ret, struct ios_nojit_image *im,
                                    struct pe_image_info *image_info )
{
    char *base = im->base;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );
    IMAGE_DATA_DIRECTORY *dir;
    struct file_view *view;
    SIZE_T total_size = image_info->map_size;
    NTSTATUS status;
    int i;

    if (total_size > im->size)
    {
        ios_nojit_trace( "[nojit] %s: map size 0x%lx > signed image 0x%lx\n", im->name,
                 (unsigned long)total_size, (unsigned long)im->size );
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if ((status = create_view( &view, base, total_size, SEC_IMAGE | SEC_FILE | VPROT_SYSTEM |
                               VPROT_COMMITTED | VPROT_READ | VPROT_WRITECOPY | VPROT_EXEC )))
    {
        ios_nojit_trace( "[nojit] %s: create_view %p+0x%lx failed %x\n", im->name, base,
                 (unsigned long)total_size, (unsigned)status );
        return status;
    }

    /* ARM64EC code ranges and the redirected entry point, before any relocation (the
     * metadata pointer and the header's ImageBase must still agree). */
    if (image_info->machine == IMAGE_FILE_MACHINE_AMD64 &&
        (dir = get_data_dir( nt, total_size, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG )))
        update_arm64ec_ranges( view, nt, dir, &image_info->entry_point );

    /* No relocation here. The wineserver answers this view with STATUS_IMAGE_NOT_AT_BASE
     * (it is not at the preferred base), and Wine relocates exactly once from the read-only
     * header's ImageBase: load_ntdll calls virtual_relocate_module for ntdll, the PE
     * loader's perform_relocations does every other image. Relocating here as well applied
     * ntdll's pointers twice (build 49). Both paths change protections through
     * mprotect_exec, where code pages are no-ops and data pages are writable. */

    set_vprot( view, base, ROUND_SIZE( 0, nt->OptionalHeader.SizeOfHeaders, page_mask ),
               VPROT_COMMITTED | VPROT_READ );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        SIZE_T size;
        BYTE vprot = VPROT_COMMITTED;

        if (sec[i].Misc.VirtualSize)
            size = ROUND_SIZE( sec[i].VirtualAddress, sec[i].Misc.VirtualSize, page_mask );
        else
            size = ROUND_SIZE( sec[i].VirtualAddress, sec[i].SizeOfRawData, page_mask );
        if (!size) continue;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_READ)    vprot |= VPROT_READ;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)   vprot |= VPROT_WRITECOPY;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) vprot |= VPROT_EXEC;
        set_vprot( view, base + sec[i].VirtualAddress, size, vprot );
    }
    im->mapped = 1;
    *view_ret = view;
    ios_nojit_trace( "[nojit] %s: mapped from its signed dylib at %p (entry +0x%x)\n", im->name, base,
             (unsigned)image_info->entry_point );
    return STATUS_SUCCESS;
}

'''


def sub(text, old, new, what):
    n = text.count(old)
    if n != 1:
        sys.exit("nojit_dylib.py: anchor for %s found %d times" % (what, n))
    return text.replace(old, new)


def main():
    path = sys.argv[1] + "/build/ntdll-unix/virtual_ios.c"
    s = open(path).read()
    if MARK in s:
        print("nojit_dylib: already applied")
        return

    # 1. helpers + the mprotect_exec hook
    head = "static inline int mprotect_exec( void *base, size_t size, int unix_prot )\n{\n#ifdef WINE_IOS\n"
    s = sub(s, head, HELPERS + head +
            "    { int nj_ret; if (ios_nojit_mprotect( base, size, unix_prot, &nj_ret )) return nj_ret; }\n",
            "mprotect_exec")

    # 2. the map hook in virtual_map_image
    vmi = "static NTSTATUS virtual_map_image( HANDLE mapping, void **addr_ptr, SIZE_T *size_ptr, HANDLE shared_file,"
    s = sub(s, vmi, MAP_VIEW + vmi, "virtual_map_image definition")
    enter = ("    server_enter_uninterrupted_section( &virtual_mutex, &sigset );\n\n"
             "    status = map_image_view( &view, image_info, size, limit_low, limit_high, alloc_type, is_builtin );\n")
    s = sub(s, enter,
            "    struct ios_nojit_image *nojit = NULL;\n"
            "    if (ios_nojit_enabled() && nt_name && !offset) nojit = ios_nojit_load( nt_name );\n\n"
            "    server_enter_uninterrupted_section( &virtual_mutex, &sigset );\n\n"
            "    if (nojit)\n"
            "    {\n"
            "        status = ios_nojit_map_view( &view, nojit, image_info );\n"
            "        if (status) goto done;\n"
            "        goto nojit_mapped;\n"
            "    }\n"
            "    status = map_image_view( &view, image_info, size, limit_low, limit_high, alloc_type, is_builtin );\n",
            "map_image_view call")
    into = ("    status = map_image_into_view( view, nt_name, unix_fd, image_info, machine, shared_fd, needs_close );\n"
            "    if (status == STATUS_SUCCESS)\n")
    s = sub(s, into,
            "    status = map_image_into_view( view, nt_name, unix_fd, image_info, machine, shared_fd, needs_close );\n"
            "nojit_mapped:\n"
            "    if (status == STATUS_SUCCESS)\n",
            "map_image_into_view call")
    eager = "        if (is_builtin && !offset)\n        {\n            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)view->base;\n"
    s = sub(s, eager, eager.replace("if (is_builtin && !offset)", "if (is_builtin && !offset && !nojit)"),
            "eager JIT copy")
    open(path, "w").write(s)
    print("nojit_dylib: applied to %s" % path)


if __name__ == "__main__":
    main()
