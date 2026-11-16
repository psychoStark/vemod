#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "cpu.h"
#include "nb.h"

#ifndef SYS_futex
#if defined(__NR_futex)
#define SYS_futex __NR_futex
#elif defined(__NR_futex_time64)
#define SYS_futex __NR_futex_time64
#endif
#endif

#ifndef SYS_getdents64
#if defined(__NR_getdents64)
#define SYS_getdents64 __NR_getdents64
#endif
#endif

#ifndef SYS_getrandom
#if defined(__NR_getrandom)
#define SYS_getrandom __NR_getrandom
#endif
#endif

namespace nb {

// ---------------------------------------------------------------- logging

void logf(int prio, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_write(prio, "ninbridge", buf);
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "VemodBridge", "FATAL: %s", buf);
#endif
    log_write(LOG_ERROR, "ninbridge", buf);
    usleep(200000);
    abort();
}

// ---------------------------------------------------------------- handler registry

namespace {

struct Import {
    std::string name;
    Handler fn;
};

std::mutex g_reg_mu;
// index = svc number - kSvcThunkBase. Fixed-size so CallSVC can read it without locking.
constexpr u32 kMaxStubs = 4096;
Import g_handlers[kMaxStubs];
std::atomic<u32> g_handler_count{0};
std::unordered_map<std::string, Handler> g_by_name;
std::unordered_map<std::string, gaddr> g_data;

std::mutex g_timer_mu;
std::unordered_map<u32, timer_t> g_timer_map;
std::atomic<u32> g_timer_seq{1};

constexpr u32 kArmBxLr = 0xE12FFF1E;
constexpr u32 kArmSvc = 0xEF000000;
constexpr u32 kArmDmbIsh = 0xF57FF05B;

}  // namespace

void reg(const char* name, Handler fn) {
    std::lock_guard lk(g_reg_mu);
    g_by_name[name] = fn;
}

void reg_data(const char* name, gaddr addr) {
    std::lock_guard lk(g_reg_mu);
    g_data[name] = addr;
}

Handler find_handler(const char* name) {
    std::lock_guard lk(g_reg_mu);
    auto it = g_by_name.find(name);
    if (it != g_by_name.end()) return it->second;
    const char* at = strchr(name, '@');
    if (at) {
        std::string unversioned(name, at - name);
        it = g_by_name.find(unversioned);
        if (it != g_by_name.end()) return it->second;
    }
    return nullptr;
}

gaddr find_data(const char* name) {
    std::lock_guard lk(g_reg_mu);
    auto it = g_data.find(name);
    if (it != g_data.end()) return it->second;
    const char* at = strchr(name, '@');
    if (at) {
        std::string unversioned(name, at - name);
        it = g_data.find(unversioned);
        if (it != g_data.end()) return it->second;
    }
    return 0;
}

gaddr make_stub(const char* name, Handler fn) {
    u32 idx;
    {
        std::lock_guard lk(g_reg_mu);
        idx = g_handler_count.load(std::memory_order_relaxed);
        if (idx >= kMaxStubs) fatal("too many stubs");
        g_handlers[idx] = {name, fn};
        g_handler_count.store(idx + 1, std::memory_order_release);
    }
    gaddr a = misc_alloc(8, 8);
    wr<u32>(a, kArmSvc | (kSvcThunkBase + idx));
    wr<u32>(a + 4, kArmBxLr);
    return a;
}

static gaddr g_return_trap = 0;

// ---------------------------------------------------------------- CP15

namespace {

// Comprehensive CP15 implementation: ID registers, feature registers, barriers, cycle counter, TPIDRURO/TPIDRURW.
using Dynarmic::A32::CoprocReg;

class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit Cp15(u32* tls) : tls_(tls) {}

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return Callback{[](void*, u32, u32) -> u64 { return 0; }, std::nullopt};
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == CoprocReg::C7) {
            // Memory barriers (CP15ISB, CP15DSB, CP15DMB) or cache ops
            if ((CRm == CoprocReg::C5 && opc2 == 4) || (CRm == CoprocReg::C10 && (opc2 == 4 || opc2 == 5))) {
                return Callback{[](void*, u32, u32) -> u64 {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    return 0;
                }, std::nullopt};
            }
        }
        if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 2) {
            return &tpidrurw_;
        }
        return Callback{[](void*, u32, u32) -> u64 { return 0; }, std::nullopt};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return Callback{[](void*, u32, u32) -> u64 { return 0; }, std::nullopt};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0) {
            if (CRn == CoprocReg::C0) {
                if (CRm == CoprocReg::C0) {
                    if (opc2 == 0) return &kMIDR;      // Main ID: Cortex-A15 r0p2
                    if (opc2 == 1) return &kCTR;       // Cache type (64B lines)
                    if (opc2 == 5) return &kMPIDR;     // Multiprocessor affinity
                    return &kZero;
                }
                if (CRm == CoprocReg::C1) {
                    // ID_PFR0, ID_PFR1, ID_DFR0, ID_AFR0, ID_MMFR0..3
                    static u32 kPFR0 = 0x00001131;
                    static u32 kPFR1 = 0x00011011;
                    if (opc2 == 0) return &kPFR0;
                    if (opc2 == 1) return &kPFR1;
                    return &kZero;
                }
                if (CRm == CoprocReg::C2) {
                    // ID_ISAR0 .. ID_ISAR5
                    static u32 kISAR0 = 0x02101110;
                    static u32 kISAR1 = 0x13112111;
                    static u32 kISAR2 = 0x21232141;
                    static u32 kISAR3 = 0x01112131;
                    static u32 kISAR4 = 0x00010142;
                    static u32 kISAR5 = 0x00000000;
                    if (opc2 == 0) return &kISAR0;
                    if (opc2 == 1) return &kISAR1;
                    if (opc2 == 2) return &kISAR2;
                    if (opc2 == 3) return &kISAR3;
                    if (opc2 == 4) return &kISAR4;
                    if (opc2 == 5) return &kISAR5;
                    return &kZero;
                }
            }
            if (CRn == CoprocReg::C9 && CRm == CoprocReg::C13 && opc2 == 0) {
                // PMCCNTR: Cycle count
                return Callback{[](void*, u32, u32) -> u64 {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    return u32(ts.tv_sec * 1000000000ull + ts.tv_nsec);
                }, std::nullopt};
            }
            if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0) {
                if (opc2 == 2) return &tpidrurw_; // User Read/Write Thread ID
                if (opc2 == 3) return tls_;       // User Read-Only Thread ID (TPIDRURO)
                return &kZero;
            }
        }
        return &kZero;
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        static u32 kTwoWords[2] = {0, 0};
        return std::array<u32*, 2>{&kTwoWords[0], &kTwoWords[1]};
    }

    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<u8>) override {
        return Callback{[](void*, u32, u32) -> u64 { return 0; }, std::nullopt};
    }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<u8>) override {
        return Callback{[](void*, u32, u32) -> u64 { return 0; }, std::nullopt};
    }

private:
    u32* tls_;
    u32 tpidrurw_ = 0;
    static inline u32 kZero = 0;
    static inline u32 kMIDR = 0x410fc0f2;
    static inline u32 kCTR = 0x8444c004;
    static inline u32 kMPIDR = 0x80000000;
};

// ---------------------------------------------------------------- processor ids for the monitor

constexpr size_t kMaxCpus = 512;
Dynarmic::ExclusiveMonitor* g_monitor;
std::mutex g_pid_mu;
std::vector<size_t> g_free_pids;
size_t g_next_pid = 0;

size_t alloc_pid() {
    std::lock_guard lk(g_pid_mu);
    if (!g_free_pids.empty()) {
        size_t p = g_free_pids.back();
        g_free_pids.pop_back();
        return p;
    }
    if (g_next_pid >= kMaxCpus) fatal("too many concurrent guest CPUs");
    return g_next_pid++;
}

void free_pid(size_t p) {
    std::lock_guard lk(g_pid_mu);
    g_monitor->ClearProcessor(p);
    g_free_pids.push_back(p);
}

}  // namespace

// ---------------------------------------------------------------- Cpu

static std::atomic<Dynarmic::A32::Jit*> g_active_jits[kMaxCpus]{};

Cpu::Cpu(GuestThread* t, int depth) : thread(t), depth(depth), pid(alloc_pid()) {
    tls_word = t->tls;
    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.processor_id = pid;
    cfg.global_monitor = g_monitor;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
    cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(g_base);
    cfg.enable_cycle_counting = false;
    // The guest never modifies its own code, and the range index costs ~100+ MB of host heap.
    cfg.track_code_ranges = false;
    // Every thread has its own JIT (dynarmic is not re-entrant); nested ones run little code.
    cfg.code_cache_size = depth == 0 ? 128u * 1024 * 1024 : 16u * 1024 * 1024;
    cfg.optimizations = Dynarmic::all_safe_optimizations |
                        Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                        Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                        Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    cfg.unsafe_optimizations = true;
    auto cp15 = std::make_shared<Cp15>(&tls_word);
    for (size_t i = 0; i < 16; ++i) {
        cfg.coprocessors[i] = cp15;
    }
    jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
    g_active_jits[pid].store(jit.get(), std::memory_order_release);
}

Cpu::~Cpu() {
    g_active_jits[pid].store(nullptr, std::memory_order_release);
    jit.reset();
    free_pid(pid);
}

bool Cpu::check(gaddr a, u32 n, const char* what) {
    if (!is_mapped(a) || !is_mapped(a + n - 1)) {
        dump("bad memory access");
        fatal("guest %s of %u bytes at unmapped address %#x", what, n, a);
        return false;
    }
    return true;
}

std::optional<u32> Cpu::MemoryReadCode(u32 a) {
    if (!is_mapped(a)) return std::nullopt;
    return rd<u32>(a);
}
u8 Cpu::MemoryRead8(u32 a) { check(a, 1, "read"); return rd<u8>(a); }
u16 Cpu::MemoryRead16(u32 a) { check(a, 2, "read"); return rd<u16>(a); }
u32 Cpu::MemoryRead32(u32 a) { check(a, 4, "read"); return rd<u32>(a); }
u64 Cpu::MemoryRead64(u32 a) { check(a, 8, "read"); return rd<u64>(a); }
void Cpu::MemoryWrite8(u32 a, u8 v) { check(a, 1, "write"); wr(a, v); }
void Cpu::MemoryWrite16(u32 a, u16 v) { check(a, 2, "write"); wr(a, v); }
void Cpu::MemoryWrite32(u32 a, u32 v) { check(a, 4, "write"); wr(a, v); }
void Cpu::MemoryWrite64(u32 a, u64 v) { check(a, 8, "write"); wr(a, v); }

