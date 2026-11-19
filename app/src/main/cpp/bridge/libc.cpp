// libc / libm / libdl imports of the guest.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <fnmatch.h>
#include <utime.h>
#include <sys/statvfs.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <unordered_map>

#include "cpu.h"
#include "nb.h"

#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif

namespace nb {
namespace {

template <class T>
T sysret(T v) {
    if (v == T(-1)) errno_from_host();
    return v;
}

// ------------------------------------------------------------------ runtime support

void x_errno(Ctx& c) { c.ret(cur_thread().tls + tlsoff::kErrno); }

void x_stack_chk_fail(Ctx& c) {
    c.cpu.dump("stack smashing");
    fatal("guest: stack corruption detected (__stack_chk_fail)");
}

void x_assert2(Ctx& c) {
    const char* file = hs(c.a32());
    int line = int(c.a32());
    const char* func = hs(c.a32());
    const char* msg = hs(c.a32());
    c.cpu.dump("assert");
    fatal("guest assertion failed: %s:%d (%s): %s", file ? file : "?", line, func ? func : "?", msg ? msg : "?");
}

void x_abort(Ctx& c) {
    c.cpu.dump("abort");
    fatal("guest called abort()");
}

void x_exit(Ctx& c) {
    int code = int(c.a32());
    c.cpu.dump("exit");
    fatal("guest called exit(%d)", code);
}

void x_raise(Ctx& c) {
    int sig = int(c.a32());
    c.cpu.dump("raise");
    if (sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGKILL || sig == SIGTERM)
        fatal("guest raised signal %d", sig);
    NB_LOGW("guest raise(%d) ignored", sig);
    c.ret(0);
}

void x_bsd_signal(Ctx& c) {
    int sig = int(c.a32());
    u32 handler = c.a32();
    NB_LOGI("guest signal(%d, %#x) ignored", sig, handler);
    c.ret(0);  // SIG_DFL
}

void x_ret0(Ctx& c) { c.ret(0); }

void x_find_exidx(Ctx& c) {
    gaddr pc = c.a32();
    gaddr pcount = c.a32();
    gaddr start;
    u32 count;
    guest_exidx(pc, &start, &count);
    if (pcount) wr<s32>(pcount, s32(count));
    c.ret(start);
}

// jmp_buf: r4-r11, sp, lr, s16-s31, fpscr
void x_setjmp(Ctx& c) {
    gaddr buf = c.a32();
    for (int i = 0; i < 8; ++i) wr<u32>(buf + i * 4, c.r[4 + i]);
    wr<u32>(buf + 32, c.r[13]);
    wr<u32>(buf + 36, c.r[14]);
    auto& ext = c.cpu.jit->ExtRegs();
    for (int i = 0; i < 16; ++i) wr<u32>(buf + 40 + i * 4, ext[16 + i]);
    wr<u32>(buf + 104, c.cpu.jit->Fpscr());
    c.ret(0);
}

void x_longjmp(Ctx& c) {
    gaddr buf = c.a32();
    u32 val = c.a32();
    for (int i = 0; i < 8; ++i) c.r[4 + i] = rd<u32>(buf + i * 4);
    c.r[13] = rd<u32>(buf + 32);
    c.r[14] = rd<u32>(buf + 36);  // the stub's "bx lr" resumes after setjmp
    auto& ext = c.cpu.jit->ExtRegs();
    for (int i = 0; i < 16; ++i) ext[16 + i] = rd<u32>(buf + 40 + i * 4);
    c.cpu.jit->SetFpscr(rd<u32>(buf + 104));
    c.r[0] = val ? val : 1;
}

// ------------------------------------------------------------------ memory & strings

void x_aeabi_memclr(Ctx& c) {
    gaddr d = c.a32();
    u32 n = c.a32();
    if (n) memset(h(d), 0, n);
}
void x_aeabi_memcpy(Ctx& c) {
    gaddr d = c.a32(), s = c.a32();
    u32 n = c.a32();
    if (n) memcpy(h(d), h(s), n);
}
void x_aeabi_memmove(Ctx& c) {
    gaddr d = c.a32(), s = c.a32();
    u32 n = c.a32();
    if (n) memmove(h(d), h(s), n);
}
void x_aeabi_memset(Ctx& c) {  // note: (dest, n, c)
    gaddr d = c.a32();
    u32 n = c.a32();
    int v = int(c.a32());
    if (n) memset(h(d), v, n);
}

// ------------------------------------------------------------------ ARM EABI helpers

static inline double get_double(Ctx& c) {
    u64 bits = c.a64();
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}
static inline void ret_double(Ctx& c, double d) {
    u64 bits;
    std::memcpy(&bits, &d, sizeof bits);
    c.ret64(bits);
}
static inline float get_float(Ctx& c) {
    u32 bits = c.a32();
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}
static inline void ret_float(Ctx& c, float f) {
    u32 bits;
    std::memcpy(&bits, &f, sizeof bits);
    c.ret(bits);
}

void x_aeabi_l2d(Ctx& c) {
    s64 v = (s64)c.a64();
    ret_double(c, (double)v);
}
void x_aeabi_ul2d(Ctx& c) {
    u64 v = c.a64();
    ret_double(c, (double)v);
}
void x_aeabi_i2d(Ctx& c) {
    s32 v = (s32)c.a32();
    ret_double(c, (double)v);
}
void x_aeabi_ui2d(Ctx& c) {
    u32 v = c.a32();
    ret_double(c, (double)v);
}
void x_aeabi_l2f(Ctx& c) {
    s64 v = (s64)c.a64();
    ret_float(c, (float)v);
}
void x_aeabi_ul2f(Ctx& c) {
    u64 v = c.a64();
    ret_float(c, (float)v);
}
void x_aeabi_d2lz(Ctx& c) {
    double d = get_double(c);
    c.ret64((u64)(s64)d);
}
void x_aeabi_d2ulz(Ctx& c) {
    double d = get_double(c);
    c.ret64((u64)d);
}
void x_aeabi_d2uiz(Ctx& c) {
    double d = get_double(c);
    c.ret((u32)d);
}
void x_aeabi_d2iz(Ctx& c) {
    double d = get_double(c);
    c.ret((u32)(s32)d);
}
void x_aeabi_f2d(Ctx& c) {
    float f = get_float(c);
    ret_double(c, (double)f);
}
void x_aeabi_d2f(Ctx& c) {
    double d = get_double(c);
    ret_float(c, (float)d);
}
void x_aeabi_f2lz(Ctx& c) {
    float f = get_float(c);
    c.ret64((u64)(s64)f);
}
void x_aeabi_f2ulz(Ctx& c) {
    float f = get_float(c);
    c.ret64((u64)f);
}
void x_aeabi_f2uiz(Ctx& c) {
    float f = get_float(c);
    c.ret((u32)f);
}
void x_aeabi_f2iz(Ctx& c) {
    float f = get_float(c);
    c.ret((u32)(s32)f);
}

void x_aeabi_dadd(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    ret_double(c, a + b);
}
void x_aeabi_dsub(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    ret_double(c, a - b);
}
void x_aeabi_drsub(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    ret_double(c, b - a);
}
void x_aeabi_dmul(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    ret_double(c, a * b);
}
void x_aeabi_ddiv(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    ret_double(c, a / b);
}

void x_aeabi_fadd(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    ret_float(c, a + b);
}
void x_aeabi_fsub(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    ret_float(c, a - b);
}
void x_aeabi_frsub(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    ret_float(c, b - a);
}
void x_aeabi_fmul(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    ret_float(c, a * b);
}
void x_aeabi_fdiv(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    ret_float(c, a / b);
}

void x_aeabi_dcmpeq(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    c.ret(a == b ? 1 : 0);
}
void x_aeabi_dcmplt(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    c.ret(a < b ? 1 : 0);
}
void x_aeabi_dcmple(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    c.ret(a <= b ? 1 : 0);
}
void x_aeabi_dcmpge(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    c.ret(a >= b ? 1 : 0);
}
void x_aeabi_dcmpgt(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    c.ret(a > b ? 1 : 0);
}

void x_aeabi_fcmpeq(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    c.ret(a == b ? 1 : 0);
}
void x_aeabi_fcmplt(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    c.ret(a < b ? 1 : 0);
}
void x_aeabi_fcmple(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    c.ret(a <= b ? 1 : 0);
}
void x_aeabi_fcmpge(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    c.ret(a >= b ? 1 : 0);
}
void x_aeabi_fcmpgt(Ctx& c) {
    float a = get_float(c);
    float b = get_float(c);
    c.ret(a > b ? 1 : 0);
}

static void set_fp_flags(Ctx& c, double a, double b) {
    u32 cpsr = c.cpu.jit->Cpsr() & ~0xF0000000u;
    if (std::isnan(a) || std::isnan(b)) {
        cpsr |= 0x10000000u; // V=1
    } else if (a == b) {
        cpsr |= 0x60000000u; // Z=1, C=1
    } else if (a < b) {
        cpsr |= 0x80000000u; // N=1
    } else {
        cpsr |= 0x20000000u; // C=1
    }
    c.cpu.jit->SetCpsr(cpsr);
}

void x_aeabi_cdcmpeq(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    set_fp_flags(c, a, b);
}
void x_aeabi_cdcmple(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    set_fp_flags(c, a, b);
}
void x_aeabi_cdrcmple(Ctx& c) {
    double a = get_double(c);
    double b = get_double(c);
    set_fp_flags(c, b, a); // reversed operands for cdrcmple
}

void x_aeabi_idiv(Ctx& c) {
    s32 num = (s32)c.a32();
    s32 den = (s32)c.a32();
    if (den == 0) c.ret(0);
    else c.ret((u32)(num / den));
}
void x_aeabi_idivmod(Ctx& c) {
    s32 num = (s32)c.a32();
    s32 den = (s32)c.a32();
    if (den == 0) {
        c.r[0] = 0;
        c.r[1] = 0;
    } else {
        c.r[0] = (u32)(num / den);
        c.r[1] = (u32)(num % den);
    }
}
void x_aeabi_uidiv(Ctx& c) {
    u32 num = c.a32();
    u32 den = c.a32();
    if (den == 0) c.ret(0);
    else c.ret(num / den);
}
void x_aeabi_uidivmod(Ctx& c) {
    u32 num = c.a32();
    u32 den = c.a32();
    if (den == 0) {
        c.r[0] = 0;
        c.r[1] = 0;
    } else {
        c.r[0] = num / den;
        c.r[1] = num % den;
    }
}
void x_aeabi_ldivmod(Ctx& c) {
    s64 num = (s64)c.a64();
    s64 den = (s64)c.a64();
    if (den == 0) {
        c.r[0] = 0; c.r[1] = 0;
        c.r[2] = 0; c.r[3] = 0;
    } else {
        s64 q = num / den;
        s64 r = num % den;
        c.r[0] = (u32)q;
        c.r[1] = (u32)(q >> 32);
        c.r[2] = (u32)r;
        c.r[3] = (u32)(r >> 32);
    }
}
void x_aeabi_uldivmod(Ctx& c) {
    u64 num = c.a64();
    u64 den = c.a64();
    if (den == 0) {
        c.r[0] = 0; c.r[1] = 0;
        c.r[2] = 0; c.r[3] = 0;
    } else {
        u64 q = num / den;
        u64 r = num % den;
        c.r[0] = (u32)q;
        c.r[1] = (u32)(q >> 32);
        c.r[2] = (u32)r;
        c.r[3] = (u32)(r >> 32);
    }
}
void x_aeabi_idiv0(Ctx& c) { c.ret(0); }
void x_aeabi_ldiv0(Ctx& c) { c.ret64(0); }
void x_aeabi_unwind_cpp_pr(Ctx& c) { c.ret(0); }
void* x_memcpy(void* d, const void* s, u32 n) { return n ? memcpy(d, s, n) : d; }
void* x_memmove(void* d, const void* s, u32 n) { return n ? memmove(d, s, n) : d; }
void* x_memset(void* d, int v, u32 n) { return n ? memset(d, v, n) : d; }
int x_memcmp(const void* a, const void* b, u32 n) { return n ? memcmp(a, b, n) : 0; }
void* x_memchr(const void* s, int ch, u32 n) { return n ? const_cast<void*>(memchr(s, ch, n)) : nullptr; }
u32 x_strlen(const char* s) { return u32(strlen(s)); }
int x_strcmp(const char* a, const char* b) { return strcmp(a, b); }
int x_strncmp(const char* a, const char* b, u32 n) { return strncmp(a, b, n); }
int x_strcasecmp(const char* a, const char* b) { return strcasecmp(a, b); }
int x_strcoll(const char* a, const char* b) { return strcmp(a, b); }
u32 x_strxfrm(char* d, const char* s, u32 n) {
    size_t l = strlen(s);
    if (n) {
        size_t k = l < n - 1 ? l : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return u32(l);
}
char* x_strcpy(char* d, const char* s) { return strcpy(d, s); }
char* x_strncpy(char* d, const char* s, u32 n) { return strncpy(d, s, n); }
char* x_strcat(char* d, const char* s) { return strcat(d, s); }
char* x_strchr(const char* s, int ch) { return const_cast<char*>(strchr(s, ch)); }
char* x_strrchr(const char* s, int ch) { return const_cast<char*>(strrchr(s, ch)); }
char* x_strstr(const char* s, const char* n) { return const_cast<char*>(strstr(s, n)); }
u32 x_strdup(const char* s) { return gstrdup(s); }

void x_strerror(Ctx& c) {
    int e = int(c.a32());
    gaddr buf = cur_thread().tls + tlsoff::kStrerror;
    snprintf(hp<char>(buf), 256, "%s", strerror(e));
    c.ret(buf);
}

// ------------------------------------------------------------------ ctype

int ct_ok(int ch) { return ch >= -1 && ch <= 255; }
int x_isalnum(int ch) { return ct_ok(ch) && ch >= 0 && ch < 128 && isalnum(ch); }
int x_isalpha(int ch) { return ct_ok(ch) && ch >= 0 && ch < 128 && isalpha(ch); }
int x_isdigit(int ch) { return ch >= '0' && ch <= '9'; }
int x_isgraph(int ch) { return ch > 32 && ch < 127; }
int x_islower(int ch) { return ch >= 'a' && ch <= 'z'; }
int x_isupper(int ch) { return ch >= 'A' && ch <= 'Z'; }
int x_isprint(int ch) { return ch >= 32 && ch < 127; }
int x_isspace(int ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); }
int x_tolower(int ch) { return x_isupper(ch) ? ch + 32 : ch; }
int x_toupper(int ch) { return x_islower(ch) ? ch - 32 : ch; }

void make_ctype_tables() {
    enum { U = 1, L = 2, N = 4, S = 8, P = 0x10, C = 0x20, X = 0x40, B = 0x80 };
    gaddr ctab = misc_alloc(257 + 3, 4);
    wr<u8>(ctab, 0);  // EOF
    for (int ch = 0; ch < 256; ++ch) {
        u8 v = 0;
        if (ch < 128) {
            if (ch >= 'A' && ch <= 'Z') v |= U;
            if (ch >= 'a' && ch <= 'z') v |= L;
            if (ch >= '0' && ch <= '9') v |= N;
            if (x_isspace(ch)) v |= S;
            if (ispunct(ch)) v |= P;
            if (ch < 32 || ch == 127) v |= C;
            if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) v |= X;
            if (ch == ' ') v |= B;
        }
        wr<u8>(ctab + 1 + ch, v);
    }
    gaddr lower = misc_alloc(257 * 2, 4), upper = misc_alloc(257 * 2, 4);
    wr<s16>(lower, -1);
    wr<s16>(upper, -1);
    for (int ch = 0; ch < 256; ++ch) {
        wr<s16>(lower + 2 + ch * 2, s16(x_tolower(ch)));
        wr<s16>(upper + 2 + ch * 2, s16(x_toupper(ch)));
    }
    // The imported symbols are pointer variables that point at the tables.
    gaddr p_ctype = misc_alloc(4), p_lower = misc_alloc(4), p_upper = misc_alloc(4);
    wr<u32>(p_ctype, ctab);
    wr<u32>(p_lower, lower);
    wr<u32>(p_upper, upper);
    reg_data("_ctype_", p_ctype);
    reg_data("_tolower_tab_", p_lower);
    reg_data("_toupper_tab_", p_upper);
}

// ------------------------------------------------------------------ stdlib

#ifdef NB_HEADLESS
// Leak hunting: tag each guest malloc with the guest return address, plus the next code address on
// the stack (the caller of operator new, typically).
u64 guest_tag(Ctx& c) {
    u32 caller = 0;
    for (gaddr p = c.r[13]; p < c.r[13] + 96; p += 4) {
        u32 v = rd<u32>(p);
        if (guest_is_code(v) && v != c.r[14]) {
            caller = v;
            break;
        }
    }
    return (u64(caller) << 32) | c.r[14] | 0x80000000u;
}
void x_malloc(Ctx& c) {
    u32 n = c.a32();
    gaddr p = gmalloc(n);
    heap_track(p, n, guest_tag(c));
    c.ret(p);
}
void x_calloc(Ctx& c) {
    u32 n = c.a32(), sz = c.a32();
    gaddr p = gcalloc(n, sz);
    heap_track(p, n * sz, guest_tag(c));
    c.ret(p);
}
void x_realloc(Ctx& c) {
    u32 old = c.a32(), n = c.a32();
    heap_untrack(old);
    gaddr p = grealloc(old, n);
    heap_track(p, n, guest_tag(c));
    c.ret(p);
}
void x_free(Ctx& c) {
    u32 p = c.a32();
    heap_untrack(p);
    gfree(p);
}
#else
u32 x_malloc(u32 n) { return gmalloc(n); }
u32 x_calloc(u32 n, u32 s) { return gcalloc(n, s); }
u32 x_realloc(u32 p, u32 n) { return grealloc(p, n); }
void x_free(u32 p) { gfree(p); }
#endif

int x_atoi(const char* s) { return int(strtol(s, nullptr, 10)); }

s32 clamp32(long long v, bool& range) {
    if (v > INT32_MAX) {
        range = true;
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        range = true;
        return INT32_MIN;
    }
    return s32(v);
}

s32 x_strtol(const char* s, u32 endp, int base) {
    char* e;
    errno = 0;
    long long v = strtoll(s, &e, base);
    bool range = errno == ERANGE;
    s32 r = clamp32(v, range);
    if (range) set_guest_errno(ERANGE);
    if (endp) wr<u32>(endp, g(e));
    return r;
}

u32 x_strtoul(const char* s, u32 endp, int base) {
    const char* p = s;
    while (x_isspace(*p)) ++p;
    bool neg = *p == '-';
    if (*p == '-' || *p == '+') ++p;
    char* e;
    errno = 0;
    unsigned long long m = strtoull(p, &e, base);
    if (e == p) e = const_cast<char*>(s);  // no digits
    u32 r;
    if (errno == ERANGE || m > UINT32_MAX) {
        set_guest_errno(ERANGE);
        r = UINT32_MAX;
    } else {
        r = neg ? u32(-u32(m)) : u32(m);
    }
    if (endp) wr<u32>(endp, g(e));
    return r;
}

s64 x_strtoll(const char* s, u32 endp, int base) {
    char* e;
    errno = 0;
    long long v = strtoll(s, &e, base);
    if (errno) errno_from_host();
    if (endp) wr<u32>(endp, g(e));
    return v;
}

double x_strtod(const char* s, u32 endp) {
    char* e;
    errno = 0;
    double v = strtod(s, &e);
    if (errno) errno_from_host();
    if (endp) wr<u32>(endp, g(e));
    return v;
}

s32 x_lrand48() { return s32(lrand48()); }
void x_srand48(s32 seed) { srand48(seed); }
u32 x_sleep(u32 s) { return sleep(s); }
int x_system(const char* cmd) {
    NB_LOGW("guest system(\"%s\") refused", cmd ? cmd : "");
    set_guest_errno(ENOSYS);
    return -1;
}

std::mutex g_env_mu;
std::unordered_map<std::string, gaddr> g_env;
u32 x_getenv(const char* name) {
    const char* v = getenv(name);
    if (!v) return 0;
    std::lock_guard lk(g_env_mu);
    auto& slot = g_env[std::string(name) + "=" + v];
    if (!slot) slot = gstrdup(v);
    return slot;
}

// In-place introsort-free quicksort over guest memory; the comparator gets guest pointers.
struct SortCtx {
    u8* base;
    u32 size;
    gaddr cmp;
};
int sort_cmp(SortCtx& s, u32 i, u32 j) {
    return s32(u32(call_guest(s.cmp, {ga(g(s.base + size_t(i) * s.size)), ga(g(s.base + size_t(j) * s.size))})));
}
void sort_swap(SortCtx& s, u32 i, u32 j) {
    if (i == j) return;
    u8* a = s.base + size_t(i) * s.size;
    u8* b = s.base + size_t(j) * s.size;
    for (u32 k = 0; k < s.size; ++k) std::swap(a[k], b[k]);
}
void sort_range(SortCtx& s, u32 lo, u32 hi) {  // [lo, hi)
    while (hi - lo > 12) {
        u32 mid = lo + (hi - lo) / 2;
        if (sort_cmp(s, mid, lo) < 0) sort_swap(s, mid, lo);
        if (sort_cmp(s, hi - 1, lo) < 0) sort_swap(s, hi - 1, lo);
        if (sort_cmp(s, hi - 1, mid) < 0) sort_swap(s, hi - 1, mid);
        sort_swap(s, mid, hi - 2);  // pivot at hi-2
        u32 p = hi - 2, i = lo, j = hi - 2;
        for (;;) {
            while (sort_cmp(s, ++i, p) < 0) {}
            while (j > lo && sort_cmp(s, --j, p) > 0) {}
            if (i >= j) break;
            sort_swap(s, i, j);
        }
        sort_swap(s, i, hi - 2);
        if (i - lo < hi - i) {
            sort_range(s, lo, i);
            lo = i + 1;
        } else {
            sort_range(s, i + 1, hi);
            hi = i;
        }
    }
    for (u32 i = lo + 1; i < hi; ++i)
        for (u32 j = i; j > lo && sort_cmp(s, j, j - 1) < 0; --j) sort_swap(s, j, j - 1);
}
void x_qsort(void* base, u32 n, u32 size, u32 cmp) {
    if (n < 2 || !size) return;
    SortCtx s{static_cast<u8*>(base), size, cmp};
    sort_range(s, 0, n);
}

// ------------------------------------------------------------------ wide chars / locale

u32 x_btowc(int ch) { return u32(btowc(ch)); }
int x_wctob(u32 wc) { return wctob(wint_t(wc)); }
u32 x_mbrtowc(wchar_t* pwc, const char* s, u32 n, u32 gps) {
    mbstate_t st{};
    if (gps) memcpy(&st, h(gps), 4);
    size_t r = mbrtowc(pwc, s, n, gps ? &st : nullptr);
    if (gps) memcpy(h(gps), &st, 4);
    if (r == size_t(-1)) errno_from_host();
    return u32(r);
}
u32 x_wcrtomb(char* s, int wc, u32 gps) {
    mbstate_t st{};
    if (gps) memcpy(&st, h(gps), 4);
    size_t r = wcrtomb(s, wchar_t(wc), gps ? &st : nullptr);
    if (gps) memcpy(h(gps), &st, 4);
    if (r == size_t(-1)) errno_from_host();
    return u32(r);
}
u32 x_wcslen(const wchar_t* s) { return u32(wcslen(s)); }
wchar_t* x_wmemchr(const wchar_t* s, int c, u32 n) { return const_cast<wchar_t*>(wmemchr(s, wchar_t(c), n)); }
int x_wmemcmp(const wchar_t* a, const wchar_t* b, u32 n) { return wmemcmp(a, b, n); }
wchar_t* x_wmemcpy(wchar_t* d, const wchar_t* s, u32 n) { return wmemcpy(d, s, n); }
wchar_t* x_wmemmove(wchar_t* d, const wchar_t* s, u32 n) { return wmemmove(d, s, n); }
wchar_t* x_wmemset(wchar_t* d, int c, u32 n) { return wmemset(d, wchar_t(c), n); }
int x_wcscoll(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b); }
u32 x_wcsxfrm(wchar_t* d, const wchar_t* s, u32 n) {
    size_t l = wcslen(s);
    if (n) {
        size_t k = l < n - 1 ? l : n - 1;
        wmemcpy(d, s, k);
        d[k] = 0;
    }
    return u32(l);
}
int x_iswalnum(u32 c) { return iswalnum(wint_t(c)); }
int x_iswspace(u32 c) { return iswspace(wint_t(c)); }
u32 x_towlower(u32 c) { return towlower(wint_t(c)); }
u32 x_towupper(u32 c) { return towupper(wint_t(c)); }

