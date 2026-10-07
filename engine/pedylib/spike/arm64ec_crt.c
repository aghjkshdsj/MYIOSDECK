/*
 * ARM64EC support for a -nostdlib DLL: the CHPE metadata, the __os_arm64x_* dispatch
 * pointers the loader fills, __icall_helper_arm64ec, and the load config that points
 * the loader at the metadata. Wine DLLs get exactly this from libs/winecrt0/arm64ec.c
 * and winebuild's output_load_config (tools/winebuild/spec32.c); this is that code,
 * minus the Wine headers and SEH annotations.
 *
 * Copyright 2023 Jacek Caban for CodeWeavers (libs/winecrt0/arm64ec.c)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifdef __arm64ec__

void *__os_arm64x_check_icall_cfg = 0;
void *__os_arm64x_dispatch_call_no_redirect = 0;
void *__os_arm64x_dispatch_fptr = 0;
void *__os_arm64x_dispatch_ret = 0;
void *__os_arm64x_get_x64_information = 0;
void *__os_arm64x_set_x64_information = 0;

void *__os_arm64x_helper3 = 0;
void *__os_arm64x_helper4 = 0;
void *__os_arm64x_helper5 = 0;
void *__os_arm64x_helper6 = 0;
void *__os_arm64x_helper7 = 0;
void *__os_arm64x_helper8 = 0;

void *__guard_check_icall_fptr = 0;
void *__guard_dispatch_icall_fptr = 0;

asm(".section .data,\"drw\"\n"
    ".balign 8\n"
    ".globl __os_arm64x_dispatch_icall\n"
    "__os_arm64x_dispatch_icall:\n"
    ".globl __os_arm64x_check_icall\n"
    "__os_arm64x_check_icall:\n"
    ".xword 0\n"
    ".globl __os_arm64x_dispatch_call\n"
    "__os_arm64x_dispatch_call:\n"
    ".globl __os_arm64x_check_call\n"
    "__os_arm64x_check_call:\n"
    ".xword 0\n");

asm(".text\n"
    ".def __icall_helper_arm64ec\n"
    ".scl 2\n"
    ".type 32\n"
    ".endef\n"
    ".globl __icall_helper_arm64ec\n"
    ".p2align 2\n"
    "__icall_helper_arm64ec:\n"
    "stp x29, x30, [sp, #-0x10]!\n"
    "mov x29, sp\n"
    "adrp x16, __os_arm64x_dispatch_icall\n"
    "ldr x16, [x16, #:lo12:__os_arm64x_dispatch_icall]\n"
    "blr x16\n"
    "ldp x29, x30, [sp], #0x10\n"
    "br x11\n");

asm(".section .rdata,\"dr\"\n"
    ".globl __chpe_metadata\n"
    ".balign 4\n"
    "__chpe_metadata:\n"
    ".word 2\n"
    ".rva __hybrid_code_map\n"
    ".word __hybrid_code_map_count\n"
    ".rva __x64_code_ranges_to_entry_points\n"
    ".rva __arm64x_redirection_metadata\n"
    ".rva __os_arm64x_dispatch_call_no_redirect\n"
    ".rva __os_arm64x_dispatch_ret\n"
    ".rva __os_arm64x_check_call\n"
    ".rva __os_arm64x_check_icall\n"
    ".rva __os_arm64x_check_icall_cfg\n"
    ".rva __arm64x_native_entrypoint\n"
    ".rva __hybrid_auxiliary_iat\n"
    ".word __x64_code_ranges_to_entry_points_count\n"
    ".word __arm64x_redirection_metadata_count\n"
    ".rva __os_arm64x_get_x64_information\n"
    ".rva __os_arm64x_set_x64_information\n"
    ".rva __arm64x_extra_rfe_table\n"
    ".word __arm64x_extra_rfe_table_size\n"
    ".rva __os_arm64x_dispatch_fptr\n"
    ".rva __hybrid_auxiliary_iat_copy\n"
    ".rva __hybrid_auxiliary_delayload_iat\n"
    ".rva __hybrid_auxiliary_delayload_iat_copy\n"
    ".word __hybrid_image_info_bitfield\n"
    ".rva __os_arm64x_helper3\n"
    ".rva __os_arm64x_helper4\n"
    ".rva __os_arm64x_helper5\n"
    ".rva __os_arm64x_helper6\n"
    ".rva __os_arm64x_helper7\n"
    ".rva __os_arm64x_helper8\n");

/* IMAGE_LOAD_CONFIG_DIRECTORY64, as winebuild emits it for ARM64EC. */
asm(".section .rdata,\"dr\"\n"
    ".globl _load_config_used\n"
    ".balign 8\n"
    "_load_config_used:\n"
    ".long 0x140\n"                      /* Size */
    ".long 0\n"                          /* TimeDateStamp */
    ".short 0\n.short 0\n"               /* Major/MinorVersion */
    ".long 0\n.long 0\n.long 0\n"        /* GlobalFlagsClear/Set, CriticalSectionDefaultTimeout */
    ".xword 0\n.xword 0\n.xword 0\n"     /* DeCommitFreeBlockThreshold, DeCommitTotalFreeThreshold, LockPrefixTable */
    ".xword 0\n.xword 0\n.xword 0\n"     /* MaximumAllocationSize, VirtualMemoryThreshold, ProcessAffinityMask */
    ".long 0\n.short 0\n.short 0\n"      /* ProcessHeapFlags, CSDVersion, DependentLoadFlags */
    ".xword 0\n.xword 0\n.xword 0\n.xword 0\n" /* EditList, SecurityCookie, SEHandlerTable, SEHandlerCount */
    ".xword __guard_check_icall_fptr\n"
    ".xword __guard_dispatch_icall_fptr\n"
    ".xword 0\n.xword 0\n"               /* GuardCFFunctionTable, GuardCFFunctionCount */
    ".long __guard_flags\n"
    ".short 0\n.short 0\n.long 0\n.long 0\n" /* CodeIntegrity */
    ".xword 0\n.xword 0\n.xword 0\n.xword 0\n" /* GuardAddressTakenIatEntry*, GuardLongJumpTarget* */
    ".xword 0\n"                         /* DynamicValueRelocTable */
    ".xword __chpe_metadata\n"           /* CHPEMetadataPointer (offset 0xC8) */
    ".xword 0\n.xword 0\n"               /* GuardRFFailureRoutine, ...FunctionPointer */
    ".long 0\n.short 0\n.short 0\n"      /* DynamicValueRelocTableOffset/Section, Reserved2 */
    ".xword 0\n"                         /* GuardRFVerifyStackPointerFunctionPointer */
    ".long 0\n.long 0\n"                 /* HotPatchTableOffset, Reserved3 */
    ".xword 0\n.xword 0\n.xword 0\n.xword 0\n" /* Enclave, VolatileMetadata, GuardEHContinuation* */
    ".xword 0\n.xword 0\n.xword 0\n.xword 0\n.xword 0\n"); /* GuardXFG*, CastGuard, GuardMemcpy */

#endif
