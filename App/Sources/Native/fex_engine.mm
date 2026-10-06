// SPDX-License-Identifier: GPL-3.0-or-later
// MYIOSDECK engine bridge: drives FEXCore on iOS.
//
// Built on the approach of Madeira's FEXBridge.mm (github.com/willfaust/Madeira,
// GPL-3.0-or-later): FEXCore's executable allocations are redirected into the
// debugger-prepared JIT pool, the RW alias offset is published through
// FEXCore::DualMap::WriteOffset, and guest exit escapes the JIT with longjmp
// (the fault-page stop does not work while a debugger owns the exception port).
//
// Differences: host features come from the kernel (sysctl) instead of being
// hardcoded, the speed presets are configurable, guests get real argv and a
// clock, their output is captured, and each run gets its own big-stack thread.

#include "fex_engine.h"

#include "jit_core.h"
#include "sysinfo.h"

#include <cstdio>
#include <cstring>
#include <mutex>

// CI writes this when FEXCore is built (engine/fex/build-ios.sh).
#if __has_include("engine_version.h")
#include "engine_version.h"
#endif

#ifndef MYIOSDECK_FEX_SHA
#define MYIOSDECK_FEX_SHA "none"
#endif

extern "C" const char *mid_run_result_output(const mid_run_result *r) { return r->output; }
extern "C" const char *mid_run_result_error(const mid_run_result *r) { return r->error; }

#if MYIOSDECK_WITH_FEX

// Xcode defines DEBUG=1 in Debug builds, which collides with LogMan::DEBUG.
#ifdef DEBUG
#undef DEBUG
#endif

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/DualMap.h>
#include <FEXCore/Utils/LogManager.h>

#include <algorithm>
#include <atomic>
#include <csetjmp>
#include <ctime>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

// compiler-rt's icache flush is not in the iOS runtime; FEX's emitter calls it.
extern "C" void __clear_cache(void *start, void *end) {
    sys_icache_invalidate(start, static_cast<size_t>(static_cast<char *>(end) - static_cast<char *>(start)));
}

namespace {

constexpr size_t kPage = 0x4000;

// ------------------------------------------------------------ allocator hooks --

void *PoolMmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if ((prot & PROT_EXEC) && mid_jit_pool_ready()) {
        void *p = mid_jit_pool_alloc(length);
        return p ? p : MAP_FAILED;
    }
    return ::mmap(addr, length, prot, flags, fd, offset);
}

int PoolMunmap(void *addr, size_t length) {
    auto a = reinterpret_cast<uintptr_t>(addr);
    auto base = reinterpret_cast<uintptr_t>(mid_jit_pool_rx_base());
    if (base && a >= base && a < base + mid_jit_pool_size()) return 0; // bump pool: never freed
    return ::munmap(addr, length);
}

// ------------------------------------------------------------------ guest run --

struct RunState {
    mid_run_result *out = nullptr;
    jmp_buf exit_jmp;
    bool exit_armed = false;
    std::atomic<uint64_t> syscalls {0};
    unsigned unhandled_logged = 0;
};

RunState *g_run = nullptr; // valid only while a guest runs (runs are serialised)

void Capture(const char *buf, size_t len) {
    auto *o = g_run->out;
    size_t room = sizeof(o->output) - 1 - o->output_len;
    if (len > room) len = room;
    memcpy(o->output + o->output_len, buf, len);
    o->output_len += len;
    o->output[o->output_len] = 0;
}

// Linux x86-64 syscall numbers implemented in stage 1.
enum : uint64_t {
    kRead = 0, kWrite = 1, kMmap = 9, kMunmap = 11, kBrk = 12, kGetpid = 39, kExit = 60,
    kUname = 63, kGettimeofday = 96, kArchPrctl = 158, kGettid = 186, kClockGettime = 228, kExitGroup = 231,
};
constexpr uint64_t kENOSYS = static_cast<uint64_t>(-38), kEBADF = static_cast<uint64_t>(-9),
                   kENOMEM = static_cast<uint64_t>(-12), kEINVAL = static_cast<uint64_t>(-22);

class StageOneSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
    StageOneSyscalls() { OSABI = FEXCore::HLE::SyscallOSABI::OS_LINUX64; }

    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame *Frame, FEXCore::HLE::SyscallArguments *Args) override {
        // Argument[0] = RAX (number), [1..6] = RDI, RSI, RDX, R10, R8, R9.
        g_run->syscalls.fetch_add(1, std::memory_order_relaxed);
        const uint64_t n = Args->Argument[0];
        const uint64_t a1 = Args->Argument[1], a2 = Args->Argument[2], a3 = Args->Argument[3];
        switch (n) {
        case kWrite:
            if (a1 == 1 || a1 == 2) {
                Capture(reinterpret_cast<const char *>(a2), a3);
                return a3;
            }
            return kEBADF;
        case kRead:
            return kEBADF;
        case kClockGettime: {
            timespec ts {};
            clock_gettime(a1 == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
            auto *g = reinterpret_cast<int64_t *>(a2);
            g[0] = ts.tv_sec;
            g[1] = ts.tv_nsec;
            return 0;
        }
        case kGettimeofday: {
            timespec ts {};
            clock_gettime(CLOCK_REALTIME, &ts);
            if (a1) {
                auto *g = reinterpret_cast<int64_t *>(a1);
                g[0] = ts.tv_sec;
                g[1] = ts.tv_nsec / 1000;
            }
            return 0;
        }
        case kGetpid:
        case kGettid:
            return static_cast<uint64_t>(getpid());
        case kMmap: {
            // Anonymous, non-executable only (guest code would need the loader path).
            const int prot = static_cast<int>(a3) & (PROT_READ | PROT_WRITE);
            const uint64_t flags = Args->Argument[4];
            if (!(flags & 0x20 /* MAP_ANONYMOUS */)) return kENOSYS;
            void *p = ::mmap(nullptr, a2, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
            return p == MAP_FAILED ? kENOMEM : reinterpret_cast<uint64_t>(p);
        }
        case kMunmap:
            return ::munmap(reinterpret_cast<void *>(a1), a2) == 0 ? 0 : kEINVAL;
        case kBrk:
            return 0; // no heap: callers fall back to mmap
        case kArchPrctl:
            if (a1 == 0x1002 /* ARCH_SET_FS */) {
                Frame->State.fs_cached = a2;
                return 0;
            }
            return kEINVAL;
        case kExit:
        case kExitGroup:
            g_run->out->exit_code = static_cast<int64_t>(static_cast<int32_t>(a1));
            if (g_run->exit_armed) longjmp(g_run->exit_jmp, 1);
            return 0;
        default:
            if (g_run->unhandled_logged++ < 16) mid_log("[fex] guest syscall %llu not implemented", (unsigned long long)n);
            return kENOSYS;
        }
    }

    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState *, uint64_t) override {
        return {.Base = 0, .Size = ~0ULL, .Writable = true};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState *,
                                                                                uint64_t) override {
        return std::nullopt;
    }
};

class NoSignals final : public FEXCore::SignalDelegator {};

// --------------------------------------------------------------------- context --

fextl::unique_ptr<FEXCore::Context::Context> g_ctx;
StageOneSyscalls g_syscalls;
NoSignals g_signals;
std::mutex g_lock; // init/shutdown/run are mutually exclusive
bool g_config_live = false;
FEXCore::Core::CPUState::gdt_segment g_gdt[1] {};

void FexLog(LogMan::DebugLevels level, const char *msg) {
    if (level != LogMan::DEBUG) mid_log("[FEXCore:%s] %s", LogMan::DebugLevelStr(level), msg);
}
void FexThrow(const char *msg) { mid_log("[FEXCore:THROW] %s", msg); }

void ApplyConfig(const mid_engine_config &cfg) {
    using namespace FEXCore::Config;
    const bool tso = cfg.preset != MID_PRESET_FASTEST;
    const bool strict = cfg.preset == MID_PRESET_COMPAT;
    Set(CONFIG_IS64BIT_MODE, "1");
    Set(CONFIG_TSOENABLED, tso ? "1" : "0");
    Set(CONFIG_VECTORTSOENABLED, strict ? "1" : "0");
    Set(CONFIG_MEMCPYSETTSOENABLED, strict ? "1" : "0");
    Set(CONFIG_HALFBARRIERTSOENABLED, "1");
    Set(CONFIG_STRICTINPROCESSSPLITLOCKS, strict ? "1" : "0");
    Set(CONFIG_X87REDUCEDPRECISION, strict ? "0" : "1");
    Set(CONFIG_MULTIBLOCK, cfg.multiblock ? "1" : "0");
    char maxinst[16];
    snprintf(maxinst, sizeof maxinst, "%d", cfg.max_inst > 0 ? cfg.max_inst : 5000);
    Set(CONFIG_MAXINST, maxinst);
}

