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

    vemod::init_native_dispatcher();
    nb::runtime_init();

    LOGI("VemodBridge core JNI loaded successfully");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL
Java_com_vemod_container_bridge_VemodBridge_nativeInit(JNIEnv* env, jclass, jstring filesDir) {
    nb::runtime_init();
    LOGI("VemodBridge runtime initialized");
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