std::mutex g_wct_mu;
std::vector<wctype_t> g_wctypes;
u32 x_wctype(const char* name) {
    wctype_t t = wctype(name);
    if (!t) return 0;
    std::lock_guard lk(g_wct_mu);
    for (size_t i = 0; i < g_wctypes.size(); ++i)
        if (g_wctypes[i] == t) return u32(i + 1);
    g_wctypes.push_back(t);
    return u32(g_wctypes.size());
}
int x_iswctype(u32 c, u32 t) {
    wctype_t ht;
    {
        std::lock_guard lk(g_wct_mu);
        if (!t || t > g_wctypes.size()) return 0;
        ht = g_wctypes[t - 1];
    }
    return iswctype(wint_t(c), ht);
}

gaddr g_c_locale_name = 0;
u32 x_setlocale(int, const char* loc) {
    if (loc && *loc && strcmp(loc, "C") != 0 && strcmp(loc, "POSIX") != 0) return 0;
    return g_c_locale_name;
}

// ------------------------------------------------------------------ time

// guest struct tm: 9 x int, long tm_gmtoff, const char* tm_zone  (44 bytes)
std::mutex g_zone_mu;
std::unordered_map<std::string, gaddr> g_zones;
gaddr zone_str(const char* z) {
    if (!z) return 0;
    std::lock_guard lk(g_zone_mu);
    auto& s = g_zones[z];
    if (!s) s = gstrdup(z);
    return s;
}
void tm_to_guest(gaddr a, const struct tm& t) {
    const int f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; ++i) wr<s32>(a + i * 4, f[i]);
    wr<s32>(a + 36, s32(t.tm_gmtoff));
    wr<u32>(a + 40, zone_str(t.tm_zone));
}
struct tm tm_from_guest(gaddr a) {
    struct tm t {};
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; ++i) *f[i] = rd<s32>(a + i * 4);
    t.tm_gmtoff = rd<s32>(a + 36);
    t.tm_zone = hs(rd<u32>(a + 40));
    return t;
}

}  // namespace
const char* headless_path(const char* p) {
    if (!p) return p;
    if (is_proc_self_exe(p)) {
        const std::string& apk = get_guest_apk_path();
        if (!apk.empty() && access(apk.c_str(), R_OK) == 0) {
            return apk.c_str();
        }
    }
    // Redirect legacy temporary directories (/data/local/tmp and /tmp) to container cache
    if (strcmp(p, "/data/local/tmp") == 0 || strcmp(p, "/data/local/tmp/") == 0 ||
        strcmp(p, "/tmp") == 0 || strcmp(p, "/tmp/") == 0) {
        return get_vemod_tmp_dir().c_str();
    }
    if (strncmp(p, "/data/local/tmp/", 16) == 0) {
        static thread_local std::string s_tmp_resolved;
        s_tmp_resolved = get_vemod_tmp_dir() + "/" + (p + 16);
        return s_tmp_resolved.c_str();
    }
    if (strncmp(p, "/tmp/", 5) == 0) {
        static thread_local std::string s_tmp_resolved;
        s_tmp_resolved = get_vemod_tmp_dir() + "/" + (p + 5);
        return s_tmp_resolved.c_str();
    }
    if (strcmp(p, "/proc/cpuinfo") == 0) {
        static std::string cpuinfo_path = [] {
            std::vector<std::string> dirs;
            std::string sdir = get_sysroot_dir();
            if (!sdir.empty()) {
                mkdir(sdir.c_str(), 0755);
                dirs.push_back(sdir);
                auto slash = sdir.rfind('/');
                if (slash != std::string::npos) {
                    dirs.push_back(sdir.substr(0, slash));
                }
            }
            std::string gdir = get_guest_lib_dir();
            if (!gdir.empty()) dirs.push_back(gdir);
            dirs.push_back("/data/local/tmp");
            dirs.push_back("/tmp");

            for (const auto& dir : dirs) {
                std::string path = dir + "/vemod_proc_cpuinfo";
                FILE* f = fopen(path.c_str(), "w");
                if (f) {
                    for (int i = 0; i < 8; ++i) {
                        fprintf(f,
                            "processor\t: %d\n"
                            "model name\t: ARMv7 Processor rev 0 (v7l)\n"
                            "BogoMIPS\t: 38.40\n"
                            "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32\n"
                            "CPU implementer\t: 0x41\n"
                            "CPU architecture: 7\n"
                            "CPU variant\t: 0x0\n"
                            "CPU part\t: 0xc0f\n"
                            "CPU revision\t: 2\n\n", i);
                    }
                    fprintf(f,
                        "Hardware\t: Qualcomm Technologies, Inc\n"
                        "Revision\t: 0000\n"
                        "Serial\t\t: 0000000000000000\n");
                    fclose(f);
                    NB_LOGI("Created vemod_proc_cpuinfo at %s", path.c_str());
                    return path;
                }
            }
            NB_LOGE("Failed to create vemod_proc_cpuinfo in any candidate directory!");
            return std::string("/proc/cpuinfo");
        }();
        return cpuinfo_path.c_str();
    }
    // Virtualize /system/lib/ and /vendor/lib/ using sysroot
    if (strncmp(p, "/system/lib/", 12) == 0 || strncmp(p, "/vendor/lib/", 12) == 0) {
        const std::string& sdir = get_sysroot_dir();
        if (!sdir.empty()) {
            static thread_local std::string s_sysroot_resolved;
            const char* fname = p + 12;
            s_sysroot_resolved = sdir + "/" + fname;
            if (access(s_sysroot_resolved.c_str(), R_OK) == 0) {
                return s_sysroot_resolved.c_str();
            }
        }
    }
    if (strcmp(p, "lib/libandroid.so") == 0 || strcmp(p, "libandroid.so") == 0) {
        const std::string& sdir = get_sysroot_dir();
        if (!sdir.empty()) {
            static thread_local std::string s_android_resolved;
            s_android_resolved = sdir + "/libandroid.so";
            if (access(s_android_resolved.c_str(), R_OK) == 0) {
                return s_android_resolved.c_str();
            }
        }
    }
#ifdef NB_HEADLESS
    if (strcmp(p, "/proc/self/stat") == 0) {
        static std::string fixed = [] {
            std::string f = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/nb_headless_stat";
            FILE* out = fopen(f.c_str(), "w");
            fputs("4242 (ninheadless) R 1 4242 4242 0 -1 4194304 1000 0 0 0 100 10 0 0 20 0 1 0 100 "
                  "268435456 16384 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n", out);
            fclose(out);
            return f;
        }();
        return fixed.c_str();
    }
#endif
    return p;
}
namespace {
#ifdef NB_HEADLESS
// Deterministic clock driven by the headless test harness.
extern "C" int64_t nb_fake_time_us;
void host_gettimeofday(struct timeval* t) {
    t->tv_sec = nb_fake_time_us / 1000000;
    t->tv_usec = nb_fake_time_us % 1000000;
}
time_t host_time() { return time_t(nb_fake_time_us / 1000000); }
clock_t host_clock() { return clock_t(nb_fake_time_us); }
#else
void host_gettimeofday(struct timeval* t) { gettimeofday(t, nullptr); }
time_t host_time() { return time(nullptr); }
clock_t host_clock() { return clock(); }
#endif

s32 x_time(u32 tp) {
    s32 t = s32(host_time());
    if (tp) wr<s32>(tp, t);
    return t;
}
int x_gettimeofday(u32 tv, u32 tz) {
    struct timeval t;
    host_gettimeofday(&t);
    if (tv) {
        wr<s32>(tv, s32(t.tv_sec));
        wr<s32>(tv + 4, s32(t.tv_usec));
    }
    if (tz) {
        wr<s32>(tz, 0);
        wr<s32>(tz + 4, 0);
    }
    return 0;
}
s32 x_clock() { return s32(host_clock()); }
s32 x_times(u32 buf) {
    struct tms t;
    clock_t r = times(&t);
#ifdef NB_HEADLESS
    t = {};
    r = clock_t(nb_fake_time_us / 10000);
#endif
    if (buf) {
        wr<s32>(buf, s32(t.tms_utime));
        wr<s32>(buf + 4, s32(t.tms_stime));
        wr<s32>(buf + 8, s32(t.tms_cutime));
        wr<s32>(buf + 12, s32(t.tms_cstime));
    }
    return s32(r);
}
int x_nanosleep(u32 req, u32 rem) {
    struct timespec a{rd<s32>(req), rd<s32>(req + 4)}, b{};
    int r = nanosleep(&a, &b);
    if (r) errno_from_host();
    if (rem) {
        wr<s32>(rem, s32(b.tv_sec));
        wr<s32>(rem + 4, s32(b.tv_nsec));
    }
    return r;
}
int x_clock_gettime(int clk_id, u32 tp) {
    struct timespec ts;
    int r = clock_gettime(clk_id, &ts);
    if (r == 0 && tp) {
        wr<s32>(tp, s32(ts.tv_sec));
        wr<s32>(tp + 4, s32(ts.tv_nsec));
    }
    return r;
}
int x_usleep(u32 usec) {
    return usleep(usec);
}
int x_sched_yield() {
    return sched_yield();
}
int x_strncasecmp(const char* s1, const char* s2, u32 n) {
    if (!s1 || !s2) return 0;
    return strncasecmp(s1, s2, n);
}
u32 x_localtime(u32 tp) {
    time_t t = rd<s32>(tp);
    struct tm r;
    if (!localtime_r(&t, &r)) return 0;
    gaddr out = cur_thread().tls + tlsoff::kTm;
    tm_to_guest(out, r);
    return out;
}
u32 x_gmtime(u32 tp) {
    time_t t = rd<s32>(tp);
    struct tm r;
    if (!gmtime_r(&t, &r)) return 0;
    gaddr out = cur_thread().tls + tlsoff::kTm;
    tm_to_guest(out, r);
    return out;
}
s32 x_mktime(u32 gtm) {
    struct tm t = tm_from_guest(gtm);
    time_t r = mktime(&t);
    tm_to_guest(gtm, t);
    return s32(r);
}
u32 x_strftime(char* s, u32 max, const char* fmt, u32 gtm) {
    struct tm t = tm_from_guest(gtm);
    return u32(strftime(s, max, fmt, &t));
}
u32 x_wcsftime(wchar_t* s, u32 max, const wchar_t* fmt, u32 gtm) {
    struct tm t = tm_from_guest(gtm);
    return u32(wcsftime(s, max, fmt, &t));
}
char* x_strptime(const char* buf, const char* fmt, u32 gtm) {
    struct tm t = tm_from_guest(gtm);
    char* r = strptime(buf, fmt, &t);
    tm_to_guest(gtm, t);
    return r;
}

}  // namespace

