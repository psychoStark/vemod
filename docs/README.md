# Vemod Documentation

Welcome to the Vemod documentation suite.

## 📚 Table of Contents

- **[Architecture & Implementation Reference](documentation.md)**
  Deep technical dive into:
  - 32-bit execution model on 64-bit-only hardware (AArch64 / ARMv8 / ARMv9)
  - In-process Dynarmic ARM32 Just-In-Time (JIT) CPU recompiler
  - Hybrid Nougat 7.0 (API 24) AOSP Bionic RootFS integration
  - Dynamic multi-module ELF loader and relocation resolving
  - Universal Linux syscall dispatcher (125+ syscalls emulated)
  - Hardware-accelerated OpenGL ES 2.0 / 1.1 passthrough & extensions
  - Low-latency OpenSL ES audio virtualization
  - Android framework & context virtualization (GuestContext, TouchPacer, Looper recovery)

- **[Licensing & Legal Notices](../NOTICE.md)**
  - Dual-licensing terms (GPLv3 public + proprietary commercial agreement)
  - Third-party attributions (Dynarmic, AOSP Bionic, ApkSig, Compose, HiddenApiBypass)
