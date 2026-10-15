// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#include "b_internal.h"
#include "b_sxs_data.h"
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

static pthread_mutex_t wb_b_seed_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wb_b_seed_condition = PTHREAD_COND_INITIALIZER;
struct wb_b_seed_entry { char *prefix; int busy, done; struct wb_b_seed_entry *next; };
static struct wb_b_seed_entry *wb_b_seed_entries;

static BOOL wb_b_mkdir(NSString *path) {
    NSError *error = nil;
    BOOL ok = [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES
        attributes:@{NSFilePosixPermissions:@0755} error:&error];
    if (!ok) wb_b_log("[wb-prefix] mkdir %s: %s", path.fileSystemRepresentation, error.description.UTF8String);
    return ok;
}
static BOOL wb_b_link(NSString *source, NSString *destination) {
    NSFileManager *fm = NSFileManager.defaultManager;
    struct stat st;
    if (!lstat(destination.fileSystemRepresentation, &st)) {
        NSError *error = nil;
        if (![fm removeItemAtPath:destination error:&error]) {
            wb_b_log("[wb-prefix] remove %s: %s", destination.fileSystemRepresentation, error.description.UTF8String); return NO;
        }
    }
    NSError *error = nil;
    BOOL ok = [fm createSymbolicLinkAtPath:destination withDestinationPath:source error:&error];
    if (!ok) wb_b_log("[wb-prefix] link %s: %s", destination.fileSystemRepresentation, error.description.UTF8String);
    return ok;
}
static NSArray<NSString *> *wb_b_files(NSString *directory) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray<NSString *> *entries = [fm contentsOfDirectoryAtPath:directory error:nil];
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    for (NSString *name in entries) {
        if ([name hasPrefix:@"."]) continue;
        BOOL isDirectory = NO;
        if ([fm fileExistsAtPath:[directory stringByAppendingPathComponent:name] isDirectory:&isDirectory] && !isDirectory) [files addObject:name];
    }
    return [files sortedArrayUsingSelector:@selector(compare:)];
}
static void wb_b_farm(NSString *source, NSString *destination, BOOL onlyMissing) {
    if (!wb_b_mkdir(destination)) return;
    NSUInteger count = 0;
    for (NSString *name in wb_b_files(source)) {
        NSString *target = [destination stringByAppendingPathComponent:name];
        if (onlyMissing && [NSFileManager.defaultManager fileExistsAtPath:target]) continue;
        if (wb_b_link([source stringByAppendingPathComponent:name], target)) ++count;
    }
    wb_b_log("[wb-prefix] farm %s count=%lu", destination.fileSystemRepresentation, (unsigned long)count);
}
static void wb_b_wmi(NSString *source, NSString *system) {
    NSString *destination = [system stringByAppendingPathComponent:@"wbem"];
    if (!wb_b_mkdir(destination)) return;
    NSUInteger count = 0;
    for (NSString *name in @[@"wbemprox.dll", @"wbemdisp.dll", @"wmiutils.dll", @"wmic.exe", @"mofcomp.exe"]) {
        NSString *file = [source stringByAppendingPathComponent:name];
        if ([NSFileManager.defaultManager fileExistsAtPath:file] && wb_b_link(file, [destination stringByAppendingPathComponent:name])) ++count;
    }
    wb_b_log("[wb-prefix] WMI farm %s count=%lu", destination.fileSystemRepresentation, (unsigned long)count);
}
static NSString *wb_b_assembly_source(const struct wb_b_assembly *a, const char *file, NSString *source) {
    const char *name = a->controls && !strcmp(file, "comctl32.dll") ? "comctl32_v6.dll" : file;
    return [source stringByAppendingPathComponent:[NSString stringWithUTF8String:name]];
}
static void wb_b_seed_sxs(NSString *windows, NSString *source) {
    NSString *root = [windows stringByAppendingPathComponent:@"winsxs"];
    NSString *manifests = [root stringByAppendingPathComponent:@"manifests"];
    if (!wb_b_mkdir(manifests)) return;
    unsigned int seeded = 0, skipped = 0;
    NSFileManager *fm = NSFileManager.defaultManager;
    for (size_t i = 0; i < sizeof wb_b_assemblies / sizeof wb_b_assemblies[0]; ++i) {
        const struct wb_b_assembly *a = &wb_b_assemblies[i];
        NSString *identity = [NSString stringWithUTF8String:a->name];
        NSString *folder = [NSString stringWithFormat:@"x86_%@_%s_%s_none_deadbeef", identity.lowercaseString, a->token, a->version];
        NSString *destination = [root stringByAppendingPathComponent:folder];
        NSString *manifestPath = [manifests stringByAppendingPathComponent:[folder stringByAppendingString:@".manifest"]];
        if (![fm fileExistsAtPath:wb_b_assembly_source(a, a->files[0], source)]) {
            // Remove a prior manifest too: never leave a declared assembly without its first DLL.
            [fm removeItemAtPath:manifestPath error:nil];
            [fm removeItemAtPath:destination error:nil];
            wb_b_log("[wb-prefix] SxS skip %s: first DLL missing", a->name); ++skipped; continue;
        }
        if (!wb_b_mkdir(destination)) { ++skipped; continue; }
        NSMutableString *xml = [NSMutableString stringWithFormat:
            @"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
             "  <assemblyIdentity type=\"win32\" name=\"%s\" version=\"%s\" processorArchitecture=\"x86\" publicKeyToken=\"%s\"/>\n",
            a->name, a->version, a->token];
        BOOL firstLinked = NO;
        for (size_t j = 0; a->files[j]; ++j) {
            NSString *name = [NSString stringWithUTF8String:a->files[j]];
            NSString *src = wb_b_assembly_source(a, a->files[j], source);
            NSString *dest = [destination stringByAppendingPathComponent:name];
            if (![fm fileExistsAtPath:src]) { [fm removeItemAtPath:dest error:nil]; continue; }
            if (!wb_b_link(src, dest)) continue;
            if (!j) firstLinked = YES;
            if (a->controls) {
                [xml appendFormat:@"  <file name=\"%@\">\n", name];
                for (size_t k = 0; k < sizeof wb_b_control_classes / sizeof wb_b_control_classes[0]; ++k)
                    [xml appendFormat:@"    <windowClass>%s</windowClass>\n", wb_b_control_classes[k]];
                [xml appendString:@"  </file>\n"];
            } else [xml appendFormat:@"  <file name=\"%@\"/>\n", name];
        }
        [xml appendString:@"</assembly>\n"];
        NSError *error = nil;
        if (!firstLinked || ![xml writeToFile:manifestPath atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
            [fm removeItemAtPath:manifestPath error:nil]; ++skipped;
            wb_b_log("[wb-prefix] SxS write failed %s: %s", a->name, error ? error.description.UTF8String : "first link failed");
        } else ++seeded;
    }
    wb_b_log("[wb-prefix] SxS seeded=%u skipped=%u", seeded, skipped);
}

void wb_seed_prefix(const char *prefix) {
    if (!prefix || !*prefix) return;
    struct wb_b_seed_entry *candidate = calloc(1, sizeof *candidate);
    if (!candidate) return;
    candidate->prefix = strdup(prefix);
    if (!candidate->prefix) { free(candidate); return; }
    pthread_mutex_lock(&wb_b_seed_lock);
    struct wb_b_seed_entry *entry = wb_b_seed_entries;
    while (entry && strcmp(entry->prefix, prefix)) entry = entry->next;
    if (!entry) { entry = candidate; candidate = NULL; entry->next = wb_b_seed_entries; wb_b_seed_entries = entry; }
    while (entry->busy) pthread_cond_wait(&wb_b_seed_condition, &wb_b_seed_lock);
    int wasDone = entry->done;
    entry->busy = 1;
    pthread_mutex_unlock(&wb_b_seed_lock);
    if (candidate) { free(candidate->prefix); free(candidate); }
    if (wasDone) {
        size_t capacity = strlen(prefix) + 16;
        char *link = malloc(capacity);
        struct stat st;
        int present = 0;
        if (link) { snprintf(link, capacity, "%s/dosdevices/c:", prefix); present = !lstat(link, &st); free(link); }
        if (present) {
            pthread_mutex_lock(&wb_b_seed_lock);
            entry->busy = 0; pthread_cond_broadcast(&wb_b_seed_condition);
            pthread_mutex_unlock(&wb_b_seed_lock);
            return;
        }
    }
    BOOL success = YES;
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *root = [NSString stringWithUTF8String:prefix];
        if (!root || !wb_b_mkdir(root)) success = NO;
        else {
            NSString *stamp = [root stringByAppendingPathComponent:@".update-timestamp"];
            if (![fm fileExistsAtPath:stamp]) {
                NSString *archive = [NSBundle.mainBundle pathForResource:@"prefix-template.tar" ofType:@"gz"];
                if (!archive) wb_b_log("[wb-prefix] prefix-template.tar.gz missing; continuing");
                else {
                    int result = wb_b_extract_tar_gz(archive.fileSystemRepresentation, prefix, NULL, NULL);
                    wb_b_log("[wb-prefix] template extraction result=%d", result);
                    if (result) success = NO;
                }
            }
            NSString *devices = [root stringByAppendingPathComponent:@"dosdevices"];
            if (!wb_b_mkdir(devices) || !wb_b_link(@"../drive_c", [devices stringByAppendingPathComponent:@"c:"])) success = NO;
            const char *setting = getenv("MADEIRA_PROFILE_LOCALLOW");
            char localLow[64] = {0};
            {
                const char *docs = getenv("MADEIRA_DOCS_DIR");
                if (docs) {
                    NSString *cfgPath = [[NSString stringWithUTF8String:docs] stringByAppendingPathComponent:@"madeira.cfg"];
                    FILE *file = fopen(cfgPath.fileSystemRepresentation, "rb");
                    if (file) {
                        struct wb_b_config cfg;
                        if (!wb_b_cfg_read(file, &cfg)) { wb_b_cfg_get(&cfg, "env.MADEIRA_PROFILE_LOCALLOW", localLow, sizeof localLow); wb_b_cfg_free(&cfg); }
                        fclose(file);
                    }
                }
            }
            NSString *users = [root stringByAppendingPathComponent:@"drive_c/users"];
            if ((!setting || setting[0] != '0') && localLow[0] != '0') {
                const char *user = getenv("USER");
                if (!user || !*user) { struct passwd *pw = getpwuid(getuid()); user = pw ? pw->pw_name : NULL; }
                if (!user || !*user) user = "wine";
                const char *tail = user;
                for (const char *p = user; *p; ++p) if (*p == '/' || *p == '\\') tail = p + 1;
                // Empty or traversal-like account names cannot escape the profile root.
                if (!*tail || !strcmp(tail, ".") || !strcmp(tail, "..")) tail = "wine";
                NSString *account = [NSString stringWithUTF8String:tail] ?: @"wine";
                if (!wb_b_mkdir([[users stringByAppendingPathComponent:account] stringByAppendingPathComponent:@"AppData/LocalLow"])) success = NO;
            }
            for (NSString *user in [fm contentsOfDirectoryAtPath:users error:nil]) {
                NSString *home = [users stringByAppendingPathComponent:user];
                BOOL directory = NO;
                if (![fm fileExistsAtPath:home isDirectory:&directory] || !directory) continue;
                for (NSString *folder in @[@"Documents", @"Desktop", @"Downloads", @"Music", @"Pictures", @"Videos"]) {
                    NSString *path = [home stringByAppendingPathComponent:folder];
                    if ([fm fileExistsAtPath:path]) continue;
                    struct stat st;
                    if (!lstat(path.fileSystemRepresentation, &st)) {
                        if (!S_ISLNK(st.st_mode)) continue;
                        NSString *old = [fm destinationOfSymbolicLinkAtPath:path error:nil];
                        wb_b_log("[wb-prefix] replace dangling shell link %s -> %s", path.fileSystemRepresentation, old.UTF8String ?: "(unknown)");
                        if (![fm removeItemAtPath:path error:nil]) { success = NO; continue; }
                    }
                    if (!wb_b_mkdir(path)) success = NO;
                }
            }
        }
    }
    pthread_mutex_lock(&wb_b_seed_lock);
    entry->done = success ? 1 : 0; entry->busy = 0;
    pthread_cond_broadcast(&wb_b_seed_condition);
    pthread_mutex_unlock(&wb_b_seed_lock);
}