// ------------------------------------------------------------------ files

// ARM32 and arm64 share O_* values; x86_64 hosts differ for these four.
constexpr int kArmODirectory = 0x4000, kArmONofollow = 0x8000, kArmODirect = 0x10000, kArmOLargefile = 0x20000;
int oflags_from_guest(int f) {
    int r = f & ~(kArmODirectory | kArmONofollow | kArmODirect | kArmOLargefile);
    if (f & kArmODirectory) r |= O_DIRECTORY;
    if (f & kArmONofollow) r |= O_NOFOLLOW;
#ifdef O_DIRECT
    if (f & kArmODirect) r |= O_DIRECT;
#endif
    return r;
}
int oflags_to_guest(int f) {
    int r = f & ~(O_DIRECTORY | O_NOFOLLOW);
#ifdef O_DIRECT
    r &= ~O_DIRECT;
    if (f & O_DIRECT) r |= kArmODirect;
#endif
#if defined(O_LARGEFILE) && O_LARGEFILE != 0
    r &= ~O_LARGEFILE;
#endif
    if (f & O_DIRECTORY) r |= kArmODirectory;
    if (f & O_NOFOLLOW) r |= kArmONofollow;
    return r;
}

namespace {
void x_open(Ctx& c) {
    const char* path = headless_path(hs(c.a32()));
    int flags = int(c.a32());
    int mode = int(c.a32());
    if (path && strcmp(path, "/dev/alarm") == 0) {
        NB_LOGI("open('/dev/alarm') -> virtual kAlarmFd");
        c.ret(kAlarmFd);
        return;
    }
    int fd = open(path, oflags_from_guest(flags) | O_CLOEXEC, mode);
    NB_LOGI("open('%s', flags=%#x) -> fd=%d (errno=%d)", path ? path : "null", flags, fd, fd < 0 ? errno : 0);
    c.ret(u32(sysret(fd)));
}
}  // namespace

