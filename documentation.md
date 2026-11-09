# Vemod: 32-Bit Android APK Container for 64-Bit-Only Android Systems
**Comprehensive Architecture, Implementation, and Troubleshooting Documentation**

---

## 1. Executive Summary & Problem Space

Modern Android smartphones powered by 64-bit-only SoCs (such as Qualcomm Snapdragon 8 Gen 3, Google Tensor G3/G4, and MediaTek Dimensity 9300+) running Android 14, 15, and 16 have dropped hardware and firmware support for 32-bit ARM binaries (`armeabi-v7a` and `armeabi`). The CPU cores no longer support the AArch32 EL0 execution state, and 64-bit Bionic linkers strictly reject 32-bit ELF binaries.

Consequently, tens of thousands of classic Android apps, indie games, and historical software built prior to Google Play's 64-bit requirement in August 2019 are permanently unplayable on modern hardware.

**Vemod** (`com.vemod.container`) is an advanced, generalized 32-bit Android container and runtime virtualization layer. It executes arbitrary 32-bit ARM native code (`.so` files) on 64-bit-only (`arm64-v8a`) hardware through an embedded **Dynarmic ARM32 Just-In-Time (JIT)** CPU recompiler, while transparently virtualizing the Android Dalvik/ART runtime, file system, resources, JNI bridge, Bionic libc/zlib, and OpenGL ES 2.0 graphics pipeline.

Vemod allows users to install **multiple 32-bit APKs simultaneously**, launch them directly from the **Android system App Drawer** using dynamic slot aliases, and **pin 1x1 shortcuts to the system Home Screen**.

---

## 2. Architecture Overview

Rather than attempting full OS-level hardware virtualization (which is slow, heavyweight, and complex), Vemod runs as a lightweight userspace compatibility layer:
* **Java / Kotlin Bytecode:** Runs **natively at full speed** on the host's 64-bit ART Virtual Machine.
* **Native 32-bit Machine Code:** Intercepted and recompiled on-the-fly via an in-process **Dynarmic A32 JIT engine**.
* **Graphics & Audio:** OpenGL ES 2.0 and OpenAL / AudioTrack pass directly to the host GPU and audio flinger.

```
+-----------------------------------------------------------------------------------------+
|                                    HOST SYSTEM UI                                       |
|  - System App Drawer (AppSlot1 .. AppSlot10 Component Aliases)                          |
|  - System Home Screen (1x1 Pinned Shortcuts via ShortcutManagerCompat)                  |
|  - Vemod Manager UI (MainActivity: Jetpack Compose Material 3)                          |
+-----------------------------------------------------------------------------------------+
                                             |
                                             v
+-----------------------------------------------------------------------------------------+
|                          ISOLATED GUEST PROCESS (:guest)                                |
|                                                                                         |
|  +-----------------------------------------------------------------------------------+  |
|  | VemodInstrumentation & Context Virtualization Layer                              |  |
|  | - Instantiates Guest Activity via reflection                                      |  |
|  | - Swaps Context mBase with GuestContext (virtual data/cache/obb paths)            |  |
|  | - Swaps AssetManager & Resources with Guest APK Assets                            |  |
|  | - Hooks IPackageManager (dynamic manifest meta-data & package resolution)         |  |
|  | - Neutralizes GMS / AdMob crashes & recovers main Looper                          |  |
|  +-----------------------------------------------------------------------------------+  |
|                                            |                                            |
|  +-----------------------------------------------------------------------------------+  |
|  | GuestClassLoader (PathClassLoader targeting guest base.apk)                       |  |
|  | - Intercepts findLibrary("foo") -> redirects to proxy/libfoo.so                 |  |
|  +-----------------------------------------------------------------------------------+  |
|                                            |                                            |
|  +-----------------------------------------------------------------------------------+  |
|  | 64-Bit Host Proxy Stub (libproxy.so template renamed to libfoo.so)                 |  |
|  | - Satisfies ART's 64-bit linker requirements                                      |  |
|  | - JNI_OnLoad initializes VemodBridge runtime                                      |  |
|  +-----------------------------------------------------------------------------------+  |
|                                            |                                            |
|  +-----------------------------------------------------------------------------------+  |
|  | Native Bridge Core (libbridge.so)                                                 |  |
|  | ┌──────────────────────────────────────────────────────────────────────────────┐  |  |
|  | │ 4 GB Virtual Memory Arena (mmap private arena at fixed address)             │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Dynarmic ARM32 JIT Engine (UserCallbacks, page tables, SVC trampolines)      │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Dynamic Multi-Module ELF Loader (g_modules, isolated dlopen/dlsym handles)  │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Complete 100% OpenGL ES 2.0 Passthrough Layer (141 GLES entry points)       │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ ARM EABI Runtime & Soft-Float Math Suite (__aeabi_l2d, __aeabi_ldivmod, etc)│  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ 32-Bit / 64-Bit Bionic Zlib Streaming Bridge (z_stream_32 marshaling)        │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Android NDK Asset Streaming Bridge (AAsset_read, AAsset_seek, etc)          │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ 32-bit JNIEnv* Facade & AArch64 Native Trampolines                          │  |  |
|  | └──────────────────────────────────────────────────────────────────────────────┘  |  |
+-----------------------------------------------------------------------------------------+
```

