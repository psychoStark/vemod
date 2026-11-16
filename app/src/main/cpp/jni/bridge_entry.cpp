#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>

#include "nb.h"
#include "jni_bridge.h"
#include "native_dispatch.h"
#include "jni_shorty.h"

#define LOG_TAG "VemodBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    nb::jni_set_vm(vm);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    setenv("GC_DONT_GC", "1", 1);
    setenv("MONO_DISABLE_SHM", "1", 1);
    setenv("MONO_DISABLE_AIO", "1", 1);

    vemod::init_native_dispatcher();
    nb::runtime_init();

    LOGI("VemodBridge core JNI loaded successfully");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeSetLogLevel(JNIEnv*, jclass, jint level) {
    nb::set_log_level(level);
}

JNIEXPORT void JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeInit(JNIEnv* env, jclass, jstring filesDir) {
    setenv("GC_DONT_GC", "1", 1);
    setenv("MONO_DISABLE_SHM", "1", 1);
    setenv("MONO_DISABLE_AIO", "1", 1);
    nb::runtime_init();
    if (filesDir) {
        const char* path = env->GetStringUTFChars(filesDir, nullptr);
        if (path) {
            std::string sysrootDir = std::string(path) + "/sysroot";
            nb::set_sysroot_dir(sysrootDir.c_str());
            nb::set_container_dir(path);
            env->ReleaseStringUTFChars(filesDir, path);
        }
    }
    LOGI("VemodBridge runtime initialized");
}

JNIEXPORT void JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeSetClassLoader(JNIEnv* env, jclass, jobject loader) {
    nb::set_guest_class_loader(loader);
}