int x_close(int fd) {
    if (fd == kAlarmFd) return 0;
    if (vfd_is(fd)) return vfd_close(fd);
    return sysret(close(fd));
}
s32 x_read(int fd, void* buf, u32 n) {
    if (vfd_is(fd)) return vfd_read(fd, buf, n);
    return s32(sysret(read(fd, buf, n)));
}
s32 x_write(int fd, const void* buf, u32 n) { return s32(sysret(write(fd, buf, n))); }
s32 x_lseek(int fd, s32 off, int whence) {
    if (vfd_is(fd)) return s32(vfd_lseek(fd, off, whence));
    off_t r = lseek(fd, off, whence);
    if (r == -1) {
        errno_from_host();
        return -1;
    }
    if (r > INT32_MAX) {
        set_guest_errno(EOVERFLOW);
        return -1;
    }
    return s32(r);
}

namespace {
int x_fsync(int fd) { return sysret(fsync(fd)); }
int x_fdatasync(int fd) { return sysret(fdatasync(fd)); }
int x_ftruncate(int fd, s32 len) { return sysret(ftruncate(fd, len)); }
int x_unlink(const char* p) { return sysret(unlink(headless_path(p))); }
int x_remove(const char* p) { return sysret(remove(headless_path(p))); }
int x_rename(const char* a, const char* b) { return sysret(rename(headless_path(a), headless_path(b))); }
int x_mkdir(const char* p, u32 mode) { return sysret(mkdir(headless_path(p), mode)); }
int x_rmdir(const char* p) { return sysret(rmdir(headless_path(p))); }
int x_access(const char* p, int mode) { return sysret(access(headless_path(p), mode)); }
int x_chdir(const char* p) { return sysret(chdir(headless_path(p))); }
int x_fchmod(int fd, u32 mode) { return sysret(fchmod(fd, mode)); }
int x_fchown(int fd, u32 uid, u32 gid) { return sysret(fchown(fd, uid, gid)); }
int x_symlink(const char* a, const char* b) { return sysret(symlink(a, b)); }
s32 x_readlink(const char* p, char* buf, u32 n) {
    if (is_proc_self_exe(p)) {
        const std::string& apk = get_guest_apk_path();
        if (!apk.empty()) {
            size_t copy_len = std::min((size_t)n, apk.size());
            memcpy(buf, apk.data(), copy_len);
            return s32(copy_len);
        }
    }
    return s32(sysret(readlink(p, buf, n)));
}
int x_isatty(int fd) { return isatty(fd); }
int x_dup2(int a, int b) { return sysret(dup2(a, b)); }
int x_pipe(int* fds) { return sysret(pipe(fds)); }
int x_flock(int fd, int op) { return sysret(flock(fd, op)); }

u32 x_getcwd(u32 buf, u32 size) {
    char tmp[4096];
    if (!getcwd(tmp, sizeof tmp)) {
        errno_from_host();
        return 0;
    }
    u32 need = u32(strlen(tmp) + 1);
    if (!buf) {
        buf = gmalloc(size > need ? size : need);
    } else if (size < need) {
        set_guest_errno(ERANGE);
        return 0;
    }
    memcpy(h(buf), tmp, need);
    return buf;
}

int x_utimes(const char* p, u32 tv) {
    if (!tv) return sysret(utimes(headless_path(p), nullptr));
    struct timeval t[2] = {{rd<s32>(tv), rd<s32>(tv + 4)}, {rd<s32>(tv + 8), rd<s32>(tv + 12)}};
    return sysret(utimes(headless_path(p), t));
}

}  // namespace

// bionic arm32 struct stat (104 bytes)
void stat_to_guest(gaddr a, const struct stat& s) {
    memset(h(a), 0, 104);
    wr<u64>(a + 0, s.st_dev);
    wr<u32>(a + 12, u32(s.st_ino));
    wr<u32>(a + 16, s.st_mode);
    wr<u32>(a + 20, u32(s.st_nlink));
    wr<u32>(a + 24, s.st_uid);
    wr<u32>(a + 28, s.st_gid);
    wr<u64>(a + 32, s.st_rdev);
    wr<s64>(a + 48, s.st_size);
    wr<u32>(a + 56, u32(s.st_blksize));
    wr<u64>(a + 64, u64(s.st_blocks));
    wr<s32>(a + 72, s32(s.st_atim.tv_sec));
    wr<s32>(a + 76, s32(s.st_atim.tv_nsec));
    wr<s32>(a + 80, s32(s.st_mtim.tv_sec));
    wr<s32>(a + 84, s32(s.st_mtim.tv_nsec));
    wr<s32>(a + 88, s32(s.st_ctim.tv_sec));
    wr<s32>(a + 92, s32(s.st_ctim.tv_nsec));
    wr<u64>(a + 96, s.st_ino);
}

namespace {
int x_stat(const char* p, u32 out) {
    struct stat s;
    if (stat(headless_path(p), &s)) return sysret(-1);
    stat_to_guest(out, s);
    return 0;
}
int x_lstat(const char* p, u32 out) {
    struct stat s;
    if (lstat(headless_path(p), &s)) return sysret(-1);
    stat_to_guest(out, s);
    return 0;
}
int x_fstat(int fd, u32 out) {
    struct stat s;
    if (vfd_is(fd)) {
        s = {};
        s.st_mode = S_IFREG | 0444;
        s.st_size = vfd_size(fd);
        s.st_blksize = 4096;
        stat_to_guest(out, s);
        return 0;
    }
    if (fstat(fd, &s)) return sysret(-1);
    stat_to_guest(out, s);
    return 0;
}
int x_fstatat(int dirfd, const char* p, u32 out, int flags) {
    struct stat s;
    if (fstatat(dirfd, p, &s, flags)) return sysret(-1);
    stat_to_guest(out, s);
    return 0;
}

// DIR* handles: the guest sees a heap block that doubles as its dirent buffer.
constexpr u32 kDirentSize = 280;
std::mutex g_dir_mu;
std::unordered_map<gaddr, DIR*> g_dirs;
u32 x_opendir(const char* p) {
    DIR* d = opendir(p);
    if (!d) {
        errno_from_host();
        return 0;
    }
    gaddr blk = gcalloc(1, kDirentSize);
    std::lock_guard lk(g_dir_mu);
    g_dirs[blk] = d;
    return blk;
}
u32 x_readdir(u32 dir) {
    DIR* d;
    {
        std::lock_guard lk(g_dir_mu);
        auto it = g_dirs.find(dir);
        if (it == g_dirs.end()) {
            set_guest_errno(EBADF);
            return 0;
        }
        d = it->second;
    }
    errno = 0;
    struct dirent* e = readdir(d);
    if (!e) {
        if (errno) errno_from_host();
        return 0;
    }
    wr<u64>(dir, e->d_ino);
    wr<s64>(dir + 8, s64(e->d_off));
    wr<u16>(dir + 16, kDirentSize);
    wr<u8>(dir + 18, e->d_type);
    snprintf(hp<char>(dir + 19), 256, "%s", e->d_name);
    return dir;
}
int x_closedir(u32 dir) {
    DIR* d;
    {
        std::lock_guard lk(g_dir_mu);
        auto it = g_dirs.find(dir);
        if (it == g_dirs.end()) {
            set_guest_errno(EBADF);
            return -1;
        }
        d = it->second;
        g_dirs.erase(it);
    }
    gfree(dir);
    return sysret(closedir(d));
}

s32 x_readdir_r(u32 dir, gaddr entry, gaddr result) {
    if (!dir || !entry || !result) return EINVAL;
    DIR* d;
    {
        std::lock_guard lk(g_dir_mu);
        auto it = g_dirs.find(dir);
        if (it == g_dirs.end()) return EBADF;
        d = it->second;
    }
    errno = 0;
    struct dirent* e = readdir(d);
    if (!e) {
        if (errno) errno_from_host();
        wr<u32>(result, 0);
        return 0;
    }
    wr<u64>(entry, e->d_ino);
    wr<s64>(entry + 8, s64(e->d_off));
    wr<u16>(entry + 16, kDirentSize);
    wr<u8>(entry + 18, e->d_type);
    snprintf(hp<char>(entry + 19), 256, "%s", e->d_name);
    wr<u32>(result, entry);
    return 0;
}

void x_fcntl(Ctx& c) {
    int fd = int(c.a32());
    int cmd = int(c.a32());
    u32 arg = c.a32();
    int r;
    if (vfd_is(fd)) {
        c.ret(cmd == F_GETFL ? u32(O_RDONLY) : 0);
        return;
    }
    switch (cmd) {
    case F_GETFL:
        r = fcntl(fd, F_GETFL);
        if (r != -1) r = oflags_to_guest(r);
        break;
    case F_SETFL: r = fcntl(fd, F_SETFL, oflags_from_guest(int(arg))); break;
    case 5:  // F_GETLK  (struct flock: short, short, long, long, pid_t)
    case 6:
    case 7: {
        struct flock fl {};
        fl.l_type = rd<s16>(arg);
        fl.l_whence = rd<s16>(arg + 2);
        fl.l_start = rd<s32>(arg + 4);
        fl.l_len = rd<s32>(arg + 8);
        fl.l_pid = rd<s32>(arg + 12);
        r = fcntl(fd, cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW, &fl);
        if (r != -1 && cmd == 5) {
            wr<s16>(arg, s16(fl.l_type));
            wr<s16>(arg + 2, s16(fl.l_whence));
            wr<s32>(arg + 4, s32(fl.l_start));
            wr<s32>(arg + 8, s32(fl.l_len));
            wr<s32>(arg + 12, fl.l_pid);
        }
        break;
    }
    case 12:  // F_GETLK64 (struct flock64: short, short, pad, s64, s64, pid_t)
    case 13:
    case 14: {
        struct flock fl {};
        fl.l_type = rd<s16>(arg);
        fl.l_whence = rd<s16>(arg + 2);
        fl.l_start = rd<s64>(arg + 8);
        fl.l_len = rd<s64>(arg + 16);
        fl.l_pid = rd<s32>(arg + 24);
        r = fcntl(fd, cmd == 12 ? F_GETLK : cmd == 13 ? F_SETLK : F_SETLKW, &fl);
        if (r != -1 && cmd == 12) {
            wr<s16>(arg, s16(fl.l_type));
            wr<s16>(arg + 2, s16(fl.l_whence));
            wr<s64>(arg + 8, fl.l_start);
            wr<s64>(arg + 16, fl.l_len);
            wr<s32>(arg + 24, fl.l_pid);
        }
        break;
    }
    case F_DUPFD:
    case F_GETFD:
    case F_SETFD:
    case F_SETOWN:
    case F_GETOWN:
    case F_DUPFD_CLOEXEC: r = fcntl(fd, cmd, int(arg)); break;
    default:
        NB_LOGW("fcntl cmd %d unsupported", cmd);
        set_guest_errno(EINVAL);
        c.ret(u32(-1));
        return;
    }
    c.ret(u32(sysret(r)));
}

void x_ioctl(Ctx& c) {
    int fd = int(c.a32());
    u32 req = c.a32();
    u32 arg = c.a32();
    if (vfd_is(fd)) {
        if (req == 0x541B) {
            s64 pos = vfd_lseek(fd, 0, SEEK_CUR);
            wr<s32>(arg, s32(vfd_size(fd)) - s32(pos));
            c.ret(0);
        } else {
            set_guest_errno(ENOTTY);
            c.ret(u32(-1));
        }
        return;
    }
    if (fd == kAlarmFd || ((req >> 8) & 0xFF) == 'a') {
        u8 nr = req & 0xFF;
        u8 cmd = nr & 0x0F;
        u8 type = nr >> 4;
        if (cmd == 4) {  // ANDROID_ALARM_GET_TIME
            clockid_t cid = CLOCK_BOOTTIME;
            if (type == 0 || type == 1) cid = CLOCK_REALTIME;
            else if (type == 4) cid = CLOCK_MONOTONIC;
            struct timespec ts;
            clock_gettime(cid, &ts);
            if (arg) {
                wr<s32>(arg, s32(ts.tv_sec));
                wr<s32>(arg + 4, s32(ts.tv_nsec));
            }
            c.ret(0);
            return;
        }
        // SET / WAIT / etc.
        c.ret(0);
        return;
    }
    char fd_path[128] = "";
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(proc_path, fd_path, sizeof(fd_path) - 1);
    if (len > 0) fd_path[len] = '\0';
    NB_LOGI("ioctl: fd=%d (%s), req=%#x, arg=%#x", fd, fd_path, req, arg);
    switch (req) {
    case 0x541B:  // FIONREAD
    case 0x5421:  // FIONBIO
    case 0x5413:  // TIOCGWINSZ:
        c.ret(u32(sysret(ioctl(fd, req, h(arg)))));
        return;
    default:
        // Try passing through to host ioctl if valid pointer or value
        int r = ioctl(fd, req, arg ? h(arg) : nullptr);
        if (r == 0) {
            NB_LOGI("ioctl %#x succeeded via host passthrough", req);
            c.ret(0);
            return;
        }
        NB_LOGW("ioctl %#x failed (host errno=%d)", req, errno);
        c.ret(u32(sysret(r)));
    }
}

// ------------------------------------------------------------------ mmap

constexpr u32 kMapFailed = 0xffffffffu;
void x_mmap(Ctx& c) {
    gaddr addr = c.a32();
    u32 len = c.a32();
    int prot = int(c.a32());
    int flags = int(c.a32());
    int fd = int(c.a32());
    u32 off = c.a32();
    if (!len) {
        set_guest_errno(EINVAL);
        c.ret(kMapFailed);
        return;
    }
    u32 size = host_page_round_up(len);
    gaddr a = 0;
    if (flags & MAP_FIXED) {
        if (addr < layout::kMapBase || u64(addr) + size > layout::kMapEnd) {
            NB_LOGW("mmap MAP_FIXED at %#x outside map area", addr);
            set_guest_errno(EINVAL);
            c.ret(kMapFailed);
            return;
        }
        map_claim(addr, size);
        a = addr;
    } else {
        if (addr && (addr & (kPageSize - 1)) == 0 && addr >= layout::kMapBase && u64(addr) + size <= layout::kMapEnd && map_claim(addr, size)) {
            a = addr;
        } else {
            a = map_alloc(size, PROT_NONE);
            if (!a) {
                set_guest_errno(ENOMEM);
                c.ret(kMapFailed);
                return;
            }
        }
    }
    int hflags = (flags & (MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS)) | MAP_FIXED;
    void* p = mmap(g_base + a, size, prot, hflags, (flags & MAP_ANONYMOUS) ? -1 : fd, off);
    if (p == MAP_FAILED) {
        errno_from_host();
        map_free(a, size);
        c.ret(kMapFailed);
        return;
    }
    mem_protect(a, size, prot == PROT_NONE ? PROT_NONE : prot | PROT_READ);
    c.ret(a);
}
}  // namespace