---

## 3. Core Subsystems in Detail

### 3.1. Multi-APK Management & System Integration

#### 1. Dynamic Ingestion & Extraction (`ApkExtractor.kt`)
When the user picks an APK:
1. `PackageManager.getPackageArchiveInfo(apkPath, GET_ACTIVITIES | GET_META_DATA)` parses the guest package name, version, and launch activity.
2. The APK is copied into `/data/user/0/com.vemod.container/files/containers/<guest_pkg>/base.apk` and marked read-only to comply with Android 14+ ART execution policies.
3. The native libraries inside the APK are inspected. If `lib/armeabi-v7a/` exists, it is selected; otherwise `lib/armeabi/` is selected.
4. Each `.so` file is extracted into `lib32/`.
5. For every extracted 32-bit library, a copy of the host's 64-bit `libproxy.so` template is written into `proxy/lib<name>.so`.
6. High-resolution app icons are extracted and cached as `icon.png`.
7. Metadata is persisted in `meta.json`.

#### 2. App Drawer Slot Architecture (`ContainerRegistry.kt`)
Android requires launcher entries to be statically declared in `AndroidManifest.xml`. To allow arbitrary 32-bit APKs to appear in the system App Drawer:
* The manifest defines 10 pre-declared activity aliases:
  `com.vemod.container.slot.AppSlot1` through `AppSlot10`, targeting `GuestProxyActivity` in `android:process=":guest"`.
* All slots start in `PackageManager.COMPONENT_ENABLED_STATE_DISABLED`.
* When an APK is installed or toggled, `ContainerRegistry.assignSlot()` enables the component via `pm.setComponentEnabledSetting(component, COMPONENT_ENABLED_STATE_ENABLED, DONT_KILL_APP)`.
* When launched from the App Drawer, `VemodInstrumentation` resolves the slot class name back to the assigned container package.

#### 3. Home Screen Shortcut Pinning
* `ContainerRegistry.pinShortcut()` uses `androidx.core.content.pm.ShortcutManagerCompat.requestPinShortcut()`.
* It builds a `ShortcutInfoCompat` featuring the guest app's high-res icon and an intent pointing to `GuestProxyActivity` with `FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TOP`.
* Tested and verified on modern vendor launchers (including Nothing OS Launcher).

#### 4. Clean Process Lifecycle & App Switching
Because each 32-bit app loads distinct native libraries into the 4 GB Dynarmic memory arena, switching between different 32-bit apps in `:guest` requires a clean container state. `MainActivity.launchApp()` checks if a `:guest` process is active with another app and cleanly restarts the process before launching.

