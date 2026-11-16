# Vemod: 32-Bit Android APK Container for 64-Bit-Only Android Systems
**Comprehensive Architecture, Implementation, and Troubleshooting Documentation**
*App Version: 0.5.0 (versionCode: 50, versionName: "0.5.0")*

---

## 1. Executive Summary & Problem Space

Modern Android smartphones powered by 64-bit-only SoCs (such as Qualcomm Snapdragon 8 Gen 3, Google Tensor G3/G4, and MediaTek Dimensity 9300+) running Android 14, 15, and 16 have dropped hardware and firmware support for 32-bit ARM binaries (`armeabi-v7a` and `armeabi`). The CPU cores no longer support the AArch32 EL0 execution state, and 64-bit Bionic linkers strictly reject 32-bit ELF binaries.

Consequently, tens of thousands of classic Android apps, indie games, and historical software built prior to Google Play's 64-bit requirement in August 2019 are permanently unplayable on modern hardware.

**Vemod** (`com.psychostark.vemod`) is an advanced, generalized 32-bit Android container and runtime virtualization layer. It executes arbitrary 32-bit ARM native code (`.so` files) on 64-bit-only (`arm64-v8a`) hardware through an embedded **Dynarmic ARM32 Just-In-Time (JIT)** CPU recompiler, while transparently virtualizing the Android Dalvik/ART runtime, file system, resources, JNI bridge, Bionic libc/zlib, OpenSL ES audio, and OpenGL ES 2.0 / 1.1 graphics pipeline.

Vemod allows users to install **multiple 32-bit APKs simultaneously**, launch them directly from the **Android system App Drawer** using signed micro-stub companion packages (`com.vemod.container_<packagename>`), and **pin 1x1 shortcuts to the system Home Screen**.

---

## 2. Architecture Overview

Rather than attempting full OS-level hardware virtualization (which is slow, heavyweight, and complex), Vemod runs as a lightweight userspace compatibility layer:
* **Java / Kotlin Bytecode:** Runs **natively at full speed** on the host's 64-bit ART Virtual Machine.
* **Native 32-bit Machine Code:** Intercepted and recompiled on-the-fly via an in-process **Dynarmic A32 JIT engine**.
* **Graphics & Audio:** OpenGL ES 2.0 / 1.1 and OpenSL ES / AudioTrack pass directly to the host GPU and audio flinger.

```
+-----------------------------------------------------------------------------------------+
|                                    HOST SYSTEM UI                                       |
|  - System App Drawer (Micro-Stub Companion Packages com.vemod.container_<pkg>)         |
|  - System Home Screen (1x1 Pinned Shortcuts via ShortcutManagerCompat)                  |
|  - Vemod Manager UI (MainActivity: Jetpack Compose Material 3)                          |
+-----------------------------------------------------------------------------------------+
                                             |
                                             v
+-----------------------------------------------------------------------------------------+
|                    ISOLATED GUEST PROCESS POOL (:guest1 .. :guest5)                     |
|                                                                                         |
|  +-----------------------------------------------------------------------------------+  |
|  | VemodInstrumentation & Context Virtualization Layer                              |  |
|  | - Instantiates Guest Activity via reflection                                      |  |
|  | - Swaps Context mBase with GuestContext (virtual data/cache/obb paths)            |  |
|  | - Swaps AssetManager & Resources with Guest APK Assets                            |  |
|  | - Hooks IPackageManager (dynamic manifest meta-data & package resolution)         |  |
|  | - Neutralizes GMS / AdMob crashes & recovers main Looper                          |  |
|  | - TouchPacer: paces sub-60ms ACTION_UP to fix high-refresh-rate tap handling       |  |
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
|  | │ Dynarmic ARM32 JIT Engine (Cycle-guarded IR, UserCallbacks, SVC dispatch)   │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Hybrid Guest Bionic RootFS (AOSP Nougat 7.0 libc/libm/libc++/libdl/libz)    │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Kernel Syscall Disptacher (do_syscall: memory, futex, sched, timerfd, etc)  │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Dynamic Multi-Module ELF Loader (g_modules, isolated dlopen/dlsym handles)  │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ OpenGL ES 2.0 & 1.1 Passthrough (141 GLES2 + GLES1 fixed-point entry points)│  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ OpenSL ES 1.0.1 Audio Subsystem Bridge (Dynamic SL_IID_* .bss resolution)   │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ ARM EABI Runtime & Soft-Float Math Suite (__aeabi_l2d, __aeabi_ldivmod, etc)│  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ Android NDK Asset Streaming Bridge (AAsset_read, AAsset_seek, etc)          │  |  |
|  | ├──────────────────────────────────────────────────────────────────────────────┤  |  |
|  | │ 32-bit JNIEnv* Facade & AArch64 Native Trampolines                          │  |  |
|  | └──────────────────────────────────────────────────────────────────────────────┘  |  |
|  +-----------------------------------------------------------------------------------+  |
+-----------------------------------------------------------------------------------------+
```

---

## 3. Core Subsystems in Detail

### 3.1. Multi-APK Management & System Integration

#### 1. Dynamic Ingestion & Extraction (`ApkExtractor.kt`)
When the user picks an APK:
1. `PackageManager.getPackageArchiveInfo(apkPath, GET_ACTIVITIES | GET_META_DATA)` parses the guest package name, version, and launch activity.
2. The APK is copied into `/data/user/0/com.psychostark.vemod/files/containers/<guest_pkg>/base.apk` and marked read-only to comply with Android 14+ ART execution policies.
3. The native libraries inside the APK are inspected. If `lib/armeabi-v7a/` exists, it is selected; otherwise `lib/armeabi/` is selected.
4. Each `.so` file is extracted into `lib32/`.
5. For every extracted 32-bit library, a copy of the host's 64-bit `libproxy.so` template is written into `proxy/lib<name>.so`.
6. High-resolution app icons are extracted and cached as `icon.png`.
7. Metadata is persisted in `meta.json`.

