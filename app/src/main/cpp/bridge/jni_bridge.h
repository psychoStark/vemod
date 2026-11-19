#pragma once

#include <jni.h>

#include "nb.h"

namespace nb {

void jni_set_vm(JavaVM* vm);
gaddr jni_guest_env();
gaddr jni_guest_vm();
JNIEnv* jni_env();  // host JNIEnv of the calling thread

// Mirrors a JVM local-reference frame for the duration of a Java -> native call.
class JniFrame {
public:
    explicit JniFrame(JNIEnv* env);
    ~JniFrame();
    JniFrame(const JniFrame&) = delete;
    JniFrame& operator=(const JniFrame&) = delete;
};

u32 jni_local(jobject o);  // host local ref -> guest handle in the current frame
u32 jni_global(jobject o); // host global ref -> permanent guest handle
jobject jni_obj(u32 h);    // guest handle -> host reference

void jni_sync_direct_buffers_to_host();
bool jni_has_direct_buffers();
void jni_sync_buffer_arg(u32 hnd);
void jni_sync_buffer_guest(gaddr ga);

}  // namespace nb