int x_munmap(u32 a, u32 len) {
    if (a & (kPageSize - 1)) {
        set_guest_errno(EINVAL);
        return -1;
    }
    map_free(a, host_page_round_up(len));
    return 0;
}

namespace {
void x_mremap(Ctx& c) {
    gaddr old = c.a32();
    u32 olen = host_page_round_up(c.a32());
    u32 nlen = host_page_round_up(c.a32());
    int flags = int(c.a32());
    if (nlen <= olen) {
        if (nlen < olen) map_free(old + nlen, olen - nlen);
        c.ret(old);
        return;
    }
    if (map_claim(old + olen, nlen - olen)) {  // grow in place
        if (mremap(g_base + old, olen, nlen, 0) != MAP_FAILED) {
            mem_protect(old, nlen, PROT_READ | PROT_WRITE);
            c.ret(old);
            return;
        }
        map_free(old + olen, nlen - olen);
    }
    if (!(flags & MREMAP_MAYMOVE)) {
        set_guest_errno(ENOMEM);
        c.ret(kMapFailed);
        return;
    }
    gaddr na = map_alloc(nlen, PROT_NONE);
    if (!na || mremap(g_base + old, olen, nlen, MREMAP_MAYMOVE | MREMAP_FIXED, g_base + na) == MAP_FAILED) {
        errno_from_host();
        if (na) map_free(na, nlen);
        c.ret(kMapFailed);
        return;
    }
    mem_protect(na, nlen, PROT_READ | PROT_WRITE);
    map_free(old, olen);  // re-reserves the hole left behind by mremap
    c.ret(na);
}
int x_msync(void* a, u32 len, int flags) { return sysret(msync(a, len, flags)); }

// ------------------------------------------------------------------ system info

s32 x_sysconf(int name) {
    long r;
    switch (name) {  // bionic numbering
    case 0x0000: r = sysconf(_SC_ARG_MAX); break;
    case 0x0005: r = sysconf(_SC_CHILD_MAX); break;
    case 0x0006: r = sysconf(_SC_CLK_TCK); break;
    case 0x000b: r = sysconf(_SC_OPEN_MAX); break;
    case 0x0027:
    case 0x0028: r = 4096; break;
    case 0x0060: r = sysconf(_SC_NPROCESSORS_CONF); break;
    case 0x0061: r = sysconf(_SC_NPROCESSORS_ONLN); break;
    case 0x0062: r = sysconf(_SC_PHYS_PAGES); break;
    case 0x0063: r = sysconf(_SC_AVPHYS_PAGES); break;
    default:
        NB_LOGW("sysconf(%#x) unsupported", name);
        set_guest_errno(EINVAL);
        return -1;
    }
    return r > INT32_MAX ? INT32_MAX : s32(r);
}
int x_getrlimit(int res, u32 out) {
    struct rlimit rl;
    if (getrlimit(res, &rl)) return sysret(-1);
    wr<u32>(out, rl.rlim_cur > UINT32_MAX ? UINT32_MAX : u32(rl.rlim_cur));
    wr<u32>(out + 4, rl.rlim_max > UINT32_MAX ? UINT32_MAX : u32(rl.rlim_max));
    return 0;
}
int x_getrusage(int who, u32 out) {
    struct rusage ru;
    if (getrusage(who, &ru)) return sysret(-1);
#ifdef NB_HEADLESS
    ru = {};
    ru.ru_utime.tv_sec = nb_fake_time_us / 1000000;
    ru.ru_utime.tv_usec = nb_fake_time_us % 1000000;
#endif
    wr<s32>(out, s32(ru.ru_utime.tv_sec));
    wr<s32>(out + 4, s32(ru.ru_utime.tv_usec));
    wr<s32>(out + 8, s32(ru.ru_stime.tv_sec));
    wr<s32>(out + 12, s32(ru.ru_stime.tv_usec));
    const long f[14] = {ru.ru_maxrss, ru.ru_ixrss,  ru.ru_idrss,  ru.ru_isrss,    ru.ru_minflt,   ru.ru_majflt, ru.ru_nswap,
                        ru.ru_inblock, ru.ru_oublock, ru.ru_msgsnd, ru.ru_msgrcv, ru.ru_nsignals, ru.ru_nvcsw, ru.ru_nivcsw};
    for (int i = 0; i < 14; ++i) wr<s32>(out + 16 + i * 4, s32(f[i]));
    return 0;
}
int x_sysinfo(u32 out) {
    struct sysinfo si;
    if (sysinfo(&si)) return sysret(-1);
    u64 unit = si.mem_unit ? si.mem_unit : 1;
    while (u64(si.totalram) * si.mem_unit / unit > UINT32_MAX || u64(si.totalswap) * si.mem_unit / unit > UINT32_MAX)
        unit *= 2;
    auto sc = [&](unsigned long v) { return u32(u64(v) * si.mem_unit / unit); };
    memset(h(out), 0, 64);
    wr<s32>(out, s32(si.uptime));
    for (int i = 0; i < 3; ++i) wr<u32>(out + 4 + i * 4, u32(si.loads[i]));
    wr<u32>(out + 16, sc(si.totalram));
    wr<u32>(out + 20, sc(si.freeram));
    wr<u32>(out + 24, sc(si.sharedram));
    wr<u32>(out + 28, sc(si.bufferram));
    wr<u32>(out + 32, sc(si.totalswap));
    wr<u32>(out + 36, sc(si.freeswap));
    wr<u16>(out + 40, si.procs);
    wr<u32>(out + 52, u32(unit));
    return 0;
}
int x_uname(struct utsname* u) {
    struct utsname hu;
    if (uname(&hu)) return sysret(-1);
    memset(u, 0, 6 * 65);
    char* f = reinterpret_cast<char*>(u);
    snprintf(f + 0 * 65, 65, "%s", hu.sysname);
    snprintf(f + 1 * 65, 65, "%s", hu.nodename);
    snprintf(f + 2 * 65, 65, "%s", hu.release);
    snprintf(f + 3 * 65, 65, "%s", hu.version);
    snprintf(f + 4 * 65, 65, "armv7l");
    return 0;
}

int x_getpid() { return getpid(); }
u32 x_geteuid() { return geteuid(); }
int x_kill(int pid, int sig) {
    if (pid == getpid() && sig != 0) {
        NB_LOGE("guest kill(self, %d)", sig);
        if (sig == SIGKILL || sig == SIGABRT || sig == SIGTERM || sig == SIGSEGV) fatal("guest killed its own process");
    }
    return sysret(kill(pid, sig));
}
int x_waitpid(int pid, int* status, int opts) { return sysret(waitpid(pid, status, opts)); }
int x_fork() {
    NB_LOGW("guest fork() refused");
    set_guest_errno(ENOSYS);
    return -1;
}
u32 x_popen(const char* cmd, const char*) {
    NB_LOGW("guest popen(\"%s\") refused", cmd ? cmd : "");
    set_guest_errno(ENOSYS);
    return 0;
}
int x_pclose(u32) {
    set_guest_errno(ECHILD);
    return -1;
}
int x_execvp(u32, u32) {
    set_guest_errno(ENOSYS);
    return -1;
}

// ------------------------------------------------------------------ select / poll / writev

int x_select(int n, fd_set* r, fd_set* w, fd_set* e, u32 tv) {
    struct timeval t, *tp = nullptr;
    if (tv) {
        t = {rd<s32>(tv), rd<s32>(tv + 4)};
        tp = &t;
    }
    int res = select(n, r, w, e, tp);
    if (res < 0) errno_from_host();
    if (tv) {
        wr<s32>(tv, s32(t.tv_sec));
        wr<s32>(tv + 4, s32(t.tv_usec));
    }
    return res;
}
int x_poll(struct pollfd* fds, u32 n, int timeout) { return sysret(poll(fds, n, timeout)); }
s32 x_writev(int fd, u32 iov, int cnt) {
    std::vector<struct iovec> v(cnt > 0 ? cnt : 0);
    for (int i = 0; i < cnt; ++i) v[i] = {h(rd<u32>(iov + i * 8)), rd<u32>(iov + i * 8 + 4)};
    return s32(sysret(writev(fd, v.data(), cnt)));
}

// ------------------------------------------------------------------ sockets

int x_socket(int d, int t, int p) { return sysret(socket(d, t, p)); }
int x_bind(int fd, const sockaddr* a, u32 l) { return sysret(bind(fd, a, l)); }
int x_connect(int fd, const sockaddr* a, u32 l) { return sysret(connect(fd, a, l)); }
int x_listen(int fd, int b) { return sysret(listen(fd, b)); }
int x_accept(int fd, sockaddr* a, socklen_t* l) { return sysret(accept(fd, a, l)); }
int x_getpeername(int fd, sockaddr* a, socklen_t* l) { return sysret(getpeername(fd, a, l)); }
s32 x_send(int fd, const void* b, u32 n, int f) { return s32(sysret(send(fd, b, n, f))); }
s32 x_recv(int fd, void* b, u32 n, int f) { return s32(sysret(recv(fd, b, n, f))); }
void x_sendto(Ctx& c) {
    int fd = int(c.a32());
    const void* b = h(c.a32());
    u32 n = c.a32();
    int f = int(c.a32());
    const sockaddr* a = hp<sockaddr>(c.a32());
    u32 l = c.a32();
    c.ret(u32(sysret(sendto(fd, b, n, f, a, l))));
}
s32 x_recvmsg(int fd, u32 gm, int flags) {
    struct msghdr m {};
    m.msg_name = h(rd<u32>(gm));
    m.msg_namelen = rd<u32>(gm + 4);
    u32 iov = rd<u32>(gm + 8);
    u32 cnt = rd<u32>(gm + 12);
    std::vector<struct iovec> v(cnt);
    for (u32 i = 0; i < cnt; ++i) v[i] = {h(rd<u32>(iov + i * 8)), rd<u32>(iov + i * 8 + 4)};
    m.msg_iov = v.data();
    m.msg_iovlen = cnt;
    if (rd<u32>(gm + 20)) NB_LOGW("recvmsg: control messages not supported");
    ssize_t r = recvmsg(fd, &m, flags);
    if (r < 0) {
        errno_from_host();
        return -1;
    }
    wr<u32>(gm + 4, m.msg_namelen);
    wr<u32>(gm + 20, 0);
    wr<s32>(gm + 24, m.msg_flags);
    return s32(r);
}
int x_setsockopt(int fd, int level, int opt, const void* val, u32 len) {
    if (level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) && len == 8) {
        const s32* t = static_cast<const s32*>(val);
        struct timeval tv{t[0], t[1]};
        return sysret(setsockopt(fd, level, opt, &tv, sizeof tv));
    }
    return sysret(setsockopt(fd, level, opt, val, len));
}
int x_gethostname(char* n, u32 l) { return sysret(gethostname(n, l)); }
u32 x_inet_addr(const char* s) { return inet_addr(s); }
void x_inet_ntoa(Ctx& c) {
    struct in_addr a;
    a.s_addr = c.a32();
    gaddr buf = cur_thread().tls + tlsoff::kNtoa;
    snprintf(hp<char>(buf), 32, "%s", inet_ntoa(a));
    c.ret(buf);
}
u32 x_hstrerror(int e) {
    gaddr buf = cur_thread().tls + tlsoff::kMisc;
    snprintf(hp<char>(buf), 256, "%s", hstrerror(e));
    return buf;
}
u32 x_get_h_errno() { return cur_thread().tls + tlsoff::kH_errno; }

