// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The x86 side-by-side assemblies the prefix gets (spec 02 Part H). The table itself is in
// wb_sxs_data.c, transcribed from upstream Wine 11.4.

#ifndef WB_SXS_H
#define WB_SXS_H

struct wb_sxs_file {
    const char *name;                    /* file name inside the assembly */
    const char *source;                  /* file in the bundle's i386-windows */
    const char *const *window_classes;   /* NULL-terminated, or NULL */
};

struct wb_sxs_assembly {
    const char *name;
    const char *token;                   /* publicKeyToken */
    const char *version;
    const struct wb_sxs_file *files;     /* files[0] decides whether the assembly is seeded */
    int file_count;
};

extern const struct wb_sxs_assembly wb_sxs_assemblies[];
extern const int wb_sxs_assembly_count;

#endif