#### 2. Micro-Stub Companion APK Generator (`StubApkGenerator.kt`)
Earlier designs relied on static slot aliases (`AppSlot1`..`AppSlot10`), which were limited in quantity and forced to use static compile-time labels. Vemod now implements dynamic **Micro-Stub Companion APK generation**:
* **On-the-Fly APK Assembly:** When a 32-bit app is imported (or the user taps `+ App Drawer`), Vemod synthesizes a microscopic (~25 KB) standalone 64-bit APK.
* **Authentic Branding & Theming:**
  - In `AndroidManifest.xml` (AXML): Dynamically rewrites the binary String Pool so `android:label` contains the guest's authentic name (e.g. "Flappy Bird", "Orange Roulette", "Angry Birds Space") and `package` is set to `com.vemod.container_<guestPackageName>` targeting Android 15/16 SDK 35 (`minSdkVersion=24`, `targetSdkVersion=35`). Configured with `android:theme="@style/Theme.Stub"`, `android:icon="@drawable/ic_launcher"`, and `android:excludeFromRecents="true"`.
  - In `resources.arsc`: Updates the 256-byte package chunk to match `com.vemod.container_<guestPackageName>`.
  - Embedded Resources: Embeds `res/drawable/ic_launcher.png` containing the extracted game icon, paired with a custom `Theme.Stub` (`windowSplashScreenBackground = #121212`, `windowSplashScreenAnimatedIcon = @drawable/ic_launcher`).
* **PackageInstaller Session API:** Installs silently with user confirmation via Android's `PackageInstaller` commit flow, managed by `StubInstallReceiver`.
* **Zero Ghost Tasks in Recents:** Setting `excludeFromRecents="true"` and `noHistory="true"` on the stub ensures the stub activity terminates immediately after routing without lingering in the Recent Apps switcher.

#### 3. Home Screen Shortcut Pinning
* `ContainerRegistry.pinShortcut()` uses `androidx.core.content.pm.ShortcutManagerCompat.requestPinShortcut()`.
* It builds a `ShortcutInfoCompat` featuring the guest app's high-res icon and an intent pointing to `GuestProxyActivity` with `FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TOP`.
* Tested and verified on modern vendor launchers (including Nothing OS Launcher).

#### 4. Clean Process Lifecycle & App Switching
Because each 32-bit app loads distinct native libraries into the 4 GB Dynarmic memory arena, switching between different 32-bit apps in `:guest` requires a clean container state. `MainActivity.launchApp()` checks if a `:guest` process is active with another app and cleanly restarts the process before launching.

---

### 3.2. Concurrent Multi-Process Container Engine (`GuestProcessManager.kt`)

Running multiple 32-bit guest apps concurrently requires complete native isolation:
* **Dynarmic Memory Arena Independence:** Each Dynarmic JIT recompiler maps a 4 GB virtual address space (`0x100000000ULL`) to emulate 32-bit pointers. Attempting to load multiple guest apps within the same process leads to memory arena conflicts, Bionic global state collisions, and GLES context interleaving.
* **5 Dedicated Isolated Processes:** Vemod declares 5 distinct Linux processes in `AndroidManifest.xml`:
  - `GuestProxyActivity1` running in `android:process=":guest1"` with `taskAffinity="com.vemod.container.task1"`
  - `GuestProxyActivity2` running in `android:process=":guest2"` with `taskAffinity="com.vemod.container.task2"`
  - `GuestProxyActivity3` running in `android:process=":guest3"` with `taskAffinity="com.vemod.container.task3"`
  - `GuestProxyActivity4` running in `android:process=":guest4"` with `taskAffinity="com.vemod.container.task4"`
  - `GuestProxyActivity5` running in `android:process=":guest5"` with `taskAffinity="com.vemod.container.task5"`
* **Dynamic Slot Allocator:** `GuestProcessManager.getTargetProxyClass()` inspects running processes via `ActivityManager.getRunningAppProcesses()`. It automatically reuses an already active process for the same package, or selects the next idle slot (`:guest1`..`:guest5`), enabling seamless multitasking between multiple games at 60 FPS without process churn.

---

### 3.3. System StartingWindow Suppression & Dynamic Splash Overlay

On Android 12+ (API 31+ through Android 16), `system_server` renders an OS-level `StartingWindow` before the target app process initializes:
* **The Blueprint Robot Problem:** Because the container host APK (`com.psychostark.vemod`) cannot know in advance which guest app is launching, an opaque theme causes WindowManager to render the framework's fallback `sym_def_app_icon` (the blueprint grid circle with an Android robot head).
* **Two-Stage Splash Architecture:**
  1. **Stub Launch Phase:** When launched from the system launcher/app drawer, the stub APK's `Theme.Stub` renders the authentic game logo centered on `#121212`.
  2. **Container Handoff Phase:** Both `Theme.Vemod.Router` and `Theme.Vemod.Guest` use `android:windowIsTranslucent="true"` and `android:windowDisablePreview="true"`. This instructs WindowManager to emit `STARTING_WINDOW_TYPE_NONE` (type 0), completely suppressing any secondary system splash screen.
  3. **In-Process Dynamic Overlay (`showSplashOverlay`):** In `VemodInstrumentation.callActivityOnCreate()`, a dynamic overlay view is attached displaying the app's real `icon.png` over `#121212`. When the native game engine or GLSurfaceView begins rendering, this overlay fades out smoothly over 400ms.
* **Task Description & Recents Thumbnail:** `activity.setTaskDescription()` is dynamically invoked with the guest's authentic title and bitmap icon, and the window background is made solid `#121212`. In Android's Recent Apps switcher, every running game appears with its authentic title, icon, and preview thumbnail.

---

### 3.4. Native Back Key Dispatching & Navigation Decoupling