void wb_b_link_farms(const char *prefix, const char *set) {
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *bundle = NSBundle.mainBundle.bundlePath;
        NSString *windows = [[NSString stringWithUTF8String:prefix] stringByAppendingPathComponent:@"drive_c/windows"];
        NSString *system = [windows stringByAppendingPathComponent:@"system32"];
        NSString *chosen = [bundle stringByAppendingPathComponent:[NSString stringWithUTF8String:set]];
        NSString *other = [bundle stringByAppendingPathComponent:!strcmp(set, "arm64ec-windows") ? @"aarch64-windows" : @"arm64ec-windows"];
        if (!strcmp(set, "aarch64-windows")) {
            for (NSString *name in [fm contentsOfDirectoryAtPath:system error:nil]) {
                NSString *path = [system stringByAppendingPathComponent:name];
                struct stat st;
                if (lstat(path.fileSystemRepresentation, &st) || !S_ISLNK(st.st_mode)) continue;
                NSString *target = [fm destinationOfSymbolicLinkAtPath:path error:nil];
                if ([target containsString:@"/x86_64-vcruntime/"]) [fm removeItemAtPath:path error:nil];
            }
        }
        wb_b_farm(chosen, system, NO);
        const char *vars[] = {"MYIOSDECK_NOJIT_EMULATOR", "MYIOSDECK_NOJIT_WOW_CPU"};
        NSArray<NSString *> *names = @[@"xtajit64.dll", @"xtajit.dll"];
        for (size_t i = 0; i < 2; ++i) {
            const char *source = getenv(vars[i]);
            if (source && *source) {
                BOOL ok = wb_b_link([NSString stringWithUTF8String:source], [system stringByAppendingPathComponent:names[i]]);
                wb_b_log("[wb-prefix] %s override %s", vars[i], ok ? "ok" : "FAILED");
            }
        }
        wb_b_wmi(chosen, system);
        wb_b_farm(other, system, YES);
        wb_b_farm([bundle stringByAppendingPathComponent:@"arm64ec-windows"], [windows stringByAppendingPathComponent:@"sysx64"], NO);
        wb_b_farm([bundle stringByAppendingPathComponent:@"aarch64-windows"], [windows stringByAppendingPathComponent:@"sysaa64"], NO);
        NSString *x86 = [bundle stringByAppendingPathComponent:@"i386-windows"];
        BOOL isDirectory = NO;
        if ([fm fileExistsAtPath:x86 isDirectory:&isDirectory] && isDirectory) {
            NSString *wow = [windows stringByAppendingPathComponent:@"syswow64"];
            wb_b_farm(x86, wow, NO); wb_b_wmi(x86, wow); wb_b_seed_sxs(windows, x86);
        }
        NSString *mono = [bundle stringByAppendingPathComponent:@"wine-mono"];
        if (![fm fileExistsAtPath:[mono stringByAppendingPathComponent:@"bin/libmono-2.0-x86.dll"]]) {
            NSString *support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
            mono = support ? [support stringByAppendingPathComponent:@"WineMono/wine-mono"] : nil;
        }
        NSString *monoParent = [windows stringByAppendingPathComponent:@"mono"];
        NSString *monoDest = [monoParent stringByAppendingPathComponent:@"mono-2.0"];
        struct stat st;
        BOOL realDirectory = !lstat(monoDest.fileSystemRepresentation, &st) && S_ISDIR(st.st_mode);
        if (!realDirectory) {
            if (mono && [fm fileExistsAtPath:[mono stringByAppendingPathComponent:@"bin/libmono-2.0-x86.dll"]]) {
                if (wb_b_mkdir(monoParent)) wb_b_link(mono, monoDest);
            } else if (!lstat(monoDest.fileSystemRepresentation, &st) && S_ISLNK(st.st_mode)) [fm removeItemAtPath:monoDest error:nil];
        }
        if (!strcmp(set, "arm64ec-windows")) {
            NSString *runtime = [bundle stringByAppendingPathComponent:@"x86_64-vcruntime"];
            NSUInteger count = 0;
            for (NSString *name in wb_b_files(runtime)) {
                NSString *lower = name.lowercaseString;
                if (![lower hasSuffix:@".dll"] || [lower isEqualToString:@"vcruntime140.dll"] || [lower isEqualToString:@"msvcp140.dll"]) continue;
                if (wb_b_link([runtime stringByAppendingPathComponent:name], [system stringByAppendingPathComponent:name])) ++count;
            }
            wb_b_log("[wb-prefix] x86_64 runtime overrides=%lu", (unsigned long)count);
        }
    }
}
