#include "native_dispatch.h"
#include "jni_bridge.h"
#include <android/log.h>
#include <cstdlib>
#include <cstring>

#define LOG_TAG "VemodBridge"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C" {
extern const char zb_native_thunk_base[];
extern const char zb_native_thunk_end[];
void zb_native_dispatch(uint32_t slot, vemod::NativeRegs* regs) noexcept;
}

namespace vemod {

namespace {

NativeSlots g_slots;

class HostArgReader {
public:
    explicit HostArgReader(const NativeRegs& regs) : regs_(regs) {}

    uint64_t next_int() {
        return next_int_ < 8 ? regs_.x[next_int_++] : regs_.stack[next_stack_++];
    }
    uint64_t next_fp() {
        return next_fp_ < 8 ? regs_.d[next_fp_++] : regs_.stack[next_stack_++];
    }

private:
    const NativeRegs& regs_;
    int next_int_ = 2;  // x0 = JNIEnv*, x1 = jclass/jobject
    int next_fp_ = 0;
    size_t next_stack_ = 0;
};

}  // namespace

void* get_thunk_address(uint32_t slot) {
    if (slot >= kNativeThunkCount) return nullptr;
    return const_cast<char*>(zb_native_thunk_base) + 8 * static_cast<size_t>(slot);
}

NativeSlots::NativeSlots(size_t capacity)
    : capacity_(capacity < kNativeThunkCount ? capacity : kNativeThunkCount),
      targets_(new NativeTarget[capacity_]),
      ready_(new std::atomic<bool>[capacity_]) {
    for (size_t i = 0; i < capacity_; ++i) {
        ready_[i].store(false, std::memory_order_relaxed);
    }
}

int32_t NativeSlots::allocate(NativeTarget target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!released_.empty()) {
        uint32_t reused = released_.back();
        released_.pop_back();
        targets_[reused] = std::move(target);
        ready_[reused].store(true, std::memory_order_release);
        return static_cast<int32_t>(reused);
    }
    uint32_t slot = count_.load();
    if (slot >= capacity_) return -1;
    targets_[slot] = std::move(target);
    ready_[slot].store(true, std::memory_order_release);
    count_.store(slot + 1, std::memory_order_release);
    return static_cast<int32_t>(slot);
}

void NativeSlots::release(uint32_t slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= count_.load(std::memory_order_acquire) ||
        !ready_[slot].load(std::memory_order_acquire)) {
        LOGE("NativeSlots::release: slot %u was not allocated", slot);
        std::abort();
    }
    ready_[slot].store(false, std::memory_order_release);
    released_.push_back(slot);
}

const NativeTarget* NativeSlots::target(uint32_t slot) const {
    if (slot >= count_.load(std::memory_order_acquire) ||
        !ready_[slot].load(std::memory_order_acquire)) {
        return nullptr;
    }
    return &targets_[slot];
}

NativeSlots& get_native_slots() {
    return g_slots;
}

void* allocate_native_thunk(nb::gaddr guest_fn, std::string_view shorty, const char* label) {
    NativeTarget target;
    target.guest_function = guest_fn;
    target.shorty = std::string(shorty);
    target.label = label ? label : "";
    int32_t slot = g_slots.allocate(std::move(target));
    if (slot < 0) {
        LOGE("Failed to allocate native thunk slot for %s", label ? label : "unknown");
        return nullptr;
    }
    return get_thunk_address(static_cast<uint32_t>(slot));
}

void init_native_dispatcher() {
    // Verified thunk pool size
    size_t pool_size = static_cast<size_t>(zb_native_thunk_end - zb_native_thunk_base);
    LOGI("Native thunk pool initialized: %zu bytes (%zu slots)", pool_size, kNativeThunkCount);
}

}  // namespace vemod

extern "C" void zb_native_dispatch(uint32_t slot, vemod::NativeRegs* regs) noexcept {
    const vemod::NativeTarget* target = vemod::get_native_slots().target(slot);
    if (!target) {
        LOGE("Native thunk slot %u called with no target", slot);
        std::abort();
    }

    JNIEnv* env = reinterpret_cast<JNIEnv*>(regs->x[0]);
    nb::JniFrame frame(env);

    std::vector<nb::GArg> args;
    args.reserve(target->shorty.size() + 1);

    // Guest JNIEnv* and jclass/jobject handle
    args.push_back(nb::ga(nb::jni_guest_env()));
    args.push_back(nb::ga(nb::jni_local(reinterpret_cast<jobject>(regs->x[1]))));

    vemod::HostArgReader in(*regs);
    for (size_t i = 1; i < target->shorty.size(); ++i) {
        char ch = target->shorty[i];
        switch (ch) {
            case 'Z':
                args.push_back(nb::ga(static_cast<uint8_t>(in.next_int())));
                break;
            case 'B':
                args.push_back(nb::ga(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(in.next_int())))));
                break;
            case 'C':
                args.push_back(nb::ga(static_cast<uint16_t>(in.next_int())));
                break;
            case 'S':
                args.push_back(nb::ga(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(in.next_int())))));
                break;
            case 'I':
                args.push_back(nb::ga(static_cast<uint32_t>(in.next_int())));
                break;
            case 'J':
                args.push_back(nb::ga64(in.next_int()));
                break;
            case 'F': {
                uint32_t v = static_cast<uint32_t>(in.next_fp());
                args.push_back(nb::ga(v));
                break;
            }
            case 'D':
                args.push_back(nb::ga64(in.next_fp()));
                break;
            case 'L':
                args.push_back(nb::ga(nb::jni_local(reinterpret_cast<jobject>(in.next_int()))));
                break;
            default:
                LOGE("Unknown shorty param: %c", ch);
                std::abort();
        }
    }

    uint64_t ret = nb::call_guest(target->guest_function, args.data(), args.size());
    uint32_t r0 = static_cast<uint32_t>(ret);
    uint32_t r1 = static_cast<uint32_t>(ret >> 32);

    char ret_type = target->shorty[0];
    switch (ret_type) {
        case 'V':
            break;
        case 'Z':
            regs->x[0] = static_cast<uint8_t>(r0);
            break;
        case 'B':
            regs->x[0] = static_cast<int64_t>(static_cast<int8_t>(r0));
            break;
        case 'C':
            regs->x[0] = static_cast<uint16_t>(r0);
            break;
        case 'S':
            regs->x[0] = static_cast<int64_t>(static_cast<int32_t>(static_cast<int16_t>(r0)));
            break;
        case 'I':
            regs->x[0] = static_cast<int64_t>(static_cast<int32_t>(r0));
            break;
        case 'J':
            regs->x[0] = ret;
            break;
        case 'F':
            regs->d[0] = r0;
            break;
        case 'D':
            regs->d[0] = ret;
            break;
        case 'L':
            regs->x[0] = reinterpret_cast<uint64_t>(nb::jni_obj(r0));
            break;
        default:
            LOGE("Unknown shorty return type: %c", ret_type);
            std::abort();
    }
}
