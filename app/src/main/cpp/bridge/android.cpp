// liblog and libandroid (AAsset) imports. On non-Android hosts (desktop test harness) the log goes
// to stderr and assets are read from the directory named by $NIN_ASSETS.
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "jni_bridge.h"
#include "nb.h"

// Headless test builds use the desktop code paths even when targeting Android.
#if defined(__ANDROID__) && !defined(NB_HEADLESS)
#define NB_ANDROID_RUNTIME 1
#endif

#ifdef NB_ANDROID_RUNTIME
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <android/input.h>
#include <dlfcn.h>
#include <thread>
#else
#include <fstream>
#include <sstream>
#endif
#include <atomic>

namespace nb {

static std::atomic<int> s_log_level{LOG_LEVEL_BASIC};

int get_log_level() {
    return s_log_level.load(std::memory_order_relaxed);
}

void set_log_level(int level) {
    s_log_level.store(level, std::memory_order_relaxed);
}

void log_write(int prio, const char* tag, const char* msg) {
    int lvl = get_log_level();
    if (lvl == LOG_LEVEL_NONE) {
        if (prio < LOG_ERROR) return;
    } else if (lvl == LOG_LEVEL_BASIC) {
        if (prio < LOG_WARN) {
            // In basic level, allow high-level module loading/app start events, suppress high-frequency info
            if (tag && strcmp(tag, "ninbridge") != 0 && strcmp(tag, "VemodBridge") != 0 && strcmp(tag, "Unity") != 0) {
                return;
            }
        }
    }

#if defined(__ANDROID__) || defined(NB_ANDROID_RUNTIME)
    static std::string s_last_msg;
    static int s_repeat_count = 0;
    if (lvl < LOG_LEVEL_FULL) {
        if (msg && s_last_msg == msg) {
            if (++s_repeat_count > 3) return;
        } else {
            s_last_msg = msg ? msg : "";
            s_repeat_count = 0;
        }
    }
    __android_log_print(prio, "VemodBridge", "[%s] %s", tag ? tag : "bridge", msg ? msg : "");
#else
    static const char kLetters[] = "??VDIWEF";
    fprintf(stderr, "%c/%s: %s\n", prio >= 0 && prio < 8 ? kLetters[prio] : '?', tag ? tag : "", msg);
#endif
}

namespace {

void x_log_print(Ctx& c) {
    int prio = int(c.a32());
    const char* tag = hs(c.a32());
    const char* fmt = hs(c.a32());
    CtxArgs a(c);
    std::string s = gformat(fmt, a);
    log_write(prio, tag ? tag : "nin", s.c_str());
    c.ret(1);
}

int x_log_write(int prio, const char* tag, const char* text) {
    log_write(prio, tag ? tag : "nin", text ? text : "");
    return 1;
}

// ------------------------------------------------------------------ assets

struct Asset {
#ifdef NB_ANDROID_RUNTIME
    AAsset* a = nullptr;
#else
    std::string data;
#endif
    gaddr buf = 0;
};

std::mutex g_mu;
std::unordered_map<u32, void*> g_managers;  // guest handle -> AAssetManager*
std::unordered_map<u32, Asset*> g_assets;
u32 g_next_handle = 0x10;

u32 x_mgr_from_java(u32 /*env*/, u32 jam) {
#ifdef NB_ANDROID_RUNTIME
    // The JNIEnv the guest passed is our own; use the host env of this thread instead.
    AAssetManager* m = AAssetManager_fromJava(jni_env(), jni_obj(jam));
    if (!m) return 0;
#else
    (void)jam;
    void* m = reinterpret_cast<void*>(1);
#endif
    std::lock_guard lk(g_mu);
    for (auto& [hnd, ptr] : g_managers)
        if (ptr == m) return hnd;
    u32 hnd = g_next_handle++ << 4;
    g_managers[hnd] = m;
    return hnd;
}

u32 x_asset_open(u32 mgr, const char* name, int mode) {
    void* m;
    {
        std::lock_guard lk(g_mu);
        auto it = g_managers.find(mgr);
        if (it == g_managers.end()) return 0;
        m = it->second;
    }
    auto* as = new Asset;
#ifdef NB_ANDROID_RUNTIME
    as->a = AAssetManager_open(static_cast<AAssetManager*>(m), name, mode);
    if (!as->a) {
        delete as;
        return 0;
    }
#else
    (void)m;
    (void)mode;
    const char* root = getenv("NIN_ASSETS");
    std::ifstream f(std::string(root ? root : "assets") + "/" + name, std::ios::binary);
    if (!f) {
        delete as;
        return 0;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    as->data = ss.str();
#endif
    std::lock_guard lk(g_mu);
    u32 hnd = g_next_handle++ << 4;
    g_assets[hnd] = as;
    return hnd;
}

Asset* asset(u32 hnd) {
    std::lock_guard lk(g_mu);
    auto it = g_assets.find(hnd);
    if (it == g_assets.end()) fatal("guest used an invalid AAsset handle %#x", hnd);
    return it->second;
}

s32 x_asset_length(u32 hnd) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    return s32(AAsset_getLength(as->a));
#else
    return s32(as->data.size());
#endif
}

s64 x_asset_length64(u32 hnd) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    return s64(AAsset_getLength64(as->a));
#else
    return s64(as->data.size());
#endif
}

u32 x_asset_buffer(u32 hnd) {
    Asset* as = asset(hnd);
    if (as->buf) return as->buf;
#ifdef NB_ANDROID_RUNTIME
    const void* p = AAsset_getBuffer(as->a);
    if (!p) return 0;
    as->buf = gmemdup(p, u32(AAsset_getLength(as->a)));
#else
    as->buf = gmemdup(as->data.data(), u32(as->data.size()));
#endif
    return as->buf;
}

s32 x_asset_read(u32 hnd, void* buf, u32 count) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    return s32(AAsset_read(as->a, buf, count));
#else
    return 0;
#endif
}