template <class T>
static bool cas(gaddr a, T v, T expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(g_base + a), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
bool Cpu::MemoryWriteExclusive8(u32 a, u8 v, u8 e) { check(a, 1, "strex"); return cas(a, v, e); }
bool Cpu::MemoryWriteExclusive16(u32 a, u16 v, u16 e) { check(a, 2, "strex"); return cas(a, v, e); }
bool Cpu::MemoryWriteExclusive32(u32 a, u32 v, u32 e) { check(a, 4, "strex"); return cas(a, v, e); }
bool Cpu::MemoryWriteExclusive64(u32 a, u64 v, u64 e) { check(a, 8, "strexd"); return cas(a, v, e); }

void Cpu::InterpreterFallback(u32 pc, size_t n) {
    dump("interpreter fallback");
    fatal("dynarmic requested interpreter fallback at %#x (%zu instrs)", pc, n);
}

void Cpu::ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    switch (e) {
    case E::SendEvent:
    case E::SendEventLocal:
    case E::WaitForInterrupt:
    case E::WaitForEvent:
    case E::Yield:
    case E::PreloadData:
    case E::PreloadDataWithIntentToWrite:
    case E::PreloadInstruction:
        return;
    default:
        break;
    }
    static const char* names[] = {"UndefinedInstruction", "UnpredictableInstruction", "DecodeError", "SendEvent",
                                  "SendEventLocal", "WaitForInterrupt", "WaitForEvent", "Yield", "Breakpoint",
                                  "PreloadData", "PreloadDataWithIntentToWrite", "PreloadInstruction", "NoExecuteFault"};
    jit->Regs()[15] = pc;
    dump("cpu exception");
    u32 insn = is_mapped(pc) ? rd<u32>(pc) : 0;
    fatal("guest exception %s at %#x [%s] insn=%08x", names[size_t(e)], pc, symbolize(pc).c_str(), insn);
}

void Cpu::dump(const char* why) {
    auto& r = jit->Regs();
    gaddr sp = r[13];
    int found = 0;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "=== guest state (%s), thread %u depth %d ===", why, thread->tid, depth);
    for (int i = 0; i < 16; i += 4)
        __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x",
                            i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "cpsr %08x", jit->Cpsr());
    __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "pc %s", symbolize(r[15]).c_str());
    __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "lr %s", symbolize(r[14]).c_str());
    __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "raw stack at sp=%#x:", sp);
    for (int offset = 0; offset < 64 && is_mapped(sp + offset); offset += 16) {
        __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "  +%02x: %08x %08x %08x %08x", offset,
                            rd<u32>(sp + offset), rd<u32>(sp + offset + 4),
                            rd<u32>(sp + offset + 8), rd<u32>(sp + offset + 12));
    }
    for (gaddr p = sp; p < sp + 1024 && found < 12 && is_mapped(p); p += 4) {
        u32 v = rd<u32>(p);
        if (v >= layout::kLibBase && v < layout::kLibBase + 0x1b40000 && v != r[14]) {
            __android_log_print(ANDROID_LOG_ERROR, "VemodBridge", "  stack[%#x] %s", p - sp, symbolize(v).c_str());
            ++found;
        }
    }
#endif
    NB_LOGE("=== guest state (%s), thread %u depth %d ===", why, thread->tid, depth);
    for (int i = 0; i < 16; i += 4)
        NB_LOGE("r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3,
                r[i + 3]);
    NB_LOGE("cpsr %08x", jit->Cpsr());
    NB_LOGE("pc %s", symbolize(r[15]).c_str());
    NB_LOGE("lr %s", symbolize(r[14]).c_str());
    // Heuristic backtrace: code addresses found on the stack.
    NB_LOGE("raw stack at sp=%#x:", sp);
    for (int offset = 0; offset < 64 && is_mapped(sp + offset); offset += 16) {
        NB_LOGE("  +%02x: %08x %08x %08x %08x", offset,
                rd<u32>(sp + offset), rd<u32>(sp + offset + 4),
                rd<u32>(sp + offset + 8), rd<u32>(sp + offset + 12));
    }
    found = 0;
    for (gaddr p = sp; p < sp + 1024 && found < 12 && is_mapped(p); p += 4) {
        u32 v = rd<u32>(p);
        if (v >= layout::kLibBase && v < layout::kLibBase + 0x1b40000 && v != r[14]) {
            NB_LOGE("  stack[%#x] %s", p - sp, symbolize(v).c_str());
            ++found;
        }
    }
}

// ---------------------------------------------------------------- threads

namespace {
std::atomic<u32> g_next_tid{1};

// Guest threads (stack, TLS, JITs with their compiled code) outlive the host threads that used
// them: when a host thread exits its GuestThread goes back to a small pool, and the next host thread
// to enter the guest inherits the warmest one instead of compiling everything again.
std::mutex g_pool_mu;
// Deliberately leaked: destroying JITs during static destruction at process exit races dynarmic's
// own statics (and the OS reclaims everything anyway).
auto& g_pool = *new std::vector<std::unique_ptr<GuestThread>>;
constexpr size_t kPoolMax = 8;

size_t warmth(const GuestThread& t) { return t.cpus.empty() ? 0 : t.compiled_hint; }

void release_to_pool(std::unique_ptr<GuestThread> t) {
    if (t->depth != -1 || t->exiting) return;  // unwound abnormally; drop it
    t->specific.clear();
    wr<s32>(t->tls + tlsoff::kErrno, 0);
    std::lock_guard lk(g_pool_mu);
    g_pool.push_back(std::move(t));
    if (g_pool.size() > kPoolMax) {
        auto coldest = std::min_element(g_pool.begin(), g_pool.end(),
                                        [](const auto& a, const auto& b) { return warmth(*a) < warmth(*b); });
        g_pool.erase(coldest);
    }
}

struct ThreadSlot {
    std::unique_ptr<GuestThread> t;
    ~ThreadSlot() {
        if (t) release_to_pool(std::move(t));
        // Never destroy a GuestThread here: on the main thread this runs during process exit.
        (void)t.release();
    }
};
thread_local ThreadSlot t_slot;
}  // namespace

void release_current_guest_thread() {
    if (t_slot.t) release_to_pool(std::move(t_slot.t));
}

GuestThread::GuestThread() {
    tid = g_next_tid++;
    stack_size = 8u << 20;
    stack_base = map_alloc(stack_size + kPageSize, PROT_NONE);
    if (!stack_base) fatal("out of guest memory for a stack");
    // lowest page stays inaccessible as a guard
    mem_protect(stack_base + kPageSize, stack_size, PROT_READ | PROT_WRITE);
    stack_base += kPageSize;
    tls = map_alloc(tlsoff::kSize, PROT_READ | PROT_WRITE);
    memset(h(tls), 0, tlsoff::kSize);

    // Setup Bionic TLS slots
    wr<u32>(tls + tlsoff::kSlotSelf, tls);
    wr<u32>(tls + tlsoff::kSlotThreadId, tls + tlsoff::kPthreadInternal);
    wr<u32>(tls + tlsoff::kSlotErrno, 0);
    wr<u32>(tls + tlsoff::kSlotStackGuard, 0xdeadbeef);

    // Setup pthread_internal_t at offset 0x400:
    // [0]: next, [4]: prev, [8]: tid
    gaddr pth = tls + tlsoff::kPthreadInternal;
    wr<u32>(pth + 0, pth);
    wr<u32>(pth + 4, pth);
    wr<u32>(pth + 8, tid);

    char name[32] = "?";
    prctl(PR_GET_NAME, name);
    NB_LOGI("host thread '%s' entered the guest (guest tid %u)", name, tid);
}

