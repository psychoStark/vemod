cmake_minimum_required(VERSION 3.22)
project(vemod_bridge C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

# 1. Main translation bridge: libbridge.so
add_library(bridge SHARED
    bridge/android.cpp
    bridge/cpu.cpp
    bridge/gles.cpp
    bridge/guest_heap.c
    bridge/host_heap.c
    bridge/host_new.cpp
    bridge/jni_bridge.cpp
    bridge/libc.cpp
    bridge/loader.cpp
    bridge/memory.cpp
    bridge/pthread.cpp
    bridge/stdio.cpp
    bridge/warmup.cpp
    bridge/zlib_bridge.cpp
    jni/thunks.S
    jni/jni_mangle.cpp
    jni/jni_shorty.cpp
    jni/native_dispatch.cpp
    jni/bridge_entry.cpp
)

target_include_directories(bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
)

target_link_libraries(bridge PRIVATE dynarmic)
target_compile_options(bridge PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility=hidden -fvisibility-inlines-hidden>
    $<$<COMPILE_LANGUAGE:C>:-fvisibility=hidden -w>)

if(ANDROID)
    target_compile_definitions(bridge PRIVATE NB_ANDROID_RUNTIME=1)
    target_link_libraries(bridge PRIVATE GLESv2 android log z)
    target_link_options(bridge PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,--exclude-libs,ALL")
else()
    find_package(JNI REQUIRED)
    target_include_directories(bridge PRIVATE ${JNI_INCLUDE_DIRS})
    target_link_libraries(bridge PRIVATE GLESv2)
endif()

# 2. Template Proxy: libproxy.so
add_library(proxy SHARED
    proxy/proxy.c
)
if(ANDROID)
    target_link_libraries(proxy PRIVATE log)
    target_link_options(proxy PRIVATE "-Wl,-z,max-page-size=16384")
endif()