s32 x_asset_seek(u32 hnd, s32 offset, int whence) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    return s32(AAsset_seek(as->a, offset, whence));
#else
    return -1;
#endif
}

s32 x_asset_remaining(u32 hnd) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    return s32(AAsset_getRemainingLength(as->a));
#else
    return 0;
#endif
}

int x_asset_open_fd(u32 hnd, s32* outStart, s32* outLength) {
    Asset* as = asset(hnd);
#ifdef NB_ANDROID_RUNTIME
    off_t s = 0, l = 0;
    int fd = AAsset_openFileDescriptor(as->a, &s, &l);
    if (outStart) *outStart = s32(s);
    if (outLength) *outLength = s32(l);
    return fd;
#else
    (void)outStart; (void)outLength;
    return -1;
#endif
}

void x_asset_close(u32 hnd) {
    Asset* as;
    {
        std::lock_guard lk(g_mu);
        auto it = g_assets.find(hnd);
        if (it == g_assets.end()) return;
        as = it->second;
        g_assets.erase(it);
    }
#ifdef NB_ANDROID_RUNTIME
    AAsset_close(as->a);
#endif
    gfree(as->buf);
    delete as;
}

}  // namespace

#ifdef NB_ANDROID_RUNTIME
static std::atomic<AInputQueue*> s_active_host_input_queue{nullptr};
static std::atomic<bool> s_input_pump_stop{false};
static std::thread s_input_pump_thread;

void stop_host_input_pump() {
    s_input_pump_stop.store(true);
    AInputQueue* q = s_active_host_input_queue.exchange(nullptr);
    if (s_input_pump_thread.joinable()) {
        s_input_pump_thread.join();
    }
    if (q) {
        AInputQueue_detachLooper(q);
    }
}

void start_host_input_pump(void* queue_ptr) {
    stop_host_input_pump();
    AInputQueue* queue = static_cast<AInputQueue*>(queue_ptr);
    if (!queue) return;
    s_active_host_input_queue.store(queue);
    s_input_pump_stop.store(false);

    s_input_pump_thread = std::thread([queue]() {
        pthread_setname_np(pthread_self(), "VemodInputPump");
        ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        if (queue && looper) {
            AInputQueue_attachLooper(queue, looper, 1, nullptr, nullptr);
            while (!s_input_pump_stop.load()) {
                int ident = ALooper_pollOnce(50, nullptr, nullptr, nullptr);
                if (ident == 1) {
                    AInputEvent* event = nullptr;
                    while (AInputQueue_getEvent(queue, &event) >= 0) {
                        if (AInputQueue_preDispatchEvent(queue, event)) {
                            continue;
                        }
                        AInputQueue_finishEvent(queue, event, 1);
                    }
                }
            }
            AInputQueue_detachLooper(queue);
        }
    });
}
#else
void start_host_input_pump(void*) {}
void stop_host_input_pump() {}
#endif