FEXCore::HostFeatures DetectHostFeatures() {
    mid_sysinfo s;
    mid_sysinfo_read(&s);
    FEXCore::HostFeatures f {};
    // 64 is a safe stride for cache maintenance even though Apple's lines are 128.
    f.DCacheLineSize = 64;
    f.ICacheLineSize = 64;
    f.SupportsCacheMaintenanceOps = true;
    f.SupportsAES = s.aes;
    f.SupportsCRC = s.crc32;
    f.SupportsAtomics = s.lse;
    f.SupportsRCPC = s.rcpc;
    f.SupportsTSOImm9 = s.rcpc2;
    f.SupportsRAND = s.rng;
    f.SupportsSHA = s.sha1 && s.sha256;
    f.SupportsPMULL_128Bit = s.pmull;
    f.SupportsCSSC = s.cssc;
    f.SupportsFCMA = s.fcma;
    f.SupportsFlagM = s.flagm;
    f.SupportsFlagM2 = s.flagm2;
    f.SupportsRPRES = s.rpres;
    f.SupportsFRINTTS = s.frintts;
    f.SupportsECV = s.ecv;
    f.SupportsWFXT = s.wfxt;
    f.SupportsMOPS = s.mops;
    f.SupportsAFP = s.afp;
    // Apple cores have no SVE; streaming SVE (SME) is not usable for this.
    f.SupportsSVE128 = false;
    f.SupportsSVE256 = false;
    f.SupportsAVX = false;
    f.CPUMIDRs.resize(s.ncpu > 0 ? s.ncpu : 6, 0x611F0250);
    mid_log("[fex] host %s (%s): %d cores (%dP+%dE) LSE=%d RCPC=%d RCPC2=%d FlagM2=%d FRINTTS=%d AFP=%d CSSC=%d",
            s.machine, s.cpu_brand, s.ncpu, s.perf_cores, s.eff_cores, s.lse, s.rcpc, s.rcpc2, s.flagm2, s.frintts,
            s.afp, s.cssc);
    return f;
}

struct ElfImage {
    void *base = nullptr;
    size_t size = 0;
    uint64_t entry = 0;
};

bool LoadElf(const uint8_t *elf, size_t len, ElfImage &img, char *err, size_t errlen) {
    struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                  uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
    struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
    if (len < sizeof(Ehdr)) { snprintf(err, errlen, "ELF too small"); return false; }
    auto *eh = reinterpret_cast<const Ehdr *>(elf);
    if (memcmp(eh->ident, "\x7f" "ELF", 4) != 0 || eh->ident[4] != 2 || eh->machine != 0x3E) {
        snprintf(err, errlen, "not an x86-64 ELF"); return false;
    }
    if (eh->phoff + (uint64_t)eh->phnum * eh->phentsize > len) { snprintf(err, errlen, "bad program headers"); return false; }
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        auto *ph = reinterpret_cast<const Phdr *>(elf + eh->phoff + (size_t)i * eh->phentsize);
        if (ph->type != 1 /* PT_LOAD */ || !ph->memsz) continue;
        lo = std::min<uint64_t>(lo, ph->vaddr & ~(kPage - 1));
        hi = std::max<uint64_t>(hi, (ph->vaddr + ph->memsz + kPage - 1) & ~(kPage - 1));
    }
    if (lo >= hi) { snprintf(err, errlen, "no loadable segments"); return false; }
    img.size = hi - lo;
    // Guest pages are data to the host: FEX reads them and emits ARM64 into the pool.
    img.base = ::mmap(nullptr, img.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (img.base == MAP_FAILED) { img.base = nullptr; snprintf(err, errlen, "mmap %zu bytes failed", img.size); return false; }
    const int64_t bias = reinterpret_cast<int64_t>(img.base) - static_cast<int64_t>(lo);
    for (int i = 0; i < eh->phnum; i++) {
        auto *ph = reinterpret_cast<const Phdr *>(elf + eh->phoff + (size_t)i * eh->phentsize);
        if (ph->type != 1 || !ph->filesz) continue;
        if (ph->offset + ph->filesz > len) { snprintf(err, errlen, "segment past end of file"); return false; }
        memcpy(reinterpret_cast<void *>(ph->vaddr + bias), elf + ph->offset, ph->filesz);
    }
    img.entry = eh->entry + bias;
    return true;
}