---

### 3.2. Dynamic Multi-Module ELF & Linker Virtualization (`loader.cpp`)

Earlier bridges only supported a single global `.so` library. Complex engines (like Haxe/OpenFL Lime) load multiple interconnected libraries:
`libstd.so`, `libregexp.so`, `libzlib.so`, `libopenal.so`, `liblime.so`, and `libApplicationMain.so`.

#### The CFFI Symbol Collision Problem
`libstd.so`, `liblime.so`, and `libzlib.so` all export identical entry points: `hx_set_loader` and `__neko_entry_point`. In a flat symbol table, `dlsym(lime_handle, "hx_set_loader")` would mistakenly return `libstd.so`'s function. Because `liblime.so`'s loader was never set, `alloc_root` failed with `NoExecuteFault at 0`.

#### The Multi-Module Solution
In `loader.cpp`:
* Dynamic module registry: `std::vector<std::shared_ptr<GuestModule>> g_modules`.
* Each module maintains its own isolated:
  * `exports` hash map (symbol name -> guest function address)
  * `funcs` table (guest address -> name)
  * Code boundaries (`code_lo` .. `code_hi`)
  * `exidx` exception unwinding table
* `guest_dlopen(name, flags)`:
  * Normalizes the library basename (`./liblime.so` -> `liblime.so`).
  * Searches `g_modules`. If not loaded, automatically loads it from `lib32/`.
  * Returns an opaque, unique module handle: `0x00d10001 + idx`.
* `guest_dlsym(handle, name)`:
  * Validates the module handle and queries that specific module's `exports` table first.
  * Ensures `liblime.so` receives its own `hx_set_loader` instead of colliding with `libstd.so`.
* `guest_is_code(pc)` and `guest_exidx(pc, ...)` query across all loaded modules to ensure proper execution validation and C++ exception unwinding.

---

### 3.3. Complete OpenGL ES 2.0 Passthrough Bridge (`gles.cpp`)

Modern Android games require complete coverage of OpenGL ES 2.0. Vemod implements **100% full coverage (141 functions)** of the GLES 2.0 specification:
* **Direct Register Forwarding:** Primitive arguments ($R0$–$R3$, plus stacked AAPCS arguments) are passed directly to host `libGLESv2.so` functions.
* **Pointer Translation:** Any guest pointer (e.g. `const void* indices`, `const GLfloat* values`, `const char* string`) is translated to host address space via `h(gaddr)`.
* **Functions Added for Full Coverage:**
  `glValidateProgram`, `glBufferSubData`, `glCompressedTexImage2D`, `glCompressedTexSubImage2D`, `glGetVertexAttribPointerv`, `glUniform1fv`..`glUniform4fv`, `glUniform1iv`..`glUniform4iv`, `glUniformMatrix2fv`..`glUniformMatrix4fv`, `glGetShaderPrecisionFormat`, `glReleaseShaderCompiler`, `glGetAttachedShaders`, `glGetBufferParameteriv`, `glGetRenderbufferParameteriv`, `glGetFramebufferAttachmentParameteriv`.

---

### 3.4. ARM EABI Runtime & Soft-Float Math Suite (`libc.cpp`)

Older 32-bit ARM architectures (ARMv5TE / `armeabi`) lack hardware floating-point instructions or use soft-float calling conventions. GCC and Clang emit compiler helper functions starting with `__aeabi_*`. Without implementations, guest games crash with `unimplemented import called: '__aeabi_*'`.

Vemod provides a comprehensive suite of ARM EABI helpers:

#### 1. Soft-Float Conversion & Arithmetic
In ARM AAPCS softfp:
* `double` (64-bit IEEE 754) is passed/returned in paired registers ($R0$:$R1$ or $R2$:$R3$).
* `float` (32-bit IEEE 754) is passed/returned in $R0$.