u32 x_gethostbyname(const char* name) {
    struct hostent* he = gethostbyname(name);
    gaddr tls = cur_thread().tls;
    wr<s32>(tls + tlsoff::kH_errno, h_errno);
    if (!he) return 0;
    // Pack into the per-thread buffer: hostent | alias ptrs | addr ptrs | data
    gaddr base = tls + tlsoff::kHostent, end = base + 1536;
    gaddr p = base + 20;
    auto put = [&](const void* src, u32 n) -> gaddr {
        if (p + n > end) return 0;
        gaddr a = p;
        memcpy(h(a), src, n);
        p = (p + n + 3) & ~3u;
        return a;
    };
    int na = 0, nd = 0;
    while (he->h_aliases && he->h_aliases[na] && na < 8) ++na;
    while (he->h_addr_list[nd] && nd < 16) ++nd;
    gaddr aliases = p;
    p += (na + 1) * 4;
    gaddr addrs = p;
    p += (nd + 1) * 4;
    for (int i = 0; i < na; ++i) wr<u32>(aliases + i * 4, put(he->h_aliases[i], u32(strlen(he->h_aliases[i]) + 1)));
    wr<u32>(aliases + na * 4, 0);
    for (int i = 0; i < nd; ++i) wr<u32>(addrs + i * 4, put(he->h_addr_list[i], u32(he->h_length)));
    wr<u32>(addrs + nd * 4, 0);
    wr<u32>(base, put(he->h_name, u32(strlen(he->h_name) + 1)));
    wr<u32>(base + 4, aliases);
    wr<s32>(base + 8, he->h_addrtype);
    wr<s32>(base + 12, he->h_length);
    wr<u32>(base + 16, addrs);
    return base;
}

s32 x_getaddrinfo(u32 g_node, u32 g_serv, u32 g_hints, u32 g_res) {
    if (!g_res) return EAI_NONAME;
    const char* node_str = g_node ? reinterpret_cast<const char*>(h(g_node)) : nullptr;
    const char* serv_str = g_serv ? reinterpret_cast<const char*>(h(g_serv)) : nullptr;

    struct addrinfo host_hints;
    memset(&host_hints, 0, sizeof(host_hints));
    if (g_hints) {
        host_hints.ai_flags = rd<s32>(g_hints + 0);
        host_hints.ai_family = rd<s32>(g_hints + 4);
        host_hints.ai_socktype = rd<s32>(g_hints + 8);
        host_hints.ai_protocol = rd<s32>(g_hints + 12);
    }

    struct addrinfo* host_res = nullptr;
    int rc = getaddrinfo(node_str, serv_str, g_hints ? &host_hints : nullptr, &host_res);
    if (rc != 0) {
        wr<u32>(g_res, 0);
        NB_LOGW("getaddrinfo('%s', '%s') returned %d (%s)", node_str ? node_str : "null", serv_str ? serv_str : "null", rc, gai_strerror(rc));
        return rc;
    }

    gaddr head = 0;
    gaddr last_node = 0;

    for (struct addrinfo* curr = host_res; curr != nullptr; curr = curr->ai_next) {
        gaddr g_ai = gmalloc(32);
        if (!g_ai) break;
        memset(h(g_ai), 0, 32);

        wr<s32>(g_ai + 0, curr->ai_flags);
        wr<s32>(g_ai + 4, curr->ai_family);
        wr<s32>(g_ai + 8, curr->ai_socktype);
        wr<s32>(g_ai + 12, curr->ai_protocol);
        wr<u32>(g_ai + 16, curr->ai_addrlen);

        if (curr->ai_canonname) {
            wr<u32>(g_ai + 20, gstrdup(curr->ai_canonname));
        } else {
            wr<u32>(g_ai + 20, 0);
        }

        if (curr->ai_addr && curr->ai_addrlen > 0) {
            gaddr g_sa = gmalloc(curr->ai_addrlen);
            if (g_sa) {
                memcpy(h(g_sa), curr->ai_addr, curr->ai_addrlen);
                wr<u32>(g_ai + 24, g_sa);
            } else {
                wr<u32>(g_ai + 24, 0);
            }
        } else {
            wr<u32>(g_ai + 24, 0);
        }

        wr<u32>(g_ai + 28, 0);

        if (head == 0) {
            head = g_ai;
        } else {
            wr<u32>(last_node + 28, g_ai);
        }
        last_node = g_ai;
    }

    freeaddrinfo(host_res);
    wr<u32>(g_res, head);
    NB_LOGI("getaddrinfo('%s', '%s') succeeded -> guest addrinfo %#x", node_str ? node_str : "null", serv_str ? serv_str : "null", head);
    return 0;
}

void x_freeaddrinfo(u32 g_res) {
    gaddr curr = g_res;
    while (curr) {
        gaddr next = rd<u32>(curr + 28);
        gaddr canon = rd<u32>(curr + 20);
        gaddr addr = rd<u32>(curr + 24);
        if (canon) gfree(canon);
        if (addr) gfree(addr);
        gfree(curr);
        curr = next;
    }
}

s32 x_android_getaddrinfofornet(u32 g_node, u32 g_serv, u32 g_hints, u32 /*netid*/, u32 /*mark*/, u32 g_res) {
    return x_getaddrinfo(g_node, g_serv, g_hints, g_res);
}

s32 x_android_getaddrinfofornetcontext(u32 g_node, u32 g_serv, u32 g_hints, u32 /*netcontext*/, u32 g_res) {
    return x_getaddrinfo(g_node, g_serv, g_hints, g_res);
}

// ------------------------------------------------------------------ libdl

u32 x_dlopen(const char* name, int flags) {
    return guest_dlopen(name, flags);
}

u32 x_dlsym(u32 handle, const char* name) {
    return guest_dlsym(handle, name);
}

u32 x_android_dlopen_ext(const char* name, int flags, u32 /*extinfo*/) {
    return guest_dlopen(name, flags);
}

u32 x_dlvsym(u32 handle, const char* name, const char* /*version*/) {
    return guest_dlsym(handle, name);
}

int x_dlclose(u32 handle) {
    return guest_dlclose(handle);
}

u32 x_dlerror() { return 0; }

// ------------------------------------------------------------------ libm

#define MATH1(n) NB_REG(#n, static_cast<double (*)(double)>(&::n))
#define MATH1F(n) NB_REG(#n, static_cast<float (*)(float)>(&::n))
double x_frexp(double v, int* e) { return frexp(v, e); }
double x_ldexp(double v, int e) { return ldexp(v, e); }
double x_pow(double a, double b) { return pow(a, b); }
float x_powf(float a, float b) { return powf(a, b); }
double x_fmod(double a, double b) { return fmod(a, b); }
float x_fmodf(float a, float b) { return fmodf(a, b); }
float x_atan2f(float a, float b) { return atan2f(a, b); }
double x_atan2(double a, double b) { return atan2(a, b); }
double x_modf(double x, double* iptr) { return modf(x, iptr); }
float x_modff(float x, float* iptr) { return modff(x, iptr); }

u32 x_gmtime_r(gaddr timep, gaddr result) {
    if (!timep || !result) return 0;
    time_t t = rd<s32>(timep);
    struct tm r;
    if (!gmtime_r(&t, &r)) return 0;
    tm_to_guest(result, r);
    return result;
}
u32 x_localtime_r(gaddr timep, gaddr result) {
    if (!timep || !result) return 0;
    time_t t = rd<s32>(timep);
    struct tm r;
    if (!localtime_r(&t, &r)) return 0;
    tm_to_guest(result, r);
    return result;
}

int x_strerror_r(int errnum, char* buf, size_t buflen) {
    char* r = strerror_r(errnum, buf, buflen);
    if (r && r != buf && buflen > 0) {
        strncpy(buf, r, buflen);
        buf[buflen - 1] = '\0';
    }
    return 0;
}

u32 x_wcschr(const wchar_t* ws, wchar_t wc) {
    const wchar_t* res = wcschr(ws, wc);
    return res ? g(res) : 0;
}
int x_wcscmp(const wchar_t* s1, const wchar_t* s2) { return wcscmp(s1, s2); }
int x_wcsncmp(const wchar_t* s1, const wchar_t* s2, size_t n) { return wcsncmp(s1, s2, n); }

// Math helpers
double x_difftime(u32 t1, u32 t0) { return difftime((time_t)t1, (time_t)t0); }
s32 x_lrint(double x) { return (s32)lrint(x); }
s64 x_llrint(double x) { return (s64)llrint(x); }
double x_remquo(double x, double y, gaddr quo_addr) {
    int quo = 0;
    double r = remquo(x, y, &quo);
    if (quo_addr) wr<s32>(quo_addr, quo);
    return r;
}
s32 x_fpclassifyd(double x) { return std::fpclassify(x); }
s32 x_isfinite(double x) { return std::isfinite(x) ? 1 : 0; }
s32 x_signbit(double x) { return std::signbit(x) ? 1 : 0; }

// Memory & String helpers
gaddr x_bsearch(gaddr key, gaddr base, u32 nmemb, u32 size, gaddr compar) {
    if (!base || !size || !nmemb || !compar) return 0;
    u32 l = 0, r = nmemb;
    while (l < r) {
        u32 m = l + (r - l) / 2;
        gaddr item = base + m * size;
        s32 res = (s32)call_guest(compar, {ga(key), ga(item)});
        if (res == 0) return item;
        if (res < 0) r = m;
        else l = m + 1;
    }
    return 0;
}
gaddr x_basename(gaddr path) {
    if (!path) return 0;
    const char* p = hs(path);
    const char* slash = strrchr(p, '/');
    return slash ? (path + u32(slash - p + 1)) : path;
}
gaddr x_memmem(gaddr haystack, u32 hlen, gaddr needle, u32 nlen) {
    if (!haystack || !needle) return 0;
    if (nlen == 0) return haystack;
    if (hlen < nlen) return 0;
    const void* res = ::memmem(h(haystack), hlen, h(needle), nlen);
    return res ? g(res) : 0;
}
gaddr x_memrchr(gaddr s, int c, u32 n) {
    if (!s || !n) return 0;
    const void* res = ::memrchr(h(s), c, n);
    return res ? g(res) : 0;
}
u32 x_strcspn(gaddr s, gaddr reject) {
    if (!s || !reject) return 0;
    return (u32)::strcspn(hs(s), hs(reject));
}
gaddr x_strpbrk(gaddr s, gaddr accept) {
    if (!s || !accept) return 0;
    const char* res = ::strpbrk(hs(s), hs(accept));
    return res ? g(res) : 0;
}
gaddr x_strncat(gaddr dest, gaddr src, u32 n) {
    if (!dest || !src) return dest;
    ::strncat(hp<char>(dest), hs(src), n);
    return dest;
}
gaddr x_strtok_r(gaddr str, gaddr delim, gaddr saveptr) {
    if (!delim || !saveptr) return 0;
    char* s = str ? hp<char>(str) : nullptr;
    gaddr sp_g = rd<u32>(saveptr);
    char* sp = sp_g ? hp<char>(sp_g) : nullptr;
    char* res = ::strtok_r(s, hs(delim), &sp);
    wr<u32>(saveptr, sp ? g(sp) : 0);
    return res ? g(res) : 0;
}
u64 x_strtoull(gaddr nptr, gaddr endptr, int base) {
    if (!nptr) return 0;
    char* end = nullptr;
    unsigned long long val = ::strtoull(hs(nptr), &end, base);
    if (endptr) wr<u32>(endptr, end ? g(end) : 0);
    return val;
}