struct RunArgs {
    const uint8_t *elf; size_t len; int argc; const char *const *argv; mid_run_result *out;
};

void *RunThread(void *p) {
    auto &a = *static_cast<RunArgs *>(p);
    auto *out = a.out;
    ElfImage img;
    if (!LoadElf(a.elf, a.len, img, out->error, sizeof out->error)) return nullptr;

    // Guest stack: 8 MB, Linux initial layout (argc, argv[], NULL, envp NULL, auxv AT_NULL).
    const size_t kStack = 8u << 20;
    auto *stack = static_cast<uint8_t *>(::mmap(nullptr, kStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (stack == MAP_FAILED) { snprintf(out->error, sizeof out->error, "guest stack alloc failed"); ::munmap(img.base, img.size); return nullptr; }
    uint8_t *top = stack + kStack;
    uint64_t argp[32];
    const int argc = std::min(a.argc, 31);
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(a.argv[i]) + 1;
        top -= l;
        memcpy(top, a.argv[i], l);
        argp[i] = reinterpret_cast<uint64_t>(top);
    }
    auto *sp = reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(top) & ~uintptr_t(15));
    const int words = 1 + argc + 1 + 1 + 2;
    if (words & 1) *--sp = 0; // keep rsp 16-byte aligned at entry
    *--sp = 0; *--sp = 0;     // auxv AT_NULL
    *--sp = 0;                // envp NULL
    *--sp = 0;                // argv NULL
    for (int i = argc - 1; i >= 0; i--) *--sp = argp[i];
    *--sp = static_cast<uint64_t>(argc);

    auto *Thread = g_ctx->CreateThread(img.entry, reinterpret_cast<uint64_t>(sp));
    if (!Thread) { snprintf(out->error, sizeof out->error, "CreateThread failed"); ::munmap(stack, kStack); ::munmap(img.base, img.size); return nullptr; }

    // Call-return prediction stack with a guard page either side, as the Linux frontend does.
    using TS = FEXCore::Core::InternalThreadState;
    const size_t kCR = TS::CALLRET_STACK_SIZE + 2 * kPage;
    auto *cr = static_cast<uint8_t *>(::mmap(nullptr, kCR, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (cr == MAP_FAILED) { snprintf(out->error, sizeof out->error, "call-ret stack alloc failed"); g_ctx->DestroyThread(Thread); ::munmap(stack, kStack); ::munmap(img.base, img.size); return nullptr; }
    Thread->CallRetStackBase = cr + kPage;
    ::mprotect(Thread->CallRetStackBase, TS::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE);
    auto &st = Thread->CurrentFrame->State;
    st.callret_sp_base = reinterpret_cast<uint64_t>(Thread->CallRetStackBase);
    st.callret_sp = st.callret_sp_base + TS::CALLRET_DEFAULT_OFFSET;

    // Flat 64-bit code segment: the decoder reads CS.L to pick long mode.
    g_gdt[0] = {};
    g_gdt[0].L = 1; g_gdt[0].D = 0; g_gdt[0].P = 1; g_gdt[0].S = 1; g_gdt[0].Type = 0b1011;
    st.segment_arrays[0] = g_gdt;
    st.segment_arrays[1] = g_gdt;
    st.cs_idx = 0;

    g_run->exit_armed = true;
    const uint64_t t0 = mach_absolute_time();
    if (setjmp(g_run->exit_jmp) == 0) {
        g_ctx->ExecuteThread(Thread);
        mid_log("[fex] ExecuteThread returned without exit()");
    }
    const uint64_t t1 = mach_absolute_time();
    g_run->exit_armed = false;

    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    out->seconds = (double)(t1 - t0) * tb.numer / tb.denom / 1e9;
    out->ok = true;

    g_ctx->DestroyThread(Thread);
    ::munmap(cr, kCR);
    ::munmap(stack, kStack);
    ::munmap(img.base, img.size);
    return nullptr;
}

} // namespace

extern "C" {

bool mid_engine_linked(void) { return true; }
const char *mid_engine_version(void) { return MYIOSDECK_FEX_SHA; }
bool mid_engine_ready(void) { return g_ctx != nullptr; }

bool mid_engine_init(const mid_engine_config *cfg, char *err, size_t errlen) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!mid_jit_pool_ready()) { snprintf(err, errlen, "JIT pool not ready: enable JIT first"); return false; }
    g_ctx.reset();

    FEXCore::Allocator::mmap = PoolMmap;
    FEXCore::Allocator::munmap = PoolMunmap;
    FEXCore::DualMap::WriteOffset = mid_jit_pool_write_offset();
    LogMan::Msg::InstallHandler(FexLog);
    LogMan::Throw::InstallHandler(FexThrow);

    const size_t before = mid_jit_pool_used();
    try {
        if (g_config_live) FEXCore::Config::Shutdown(); // preset change: start from a clean config
        FEXCore::Config::Initialize();
        g_config_live = true;
        ApplyConfig(*cfg);
        g_ctx = FEXCore::Context::Context::CreateNewContext(DetectHostFeatures());
        if (!g_ctx) { snprintf(err, errlen, "CreateNewContext returned null"); return false; }
        g_ctx->SetSignalDelegator(&g_signals);
        g_ctx->SetSyscallHandler(&g_syscalls);
        // iPhone cores run weakly ordered for apps (TSO mode is Rosetta-only on macOS),
        // so FEX must emulate x86 ordering unless the FASTEST preset turns it off.
        g_ctx->SetHardwareTSOSupport(false);
        if (!g_ctx->InitCore()) { g_ctx.reset(); snprintf(err, errlen, "InitCore failed"); return false; }
    } catch (const std::exception &e) {
        g_ctx.reset(); snprintf(err, errlen, "FEXCore threw: %s", e.what()); return false;
    } catch (...) {
        g_ctx.reset(); snprintf(err, errlen, "FEXCore threw an unknown exception"); return false;
    }
    mid_log("[fex] FEXCore ready (preset %d, multiblock %d) using %zu KB of JIT pool for the dispatcher",
            cfg->preset, cfg->multiblock, (mid_jit_pool_used() - before) >> 10);
    return true;
}