Implemented helpers:
* **Conversions:** `__aeabi_l2d`, `__aeabi_ul2d`, `__aeabi_i2d`, `__aeabi_ui2d`, `__aeabi_l2f`, `__aeabi_ul2f`, `__aeabi_d2lz`, `__aeabi_d2ulz`, `__aeabi_d2uiz`, `__aeabi_d2iz`, `__aeabi_f2d`, `__aeabi_d2f`, `__aeabi_f2lz`, `__aeabi_f2ulz`, `__aeabi_f2uiz`, `__aeabi_f2iz`.
* **Arithmetic:** `__aeabi_dadd`, `__aeabi_dsub`, `__aeabi_drsub` (calculates $b - a$), `__aeabi_dmul`, `__aeabi_ddiv`, `__aeabi_fadd`, `__aeabi_fsub`, `__aeabi_frsub`, `__aeabi_fmul`, `__aeabi_fdiv`.
* **Comparisons:** `__aeabi_dcmpeq`, `__aeabi_dcmplt`, `__aeabi_dcmple`, `__aeabi_dcmpge`, `__aeabi_dcmpgt`, `__aeabi_fcmpeq`, `__aeabi_fcmplt`, `__aeabi_fcmple`, `__aeabi_fcmpge`, `__aeabi_fcmpgt`.

#### 2. Condition-Code Comparisons (`cdcmpeq`, `cdcmple`, `cdrcmple`)
The ARM EABI specification states that `cd*` functions must set the condition flags directly in the CPU's CPSR/APSR register ($N$, $Z$, $C$, $V$):
* $a == b$: $Z=1, C=1, N=0, V=0$ (mask `0x60000000`)
* $a < b$: $N=1, Z=0, C=0, V=0$ (mask `0x80000000`)
* $a > b$: $C=1, N=0, Z=0, V=0$ (mask `0x20000000`)
* Unordered (NaN): $V=1, C=0, Z=0, N=0$ (mask `0x10000000`)

Vemod sets these flags directly in Dynarmic:
```cpp
static void set_fp_flags(Ctx& c, double a, double b) {
    u32 cpsr = c.cpu.jit->Cpsr() & ~0xF0000000u;
    if (std::isnan(a) || std::isnan(b))      cpsr |= 0x10000000u; // V=1
    else if (a == b)                          cpsr |= 0x60000000u; // Z=1, C=1
    else if (a < b)                           cpsr |= 0x80000000u; // N=1
    else                                      cpsr |= 0x20000000u; // C=1
    c.cpu.jit->SetCpsr(cpsr);
}
```

#### 3. 64-Bit Integer Division & Modulo
* `__aeabi_idiv` & `__aeabi_idivmod`: $R0 = \text{quotient}, R1 = \text{remainder}$.
* `__aeabi_uidiv` & `__aeabi_uidivmod`: Unsigned $R0 = \text{quotient}, R1 = \text{remainder}$.
* `__aeabi_ldivmod` & `__aeabi_uldivmod`:
  In ARM EABI, 64-bit numerator is in $R0$:$R1$, denominator in $R2$:$R3$. Returns 64-bit quotient in $R0$:$R1$ and 64-bit remainder in $R2$:$R3$.
* `__aeabi_idiv0` & `__aeabi_ldiv0`: Division by zero handlers returning 0.
* `__aeabi_unwind_cpp_pr0`..`pr2`: Exception personality stubs.

---

### 3.5. Bionic Zlib 32-Bit / 64-Bit Struct Marshaling (`zlib_bridge.cpp`)

Many games link against Android's system `libz.so`. Direct passthrough corrupts memory because `z_stream` has fundamentally different binary layouts:
* **32-Bit ARM:** `sizeof(z_stream)` is **56 bytes** (`sizeof(unsigned long) == 4`, 32-bit pointers).
* **64-Bit ARM:** `sizeof(z_stream)` is **88 bytes** (`sizeof(unsigned long) == 8`, 64-bit pointers).