// System, File & Network helpers
s32 x_dup(int oldfd) { return sysret(::dup(oldfd)); }
s32 x_chmod(gaddr path, u32 mode) {
    if (!path) return -1;
    return sysret(::chmod(hs(path), (mode_t)mode));
}
s32 x_utime(gaddr path, gaddr times) {
    if (!path) return -1;
    struct utimbuf host_ut;
    struct utimbuf* p_ut = nullptr;
    if (times) {
        p_ut = &host_ut;
        host_ut.actime = static_cast<time_t>(rd<s32>(times));
        host_ut.modtime = static_cast<time_t>(rd<s32>(times + 4));
    }
    return sysret(::utime(headless_path(hs(path)), p_ut));
}
s32 x_utimensat(int dirfd, gaddr path, gaddr times, int flags) {
    const char* p = path ? hs(path) : nullptr;
    struct timespec host_ts[2];
    struct timespec* p_ts = nullptr;
    if (times) {
        p_ts = host_ts;
        host_ts[0].tv_sec = static_cast<time_t>(rd<s32>(times));
        host_ts[0].tv_nsec = static_cast<long>(rd<s32>(times + 4));
        host_ts[1].tv_sec = static_cast<time_t>(rd<s32>(times + 8));
        host_ts[1].tv_nsec = static_cast<long>(rd<s32>(times + 12));
    }
    return sysret(::utimensat(dirfd, p ? headless_path(p) : nullptr, p_ts, flags));
}
s32 x_fnmatch(gaddr pattern, gaddr string, int flags) {
    if (!pattern || !string) return -1;
    return ::fnmatch(hs(pattern), hs(string), flags);
}
s32 x_pathconf(gaddr, int) { return 4096; }
s32 x_statfs(gaddr, gaddr buf) {
    if (buf) memset(h(buf), 0, 64);
    return 0;
}
gaddr x_getpwuid(u32 uid) {
    static gaddr s_pw = 0;
    if (!s_pw) {
        s_pw = misc_alloc(64);
        gaddr name = gstrdup("app");
        gaddr dir = gstrdup("/data");
        wr<u32>(s_pw + 0, name);
        wr<u32>(s_pw + 4, name);
        wr<u32>(s_pw + 8, uid);
        wr<u32>(s_pw + 12, uid);
        wr<u32>(s_pw + 16, dir);
        wr<u32>(s_pw + 20, dir);
    }
    return s_pw;
}
u32 x_getauxval(u32 type) {
    if (type == 6) return 4096; // AT_PAGESZ
    if (type == 16) {
        // AT_HWCAP: VFP, EDSP, NEON, VFPv3, VFPv3D16, TLS, VFPv4, IDIVA, IDIVT, VFPD32
        return (1 << 6) | (1 << 7) | (1 << 12) | (1 << 13) | (1 << 14) |
               (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19);
    }
    if (type == 26) return 0; // AT_HWCAP2: ARMv7 has no ARMv8 crypto extensions (prevents illegal sha1/aes opcodes)
    return 0;
}
u32 x_android_getCpuFamily() {
    return 1; // ANDROID_CPU_FAMILY_ARM
}
u64 x_android_getCpuFeatures() {
    return 0x000003ffull; // ARMv7, VFPv3, NEON, VFP_D32, VFP_FP16, etc.
}
s32 x_android_getCpuCount() {
    return 8;
}
u32 x_gettid() {
    return cur_thread().tid;
}
s32 x_dladdr(gaddr, gaddr info) {
    if (info) memset(h(info), 0, 16);
    return 0;
}
s32 x_system_property_get(gaddr name, gaddr value) {
    if (!name || !value) return 0;
#if defined(__ANDROID__)
    return ::__system_property_get(hs(name), hp<char>(value));
#else
    return 0;
#endif
}
s32 x_getsockname(int fd, gaddr addr, gaddr addrlen) {
    if (!addr || !addrlen) return -1;
    socklen_t len = rd<u32>(addrlen);
    int r = ::getsockname(fd, hp<struct sockaddr>(addr), &len);
    wr<u32>(addrlen, len);
    return sysret(r);
}
s32 x_getsockopt(int fd, int level, int optname, gaddr optval, gaddr optlen) {
    if (!optval || !optlen) return -1;
    socklen_t len = rd<u32>(optlen);
    int r = ::getsockopt(fd, level, optname, h(optval), &len);
    wr<u32>(optlen, len);
    return sysret(r);
}
gaddr x_inet_ntop(int af, gaddr src, gaddr dst, u32 size) {
    if (!src || !dst) return 0;
    const char* r = ::inet_ntop(af, h(src), hp<char>(dst), size);
    return r ? dst : 0;
}
s32 x_inet_pton(int af, gaddr src, gaddr dst) {
    if (!src || !dst) return -1;
    return ::inet_pton(af, hs(src), h(dst));
}
s32 x_recvfrom(int fd, gaddr buf, u32 len, int flags, gaddr src_addr, gaddr addrlen) {
    if (!buf) return -1;
    socklen_t slen = addrlen ? rd<u32>(addrlen) : 0;
    ssize_t r = ::recvfrom(fd, h(buf), len, flags, src_addr ? hp<struct sockaddr>(src_addr) : nullptr, addrlen ? &slen : nullptr);
    if (addrlen) wr<u32>(addrlen, slen);
    return (s32)sysret(r);
}
s32 x_sigaction(int, gaddr, gaddr oldact) {
    // 32-bit ARM Bionic sizeof(struct sigaction) is 16 bytes:
    // sa_handler (4) + sa_mask (4) + sa_flags (4) + sa_restorer (4).
    // Writing 32 bytes overflows stack allocations and corrupts saved lr/r4.
    if (oldact) memset(h(oldact), 0, 16);
    return 0;
}
s32 x_sigprocmask(int, gaddr, gaddr oldset) {
    // 32-bit ARM Bionic sizeof(sigset_t) is 4 bytes (unsigned long).
    if (oldset) memset(h(oldset), 0, 4);
    return 0;
}
void x_mono_thread_suspend_all_other_threads() {
    NB_LOGI("mono_thread_suspend_all_other_threads called - bypassing signal suspension in JIT");
}
void x_mono_gc_collect(int gen) {
    NB_LOGI("mono_gc_collect(%d) called - bypassing GC stop-the-world in JIT", gen);
}

}  // namespace

