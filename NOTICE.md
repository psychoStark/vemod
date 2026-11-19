# Vemod - Licensing & Legal Notices

Copyright (C) 2026 psychoStark.

Vemod is free and open-source software licensed under the **GNU General Public License version 3 (GPLv3)**.

---

## 📜 GNU General Public License v3
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <https://www.gnu.org/licenses/>.

---

## 💼 Commercial & Proprietary Licensing (Dual Licensing)
Commercial and proprietary licensing options apply **exclusively to original code authored by psychoStark** (`com.vemod.*`, native bridges in `libbridge.so`). 

If you wish to incorporate parts of Vemod's original source code, architecture, or native components into a closed-source, proprietary, or commercial application without subjecting your application to the copyleft requirements of the GNU GPLv3, you **must obtain express written permission and a commercial license** from the copyright holder.

For commercial inquiries and licensing exceptions, contact:
**psychoStark**

---

## 🧩 Third-Party Components & Attributions

Vemod incorporates or links against open-source third-party software:

### 1. Dynarmic ARM32 JIT CPU Recompiler
* **Copyright:** MerryMage and Dynarmic Project Contributors
* **License:** 0BSD (BSD Zero Clause License)
* **URL:** https://github.com/merryhime/dynarmic

### 2. Android Open Source Project (AOSP) Bionic Libraries
* **Copyright:** The Android Open Source Project
* **License:** Apache License, Version 2.0 / BSD 2-Clause & 3-Clause
* **URL:** https://android.googlesource.com/platform/bionic/

### 3. Google ApkSig Library
* **Copyright:** The Android Open Source Project
* **License:** Apache License, Version 2.0
* **URL:** https://android.googlesource.com/platform/tools/apksig/

### 4. dlmalloc (Doug Lea's Memory Allocator 2.8.6)
* **Copyright:** Doug Lea
* **License:** Public Domain / CC0 / MIT-0 Equivalent
* **URL:** http://gee.cs.oswego.edu/dl/html/malloc.html

### 5. Boost C++ Libraries
* **Copyright:** Boost Authors
* **License:** Boost Software License 1.0 (BSL-1.0)
* **URL:** https://www.boost.org/

### 6. microG Project
* **Copyright:** microG Team
* **License:** Apache License, Version 2.0
* **URL:** https://github.com/microg/GmsCore

### 7. Bouncy Castle Cryptography APIs
* **Copyright:** Legion of the Bouncy Castle Inc.
* **License:** Bouncy Castle Licence (MIT-style)
* **URL:** https://www.bouncycastle.org/

### 8. AndroidX & Jetpack Compose
* **Copyright:** The Android Open Source Project
* **License:** Apache License, Version 2.0
* **URL:** https://developer.android.com/jetpack

### 9. HiddenApiBypass
* **Copyright:** LSPosed Developers
* **License:** Apache License, Version 2.0
* **URL:** https://github.com/LSPosed/AndroidHiddenApiBypass

---

## 🛡️ Clean-Room Interoperability Shims (`com.vemod.container.shims.*`)

To guarantee 100% trademark sanitization in the source tree, all interoperability shims are isolated in `com.vemod.container.shims.*` and dynamically bound at runtime via `GuestClassLoader` redirection:

### 1. Clean-Room Asset Shim (`com.vemod.container.shims.RovioFileReader`)
* **Purpose:** Independent clean-room file and asset stream provider interfacing with Android `AssetManager` and virtualized container assets, normalizing path handling and decompressing sprite atlases. Dynamically bound to guest lookups for `com.rovio.fusion.FileReader`.
* **Compatibility:** Used by Rovio Fusion games (*Angry Birds Classic, Stella, Rio, Space HD*).
* **Legal Notice:** This class is an original clean-room reimplementation written strictly for ABI compatibility. It contains no proprietary source code or assets from Rovio Entertainment Oyj.

### 2. Clean-Room Audio Shim (`com.vemod.container.shims.PolarbitFuseAudio`)
* **Purpose:** Independent clean-room audio driver providing low-latency PCM streaming, `AudioTrack` output stream normalization (`AudioManager.STREAM_MUSIC`), and JNI mixer synchronization. Dynamically bound to guest lookups for `com.polarbit.fuse.FuseAudio`.
* **Compatibility:** Used by Polarbit 3D games (*Reckless Racing Lite*).
* **Legal Notice:** This class is an original clean-room reimplementation written strictly for ABI compatibility. It contains no proprietary source code or assets from Polarbit AB.

Under established international software and copyright jurisprudence (including *Google LLC v. Oracle America, Inc.*, 141 S. Ct. 1183 (2021) and EU Software Directive 2009/24/EC Art. 6), implementing functional declarations and API method signatures for the sole purpose of software interoperability is recognized as lawful fair use.

---

All trademarks, logos, and brand names are the property of their respective owners.