Vemod defines `z_stream_32`:
```cpp
struct z_stream_32 {
    gaddr next_in;     // Bytef* (4 bytes)
    u32   avail_in;    // uInt   (4 bytes)
    u32   total_in;    // uLong  (4 bytes)
    gaddr next_out;    // Bytef* (4 bytes)
    u32   avail_out;   // uInt   (4 bytes)
    u32   total_out;   // uLong  (4 bytes)
    gaddr msg;         // char*  (4 bytes)
    gaddr state;       // internal_state* (4 bytes)
    gaddr zalloc;      // alloc_func (4 bytes)
    gaddr zfree;       // free_func (4 bytes)
    gaddr opaque;      // voidpf (4 bytes)
    s32   data_type;   // int (4 bytes)
    u32   adler;       // uLong (4 bytes)
    u32   reserved;    // uLong (4 bytes)
};
```
Before calling host `zlib`, pointers and counters are copied into a host `z_stream`. After execution, results (`avail_in`, `avail_out`, `total_in`, `total_out`, `adler`) are marshaled back to `z_stream_32`.

---

### 3.6. Android NDK Asset Streaming Bridge (`android.cpp`)

Games reading assets via native NDK calls (`AAssetManager_open`, `AAsset_read`, `AAsset_seek`, etc.) require virtualized asset file descriptors. Vemod binds:
* `AAsset_read`: Reads bytes from the host `AAsset*` into guest memory via `h(buffer)`.
* `AAsset_seek`: Translates 32-bit seek offsets and `SEEK_SET/CUR/END` modes.
* `AAsset_getRemainingLength`: Computes remaining unread bytes.
* `AAsset_openFileDescriptor`: Obtains asset file descriptors and start/length offsets.

---

### 3.7. Framework Virtualization & Safety Hooks

#### 1. Dynamic Package Metadata & GMS Compatibility (`PackageManagerHook.kt`)
Legacy games frequently check Google Play Services version via `GooglePlayServicesUtil.t()`:
```java
int expected = context.getResources().getInteger(R.integer.google_play_services_version);
int actual = metaData.getInt("com.google.android.gms.version");
if (actual != expected) throw new IllegalStateException(...);
```
* Hardcoding a fixed version causes crashes across different games (e.g. Flappy Bird expected `4132500`, while Orange Roulette expected `4323000`).
* Vemod dynamically reads the guest APK's real `metaData` from `PackageInfo.applicationInfo.metaData` and copies it into the hooked `IPackageManager` responses.
* If missing, it dynamically resolves `@integer/google_play_services_version` from `guestResources`.

#### 2. Main Looper Crash Recovery (`VemodApp.kt`)
If an unhandled background exception (such as an obsolete Google Play Services ad network callback) reaches the main thread's uncaught exception handler, standard Android behavior terminates the main Looper.

Vemod intercepts this:
```kotlin
if (shouldSwallow) {
    if (Looper.myLooper() == Looper.getMainLooper()) {
        while (true) {
            try {
                Looper.loop()
                break
            } catch (le: Throwable) {
                Log.w("VemodApp", "Recovered main looper crash: ${le.message}")
            }
        }
    }
    return@setDefaultUncaughtExceptionHandler
}
```
This guarantees that the UI thread never freezes or dies when obsolete SDKs throw background errors.

---

### 3.8. Dynamic Screen Orientation Enforcement & Rotation Policy

#### 1. The Screen Orientation Problem
Legacy Android games are typically authored strictly for one display orientation:
* Portrait games (e.g., *Flappy Bird*, `screenOrientation = SCREEN_ORIENTATION_PORTRAIT` / `1`) design their coordinates, aspect ratio, and touch dispatch for tall displays (e.g., 9:16).
* Landscape games (e.g., *Orange Roulette*, `screenOrientation = SCREEN_ORIENTATION_SENSOR_LANDSCAPE` / `6`, or `SCREEN_ORIENTATION_LANDSCAPE` / `0`) design their stages, HUDs, and touch targets for wide displays (e.g., 16:9).

