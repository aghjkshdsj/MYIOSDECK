// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_logic.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <stdarg.h>

static int wb_b_failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); ++wb_b_failures; } } while (0)
void wb_b_log(const char *format, ...) { (void)format; }

static struct wb_b_config wb_b_config_text(const char *text) {
    FILE *file = tmpfile();
    struct wb_b_config cfg = {0};
    CHECK(file != NULL);
    if (!file) return cfg;
    fwrite(text, 1, strlen(text), file); rewind(file);
    CHECK(wb_b_cfg_read(file, &cfg) == 0); fclose(file);
    return cfg;
}
static void wb_b_test_config(void) {
    struct wb_b_config cfg = wb_b_config_text("\xef\xbb\xbf # ignored\n broken line\n k = first\nk= last = value \r\nflag=YeS\ni=42\ni2=999999999999999999999999\nempty=\n");
    char out[8];
    CHECK(wb_b_cfg_get(&cfg, "k", out, sizeof out)); CHECK(!strcmp(out, "last = "));
    CHECK(!strcmp(wb_b_cfg_value(&cfg, "k"), "last = value"));
    CHECK(wb_b_cfg_get(&cfg, "empty", out, sizeof out) && !*out);
    CHECK(!wb_b_cfg_get(&cfg, "missing", out, sizeof out)); CHECK(!*out);
    CHECK(wb_b_cfg_bool(&cfg, "flag")); CHECK(wb_b_cfg_int(&cfg, "i", -1) == 42);
    CHECK(wb_b_cfg_int(&cfg, "i2", -1) == -1);
    CHECK(wb_b_sync_default(&cfg, 0)); CHECK(!wb_b_sync_default(&cfg, 1));
    wb_b_cfg_free(&cfg);
    const char *suppressed[] = {"inproc-sync=true", "inproc-sync=false", "env.MADEIRA_FASTSYNC=", "inproc-sync=off\nenv.MADEIRA_FASTSYNC=auto"};
    for (size_t i = 0; i < sizeof suppressed / sizeof suppressed[0]; ++i) {
        cfg = wb_b_config_text(suppressed[i]); CHECK(!wb_b_sync_default(&cfg, 0)); wb_b_cfg_free(&cfg);
    }
    cfg = wb_b_config_text("env.MADEIRA_FASTSYNC=first\nenv.MADEIRA_FASTSYNC=last");
    CHECK(!strcmp(wb_b_cfg_value(&cfg, "env.MADEIRA_FASTSYNC"), "last")); wb_b_cfg_free(&cfg);
}
static void wb_b_test_arguments_and_paths(void) {
    char *args[64] = {0};
    int n = wb_b_split_args("  a\t\"b c\" \"\" x\" y\"z \"tail", args, 64);
    CHECK(n == 5);
    if (n == 5) {
        CHECK(!strcmp(args[0], "a")); CHECK(!strcmp(args[1], "b c")); CHECK(!strcmp(args[2], ""));
        CHECK(!strcmp(args[3], "x yz")); CHECK(!strcmp(args[4], "tail"));
    }
    if (n >= 0) wb_b_free_args(args, n);
    n = wb_b_split_args("a b c", args, 2); CHECK(n == 2); if (n >= 0) wb_b_free_args(args, n);
    CHECK(wb_b_split_args(NULL, args, 64) == 0);
    char *folder = wb_b_workdir("c:\\games\\title\\\\", "C:\\fallback\\game.exe");
    CHECK(folder && !strcmp(folder, "c:\\games\\title")); free(folder);
    folder = wb_b_workdir("C:\\..\\escape", "C:\\game.exe"); CHECK(folder && !strcmp(folder, "C:\\")); free(folder);
    folder = wb_b_workdir("C:\\", "C:\\games\\game.exe"); CHECK(folder && !strcmp(folder, "C:\\games")); free(folder);
    CHECK(wb_b_workdir(NULL, "notepad.exe") == NULL);
    CHECK(!wb_b_c_path("D:\\x", 510, 1)); CHECK(!wb_b_c_path("C:\\x..y", 510, 1));
    CHECK(wb_b_c_path("c:\\x", 510, 1));
    char *path = wb_b_unix_c_path("/prefix", "C:\\games\\a.exe"); CHECK(path && !strcmp(path, "/prefix/drive_c/games/a.exe")); free(path);
    path = wb_b_windows_path("notepad.exe", 1); CHECK(path && !strcmp(path, "C:\\windows\\syswow64\\notepad.exe")); free(path);
    path = wb_b_windows_path("cmd.exe", 0); CHECK(path && !strcmp(path, "C:\\windows\\system32\\cmd.exe")); free(path);
    path = wb_b_windows_path("Z:\\hello.exe", 0); CHECK(path && !strcmp(path, "Z:\\hello.exe")); free(path);
    CHECK(wb_b_steam_identity("0000123", "c:\\Games\\Title"));
    CHECK(!wb_b_steam_identity("12345678901", "C:\\x")); CHECK(!wb_b_steam_identity("1x", "C:\\x"));
    CHECK(!wb_b_steam_identity("123", "C:\\..\\x"));
}
static void wb_b_test_pe(const char *root) {
    char path[512]; snprintf(path, sizeof path, "%s/target.exe", root);
    unsigned char image[128] = {0}; image[0] = 'M'; image[1] = 'Z'; image[0x3c] = 80;
    memcpy(image + 80, "PE\0\0", 4); image[84] = 0x4c; image[85] = 1;
    FILE *f = fopen(path, "wb"); CHECK(f != NULL); if (!f) return;
    fwrite(image, 1, sizeof image, f); fclose(f); CHECK(wb_b_pe_machine(path) == 0x014c);
    image[85] = 0x86; image[84] = 0x64;
    f = fopen(path, "wb"); fwrite(image, 1, sizeof image, f); fclose(f); CHECK(wb_b_pe_machine(path) == 0x8664);
    image[0x3f] = 2; // Offset is now greater than16MB.
    f = fopen(path, "wb"); fwrite(image, 1, sizeof image, f); fclose(f); CHECK(wb_b_pe_machine(path) == -1);
    f = fopen(path, "wb"); fwrite("MZ", 1, 2, f); fclose(f); CHECK(wb_b_pe_machine(path) == -1);
    unlink(path); CHECK(wb_b_pe_machine(path) == -1);
}
static void wb_b_test_pictures(void) {
    struct wb_b_picture list[] = {{1,2,800,600,0}, {3,4,100,100,1}, {5,6,600,400,1}, {7,8,1024,768,0}};
    CHECK(wb_b_choose_picture(list, 4, 800,600,1024,768) == 2);
    list[1].metal = list[2].metal = 0;
    CHECK(wb_b_choose_picture(list, 4, 800,600,1024,768) == 0);
    CHECK(wb_b_choose_picture(list, 4, 100,100,1024,768) == -1);
    CHECK(wb_b_choose_picture(list, 4, 1024,768,1024,768) == 3);
    CHECK(wb_b_choose_picture(NULL, 0, 1,1,1,1) == -1);
    struct wb_b_picture quarter[] = {{0,0,512,384,0}};
    CHECK(wb_b_choose_picture(quarter,1,512,384,1024,768) == 0);
}
static void wb_b_tar_header(gzFile gz, const char *name, char type, size_t size, const char *prefix) {
    unsigned char block[512] = {0};
    snprintf((char *)block, 100, "%s", name); snprintf((char *)block + 124, 12, "%011lo", (unsigned long)size);
    block[156] = (unsigned char)type;
    if (prefix) { memcpy(block + 257, "ustar\0", 6); snprintf((char *)block + 345, 155, "%s", prefix); }
    CHECK(gzwrite(gz, block, 512) == 512);
}
static void wb_b_tar_data(gzFile gz, const char *text, size_t size) {
    unsigned char block[512] = {0}; CHECK(size <= sizeof block);
    if (size <= sizeof block) memcpy(block, text, size);
    CHECK(gzwrite(gz, block, 512) == 512);
}
static void wb_b_test_tar(const char *root) {
    char archive[512], file[512], target[512], outside[512];
    snprintf(archive,sizeof archive,"%s/template.gz",root); snprintf(target,sizeof target,"%s/destination",root);
    CHECK(!mkdir(target,0755));
    gzFile gz = gzopen(archive,"wb"); CHECK(gz != NULL); if (!gz) return;
    wb_b_tar_header(gz,"./prefix/dir/",'5',0,NULL);
    wb_b_tar_header(gz,"dir/a.txt",'0',5,"prefix"); wb_b_tar_data(gz,"hello",5);
    wb_b_tar_header(gz,"prefix/dir/../escape",'0',3,NULL); wb_b_tar_data(gz,"bad",3);
    wb_b_tar_header(gz,"/absolute",'0',3,NULL); wb_b_tar_data(gz,"bad",3);
    wb_b_tar_header(gz,"prefix/ignored",'x',4,NULL); wb_b_tar_data(gz,"meta",4);
    wb_b_tar_header(gz,"prefix/link",'2',0,NULL);
    unsigned char zero[512] = {0}; gzwrite(gz,zero,512); gzclose(gz);
    size_t nf=0,nd=0; CHECK(wb_b_extract_tar_gz(archive,target,&nf,&nd) == 0); CHECK(nf == 1 && nd == 1);
    snprintf(file,sizeof file,"%s/dir/a.txt",target); FILE *f=fopen(file,"rb"); CHECK(f != NULL);
    if (f) { char text[6]={0}; CHECK(fread(text,1,5,f)==5); CHECK(!strcmp(text,"hello")); fclose(f); }
    struct stat st={0}; CHECK(!stat(file,&st)); CHECK((st.st_mode & 0777) == 0644);
    unlink(file); snprintf(file,sizeof file,"%s/dir",target); rmdir(file);
    snprintf(file,sizeof file,"%s/escape",target); CHECK(access(file,F_OK) != 0);
    // A safe-looking parent component must not follow an existing symlink.
    snprintf(outside,sizeof outside,"%s/outside",root); CHECK(!mkdir(outside,0755));
    snprintf(file,sizeof file,"%s/jump",target); CHECK(!symlink("../outside",file));
    gz=gzopen(archive,"wb"); wb_b_tar_header(gz,"prefix/jump/escape",'0',3,NULL); wb_b_tar_data(gz,"bad",3);
    wb_b_tar_header(gz,"prefix/after",'0',2,NULL); wb_b_tar_data(gz,"ok",2); gzwrite(gz,zero,512); gzclose(gz);
    CHECK(wb_b_extract_tar_gz(archive,target,NULL,NULL)==-1);
    snprintf(file,sizeof file,"%s/after",target); CHECK(access(file,F_OK)==0); unlink(file);
    snprintf(file,sizeof file,"%s/escape",outside); CHECK(access(file,F_OK) != 0);
    snprintf(file,sizeof file,"%s/jump",target); unlink(file); rmdir(outside);
    // Invalid octal, truncated header and truncated payload are errors.
    unsigned char bad[512]={0}; memcpy(bad,"prefix/bad",10); bad[124]='8';
    gz=gzopen(archive,"wb"); gzwrite(gz,bad,512); gzwrite(gz,zero,512); gzclose(gz); CHECK(wb_b_extract_tar_gz(archive,target,NULL,NULL)==-1);
    gz=gzopen(archive,"wb"); gzwrite(gz,zero,17); gzclose(gz); CHECK(wb_b_extract_tar_gz(archive,target,NULL,NULL)==-1);
    gz=gzopen(archive,"wb"); wb_b_tar_header(gz,"prefix/short",'0',513,NULL); wb_b_tar_data(gz,"a",1); gzclose(gz);
    CHECK(wb_b_extract_tar_gz(archive,target,NULL,NULL)==-1);
    snprintf(file,sizeof file,"%s/short",target); unlink(file); unlink(archive); rmdir(target);
}
int main(void) {
    char root[] = "./b-test-data-XXXXXX";
    if (!mkdtemp(root)) return 2;
    wb_b_test_arguments_and_paths(); wb_b_test_config(); wb_b_test_pe(root); wb_b_test_pictures(); wb_b_test_tar(root);
    CHECK(rmdir(root) == 0);
    return wb_b_failures ? 1 : 0;
}