void register_libc() {
    reg("__errno", x_errno);
    reg("__stack_chk_fail", x_stack_chk_fail);
    gaddr guard = misc_alloc(4);
    wr<u32>(guard, 0x9e3779b9u ^ u32(time(nullptr)));
    reg_data("__stack_chk_guard", guard);

    gaddr page_size_addr = misc_alloc(4);
    wr<u32>(page_size_addr, 4096);
    reg_data("__page_size", page_size_addr);

    gaddr siglist_addr = misc_alloc(32 * 4);
    gaddr unknown_sig = gstrdup("Unknown signal");
    for (int i = 0; i < 32; ++i) wr<u32>(siglist_addr + i * 4, unknown_sig);
    reg_data("sys_siglist", siglist_addr);

    reg("__assert2", x_assert2);
    reg("abort", x_abort);
    reg("exit", x_exit);
    reg("_exit", x_exit);
    reg("_Exit", x_exit);
    reg("raise", x_raise);
    reg("bsd_signal", x_bsd_signal);
    reg("__cxa_atexit", x_ret0);
    reg("__cxa_finalize", x_ret0);
    reg("__gnu_Unwind_Find_exidx", x_find_exidx);
    reg("dl_unwind_find_exidx", x_find_exidx);
    reg("android_get_application_target_sdk_version", [](Ctx& c) { c.ret(24); });
    reg("setjmp", x_setjmp);
    reg("longjmp", x_longjmp);
    reg("sigsetjmp", x_setjmp);
    reg("siglongjmp", x_longjmp);

    reg("__aeabi_memclr", x_aeabi_memclr);
    reg("__aeabi_memclr4", x_aeabi_memclr);
    reg("__aeabi_memclr8", x_aeabi_memclr);
    reg("__aeabi_memcpy", x_aeabi_memcpy);
    reg("__aeabi_memcpy4", x_aeabi_memcpy);
    reg("__aeabi_memcpy8", x_aeabi_memcpy);
    reg("__aeabi_memmove", x_aeabi_memmove);
    reg("__aeabi_memmove4", x_aeabi_memmove);
    reg("__aeabi_memmove8", x_aeabi_memmove);
    reg("__aeabi_memset", x_aeabi_memset);
    reg("__aeabi_memset4", x_aeabi_memset);
    reg("__aeabi_memset8", x_aeabi_memset);

    reg("__aeabi_l2d", x_aeabi_l2d);
    reg("__aeabi_ul2d", x_aeabi_ul2d);
    reg("__aeabi_i2d", x_aeabi_i2d);
    reg("__aeabi_ui2d", x_aeabi_ui2d);
    reg("__aeabi_l2f", x_aeabi_l2f);
    reg("__aeabi_ul2f", x_aeabi_ul2f);
    reg("__aeabi_d2lz", x_aeabi_d2lz);
    reg("__aeabi_d2ulz", x_aeabi_d2ulz);
    reg("__aeabi_d2uiz", x_aeabi_d2uiz);
    reg("__aeabi_d2iz", x_aeabi_d2iz);
    reg("__aeabi_f2d", x_aeabi_f2d);
    reg("__aeabi_d2f", x_aeabi_d2f);
    reg("__aeabi_f2lz", x_aeabi_f2lz);
    reg("__aeabi_f2ulz", x_aeabi_f2ulz);
    reg("__aeabi_f2uiz", x_aeabi_f2uiz);
    reg("__aeabi_f2iz", x_aeabi_f2iz);

    reg("__aeabi_dadd", x_aeabi_dadd);
    reg("__aeabi_dsub", x_aeabi_dsub);
    reg("__aeabi_drsub", x_aeabi_drsub);
    reg("__aeabi_dmul", x_aeabi_dmul);
    reg("__aeabi_ddiv", x_aeabi_ddiv);

    reg("__aeabi_fadd", x_aeabi_fadd);
    reg("__aeabi_fsub", x_aeabi_fsub);
    reg("__aeabi_frsub", x_aeabi_frsub);
    reg("__aeabi_fmul", x_aeabi_fmul);
    reg("__aeabi_fdiv", x_aeabi_fdiv);

    reg("__aeabi_dcmpeq", x_aeabi_dcmpeq);
    reg("__aeabi_dcmplt", x_aeabi_dcmplt);
    reg("__aeabi_dcmple", x_aeabi_dcmple);
    reg("__aeabi_dcmpge", x_aeabi_dcmpge);
    reg("__aeabi_dcmpgt", x_aeabi_dcmpgt);

    reg("__aeabi_fcmpeq", x_aeabi_fcmpeq);
    reg("__aeabi_fcmplt", x_aeabi_fcmplt);
    reg("__aeabi_fcmple", x_aeabi_fcmple);
    reg("__aeabi_fcmpge", x_aeabi_fcmpge);
    reg("__aeabi_fcmpgt", x_aeabi_fcmpgt);

    reg("__aeabi_cdcmpeq", x_aeabi_cdcmpeq);
    reg("__aeabi_cdcmple", x_aeabi_cdcmple);
    reg("__aeabi_cdrcmple", x_aeabi_cdrcmple);

    reg("__aeabi_idiv", x_aeabi_idiv);
    reg("__aeabi_idivmod", x_aeabi_idivmod);
    reg("__aeabi_uidiv", x_aeabi_uidiv);
    reg("__aeabi_uidivmod", x_aeabi_uidivmod);
    reg("__aeabi_ldivmod", x_aeabi_ldivmod);
    reg("__aeabi_uldivmod", x_aeabi_uldivmod);
    reg("__aeabi_idiv0", x_aeabi_idiv0);
    reg("__aeabi_ldiv0", x_aeabi_ldiv0);
    reg("__aeabi_unwind_cpp_pr0", x_aeabi_unwind_cpp_pr);
    reg("__aeabi_unwind_cpp_pr1", x_aeabi_unwind_cpp_pr);
    reg("__aeabi_unwind_cpp_pr2", x_aeabi_unwind_cpp_pr);
    NB_REG("memcpy", x_memcpy);
    NB_REG("memmove", x_memmove);
    NB_REG("memset", x_memset);
    NB_REG("memcmp", x_memcmp);
    NB_REG("memchr", x_memchr);
    NB_REG("strlen", x_strlen);
    NB_REG("strcmp", x_strcmp);
    NB_REG("strncmp", x_strncmp);
    NB_REG("strcasecmp", x_strcasecmp);
    NB_REG("strcoll", x_strcoll);
    NB_REG("strxfrm", x_strxfrm);
    NB_REG("strcpy", x_strcpy);
    NB_REG("strncpy", x_strncpy);
    NB_REG("strcat", x_strcat);
    NB_REG("strchr", x_strchr);
    NB_REG("strrchr", x_strrchr);
    NB_REG("strstr", x_strstr);
    NB_REG("strdup", x_strdup);
    reg("strerror", x_strerror);

    NB_REG("isalnum", x_isalnum);
    NB_REG("isalpha", x_isalpha);
    NB_REG("isdigit", x_isdigit);
    NB_REG("isgraph", x_isgraph);
    NB_REG("islower", x_islower);
    NB_REG("isupper", x_isupper);
    NB_REG("isprint", x_isprint);
    NB_REG("isspace", x_isspace);
    NB_REG("tolower", x_tolower);
    NB_REG("toupper", x_toupper);
    make_ctype_tables();

#ifdef NB_HEADLESS
    reg("malloc", x_malloc);
    reg("calloc", x_calloc);
    reg("realloc", x_realloc);
    reg("free", x_free);
#else
    NB_REG("malloc", x_malloc);
    NB_REG("calloc", x_calloc);
    NB_REG("realloc", x_realloc);
    NB_REG("free", x_free);
#endif
    NB_REG("atoi", x_atoi);
    NB_REG("atol", x_atoi);
    NB_REG("strtol", x_strtol);
    NB_REG("strtoul", x_strtoul);
    NB_REG("strtoll", x_strtoll);
    NB_REG("strtod", x_strtod);
    NB_REG("lrand48", x_lrand48);
    NB_REG("srand48", x_srand48);
    NB_REG("sleep", x_sleep);
    NB_REG("system", x_system);
    NB_REG("getenv", x_getenv);
    NB_REG("qsort", x_qsort);

    NB_REG("btowc", x_btowc);
    NB_REG("wctob", x_wctob);
    NB_REG("mbrtowc", x_mbrtowc);
    NB_REG("wcrtomb", x_wcrtomb);
    NB_REG("wcslen", x_wcslen);
    NB_REG("wmemchr", x_wmemchr);
    NB_REG("wmemcmp", x_wmemcmp);
    NB_REG("wmemcpy", x_wmemcpy);
    NB_REG("wmemmove", x_wmemmove);
    NB_REG("wmemset", x_wmemset);
    NB_REG("wcscoll", x_wcscoll);
    NB_REG("wcsxfrm", x_wcsxfrm);
    NB_REG("iswalnum", x_iswalnum);
    NB_REG("iswspace", x_iswspace);
    NB_REG("towlower", x_towlower);
    NB_REG("towupper", x_towupper);
    NB_REG("wctype", x_wctype);
    NB_REG("iswctype", x_iswctype);
    g_c_locale_name = misc_alloc(2);
    wr<u16>(g_c_locale_name, 'C');
    NB_REG("setlocale", x_setlocale);

    NB_REG("time", x_time);
    NB_REG("gettimeofday", x_gettimeofday);
    NB_REG("clock_gettime", x_clock_gettime);
    NB_REG("clock", x_clock);
    NB_REG("times", x_times);
    NB_REG("nanosleep", x_nanosleep);
    NB_REG("usleep", x_usleep);
    NB_REG("sched_yield", x_sched_yield);
    NB_REG("strncasecmp", x_strncasecmp);
    NB_REG("localtime", x_localtime);
    NB_REG("gmtime", x_gmtime);
    NB_REG("mktime", x_mktime);
    NB_REG("strftime", x_strftime);
    NB_REG("wcsftime", x_wcsftime);
    NB_REG("strptime", x_strptime);

    reg("open", x_open);
    NB_REG("close", x_close);
    NB_REG("read", x_read);
    NB_REG("write", x_write);
    NB_REG("lseek", x_lseek);
    NB_REG("fsync", x_fsync);
    NB_REG("fdatasync", x_fdatasync);
    NB_REG("ftruncate", x_ftruncate);
    NB_REG("unlink", x_unlink);
    NB_REG("remove", x_remove);
    NB_REG("rename", x_rename);
    NB_REG("mkdir", x_mkdir);
    NB_REG("rmdir", x_rmdir);
    NB_REG("access", x_access);
    NB_REG("chdir", x_chdir);
    NB_REG("fchmod", x_fchmod);
    NB_REG("fchown", x_fchown);
    NB_REG("symlink", x_symlink);
    NB_REG("readlink", x_readlink);
    NB_REG("isatty", x_isatty);
    NB_REG("dup2", x_dup2);
    NB_REG("pipe", x_pipe);
    NB_REG("flock", x_flock);
    NB_REG("getcwd", x_getcwd);
    NB_REG("utimes", x_utimes);
    NB_REG("stat", x_stat);
    NB_REG("stat64", x_stat);
    NB_REG("lstat", x_lstat);
    NB_REG("lstat64", x_lstat);
    NB_REG("fstat", x_fstat);
    NB_REG("fstat64", x_fstat);
    NB_REG("fstatat", x_fstatat);
    NB_REG("fstatat64", x_fstatat);
    NB_REG("opendir", x_opendir);
    NB_REG("readdir", x_readdir);
    NB_REG("closedir", x_closedir);
    reg("fcntl", x_fcntl);
    reg("ioctl", x_ioctl);
    reg("mmap", x_mmap);
    reg("mmap64", x_mmap);
    reg("__mmap2", x_mmap);
    NB_REG("munmap", x_munmap);
    reg("mremap", x_mremap);
    NB_REG("msync", x_msync);

    NB_REG("sysconf", x_sysconf);
    NB_REG("getrlimit", x_getrlimit);
    NB_REG("getrusage", x_getrusage);
    NB_REG("sysinfo", x_sysinfo);
    NB_REG("uname", x_uname);
    NB_REG("getpid", x_getpid);
    NB_REG("geteuid", x_geteuid);
    NB_REG("kill", x_kill);
    NB_REG("waitpid", x_waitpid);
    NB_REG("fork", x_fork);
    NB_REG("execvp", x_execvp);
    NB_REG("popen", x_popen);
    NB_REG("pclose", x_pclose);

    NB_REG("select", x_select);
    NB_REG("poll", x_poll);
    NB_REG("writev", x_writev);
    NB_REG("socket", x_socket);
    NB_REG("bind", x_bind);
    NB_REG("connect", x_connect);
    NB_REG("listen", x_listen);
    NB_REG("accept", x_accept);
    NB_REG("getpeername", x_getpeername);
    NB_REG("send", x_send);
    NB_REG("recv", x_recv);
    reg("sendto", x_sendto);
    NB_REG("recvmsg", x_recvmsg);
    NB_REG("setsockopt", x_setsockopt);
    NB_REG("gethostname", x_gethostname);
    NB_REG("inet_addr", x_inet_addr);
    reg("inet_ntoa", x_inet_ntoa);
    NB_REG("hstrerror", x_hstrerror);
    NB_REG("__get_h_errno", x_get_h_errno);
    NB_REG("gethostbyname", x_gethostbyname);
    NB_REG("getaddrinfo", x_getaddrinfo);
    NB_REG("freeaddrinfo", x_freeaddrinfo);
    NB_REG("android_getaddrinfofornet", x_android_getaddrinfofornet);
    NB_REG("android_getaddrinfofornetcontext", x_android_getaddrinfofornetcontext);

    NB_REG("dlopen", x_dlopen);
    NB_REG("dlsym", x_dlsym);
    NB_REG("dlerror", x_dlerror);
    NB_REG("dlclose", x_dlclose);
    NB_REG("dlvsym", x_dlvsym);
    NB_REG("android_dlopen_ext", x_android_dlopen_ext);

    MATH1(acos);
    MATH1F(acosf);
    MATH1(acosh);
    MATH1(asin);
    MATH1(asinh);
    MATH1(atan);
    MATH1F(atanf);
    MATH1(atanh);
    MATH1(ceil);
    MATH1F(ceilf);
    MATH1(cos);
    MATH1F(cosf);
    MATH1(cosh);
    MATH1(exp);
    MATH1(exp2);
    MATH1(fabs);
    MATH1(floor);
    MATH1F(floorf);
    MATH1(log);
    MATH1(log10);
    MATH1F(logf);
    MATH1(sin);
    MATH1F(sinf);
    MATH1(sinh);
    MATH1(sqrt);
    MATH1F(sqrtf);
    MATH1(tan);
    MATH1F(tanf);
    MATH1(tanh);
    MATH1(rint);
    MATH1F(rintf);
    MATH1(round);
    MATH1F(roundf);
    MATH1(trunc);
    MATH1F(truncf);
    NB_REG("frexp", x_frexp);
    NB_REG("ldexp", x_ldexp);
    NB_REG("pow", x_pow);
    NB_REG("powf", x_powf);
    NB_REG("fmod", x_fmod);
    NB_REG("fmodf", x_fmodf);
    NB_REG("atan2", x_atan2);
    NB_REG("atan2f", x_atan2f);
    NB_REG("modf", x_modf);
    NB_REG("modff", x_modff);
    NB_REG("gmtime_r", x_gmtime_r);
    NB_REG("localtime_r", x_localtime_r);
    NB_REG("strerror_r", x_strerror_r);
    NB_REG("wcschr", x_wcschr);
    NB_REG("wcscmp", x_wcscmp);
    NB_REG("wcsncmp", x_wcsncmp);

    MATH1(cbrt);
    MATH1(erf);
    MATH1(erfc);
    MATH1(expm1);
    MATH1(lgamma);
    MATH1(log1p);
    MATH1(logb);
    MATH1(nearbyint);
    MATH1(tgamma);
    MATH1F(asinf);
    MATH1F(coshf);
    MATH1F(expf);
    MATH1F(log10f);
    MATH1F(sinhf);
    MATH1F(tanhf);
    NB_REG("hypot", static_cast<double (*)(double, double)>(&::hypot));
    NB_REG("remainder", static_cast<double (*)(double, double)>(&::remainder));
    NB_REG("nextafter", static_cast<double (*)(double, double)>(&::nextafter));
    NB_REG("nextafterf", static_cast<float (*)(float, float)>(&::nextafterf));
    NB_REG("ldexpf", static_cast<float (*)(float, int)>(&::ldexpf));
    NB_REG("scalbn", static_cast<double (*)(double, int)>(&::scalbn));
    NB_REG("scalbnl", static_cast<double (*)(double, int)>(&::scalbn));
    NB_REG("difftime", x_difftime);
    NB_REG("lrint", x_lrint);
    NB_REG("llrint", x_llrint);
    NB_REG("remquo", x_remquo);
    NB_REG("__fpclassifyd", x_fpclassifyd);
    NB_REG("__isfinite", x_isfinite);
    NB_REG("__signbit", x_signbit);

    NB_REG("bsearch", x_bsearch);
    NB_REG("basename", x_basename);
    NB_REG("memmem", x_memmem);
    NB_REG("memrchr", x_memrchr);
    NB_REG("strcspn", x_strcspn);
    NB_REG("strpbrk", x_strpbrk);
    NB_REG("strncat", x_strncat);
    NB_REG("strtok_r", x_strtok_r);
    NB_REG("strtoull", x_strtoull);

    NB_REG("dup", x_dup);
    NB_REG("chmod", x_chmod);
    NB_REG("utime", x_utime);
    NB_REG("utimensat", x_utimensat);
    NB_REG("fnmatch", x_fnmatch);
    NB_REG("pathconf", x_pathconf);
    NB_REG("statfs", x_statfs);
    NB_REG("readdir_r", x_readdir_r);
    NB_REG("getpwuid", x_getpwuid);
    NB_REG("getauxval", x_getauxval);
    NB_REG("android_getCpuFamily", x_android_getCpuFamily);
    NB_REG("android_getCpuFeatures", x_android_getCpuFeatures);
    NB_REG("android_getCpuCount", x_android_getCpuCount);
    NB_REG("gettid", x_gettid);
    NB_REG("dladdr", x_dladdr);
    NB_REG("__system_property_get", x_system_property_get);
    NB_REG("getsockname", x_getsockname);
    NB_REG("getsockopt", x_getsockopt);
    NB_REG("inet_ntop", x_inet_ntop);
    NB_REG("inet_pton", x_inet_pton);
    NB_REG("recvfrom", x_recvfrom);
    NB_REG("sigaction", x_sigaction);
    NB_REG("sigprocmask", x_sigprocmask);
    NB_REG("mono_thread_suspend_all_other_threads", x_mono_thread_suspend_all_other_threads);
    NB_REG("mono_gc_collect", x_mono_gc_collect);
}

}  // namespace nb
