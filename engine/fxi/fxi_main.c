// SPDX-License-Identifier: GPL-3.0-or-later
// fxi CLI (CI and desktop testing):
//   fxi <guest.elf> [args...]   run the guest, echo its output, print a stats line, exit with its code
//   fxi --scan-pe <file.exe|dll> [...]
//                               decode every x64 function listed in the PE's .pdata and report
//                               the instructions FXI does not implement (with counts): the to-do
//                               list for a Windows program, without a device round per instruction

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fxi.h"
#include "fxi_internal.h"

static unsigned char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)*len + 16);
    if (fread(buf, 1, (size_t)*len, f) != (size_t)*len) { perror("read"); fclose(f); return NULL; }
    memset(buf + *len, 0, 16);   // decoding past the end reads zeros
    fclose(f);
    return buf;
}

static uint32_t u16at(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t u32at(const unsigned char *p) { return u16at(p) | u16at(p + 2) << 16; }

// RVA -> file pointer (NULL outside a section's raw data)
static const unsigned char *rva_ptr(const unsigned char *f, long len, const unsigned char *sec, uint32_t nsec, uint32_t rva) {
    for (uint32_t i = 0; i < nsec; i++) {
        const unsigned char *s = sec + i * 40;
        uint32_t va = u32at(s + 12), vsz = u32at(s + 8), rawsz = u32at(s + 16), raw = u32at(s + 20);
        uint32_t sz = vsz && vsz < rawsz ? vsz : rawsz;
        if (rva >= va && rva < va + sz && (long)(raw + (rva - va)) < len) return f + raw + (rva - va);
    }
    return NULL;
}

typedef struct { char key[96]; char example[160]; unsigned long long count; uint64_t first; } Miss;

static int scan_pe(const char *path) {
    long len;
    unsigned char *f = read_file(path, &len);
    if (!f) return 2;
    if (len < 0x40 || f[0] != 'M' || f[1] != 'Z') { fprintf(stderr, "%s: not a PE file\n", path); return 2; }
    const unsigned char *pe = f + u32at(f + 0x3c);
    uint32_t machine = u16at(pe + 4), nsec = u16at(pe + 6), optsz = u16at(pe + 20);
    const unsigned char *opt = pe + 24, *sec = opt + optsz;
    if (u16at(opt) != 0x20b) { fprintf(stderr, "%s: not a PE32+ image\n", path); return 2; }
    uint64_t base = (uint64_t)u32at(opt + 24) | (uint64_t)u32at(opt + 28) << 32;
    uint32_t exc_rva = u32at(opt + 112 + 3 * 8), exc_size = u32at(opt + 112 + 3 * 8 + 4);

#define RVA(r) rva_ptr(f, len, sec, nsec, (r))
    const unsigned char *pdata = RVA(exc_rva);
    if (!pdata || !exc_size) { fprintf(stderr, "%s: no .pdata\n", path); return 2; }
    unsigned nfunc = exc_size / 12;

    // Functions: every .pdata entry, then (leaf functions have no .pdata) the entry point and
    // every direct call/jmp target found on the way, swept up to the first ret/jmp/int3.
    uint32_t image_size = u32at(opt + 56);
    uint8_t *seen = calloc(image_size / 8 + 1, 1);   // one bit per decoded instruction start
    enum { MAX_WORK = 1 << 16 };
    static uint32_t work[MAX_WORK];
    unsigned nwork = 0;
    work[nwork++] = u32at(opt + 16);                 // AddressOfEntryPoint
    static Miss miss[512];
    unsigned nmiss = 0;
    unsigned long long insns = 0, bad = 0, invalid = 0, funcs = 0, leafs = 0;
    for (unsigned i = 0; i <= nfunc; i++) {
        uint32_t begin, end;
        if (i < nfunc) {
            begin = u32at(pdata + i * 12); end = u32at(pdata + i * 12 + 4);
            if (end <= begin || end > image_size) continue;
        } else {
            if (!nwork) break;
            begin = work[--nwork]; end = 0; i--;     // stay on the worklist until it is empty
            if (begin >= image_size || (seen[begin >> 3] >> (begin & 7)) & 1) continue;
            leafs++;
        }
        funcs++;
        for (uint32_t rva = begin; end ? rva < end : rva < image_size;) {
            if ((seen[rva >> 3] >> (rva & 7)) & 1) break;   // joins code already swept
            const unsigned char *p = RVA(rva);
            if (!p) break;
            int op_end = 0, n = fxi_insn_length(p, &op_end);
            if (n <= 0) { invalid++; break; }
            seen[rva >> 3] |= (uint8_t)(1u << (rva & 7));
            insns++;
            uint8_t opc = p[op_end - 1];
            int two_byte = op_end >= 2 && p[op_end - 2] == 0x0f;
            if (!two_byte && (opc == 0xe8 || opc == 0xe9) && nwork < MAX_WORK) {
                int32_t rel; memcpy(&rel, p + op_end, 4);
                work[nwork++] = rva + (uint32_t)n + (uint32_t)rel;
            }
            int stop = !end && !two_byte && (opc == 0xc3 || opc == 0xc2 || opc == 0xe9 || opc == 0xeb || opc == 0xcc ||
                                            (opc == 0xff && ((p[op_end] >> 3) & 7) == 4));
            if (!end && two_byte && opc == 0x0b) stop = 1;   // ud2
            char why[160];
            if (!fxi_probe(p, base + rva, why, sizeof why)) {
                bad++;
                char key[96]; int o = 0;
                const char *br = strchr(why, '[');
                o += snprintf(key + o, sizeof key - (size_t)o, "%.*s[", br ? (int)(br - why) : 40, why);
                for (int k = 0; k < op_end && o < 80; k++) o += snprintf(key + o, sizeof key - (size_t)o, k ? " %02x" : "%02x", p[k]);
                snprintf(key + o, sizeof key - (size_t)o, "]");
                unsigned j;
                for (j = 0; j < nmiss; j++) if (!strcmp(miss[j].key, key)) break;
                if (j == nmiss && nmiss < 512) {
                    nmiss++;
                    snprintf(miss[j].key, sizeof miss[j].key, "%s", key);
                    snprintf(miss[j].example, sizeof miss[j].example, "%s", why);
                    miss[j].first = base + rva;
                }
                if (j < nmiss) miss[j].count++;
            }
            rva += (uint32_t)n;
            if (stop) break;
        }
    }
    free(seen);
    // Most frequent first.
    for (unsigned a = 0; a < nmiss; a++)
        for (unsigned b = a + 1; b < nmiss; b++)
            if (miss[b].count > miss[a].count) { Miss t = miss[a]; miss[a] = miss[b]; miss[b] = t; }
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    printf("### %s (machine %#x): %llu functions (%llu without .pdata), %llu instructions, %llu not implemented (%u kinds)%s\n\n",
           name, machine, funcs, leafs, insns, bad, nmiss, invalid ? ", some bytes undecodable" : "");
    if (nmiss) {
        printf("| count | instruction (prefixes + opcode) | first at | example |\n|---|---|---|---|\n");
        for (unsigned j = 0; j < nmiss; j++)
            printf("| %llu | `%s` | %#llx | `%s` |\n", miss[j].count, miss[j].key, (unsigned long long)miss[j].first, miss[j].example);
        printf("\n");
    }
    free(f);
    return 0;
#undef RVA
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--scan-pe")) {
        int rc = 0;
        for (int i = 2; i < argc; i++) rc |= scan_pe(argv[i]);
        return rc;
    }
    if (argc < 2) { fprintf(stderr, "usage: fxi <guest.elf> [args...] | fxi --scan-pe <pe>...\n"); return 2; }
    long len;
    unsigned char *buf = read_file(argv[1], &len);
    if (!buf) return 2;

    static fxi_result r;
    fxi_set_echo_fd(1);
    int ok = fxi_run_elf(buf, (size_t)len, argc - 1, (const char *const *)(argv + 1), &r);
    fflush(stdout);
    fprintf(stderr, "[fxi] %s ok=%d exit=%lld %.3f s, %llu blocks, %llu syscalls%s%s\n", fxi_version(), ok,
            r.exit_code, r.seconds, r.blocks, r.syscalls, r.error[0] ? " error: " : "", r.error);
    return ok ? (int)r.exit_code : 99;
}