* **Native Back Gesture Forwarding:** Android 13+ introduced predictive back gestures (`OnBackInvokedDispatcher`). Setting `android:enableOnBackInvokedCallback="true"` in the application manifest allows `VemodInstrumentation` to register an `OnBackInvokedCallback`. When the user triggers the back button or swipe gesture:
  - Synthetic `KeyEvent(ACTION_DOWN, KEYCODE_BACK)` and `KeyEvent(ACTION_UP, KEYCODE_BACK)` events are dispatched directly to `activity.dispatchKeyEvent()`.
  - Game engines (such as OpenFL/Lime `MainView.onKeyDown` in *Orange Roulette* or AndEngine in *Flappy Bird*) consume the event to handle in-game menus, pause screens, or level navigation.
* **Decoupled Exit Navigation:** If the game does not consume the back key (or user presses Back on the game's root screen to exit):
  - `activity.finishAndRemoveTask()` cleanly closes that app's specific task affinity.
  - The system returns directly to the phone's Home Screen or App Drawer, NEVER bouncing to Vemod's `MainActivity`.

---

### 3.5. Dynamic Multi-Module ELF & Linker Virtualization (`loader.cpp`)

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

### 3.6. Complete OpenGL ES 2.0 & 1.1 Passthrough Bridge (`gles.cpp`)

Modern Android games require complete coverage of OpenGL ES 2.0 and early 3D fixed-function GLES 1.1 entry points:
* **OpenGL ES 2.0 Full Coverage (141 functions):** Direct register forwarding and pointer translation via `h(gaddr)`. Includes `glValidateProgram`, `glBufferSubData`, `glCompressedTexImage2D`, `glCompressedTexSubImage2D`, `glGetVertexAttribPointerv`, `glUniform*`, `glReleaseShaderCompiler`, `glGetAttachedShaders`, `glGetBufferParameteriv`, etc.
* **OpenGL ES 1.1 Fixed-Function & Fixed-Point Pipeline:**
  - `glTexEnvx(target, pname, param)`: Earlier bridges routed fixed-point `glTexEnvx` to integer `glTexEnvi`. For texture environment parameters where fixed-point $1.0 = 0x10000$ was passed, `glTexEnvi` interpreted this as an invalid enum, disabling texture modulation and causing pure white, untextured models (as observed in *Reckless Racing Lite*). Direct pass-through to native `glTexEnvx` fixed this.
  - Comprehensive support for `glAlphaFunc`, `glAlphaFuncx`, `glBlendFunc`, `glColorPointer`, `glVertexPointer`, `glTexCoordPointer`, `glNormalPointer`, `glMatrixMode`, `glLoadIdentity`, `glPushMatrix`, `glPopMatrix`, `glLight*`, `glMaterial*`.

---

### 3.7. ARM EABI Runtime & Soft-Float Math Suite (`libc.cpp`)

Older 32-bit ARM architectures (ARMv5TE / `armeabi`) lack hardware floating-point instructions or use soft-float calling conventions. GCC and Clang emit compiler helper functions starting with `__aeabi_*`. Without implementations, guest games crash with `unimplemented import called: '__aeabi_*'`.

Vemod provides a comprehensive suite of ARM EABI helpers:
* **Conversions:** `__aeabi_l2d`, `__aeabi_ul2d`, `__aeabi_i2d`, `__aeabi_ui2d`, `__aeabi_l2f`, `__aeabi_ul2f`, `__aeabi_d2lz`, `__aeabi_d2ulz`, `__aeabi_d2uiz`, `__aeabi_d2iz`, `__aeabi_f2d`, `__aeabi_d2f`, `__aeabi_f2lz`, `__aeabi_f2ulz`, `__aeabi_f2uiz`, `__aeabi_f2iz`.
* **Arithmetic:** `__aeabi_dadd`, `__aeabi_dsub`, `__aeabi_drsub` ($b - a$), `__aeabi_dmul`, `__aeabi_ddiv`, `__aeabi_fadd`, `__aeabi_fsub`, `__aeabi_frsub`, `__aeabi_fmul`, `__aeabi_fdiv`.
* **Condition-Code Comparisons (`cdcmpeq`, `cdcmple`, `cdrcmple`):** Sets condition flags directly in Dynarmic's CPSR/APSR register ($N, Z, C, V$):
  - $a == b$: $Z=1, C=1, N=0, V=0$ (mask `0x60000000`)
  - $a < b$: $N=1, Z=0, C=0, V=0$ (mask `0x80000000`)
  - $a > b$: $C=1, N=0, Z=0, V=0$ (mask `0x20000000`)
  - Unordered (NaN): $V=1, C=0, Z=0, N=0$ (mask `0x10000000`)
* **64-Bit Integer Division & Modulo:** `__aeabi_idiv`, `__aeabi_idivmod`, `__aeabi_uidiv`, `__aeabi_uidivmod`, `__aeabi_ldivmod`, `__aeabi_uldivmod`.

---

### 3.8. Bionic Zlib 32-Bit / 64-Bit Struct Marshaling (`zlib_bridge.cpp`)

Many games link against Android's system `libz.so`. Direct passthrough corrupts memory because `z_stream` has fundamentally different binary layouts:
* **32-Bit ARM:** `sizeof(z_stream)` is **56 bytes** (`sizeof(unsigned long) == 4`, 32-bit pointers).
* **64-Bit ARM:** `sizeof(z_stream)` is **88 bytes** (`sizeof(unsigned long) == 8`, 64-bit pointers).

Vemod defines `z_stream_32` with 32-bit pointers and counters. Before calling host `zlib`, pointers and counters are copied into a host `z_stream`. After execution, results (`avail_in`, `avail_out`, `total_in`, `total_out`, `adler`) are marshaled back to `z_stream_32`.

---

### 3.9. Android NDK Asset Streaming Bridge (`android.cpp`)

Games reading assets via native NDK calls (`AAssetManager_open`, `AAsset_read`, `AAsset_seek`, etc.) require virtualized asset file descriptors. Vemod binds:
* `AAsset_read`: Reads bytes from the host `AAsset*` into guest memory via `h(buffer)`.
* `AAsset_seek`: Translates 32-bit seek offsets and `SEEK_SET/CUR/END` modes.
* `AAsset_getRemainingLength`: Computes remaining unread bytes.
* `AAsset_openFileDescriptor`: Obtains asset file descriptors and start/length offsets.

---

### 3.10. Framework Virtualization, Safety Hooks & Looper Recovery

#### 1. Dynamic Package Metadata & GMS Compatibility (`PackageManagerHook.kt`)
Legacy games frequently check Google Play Services version via `GooglePlayServicesUtil.t()`:
* Hardcoding a fixed version causes crashes across different games (e.g. Flappy Bird expected `4132500`, while Orange Roulette expected `4323000`).
* Vemod dynamically reads the guest APK's real `metaData` from `PackageInfo.applicationInfo.metaData` and copies it into the hooked `IPackageManager` responses.
* If missing, it dynamically resolves `@integer/google_play_services_version` from `guestResources`.

#### 2. Main Looper Crash Recovery (`VemodApp.kt`)
If an unhandled background exception (such as an obsolete Google Play Services ad network callback) reaches the main thread's uncaught exception handler, standard Android behavior terminates the main Looper.
Vemod intercepts this via `Looper.loop()` recovery in `Thread.setDefaultUncaughtExceptionHandler`, guaranteeing that the UI thread never freezes or dies when obsolete SDKs throw background errors.

---

### 3.11. Dynamic Screen Orientation Enforcement & Rotation Policy

1. **Manifest Detection & Metadata Persistence (`ApkExtractor.kt`):** Queries `pkgInfo.activities?.firstOrNull()?.screenOrientation` and stores it in `meta.json`.
2. **Intent & Shortcut Propagation:** Transmits `EXTRA_SCREEN_ORIENTATION` via launcher shortcuts and `MainActivity`.
3. **Early Lifecycle Enforcement (`VemodInstrumentation.kt`):** Invokes `activity.requestedOrientation = orientation` inside `callActivityOnCreate()` **before** view inflation and native engine initialization.
4. **Strict Rotation Policy:**
   - Landscape Games (`0`, `6`, `8`, `11`): Window is automatically rotated and locked into landscape.
   - Portrait Games (`1`, `7`, `9`, `12`): Window is locked into portrait.
   - Unspecified / Sensor-Driven (`-1`, `2`, `4`, `10`): Window follows user device rotation freely.

---

### 3.12. "Expose App Data" Storage Virtualization Architecture

Modern Android versions (Android 11 through 16) enforce Scoped Storage restrictions, blocking access to `/sdcard/Android/data` and `/sdcard/Android/obb`.
Vemod provides the **"Expose App Data"** engine:
* **Storage Location:** Mapped to `internal storage/vemod/<app_name>/data/` and `obb/`.
* **Name Collision Disambiguation:** Automatically disambiguates duplicate titles: `vemod/<app_name> (<package_name>)`.
* **Bidirectional Migration:** Toggling "Expose App Data" migrates files bidirectionally with timestamps between private internal storage (`/data/user/0/com.psychostark.vemod/files/containers/<pkg>/`) and public storage (`/sdcard/vemod/...`).
* **Runtime Redirection in `GuestContext`:** Redirects `getDataDir()`, `getFilesDir()`, `getCacheDir()`, `getObbDir()`, `getExternalFilesDir()`, and `getDatabasePath()` directly into `/sdcard/vemod/<app_name>/` in real-time.

---

### 3.13. High-Refresh-Rate Touch Event Pacing (`TouchPacer.kt`)

On modern Android devices featuring 120Hz or 240Hz touch digitizers and displays, touch event timing can differ significantly from legacy 60Hz devices:
* **The Problem:** In games using decoupled render threads (e.g., Rovio Fusion in *Angry Birds Space* and *Angry Birds Rio*), sub-30ms taps batch `ACTION_DOWN` and `ACTION_UP` into the same render frame. Button state machines transition to `PRESSED` on `TouchBegan`, but `TouchEnded` in the same tick fails to register as a completed tap, requiring a second tap to trigger.
* **The Solution:** `TouchPacer` is installed via a transparent `Window.Callback` proxy. If `ACTION_UP` occurs within 60ms of `ACTION_DOWN`, it schedules `ACTION_UP` on the main looper for `60ms - elapsed`. This guarantees events span distinct render frames, restoring instant single-tap responsiveness.

---

### 3.14. Hybrid Guest Bionic RootFS Architecture (Nougat 7.0 / API 24)

#### 1. Problem with Emulated Libc Thunks
Complex C++ game engines use internal `std::string`, `std::vector`, `std::iostream`, jemalloc metadata, and custom exception unwinding (`_Unwind_*`, `__cxa_*`) that assume byte-for-byte fidelity with 32-bit Android Bionic structures. Incomplete struct layouts in host thunks caused subtle heap corruptions, thread-local storage misalignment, and unwinding crashes.

#### 2. Hybrid Sysroot Engine
Vemod loads official Android Nougat 7.0 AOSP (`armeabi-v7a`, API 24) system libraries (`libc.so`, `libm.so`, `libstdc++.so`, `libz.so`, `libc++.so`, `liblog.so`, `libcutils.so`) directly into the Dynarmic 32-bit guest address space:
* **Bionic TLS & Thread State:**
  - TLS slots 0 through 5 are initialized per thread according to Bionic conventions (`kSlotSelf`, `kSlotThreadId`, `kSlotErrno`, `kSlotOpenGLApi`, `kSlotOpenGL`, `kSlotStackGuard`).
  - Circular `pthread_internal_t` structure placed at `tls + 0x20` with guest tid at offset `0x08`, satisfying Bionic `pthread_self()` and `gettid()`.
  - `KernelArgumentBlock` with `AT_PAGESZ=4096`, `AT_RANDOM`, `AT_SYSINFO_EHDR`, `argv`, and `envp` placed at `tls[kSlotOpenGLApi]`, satisfying `__libc_preinit` and jemalloc initialization.
* **Packed Relocations (`APS2` / `DT_ANDROID_REL`):** Built-in SLEB128 decoder in `loader.cpp` unpacks delta-encoded relocation groups (`RELOCATION_GROUPED_BY_INFO_FLAG`, `RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG`, `RELOCATION_GROUPED_BY_ADDEND_FLAG`), resolving all 2,199 symbols in `libc++.so`.
* **Dual-Layer Passthrough:** Pure OS & C++ runtime calls route directly through guest Bionic; hardware-dependent APIs (`gl*`, `egl*`), NDK asset streaming (`AAsset*`), logcat, and OpenSL ES route to host bridges.

---

### 3.15. Dynarmic ARM32 JIT Robustness: Infinite IR Identity Recursion & Cycle Guards

#### 1. The Dynamic Recompilation Cycle Problem
In heavy games (such as *Spider-Man Unlimited*, *Angry Birds Space*, *Angry Birds Stella*, and *Orange Roulette*), dynamic JIT recompilation with aggressive optimization passes (`constant_propagation_pass`, `reg_alloc`, phi-node elimination) occasionally generated circular `Identity` chains:
```
Inst #1: Identity(Inst #2)
Inst #2: Identity(Inst #1)
```
When Dynarmic's IR inspection methods (`IsImmediate()`, `GetType()`, `GetInstRecursive()`, `GetU32()`, etc.) traversed these chains recursively without bounds, the call stack overflowed on the host AArch64 thread, triggering `Fatal signal 11 SIGSEGV SEGV_ACCERR` at frame 512.

#### 2. Safe Iterative Traversal & Self-Replacement Guards
1. **Iterative Identity Resolution (`value.cpp`):**
   ```cpp
   static Value ResolveIdentity(Value val) {
       size_t depth = 0;
       while (val.IsIdentity() && depth < 32) {
           val = val.GetInst()->GetArg(0);
           depth++;
       }
       return val;
   }
   ```
   Replaced all recursive methods with bounded iterative traversal.
2. **Cycle-Safe Replacement (`microinstruction.cpp`):**
   In `Inst::ReplaceUsesWith(Value replacement)`, added cycle detection resolving identity chains to guarantee an instruction never replaces uses with itself or an instruction in its own identity ancestry.
3. **Bounded Pass Unrolling (`identity_removal_pass.cpp`):** Bounded `while (true)` unrolling loops to maximum depth 32.

This completely eliminated dynamic recompilation segmentation faults across all titles.

---

### 3.16. Complete ARM32 Linux Syscall Emulation & Non-Root Real-Time Scheduling (`cpu.cpp`)

When guest Bionic issues `SVC #0`, Dynarmic dispatches to `do_syscall`. Vemod provides a comprehensive Linux kernel surface:
* **Memory Management:** `brk` (guest heap resizing via `nb::brk`), `mmap2`, `munmap`, `mprotect`, `madvise`.
* **File I/O & Storage:** `open`, `openat`, `close`, `read`, `write`, `readv`, `writev`, `lseek`, `_llseek`, `ioctl`, `fcntl64`, `access`, `getcwd`, `stat64`, `lstat64`, `fstat64`, `fstatat64`, `statfs64`, `fstatfs64`, `fallocate` (syscall 324).
* **Timers & Clocks:** `times` (43), `setitimer` (104), `getitimer` (105), `clock_gettime` (263), `clock_getres` (264), `nanosleep` (162), `clock_nanosleep` (267), `timerfd_create` (350), `timerfd_settime` (353), `timerfd_gettime` (354).
* **Threading & Synchronization:** `futex` (`FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_REQUEUE`), `set_tid_address`, `clone` (backed by host `std::thread`), `sched_yield`, `tgkill`, `tkill`, `exit`, `exit_group`.
* **Process & Thread Scheduling Compatibility (`sched_*`):**
  - Games like *Lane Racer* and Unity engine titles query and set thread priorities (`sched_setscheduler` 156, `sched_setparam` 154, `sched_getparam` 155, `sched_getscheduler` 157, `sched_get_priority_max` 159, `sched_get_priority_min` 160, `sched_setaffinity` 241).
  - On non-root Android hosts, unprivileged processes cannot acquire `SCHED_FIFO` or `SCHED_RR` real-time scheduling. Calling host `sched_setscheduler` returns `-EPERM` or `-EACCES`, causing Unity and Bionic to spin or abort with ANRs.
  - **The Fix:** In `cpu.cpp`, if `sched_setscheduler`, `sched_setparam`, or `sched_setaffinity` returns `-EPERM` or `-EACCES`, Vemod returns `0` (success). This fakes real-time privilege acquisition, allowing game engine threads to proceed smoothly.
* **ARM Private Operations:** `ARM_NR_cacheflush` (0xf0002), `ARM_NR_set_tls` (0xf0005, updates Dynarmic TPIDRURO / CP15 c13), `ARM_NR_breakpoint` (0xf0001).

---

### 3.17. Native OpenSL ES 1.0.1 Audio Subsystem Bridge (`opensles_bridge.cpp`)

Many 32-bit Android titles (*Spider-Man Unlimited*, *Lane Racer*, Gameloft and Unity titles) rely on the NDK OpenSL ES C API (`<SLES/OpenSLES.h>`, `<SLES/OpenSLES_Android.h>`) for low-latency native audio.

#### 1. The Prebuilt Sysroot `.bss` Zeroed Symbol Problem
In prebuilt 32-bit sysroot libraries (`libOpenSLES.so`), standard interface IDs (`SL_IID_ENGINE`, `SL_IID_PLAY`, `SL_IID_BUFFERQUEUE`, `SL_IID_ANDROIDSIMPLEBUFFERQUEUE`, `SL_IID_VOLUME`, `SL_IID_SEEK`, etc.) were declared in `.bss` and contained all zeroes (`NULL`). When guest games invoked `(*engine)->GetInterface(engine, SL_IID_*, ...)`, they passed a pointer containing NULL, causing audio initialization to fail across all OpenSL ES titles.

#### 2. Dynamic UUID Struct Allocation & Multi-Indirection Resolution
* **Dynamic IID Initialization (`init_opensles_iids`):** On sysroot preload in `loader.cpp`, Vemod allocates 16-byte UUID structs in guest memory for all standard OpenSL ES interfaces and updates the symbol locations in `libOpenSLES.so` via `guest_dlsym(0, name)`.
* **Double Dereferencing Support (`match_iid`):** OpenSL ES defines `SLInterfaceID` as `const struct SLInterfaceID_ * const *` (pointer-to-pointer). `match_iid()` checks direct UUID equality, single dereferencing, and double dereferencing to reliably identify interface queries.
* **Interface Implementations:**
  - `SLObjectItf`: Realize, Resume, GetState, GetInterface, RegisterCallback, Abort, Destroy.
  - `SLEngineItf`: CreateAudioPlayer (PCM buffer queues, URI, and asset data sources), CreateOutputMix, CreateAudioRecorder.
  - `SLPlayItf`: SetPlayState (`SL_PLAYSTATE_PLAYING`, `SL_PLAYSTATE_PAUSED`, `SL_PLAYSTATE_STOPPED`), GetPlayState, GetDuration, GetPosition.
  - `SLVolumeItf`: SetVolumeLevels, GetVolumeLevels, SetMute, GetMute.
  - `SLAndroidSimpleBufferQueueItf`: Enqueue, Clear, GetState, RegisterCallback. Invokes guest audio streaming callbacks from the audio thread.
  - `SLSeekItf`, `SLPlaybackRateItf`, `SLPrefetchStatusItf`: Fully stubbed and bridged to satisfy game audio managers.

---

### 3.18. Android AudioTrack Virtualization & AttributionSource Forwarding (`GuestContext.kt`)

In Android 12+ (SDK 31 through Android 16), `system_server`'s `AudioService` and `AudioFlinger` strictly validate UID ownership for `AudioTrack`, `MediaPlayer`, and `SoundPool`:
* **The Package UID Rejection Problem:** When a guest game creates an `AudioTrack` or `SoundPool`, the framework passes `context.getPackageName()` to `AudioService`. Because the guest package name (e.g. `com.polarbit.RecklessRacingLite` or `com.imangi.templerun`) does not match the host's Linux UID (`10618`), `AudioService` rejects playback with `SecurityException` or silences the track.
* **Stack Inspection in `GuestContext.getPackageName()`:**
  ```kotlin
  override fun getPackageName(): String {
      val stack = Thread.currentThread().stackTrace
      for (i in 2 until minOf(stack.size, 10)) {
          val cls = stack[i].className
          if (cls.startsWith("android.media.") ||
              cls.startsWith("android.app.AppOpsManager") ||
              cls.startsWith("android.view.") ||
              cls.startsWith("android.os.Vibrator") ||
              cls.startsWith("android.os.SystemVibrator")) {
              return baseContext.packageName
          }
      }
      return guestPackageName
  }
  ```
* **AttributionSource Forwarding:** On Android 12+ (API 31+), `GuestContext.getAttributionSource()` returns `baseContext.attributionSource`. This passes framework permissions checks cleanly, restoring full sound effects and background music in *Reckless Racing*, *Temple Run*, *Lane Racer*, and *Spider-Man Unlimited*.

---

## 4. Maximum 32-Bit Compatibility: The Five Architectural Pillars

To guarantee compatibility across arbitrary 32-bit Android binaries without requiring individual per-app patches, Vemod implements five core pillars:

### Pillar 1: Deterministic Historical Bionic ABI & Kernel Syscall Surface
* **104-Byte `struct stat` & `stat64`:** Converts host filesystem metadata directly into 32-bit Bionic layouts (`st_size` at offset 48, `st_ino` at offset 96).
* **44-Byte `struct tm` Time Serialization:** Marshals `localtime`, `gmtime`, `mktime`, and `strftime` to and from historical 44-byte layouts.
* **16-Byte `struct sigaction` Stack Frame Preservation:** Strictly writes at most 16 bytes for `oldact` in `sigaction` and 4 bytes for `oldset` in `sigprocmask`, preventing stack frame clobbering.
* **Complete Linux Syscall Surface:** Direct handling of `brk`, `mmap2`, `futex`, `timerfd_*`, `times`, `itimer`, and safe real-time scheduling emulation (`sched_*`).

### Pillar 2: Universal High-Refresh-Rate Touch Pacing (`TouchPacer.kt`)
Paces sub-60ms `ACTION_UP` events across separate render ticks on 90Hz, 120Hz, and 240Hz screens, ensuring legacy button state machines register single-tap clicks without engine-specific modifications.

### Pillar 3: Automated Defunct SDK & GMS Neutralizer (`AdSdkNeutralizer.kt`)
Neutralizes permanently defunct ad networks (`AdColony`, `Chartboost`, `Flurry`, `Vungle`, `Tapjoy`), suppresses uncaught background exceptions, forces Google Play Games `GameHelper` to `STATE_CONNECTING`, and suppresses background looper crashes.

### Pillar 4: General-Purpose System Service Hooks & Security Rewriting
* **PendingIntent Mutability:** Injects `FLAG_IMMUTABLE` into legacy `PendingIntent` instances to satisfy Android 12+ requirements.
* **Package UID Ownership:** Rewrites guest package names to the host package (`com.psychostark.vemod`) for `INotificationManager`, `IActivityManager`, and `AudioService`.
* **Dynamic Package Metadata:** Dynamically synthesizes `PackageInfo` and `ApplicationInfo` containing authentic guest metadata and Google Play Services versions.

### Pillar 5: Hardware-Accelerated Graphics & Audio Virtualization
* **Dual-Pipeline GLES Bridge:** Direct hardware pass-through for 141 OpenGL ES 2.0 functions alongside fixed-function OpenGL ES 1.1 with native fixed-point `glTexEnvx` translation.
* **Low-Latency OpenSL ES Bridge:** Full NDK audio buffer queues with dynamically resolved interface IDs in `.bss`.
* **AttributionSource AudioTrack Virtualization:** Authentic audio output across modern Android versions.

---

## 5. Verification & Canary Benchmark Results

### Testing Environment:
* **Hardware:** Physical smartphone (`Nothing Phone (2a)`, Device ID: `00175359K003334`).
* **SoC & Architecture:** MediaTek Dimensity 7200 Pro (`arm64-v8a`, 64-bit-only hardware, 4 KB page size).
* **OS:** Android 16 (SDK 36, Vanilla Ice Cream Preview).
* **Host Package:** `com.psychostark.vemod` (Version 0.5.0).

### Verified Applications Matrix:
| Benchmark Application | Engine & Native Binaries | Key Systems Verified | Status |
|---|---|---|---|
| **Flappy Bird 1.2** (2014, ~1.3 MB) | Pure C++ AndEngine (`libandengine.so`) | Dynarmic A32 JIT, JNI method binding, GLES 2.0 direct pass-through, touch event loop, offline GameHelper neutralization. Locked portrait orientation (`SCREEN_ORIENTATION_PORTRAIT`). | **100% PASSED** (Fully playable, 60 FPS) |
| **Orange Roulette 1.0.0** (2014, ~8 MB) | Haxe / OpenFL Lime (`libApplicationMain.so`, `liblime.so`, `libopenal.so`, `libregexp.so`, `libstd.so`, `libzlib.so`) | Multi-module ELF loader, CFFI isolated handles, OpenAL audio track, 32-bit zlib streaming, ARM EABI soft-float math, dynamic GMS metadata, interactive cutscenes and gameplay. Sensor landscape auto-lock (`SCREEN_ORIENTATION_SENSOR_LANDSCAPE`, 2392x1080). Dynarmic IR cycle prevention verified across extended playthroughs. | **100% PASSED** (Fully playable, interactive audio & GLES) |
| **Angry Birds Rio 2.6.13** (2016, ~49 MB) | Rovio Fusion C++ Engine (`libAngryBirdsRio.so`) | Heavy multi-threaded 32-bit C++ engine, complex ELF relocation, JNI method dispatch with soft-float and double arguments, Dynarmic JIT multithreading (`GraphicsThread`), 16-byte `struct sigaction` stack frame preservation, 60+ FPS GLES 2.0 rendering in landscape. `TouchPacer` enables immediate single-tap UI button responsiveness. | **100% PASSED** (Fully playable, 60 FPS, touch verified) |
| **Angry Birds Space HD 2.2.14** (2015, ~48 MB) | Rovio Fusion C++ Engine (`libAngryBirdsSpace.so`, `libadcolony.so`, `libjs.so`) | Multi-threaded 32-bit C++ graphics engine with recursive mutex re-entrancy mapping (`PTHREAD_MUTEX_RECURSIVE`), 44-byte guest `struct tm` time serialization, immutable PendingIntent compatibility for Android 14+, 60 FPS animated OpenGL ES deep space rendering in landscape. Dynarmic IR cycle prevention resolved random crashes during level select and slingshot physics (Level 1 completed with 3 stars). | **100% PASSED** (Fully running, 60 FPS, level cleared) |
| **Angry Birds Classic** | Rovio Fusion C++ Engine (`libAngryBirdsClassic.so`) | Fixed loading screen abort and level crashes via Dynarmic IR identity cycle breaker and complete ARM32 syscall emulation. | **100% PASSED** (Fully playable, 60 FPS) |
| **Angry Birds Stella** | Rovio Fusion C++ Engine (`libAngryBirdsStella.so`) | Fixed crash on pressing Play via Dynarmic IR cycle prevention and timerfd/itimer syscalls. Full UI button responsiveness via `TouchPacer`. | **100% PASSED** (Fully playable, Level 1 verified) |
| **Doodle Jump** (`com.lima.doodlejump`) | Custom C++ Engine | Fixed crash after loading screen by emulating ARM32 `stat64`/`fstat64` structures and virtualizing system properties. | **100% PASSED** (Fully playable, tilt & touch verified) |
| **ES File Explorer** (`com.estrongs.androidpop`) | Native Bionic + Dalvik UI | Fixed invisible file text labels by neutralizing `#121212` text color override and properly handling theme resource inheritance. | **100% PASSED** (All labels rendered and readable) |
| **Lane Racer** (`com.boombit.LaneRacer`) | Unity 32-bit Engine (`libunity.so`, `libmono.so`) | Fixed ANR/freeze on pressing Single Play caused by `sched_setscheduler` returning `-EPERM`; spoofed success for non-root real-time scheduling. Restored full game audio via `GuestContext` attribution source forwarding. | **100% PASSED** (Interactive 60 FPS 3D racing with full audio) |
| **Reckless Racing Lite** (`com.polarbit.RecklessRacingLite`) | Polarbit 3D Engine (`librecklesslite.so`) | Fixed washed-out/pure-white UI and textures by replacing integer `glTexEnvi` with native fixed-point `glTexEnvx` in GLES bridge. Restored audio via `GuestContext` package and attribution source forwarding to system AudioTrack. | **100% PASSED** (Full 3D graphics, textured tracks and vehicles, interactive audio) |
| **Spider-Man Unlimited** (`com.gameloft.android.ANMP.GloftSIHM.charles`) | Gameloft Engine (`libGloftSIHM.so`) | Fixed immediate crash on open caused by Dynarmic JIT IR recursive identity cycle (`SIGSEGV` at frame 512). Fixed audio initialization failure via OpenSL ES bridge dynamic `SL_IID_*` resolution in `.bss`. | **100% PASSED** (Boots cleanly, OpenSL ES audio initialized, runs smoothly) |
| **Temple Run** (`com.imangi.templerun`) | Unity 32-bit Engine | Fixed missing audio via `GuestContext` package name stack inspection and `attributionSource` forwarding. Optimized JIT execution and frame pacing for smooth 3D gameplay. | **100% PASSED** (Full audio and smooth 3D gameplay) |

---

## 6. Repository File Structure

```
vemod/
├── app/
│   ├── src/main/
│   │   ├── AndroidManifest.xml            # Manifest declaring GuestProxyActivity & isolated guest processes (:guest1..:guest5)
│   │   ├── java/com/vemod/container/
│   │   │   ├── VemodApp.kt                # Application subclass, hidden API exemptions, Looper recovery
│   │   │   ├── bridge/
│   │   │   │   └── VemodBridge.kt         # JNI bridge loader & native runtime manager
│   │   │   ├── container/
│   │   │   │   ├── ApkExtractor.kt        # APK unpacker, ABI detector (v7a/v5), proxy generator, icon extractor
│   │   │   │   ├── ContainerRegistry.kt   # Container registry & exposed storage manager
│   │   │   │   ├── StubApkGenerator.kt    # On-device APK builder & v1/v2/v3 signer (com.vemod.container_<pkg>)
│   │   │   │   └── StubInstallReceiver.kt # PackageInstaller completion callback receiver
│   │   │   ├── model/
│   │   │   │   └── ContainerApp.kt        # Data model for installed 32-bit containers (with screenOrientation)
│   │   │   ├── proxy/
│   │   │   │   ├── ActivityManagerHook.kt # IActivityManager proxy rewriting pending intent packages
│   │   │   │   ├── AdSdkNeutralizer.kt    # Neutralizer for defunct ad & tracking SDKs (AdColony, Chartboost, etc.)
│   │   │   │   ├── GuestProcessManager.kt # Multi-process pool manager (:guest1 .. :guest5)
│   │   │   │   ├── GuestProxyActivity.kt  # Activity router & slot activities
│   │   │   │   ├── NotificationManagerHook.kt # INotificationManager proxy preventing UID security exceptions
│   │   │   │   ├── PackageManagerHook.kt  # Dynamic IPackageManager proxy & metadata injector
│   │   │   │   ├── TouchPacer.kt          # Touch event pacer for high-refresh-rate displays (120Hz/240Hz)
│   │   │   │   └── VemodInstrumentation.kt# Context/resource swapper, back navigation, lifecycle hooks
│   │   │   ├── ui/
│   │   │   │   └── MainActivity.kt        # Jetpack Compose Material 3 manager UI
│   │   │   └── virtual/
│   │   │       ├── GuestClassLoader.kt    # PathClassLoader redirecting findLibrary to proxy stubs
│   │   │       └── GuestContext.kt        # Virtualized ContextWrapper for sandbox paths, AudioTrack stack inspector & AttributionSource
│   │   └── cpp/
│   │       ├── CMakeLists.txt             # Native build script linking Dynarmic, Bionic, GLES, zlib, OpenSLES
│   │       ├── bridge/
│   │       │   ├── android.cpp            # NDK AAsset streaming bridge
│   │       │   ├── cpu.cpp / cpu.h        # Dynarmic A32 JIT callbacks, CPSR manipulation & full Linux syscall surface
│   │       │   ├── gles.cpp               # OpenGL ES 2.0 (141 funcs) + GLES 1.1 fixed-point pass-through
│   │       │   ├── jni_bridge.cpp         # 32-bit JNIEnv* facade & AArch64 trampolines
│   │       │   ├── libc.cpp               # Bionic libc/libm thunks & ARM EABI softfp math suite
│   │       │   ├── loader.cpp             # Dynamic multi-module ELF loader, packed relocation decoder (APS2) & isolated dlopen/dlsym
│   │       │   ├── memory.cpp             # 4 GB virtual memory arena & page management
│   │       │   ├── nb.h                   # Core bridge data structures (Ctx, layout, memory)
│   │       │   ├── opensles_bridge.cpp    # OpenSL ES 1.0.1 audio bridge & dynamic SL_IID_* .bss resolution
│   │       │   ├── pthread.cpp            # POSIX threads, mutexes, condition variables
│   │       │   ├── stdio.cpp              # Guest file I/O & logging redirection
│   │       │   ├── warmup.cpp             # JIT warmup & precompilation
│   │       │   └── zlib_bridge.cpp        # 32-bit z_stream_32 marshaling bridge
│   │       ├── proxy/
│   │       │   └── proxy.cpp              # 64-bit libproxy.so template
│   │       └── third_party/
│   │           └── dynarmic/              # ARM32 JIT recompiler with cycle-guarded IR identity traversal
│   └── assets/sysroot/                    # Preloaded 32-bit Android Nougat 7.0 AOSP Bionic system libraries
├── refference/
│   ├── Flappy Bird_1.2.apk                # Tier 1 Canary test APK (armeabi-v7a)
│   ├── orange-roulette-1-0-0.apk          # Tier 2 Canary test APK (armeabi, multi-module)
│   ├── angry-birds-rio-2-6-13.apk         # Tier 3 Canary test APK (complex multithreaded C++ engine)
│   └── angry-birds-space-hd-2-2-14.apk    # Tier 4 Canary test APK (heavy assets, deep space GLES)
└── documentation.md                       # Comprehensive documentation (this document)
```