void mid_engine_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_ctx.reset();
}

bool mid_engine_run_elf(const uint8_t *elf, size_t len, int argc, const char *const *argv, mid_run_result *out) {
    memset(out, 0, sizeof *out);
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_ctx) { snprintf(out->error, sizeof out->error, "engine not initialised"); return false; }

    RunState rs;
    rs.out = out;
    g_run = &rs;
    out->pool_used_before = mid_jit_pool_used();

    RunArgs args {elf, len, argc, argv, out};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_t th;
    if (pthread_create(&th, &attr, RunThread, &args) != 0) {
        snprintf(out->error, sizeof out->error, "pthread_create failed");
    } else {
        pthread_join(th, nullptr);
    }
    pthread_attr_destroy(&attr);

    out->syscalls = rs.syscalls.load();
    out->pool_used_after = mid_jit_pool_used();
    g_run = nullptr;
    return out->ok;
}

} // extern "C"

#else // !MYIOSDECK_WITH_FEX: UI-only build, so the app still runs without the engine.

extern "C" {
bool mid_engine_linked(void) { return false; }
const char *mid_engine_version(void) { return "none"; }
bool mid_engine_ready(void) { return false; }
bool mid_engine_init(const mid_engine_config *, char *err, size_t errlen) {
    snprintf(err, errlen, "This build was made without FEXCore");
    return false;
}
void mid_engine_shutdown(void) {}
bool mid_engine_run_elf(const uint8_t *, size_t, int, const char *const *, mid_run_result *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->error, sizeof out->error, "This build was made without FEXCore");
    return false;
}
}

#endif