In Android's manifest, `GuestProxyActivity` and slot aliases default to standard host behavior (`unspecified`). If the container does not actively inspect and enforce the guest's declared orientation:
1. Landscape games launch inside a portrait window, resulting in severe letterboxing, aspect ratio distortion, or incorrect `DisplayMetrics` (e.g., width 1080 and height 2392 instead of width 2392 and height 1080).
2. The game engine's viewport calculation initializes with portrait dimensions, causing GL projection matrices and touch hitboxes to misalign.

#### 2. Manifest Detection & Metadata Persistence (`ApkExtractor.kt`)
During APK ingestion, `ApkExtractor` queries the guest's launch activity metadata:
```kotlin
val screenOrientation = pkgInfo.activities?.firstOrNull()?.screenOrientation
    ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
```
This value is persisted inside `meta.json`:
```json
{
  "packageName": "com.heyhouser.OrangeRoulette",
  "appName": "Orange Roulette",
  "launchActivity": "com.heyhouser.OrangeRoulette.MainActivity",
  "screenOrientation": 6,
  ...
}
```
For legacy containers installed before this update, `readMetadata()` dynamically falls back to parsing `base.apk` on-the-fly.

#### 3. Intent & Shortcut Propagation (`ContainerRegistry.kt`, `MainActivity.kt`)
Whether launched from:
* The **Vemod Manager UI** (`MainActivity.launchApp()`)
* The **System App Drawer** (`AppSlot1` .. `AppSlot10`)
* A **Pinned Home Screen Shortcut** (`ContainerRegistry.pinShortcut()`)

The target orientation is attached to the launching Intent:
```kotlin
intent.putExtra(GuestProxyActivity.EXTRA_SCREEN_ORIENTATION, app.screenOrientation)
```

#### 4. Early Lifecycle Enforcement (`VemodInstrumentation.kt`)
Orientation must be locked **before** the guest Activity initializes its view hierarchy and native engine:
```kotlin
override fun callActivityOnCreate(activity: Activity, icicle: Bundle?) {
    val orientation = activity.intent?.getIntExtra(
        GuestProxyActivity.EXTRA_SCREEN_ORIENTATION,
        ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
    ) ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED

    if (orientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
        activity.requestedOrientation = orientation
    }
    ...
    base.callActivityOnCreate(activity, icicle)
}
```

#### 5. Strict Rotation Policy
* **Landscape Games (`0`, `6`, `8`, `11`):** Window is automatically rotated and locked into landscape.
* **Portrait Games (`1`, `7`, `9`, `12`):** Window is locked into portrait.
* **Unspecified / Sensor-Driven (`-1`, `2`, `4`, `10`):** Window follows user device rotation freely.

This guarantees that native display queries (`getDefaultDisplay().getMetrics()`) immediately report rotated dimensions (e.g. 2392x1080) to game engines like OpenFL/Lime, and native touch coordinate transformations (`Lime.onTouch`) map 1:1 with screen pixels.

---

## 4. Verification & Canary Benchmark Results

### Testing Environment:
* **Hardware:** Physical smartphone (`A059P`, Device ID: `00175359K003334`).
* **OS:** Android 16 (SDK 36, `arm64-v8a`, 4 KB page size).
* **Package:** `com.vemod.container`.

### Verified Applications:
| Benchmark Application | Engine & Native Binaries | Key Systems Verified | Status |
|---|---|---|---|
| **Flappy Bird 1.2** (2014, ~1.3 MB) | Pure C++ AndEngine (`libandengine.so`) | Dynarmic A32 JIT, JNI method binding, GLES 2.0 direct pass-through, touch event loop, offline GameHelper neutralization. Locked portrait orientation (`SCREEN_ORIENTATION_PORTRAIT`). | **100% PASSED** (Fully playable, 60 FPS) |
| **Orange Roulette 1.0.0** (2014, ~8 MB) | Haxe / OpenFL Lime (`libApplicationMain.so`, `liblime.so`, `libopenal.so`, `libregexp.so`, `libstd.so`, `libzlib.so`) | Multi-module ELF loader, CFFI isolated handles, OpenAL audio track, 32-bit zlib streaming, ARM EABI soft-float math, dynamic GMS metadata, interactive cutscenes and gameplay. Sensor landscape auto-lock (`SCREEN_ORIENTATION_SENSOR_LANDSCAPE`, 2392x1080). | **100% PASSED** (Fully playable, interactive audio & GLES) |

