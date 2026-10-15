// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
#import <AVFAudio/AVFAudio.h>
#import <os/log.h>
#include "b_internal.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <errno.h>
#include <pthread/qos.h>

_Thread_local jmp_buf wine_ios_exit_jmpbuf;
_Thread_local volatile int wine_ios_exit_code;
_Thread_local pthread_t wine_ios_main_thread;
_Thread_local int wine_ios_exit_initialized;
volatile int g_wineserver_should_stop;
extern int64_t fex_get_jit_write_offset(void);

static char *wb_b_documents;
static char *wb_b_log_name;
static pthread_once_t wb_b_paths_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t wb_b_log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wb_b_server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wb_b_server_condition = PTHREAD_COND_INITIALIZER;
static pthread_t wb_b_server_thread;
static int wb_b_server_created, wb_b_server_starting, wb_b_server_joining;
static _Atomic int wb_b_server_running;
static _Atomic int wb_b_process_running;
static _Atomic uint64_t wb_b_crash; // bit32 valid, low32 exact NTSTATUS, one atomic snapshot.
static pthread_mutex_t wb_b_phase_lock = PTHREAD_MUTEX_INITIALIZER;
static double wb_b_phase_origin;
static int wb_b_phase_started;

static const char *const wb_b_launch_keys[] = {
    "MADEIRA_FASTSYNC", "MADEIRA_FASTSYNC_SEM", "MADEIRA_CPU_COUNT", "DXMT_D9_ANISO_LIMIT",
    "FEX_X87REDUCEDPRECISION", "MADEIRA_DINPUT_PAD", "MADEIRA_FEX_AVX", "MADEIRA_FRAMEGEN",
    "DXMT_ENABLE_NVEXT", "DXMT_WSI_MONITOR_IDENTITY", "DXMT_WSI_MODE_TABLE"
};
#define B_KEY_COUNT (sizeof wb_b_launch_keys / sizeof wb_b_launch_keys[0])
struct wb_b_launch { char *prefix; int client; unsigned char app_set[B_KEY_COUNT]; };

