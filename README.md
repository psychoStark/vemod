# Vemod 🎮

**Play your favorite classic 32-bit Android games and apps on modern phones.**

Got a newer phone running Android 14, 15, or 16 (like the Pixel 8/9, Galaxy S24, or OnePlus 12) and found that your favorite older games refuse to install or say *"App not compatible with your phone"*? 

**Vemod** is an easy-to-use container app that lets you run classic 32-bit games smoothly on modern 64-bit-only devices—**no root required**.

---

## ✨ Features

- 🕹️ **Revive Classic Games & Apps:** Play legendary classics like original *Flappy Bird*, classic *Angry Birds* games, early *Temple Run*, *Reckless Racing*, *Spider-Man Unlimited*, and vintage Android apps that modern phones dropped support for.
- 🚀 **No Root Needed:** Runs 100% in user space on stock Android 14, 15, and 16.
- 🌐 **Google Play Games & microG Integration:** Native compatibility with open-source microG and ReVanced GmsCore for authentic Google Play Games sign-in, cloud saves, and achievements—plus a safe, compliant offline fallback when microG is absent.
- 📱 **Home Screen Shortcuts & App Drawer Slots:** Pin installed games directly to your launcher or assign them to native system app drawer launcher shortcuts.
- ⚡ **Smooth & Fast:** AOT precompiled code cache, direct GPU passthrough (GLES 1.1 / 2.0 / 3.0), OpenSL ES low-latency audio, and 60 FPS pacing.
- 📦 **One-Click APK / XAPK / APKS Installer:** Pick `.apk`, `.xapk`, or `.apks` files with automatic `.obb` expansion extraction.
- 🔗 **System "Open With" Integration:** Tap APKs or XAPKs directly from Files by Google, ZArchiver, Telegram, or your browser to install instantly.
- 📂 **Expose App Data & Public Storage:** Directly access save games and drag-and-drop OBB files via `/sdcard/vemod/<App_Name>/`.

---

## 🚀 Getting Started

1. **Download & Install Vemod** on your device.
2. Open Vemod and tap **Add App** (the `+` button).
3. Select any 32-bit `.apk` file from your device.
4. Once installed, tap the game icon to launch!
5. *(Optional)* Tap the menu on any game card to **Add to Home Screen** or assign it to an **App Drawer Slot** for instant access.

---

## ❓ Frequently Asked Questions

<details>
<summary><b>Why do older games stop working on newer phones?</b></summary>
Modern smartphone processors (like Snapdragon 8 Gen 3, Google Tensor G3/G4) dropped hardware support for older 32-bit code to save power and simplify chips. Vemod bridges this gap so your modern phone can understand and run those older apps seamlessly.
</details>

<details>
<summary><b>Do I need to root my device?</b></summary>
No! Vemod works entirely as a normal, safe Android app without root permissions.
</details>

<details>
<summary><b>Can I sign in to Google Play Games and sync cloud saves?</b></summary>
Yes! Vemod provides native support for open-source <b>microG</b> (including ReVanced GmsCore). Simply install microG on your device, and Vemod will automatically route Google Play Services IPC, account selection, and cloud saves directly to microG. If microG is not installed, games cleanly fall back to offline mode without crashing.
</details>

<details>
<summary><b>Where are my game files stored?</b></summary>
Each app is isolated safely inside Vemod. You can remove any installed app at any time directly from the Vemod dashboard.
</details>

---

## 🔨 Building from Source

### Prerequisites

| Requirement | Version |
|---|---|
| Android Studio | Ladybug (2024.2) or later |
| JDK | 17+ |
| Android SDK (compileSdk) | 35 |
| Android NDK | 28.2.13676358 |
| CMake | 3.22.1 |
| Kotlin | 2.x (via Compose compiler plugin) |

### Build Steps

1. **Clone** the repository:
   ```bash
   git clone https://github.com/<your-org>/vemod.git
   cd vemod
   ```

2. **Build** (debug):
   ```bash
   ./gradlew assembleDebug
   ```
   or for release:
   ```bash
   ./gradlew assembleRelease
   ```

3. **Output APK** will be at:
   ```
   app/build/outputs/apk/debug/app-debug.apk
   app/build/outputs/apk/release/app-release-unsigned.apk
   ```

### Architecture

Vemod builds exclusively for `arm64-v8a`. The 32-bit guest binaries run via the embedded Dynarmic JIT — no 32-bit host libraries are required.

### Native Build Notes

The native C++ layer (Dynarmic JIT, bridge, GLES, OpenSL ES) is built via CMake. First build may take several minutes as it compiles Dynarmic and its dependencies (biscuit, fmt, mcl, oaknut, zydis). Subsequent builds are incremental.

---

## ⚠️ Unsupported Systems & Limitations

Because Vemod runs as a 100% userspace container without device root, kernel modules, or low-level OS modifications:
- **Play Integrity & SafetyNet:** Hardware attestation cannot be passed for apps requiring certified OEM keybox signatures.
- **Google Services Without microG:** Games requiring Google Play Services will run in offline guest mode unless microG or ReVanced GmsCore is installed.
- **Custom Hardware Drivers:** External accessories requiring proprietary kernel USB/Bluetooth drivers are not supported.

---

## 📖 Etymology

**Vemod** is a Norwegian word (*pronounced "vay-mood"*). It describes a wistful, tender longing or quiet nostalgia for a past that cannot be recovered—a sentiment that perfectly embodies the spirit of preserving and playing the golden generation of mobile gaming classics that modern hardware left behind.

---

## 🛠️ For Developers & Technical Deep-Dive

Looking for the technical internals, JIT compilation engine (Dynarmic), JNI bridging, syscall router, and architecture? See the full [Technical Documentation](docs/documentation.md).
