// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Host tests of spec 02's pure pieces. See ../TESTS.md for the build line. Exit status 0 when
// every check passed.

#include "wb_internal.h"
#include "host_stubs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static int g_checks, g_failures;

#define CHECK(cond) do {                                                          \
    g_checks++;                                                                   \
    if (!(cond)) {                                                                \
        g_failures++;                                                             \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                                             \
} while (0)

#define CHECK_STR(a, b) do {                                                      \
    const char *a_ = (a), *b_ = (b);                                              \
    g_checks++;                                                                   \
    if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                      \
        g_failures++;                                                             \
        fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,          \
                a_ ? a_ : "(null)", b_ ? b_ : "(null)");                          \
    }                                                                             \
} while (0)

static char g_tmp[PATH_MAX];

static void path_of(char *out, const char *rel)
{
    snprintf(out, PATH_MAX, "%s/%s", g_tmp, rel);
}

static void write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Whole file as a string, or NULL. Static buffer. */
static const char *read_text(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) return NULL;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

/* ================= tar.gz reader ================= */

static void tar_entry(gzFile gz, const char *name, const char *ustar_prefix, char type,
                      const void *data, size_t size, const char *raw_size)
{
    unsigned char h[512];
    unsigned int sum = 0;
    static const unsigned char zero[512];

    memset(h, 0, sizeof(h));
    memcpy(h, name, strlen(name) < 100 ? strlen(name) : 100);
    memcpy(h + 100, "0000644", 7);
    memcpy(h + 108, "0000000", 7);
    memcpy(h + 116, "0000000", 7);
    if (raw_size) memcpy(h + 124, raw_size, strlen(raw_size));
    else snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size);
    memcpy(h + 136, "00000000000", 11);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 6);   /* with its NUL */
    memcpy(h + 263, "00", 2);
    if (ustar_prefix) memcpy(h + 345, ustar_prefix, strlen(ustar_prefix));
    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 7, "%06o", sum);
    h[155] = ' ';
    gzwrite(gz, h, 512);
    if (size && data) {
        gzwrite(gz, data, (unsigned)size);
        if (size % 512) gzwrite(gz, zero, (unsigned)(512 - size % 512));
    }
}

static void tar_end(gzFile gz)
{
    static const unsigned char zero[1024];
    gzwrite(gz, zero, sizeof(zero));
}

static void test_targz(void)
{
    char archive[PATH_MAX], dest[PATH_MAX], p[PATH_MAX];
    unsigned char big[600];
    gzFile gz;
    struct stat st;

    for (size_t i = 0; i < sizeof(big); i++) big[i] = (unsigned char)(i * 7);

    /* A good archive with every kind of entry the reader must handle. */
    path_of(archive, "t1.tar.gz");
    path_of(dest, "t1");
    gz = gzopen(archive, "wb");
    tar_entry(gz, "prefix/", NULL, '5', NULL, 0, NULL);
    tar_entry(gz, "prefix/drive_c/", NULL, '5', NULL, 0, NULL);
    tar_entry(gz, "prefix/drive_c/hello.txt", NULL, '0', "hello", 5, NULL);
    tar_entry(gz, "prefix/big.bin", NULL, '0', big, sizeof(big), NULL);
    tar_entry(gz, "deep/file.txt", "prefix/drive_c/very", 0, "x", 1, NULL);   /* ustar prefix field */
    tar_entry(gz, "prefix/nuldir/", NULL, 0, NULL, 0, NULL);                   /* NUL type + '/' */
    tar_entry(gz, "prefix/../evil.txt", NULL, '0', "bad", 3, NULL);
    tar_entry(gz, "/abs.txt", NULL, '0', "bad", 3, NULL);
    tar_entry(gz, "prefix/link", NULL, '2', NULL, 0, NULL);
    tar_entry(gz, "prefix/paxhdr", NULL, 'x', "30 path=whatever/long/name\n", 27, NULL);
    tar_entry(gz, "prefix/longname", NULL, 'L', "a-very-long-name", 16, NULL);
    tar_entry(gz, "prefix/after.txt", NULL, '0', "ok", 2, NULL);
    tar_end(gz);
    gzclose(gz);

    CHECK(wb_targz_extract(archive, dest) == 0);
    path_of(p, "t1/drive_c/hello.txt");
    CHECK_STR(read_text(p), "hello");
    path_of(p, "t1/big.bin");
    CHECK(stat(p, &st) == 0 && st.st_size == 600 && (st.st_mode & 0777) == 0644);
    path_of(p, "t1/drive_c/very/deep/file.txt");
    CHECK_STR(read_text(p), "x");
    path_of(p, "t1/nuldir");
    CHECK(wb_is_dir(p));
    path_of(p, "evil.txt");
    CHECK(!wb_lexists(p));
    path_of(p, "t1/abs.txt");
    CHECK(!wb_lexists(p));
    path_of(p, "t1/link");
    CHECK(!wb_lexists(p));
    path_of(p, "t1/paxhdr");
    CHECK(!wb_lexists(p));
    path_of(p, "t1/longname");
    CHECK(!wb_lexists(p));
    path_of(p, "t1/after.txt");
    CHECK_STR(read_text(p), "ok");   /* the skipped entries' data was skipped correctly */
    {
        FILE *f;
        unsigned char got[600];
        path_of(p, "t1/big.bin");
        f = fopen(p, "rb");
        CHECK(f && fread(got, 1, sizeof(got), f) == sizeof(got) && memcmp(got, big, sizeof(big)) == 0);
        if (f) fclose(f);
    }

    /* No end-of-archive block: a short read is an error. */
    path_of(archive, "t2.tar.gz");
    path_of(dest, "t2");
    gz = gzopen(archive, "wb");
    tar_entry(gz, "prefix/a.txt", NULL, '0', "abc", 3, NULL);
    gzclose(gz);
    CHECK(wb_targz_extract(archive, dest) == -1);

    /* Data cut short. */
    path_of(archive, "t3.tar.gz");
    path_of(dest, "t3");
    gz = gzopen(archive, "wb");
    tar_entry(gz, "prefix/a.txt", NULL, '0', NULL, 2000, NULL);
    gzwrite(gz, big, 512);
    gzclose(gz);
    CHECK(wb_targz_extract(archive, dest) == -1);

    /* A non-octal size byte is an error; spaces and NULs are not. */
    path_of(archive, "t4.tar.gz");
    path_of(dest, "t4");
    gz = gzopen(archive, "wb");
    tar_entry(gz, "prefix/a.txt", NULL, '0', NULL, 0, "0000000001x");
    tar_end(gz);
    gzclose(gz);
    CHECK(wb_targz_extract(archive, dest) == -1);

    path_of(archive, "t5.tar.gz");
    path_of(dest, "t5");
    gz = gzopen(archive, "wb");
    tar_entry(gz, "prefix/s.txt", NULL, '0', NULL, 0, "   3 ");
    gzwrite(gz, "abc", 3);
    gzwrite(gz, big, 509);   /* pad the data block */
    tar_end(gz);
    gzclose(gz);
    CHECK(wb_targz_extract(archive, dest) == 0);
    path_of(p, "t5/s.txt");
    CHECK_STR(read_text(p), "abc");

    /* Missing archive. */
    path_of(archive, "missing.tar.gz");
    CHECK(wb_targz_extract(archive, dest) == -1);
}