namespace {

// ------------------------------------------------------------------ ALooper
constexpr int kALooperPollTimeout = -3;

u32 x_ALooper_forThread() {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = ALooper_forThread();
    if (!l) l = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    return to_guest_egl(l);
#else
    return 0x1000;
#endif
}

u32 x_ALooper_prepare(int opts) {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = ALooper_prepare(opts);
    return to_guest_egl(l);
#else
    return 0x1000;
#endif
}

void x_ALooper_acquire(u32 looper) {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (l) ALooper_acquire(l);
#endif
}

void x_ALooper_release(u32 looper) {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (l) ALooper_release(l);
#endif
}

int x_ALooper_pollOnce(int timeoutMillis, u32 outFd, u32 outEvents, u32 outData) {
#ifdef NB_ANDROID_RUNTIME
    int fd = 0, events = 0;
    void* data = nullptr;
    int res = ALooper_pollOnce(timeoutMillis, outFd ? &fd : nullptr, outEvents ? &events : nullptr, outData ? &data : nullptr);
    if (outFd && is_mapped(outFd)) wr<s32>(outFd, fd);
    if (outEvents && is_mapped(outEvents)) wr<s32>(outEvents, events);
    if (outData && is_mapped(outData)) wr<u32>(outData, to_guest_egl(data));
    return res;
#else
    return kALooperPollTimeout;
#endif
}

int x_ALooper_pollAll(int timeoutMillis, u32 outFd, u32 outEvents, u32 outData) {
#ifdef NB_ANDROID_RUNTIME
    int fd = 0, events = 0;
    void* data = nullptr;
    typedef int (*PollAllFn)(int, int*, int*, void**);
    static auto pollAll = reinterpret_cast<PollAllFn>(dlsym(RTLD_DEFAULT, "ALooper_pollAll"));
    int res = pollAll ? pollAll(timeoutMillis, outFd ? &fd : nullptr, outEvents ? &events : nullptr, outData ? &data : nullptr)
                      : ALooper_pollOnce(timeoutMillis, outFd ? &fd : nullptr, outEvents ? &events : nullptr, outData ? &data : nullptr);
    if (outFd && is_mapped(outFd)) wr<s32>(outFd, fd);
    if (outEvents && is_mapped(outEvents)) wr<s32>(outEvents, events);
    if (outData && is_mapped(outData)) wr<u32>(outData, to_guest_egl(data));
    return res;
#else
    return kALooperPollTimeout;
#endif
}

void x_ALooper_wake(u32 looper) {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (l) ALooper_wake(l);
#endif
}

int x_ALooper_addFd(u32 looper, int fd, int ident, int events, u32 /*callback*/, u32 /*data*/) {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (l) return ALooper_addFd(l, fd, ident, events, nullptr, nullptr);
#endif
    return 1;
}

int x_ALooper_removeFd(u32 looper, int fd) {
#ifdef NB_ANDROID_RUNTIME
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (l) return ALooper_removeFd(l, fd);
#endif
    return 1;
}

// ------------------------------------------------------------------ AInputQueue
void x_AInputQueue_attachLooper(u32 queue, u32 looper, int ident, u32 /*callback*/, u32 /*data*/) {
#ifdef NB_ANDROID_RUNTIME
    AInputQueue* q = (AInputQueue*)to_host_egl(queue);
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (q && l) AInputQueue_attachLooper(q, l, ident, nullptr, nullptr);
#endif
}

void x_AInputQueue_detachLooper(u32 queue) {
#ifdef NB_ANDROID_RUNTIME
    AInputQueue* q = (AInputQueue*)to_host_egl(queue);
    if (q) AInputQueue_detachLooper(q);
#endif
}

int x_AInputQueue_hasEvents(u32 queue) {
#ifdef NB_ANDROID_RUNTIME
    AInputQueue* q = (AInputQueue*)to_host_egl(queue);
    if (q) return AInputQueue_hasEvents(q);
#endif
    return 0;
}

int x_AInputQueue_getEvent(u32 queue, u32 outEvent) {
#ifdef NB_ANDROID_RUNTIME
    AInputQueue* q = (AInputQueue*)to_host_egl(queue);
    if (q && outEvent) {
        AInputEvent* ev = nullptr;
        int res = AInputQueue_getEvent(q, &ev);
        if (res >= 0 && ev) {
            wr<u32>(outEvent, to_guest_egl(ev));
            return res;
        }
    }
#endif
    return -1;
}

int x_AInputQueue_preDispatchEvent(u32 queue, u32 event) {
#ifdef NB_ANDROID_RUNTIME
    AInputQueue* q = (AInputQueue*)to_host_egl(queue);
    AInputEvent* ev = (AInputEvent*)to_host_egl(event);
    if (q && ev) return AInputQueue_preDispatchEvent(q, ev);
#endif
    return 0;
}

void x_AInputQueue_finishEvent(u32 queue, u32 event, int handled) {
#ifdef NB_ANDROID_RUNTIME
    AInputQueue* q = (AInputQueue*)to_host_egl(queue);
    AInputEvent* ev = (AInputEvent*)to_host_egl(event);
    if (q && ev) AInputQueue_finishEvent(q, ev, handled);
#endif
}

// ------------------------------------------------------------------ AInputEvent & AMotionEvent
int x_AInputEvent_getType(u32 /*event*/) { return 0; }
int x_AInputEvent_getDeviceId(u32 /*event*/) { return 0; }
int x_AInputEvent_getSource(u32 /*event*/) { return 0; }

int x_AMotionEvent_getAction(u32 /*event*/) { return 0; }
int x_AMotionEvent_getFlags(u32 /*event*/) { return 0; }
int x_AMotionEvent_getMetaState(u32 /*event*/) { return 0; }
int x_AMotionEvent_getButtonState(u32 /*event*/) { return 0; }
int x_AMotionEvent_getEdgeFlags(u32 /*event*/) { return 0; }
s64 x_AMotionEvent_getDownTime(u32 /*event*/) { return 0; }
s64 x_AMotionEvent_getEventTime(u32 /*event*/) { return 0; }
u32 x_AMotionEvent_getXOffset(u32 /*event*/) { return 0; }
u32 x_AMotionEvent_getYOffset(u32 /*event*/) { return 0; }
u32 x_AMotionEvent_getXPrecision(u32 /*event*/) { return 0; }
u32 x_AMotionEvent_getYPrecision(u32 /*event*/) { return 0; }
u32 x_AMotionEvent_getPointerCount(u32 /*event*/) { return 1; }
int x_AMotionEvent_getPointerId(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getToolType(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getRawX(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getRawY(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getX(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getY(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getPressure(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getSize(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getTouchMajor(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getTouchMinor(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getToolMajor(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getToolMinor(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getOrientation(u32 /*event*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getAxisValue(u32 /*event*/, int /*axis*/, u32 /*idx*/) { return 0; }
u32 x_AMotionEvent_getHistorySize(u32 /*event*/) { return 0; }

// ------------------------------------------------------------------ AConfiguration
u32 x_AConfiguration_new() { return 0x3000; }
void x_AConfiguration_delete(u32 /*config*/) {}
void x_AConfiguration_fromAssetManager(u32 /*config*/, u32 /*am*/) {}
int x_AConfiguration_getOrientation(u32 /*config*/) { return 1; }

// ------------------------------------------------------------- AKeyEvent
int x_AKeyEvent_getAction(u32 /*event*/) { return 0; }
int x_AKeyEvent_getKeyCode(u32 /*event*/) { return 0; }
int x_AKeyEvent_getMetaState(u32 /*event*/) { return 0; }

// ------------------------------------------------------------- AMotionEvent historical
float x_AMotionEvent_getHistoricalX(u32 /*event*/, u32 /*idx*/, u32 /*history_idx*/) { return 0.0f; }
float x_AMotionEvent_getHistoricalY(u32 /*event*/, u32 /*idx*/, u32 /*history_idx*/) { return 0.0f; }
s64 x_AMotionEvent_getHistoricalEventTime(u32 /*event*/, u32 /*history_idx*/) { return 0; }

// ------------------------------------------------------------- ASensor
u32 x_ASensorManager_getInstance() {
#ifdef NB_ANDROID_RUNTIME
    ASensorManager* sm = ASensorManager_getInstanceForPackage("com.psychostark.vemod");
    NB_LOGI("ASensorManager_getInstance -> %p (handle %#x)", sm, to_guest_egl(sm));
    return to_guest_egl(sm);
#else
    return 0x3000;
#endif
}

u32 x_ASensorManager_getInstanceForPackage(u32 /*pkg*/) {
#ifdef NB_ANDROID_RUNTIME
    ASensorManager* sm = ASensorManager_getInstanceForPackage("com.psychostark.vemod");
    NB_LOGI("ASensorManager_getInstanceForPackage -> %p (handle %#x)", sm, to_guest_egl(sm));
    return to_guest_egl(sm);
#else
    return 0x3000;
#endif
}

u32 x_ASensorManager_getDefaultSensor(u32 mgr, int type) {
#ifdef NB_ANDROID_RUNTIME
    ASensorManager* sm = (ASensorManager*)to_host_egl(mgr);
    if (!sm) {
        sm = ASensorManager_getInstanceForPackage("com.psychostark.vemod");
    }
    const ASensor* s = sm ? ASensorManager_getDefaultSensor(sm, type) : nullptr;
    NB_LOGI("ASensorManager_getDefaultSensor(type=%d) -> %p (handle %#x)", type, s, to_guest_egl(const_cast<ASensor*>(s)));
    return to_guest_egl(const_cast<ASensor*>(s));
#else
    return 0x3010;
#endif
}

int x_ASensorManager_getSensorList(u32 mgr, u32 /*list*/) {
#ifdef NB_ANDROID_RUNTIME
    ASensorManager* sm = (ASensorManager*)to_host_egl(mgr);
    if (!sm) sm = ASensorManager_getInstanceForPackage("com.psychostark.vemod");
    if (sm) {
        ASensorList l;
        return ASensorManager_getSensorList(sm, &l);
    }
#endif
    return 0;
}

u32 x_ASensorManager_createEventQueue(u32 mgr, u32 looper, int ident, u32 /*cb*/, u32 /*data*/) {
#ifdef NB_ANDROID_RUNTIME
    ASensorManager* sm = (ASensorManager*)to_host_egl(mgr);
    if (!sm) {
        sm = ASensorManager_getInstanceForPackage("com.psychostark.vemod");
    }
    ALooper* l = (ALooper*)to_host_egl(looper);
    if (!l) {
        l = ALooper_forThread();
        if (!l) l = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    }
    ASensorEventQueue* eq = sm ? ASensorManager_createEventQueue(sm, l, ident, nullptr, nullptr) : nullptr;
    NB_LOGI("ASensorManager_createEventQueue(sm=%p, looper=%p, ident=%d) -> %p (handle %#x)", sm, l, ident, eq, to_guest_egl(eq));
    return to_guest_egl(eq);
#else
    return 0x3020;
#endif
}

int x_ASensorManager_destroyEventQueue(u32 mgr, u32 queue) {
#ifdef NB_ANDROID_RUNTIME
    ASensorManager* sm = (ASensorManager*)to_host_egl(mgr);
    ASensorEventQueue* eq = (ASensorEventQueue*)to_host_egl(queue);
    if (sm && eq) return ASensorManager_destroyEventQueue(sm, eq);
#endif
    return 0;
}

int x_ASensorEventQueue_enableSensor(u32 queue, u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    ASensorEventQueue* eq = (ASensorEventQueue*)to_host_egl(queue);
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (eq && s) {
        int r = ASensorEventQueue_enableSensor(eq, s);
        NB_LOGI("ASensorEventQueue_enableSensor(eq=%p, s=%p) -> %d", eq, s, r);
        return r;
    }
#endif
    return 0;
}

int x_ASensorEventQueue_disableSensor(u32 queue, u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    ASensorEventQueue* eq = (ASensorEventQueue*)to_host_egl(queue);
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (eq && s) return ASensorEventQueue_disableSensor(eq, s);
#endif
    return 0;
}

int x_ASensorEventQueue_setEventRate(u32 queue, u32 sensor, s32 usec) {
#ifdef NB_ANDROID_RUNTIME
    ASensorEventQueue* eq = (ASensorEventQueue*)to_host_egl(queue);
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (eq && s) {
        int r = ASensorEventQueue_setEventRate(eq, s, usec);
        NB_LOGI("ASensorEventQueue_setEventRate(usec=%d) -> %d", usec, r);
        return r;
    }
#endif
    return 0;
}

int x_ASensorEventQueue_hasEvents(u32 queue) {
#ifdef NB_ANDROID_RUNTIME
    ASensorEventQueue* eq = (ASensorEventQueue*)to_host_egl(queue);
    if (eq) return ASensorEventQueue_hasEvents(eq);
#endif
    return 0;
}

int x_ASensorEventQueue_getEvents(u32 queue, u32 events_out, u32 count) {
#ifdef NB_ANDROID_RUNTIME
    ASensorEventQueue* eq = (ASensorEventQueue*)to_host_egl(queue);
    if (eq && events_out && count > 0) {
        ASensorEvent host_events[16];
        size_t n_to_read = std::min<size_t>(count, 16);
        ssize_t n = ASensorEventQueue_getEvents(eq, host_events, n_to_read);
        if (n > 0) {
            std::memcpy(h(events_out), host_events, n * sizeof(ASensorEvent));
            return static_cast<int>(n);
        }
        return static_cast<int>(n);
    }
#endif
    return 0;
}

gaddr x_ASensor_getName(u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (s) {
        const char* name = ASensor_getName(s);
        if (name) {
            gaddr gstr = misc_alloc(strlen(name) + 1, 1);
            memcpy(h(gstr), name, strlen(name) + 1);
            return gstr;
        }
    }
#endif
    return 0;
}

gaddr x_ASensor_getVendor(u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (s) {
        const char* v = ASensor_getVendor(s);
        if (v) {
            gaddr gstr = misc_alloc(strlen(v) + 1, 1);
            memcpy(h(gstr), v, strlen(v) + 1);
            return gstr;
        }
    }
#endif
    return 0;
}

int x_ASensor_getType(u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (s) return ASensor_getType(s);
#endif
    return 1;
}

float x_ASensor_getResolution(u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (s) return ASensor_getResolution(s);
#endif
    return 0.1f;
}

int x_ASensor_getMinDelay(u32 sensor) {
#ifdef NB_ANDROID_RUNTIME
    const ASensor* s = (const ASensor*)to_host_egl(sensor);
    if (s) return ASensor_getMinDelay(s);
#endif
    return 10000;
}

// ------------------------------------------------------------------ ANativeWindow
u32 x_ANativeWindow_fromSurface(u32 /*env*/, u32 surface_obj) {
#ifdef NB_ANDROID_RUNTIME
    JNIEnv* env = jni_env();
    jobject surf = jni_obj(surface_obj);
    if (!env || !surf) {
        NB_LOGW("ANativeWindow_fromSurface: null env (%p) or surf (%p)", env, surf);
        return 0;
    }
    ANativeWindow* win = ANativeWindow_fromSurface(env, surf);
    NB_LOGI("ANativeWindow_fromSurface: created ANativeWindow %p", win);
    return to_guest_egl(win);
#else
    return 0x2000;
#endif
}

int x_ANativeWindow_getWidth(u32 win) {
#ifdef NB_ANDROID_RUNTIME
    ANativeWindow* w = (ANativeWindow*)to_host_egl(win);
    if (w) return ANativeWindow_getWidth(w);
#endif
    return 1080;
}
int x_ANativeWindow_getHeight(u32 win) {
#ifdef NB_ANDROID_RUNTIME
    ANativeWindow* w = (ANativeWindow*)to_host_egl(win);
    if (w) return ANativeWindow_getHeight(w);
#endif
    return 2392;
}
int x_ANativeWindow_getFormat(u32 win) {
#ifdef NB_ANDROID_RUNTIME
    ANativeWindow* w = (ANativeWindow*)to_host_egl(win);
    if (w) return ANativeWindow_getFormat(w);
#endif
    return 1; /* WINDOW_FORMAT_RGBA_8888 */
}
int x_ANativeWindow_setBuffersGeometry(u32 win, int w, int h, int fmt) {
#ifdef NB_ANDROID_RUNTIME
    ANativeWindow* nw = (ANativeWindow*)to_host_egl(win);
    if (nw) return ANativeWindow_setBuffersGeometry(nw, w, h, fmt);
#endif
    return 0;
}
void x_ANativeWindow_acquire(u32 win) {
#ifdef NB_ANDROID_RUNTIME
    ANativeWindow* nw = (ANativeWindow*)to_host_egl(win);
    if (nw) ANativeWindow_acquire(nw);
#endif
}
void x_ANativeWindow_release(u32 win) {
#ifdef NB_ANDROID_RUNTIME
    ANativeWindow* nw = (ANativeWindow*)to_host_egl(win);
    if (nw) ANativeWindow_release(nw);
#endif
}
int x_ANativeWindow_lock(u32 /*win*/, u32 /*outBuf*/, u32 /*bounds*/) { return 0; }
int x_ANativeWindow_unlockAndPost(u32 /*win*/) { return 0; }

}  // namespace

void register_android() {
    reg("__android_log_print", x_log_print);
    NB_REG("__android_log_write", x_log_write);
    NB_REG("AAssetManager_fromJava", x_mgr_from_java);
    NB_REG("AAssetManager_open", x_asset_open);
    NB_REG("AAsset_getLength", x_asset_length);
    NB_REG("AAsset_getLength64", x_asset_length64);
    NB_REG("AAsset_getBuffer", x_asset_buffer);
    NB_REG("AAsset_read", x_asset_read);
    NB_REG("AAsset_seek", x_asset_seek);
    NB_REG("AAsset_getRemainingLength", x_asset_remaining);
    NB_REG("AAsset_openFileDescriptor", x_asset_open_fd);
    NB_REG("AAsset_close", x_asset_close);

    // ALooper
    NB_REG("ALooper_forThread", x_ALooper_forThread);
    NB_REG("ALooper_prepare", x_ALooper_prepare);
    NB_REG("ALooper_acquire", x_ALooper_acquire);
    NB_REG("ALooper_release", x_ALooper_release);
    NB_REG("ALooper_pollOnce", x_ALooper_pollOnce);
    NB_REG("ALooper_pollAll", x_ALooper_pollAll);
    NB_REG("ALooper_wake", x_ALooper_wake);
    NB_REG("ALooper_addFd", x_ALooper_addFd);
    NB_REG("ALooper_removeFd", x_ALooper_removeFd);

    // AInputQueue
    NB_REG("AInputQueue_attachLooper", x_AInputQueue_attachLooper);
    NB_REG("AInputQueue_detachLooper", x_AInputQueue_detachLooper);
    NB_REG("AInputQueue_hasEvents", x_AInputQueue_hasEvents);
    NB_REG("AInputQueue_getEvent", x_AInputQueue_getEvent);
    NB_REG("AInputQueue_preDispatchEvent", x_AInputQueue_preDispatchEvent);
    NB_REG("AInputQueue_finishEvent", x_AInputQueue_finishEvent);

    // AInputEvent & AMotionEvent
    NB_REG("AInputEvent_getType", x_AInputEvent_getType);
    NB_REG("AInputEvent_getDeviceId", x_AInputEvent_getDeviceId);
    NB_REG("AInputEvent_getSource", x_AInputEvent_getSource);
    NB_REG("AMotionEvent_getAction", x_AMotionEvent_getAction);
    NB_REG("AMotionEvent_getFlags", x_AMotionEvent_getFlags);
    NB_REG("AMotionEvent_getMetaState", x_AMotionEvent_getMetaState);
    NB_REG("AMotionEvent_getButtonState", x_AMotionEvent_getButtonState);
    NB_REG("AMotionEvent_getEdgeFlags", x_AMotionEvent_getEdgeFlags);
    NB_REG("AMotionEvent_getDownTime", x_AMotionEvent_getDownTime);
    NB_REG("AMotionEvent_getEventTime", x_AMotionEvent_getEventTime);
    NB_REG("AMotionEvent_getXOffset", x_AMotionEvent_getXOffset);
    NB_REG("AMotionEvent_getYOffset", x_AMotionEvent_getYOffset);
    NB_REG("AMotionEvent_getXPrecision", x_AMotionEvent_getXPrecision);
    NB_REG("AMotionEvent_getYPrecision", x_AMotionEvent_getYPrecision);
    NB_REG("AMotionEvent_getPointerCount", x_AMotionEvent_getPointerCount);
    NB_REG("AMotionEvent_getPointerId", x_AMotionEvent_getPointerId);
    NB_REG("AMotionEvent_getToolType", x_AMotionEvent_getToolType);
    NB_REG("AMotionEvent_getRawX", x_AMotionEvent_getRawX);
    NB_REG("AMotionEvent_getRawY", x_AMotionEvent_getRawY);
    NB_REG("AMotionEvent_getX", x_AMotionEvent_getX);
    NB_REG("AMotionEvent_getY", x_AMotionEvent_getY);
    NB_REG("AMotionEvent_getPressure", x_AMotionEvent_getPressure);
    NB_REG("AMotionEvent_getSize", x_AMotionEvent_getSize);
    NB_REG("AMotionEvent_getTouchMajor", x_AMotionEvent_getTouchMajor);
    NB_REG("AMotionEvent_getTouchMinor", x_AMotionEvent_getTouchMinor);
    NB_REG("AMotionEvent_getToolMajor", x_AMotionEvent_getToolMajor);
    NB_REG("AMotionEvent_getToolMinor", x_AMotionEvent_getToolMinor);
    NB_REG("AMotionEvent_getOrientation", x_AMotionEvent_getOrientation);
    NB_REG("AMotionEvent_getAxisValue", x_AMotionEvent_getAxisValue);
    NB_REG("AMotionEvent_getHistorySize", x_AMotionEvent_getHistorySize);
    NB_REG("AMotionEvent_getHistoricalX", x_AMotionEvent_getHistoricalX);
    NB_REG("AMotionEvent_getHistoricalY", x_AMotionEvent_getHistoricalY);
    NB_REG("AMotionEvent_getHistoricalEventTime", x_AMotionEvent_getHistoricalEventTime);

    // AKeyEvent
    NB_REG("AKeyEvent_getAction", x_AKeyEvent_getAction);
    NB_REG("AKeyEvent_getKeyCode", x_AKeyEvent_getKeyCode);
    NB_REG("AKeyEvent_getMetaState", x_AKeyEvent_getMetaState);

    // ASensor
    NB_REG("ASensorManager_getInstance", x_ASensorManager_getInstance);
    NB_REG("ASensorManager_getInstanceForPackage", x_ASensorManager_getInstanceForPackage);
    NB_REG("ASensorManager_getDefaultSensor", x_ASensorManager_getDefaultSensor);
    NB_REG("ASensorManager_getSensorList", x_ASensorManager_getSensorList);
    NB_REG("ASensorManager_createEventQueue", x_ASensorManager_createEventQueue);
    NB_REG("ASensorManager_destroyEventQueue", x_ASensorManager_destroyEventQueue);
    NB_REG("ASensorEventQueue_enableSensor", x_ASensorEventQueue_enableSensor);
    NB_REG("ASensorEventQueue_disableSensor", x_ASensorEventQueue_disableSensor);
    NB_REG("ASensorEventQueue_setEventRate", x_ASensorEventQueue_setEventRate);
    NB_REG("ASensorEventQueue_hasEvents", x_ASensorEventQueue_hasEvents);
    NB_REG("ASensorEventQueue_getEvents", x_ASensorEventQueue_getEvents);
    NB_REG("ASensor_getName", x_ASensor_getName);
    NB_REG("ASensor_getVendor", x_ASensor_getVendor);
    NB_REG("ASensor_getType", x_ASensor_getType);
    NB_REG("ASensor_getResolution", x_ASensor_getResolution);
    NB_REG("ASensor_getMinDelay", x_ASensor_getMinDelay);

    // AConfiguration
    NB_REG("AConfiguration_new", x_AConfiguration_new);
    NB_REG("AConfiguration_delete", x_AConfiguration_delete);
    NB_REG("AConfiguration_fromAssetManager", x_AConfiguration_fromAssetManager);
    NB_REG("AConfiguration_getOrientation", x_AConfiguration_getOrientation);

    // ANativeWindow
    NB_REG("ANativeWindow_fromSurface", x_ANativeWindow_fromSurface);
    NB_REG("ANativeWindow_getWidth", x_ANativeWindow_getWidth);
    NB_REG("ANativeWindow_getHeight", x_ANativeWindow_getHeight);
    NB_REG("ANativeWindow_getFormat", x_ANativeWindow_getFormat);
    NB_REG("ANativeWindow_setBuffersGeometry", x_ANativeWindow_setBuffersGeometry);
    NB_REG("ANativeWindow_acquire", x_ANativeWindow_acquire);
    NB_REG("ANativeWindow_release", x_ANativeWindow_release);
    NB_REG("ANativeWindow_lock", x_ANativeWindow_lock);
    NB_REG("ANativeWindow_unlockAndPost", x_ANativeWindow_unlockAndPost);
}

}  // namespace nb