GuestThread::~GuestThread() {
    if (clear_child_tid && is_mapped(clear_child_tid)) {
        wr<u32>(clear_child_tid, 0);
        syscall(SYS_futex, h(clear_child_tid), FUTEX_WAKE, 1, nullptr, nullptr, 0);
        syscall(SYS_futex, h(clear_child_tid), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }
    cpus.clear();
    if (own_stack && stack_base) {
        map_free(stack_base - kPageSize, stack_size + kPageSize);
    }
    if (own_tls && tls) {
        map_free(tls, tlsoff::kSize);
    }
}

GuestThread& cur_thread() {
    if (!t_slot.t) {
        {
            std::lock_guard lk(g_pool_mu);
            if (!g_pool.empty()) {
                auto warmest = std::max_element(g_pool.begin(), g_pool.end(),
                                                [](const auto& a, const auto& b) { return warmth(*a) < warmth(*b); });
                t_slot.t = std::move(*warmest);
                g_pool.erase(warmest);
            }
        }
        if (t_slot.t) {
            char name[32] = "?";
            prctl(PR_GET_NAME, name);
            NB_LOGI("host thread '%s' inherited guest tid %u (warm JIT)", name, t_slot.t->tid);
        } else {
            t_slot.t = std::make_unique<GuestThread>();
        }
    }
    return *t_slot.t;
}

void set_guest_errno(int e) { wr<s32>(cur_thread().tls + tlsoff::kErrno, e); }

// ---------------------------------------------------------------- SVC dispatch

static void do_syscall(Cpu& cpu) {
    auto& r = cpu.jit->Regs();
    u32 nr = r[7];
    if (get_log_level() >= LOG_LEVEL_FULL && nr != 240 && nr != 78 && nr != 174 && nr != 175 && nr != 162 && nr != 0xf0002 && nr != 327) {
        NB_LOGI("raw syscall nr=%u r0=%#x r1=%#x r2=%#x r3=%#x", nr, r[0], r[1], r[2], r[3]);
    }
    switch (nr) {
    // ARM Private
    case 0xf0001:  // __ARM_NR_breakpoint
        cpu.dump("guest breakpoint");
        r[0] = 0;
        return;
    case 0xf0002:  // __ARM_NR_cacheflush
        // Guest JITted code in data memory is already coherent with host fastmem reads.
        // Without track_code_ranges, calling InvalidateCacheRange flushes the entire 128MB JIT cache,
        // causing severe stuttering on every Mono JIT compilation during animations and gameplay turns.
        r[0] = 0;
        return;
    case 0xf0005:  // __ARM_NR_set_tls
        cpu.tls_word = r[0];
        r[0] = 0;
        return;

    // Process & thread lifecycle
    case 1:  // exit
        cpu.thread->exiting = true;
        cpu.jit->HaltExecution();
        return;
    case 248:  // exit_group
        exit(int(r[0]));
        return;
    case 120: {  // clone
        u32 flags = r[0];
        gaddr child_sp = r[1];
        gaddr parent_tidptr = r[2];
        gaddr new_tls = r[3];
        gaddr child_tidptr = r[4];

        auto child_t = std::make_unique<GuestThread>();
        u32 child_tid = child_t->tid;
        if (flags & 0x00080000) {  // CLONE_SETTLS
            child_t->tls = new_tls;
            child_t->own_tls = false;
        }
        if (child_sp) {
            child_t->own_stack = false;
        }
        if (flags & 0x00200000) {  // CLONE_CHILD_CLEARTID
            child_t->clear_child_tid = child_tidptr;
        }
        if (flags & 0x01000000) {  // CLONE_CHILD_SETTID
            if (is_mapped(child_tidptr)) wr<u32>(child_tidptr, child_tid);
        }
        if (flags & 0x00100000) {  // CLONE_PARENT_SETTID
            if (is_mapped(parent_tidptr)) wr<u32>(parent_tidptr, child_tid);
        }

        r[0] = child_tid;

        auto parent_regs = r;
        u32 parent_cpsr = cpu.jit->Cpsr();
        u32 parent_fpscr = cpu.jit->Fpscr();

        NB_LOGI("SYS_clone: guest parent_pc=%#x sp=%#x flags=%#x, spawning child_tid=%u",
                parent_regs[15], child_sp, flags, child_tid);

        std::thread([ct = std::move(child_t), child_sp, parent_regs, parent_cpsr, parent_fpscr, flags, new_tls]() mutable {
            t_slot.t = std::move(ct);
            GuestThread& t = *t_slot.t;
            t.depth = 0;
            if (t.cpus.empty()) t.cpus.push_back(std::make_unique<Cpu>(&t, 0));
            Cpu& c = *t.cpus[0];
            if (flags & 0x00080000) c.tls_word = new_tls;
            auto& cr = c.jit->Regs();
            cr = parent_regs;
            cr[0] = 0;  // child returns 0
            cr[13] = child_sp;
            c.jit->SetCpsr(parent_cpsr);
            c.jit->SetFpscr(parent_fpscr);
            c.jit->ClearExclusiveState();
            c.returned = false;
            char name[32] = "?";
            prctl(PR_GET_NAME, name);
            NB_LOGI("clone child started: guest tid=%u host name='%s' pc=%#x", t.tid, name, cr[15]);
            while (!c.returned && !t.exiting) {
                auto hr = c.jit->Run();
                if (bool(hr & Dynarmic::HaltReason::UserDefined1)) {
                    c.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
                }
            }
            NB_LOGI("clone child exited: guest tid=%u", t.tid);
        }).detach();
        return;
    }
    case 114: {  // wait4(pid, wstatus, options, rusage)
        int status = 0;
        int res = wait4(r[0], &status, r[2], nullptr);
        if (r[1] && is_mapped(r[1])) wr<s32>(r[1], status);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 256:  // set_tid_address
        cpu.thread->clear_child_tid = r[0];
        r[0] = cpu.thread->tid;
        return;

    // Futex & synchronization
    case 240:
    case 422: {  // futex / futex_time64
        if (!r[0] || !is_mapped(r[0])) {
            r[0] = u32(-EFAULT);
            return;
        }
        int cmd = r[1] & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
        bool takes_timespec = (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET ||
                               cmd == FUTEX_LOCK_PI || cmd == FUTEX_WAIT_REQUEUE_PI);
        bool takes_val2 = (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE || cmd == FUTEX_WAKE_OP);
        bool takes_uaddr2 = (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE ||
                             cmd == FUTEX_WAKE_OP || cmd == FUTEX_WAIT_REQUEUE_PI ||
                             cmd == FUTEX_CMP_REQUEUE_PI);

        void* arg4 = nullptr;
        struct timespec host_ts;
        if (takes_timespec) {
            if (r[3] && is_mapped(r[3])) {
                if (nr == 422) {
                    host_ts.tv_sec = rd<s64>(r[3] + 0);
                    host_ts.tv_nsec = rd<s64>(r[3] + 8);
                } else {
                    struct guest_ts { s32 sec; s32 nsec; };
                    guest_ts* gts = reinterpret_cast<guest_ts*>(h(r[3]));
                    host_ts.tv_sec = gts->sec;
                    host_ts.tv_nsec = gts->nsec;
                }
                arg4 = &host_ts;
            }
        } else if (takes_val2) {
            arg4 = reinterpret_cast<void*>(static_cast<uintptr_t>(r[3]));
        }

        void* uaddr2 = (takes_uaddr2 && r[4] && is_mapped(r[4])) ? h(r[4]) : nullptr;
        u32 val3 = r[5];
        if ((cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_WAKE_BITSET) && val3 == 0) {
            val3 = 0xffffffff;  // FUTEX_BITSET_MATCH_ANY
        }

        int res = syscall(SYS_futex, h(r[0]), r[1], r[2], arg4, uaddr2, val3);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 154: {  // sched_setparam
        struct sched_param p{};
        if (r[1] && is_mapped(r[1])) p.sched_priority = rd<s32>(r[1]);
        int res = sched_setparam(r[0], &p);
        if (res < 0 && (errno == EPERM || errno == EACCES)) {
            r[0] = 0;
        } else {
            r[0] = res < 0 ? -errno : res;
        }
        return;
    }
    case 155: {  // sched_getparam
        struct sched_param p{};
        int res = sched_getparam(r[0], &p);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[1] && is_mapped(r[1])) wr<s32>(r[1], p.sched_priority);
            r[0] = 0;
        }
        return;
    }
    case 156: {  // sched_setscheduler
        struct sched_param p{};
        if (r[2] && is_mapped(r[2])) p.sched_priority = rd<s32>(r[2]);
        int res = sched_setscheduler(r[0], r[1], &p);
        if (res < 0 && (errno == EPERM || errno == EACCES)) {
            r[0] = 0;
        } else {
            r[0] = res < 0 ? -errno : res;
        }
        return;
    }
    case 157: {  // sched_getscheduler
        int res = sched_getscheduler(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 158:  // sched_yield
        r[0] = sched_yield();
        return;
    case 159: {  // sched_get_priority_max
        int res = sched_get_priority_max(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 160: {  // sched_get_priority_min
        int res = sched_get_priority_min(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 241: {  // sched_setaffinity
        int res = sched_setaffinity(r[0], r[1], reinterpret_cast<const cpu_set_t*>(h(r[2])));
        if (res < 0 && (errno == EPERM || errno == EACCES || errno == EINVAL)) {
            r[0] = 0;
        } else {
            r[0] = res < 0 ? -errno : res;
        }
        return;
    }
    case 242: {  // sched_getaffinity
        gaddr mask = r[2];
        u32 len = r[1];
        if (!mask || !is_mapped(mask) || len == 0) {
            r[0] = u32(-EINVAL);
            return;
        }
        int res = sched_getaffinity(r[0], len, reinterpret_cast<cpu_set_t*>(h(mask)));
        if (res < 0) {
            // Fallback for restricted sandboxes / OEM permission errors: populate with online cores
            memset(h(mask), 0, len);
            int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
            if (ncpus <= 0) ncpus = 4;
            for (int i = 0; i < std::min<int>(ncpus, (int)len * 8); ++i) {
                reinterpret_cast<u8*>(h(mask))[i / 8] |= (1u << (i % 8));
            }
            r[0] = std::min<u32>(len, 4);
        } else {
            r[0] = res;
        }
        return;
    }
    case 238: {  // tkill(tid, sig)
        int tid = (int)r[0];
        int sig = (int)r[1];
        if (get_log_level() >= LOG_LEVEL_ADVANCED) {
            NB_LOGI("raw syscall tkill: tid=%d sig=%d", tid, sig);
        }
        if (sig == 0) {
            r[0] = 0; // thread alive check
        } else if (sig == 30 || sig == 24 || sig >= 32) {
            // Signal-based GC thread suspension (e.g. SIGPWR/SIGXCPU in Boehm GC / Mono).
            // Under JIT, guest threads cannot be asynchronously interrupted via signals.
            // Returning -ESRCH signals to Boehm GC that this thread is not suspendable via signals,
            // causing it to decrement its expected ack counter and proceed without deadlocking on sem_wait.
            r[0] = -ESRCH;
        } else {
            r[0] = 0;
        }
        return;
    }
    case 268: {  // tgkill(tgid, tid, sig)
        int tgid = (int)r[0];
        int tid = (int)r[1];
        int sig = (int)r[2];
        if (get_log_level() >= LOG_LEVEL_ADVANCED) {
            NB_LOGI("raw syscall tgkill: tgid=%d tid=%d sig=%d", tgid, tid, sig);
        }
        if (sig == 0) {
            r[0] = 0;
        } else if (sig == 30 || sig == 24 || sig >= 32) {
            r[0] = -ESRCH;
        } else {
            r[0] = 0;
        }
        return;
    }

    // Memory
    case 45:  // brk
        r[0] = mem_brk(r[0]);
        return;
    case 91:  // munmap
        r[0] = x_munmap(r[0], r[1]) < 0 ? -errno : 0;
        return;
    case 125:  // mprotect
        mem_protect(r[0], host_page_round_up(r[1]), r[2]);
        r[0] = 0;
        return;
    case 192: {  // mmap2
        gaddr addr = r[0];
        u32 len = r[1];
        int prot = int(r[2]);
        int flags = int(r[3]);
        int fd = int(r[4]);
        u32 pgoff = r[5];
        u32 off = pgoff << 12;
        if (!len) { r[0] = u32(-EINVAL); return; }
        u32 size = host_page_round_up(len);
        gaddr a = 0;
        if (flags & MAP_FIXED) {
            if (addr < layout::kMapBase || u64(addr) + size > layout::kMapEnd) {
                r[0] = u32(-EINVAL);
                return;
            }
            map_claim(addr, size);
            a = addr;
        } else {
            if (addr && (addr & (kPageSize - 1)) == 0 && addr >= layout::kMapBase && u64(addr) + size <= layout::kMapEnd && map_claim(addr, size)) {
                a = addr;
            } else {
                a = map_alloc(size, PROT_NONE);
                if (!a) { r[0] = u32(-ENOMEM); return; }
            }
        }
        int hflags = (flags & (MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS)) | MAP_FIXED;
        void* p = mmap(g_base + a, size, prot, hflags, (flags & MAP_ANONYMOUS) ? -1 : fd, off);
        if (p == MAP_FAILED) {
            map_free(a, size);
            r[0] = u32(-errno);
            return;
        }
        mem_protect(a, size, prot == PROT_NONE ? PROT_NONE : prot | PROT_READ);
        r[0] = a;
        return;
    }
    case 220:  // madvise
        madvise(g_base + r[0], host_page_round_up(r[1]), r[2]);
        r[0] = 0;
        return;

    // File I/O
    case 3: {  // read
        s32 n = x_read(r[0], h(r[1]), r[2]);
        r[0] = n < 0 ? -errno : n;
        return;
    }
    case 4: {  // write
        s32 n = x_write(r[0], h(r[1]), r[2]);
        r[0] = n < 0 ? -errno : n;
        return;
    }
    case 5: {  // open
        const char* p = hs(r[0]);
        int fd = open(headless_path(p), oflags_from_guest(r[1]) | O_CLOEXEC, r[2]);
        r[0] = fd < 0 ? -errno : fd;
        return;
    }
    case 6:  // close
        r[0] = x_close(r[0]) < 0 ? -errno : 0;
        return;
    case 10: {  // unlink
        int res = unlink(hs(r[0]));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 12: {  // chdir
        int res = chdir(hs(r[0]));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 19: {  // lseek
        s32 res = x_lseek(r[0], s32(r[1]), r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 33: {  // access
        int res = access(headless_path(hs(r[0])), r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 38: {  // rename
        int res = rename(hs(r[0]), hs(r[1]));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 39: {  // mkdir
        int res = mkdir(hs(r[0]), r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 40: {  // rmdir
        int res = rmdir(hs(r[0]));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 41: {  // dup
        int res = dup(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 42: {  // pipe
        int fds[2];
        int res = pipe(fds);
        if (res < 0) {
            r[0] = -errno;
        } else {
            wr<s32>(r[0], fds[0]);
            wr<s32>(r[0] + 4, fds[1]);
            r[0] = 0;
        }
        return;
    }
    case 54: {  // ioctl
        int fd = r[0];
        u32 cmd = r[1];
        gaddr arg = r[2];
        if (fd == kAlarmFd) { r[0] = 0; return; }
        int res = ioctl(fd, cmd, arg ? h(arg) : nullptr);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 63: {  // dup2
        int res = dup2(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 85: {  // readlink
        const char* p = hs(r[0]);
        if (is_proc_self_exe(p)) {
            const std::string& apk = get_guest_apk_path();
            if (!apk.empty()) {
                size_t len = std::min((size_t)r[2], apk.length());
                memcpy(h(r[1]), apk.data(), len);
                r[0] = len;
                return;
            }
        }
        ssize_t res = readlink(p, reinterpret_cast<char*>(h(r[1])), r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 93: {  // ftruncate
        int res = ftruncate(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 94: {  // fchmod
        int res = fchmod(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 99: {  // statfs(path, buf)
        struct statfs st;
        int res = statfs(hs(r[0]), &st);
        if (res < 0) {
            r[0] = -errno;
        } else {
            gaddr buf = r[1];
            if (buf && is_mapped(buf)) {
                memset(h(buf), 0, 64);
                wr<u32>(buf + 0, (u32)st.f_type);
                wr<u32>(buf + 4, (u32)st.f_bsize);
                wr<u32>(buf + 8, (u32)std::min<u64>(st.f_blocks, 0xffffffffULL));
                wr<u32>(buf + 12, (u32)std::min<u64>(st.f_bfree, 0xffffffffULL));
                wr<u32>(buf + 16, (u32)std::min<u64>(st.f_bavail, 0xffffffffULL));
                wr<u32>(buf + 20, (u32)std::min<u64>(st.f_files, 0xffffffffULL));
                wr<u32>(buf + 24, (u32)std::min<u64>(st.f_ffree, 0xffffffffULL));
                memcpy(h(buf + 28), &st.f_fsid, 8);
                wr<u32>(buf + 36, (u32)st.f_namelen);
                wr<u32>(buf + 40, (u32)st.f_frsize);
                wr<u32>(buf + 44, (u32)st.f_flags);
            }
            r[0] = 0;
        }
        return;
    }
    case 100: {  // fstatfs(fd, buf)
        struct statfs st;
        int res = fstatfs(r[0], &st);
        if (res < 0) {
            r[0] = -errno;
        } else {
            gaddr buf = r[1];
            if (buf && is_mapped(buf)) {
                memset(h(buf), 0, 64);
                wr<u32>(buf + 0, (u32)st.f_type);
                wr<u32>(buf + 4, (u32)st.f_bsize);
                wr<u32>(buf + 8, (u32)std::min<u64>(st.f_blocks, 0xffffffffULL));
                wr<u32>(buf + 12, (u32)std::min<u64>(st.f_bfree, 0xffffffffULL));
                wr<u32>(buf + 16, (u32)std::min<u64>(st.f_bavail, 0xffffffffULL));
                wr<u32>(buf + 20, (u32)std::min<u64>(st.f_files, 0xffffffffULL));
                wr<u32>(buf + 24, (u32)std::min<u64>(st.f_ffree, 0xffffffffULL));
                memcpy(h(buf + 28), &st.f_fsid, 8);
                wr<u32>(buf + 36, (u32)st.f_namelen);
                wr<u32>(buf + 40, (u32)st.f_frsize);
                wr<u32>(buf + 44, (u32)st.f_flags);
            }
            r[0] = 0;
        }
        return;
    }
    case 118: {  // fsync
        int res = fsync(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 133: {  // fchdir
        int res = fchdir(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 140: {  // _llseek
        int fd = r[0];
        u64 off = (u64(r[1]) << 32) | r[2];
        gaddr res_ptr = r[3];
        int whence = r[4];
        off_t res = vfd_is(fd) ? vfd_lseek(fd, off, whence) : lseek(fd, off, whence);
        if (res == (off_t)-1) {
            r[0] = -errno;
        } else {
            wr<s64>(res_ptr, res);
            r[0] = 0;
        }
        return;
    }
    case 142: {  // _newselect
        int nfds = r[0];
        gaddr r_fds = r[1];
        gaddr w_fds = r[2];
        gaddr e_fds = r[3];
        gaddr to_addr = r[4];

        fd_set read_set, write_set, except_set;
        fd_set* p_r = nullptr;
        fd_set* p_w = nullptr;
        fd_set* p_e = nullptr;

        size_t fds_bytes = (nfds > 0 && nfds <= 1024) ? ((nfds + 7) / 8) : 128;
        if (r_fds && is_mapped(r_fds)) {
            FD_ZERO(&read_set);
            memcpy(&read_set, h(r_fds), fds_bytes);
            p_r = &read_set;
        }
        if (w_fds && is_mapped(w_fds)) {
            FD_ZERO(&write_set);
            memcpy(&write_set, h(w_fds), fds_bytes);
            p_w = &write_set;
        }
        if (e_fds && is_mapped(e_fds)) {
            FD_ZERO(&except_set);
            memcpy(&except_set, h(e_fds), fds_bytes);
            p_e = &except_set;
        }

        struct timeval host_tv{};
        struct timeval* p_to = nullptr;
        if (to_addr && is_mapped(to_addr)) {
            host_tv.tv_sec = rd<s32>(to_addr + 0);
            host_tv.tv_usec = rd<s32>(to_addr + 4);
            p_to = &host_tv;
        }

        int res = select(nfds, p_r, p_w, p_e, p_to);
        if (res > 0) {
            if (p_r && r_fds && is_mapped(r_fds)) memcpy(h(r_fds), &read_set, fds_bytes);
            if (p_w && w_fds && is_mapped(w_fds)) memcpy(h(w_fds), &write_set, fds_bytes);
            if (p_e && e_fds && is_mapped(e_fds)) memcpy(h(e_fds), &except_set, fds_bytes);
        }
        if (p_to && to_addr && is_mapped(to_addr)) {
            wr<s32>(to_addr + 0, s32(host_tv.tv_sec));
            wr<s32>(to_addr + 4, s32(host_tv.tv_usec));
        }
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 145: {  // readv
        int fd = r[0];
        gaddr iov_p = r[1];
        int iovcnt = r[2];
        if (iovcnt < 0 || iovcnt > 1024) { r[0] = u32(-EINVAL); return; }
        std::vector<iovec> h_iov(iovcnt);
        for (int i = 0; i < iovcnt; ++i) {
            gaddr base = rd<u32>(iov_p + i * 8);
            u32 len = rd<u32>(iov_p + i * 8 + 4);
            h_iov[i].iov_base = h(base);
            h_iov[i].iov_len = len;
        }
        ssize_t n = readv(fd, h_iov.data(), iovcnt);
        r[0] = n < 0 ? -errno : n;
        return;
    }
    case 146: {  // writev
        int fd = r[0];
        gaddr iov_p = r[1];
        int iovcnt = r[2];
        if (iovcnt < 0 || iovcnt > 1024) { r[0] = u32(-EINVAL); return; }
        std::vector<iovec> h_iov(iovcnt);
        for (int i = 0; i < iovcnt; ++i) {
            gaddr base = rd<u32>(iov_p + i * 8);
            u32 len = rd<u32>(iov_p + i * 8 + 4);
            h_iov[i].iov_base = h(base);
            h_iov[i].iov_len = len;
        }
        ssize_t n = writev(fd, h_iov.data(), iovcnt);
        r[0] = n < 0 ? -errno : n;
        return;
    }
    case 148: {  // fdatasync
        int res = fdatasync(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 168: {  // poll
        gaddr fds = r[0];
        nfds_t nfds = r[1];
        int timeout = r[2];
        if (!fds && nfds > 0) {
            r[0] = u32(-EFAULT);
            return;
        }
        int res = poll(reinterpret_cast<pollfd*>(h(fds)), nfds, timeout);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 180: {  // pread64
        int fd = r[0];
        gaddr buf = r[1];
        size_t count = r[2];
        off64_t off = (u64(r[5]) << 32) | r[4];
        ssize_t n = pread(fd, h(buf), count, off);
        r[0] = n < 0 ? -errno : n;
        return;
    }
    case 181: {  // pwrite64
        int fd = r[0];
        gaddr buf = r[1];
        size_t count = r[2];
        off64_t off = (u64(r[5]) << 32) | r[4];
        ssize_t n = pwrite(fd, h(buf), count, off);
        r[0] = n < 0 ? -errno : n;
        return;
    }
    case 183: {  // getcwd
        char* p = getcwd(reinterpret_cast<char*>(h(r[0])), r[1]);
        r[0] = p ? u32(strlen(p) + 1) : -errno;
        return;
    }
    case 193: {  // truncate64
        off64_t len = (u64(r[3]) << 32) | r[2];
        int res = truncate(hs(r[0]), len);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 194: {  // ftruncate64
        off64_t len = (u64(r[3]) << 32) | r[2];
        int res = ftruncate(r[0], len);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 324: {  // fallocate
        int fd = r[0];
        int mode = r[1];
        off64_t offset = (u64(r[3]) << 32) | r[2];
        off64_t len = (u64(r[5]) << 32) | r[4];
        int res = fallocate(fd, mode, offset, len);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 195: {  // stat64
        struct stat s;
        int res = stat(headless_path(hs(r[0])), &s);
        if (res < 0) { r[0] = -errno; } else { stat_to_guest(r[1], s); r[0] = 0; }
        return;
    }
    case 196: {  // lstat64
        struct stat s;
        int res = lstat(headless_path(hs(r[0])), &s);
        if (res < 0) { r[0] = -errno; } else { stat_to_guest(r[1], s); r[0] = 0; }
        return;
    }
    case 197: {  // fstat64
        struct stat s;
        int res = fstat(r[0], &s);
        if (res < 0) { r[0] = -errno; } else { stat_to_guest(r[1], s); r[0] = 0; }
        return;
    }
    case 205: {  // getgroups32
        int sz = r[0];
        if (sz > 0 && r[1]) {
            wr<u32>(r[1], getgid());
        }
        r[0] = 1;
        return;
    }
    case 141: {  // getdents (legacy 32-bit dirent)
        int fd = r[0];
        gaddr buf = r[1];
        u32 count = r[2];
        if (!buf || !is_mapped(buf) || count < 16) {
            r[0] = u32(-EINVAL);
            return;
        }
        std::vector<u8> kbuf(std::max(count * 2, 2048u));
        int nread = syscall(SYS_getdents64, fd, kbuf.data(), kbuf.size());
        if (nread <= 0) {
            r[0] = nread < 0 ? -errno : 0;
            return;
        }
        struct sys_d64 {
            u64 d_ino;
            s64 d_off;
            u16 d_reclen;
            u8  d_type;
            char d_name[];
        };
        u32 in_pos = 0;
        u32 out_pos = 0;
        s64 last_off = 0;
        while (in_pos < (u32)nread) {
            auto* d64 = reinterpret_cast<sys_d64*>(kbuf.data() + in_pos);
            if (d64->d_reclen == 0 || in_pos + d64->d_reclen > (u32)nread) break;
            size_t namlen = strlen(d64->d_name);
            u16 reclen = (10 + namlen + 1 + 1 + 3) & ~3;  // 10 bytes prefix (4 ino + 4 off + 2 reclen) + name + null + d_type
            if (out_pos + reclen > count) {
                // Buffer full: seek directory back to last consumed entry
                if (last_off != 0) {
                    lseek(fd, last_off, SEEK_SET);
                }
                break;
            }
            gaddr cur = buf + out_pos;
            wr<u32>(cur + 0, (u32)d64->d_ino);
            wr<u32>(cur + 4, (u32)(d64->d_off & 0x7fffffff));
            wr<u16>(cur + 8, reclen);
            memcpy(h(cur + 10), d64->d_name, namlen + 1);
            memset(h(cur + 10 + namlen + 1), 0, reclen - (10 + namlen + 1));
            wr<u8>(cur + reclen - 1, d64->d_type);
            last_off = d64->d_off;
            out_pos += reclen;
            in_pos += d64->d_reclen;
        }
        r[0] = out_pos;
        return;
    }
    case 217: {  // getdents64
        int fd = r[0];
        gaddr buf = r[1];
        u32 count = r[2];
        if (!buf || !is_mapped(buf)) {
            r[0] = u32(-EINVAL);
            return;
        }
        int res = syscall(SYS_getdents64, fd, h(buf), count);
        if (res > 0) {
            struct sys_d64 {
                u64 d_ino;
                s64 d_off;
                u16 d_reclen;
                u8  d_type;
                char d_name[];
            };
            u32 pos = 0;
            while (pos < (u32)res) {
                auto* d = reinterpret_cast<sys_d64*>(static_cast<char*>(h(buf)) + pos);
                if (d->d_reclen == 0 || pos + d->d_reclen > (u32)res) break;
                // Clamp 64-bit ext4 directory cookie to avoid 32-bit signed overflow (-EOVERFLOW / 75)
                if (d->d_off > 0x7fffffffLL) {
                    d->d_off = d->d_off & 0x7fffffffLL;
                }
                pos += d->d_reclen;
            }
        }
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 221: {  // fcntl64
        int fd = r[0];
        int cmd = r[1];
        u32 arg = r[2];
        if (cmd == F_GETFL) {
            int fl = fcntl(fd, F_GETFL);
            r[0] = fl < 0 ? -errno : oflags_to_guest(fl);
        } else if (cmd == F_SETFL) {
            int res = fcntl(fd, F_SETFL, oflags_from_guest(arg));
            r[0] = res < 0 ? -errno : res;
        } else {
            int res = fcntl(fd, cmd, arg);
            r[0] = res < 0 ? -errno : res;
        }
        return;
    }
    case 322: {  // openat
        const char* p = hs(r[1]);
        int fd = openat(r[0], headless_path(p), oflags_from_guest(r[2]) | O_CLOEXEC, r[3]);
        r[0] = fd < 0 ? -errno : fd;
        return;
    }
    case 323: {  // mkdirat
        int res = mkdirat(r[0], hs(r[1]), r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 327: {  // fstatat64
        struct stat s;
        int res = fstatat(r[0], headless_path(hs(r[1])), &s, r[3]);
        if (res < 0) { r[0] = -errno; } else { stat_to_guest(r[2], s); r[0] = 0; }
        return;
    }
    case 328: {  // unlinkat
        int res = unlinkat(r[0], hs(r[1]), r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 329: {  // renameat
        int res = renameat(r[0], hs(r[1]), r[2], hs(r[3]));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 332: {  // readlinkat
        const char* p = hs(r[1]);
        if (is_proc_self_exe(p)) {
            const std::string& apk = get_guest_apk_path();
            if (!apk.empty()) {
                size_t len = std::min((size_t)r[3], apk.length());
                memcpy(h(r[2]), apk.data(), len);
                r[0] = len;
                return;
            }
        }
        ssize_t res = readlinkat(r[0], p, reinterpret_cast<char*>(h(r[2])), r[3]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 333: {  // fchmodat
        int res = fchmodat(r[0], hs(r[1]), r[2], r[3]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 334:   // faccessat
    case 439: { // faccessat2
        int res = faccessat(r[0], hs(r[1]), r[2], r[3]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 335:
    case 413: {  // pselect6 / pselect6_time64
        int nfds = r[0];
        gaddr r_fds = r[1];
        gaddr w_fds = r[2];
        gaddr e_fds = r[3];
        gaddr to_addr = r[4];

        fd_set read_set, write_set, except_set;
        fd_set* p_r = nullptr;
        fd_set* p_w = nullptr;
        fd_set* p_e = nullptr;

        size_t fds_bytes = (nfds > 0 && nfds <= 1024) ? ((nfds + 7) / 8) : 128;
        if (r_fds && is_mapped(r_fds)) {
            FD_ZERO(&read_set);
            memcpy(&read_set, h(r_fds), fds_bytes);
            p_r = &read_set;
        }
        if (w_fds && is_mapped(w_fds)) {
            FD_ZERO(&write_set);
            memcpy(&write_set, h(w_fds), fds_bytes);
            p_w = &write_set;
        }
        if (e_fds && is_mapped(e_fds)) {
            FD_ZERO(&except_set);
            memcpy(&except_set, h(e_fds), fds_bytes);
            p_e = &except_set;
        }

        struct timespec host_ts{};
        struct timespec* p_ts = nullptr;
        if (to_addr && is_mapped(to_addr)) {
            if (nr == 413) {
                host_ts.tv_sec = rd<s64>(to_addr + 0);
                host_ts.tv_nsec = rd<s64>(to_addr + 8);
            } else {
                host_ts.tv_sec = rd<s32>(to_addr + 0);
                host_ts.tv_nsec = rd<s32>(to_addr + 4);
            }
            p_ts = &host_ts;
        }

        int res = pselect(nfds, p_r, p_w, p_e, p_ts, nullptr);
        if (res > 0) {
            if (p_r && r_fds && is_mapped(r_fds)) memcpy(h(r_fds), &read_set, fds_bytes);
            if (p_w && w_fds && is_mapped(w_fds)) memcpy(h(w_fds), &write_set, fds_bytes);
            if (p_e && e_fds && is_mapped(e_fds)) memcpy(h(e_fds), &except_set, fds_bytes);
        }
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 358: {  // dup3
        int res = dup3(r[0], r[1], r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 359: {  // pipe2
        int fds[2];
        int res = pipe2(fds, r[1]);
        if (res < 0) {
            r[0] = -errno;
        } else {
            wr<s32>(r[0], fds[0]);
            wr<s32>(r[0] + 4, fds[1]);
            r[0] = 0;
        }
        return;
    }

    // Time
    case 43: {  // times
        struct tms host_tms{};
        clock_t ret = times(&host_tms);
        if (ret == (clock_t)-1) {
            r[0] = -errno;
        } else {
            if (r[0] && is_mapped(r[0])) {
                wr<s32>(r[0] + 0, s32(host_tms.tms_utime));
                wr<s32>(r[0] + 4, s32(host_tms.tms_stime));
                wr<s32>(r[0] + 8, s32(host_tms.tms_cutime));
                wr<s32>(r[0] + 12, s32(host_tms.tms_cstime));
            }
            r[0] = u32(ret);
        }
        return;
    }
    case 78: {  // gettimeofday
        struct timeval tv;
        int res = gettimeofday(&tv, nullptr);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[0]) {
                wr<s32>(r[0], s32(tv.tv_sec));
                wr<s32>(r[0] + 4, s32(tv.tv_usec));
            }
            r[0] = 0;
        }
        return;
    }
    case 162: {  // nanosleep
        struct timespec req = {rd<s32>(r[0]), rd<s32>(r[0] + 4)};
        struct timespec rem = {};
        int res = nanosleep(&req, r[1] ? &rem : nullptr);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[1]) {
                wr<s32>(r[1], s32(rem.tv_sec));
                wr<s32>(r[1] + 4, s32(rem.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 257: {  // timer_create(clockid, evp, timerid)
        clockid_t clock_id = r[0];
        gaddr evp = r[1];
        gaddr id_ptr = r[2];
        if (!id_ptr || !is_mapped(id_ptr)) {
            r[0] = u32(-EFAULT);
            return;
        }
        struct sigevent host_sev{};
        struct sigevent* p_sev = nullptr;
        if (evp && is_mapped(evp)) {
            host_sev.sigev_value.sival_int = rd<s32>(evp + 0);
            host_sev.sigev_signo = rd<s32>(evp + 4);
            host_sev.sigev_notify = rd<s32>(evp + 8);
            p_sev = &host_sev;
        }
        timer_t host_t = nullptr;
        int res = timer_create(clock_id, p_sev, &host_t);
        if (res < 0) {
            r[0] = -errno;
        } else {
            u32 gid;
            {
                std::lock_guard lk(g_timer_mu);
                gid = g_timer_seq.fetch_add(1);
                g_timer_map[gid] = host_t;
            }
            wr<u32>(id_ptr, gid);
            r[0] = 0;
        }
        return;
    }
    case 258: {  // timer_settime(timerid, flags, new_value, old_value)
        u32 gid = r[0];
        int flags = r[1];
        gaddr new_addr = r[2];
        gaddr old_addr = r[3];
        timer_t host_t = nullptr;
        {
            std::lock_guard lk(g_timer_mu);
            auto it = g_timer_map.find(gid);
            if (it == g_timer_map.end()) {
                r[0] = u32(-EINVAL);
                return;
            }
            host_t = it->second;
        }
        struct itimerspec new_ts{}, old_ts{};
        if (new_addr && is_mapped(new_addr)) {
            new_ts.it_interval.tv_sec = rd<s32>(new_addr + 0);
            new_ts.it_interval.tv_nsec = rd<s32>(new_addr + 4);
            new_ts.it_value.tv_sec = rd<s32>(new_addr + 8);
            new_ts.it_value.tv_nsec = rd<s32>(new_addr + 12);
        }
        int res = timer_settime(host_t, flags, new_addr ? &new_ts : nullptr, old_addr ? &old_ts : nullptr);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (old_addr && is_mapped(old_addr)) {
                wr<s32>(old_addr + 0, s32(old_ts.it_interval.tv_sec));
                wr<s32>(old_addr + 4, s32(old_ts.it_interval.tv_nsec));
                wr<s32>(old_addr + 8, s32(old_ts.it_value.tv_sec));
                wr<s32>(old_addr + 12, s32(old_ts.it_value.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 259: {  // timer_gettime(timerid, curr_value)
        u32 gid = r[0];
        gaddr cur_addr = r[1];
        if (!cur_addr || !is_mapped(cur_addr)) {
            r[0] = u32(-EFAULT);
            return;
        }
        timer_t host_t = nullptr;
        {
            std::lock_guard lk(g_timer_mu);
            auto it = g_timer_map.find(gid);
            if (it == g_timer_map.end()) {
                r[0] = u32(-EINVAL);
                return;
            }
            host_t = it->second;
        }
        struct itimerspec cur_ts{};
        int res = timer_gettime(host_t, &cur_ts);
        if (res < 0) {
            r[0] = -errno;
        } else {
            wr<s32>(cur_addr + 0, s32(cur_ts.it_interval.tv_sec));
            wr<s32>(cur_addr + 4, s32(cur_ts.it_interval.tv_nsec));
            wr<s32>(cur_addr + 8, s32(cur_ts.it_value.tv_sec));
            wr<s32>(cur_addr + 12, s32(cur_ts.it_value.tv_nsec));
            r[0] = 0;
        }
        return;
    }
    case 260: {  // timer_getoverrun(timerid)
        u32 gid = r[0];
        timer_t host_t = nullptr;
        {
            std::lock_guard lk(g_timer_mu);
            auto it = g_timer_map.find(gid);
            if (it == g_timer_map.end()) {
                r[0] = u32(-EINVAL);
                return;
            }
            host_t = it->second;
        }
        int res = timer_getoverrun(host_t);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 261: {  // timer_delete(timerid)
        u32 gid = r[0];
        timer_t host_t = nullptr;
        {
            std::lock_guard lk(g_timer_mu);
            auto it = g_timer_map.find(gid);
            if (it == g_timer_map.end()) {
                r[0] = u32(-EINVAL);
                return;
            }
            host_t = it->second;
            g_timer_map.erase(it);
        }
        int res = timer_delete(host_t);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 262: {  // clock_settime
        struct timespec ts = {rd<s32>(r[1]), rd<s32>(r[1] + 4)};
        int res = clock_settime(r[0], &ts);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 263: {  // clock_gettime
        struct timespec ts;
        int res = clock_gettime(r[0], &ts);
        if (res < 0) {
            r[0] = -errno;
        } else {
            wr<s32>(r[1], s32(ts.tv_sec));
            wr<s32>(r[1] + 4, s32(ts.tv_nsec));
            r[0] = 0;
        }
        return;
    }
    case 264: {  // clock_getres
        struct timespec ts;
        int res = clock_getres(r[0], &ts);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[1]) {
                wr<s32>(r[1], s32(ts.tv_sec));
                wr<s32>(r[1] + 4, s32(ts.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 265: {  // clock_nanosleep
        struct timespec req = {rd<s32>(r[2]), rd<s32>(r[2] + 4)};
        struct timespec rem = {};
        int res = clock_nanosleep(r[0], r[1], &req, r[3] ? &rem : nullptr);
        if (res != 0) {
            r[0] = -res;
        } else {
            if (r[3] && !r[1]) {
                wr<s32>(r[3], s32(rem.tv_sec));
                wr<s32>(r[3] + 4, s32(rem.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 403: {  // clock_gettime64
        struct timespec ts;
        int res = clock_gettime(r[0], &ts);
        if (res < 0) {
            r[0] = -errno;
        } else {
            wr<s64>(r[1] + 0, s64(ts.tv_sec));
            wr<s64>(r[1] + 8, s64(ts.tv_nsec));
            r[0] = 0;
        }
        return;
    }
    case 404: {  // clock_settime64
        struct timespec ts = {time_t(rd<s64>(r[1])), long(rd<s64>(r[1] + 8))};
        int res = clock_settime(r[0], &ts);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 406: {  // clock_getres_time64
        struct timespec ts;
        int res = clock_getres(r[0], &ts);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[1]) {
                wr<s64>(r[1] + 0, s64(ts.tv_sec));
                wr<s64>(r[1] + 8, s64(ts.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 407: {  // clock_nanosleep_time64
        struct timespec req = {time_t(rd<s64>(r[2])), long(rd<s64>(r[2] + 8))};
        struct timespec rem = {};
        int res = clock_nanosleep(r[0], r[1], &req, r[3] ? &rem : nullptr);
        if (res != 0) {
            r[0] = -res;
        } else {
            if (r[3] && !r[1]) {
                wr<s64>(r[3] + 0, s64(rem.tv_sec));
                wr<s64>(r[3] + 8, s64(rem.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 104: {  // setitimer
        struct itimerval new_it{}, old_it{};
        if (r[1] && is_mapped(r[1])) {
            new_it.it_interval.tv_sec = rd<s32>(r[1] + 0);
            new_it.it_interval.tv_usec = rd<s32>(r[1] + 4);
            new_it.it_value.tv_sec = rd<s32>(r[1] + 8);
            new_it.it_value.tv_usec = rd<s32>(r[1] + 12);
        }
        int res = setitimer(r[0], r[1] ? &new_it : nullptr, r[2] ? &old_it : nullptr);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[2] && is_mapped(r[2])) {
                wr<s32>(r[2] + 0, s32(old_it.it_interval.tv_sec));
                wr<s32>(r[2] + 4, s32(old_it.it_interval.tv_usec));
                wr<s32>(r[2] + 8, s32(old_it.it_value.tv_sec));
                wr<s32>(r[2] + 12, s32(old_it.it_value.tv_usec));
            }
            r[0] = 0;
        }
        return;
    }
    case 105: {  // getitimer
        struct itimerval curr_it{};
        int res = getitimer(r[0], &curr_it);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[1] && is_mapped(r[1])) {
                wr<s32>(r[1] + 0, s32(curr_it.it_interval.tv_sec));
                wr<s32>(r[1] + 4, s32(curr_it.it_interval.tv_usec));
                wr<s32>(r[1] + 8, s32(curr_it.it_value.tv_sec));
                wr<s32>(r[1] + 12, s32(curr_it.it_value.tv_usec));
            }
            r[0] = 0;
        }
        return;
    }
    case 350: {  // timerfd_create
        int res = timerfd_create(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 353: {  // timerfd_settime
        int fd = r[0];
        int flags = r[1];
        struct itimerspec new_val{}, old_val{};
        if (r[2] && is_mapped(r[2])) {
            new_val.it_interval.tv_sec = rd<s32>(r[2] + 0);
            new_val.it_interval.tv_nsec = rd<s32>(r[2] + 4);
            new_val.it_value.tv_sec = rd<s32>(r[2] + 8);
            new_val.it_value.tv_nsec = rd<s32>(r[2] + 12);
        }
        int res = timerfd_settime(fd, flags, r[2] ? &new_val : nullptr, r[3] ? &old_val : nullptr);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[3] && is_mapped(r[3])) {
                wr<s32>(r[3] + 0, s32(old_val.it_interval.tv_sec));
                wr<s32>(r[3] + 4, s32(old_val.it_interval.tv_nsec));
                wr<s32>(r[3] + 8, s32(old_val.it_value.tv_sec));
                wr<s32>(r[3] + 12, s32(old_val.it_value.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }
    case 354: {  // timerfd_gettime
        int fd = r[0];
        struct itimerspec curr_val{};
        int res = timerfd_gettime(fd, &curr_val);
        if (res < 0) {
            r[0] = -errno;
        } else {
            if (r[1] && is_mapped(r[1])) {
                wr<s32>(r[1] + 0, s32(curr_val.it_interval.tv_sec));
                wr<s32>(r[1] + 4, s32(curr_val.it_interval.tv_nsec));
                wr<s32>(r[1] + 8, s32(curr_val.it_value.tv_sec));
                wr<s32>(r[1] + 12, s32(curr_val.it_value.tv_nsec));
            }
            r[0] = 0;
        }
        return;
    }

    // Identity & system information
    case 20:   // getpid
        r[0] = u32(getpid());
        return;
    case 64:   // getppid
        r[0] = u32(getppid());
        return;
    case 116: {  // sysinfo
        int res = sysinfo(reinterpret_cast<struct sysinfo*>(h(r[0])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 122: {  // uname
        int res = uname(reinterpret_cast<struct utsname*>(h(r[0])));
        if (res < 0) {
            r[0] = -errno;
        } else {
            struct utsname* u = reinterpret_cast<struct utsname*>(h(r[0]));
            strncpy(u->machine, "armv7l", sizeof(u->machine));
            r[0] = 0;
        }
        return;
    }
    case 172: {  // prctl
        int opt = r[0];
        if (opt == PR_SET_NAME) {
            r[0] = prctl(PR_SET_NAME, hs(r[1]));
        } else if (opt == PR_GET_NAME) {
            r[0] = prctl(PR_GET_NAME, h(r[1]));
        } else {
            int res = prctl(r[0], r[1], r[2], r[3], r[4]);
            r[0] = res < 0 ? -errno : res;
        }
        return;
    }
    case 191: {  // ugetrlimit / getrlimit
        struct rlimit rl;
        int res = getrlimit(r[0], &rl);
        if (res < 0) {
            r[0] = -errno;
        } else {
            wr<u32>(r[1], u32(rl.rlim_cur));
            wr<u32>(r[1] + 4, u32(rl.rlim_max));
            r[0] = 0;
        }
        return;
    }
    case 199:  // getuid
        r[0] = getuid();
        return;
    case 200:  // getgid
        r[0] = getgid();
        return;
    case 201:  // geteuid
        r[0] = geteuid();
        return;
    case 202:  // getegid
        r[0] = getegid();
        return;
    case 224:  // gettid
        r[0] = cpu.thread->tid;
        return;
    case 369: {  // prlimit64
        int res = prlimit(r[0], r[1], r[2] ? reinterpret_cast<const struct rlimit*>(h(r[2])) : nullptr,
                          r[3] ? reinterpret_cast<struct rlimit*>(h(r[3])) : nullptr);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 77: {  // getrusage
        struct rusage ru;
        int res = getrusage(r[0], &ru);
        if (res < 0) {
            r[0] = -errno;
        } else {
            memset(h(r[1]), 0, 72);
            wr<s32>(r[1] + 0, s32(ru.ru_utime.tv_sec));
            wr<s32>(r[1] + 4, s32(ru.ru_utime.tv_usec));
            wr<s32>(r[1] + 8, s32(ru.ru_stime.tv_sec));
            wr<s32>(r[1] + 12, s32(ru.ru_stime.tv_usec));
            wr<s32>(r[1] + 16, s32(ru.ru_maxrss));
            r[0] = 0;
        }
        return;
    }

    // Signals
    case 37:   // kill
    case 67:   // sigaction
    case 126:  // sigprocmask
    case 174:  // rt_sigaction
    case 175:  // rt_sigprocmask
    case 186:  // sigaltstack
        r[0] = 0;
        return;

    // Filesystem stats
    case 266: {  // statfs64(path, sz, buf)
        struct statfs st;
        int res = statfs(hs(r[0]), &st);
        if (res < 0) {
            r[0] = -errno;
        } else {
            gaddr buf = r[2];
            u32 sz = r[1];
            if (buf && sz >= 84) {
                memset(h(buf), 0, sz);
                wr<u32>(buf + 0, (u32)st.f_type);
                wr<u32>(buf + 4, (u32)st.f_bsize);
                wr<u64>(buf + 8, (u64)st.f_blocks);
                wr<u64>(buf + 16, (u64)st.f_bfree);
                wr<u64>(buf + 24, (u64)st.f_bavail);
                wr<u64>(buf + 32, (u64)st.f_files);
                wr<u64>(buf + 40, (u64)st.f_ffree);
                memcpy(h(buf + 48), &st.f_fsid, 8);
                wr<u32>(buf + 56, (u32)st.f_namelen);
                wr<u32>(buf + 60, (u32)st.f_frsize);
                wr<u32>(buf + 64, (u32)st.f_flags);
            }
            r[0] = 0;
        }
        return;
    }
    case 267: {  // fstatfs64(fd, sz, buf)
        struct statfs st;
        int res = fstatfs(r[0], &st);
        if (res < 0) {
            r[0] = -errno;
        } else {
            gaddr buf = r[2];
            u32 sz = r[1];
            if (buf && sz >= 84) {
                memset(h(buf), 0, sz);
                wr<u32>(buf + 0, (u32)st.f_type);
                wr<u32>(buf + 4, (u32)st.f_bsize);
                wr<u64>(buf + 8, (u64)st.f_blocks);
                wr<u64>(buf + 16, (u64)st.f_bfree);
                wr<u64>(buf + 24, (u64)st.f_bavail);
                wr<u64>(buf + 32, (u64)st.f_files);
                wr<u64>(buf + 40, (u64)st.f_ffree);
                memcpy(h(buf + 48), &st.f_fsid, 8);
                wr<u32>(buf + 56, (u32)st.f_namelen);
                wr<u32>(buf + 60, (u32)st.f_frsize);
                wr<u32>(buf + 64, (u32)st.f_flags);
            }
            r[0] = 0;
        }
        return;
    }

    // Sockets & IPC
    case 250: {  // epoll_create
        int res = epoll_create(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 251: {  // epoll_ctl
        int epfd = r[0];
        int op = r[1];
        int fd = r[2];
        gaddr ev_addr = r[3];
        struct epoll_event host_ev{};
        struct epoll_event* p_ev = nullptr;
        if (ev_addr && is_mapped(ev_addr)) {
            host_ev.events = rd<u32>(ev_addr);
            host_ev.data.u64 = rd<u64>(ev_addr + 4);
            p_ev = &host_ev;
        }
        int res = epoll_ctl(epfd, op, fd, p_ev);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 252: {  // epoll_wait
        int epfd = r[0];
        gaddr ev_addr = r[1];
        int maxevents = r[2];
        int timeout = r[3];
        if (maxevents <= 0 || !ev_addr || !is_mapped(ev_addr)) {
            r[0] = u32(-EINVAL);
            return;
        }
        int safe_max = std::min(maxevents, 128);
        struct epoll_event host_events[128];
        int res = epoll_wait(epfd, host_events, safe_max, timeout);
        if (res > 0) {
            for (int i = 0; i < res; ++i) {
                gaddr out = ev_addr + i * 12;
                wr<u32>(out + 0, host_events[i].events);
                wr<u64>(out + 4, host_events[i].data.u64);
            }
        }
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 346: {  // epoll_pwait
        int epfd = r[0];
        gaddr ev_addr = r[1];
        int maxevents = r[2];
        int timeout = r[3];
        gaddr sigmask_addr = r[4];
        sigset_t host_sigmask;
        sigset_t* p_sigmask = nullptr;
        if (sigmask_addr && is_mapped(sigmask_addr)) {
            sigemptyset(&host_sigmask);
            u32 gsig = rd<u32>(sigmask_addr);
            for (int s = 1; s <= 32; ++s) {
                if (gsig & (1u << (s - 1))) sigaddset(&host_sigmask, s);
            }
            p_sigmask = &host_sigmask;
        }
        if (maxevents <= 0 || !ev_addr || !is_mapped(ev_addr)) {
            r[0] = u32(-EINVAL);
            return;
        }
        int safe_max = std::min(maxevents, 128);
        struct epoll_event host_events[128];
        int res = epoll_pwait(epfd, host_events, safe_max, timeout, p_sigmask);
        if (res > 0) {
            for (int i = 0; i < res; ++i) {
                gaddr out = ev_addr + i * 12;
                wr<u32>(out + 0, host_events[i].events);
                wr<u64>(out + 4, host_events[i].data.u64);
            }
        }
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 281: {  // socket
        int res = socket(r[0], r[1], r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 282: {  // bind
        int res = bind(r[0], reinterpret_cast<const sockaddr*>(h(r[1])), r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 283: {  // connect
        int res = connect(r[0], reinterpret_cast<const sockaddr*>(h(r[1])), r[2]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 284: {  // listen
        int res = listen(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 285: {  // accept
        int res = accept(r[0], reinterpret_cast<sockaddr*>(h(r[1])),
                         reinterpret_cast<socklen_t*>(h(r[2])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 286: {  // getsockname
        int res = getsockname(r[0], reinterpret_cast<sockaddr*>(h(r[1])), reinterpret_cast<socklen_t*>(h(r[2])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 287: {  // getpeername
        int res = getpeername(r[0], reinterpret_cast<sockaddr*>(h(r[1])), reinterpret_cast<socklen_t*>(h(r[2])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 288: {  // socketpair
        int res = socketpair(r[0], r[1], r[2], reinterpret_cast<int*>(h(r[3])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 290: {  // sendto
        ssize_t res = sendto(r[0], h(r[1]), r[2], r[3],
                             r[4] ? reinterpret_cast<const sockaddr*>(h(r[4])) : nullptr, r[5]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 292: {  // recvfrom
        ssize_t res = recvfrom(r[0], h(r[1]), r[2], r[3],
                              r[4] ? reinterpret_cast<sockaddr*>(h(r[4])) : nullptr,
                              reinterpret_cast<socklen_t*>(h(r[5])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 293: {  // shutdown
        int res = shutdown(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 294: {  // setsockopt
        int res = setsockopt(r[0], r[1], r[2], h(r[3]), r[4]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 295: {  // getsockopt
        int res = getsockopt(r[0], r[1], r[2], h(r[3]), reinterpret_cast<socklen_t*>(h(r[4])));
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 296: {  // sendmsg
        int fd = r[0];
        gaddr g_msg = r[1];
        int flags = r[2];
        if (!g_msg || !is_mapped(g_msg)) { r[0] = u32(-EFAULT); return; }
        gaddr name_p = rd<u32>(g_msg + 0);
        u32 namelen = rd<u32>(g_msg + 4);
        gaddr iov_p = rd<u32>(g_msg + 8);
        u32 iovcnt = rd<u32>(g_msg + 12);
        gaddr ctl_p = rd<u32>(g_msg + 16);
        u32 ctllen = rd<u32>(g_msg + 20);

        if (iovcnt > 1024) { r[0] = u32(-EINVAL); return; }
        std::vector<iovec> h_iov(iovcnt);
        for (u32 i = 0; i < iovcnt; ++i) {
            h_iov[i].iov_base = h(rd<u32>(iov_p + i * 8));
            h_iov[i].iov_len = rd<u32>(iov_p + i * 8 + 4);
        }
        msghdr h_msg{};
        h_msg.msg_name = name_p ? h(name_p) : nullptr;
        h_msg.msg_namelen = namelen;
        h_msg.msg_iov = h_iov.data();
        h_msg.msg_iovlen = iovcnt;
        h_msg.msg_control = ctl_p ? h(ctl_p) : nullptr;
        h_msg.msg_controllen = ctllen;
        h_msg.msg_flags = 0;

        ssize_t res = sendmsg(fd, &h_msg, flags);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 297: {  // recvmsg
        int fd = r[0];
        gaddr g_msg = r[1];
        int flags = r[2];
        if (!g_msg || !is_mapped(g_msg)) { r[0] = u32(-EFAULT); return; }
        gaddr name_p = rd<u32>(g_msg + 0);
        u32 namelen = rd<u32>(g_msg + 4);
        gaddr iov_p = rd<u32>(g_msg + 8);
        u32 iovcnt = rd<u32>(g_msg + 12);
        gaddr ctl_p = rd<u32>(g_msg + 16);
        u32 ctllen = rd<u32>(g_msg + 20);

        if (iovcnt > 1024) { r[0] = u32(-EINVAL); return; }
        std::vector<iovec> h_iov(iovcnt);
        for (u32 i = 0; i < iovcnt; ++i) {
            h_iov[i].iov_base = h(rd<u32>(iov_p + i * 8));
            h_iov[i].iov_len = rd<u32>(iov_p + i * 8 + 4);
        }
        msghdr h_msg{};
        h_msg.msg_name = name_p ? h(name_p) : nullptr;
        h_msg.msg_namelen = namelen;
        h_msg.msg_iov = h_iov.data();
        h_msg.msg_iovlen = iovcnt;
        h_msg.msg_control = ctl_p ? h(ctl_p) : nullptr;
        h_msg.msg_controllen = ctllen;
        h_msg.msg_flags = 0;

        ssize_t res = recvmsg(fd, &h_msg, flags);
        if (res >= 0) {
            wr<u32>(g_msg + 4, h_msg.msg_namelen);
            wr<u32>(g_msg + 20, h_msg.msg_controllen);
            wr<s32>(g_msg + 24, h_msg.msg_flags);
        }
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 336: {  // ppoll
        struct timespec host_ts;
        struct timespec* to_ptr = nullptr;
        if (r[2]) {
            struct guest_ts { s32 sec; s32 nsec; };
            guest_ts* gts = reinterpret_cast<guest_ts*>(h(r[2]));
            host_ts.tv_sec = gts->sec;
            host_ts.tv_nsec = gts->nsec;
            to_ptr = &host_ts;
        }
        int res = ppoll(reinterpret_cast<pollfd*>(h(r[0])), r[1], to_ptr,
                        r[3] ? reinterpret_cast<const sigset_t*>(h(r[3])) : nullptr);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 338: {  // set_robust_list
        cpu.thread->robust_list_head = r[0];
        cpu.thread->robust_list_len = r[1];
        r[0] = 0;
        return;
    }
    case 339: {  // get_robust_list
        if (r[1] && is_mapped(r[1])) wr<u32>(r[1], cpu.thread->robust_list_head);
        if (r[2] && is_mapped(r[2])) wr<u32>(r[2], cpu.thread->robust_list_len);
        r[0] = 0;
        return;
    }
    case 356: {  // eventfd2
        int res = eventfd(r[0], r[1]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 357: {  // epoll_create1
        int res = epoll_create1(r[0]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 366: {  // accept4
        int res = accept4(r[0], reinterpret_cast<sockaddr*>(h(r[1])),
                          reinterpret_cast<socklen_t*>(h(r[2])), r[3]);
        r[0] = res < 0 ? -errno : res;
        return;
    }
    case 384: {  // getrandom
        gaddr buf = r[0];
        size_t buflen = r[1];
        unsigned int flags = r[2];
        if (!buf || !is_mapped(buf)) {
            r[0] = u32(-EFAULT);
            return;
        }
#if defined(SYS_getrandom)
        ssize_t res = syscall(SYS_getrandom, h(buf), buflen, flags);
        r[0] = res < 0 ? -errno : res;
#elif defined(__NR_getrandom)
        ssize_t res = syscall(__NR_getrandom, h(buf), buflen, flags);
        r[0] = res < 0 ? -errno : res;
#else
        int urfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (urfd >= 0) {
            ssize_t n = read(urfd, h(buf), buflen);
            close(urfd);
            r[0] = n < 0 ? -errno : n;
        } else {
            r[0] = u32(-ENOSYS);
        }
#endif
        return;
    }
    case 436: {  // close_range
        u32 first = r[0];
        u32 last = r[1];
        if (first > last) {
            r[0] = u32(-EINVAL);
            return;
        }
#if defined(__NR_close_range)
        int res = syscall(__NR_close_range, first, last, r[2]);
        if (res == 0) {
            r[0] = 0;
            return;
        }
#endif
        u32 cap = std::min(last, 1024u);
        for (u32 fd = first; fd <= cap; ++fd) {
            close(fd);
        }
        r[0] = 0;
        return;
    }

    default:
        if (get_log_level() >= LOG_LEVEL_ADVANCED) {
            cpu.dump("raw syscall");
        }
        NB_LOGW("unsupported raw syscall %u", nr);
        r[0] = u32(-ENOSYS);
    }
}

void Cpu::CallSVC(u32 swi) {
    auto& r = jit->Regs();
    if (swi >= kSvcThunkBase) {
        u32 idx = swi - kSvcThunkBase;
        if (idx >= g_handler_count.load(std::memory_order_acquire)) fatal("bad thunk svc %#x", swi);
        const Import* imp = &g_handlers[idx];
        Ctx c(*this, imp->name.c_str());
        imp->fn(c);
        return;
    }
    switch (swi) {
    case kSvcReturn:
        returned = true;
        jit->HaltExecution();
        return;
    case kSvcKuserCmpxchg: {
        bool ok = cas<u32>(r[2], r[1], r[0]);
        r[0] = ok ? 0 : 1;
        u32 cpsr = jit->Cpsr();
        jit->SetCpsr(ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
        return;
    }
    case kSvcKuserCmpxchg64: {
        u64 oldv = rd<u64>(r[0]), newv = rd<u64>(r[1]);
        bool ok = cas<u64>(r[2], newv, oldv);
        r[0] = ok ? 0 : 1;
        u32 cpsr = jit->Cpsr();
        jit->SetCpsr(ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
        return;
    }
    case kSvcKuserGetTls:
        r[0] = tls_word ? tls_word : thread->tls;
        return;
    case 0:
        do_syscall(*this);
        return;
    default:
        dump("unknown svc");
        fatal("unknown svc #%#x", swi);
    }
}

Ctx::Ctx(Cpu& c, const char* nm) : cpu(c), r(c.jit->Regs()), name(nm), nsaa(c.jit->Regs()[13]) {}
GuestThread& Ctx::thread() { return *cpu.thread; }

// ---------------------------------------------------------------- calls

u64 call_guest(gaddr fn, std::initializer_list<GArg> args) { return call_guest(fn, args.begin(), args.size()); }

u64 call_guest(gaddr fn, const GArg* args, size_t n) {
    if (!fn) fatal("call_guest: null function");
    GuestThread& t = cur_thread();
    int d = ++t.depth;
    if (int(t.cpus.size()) <= d) t.cpus.push_back(std::make_unique<Cpu>(&t, d));
    Cpu& c = *t.cpus[d];

    gaddr sp = d == 0 ? t.stack_base + t.stack_size : t.cpus[d - 1]->jit->Regs()[13];
    sp = (sp - 256) & ~7u;

    // AAPCS argument assignment.
    u32 regs[4] = {};
    int ncrn = 0;
    std::vector<std::pair<u32, u32>> stacked;  // (offset, word)
    u32 nsaa = 0;
    for (size_t i = 0; i < n; ++i) {
        if (args[i].wide) {
            ncrn += ncrn & 1;
            if (ncrn <= 2) {
                regs[ncrn++] = u32(args[i].v);
                regs[ncrn++] = u32(args[i].v >> 32);
            } else {
                ncrn = 4;
                nsaa = (nsaa + 7) & ~7u;
                stacked.push_back({nsaa, u32(args[i].v)});
                stacked.push_back({nsaa + 4, u32(args[i].v >> 32)});
                nsaa += 8;
            }
        } else if (ncrn < 4) {
            regs[ncrn++] = u32(args[i].v);
        } else {
            stacked.push_back({nsaa, u32(args[i].v)});
            nsaa += 4;
        }
    }
    sp = (sp - nsaa) & ~7u;
    for (auto [off, w] : stacked) wr<u32>(sp + off, w);

    auto& R = c.jit->Regs();
    for (int i = 0; i < 4; ++i) R[i] = regs[i];
    R[13] = sp;
    R[14] = g_return_trap;
    R[15] = fn & ~1u;
    c.jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));  // USR mode, ARM/Thumb per target
    c.jit->SetFpscr(0);
    c.jit->ClearExclusiveState();
    c.returned = false;
    while (!c.returned && !t.exiting) {
        auto hr = c.jit->Run();
        if (bool(hr & Dynarmic::HaltReason::UserDefined1)) {
            c.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        }
    }
    u64 res = R[0] | (u64(R[1]) << 32);
    --t.depth;
    return res;
}

// ---------------------------------------------------------------- setup

void cpu_init() {
    g_monitor = new Dynarmic::ExclusiveMonitor(kMaxCpus);

    g_return_trap = misc_alloc(8, 8);
    wr<u32>(g_return_trap, kArmSvc | kSvcReturn);
    wr<u32>(g_return_trap + 4, kArmBxLr);

    // ARM Linux kernel user helpers (used by libgcc's __sync_* implementation).
    using layout::kKuserPage;
    mem_protect(kKuserPage, kPageSize, PROT_READ | PROT_WRITE);
    wr<u32>(kKuserPage + 0xf60, kArmSvc | kSvcKuserCmpxchg64);
    wr<u32>(kKuserPage + 0xf64, kArmBxLr);
    wr<u32>(kKuserPage + 0xfa0, kArmDmbIsh);
    wr<u32>(kKuserPage + 0xfa4, kArmBxLr);
    wr<u32>(kKuserPage + 0xfc0, kArmSvc | kSvcKuserCmpxchg);
    wr<u32>(kKuserPage + 0xfc4, kArmBxLr);
    wr<u32>(kKuserPage + 0xfe0, kArmSvc | kSvcKuserGetTls);
    wr<u32>(kKuserPage + 0xfe4, kArmBxLr);
    wr<u32>(kKuserPage + 0xffc, 5);  // __kuser_helper_version
}

}  // namespace nb