/* ================= argument splitting ================= */

static void test_args(void)
{
    char *argv[WB_MAX_ARGS];
    char line[1024];
    int n, dropped;

    n = wb_split_args("a b", argv, WB_MAX_ARGS, &dropped);
    CHECK(n == 2 && dropped == 0);
    if (n == 2) { CHECK_STR(argv[0], "a"); CHECK_STR(argv[1], "b"); }
    wb_free_args(argv, n);

    n = wb_split_args("  \"a b\"\tc  ", argv, WB_MAX_ARGS, NULL);
    CHECK(n == 2);
    if (n == 2) { CHECK_STR(argv[0], "a b"); CHECK_STR(argv[1], "c"); }
    wb_free_args(argv, n);

    n = wb_split_args("x\"y z\"w -opt=\"1 2\"", argv, WB_MAX_ARGS, NULL);
    CHECK(n == 2);
    if (n == 2) { CHECK_STR(argv[0], "xy zw"); CHECK_STR(argv[1], "-opt=1 2"); }
    wb_free_args(argv, n);

    n = wb_split_args("\"\" end", argv, WB_MAX_ARGS, NULL);
    CHECK(n == 2);
    if (n == 2) { CHECK_STR(argv[0], ""); CHECK_STR(argv[1], "end"); }
    wb_free_args(argv, n);

    CHECK(wb_split_args(" \t ", argv, WB_MAX_ARGS, NULL) == 0);
    CHECK(wb_split_args(NULL, argv, WB_MAX_ARGS, NULL) == 0);

    /* Clarification 21: no escaping beyond quote grouping; a backslash is an ordinary byte. */
    n = wb_split_args("a\\\"b c\" C:\\path\\ \\\\x", argv, WB_MAX_ARGS, NULL);
    CHECK(n == 3);
    if (n == 3) {
        CHECK_STR(argv[0], "a\\b c");
        CHECK_STR(argv[1], "C:\\path\\");
        CHECK_STR(argv[2], "\\\\x");
    }
    wb_free_args(argv, n);

    n = wb_split_args("a \"\" \"\"\"\" b", argv, WB_MAX_ARGS, NULL);   /* "" and """" are empty */
    CHECK(n == 4);
    if (n == 4) { CHECK_STR(argv[1], ""); CHECK_STR(argv[2], ""); CHECK_STR(argv[3], "b"); }
    wb_free_args(argv, n);

    n = wb_split_args("\"unterminated quote", argv, WB_MAX_ARGS, NULL);
    CHECK(n == 1);
    if (n == 1) CHECK_STR(argv[0], "unterminated quote");
    wb_free_args(argv, n);

    line[0] = 0;
    for (int i = 0; i < 70; i++) strcat(line, "a ");
    n = wb_split_args(line, argv, WB_MAX_ARGS, &dropped);
    CHECK(n == 64 && dropped == 6);   /* argv then has 2 + 64 = 66 entries before its NULL */
    wb_free_args(argv, n);

    line[0] = 0;
    for (int i = 0; i < 64; i++) strcat(line, "\"a b\" ");
    n = wb_split_args(line, argv, WB_MAX_ARGS, &dropped);
    CHECK(n == 64 && dropped == 0);
    wb_free_args(argv, n);
}

/* ================= working folder ================= */

static void test_workdir(void)
{
    char w[1024], u[PATH_MAX], c[1100];
    char long_wd[600];

    CHECK(wb_workdir_choose("C:\\Games\\Foo\\\\", NULL, w, sizeof(w)) == 1);
    CHECK_STR(w, "C:\\Games\\Foo");
    CHECK(wb_workdir_choose("c:\\x", NULL, w, sizeof(w)) == 1);
    CHECK_STR(w, "c:\\x");
    CHECK(wb_workdir_choose("C:\\\\", NULL, w, sizeof(w)) == 1);   /* a char after C:\ */
    CHECK_STR(w, "C:\\");
    CHECK(wb_workdir_choose("C:\\", NULL, w, sizeof(w)) == 0);     /* nothing after C:\ */
    CHECK(wb_workdir_choose("C:\\a\\..\\b", NULL, w, sizeof(w)) == 0);
    CHECK(wb_workdir_choose("D:\\x", NULL, w, sizeof(w)) == 0);
    CHECK(wb_workdir_choose("Games", NULL, w, sizeof(w)) == 0);

    memset(long_wd, 'a', sizeof(long_wd));
    memcpy(long_wd, "C:\\", 3);
    long_wd[509] = 0;                                                /* 509 characters */
    CHECK(wb_workdir_choose(long_wd, NULL, w, sizeof(w)) == 1);
    long_wd[509] = 'a';
    long_wd[510] = 0;                                                /* 510 characters */
    CHECK(wb_workdir_choose(long_wd, NULL, w, sizeof(w)) == 0);

    /* An invalid MADEIRA_WORKDIR falls back to the program's folder. */
    CHECK(wb_workdir_choose("C:\\..", "C:\\Games\\Foo\\game.exe", w, sizeof(w)) == 1);
    CHECK_STR(w, "C:\\Games\\Foo");
    CHECK(wb_workdir_choose(NULL, "C:\\game.exe", w, sizeof(w)) == 1);
    CHECK_STR(w, "C:\\");
    CHECK(wb_workdir_choose(NULL, "notepad.exe", w, sizeof(w)) == 0);
    CHECK(wb_workdir_choose(NULL, "D:\\x\\game.exe", w, sizeof(w)) == 0);

    CHECK(wb_workdir_unix("/p", "C:\\Games\\Foo", u, sizeof(u)) == 0);
    CHECK_STR(u, "/p/drive_c/Games/Foo");
    CHECK(wb_workdir_unix("/p", "C:\\", u, sizeof(u)) == 0);
    CHECK_STR(u, "/p/drive_c");

    CHECK(wb_workdir_initial_cwd("C:\\Games", c, sizeof(c)) == 0);
    CHECK_STR(c, "C:\\Games\\");
    CHECK(wb_workdir_initial_cwd("C:\\Games\\\\", c, sizeof(c)) == 0);
    CHECK_STR(c, "C:\\Games\\");
    CHECK(wb_workdir_initial_cwd("C:\\", c, sizeof(c)) == 0);
    CHECK_STR(c, "C:\\");
}

/* ================= PE probe and target choice ================= */