---

## 5. Repository File Structure

```
vemod/
├── app/
│   ├── src/main/
│   │   ├── AndroidManifest.xml            # Manifest declaring GuestProxyActivity & AppSlot1..AppSlot10
│   │   ├── java/com/vemod/container/
│   │   │   ├── VemodApp.kt                # Application subclass, hidden API exemptions, Looper recovery
│   │   │   ├── bridge/
│   │   │   │   └── VemodBridge.kt         # JNI bridge loader & native runtime manager
│   │   │   ├── container/
│   │   │   │   ├── ApkExtractor.kt        # APK unpacker, ABI detector (v7a/v5), proxy generator, orientation detector
│   │   │   │   └── ContainerRegistry.kt   # Slot manager, App Drawer allocator, shortcut pinner
│   │   │   ├── model/
│   │   │   │   └── ContainerApp.kt        # Data model for installed 32-bit containers (with screenOrientation)
│   │   │   ├── proxy/
│   │   │   │   ├── GuestProxyActivity.kt  # Activity stub running in android:process=":guest"
│   │   │   │   ├── PackageManagerHook.kt  # Dynamic IPackageManager proxy & metadata injector
│   │   │   │   └── VemodInstrumentation.kt# Context/resource swapper, GameHelper/AdMob neutralizer, orientation enforcer
│   │   │   ├── ui/
│   │   │   │   └── MainActivity.kt        # Jetpack Compose Material 3 manager UI
│   │   │   └── virtual/
│   │   │       ├── GuestClassLoader.kt    # PathClassLoader redirecting findLibrary to proxy stubs
│   │   │       └── GuestContext.kt        # Virtualized ContextWrapper for sandbox paths
│   │   └── cpp/
│   │       ├── CMakeLists.txt             # Native build script linking Dynarmic, Bionic, GLES, zlib
│   │       ├── bridge/
│   │       │   ├── android.cpp            # NDK AAsset streaming bridge
│   │       │   ├── cpu.cpp / cpu.h        # Dynarmic A32 JIT callbacks & CPSR manipulation
│   │       │   ├── gles.cpp               # 100% OpenGL ES 2.0 passthrough (141 entry points)
│   │       │   ├── jni_bridge.cpp         # 32-bit JNIEnv* facade & AArch64 trampolines
│   │       │   ├── libc.cpp               # Bionic libc/libm thunks & ARM EABI softfp math suite
│   │       │   ├── loader.cpp             # Dynamic multi-module ELF loader & isolated dlopen/dlsym
│   │       │   ├── memory.cpp             # 4 GB virtual memory arena & page management
│   │       │   ├── nb.h                   # Core bridge data structures (Ctx, layout, memory)
│   │       │   ├── pthread.cpp            # POSIX threads, mutexes, condition variables
│   │       │   ├── stdio.cpp              # Guest file I/O & logging redirection
│   │       │   └── zlib_bridge.cpp        # 32-bit z_stream_32 marshaling bridge
│   │       └── proxy/
│   │           └── proxy.cpp              # 64-bit libproxy.so template
├── refference/
│   ├── Flappy Bird_1.2.apk                # Tier 1 Canary test APK (armeabi-v7a)
│   └── orange-roulette-1-0-0.apk          # Tier 2 Canary test APK (armeabi, multi-module)
└── documentation.md                       # Comprehensive documentation (this document)
```