JNIEXPORT void JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeSetSysrootDir(JNIEnv* env, jclass, jstring path) {
    if (!path) return;
    const char* p = env->GetStringUTFChars(path, nullptr);
    if (p) {
        LOGI("nativeSetSysrootDir: %s", p);
        nb::set_sysroot_dir(p);
        nb::preload_sysroot(p);
        env->ReleaseStringUTFChars(path, p);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeLoadLibrary(JNIEnv* env, jclass, jstring libPath) {
    const char* path = env->GetStringUTFChars(libPath, nullptr);
    if (!path) return JNI_FALSE;

    LOGI("nativeLoadLibrary: %s", path);
    bool ok = nb::load_guest_library(path);
    env->ReleaseStringUTFChars(libPath, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeGetExports(JNIEnv* env, jclass) {
    const auto& exports = nb::get_guest_exports();
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(exports.size()), stringClass, nullptr);

    jsize idx = 0;
    for (const auto& [name, addr] : exports) {
        jstring str = env->NewStringUTF(name.c_str());
        env->SetObjectArrayElement(arr, idx++, str);
        env->DeleteLocalRef(str);
    }
    return arr;
}

JNIEXPORT jlong JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeGetGuestSymbol(JNIEnv* env, jclass, jstring symName) {
    const char* name = env->GetStringUTFChars(symName, nullptr);
    if (!name) return 0;

    nb::gaddr addr = nb::guest_sym(name);
    env->ReleaseStringUTFChars(symName, name);
    return static_cast<jlong>(addr);
}

JNIEXPORT jint JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeCallJniOnLoad(JNIEnv* env, jclass) {
    nb::gaddr on_load = nb::guest_last_jni_onload();
    if (!on_load) {
        LOGI("No JNI_OnLoad exported by newly loaded guest library; returning JNI_VERSION_1_6");
        return JNI_VERSION_1_6;
    }

    LOGI("Calling guest JNI_OnLoad at %#x", on_load);
    nb::JniFrame frame(env);
    uint64_t ret = nb::call_guest(on_load, {
        nb::ga(nb::jni_guest_vm()),
        nb::ga(0) // reserved
    });

    jint version = static_cast<jint>(static_cast<uint32_t>(ret));
    LOGI("Guest JNI_OnLoad returned %#x", version);
    return version;
}

JNIEXPORT jboolean JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeCallNativeActivityOnCreate(
    JNIEnv* env, jclass, jlong hostActivity, jlong hostSavedState, jlong hostSavedStateSize) {

    nb::gaddr on_create = nb::guest_sym("ANativeActivity_onCreate");
    if (!on_create) {
        LOGI("ANativeActivity_onCreate not found in guest libraries; skipping");
        return JNI_TRUE;
    }

    LOGI("Invoking guest ANativeActivity_onCreate at %#x (hostActivity=%p)", on_create, reinterpret_cast<void*>(hostActivity));

    // Allocate 32-bit guest structures
    static nb::gaddr s_guest_activity = 0;
    static nb::gaddr s_callbacks = 0;
    if (!s_guest_activity) {
        s_callbacks = nb::misc_alloc(128, 8);
        s_guest_activity = nb::misc_alloc(128, 8);
    }
    if (!s_guest_activity || !s_callbacks) {
        LOGE("Failed to allocate guest memory for ANativeActivity");
        return JNI_FALSE;
    }

    memset(nb::h(s_callbacks), 0, 128);
    memset(nb::h(s_guest_activity), 0, 128);

    struct HostNativeActivityCallbacks {
        void (*onStart)(void* activity);
        void (*onResume)(void* activity);
        void* (*onSaveInstanceState)(void* activity, size_t* outSize);
        void (*onPause)(void* activity);
        void (*onStop)(void* activity);
        void (*onDestroy)(void* activity);
        void (*onWindowFocusChanged)(void* activity, int hasFocus);
        void (*onNativeWindowCreated)(void* activity, void* window);
        void (*onNativeWindowResized)(void* activity, void* window);
        void (*onNativeWindowRedrawNeeded)(void* activity, void* window);
        void (*onNativeWindowDestroyed)(void* activity, void* window);
        void (*onInputQueueCreated)(void* activity, void* queue);
        void (*onInputQueueDestroyed)(void* activity, void* queue);
        void (*onContentRectChanged)(void* activity, const void* rect);
        void (*onConfigurationChanged)(void* activity);
        void (*onLowMemory)(void* activity);
    };

    struct HostNativeActivity {
        void* callbacks;
        JavaVM* vm;
        JNIEnv* env;
        jobject clazz;
        const char* internalDataPath;
        const char* externalDataPath;
        int32_t sdkVersion;
        void* instance;
        void* assetManager;
        const char* obbPath;
    };

    static auto host_onStart = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 0 * 4);
        if (fn) {
            LOGI("Forwarding onStart to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    static auto host_onResume = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 1 * 4);
        if (fn) {
            LOGI("Forwarding onResume to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    static auto host_onSaveInstanceState = [](void*, size_t* outSize) -> void* {
        if (!s_callbacks || !s_guest_activity) return nullptr;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 2 * 4);
        if (fn) {
            LOGI("Forwarding onSaveInstanceState to guest %#x", fn);
            nb::gaddr psize = nb::misc_alloc(4, 4);
            nb::wr<uint32_t>(psize, 0);
            nb::gaddr res = nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(psize)});
            uint32_t guest_size = nb::rd<uint32_t>(psize);
            if (outSize) *outSize = guest_size;
            if (res && guest_size > 0) {
                void* host_buf = malloc(guest_size);
                memcpy(host_buf, nb::h(res), guest_size);
                return host_buf;
            }
        }
        return nullptr;
    };

    static auto host_onPause = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 3 * 4);
        if (fn) {
            LOGI("Forwarding onPause to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    static auto host_onStop = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 4 * 4);
        if (fn) {
            LOGI("Forwarding onStop to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    static auto host_onDestroy = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 5 * 4);
        if (fn) {
            LOGI("Forwarding onDestroy to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    static auto host_onWindowFocusChanged = [](void*, int hasFocus) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 6 * 4);
        if (fn) {
            LOGI("Forwarding onWindowFocusChanged(%d) to guest %#x", hasFocus, fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(static_cast<uint32_t>(hasFocus))});
        }
    };

    static auto host_onNativeWindowCreated = [](void*, void* window) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 7 * 4);
        if (fn) {
            LOGI("Forwarding onNativeWindowCreated(window=%p) to guest %#x", window, fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(0x1000)});
        }
    };

    static auto host_onNativeWindowResized = [](void*, void* window) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 8 * 4);
        if (fn) {
            LOGI("Forwarding onNativeWindowResized(window=%p) to guest %#x", window, fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(0x1000)});
        }
    };

    static auto host_onNativeWindowRedrawNeeded = [](void*, void* window) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 9 * 4);
        if (fn) {
            LOGI("Forwarding onNativeWindowRedrawNeeded(window=%p) to guest %#x", window, fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(0x1000)});
        }
    };

    static auto host_onNativeWindowDestroyed = [](void*, void* window) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 10 * 4);
        if (fn) {
            LOGI("Forwarding onNativeWindowDestroyed to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(0x1000)});
        }
    };

    static auto host_onInputQueueCreated = [](void*, void* queue) {
        nb::start_host_input_pump(queue);
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 11 * 4);
        if (fn) {
            LOGI("Forwarding onInputQueueCreated(queue=%p) to guest %#x", queue, fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(nb::to_guest_egl(queue))});
        }
    };

    static auto host_onInputQueueDestroyed = [](void*, void* queue) {
        nb::stop_host_input_pump();
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 12 * 4);
        if (fn) {
            LOGI("Forwarding onInputQueueDestroyed to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(nb::to_guest_egl(queue))});
        }
    };

    static auto host_onContentRectChanged = [](void*, const void* rect) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 13 * 4);
        if (fn) {
            LOGI("Forwarding onContentRectChanged to guest %#x", fn);
            nb::gaddr grect = nb::misc_alloc(16, 4);
            if (rect) memcpy(nb::h(grect), rect, 16);
            nb::call_guest(fn, {nb::ga(s_guest_activity), nb::ga(grect)});
        }
    };

    static auto host_onConfigurationChanged = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 14 * 4);
        if (fn) {
            LOGI("Forwarding onConfigurationChanged to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    static auto host_onLowMemory = [](void*) {
        if (!s_callbacks || !s_guest_activity) return;
        nb::gaddr fn = nb::rd<uint32_t>(s_callbacks + 15 * 4);
        if (fn) {
            LOGI("Forwarding onLowMemory to guest %#x", fn);
            nb::call_guest(fn, {nb::ga(s_guest_activity)});
        }
    };

    auto* host = reinterpret_cast<HostNativeActivity*>(hostActivity);

    nb::gaddr internal_path_addr = (host && host->internalDataPath) ? nb::gstrdup(host->internalDataPath) : 0;
    nb::gaddr external_path_addr = (host && host->externalDataPath) ? nb::gstrdup(host->externalDataPath) : 0;
    nb::gaddr obb_path_addr = (host && host->obbPath) ? nb::gstrdup(host->obbPath) : 0;

    nb::JniFrame frame(env);
    uint32_t guest_clazz = (host && host->clazz) ? nb::jni_local(host->clazz) : 0;

    auto* act32 = reinterpret_cast<uint32_t*>(nb::h(s_guest_activity));
    act32[0] = s_callbacks;                              // callbacks
    act32[1] = nb::jni_guest_vm();                       // vm
    act32[2] = nb::jni_guest_env();                      // env
    act32[3] = guest_clazz;                              // clazz
    act32[4] = internal_path_addr;                       // internalDataPath
    act32[5] = external_path_addr;                       // externalDataPath
    act32[6] = host ? host->sdkVersion : 24;             // sdkVersion
    act32[7] = 0;                                        // instance
    act32[8] = 0;                                        // assetManager
    act32[9] = obb_path_addr;                            // obbPath

    nb::gaddr saved_state_addr = 0;
    if (hostSavedState && hostSavedStateSize > 0) {
        saved_state_addr = nb::misc_alloc(static_cast<uint32_t>(hostSavedStateSize), 4);
        memcpy(nb::h(saved_state_addr), reinterpret_cast<const void*>(hostSavedState), hostSavedStateSize);
    }

    LOGI("Calling guest ANativeActivity_onCreate(act32=%#x, cb32=%#x, vm=%#x, env=%#x)",
         s_guest_activity, s_callbacks, nb::jni_guest_vm(), nb::jni_guest_env());

    nb::call_guest(on_create, {
        nb::ga(s_guest_activity),
        nb::ga(saved_state_addr),
        nb::ga(static_cast<uint32_t>(hostSavedStateSize))
    });

    LOGI("Guest ANativeActivity_onCreate finished successfully!");

    if (host) {
        if (act32[7]) {
            host->instance = reinterpret_cast<void*>(static_cast<uintptr_t>(act32[7]));
        }
        if (host->callbacks) {
            auto* host_cb = reinterpret_cast<HostNativeActivityCallbacks*>(host->callbacks);
            host_cb->onStart = host_onStart;
            host_cb->onResume = host_onResume;
            host_cb->onSaveInstanceState = host_onSaveInstanceState;
            host_cb->onPause = host_onPause;
            host_cb->onStop = host_onStop;
            host_cb->onDestroy = host_onDestroy;
            host_cb->onWindowFocusChanged = host_onWindowFocusChanged;
            host_cb->onNativeWindowCreated = host_onNativeWindowCreated;
            host_cb->onNativeWindowResized = host_onNativeWindowResized;
            host_cb->onNativeWindowRedrawNeeded = host_onNativeWindowRedrawNeeded;
            host_cb->onNativeWindowDestroyed = host_onNativeWindowDestroyed;
            host_cb->onInputQueueCreated = host_onInputQueueCreated;
            host_cb->onInputQueueDestroyed = host_onInputQueueDestroyed;
            host_cb->onContentRectChanged = host_onContentRectChanged;
            host_cb->onConfigurationChanged = host_onConfigurationChanged;
            host_cb->onLowMemory = host_onLowMemory;
            LOGI("Populated 64-bit host->callbacks with bridge trampolines!");
        }
    }

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeRegisterMethod(
    JNIEnv* env, jclass, jclass targetClass, jstring methodName, jstring methodSig, jlong guestFn) {

    const char* name = env->GetStringUTFChars(methodName, nullptr);
    const char* sig = env->GetStringUTFChars(methodSig, nullptr);

    auto shorty = zb::shorty_from_signature(sig);
    if (!shorty) {
        LOGE("nativeRegisterMethod: invalid signature %s for %s", sig, name);
        env->ReleaseStringUTFChars(methodName, name);
        env->ReleaseStringUTFChars(methodSig, sig);
        return JNI_FALSE;
    }

    void* thunk = vemod::allocate_native_thunk(static_cast<nb::gaddr>(guestFn), *shorty, name);
    if (!thunk) {
        LOGE("nativeRegisterMethod: failed to allocate thunk for %s%s", name, sig);
        env->ReleaseStringUTFChars(methodName, name);
        env->ReleaseStringUTFChars(methodSig, sig);
        return JNI_FALSE;
    }

    JNINativeMethod m;
    m.name = const_cast<char*>(name);
    m.signature = const_cast<char*>(sig);
    m.fnPtr = thunk;

    jint res = env->RegisterNatives(targetClass, &m, 1);
    if (res != JNI_OK) {
        LOGE("RegisterNatives failed for %s%s: code %d", name, sig, res);
    } else {
        LOGI("Bound native: %s%s -> guest %#x via thunk %p", name, sig, static_cast<uint32_t>(guestFn), thunk);
    }

    env->ReleaseStringUTFChars(methodName, name);
    env->ReleaseStringUTFChars(methodSig, sig);
    return res == JNI_OK ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