static void wb_b_init_paths(void) {
    @autoreleasepool {
        NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        if (!docs) docs = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents"];
        wb_b_documents = strdup(docs.fileSystemRepresentation);
        wb_b_log_name = strdup([docs stringByAppendingPathComponent:@"wine-log.txt"].fileSystemRepresentation);
    }
}
__attribute__((constructor)) static void wb_b_early_docs(void) {
    // Capture Foundation's container directory before any thread changes HOME.
    pthread_once(&wb_b_paths_once, wb_b_init_paths);
    if (!getenv("MADEIRA_DOCS_DIR")) {
        const char *home = getenv("HOME");
        if (home) {
            size_t cap = strlen(home) + 11;
            char *docs = malloc(cap);
            if (docs) { snprintf(docs, cap, "%s/Documents", home); setenv("MADEIRA_DOCS_DIR", docs, 1); free(docs); }
        }
    }
}
const char *wb_log_path(void) {
    pthread_once(&wb_b_paths_once, wb_b_init_paths);
    return wb_b_log_name ? wb_b_log_name : "wine-log.txt";
}
void wb_b_log(const char *format, ...) {
    char message[8192], line[8256];
    va_list ap; va_start(ap, format); vsnprintf(message, sizeof message, format, ap); va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    snprintf(line, sizeof line, "[%02d:%02d:%02d.%03ld] %s", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, message);
    const char *path = wb_log_path();
    pthread_mutex_lock(&wb_b_log_lock);
    // Reopen for every line: the app may unlink the previous run's log before boot.
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", line); fflush(f); fclose(f); }
    pthread_mutex_unlock(&wb_b_log_lock);
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "%{public}s", line);
    wine_ui_log(line);
}
void wb_b_redirect_log(int include_stdout) {
    int fd = open(wb_log_path(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { wb_b_log("[wb-launch] log open failed errno=%d", errno); return; }
    if (dup2(fd, STDERR_FILENO) < 0) wb_b_log("[wb-launch] stderr redirect failed errno=%d", errno);
    if (include_stdout && dup2(fd, STDOUT_FILENO) < 0) wb_b_log("[wb-launch] stdout redirect failed errno=%d", errno);
    if (fd != STDERR_FILENO && (!include_stdout || fd != STDOUT_FILENO)) close(fd);
}
static double wb_b_monotonic(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
void winios_phase(const char *name) {
    double now = wb_b_monotonic();
    pthread_mutex_lock(&wb_b_phase_lock);
    if (!wb_b_phase_started) { wb_b_phase_started = 1; wb_b_phase_origin = now; }
    double elapsed = now > wb_b_phase_origin ? now - wb_b_phase_origin : 0;
    pthread_mutex_unlock(&wb_b_phase_lock);
    wb_b_log("[phase] %s t+%.3fs", name ? name : "(null)", elapsed);
}
void wine_launched_process_did_exit(int status) {
    uint32_t s = (uint32_t)status;
    if (s >= 0xc0000000u) atomic_store_explicit(&wb_b_crash, (UINT64_C(1) << 32) | s, memory_order_release);
}
void wb_exit_status_reset(void) { atomic_store_explicit(&wb_b_crash, 0, memory_order_release); }
int wb_crash_exit_status(uint32_t *status) {
    uint64_t value = atomic_load_explicit(&wb_b_crash, memory_order_acquire);
    if (!(value >> 32)) return 0;
    if (status) *status = (uint32_t)value;
    return 1;
}
int wine_crash_exit_status(uint32_t *status) { return wb_crash_exit_status(status); }
int wb_server_is_running(void) { return atomic_load_explicit(&wb_b_server_running, memory_order_acquire); }
int wb_server_is_ready(void) { return __atomic_load_n(&wineserver_ready, __ATOMIC_ACQUIRE); }
int wb_process_is_running(void) { return atomic_load_explicit(&wb_b_process_running, memory_order_acquire); }
void fatal_error(const char *format, ...) {
    char message[8192]; va_list ap; va_start(ap, format); vsnprintf(message, sizeof message, format, ap); va_end(ap);
    wb_b_log("[wb-server] FATAL: %s", message);
    pthread_exit(NULL);
}

static void wb_b_server_cleanup(void *prefix) {
    free(prefix);
    __atomic_store_n(&wineserver_ready, 0, __ATOMIC_RELEASE);
    atomic_store_explicit(&wb_b_server_running, 0, memory_order_release);
}
static void *wb_b_server_main(void *prefix) {
    pthread_cleanup_push(wb_b_server_cleanup, prefix);
    @autoreleasepool {
        wb_b_redirect_log(0);
        wineserver_log_set_file(wb_log_path());
        setenv("WINEPREFIX", prefix, 1); setenv("HOME", prefix, 1);
        if (mkdir(prefix, 0755) && errno != EEXIST) wb_b_log("[wb-server] mkdir failed errno=%d", errno);
        NSString *nls = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"nls"];
        wineserver_set_nls_dir(nls.fileSystemRepresentation);
        foreground = 1;
        char *argv[] = {"wineserver", "--foreground", NULL};
        int result = wineserver_main(2, argv);
        wb_b_log("[wb-server] returned %d", result);
    }
    pthread_cleanup_pop(1);
    return NULL;
}
int wb_server_start(const char *prefix) {
    if (wb_server_is_running()) return 0;
    if (!prefix || !*prefix) return -1;
    char *copy = strdup(prefix);
    if (!copy) return -1;
    pthread_mutex_lock(&wb_b_server_lock);
    while (wb_b_server_joining) pthread_cond_wait(&wb_b_server_condition, &wb_b_server_lock);
    if (wb_b_server_starting || wb_server_is_running()) { pthread_mutex_unlock(&wb_b_server_lock); free(copy); return 0; }
    wb_b_server_starting = 1;
    pthread_t prior = wb_b_server_thread;
    int reap = wb_b_server_created;
    wb_b_server_created = 0;
    pthread_mutex_unlock(&wb_b_server_lock);
    if (reap) pthread_join(prior, NULL);
    __atomic_store_n(&wineserver_ready, 0, __ATOMIC_RELEASE);
    atomic_store_explicit(&wb_b_server_running, 1, memory_order_release);
    wb_seed_prefix(copy);
    __atomic_store_n(&g_wineserver_should_stop, 0, __ATOMIC_RELEASE);
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    int attr_ready = error == 0;
    if (!error) error = pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t thread;
    if (!error) error = pthread_create(&thread, &attr, wb_b_server_main, copy);
    if (attr_ready) pthread_attr_destroy(&attr);
    pthread_mutex_lock(&wb_b_server_lock);
    if (!error) { wb_b_server_thread = thread; wb_b_server_created = 1; }
    else atomic_store_explicit(&wb_b_server_running, 0, memory_order_release);
    wb_b_server_starting = 0;
    pthread_cond_broadcast(&wb_b_server_condition);
    pthread_mutex_unlock(&wb_b_server_lock);
    if (error) { free(copy); wb_b_log("[wb-server] thread creation failed %d", error); return -1; }
    return 0;
}
void wb_server_stop(void) {
    pthread_mutex_lock(&wb_b_server_lock);
    while (wb_b_server_starting || wb_b_server_joining) pthread_cond_wait(&wb_b_server_condition, &wb_b_server_lock);
    __atomic_store_n(&g_wineserver_should_stop, 1, __ATOMIC_RELEASE);
    if (!wb_b_server_created) { pthread_mutex_unlock(&wb_b_server_lock); return; }
    pthread_t thread = wb_b_server_thread;
    if (pthread_equal(thread, pthread_self())) { pthread_mutex_unlock(&wb_b_server_lock); return; }
    wb_b_server_joining = 1;
    pthread_mutex_unlock(&wb_b_server_lock);
    wb_b_log("[wb-server] joining");
    int error = pthread_join(thread, NULL);
    wb_b_log("[wb-server] join completed result=%d", error);
    pthread_mutex_lock(&wb_b_server_lock);
    wb_b_server_created = 0; wb_b_server_joining = 0;
    atomic_store_explicit(&wb_b_server_running, 0, memory_order_release);
    pthread_cond_broadcast(&wb_b_server_condition);
    pthread_mutex_unlock(&wb_b_server_lock);
}

static struct wb_b_config wb_b_load_config(void) {
    struct wb_b_config cfg = {0};
    pthread_once(&wb_b_paths_once, wb_b_init_paths);
    NSString *path = [[NSString stringWithUTF8String:wb_b_documents ? wb_b_documents : ""] stringByAppendingPathComponent:@"madeira.cfg"];
    FILE *file = fopen(path.fileSystemRepresentation, "rb");
    if (file) { if (wb_b_cfg_read(file, &cfg)) wb_b_log("[wb-launch] settings read failed"); fclose(file); }
    return cfg;
}
static void wb_b_export_config(const struct wb_b_launch *launch) {
    struct wb_b_config cfg = wb_b_load_config();
    for (size_t i = 0; i < cfg.count; ++i) {
        const char *key = cfg.items[i].key;
        if (strncmp(key, "env.", 4) || !key[4]) continue;
        const char *name = key + 4;
        int preserve = 0;
        for (size_t j = 0; j < B_KEY_COUNT; ++j) if (!strcmp(name, wb_b_launch_keys[j]) && launch->app_set[j]) preserve = 1;
        if (preserve) { wb_b_log("[wb-launch] config env.%s ignored: app value retained", name); continue; }
        int result = setenv(name, cfg.items[i].value, 1);
        wb_b_log("[wb-launch] export %s=%s result=%d", name, cfg.items[i].value, result);
    }
    if (wb_b_sync_default(&cfg, getenv("MADEIRA_FASTSYNC") != NULL)) {
        setenv("MADEIRA_FASTSYNC", "auto", 1); wb_b_log("[wb-launch] default MADEIRA_FASTSYNC=auto");
    }
    wb_b_cfg_free(&cfg);
}
static void wb_b_steam(void) {
    const char *dock = getenv("MADEIRA_DOCK_SESSION");
    const char *idValue = getenv("MADEIRA_STEAM_APPID"), *pathValue = getenv("MADEIRA_STEAM_APPPATH");
    char *id = idValue ? strdup(idValue) : NULL, *path = pathValue ? strdup(pathValue) : NULL;
    if ((!dock || strcmp(dock, "1")) && wb_b_steam_identity(id, path)) {
        setenv("SteamAppPath", path, 1); setenv("SteamGameId", id, 1); setenv("SteamAppId", id, 1);
        wb_b_log("[wb-launch] Steam identity id=%s path=%s", id, path);
    } else { unsetenv("SteamAppPath"); unsetenv("SteamGameId"); unsetenv("SteamAppId"); }
    unsetenv("MADEIRA_STEAM_APPID"); unsetenv("MADEIRA_STEAM_APPPATH");
    free(id); free(path);
}
static void wb_b_cpu_probe(void) {
    const char *keys[] = {"AFP", "FLAGM", "FLAGM2", "FCMA", "RCPC", "LRCPC2", "AES", "PMULL", "SHA", "CRC", "ATOMICS"};
    const char *names[] = {"hw.optional.arm.FEAT_AFP", "hw.optional.arm.FEAT_FlagM", "hw.optional.arm.FEAT_FlagM2", "hw.optional.arm.FEAT_FCMA",
        "hw.optional.arm.FEAT_LRCPC", "hw.optional.arm.FEAT_LRCPC2", "hw.optional.arm.FEAT_AES", "hw.optional.arm.FEAT_PMULL",
        "hw.optional.arm.FEAT_SHA256", "hw.optional.armv8_crc32", "hw.optional.arm.FEAT_LSE"};
    char result[256] = {0};
    size_t used = 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        int32_t value = 0; size_t size = sizeof value;
        char c = sysctlbyname(names[i], &value, &size, NULL, 0) == 0 && size == sizeof value ? (value ? '1' : '0') : '?';
        used += (size_t)snprintf(result + used, sizeof result - used, "%s%s=%c", i ? "," : "", keys[i], c);
    }
    setenv("FEX_MADEIRA_HOSTPROBE", result, 1); wb_b_log("[wb-launch] host probe %s", result);
}
static const char *wb_b_target(const char *prefix, const char *exe, int *is32) {
    NSString *bundle = NSBundle.mainBundle.bundlePath;
    NSFileManager *fm = NSFileManager.defaultManager;
    const char *force = getenv("MADEIRA_USE_ARM64EC");
    const char *chosen = (force && !strcmp(force, "1")) || strstr(exe, "x64") || strchr(exe, '\\') ? "arm64ec-windows" : "aarch64-windows";
    int machine = -1;
    *is32 = 0;
    if ([fm fileExistsAtPath:[bundle stringByAppendingPathComponent:@"i386-windows/ntdll.dll"]]) {
        if (wb_b_c_absolute(exe)) {
            char *path = wb_b_unix_c_path(prefix, exe); machine = wb_b_pe_machine(path); free(path);
        } else if (!strchr(exe, '\\') && !strchr(exe, ':') && !strchr(exe, '/')) {
            NSString *name = [NSString stringWithUTF8String:exe];
            BOOL a = [fm fileExistsAtPath:[[bundle stringByAppendingPathComponent:@"aarch64-windows"] stringByAppendingPathComponent:name]];
            BOOL ec = [fm fileExistsAtPath:[[bundle stringByAppendingPathComponent:@"arm64ec-windows"] stringByAppendingPathComponent:name]];
            if (a || ec) machine = 0xaa64;
            else machine = wb_b_pe_machine([[bundle stringByAppendingPathComponent:@"i386-windows"] stringByAppendingPathComponent:name].fileSystemRepresentation);
        }
        if (machine == 0x014c) { *is32 = 1; chosen = "aarch64-windows"; }
    }
    wb_b_log("[wb-launch] PE probe=%d is32=%d DLL set=%s", machine, *is32, chosen);
    return chosen;
}
static void wb_b_apply_workdir(const char *prefix, const char *exe) {
    const char *setting = getenv("MADEIRA_WORKDIR");
    char *override = setting ? strdup(setting) : NULL;
    unsetenv("MADEIRA_WORKDIR");
    char *windows = wb_b_workdir(override, exe); free(override);
    if (!windows) { unsetenv("MADEIRA_INITIAL_CWD"); return; }
    char *unix_path = wb_b_unix_c_path(prefix, windows);
    if (unix_path) {
        errno = 0; int result = chdir(unix_path), saved = errno;
        if (!result) setenv("PWD", unix_path, 1);
        size_t n = strlen(windows);
        char *initial = malloc(n + 2);
        if (initial) {
            memcpy(initial, windows, n);
            if (!n || initial[n - 1] != '\\') initial[n++] = '\\';
            initial[n] = 0; setenv("MADEIRA_INITIAL_CWD", initial, 1); free(initial);
        }
        wb_b_log("[wb-launch] cwd=%s result=%d errno=%d", unix_path, result, saved);
        free(unix_path);
    }
    free(windows);
}
static void wb_b_clear_tsd(void) {
#if defined(__aarch64__) || defined(__arm64__)
    uintptr_t base;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    base &= ~(uintptr_t)7;
    if (base) ((void *volatile *)base)[275] = NULL;
#endif
}
static void wb_b_launch_cleanup(void *opaque) {
    struct wb_b_launch *launch = opaque;
    for (size_t i = 0; i < B_KEY_COUNT; ++i) unsetenv(wb_b_launch_keys[i]);
    // Also clear internal session-derived values so a later launch cannot inherit them.
    unsetenv("MADEIRA_INITIAL_CWD"); unsetenv("MADEIRA_CA_BUNDLE"); unsetenv("MADEIRA_JIT_WRITE_OFFSET");
    unsetenv("WINESERVERSOCKET");
    unsetenv("MADEIRA_WORKDIR");
    wb_server_stop();
    if (launch->client >= 0) close(launch->client);
    free(launch->prefix); free(launch);
    wine_ios_exit_initialized = 0;
    wb_b_clear_tsd();
    atomic_store_explicit(&wb_b_process_running, 0, memory_order_release);
}
static void *wb_b_launch_main(void *opaque) {
    struct wb_b_launch *launch = opaque;
    pthread_cleanup_push(wb_b_launch_cleanup, launch);
    @autoreleasepool {
        const char *prefix = launch->prefix;
        wb_seed_prefix(prefix);
        setenv("WINEPREFIX", prefix, 1); setenv("HOME", prefix, 1); setenv("WINELOADERNOEXEC", "1", 1);
        setenv("WINEDLLPATH", NSBundle.mainBundle.bundlePath.fileSystemRepresentation, 1);
        const char *verbose = getenv("MADEIRA_DEBUG_VERBOSE");
        const char *debug = verbose && *verbose && verbose[0] != '0' ? "err+all,fixme+all,warn+module,trace+loaddll" : "err+all,err-virtual,fixme-d3dcompiler";
        setenv("WINEDEBUG", debug, 1); wb_b_log("[wb-launch] WINEDEBUG=%s", debug);
        setenv("MADEIRA_WIN32U", "1", 1); setenv("MADEIRA_QUIET", "1", 1);
        setenv("FNA3D_FORCE_DRIVER", "D3D11", 0); setenv("MONO_LOG_LEVEL", "warning", 0);
        NSError *error = nil;
        AVAudioSession *audio = AVAudioSession.sharedInstance;
        BOOL category = [audio setCategory:AVAudioSessionCategoryPlayback error:&error];
        if (!category) wb_b_log("[wb-launch] audio category: %s", error.description.UTF8String);
        error = nil;
        BOOL activated = [audio setActive:YES error:&error];
        if (!activated) wb_b_log("[wb-launch] audio activation: %s", error.description.UTF8String);
        if (category && activated) wb_b_log("[wb-launch] audio sampleRate=%.0f latency=%.6f", audio.sampleRate, audio.outputLatency);
        wb_b_steam();
        int64_t offset = fex_get_jit_write_offset();
        if (offset) { char value[32]; snprintf(value, sizeof value, "0x%llx", (unsigned long long)offset); setenv("MADEIRA_JIT_WRITE_OFFSET", value, 1); }
        else { unsetenv("MADEIRA_JIT_WRITE_OFFSET"); const char *nojit = getenv("WINE_IOS_NOJIT"); if (!nojit || strcmp(nojit, "1")) wb_b_log("[wb-launch] warning: JIT write offset is zero"); }
        wine_log_set_file(wb_log_path()); winios_session_reset();
        if (wb_b_documents) setenv("MADEIRA_DOCS_DIR", wb_b_documents, 1);
        wb_b_export_config(launch);
        NSString *ca = [NSBundle.mainBundle pathForResource:@"cacert" ofType:@"pem"];
        if (ca) setenv("MADEIRA_CA_BUNDLE", ca.fileSystemRepresentation, 1);
        else { unsetenv("MADEIRA_CA_BUNDLE"); wb_b_log("[wb-launch] cacert.pem missing: Windows HTTPS certificate checks will fail"); }
        wb_b_redirect_log(1);
        const char *exe_value = getenv("MADEIRA_EXE");
        char *exe = exe_value ? strdup(exe_value) : NULL;
        if (!exe || !*exe) {
            wb_b_log("[wb-launch] MADEIRA_EXE missing"); free(exe);
        } else {
            int is32;
            const char *set = wb_b_target(prefix, exe, &is32);
            wb_b_link_farms(prefix, set);
            char *path = wb_b_windows_path(exe, is32);
            char *argv[67] = {"wine", path, NULL};
            int extra = wb_b_split_args(getenv("MADEIRA_ARGS"), argv + 2, 64);
            if (path && extra >= 0) {
                int argc = extra + 2; argv[argc] = NULL;
                for (int i = 0; i < argc; ++i) wb_b_log("[wb-launch] argv[%d]=%s", i, argv[i]);
                wb_b_apply_workdir(prefix, exe);
                wine_ios_main_thread = pthread_self(); wine_ios_exit_initialized = 1;
                ios_main_image_i386 = is32; wb_b_cpu_probe();
                launch->client = -1; // Wine now owns its socket descriptor.
                if (setjmp(wine_ios_exit_jmpbuf) == 0) { __wine_main(argc, argv); wb_b_log("[wb-launch] __wine_main returned"); }
                else wb_b_log("[wb-launch] process exited status=0x%x", (unsigned int)wine_ios_exit_code);
                char children[256] = {0};
                int n = madeira_live_game_children(children, sizeof children, 60.0);
                const char *wait = getenv("MADEIRA_WAIT_CHILDREN");
                if (n > 0 && wait && !strcmp(wait, "1")) {
                    double start = wb_b_monotonic(), last = start;
                    wb_b_log("[wb-launch] waiting for %d children: %s", n, children);
                    do {
                        struct timespec delay = {0, 200000000}; nanosleep(&delay, NULL);
                        n = madeira_live_game_children(children, sizeof children, -1);
                        double now = wb_b_monotonic();
                        if (now - last >= 60) { wb_b_log("[wb-launch] still waiting children=%d %s", n, children); last = now; }
                    } while (n > 0);
                    wb_b_log("[wb-launch] child wait total=%.3fs", wb_b_monotonic() - start);
                } else if (n > 0) wb_b_log("[wb-launch] session ends with %d children: %s", n, children);
            } else wb_b_log("[wb-launch] argument allocation failed");
            if (extra >= 0) wb_b_free_args(argv + 2, extra);
            free(path); free(exe);
        }
    }
    pthread_cleanup_pop(1);
    return NULL;
}
int wb_process_start(const char *prefix) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&wb_b_process_running, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        wb_b_log("[wb-launch] program already running"); return 0;
    }
    if (!prefix || !*prefix || !wb_server_is_running() || !wb_server_is_ready()) { atomic_store(&wb_b_process_running, 0); return -1; }
    struct wb_b_launch *launch = calloc(1, sizeof *launch);
    if (!launch) { atomic_store(&wb_b_process_running, 0); return -1; }
    launch->prefix = strdup(prefix); launch->client = -1;
    if (!launch->prefix) { free(launch); atomic_store(&wb_b_process_running, 0); return -1; }
    for (size_t i = 0; i < B_KEY_COUNT; ++i) launch->app_set[i] = getenv(wb_b_launch_keys[i]) != NULL;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { free(launch->prefix); free(launch); atomic_store(&wb_b_process_running, 0); return -1; }
    launch->client = pair[1];
    wineserver_inject_client_fd(pair[0]);
    char fd_text[32]; snprintf(fd_text, sizeof fd_text, "%d", pair[1]); setenv("WINESERVERSOCKET", fd_text, 1);
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr), initialized = error == 0;
    if (!error) error = pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    if (!error) error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (!error) error = pthread_create(&thread, &attr, wb_b_launch_main, launch);
    if (initialized) pthread_attr_destroy(&attr);
    if (error) {
        close(pair[0]); close(pair[1]); unsetenv("WINESERVERSOCKET");
        free(launch->prefix); free(launch); atomic_store_explicit(&wb_b_process_running, 0, memory_order_release);
        wb_b_log("[wb-launch] thread creation failed %d", error); return -1;
    }
    return 0;
}
