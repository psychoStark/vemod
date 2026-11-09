#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <jni.h>

#include "nb.h"

namespace vemod {

// Registers of a JNI native call saved by thunks.S (AArch64)
struct NativeRegs {
    uint64_t x[8];
    uint64_t d[8];
    uint64_t* stack;
    uint64_t pad;
};

constexpr size_t kNativeThunkCount = 16384;

struct NativeTarget {
    nb::gaddr guest_function = 0;
    std::string shorty;  // return type first, e.g. "VIL"
    bool is_static = false;
    std::string label;
};

void* get_thunk_address(uint32_t slot);

class NativeSlots {
public:
    explicit NativeSlots(size_t capacity = kNativeThunkCount);
    int32_t allocate(NativeTarget target);
    void release(uint32_t slot);
    const NativeTarget* target(uint32_t slot) const;

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::unique_ptr<NativeTarget[]> targets_;
    std::unique_ptr<std::atomic<bool>[]> ready_;
    std::atomic<uint32_t> count_{0};
    std::vector<uint32_t> released_;
};

void init_native_dispatcher();
NativeSlots& get_native_slots();

void* allocate_native_thunk(nb::gaddr guest_fn, std::string_view shorty, const char* label);

}  // namespace vemod