static void write_pe(const char *path, uint32_t lfanew, int machine)
{
    unsigned char hdr[0x200];
    int fd;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'M';
    hdr[1] = 'Z';
    hdr[0x3C] = (unsigned char)lfanew;
    hdr[0x3D] = (unsigned char)(lfanew >> 8);
    hdr[0x3E] = (unsigned char)(lfanew >> 16);
    hdr[0x3F] = (unsigned char)(lfanew >> 24);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(path); exit(2); }
    if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) exit(2);
    if (machine >= 0) {
        unsigned char pe[6] = { 'P', 'E', 0, 0, (unsigned char)machine, (unsigned char)(machine >> 8) };
        if (pwrite(fd, pe, sizeof(pe), (off_t)lfanew) != (ssize_t)sizeof(pe)) exit(2);
    }
    close(fd);
}

static void test_pe_and_target(void)
{
    char p[PATH_MAX], bundle[PATH_MAX], prefix[PATH_MAX], q[PATH_MAX];
    struct wb_target t;

    path_of(p, "pe32.exe");
    write_pe(p, 0x80, 0x014c);
    CHECK(wb_pe_machine(p) == 0x014c);
    path_of(p, "pe64.exe");
    write_pe(p, 0x80, 0x8664);
    CHECK(wb_pe_machine(p) == 0x8664);
    path_of(p, "pe-at-16mb.exe");
    write_pe(p, 16u * 1024 * 1024, 0x014c);          /* e_lfanew of exactly 16 MB is allowed */
    CHECK(wb_pe_machine(p) == 0x014c);
    path_of(p, "pe-past-16mb.exe");
    write_pe(p, 16u * 1024 * 1024 + 8, 0x014c);      /* above 16 MB: rejected */
    CHECK(wb_pe_machine(p) == -1);
    path_of(p, "no-pe-sig.exe");
    write_pe(p, 0x80, -1);
    CHECK(wb_pe_machine(p) == -1);
    path_of(p, "short.exe");
    write_bytes(p, "MZ", 2);
    CHECK(wb_pe_machine(p) == -1);
    path_of(p, "text.txt");
    write_bytes(p, "hello world, not a program at all, long enough for a DOS header..........", 74);
    CHECK(wb_pe_machine(p) == -1);
    path_of(p, "does-not-exist.exe");
    CHECK(wb_pe_machine(p) == -1);

    /* A bundle with all three sets and a prefix with two programs. */
    path_of(bundle, "bundle");
    path_of(prefix, "prefix");
    snprintf(q, sizeof(q), "%s/i386-windows", bundle);       wb_mkdirs(q, 0755);
    snprintf(q, sizeof(q), "%s/aarch64-windows", bundle);    wb_mkdirs(q, 0755);
    snprintf(q, sizeof(q), "%s/arm64ec-windows", bundle);    wb_mkdirs(q, 0755);
    snprintf(q, sizeof(q), "%s/drive_c/Games", prefix);      wb_mkdirs(q, 0755);
    snprintf(q, sizeof(q), "%s/i386-windows/ntdll.dll", bundle);       write_pe(q, 0x80, 0x014c);
    snprintf(q, sizeof(q), "%s/i386-windows/game32.exe", bundle);      write_pe(q, 0x80, 0x014c);
    snprintf(q, sizeof(q), "%s/i386-windows/notepad.exe", bundle);     write_pe(q, 0x80, 0x014c);
    snprintf(q, sizeof(q), "%s/aarch64-windows/notepad.exe", bundle);  write_pe(q, 0x80, 0xAA64);
    snprintf(q, sizeof(q), "%s/drive_c/Games/g32.exe", prefix);        write_pe(q, 0x80, 0x014c);
    snprintf(q, sizeof(q), "%s/drive_c/Games/g64.exe", prefix);        write_pe(q, 0x80, 0x8664);

    /* Bare name in a 64-bit set: 64-bit, not probed (even though i386 has one too). */
    CHECK(wb_choose_target("notepad.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.probe == WB_PROBE_BUNDLE64 && !t.is32 && t.set == WB_SET_AARCH64);
    CHECK_STR(t.winpath, "C:\\windows\\system32\\notepad.exe");

    /* Bare name only in i386-windows: probed, 32-bit, syswow64. */
    CHECK(wb_choose_target("game32.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.probe == WB_PROBE_FILE && t.is32 && t.machine == 0x014c && t.set == WB_SET_AARCH64);
    CHECK_STR(t.winpath, "C:\\windows\\syswow64\\game32.exe");

    /* Full path to a 32-bit program: aarch64 for the 64-bit half despite the backslash. */
    CHECK(wb_choose_target("C:\\Games\\g32.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.is32 && t.set == WB_SET_AARCH64);
    CHECK(strstr(t.probed, "/drive_c/Games/g32.exe") != NULL);
    CHECK_STR(t.winpath, "C:\\Games\\g32.exe");

    /* Lower-case drive letter, 64-bit program: arm64ec (backslash). */
    CHECK(wb_choose_target("c:\\Games\\g64.exe", bundle, prefix, 0, &t) == 0);
    CHECK(!t.is32 && t.machine == 0x8664 && t.set == WB_SET_ARM64EC);

    /* A bare name nowhere: probe fails, unknown; "x64" in the name picks arm64ec. */
    CHECK(wb_choose_target("tool_x64.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.probe == WB_PROBE_FILE && t.machine == -1 && !t.is32 && t.set == WB_SET_ARM64EC);
    CHECK_STR(t.winpath, "C:\\windows\\system32\\tool_x64.exe");

    /* MADEIRA_USE_ARM64EC=1 forces arm64ec for a 64-bit program. */
    CHECK(wb_choose_target("notepad.exe", bundle, prefix, 1, &t) == 0);
    CHECK(t.set == WB_SET_ARM64EC);

    /* ...but not for a 32-bit one. */
    CHECK(wb_choose_target("game32.exe", bundle, prefix, 1, &t) == 0);
    CHECK(t.is32 && t.set == WB_SET_AARCH64);

    /* Other forms are not probed. */
    CHECK(wb_choose_target("D:\\x.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.probe == WB_PROBE_OTHER_FORM && !t.is32 && t.set == WB_SET_ARM64EC);
    CHECK_STR(t.winpath, "D:\\x.exe");
    CHECK(wb_choose_target("C:x.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.probe == WB_PROBE_OTHER_FORM && t.set == WB_SET_AARCH64);
    CHECK_STR(t.winpath, "C:x.exe");

    /* Without i386-windows/ntdll.dll there is no 32-bit detection at all. */
    snprintf(q, sizeof(q), "%s/i386-windows/ntdll.dll", bundle);
    unlink(q);
    CHECK(wb_choose_target("game32.exe", bundle, prefix, 0, &t) == 0);
    CHECK(t.probe == WB_PROBE_NO_32BIT_SET && !t.is32 && t.set == WB_SET_AARCH64);
    CHECK_STR(t.winpath, "C:\\windows\\system32\\game32.exe");
}

/* ================= settings file and sync rule ================= */

static void test_settings(void)
{
    static const char text[] =
        "\xEF\xBB\xBF# comment\n"
        "  key1 = value one  \r\n"
        "noequals\n"
        "key2=a=b=c\n"
        "=novalue\n"
        "  # indented comment = still a comment\n"
        "key1 = second\n"
        "empty =\n";
    struct wb_cfg cfg;
    char path[PATH_MAX], out[64];

    CHECK(wb_cfg_parse(&cfg, text, sizeof(text) - 1) == 0);
    CHECK(cfg.count == 4);
    CHECK_STR(wb_cfg_lookup(&cfg, "key1"), "second");   /* last occurrence wins */
    CHECK_STR(wb_cfg_lookup(&cfg, "key2"), "a=b=c");
    CHECK_STR(wb_cfg_lookup(&cfg, "empty"), "");
    CHECK(wb_cfg_lookup(&cfg, "noequals") == NULL);
    CHECK(wb_cfg_lookup(&cfg, "# comment") == NULL);
    wb_cfg_free(&cfg);

    CHECK(wb_cfg_is_true("1") && wb_cfg_is_true("on") && wb_cfg_is_true("TRUE") && wb_cfg_is_true("Yes"));
    CHECK(!wb_cfg_is_true("0") && !wb_cfg_is_true("off") && !wb_cfg_is_true("2") && !wb_cfg_is_true("") &&
          !wb_cfg_is_true(NULL));

    /* The sync engine default. */
#define SYNC(txt, env_set) (wb_cfg_parse(&cfg, txt, strlen(txt)), r = wb_sync_default_needed(&cfg, env_set), \
                            wb_cfg_free(&cfg), r)
    {
        int r;
        CHECK(SYNC("", 0) == 1);
        CHECK(SYNC("other = 1\n", 0) == 1);
        CHECK(SYNC("", 1) == 0);                                   /* already set */
        CHECK(SYNC("inproc-sync = true\n", 0) == 0);
        CHECK(SYNC("inproc-sync = 0\n", 0) == 0);                  /* present and false */
        CHECK(SYNC("env.MADEIRA_FASTSYNC = 0\n", 0) == 0);         /* exported from the file */
        CHECK(SYNC("inproc-sync = no\nenv.MADEIRA_FASTSYNC = 1\n", 0) == 0);
    }
#undef SYNC

    /* env. exports: a per-launch key the app set keeps its value; a later line in the file
     * still replaces an earlier one. */
    setenv("MADEIRA_CPU_COUNT", "4", 1);
    unsetenv("MADEIRA_FRAMEGEN");
    unsetenv("WB_TEST_PLAIN");
    {
        static const char env_text[] =
            "env.MADEIRA_CPU_COUNT = 2\n"
            "env.MADEIRA_FRAMEGEN = 1\n"
            "env.MADEIRA_FRAMEGEN = 0\n"
            "env.WB_TEST_PLAIN = hello world\n"
            "env. = ignored\n"
            "other = 1\n";
        wb_cfg_parse(&cfg, env_text, sizeof(env_text) - 1);
        wb_cfg_export_env(&cfg);
        wb_cfg_free(&cfg);
    }
    CHECK_STR(getenv("MADEIRA_CPU_COUNT"), "4");
    CHECK_STR(getenv("MADEIRA_FRAMEGEN"), "0");
    CHECK_STR(getenv("WB_TEST_PLAIN"), "hello world");
    wb_unset_per_launch_keys();
    CHECK(getenv("MADEIRA_CPU_COUNT") == NULL && getenv("MADEIRA_FRAMEGEN") == NULL);
    CHECK(wb_per_launch_key_count == 11);

    /* The file itself, through the documents folder. */
    setenv("WB_TEST_DOCS", g_tmp, 1);
    path_of(path, "madeira.cfg");
    unlink(path);
    CHECK(wb_cfg_get("count", out, sizeof(out)) == 0);
    CHECK(wb_cfg_get_int("count", 7) == 7);
    write_bytes(path, "count = 12\nflag = on\nbad = 12x\n", 31);
    CHECK(wb_cfg_get("count", out, sizeof(out)) == 1);
    CHECK_STR(out, "12");
    CHECK(wb_cfg_get_int("count", 7) == 12);
    CHECK(wb_cfg_get_int("bad", 7) == 7);
    CHECK(wb_cfg_get_bool("flag", 0) == 1);
    CHECK(wb_cfg_get_bool("missing", 1) == 1);
    CHECK(wb_cfg_load(&cfg) == 1 && cfg.count == 3);
    wb_cfg_free(&cfg);
}

/* ================= Steam identity and CPU probe ================= */

static void test_steam(void)
{
    char longpath[1100];

    CHECK(wb_steam_identity_valid("480", "C:\\Games\\Spacewar"));
    CHECK(wb_steam_identity_valid("1234567890", "c:\\x"));
    CHECK(!wb_steam_identity_valid("12345678901", "C:\\x"));   /* 11 digits */
    CHECK(!wb_steam_identity_valid("", "C:\\x"));
    CHECK(!wb_steam_identity_valid("48a0", "C:\\x"));
    CHECK(!wb_steam_identity_valid("-480", "C:\\x"));
    CHECK(!wb_steam_identity_valid("480", "D:\\x"));
    CHECK(!wb_steam_identity_valid("480", "C:/x"));
    CHECK(!wb_steam_identity_valid("480", "C:\\a\\..\\b"));
    CHECK(!wb_steam_identity_valid(NULL, "C:\\x"));
    CHECK(!wb_steam_identity_valid("480", NULL));
    memset(longpath, 'a', sizeof(longpath));
    memcpy(longpath, "C:\\", 3);
    longpath[1023] = 0;
    CHECK(wb_steam_identity_valid("480", longpath));           /* 1023 characters */
    longpath[1023] = 'a';
    longpath[1024] = 0;
    CHECK(!wb_steam_identity_valid("480", longpath));          /* 1024 characters */
}

static int fake_sysctl(const char *name, int *value)
{
    if (strcmp(name, "hw.optional.arm.FEAT_AES") == 0) { *value = 1; return 1; }
    if (strcmp(name, "hw.optional.arm.FEAT_AFP") == 0) { *value = 0; return 1; }
    if (strcmp(name, "hw.optional.armv8_crc32") == 0) { *value = 7; return 1; }
    return 0;
}

static void test_hostprobe(void)
{
    char out[256];

    wb_hostprobe_format(out, sizeof(out), fake_sysctl);
    CHECK_STR(out, "AFP=0,FLAGM=?,FLAGM2=?,FCMA=?,RCPC=?,LRCPC2=?,AES=1,PMULL=?,SHA=?,CRC=1,ATOMICS=?");
}

/* ================= controller stores ================= */

static void test_pads(void)
{
    struct winios_gamepad a, b, out;
    struct winios_hidpad h, hout;
    struct winios_hidpad_output o, oout;
    struct winios_gamepad_vibration v;

    memset(&a, 0, sizeof(a));
    a.connected = 5;           /* any non-zero value means connected; stored as 1 */
    a.buttons = 0x1000;
    a.lx = -32768;
    a.packet = 999;            /* the store owns the packet */
    a.reserved[1] = 9;         /* zeroed by the store */
    b = a;
    b.buttons = 0x2000;

    CHECK(winios_gamepad_get_state(0, &out) == 0);
    winios_gamepad_set_state(0, &a);
    CHECK(winios_gamepad_get_state(0, &out) == 1);
    CHECK(out.packet == 1 && out.connected == 1 && out.buttons == 0x1000 && out.lx == -32768 &&
          out.reserved[1] == 0);
    winios_gamepad_set_state(0, &a);                     /* same value: no change */
    winios_gamepad_get_state(0, &out);
    CHECK(out.packet == 1);
    a.reserved[0] = 3;                                   /* only reserved differs: no change */
    winios_gamepad_set_state(0, &a);
    winios_gamepad_get_state(0, &out);
    CHECK(out.packet == 1);
    winios_gamepad_set_state(0, &b);
    winios_gamepad_get_state(0, &out);
    CHECK(out.packet == 2 && out.buttons == 0x2000);
    winios_gamepad_set_state(0, NULL);                   /* unplugged: packet 3, read as zeros */
    memset(&out, 0xAB, sizeof(out));
    CHECK(winios_gamepad_get_state(0, &out) == 0);
    {
        struct winios_gamepad zero;
        memset(&zero, 0, sizeof(zero));
        CHECK(memcmp(&out, &zero, sizeof(out)) == 0);
    }
    winios_gamepad_set_state(0, NULL);                   /* still unplugged: no change */
    winios_gamepad_set_state(0, &b);
    winios_gamepad_get_state(0, &out);
    CHECK(out.packet == 4);
    b.connected = 0;                                     /* connected == 0 means unplugged */
    winios_gamepad_set_state(0, &b);
    CHECK(winios_gamepad_get_state(0, NULL) == 0);
    winios_gamepad_set_state(4, &a);                     /* out of range: ignored */
    CHECK(winios_gamepad_get_state(4, &out) == 0 && out.packet == 0);
    CHECK(winios_gamepad_get_state(-1, NULL) == 0);
    CHECK(winios_gamepad_get_state(1, &out) == 0);       /* other slots untouched */

    /* HID pad: a disconnected read keeps the packet. */
    memset(&h, 0, sizeof(h));
    h.connected = 1;
    h.buttons = WINIOS_HIDPAD_TOUCHPAD;
    h.reserved[0] = 1;
    h.reserved2 = 77;
    winios_hidpad_set_state(&h);
    CHECK(winios_hidpad_get_state(&hout) == 1);
    CHECK(hout.packet == 1 && hout.reserved[0] == 0 && hout.reserved2 == 0 && hout.buttons == WINIOS_HIDPAD_TOUCHPAD);
    h.reserved2 = 5;
    winios_hidpad_set_state(&h);
    winios_hidpad_get_state(&hout);
    CHECK(hout.packet == 1);
    winios_hidpad_set_state(NULL);
    memset(&hout, 0xAB, sizeof(hout));
    CHECK(winios_hidpad_get_state(&hout) == 0);
    CHECK(hout.packet == 2 && hout.buttons == 0 && hout.connected == 0 && hout.gyro[2] == 0);

    /* HID output: serial counts changes. */
    memset(&o, 0, sizeof(o));
    o.serial = 1234;
    o.rumble_valid = 1;
    o.rumble_left = 200;
    o.reserved[2] = 9;
    winios_hidpad_set_output(&o);
    wb_hidpad_get_output(&oout);
    CHECK(oout.serial == 1 && oout.rumble_left == 200 && oout.reserved[2] == 0);
    winios_hidpad_set_output(&o);
    wb_hidpad_get_output(&oout);
    CHECK(oout.serial == 1);
    o.lightbar_valid = 1;
    o.blue = 255;
    winios_hidpad_set_output(&o);
    wb_hidpad_get_output(&oout);
    CHECK(oout.serial == 2 && oout.blue == 255);
    winios_hidpad_set_output(NULL);
    wb_hidpad_get_output(&oout);
    CHECK(oout.serial == 2);

    /* XInput rumble: the first set counts even with zero speeds. */
    wb_gamepad_get_vibration(2, &v);
    CHECK(v.serial == 0);
    winios_gamepad_set_vibration(2, 0, 0);
    wb_gamepad_get_vibration(2, &v);
    CHECK(v.serial == 1);
    winios_gamepad_set_vibration(2, 0, 0);
    wb_gamepad_get_vibration(2, &v);
    CHECK(v.serial == 1);
    winios_gamepad_set_vibration(2, 1000, 65535);
    wb_gamepad_get_vibration(2, &v);
    CHECK(v.serial == 2 && v.left == 1000 && v.right == 65535);
    winios_gamepad_set_vibration(9, 1, 1);               /* ignored */
    CHECK(winios_gamepad_rumble_caps() == 0);
}

/* ================= game picture ================= */

static void test_picture(void)
{
    int x, y, w, h;
    struct wb_client_rect r[4];

    CHECK(wb_pick_picture_rect(NULL, 0, 800, 600, 1024, 768, &x, &y, &w, &h) == 0);

    /* 1. Metal windows win, the largest one. */
    r[0] = (struct wb_client_rect){ 0, 0, 1024, 768, 0 };
    r[1] = (struct wb_client_rect){ 10, 20, 640, 480, 1 };
    r[2] = (struct wb_client_rect){ 30, 40, 800, 600, 1 };
    CHECK(wb_pick_picture_rect(r, 3, 1024, 768, 1024, 768, &x, &y, &w, &h) == 1);
    CHECK(x == 30 && y == 40 && w == 800 && h == 600);

    /* 2. No Metal window: exact size match, at least a quarter of the desktop. */
    r[0] = (struct wb_client_rect){ 5, 25, 800, 600, 0 };
    r[1] = (struct wb_client_rect){ 0, 0, 300, 200, 0 };
    CHECK(wb_pick_picture_rect(r, 2, 800, 600, 1024, 768, &x, &y, &w, &h) == 1);
    CHECK(x == 5 && y == 25 && w == 800 && h == 600);
    CHECK(wb_pick_picture_rect(r, 2, 640, 480, 1024, 768, &x, &y, &w, &h) == 0);   /* no exact match */

    /* Exactly a quarter qualifies; just under does not. */
    r[0] = (struct wb_client_rect){ 0, 0, 512, 384, 0 };
    CHECK(wb_pick_picture_rect(r, 1, 512, 384, 1024, 768, &x, &y, &w, &h) == 1);
    r[0] = (struct wb_client_rect){ 0, 0, 511, 384, 0 };
    CHECK(wb_pick_picture_rect(r, 1, 511, 384, 1024, 768, &x, &y, &w, &h) == 0);
}

/* ================= exit status ================= */

static void test_exit_status(void)
{
    uint32_t s = 0;

    wb_exit_status_reset();
    CHECK(wb_crash_exit_status(&s) == 0);
    wine_launched_process_did_exit(0);
    wine_launched_process_did_exit(1);
    wine_launched_process_did_exit(0x7FFFFFFF);
    CHECK(wb_crash_exit_status(&s) == 0);
    wine_launched_process_did_exit((int)0xC0000005u);
    CHECK(wb_crash_exit_status(&s) == 1 && s == 0xC0000005u);
    CHECK(wine_crash_exit_status(NULL) == 1);
    wine_launched_process_did_exit(0);                   /* a later clean exit doesn't clear it */
    CHECK(wb_crash_exit_status(&s) == 1 && s == 0xC0000005u);
    wine_launched_process_did_exit(-1);                  /* 0xFFFFFFFF */
    CHECK(wb_crash_exit_status(&s) == 1 && s == 0xFFFFFFFFu);
    wb_exit_status_reset();
    CHECK(wb_crash_exit_status(&s) == 0);
}

/* ================= WINEDEBUG choice ================= */

static void test_debug_verbose(void)
{
    CHECK(!wb_debug_verbose(NULL));
    CHECK(!wb_debug_verbose(""));      /* clarification 17: empty is off */
    CHECK(!wb_debug_verbose("0"));
    CHECK(!wb_debug_verbose("0x"));
    CHECK(wb_debug_verbose("1"));
    CHECK(wb_debug_verbose("yes"));
}

/* ================= prefix seeding ================= */

static const char *link_target(const char *path)
{
    static char buf[PATH_MAX];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);

    if (n < 0) return NULL;
    buf[n] = 0;
    return buf;
}

static void mkdir_at(const char *base, const char *rel)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    wb_mkdirs(p, 0755);
}

static void file_at(const char *base, const char *rel)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    write_bytes(p, "x", 1);
}

static void link_at(const char *base, const char *rel, const char *target)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    unlink(p);
    if (symlink(target, p) != 0) { perror(p); exit(2); }
}

static void test_seed(void)
{
    char bundle[PATH_MAX], prefix[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    gzFile gz;

    path_of(bundle, "seedbundle");
    path_of(prefix, "seedprefix");
    wb_mkdirs(bundle, 0755);
    setenv("WB_TEST_BUNDLE", bundle, 1);
    setenv("USER", "some/path\\tester", 1);   /* only what follows the last / or \ */
    unsetenv("MADEIRA_PROFILE_LOCALLOW");

    snprintf(p, sizeof(p), "%s/prefix-template.tar.gz", bundle);
    gz = gzopen(p, "wb");
    tar_entry(gz, "prefix/", NULL, '5', NULL, 0, NULL);
    tar_entry(gz, "prefix/system.reg", NULL, '0', "WINE REGISTRY", 13, NULL);
    tar_entry(gz, "prefix/drive_c/users/builder/", NULL, '5', NULL, 0, NULL);
    tar_entry(gz, "prefix/drive_c/users/builder/Music/song.txt", NULL, '0', "la", 2, NULL);
    tar_end(gz);
    gzclose(gz);

    /* Left from somewhere else: an old c:, a z: (left alone), a dangling Documents link. */
    mkdir_at(prefix, "dosdevices");
    mkdir_at(prefix, "drive_c/users/builder");
    link_at(prefix, "dosdevices/c:", "/old/place");
    link_at(prefix, "dosdevices/z:", "/");
    link_at(prefix, "drive_c/users/builder/Documents", "/Users/builder/Documents");

    wb_seed_prefix(prefix);

    path_of(p, "seedprefix/system.reg");
    CHECK_STR(read_text(p), "WINE REGISTRY");
    path_of(p, "seedprefix/dosdevices/c:");
    CHECK_STR(link_target(p), "../drive_c");
    path_of(p, "seedprefix/dosdevices/z:");
    CHECK_STR(link_target(p), "/");                                 /* clarification 10 */
    path_of(p, "seedprefix/drive_c/users/tester/AppData/LocalLow");
    CHECK(wb_is_real_dir(p));
    path_of(p, "seedprefix/drive_c/users/tester/AppData/Roaming");
    CHECK(!wb_lexists(p));                                          /* only LocalLow */
    path_of(p, "seedprefix/drive_c/users/builder/Documents");
    CHECK(wb_is_real_dir(p));                                       /* dangling link replaced */
    path_of(p, "seedprefix/drive_c/users/builder/Music/song.txt");
    CHECK_STR(read_text(p), "la");                                  /* real folder kept */
    path_of(p, "seedprefix/drive_c/users/tester/Videos");
    CHECK(wb_is_real_dir(p));

    /* Clarification 15: the same prefix with its c: link is not seeded again... */
    path_of(p, "seedprefix/system.reg");
    unlink(p);
    wb_seed_prefix(prefix);
    CHECK(!wb_lexists(p));
    /* ...but without c: it is, in full (no .update-timestamp: the template again). */
    path_of(q, "seedprefix/dosdevices/c:");
    unlink(q);
    wb_seed_prefix(prefix);
    CHECK_STR(read_text(p), "WINE REGISTRY");
    CHECK_STR(link_target(q), "../drive_c");

    /* .update-timestamp: the template is not extracted again. */
    file_at(prefix, ".update-timestamp");
    unlink(p);
    unlink(q);
    wb_seed_prefix(prefix);
    CHECK(!wb_lexists(p));

    /* MADEIRA_PROFILE_LOCALLOW starting with 0 skips LocalLow. */
    path_of(prefix, "seedprefix2");
    setenv("MADEIRA_PROFILE_LOCALLOW", "0 please", 1);
    wb_seed_prefix(prefix);
    path_of(p, "seedprefix2/drive_c/users/tester/AppData/LocalLow");
    CHECK(!wb_lexists(p));
    unsetenv("MADEIRA_PROFILE_LOCALLOW");

    /* No template in the bundle: seeding still makes c:. */
    snprintf(p, sizeof(p), "%s/prefix-template.tar.gz", bundle);
    unlink(p);
    path_of(prefix, "seedprefix3");
    wb_seed_prefix(prefix);
    path_of(p, "seedprefix3/dosdevices/c:");
    CHECK_STR(link_target(p), "../drive_c");
}

/* ================= DLL farms and winsxs ================= */

static void test_farms(void)
{
    char bundle[PATH_MAX], prefix[PATH_MAX], p[PATH_MAX], r[PATH_MAX], want[PATH_MAX];
    const char *m;
    static const char cc_dir[] =
        "drive_c/windows/winsxs/x86_microsoft.windows.common-controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef";
    static const char cc_manifest[] =
        "drive_c/windows/winsxs/manifests/x86_microsoft.windows.common-controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef.manifest";
    static const char crt80_manifest[] =
        "drive_c/windows/winsxs/manifests/x86_microsoft.vc80.crt_1fc8b3b9a1e18e3b_8.0.50727.9672_none_deadbeef.manifest";
    static const char atl80_name[] = "x86_microsoft.vc80.atl_1fc8b3b9a1e18e3b_8.0.50727.4053_none_deadbeef";

    path_of(bundle, "farmbundle");
    path_of(prefix, "farmprefix");
    unsetenv("MYIOSDECK_NOJIT_EMULATOR");
    unsetenv("MYIOSDECK_NOJIT_WOW_CPU");

    mkdir_at(bundle, "arm64ec-windows");
    mkdir_at(bundle, "aarch64-windows");
    mkdir_at(bundle, "i386-windows");
    mkdir_at(bundle, "x86_64-vcruntime");
    mkdir_at(bundle, "arm64ec-windows/subdir");
    file_at(bundle, "arm64ec-windows/a.dll");
    file_at(bundle, "arm64ec-windows/ec_only.dll");
    file_at(bundle, "arm64ec-windows/wbemprox.dll");
    file_at(bundle, "arm64ec-windows/.hidden");
    file_at(bundle, "aarch64-windows/a.dll");
    file_at(bundle, "aarch64-windows/aa_only.dll");
    file_at(bundle, "i386-windows/ntdll.dll");
    file_at(bundle, "i386-windows/wmic.exe");
    file_at(bundle, "i386-windows/comctl32_v6.dll");
    file_at(bundle, "i386-windows/msvcr80.dll");
    file_at(bundle, "i386-windows/msvcp80.dll");   /* no msvcm80.dll, no atl80.dll */
    file_at(bundle, "i386-windows/gdiplus.dll");
    file_at(bundle, "x86_64-vcruntime/vcruntime140.dll");
    file_at(bundle, "x86_64-vcruntime/msvcp140.dll");
    file_at(bundle, "x86_64-vcruntime/vcruntime140_1.dll");
    file_at(bundle, "x86_64-vcruntime/readme.txt");

    /* An earlier build's ATL80 assembly (its DLL is gone now), and an old mono link. */
    snprintf(p, sizeof(p), "drive_c/windows/winsxs/%s", atl80_name);
    mkdir_at(prefix, p);
    snprintf(p, sizeof(p), "drive_c/windows/winsxs/%s/atl80.dll", atl80_name);
    link_at(prefix, p, "/old/bundle/i386-windows/atl80.dll");
    mkdir_at(prefix, "drive_c/windows/winsxs/manifests");
    snprintf(p, sizeof(p), "drive_c/windows/winsxs/manifests/%s.manifest", atl80_name);
    file_at(prefix, p);
    mkdir_at(prefix, "drive_c/windows/mono");
    link_at(prefix, "drive_c/windows/mono/mono-2.0", "/old/bundle/wine-mono");
    setenv("MYIOSDECK_NOJIT_EMULATOR", "/emu/xtajit64.dll", 1);

    /* ---- an arm64ec session ---- */
    wb_link_dll_farms(prefix, bundle, WB_SET_ARM64EC);

#define LINK_IS(rel, target_rel) do {                                   \
        snprintf(p, sizeof(p), "%s/%s", prefix, rel);                   \
        snprintf(want, sizeof(want), "%s/%s", bundle, target_rel);      \
        CHECK_STR(link_target(p), want);                                \
    } while (0)
#define ABSENT(rel) do {                                                \
        snprintf(p, sizeof(p), "%s/%s", prefix, rel);                   \
        CHECK(!wb_lexists(p));                                          \
    } while (0)

    LINK_IS("drive_c/windows/system32/a.dll", "arm64ec-windows/a.dll");
    LINK_IS("drive_c/windows/system32/ec_only.dll", "arm64ec-windows/ec_only.dll");
    LINK_IS("drive_c/windows/system32/aa_only.dll", "aarch64-windows/aa_only.dll");   /* other set */
    LINK_IS("drive_c/windows/system32/wbem/wbemprox.dll", "arm64ec-windows/wbemprox.dll");
    ABSENT("drive_c/windows/system32/wbem/a.dll");
    ABSENT("drive_c/windows/system32/.hidden");
    ABSENT("drive_c/windows/system32/subdir");
    LINK_IS("drive_c/windows/system32/vcruntime140_1.dll", "x86_64-vcruntime/vcruntime140_1.dll");
    ABSENT("drive_c/windows/system32/vcruntime140.dll");   /* stays built-in */
    ABSENT("drive_c/windows/system32/msvcp140.dll");
    ABSENT("drive_c/windows/system32/readme.txt");
    snprintf(p, sizeof(p), "%s/drive_c/windows/system32/xtajit64.dll", prefix);
    CHECK_STR(link_target(p), "/emu/xtajit64.dll");
    LINK_IS("drive_c/windows/sysx64/ec_only.dll", "arm64ec-windows/ec_only.dll");
    ABSENT("drive_c/windows/sysx64/aa_only.dll");
    LINK_IS("drive_c/windows/sysaa64/a.dll", "aarch64-windows/a.dll");
    LINK_IS("drive_c/windows/syswow64/ntdll.dll", "i386-windows/ntdll.dll");
    LINK_IS("drive_c/windows/syswow64/wbem/wmic.exe", "i386-windows/wmic.exe");
    ABSENT("drive_c/windows/syswow64/wbem/ntdll.dll");

    /* winsxs */
    snprintf(r, sizeof(r), "%s/comctl32.dll", cc_dir);
    LINK_IS(r, "i386-windows/comctl32_v6.dll");
    snprintf(p, sizeof(p), "%s/%s", prefix, cc_manifest);
    m = read_text(p);
    CHECK(m && strncmp(m, "<?xml version=\"1.0\" encoding=\"UTF-8\"", 36) == 0);
    CHECK(m && strstr(m, "name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.2600.2982\" "
                         "processorArchitecture=\"x86\" publicKeyToken=\"6595b64144ccf1df\""));
    CHECK(m && strstr(m, "<file name=\"comctl32.dll\">\n    <windowClass>Button</windowClass>\n"));
    CHECK(m && strstr(m, "<windowClass>tooltips_class32</windowClass>\n  </file>\n</assembly>\n"));
    CHECK(m && !strchr(m, '\r'));
    snprintf(p, sizeof(p), "%s/%s", prefix, crt80_manifest);
    m = read_text(p);
    CHECK(m && strstr(m, "<file name=\"msvcr80.dll\"/>") && strstr(m, "<file name=\"msvcp80.dll\"/>") &&
          !strstr(m, "msvcm80.dll"));                       /* only the files linked */
    ABSENT("drive_c/windows/winsxs/manifests/x86_microsoft.vc90.crt_1fc8b3b9a1e18e3b_9.0.30729.6161_none_deadbeef.manifest");
    LINK_IS("drive_c/windows/winsxs/x86_microsoft.windows.gdiplus_6595b64144ccf1df_1.0.6000.16386_none_deadbeef/gdiplus.dll",
            "i386-windows/gdiplus.dll");
    LINK_IS("drive_c/windows/winsxs/x86_microsoft.windows.gdiplus_6595b64144ccf1df_1.1.7601.23038_none_deadbeef/gdiplus.dll",
            "i386-windows/gdiplus.dll");
    ABSENT("drive_c/windows/winsxs/x86_microsoft-windows-msxml30_31bf3856ad364e35_6.0.6000.16386_none_deadbeef");
    /* Clarification 13: the skipped ATL80 lost its manifest and its folder. */
    snprintf(r, sizeof(r), "drive_c/windows/winsxs/manifests/%s.manifest", atl80_name);
    ABSENT(r);
    snprintf(r, sizeof(r), "drive_c/windows/winsxs/%s", atl80_name);
    ABSENT(r);

    /* No Wine Mono: the old link is removed. */
    ABSENT("drive_c/windows/mono/mono-2.0");

    /* ---- then an aarch64 session (clarification 9) ---- */
    unsetenv("MYIOSDECK_NOJIT_EMULATOR");
    link_at(prefix, "drive_c/windows/system32/foreign.dll", "/elsewhere/x86_64-vcruntime/foreign.dll");
    mkdir_at(bundle, "wine-mono/bin");
    file_at(bundle, "wine-mono/bin/libmono-2.0-x86.dll");
    wb_link_dll_farms(prefix, bundle, WB_SET_AARCH64);

    LINK_IS("drive_c/windows/system32/a.dll", "aarch64-windows/a.dll");
    LINK_IS("drive_c/windows/system32/aa_only.dll", "aarch64-windows/aa_only.dll");
    LINK_IS("drive_c/windows/system32/ec_only.dll", "arm64ec-windows/ec_only.dll");   /* other set */
    ABSENT("drive_c/windows/system32/vcruntime140_1.dll");        /* the bundle's x64 runtime link */
    snprintf(p, sizeof(p), "%s/drive_c/windows/system32/foreign.dll", prefix);
    CHECK(wb_is_symlink(p));                                      /* not the bundle's: left */
    snprintf(p, sizeof(p), "%s/drive_c/windows/system32/xtajit64.dll", prefix);
    CHECK_STR(link_target(p), "/emu/xtajit64.dll");               /* resolves nowhere, but no farm owns the name */
    LINK_IS("drive_c/windows/mono/mono-2.0", "wine-mono");

    /* A real mono-2.0 folder is kept. */
    snprintf(p, sizeof(p), "%s/drive_c/windows/mono/mono-2.0", prefix);
    unlink(p);
    mkdir_at(prefix, "drive_c/windows/mono/mono-2.0");
    wb_link_dll_farms(prefix, bundle, WB_SET_AARCH64);
    CHECK(wb_is_real_dir(p));

    /* After a reinstall (the bundle moved), every link is healed. */
    snprintf(want, sizeof(want), "%s-moved", bundle);
    if (rename(bundle, want) == 0) {
        strcat(bundle, "-moved");
        wb_link_dll_farms(prefix, bundle, WB_SET_AARCH64);
        LINK_IS("drive_c/windows/system32/a.dll", "aarch64-windows/a.dll");
        LINK_IS("drive_c/windows/syswow64/ntdll.dll", "i386-windows/ntdll.dll");
        LINK_IS("drive_c/windows/system32/ec_only.dll", "arm64ec-windows/ec_only.dll");   /* was dangling */
    } else {
        CHECK(!"rename of the test bundle failed");
    }
#undef LINK_IS
#undef ABSENT
}

/* ================= input queue ================= */

static void test_input_queue(void)
{
    stub_call_count = 0;
    CHECK(winios_pProcessEvents(0) == 0);

    wb_input_key(0x41, 1);
    wb_input_key(0x41, 0);
    wb_input_pointer(5, 6, 0x0800, 120);
    wb_input_touch(0, 10, 20);
    wb_input_touch(1, 11, 21);
    wb_input_touch(2, 12, 22);
    wb_input_touch(7, 0, 0);                             /* unknown phase: nothing */
    CHECK(winios_pProcessEvents(0) == 1);
    CHECK(stub_call_count == 6);
    if (stub_call_count == 6) {
        CHECK(stub_calls[0].kind == 'K' && stub_calls[0].a == 0x41 && stub_calls[0].flags == 0);
        CHECK(stub_calls[1].kind == 'K' && stub_calls[1].flags == 0x0002);
        CHECK(stub_calls[2].kind == 'M' && stub_calls[2].a == 5 && stub_calls[2].b == 6 &&
              stub_calls[2].flags == 0x0800 && stub_calls[2].data == 120);
        CHECK(stub_calls[3].flags == (0x0001 | 0x0002 | 0x8000) && stub_calls[3].a == 10);
        CHECK(stub_calls[4].flags == (0x0001 | 0x8000));
        CHECK(stub_calls[5].flags == (0x0004 | 0x8000) && stub_calls[5].b == 22);
    }
    CHECK(winios_pProcessEvents(0) == 0);

    /* Full ring: the new events are dropped, the oldest 256 are kept in order. */
    stub_call_count = 0;
    for (int i = 0; i < 300; i++) wb_input_pointer(i, 0, 0x0001, 0);
    CHECK(winios_pProcessEvents(0) == 1);
    CHECK(stub_call_count == 256);
    CHECK(stub_calls[0].a == 0 && stub_calls[255].a == 255);

    /* The other hooks. */
    CHECK(winios_pCreateWindow(NULL) == 1);
    CHECK(winios_pShowWindow(NULL, 5, NULL, 0x40) == ~0u);
}

int main(void)
{
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/wbtestXXXXXX");
    if (!mkdtemp(g_tmp)) {
        perror("mkdtemp");
        return 2;
    }

    test_targz();
    test_args();
    test_workdir();
    test_pe_and_target();
    test_settings();
    test_steam();
    test_hostprobe();
    test_pads();
    test_picture();
    test_exit_status();
    test_debug_verbose();
    test_seed();
    test_farms();
    test_input_queue();

    printf("%d checks, %d failed (scratch folder %s)\n", g_checks, g_failures, g_tmp);
    return g_failures ? 1 : 0;
}
