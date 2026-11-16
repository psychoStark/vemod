#include <cxxabi.h>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpu.h"
#include "nb.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#endif

namespace nb {

namespace {

struct GuestModule {
    std::string path;
    std::string soname;
    gaddr base = 0;
    u32 image_size = 0;
    gaddr code_lo = 0, code_hi = 0;  // executable PT_LOAD
    u32 image_id = 0;                // hash of the build-id note
    gaddr exidx = 0;
    u32 exidx_count = 0;
    gaddr jni_onload = 0;
    gaddr soinfo = 0;                // Bionic soinfo structure in guest memory
    std::unordered_map<std::string, gaddr> exports;
    std::vector<std::pair<gaddr, gaddr>> funcs;  // (address, guest name pointer), sorted
};

// Android Bionic soinfo structure (ARM 32-bit layout for Android 2.3 - 7.1)
struct GuestSoInfo {
    char name[128];
    gaddr phdr;
    u32 phnum;
    gaddr entry;
    gaddr base;
    u32 size;
    u32 unused;
    gaddr dynamic;
    gaddr wrprotect_start;
    gaddr wrprotect_end;
    gaddr next;
    u32 flags;
    gaddr strtab;
    gaddr symtab;
    u32 nbucket;
    u32 nchain;
    gaddr bucket;
    gaddr chain;
    gaddr plt_got;
    gaddr plt_rel;
    u32 plt_rel_count;
    gaddr rel;
    u32 rel_count;
    gaddr preinit_array;
    u32 preinit_array_count;
    gaddr init_array;
    u32 init_array_count;
    gaddr fini_array;
    u32 fini_array_count;
    gaddr init_func;
    gaddr fini_func;
    u32 refcount;
    u32 link_map[5];
    u32 constructors_called;
    gaddr load_bias;
    u32 has_text_relocations;
    u32 has_DT_SYMBOLIC;
};

std::vector<std::shared_ptr<GuestModule>> g_modules;
std::string s_guest_lib_dir;
std::string s_sysroot_dir;
bool s_sysroot_loaded = false;
bool s_sysroot_loading = false;
std::unordered_set<std::string> s_loading_libs;
std::unordered_map<std::string, gaddr> g_merged_exports;
std::unordered_map<std::string, gaddr> g_stub_cache;
gaddr s_next_lib_base = layout::kLibBase;

static inline u32 elf_hash(const char* name) {
    u32 h = 0, g;
    while (*name) {
        h = (h << 4) + *reinterpret_cast<const u8*>(name++);
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static bool should_override_with_host(const char* name) {
    if (!name) return false;
    // Only override if we actually have a host implementation registered!
    if (!find_handler(name)) return false;

    // Dynamic linker stubs
    if (strncmp(name, "dl", 2) == 0 || strncmp(name, "android_dl", 10) == 0) return true;
    // GLES / EGL GPU host passthrough
    if (strncmp(name, "gl", 2) == 0 || strncmp(name, "egl", 3) == 0) return true;
    // Android NDK host passthrough (only functions with actual host implementations)
    if (name[0] == 'A' && (strncmp(name, "AAsset", 6) == 0 ||
                           strncmp(name, "ALooper_", 8) == 0 ||
                           strncmp(name, "AInputQueue_", 12) == 0 ||
                           strncmp(name, "AInputEvent_", 12) == 0 ||
                           strncmp(name, "AMotionEvent_", 13) == 0 ||
                           strncmp(name, "AKeyEvent_", 10) == 0 ||
                           strncmp(name, "AConfiguration_", 15) == 0 ||
                           strncmp(name, "ASensor", 7) == 0 ||
                           strncmp(name, "ANativeWindow_", 14) == 0)) return true;
    // OpenSL ES audio stubs
    if (strncmp(name, "sl", 2) == 0) return true;
    // System properties
    if (strcmp(name, "__system_property_get") == 0) return true;
    // Android logcat passthrough
    if (strncmp(name, "__android_log_", 14) == 0) return true;
    // Mono runtime thread suspension override (prevent signal deadlocks)
    if (strcmp(name, "mono_thread_suspend_all_other_threads") == 0) return true;
    if (strcmp(name, "mono_gc_collect") == 0) return true;
    return false;
}

static void setup_kernel_args(GuestThread& t) {
    gaddr rnd = misc_alloc(16, 4);
    wr<u32>(rnd + 0, 0x12345678);
    wr<u32>(rnd + 4, 0x9abcdef0);
    wr<u32>(rnd + 8, 0x0fedcba9);
    wr<u32>(rnd + 12, 0x87654321);

    // ARMv7-A capabilities: VFP, EDSP, NEON, VFPv3, VFPv3D16, TLS, VFPv4, IDIVA, IDIVT, VFPD32
    constexpr u32 kHwcapArmv7 = (1 << 6) | (1 << 7) | (1 << 12) | (1 << 13) | (1 << 14) |
                                (1 << 15) | (1 << 16) | (1 << 17) | (1 << 18) | (1 << 19);
    constexpr u32 kHwcap2Armv7 = 0; // ARMv7 has no ARMv8 crypto extensions (prevents illegal sha1/aes opcodes)

    gaddr auxv = misc_alloc(6 * 8, 4);
    wr<u32>(auxv + 0, 6);            // AT_PAGESZ
    wr<u32>(auxv + 4, 4096);
    wr<u32>(auxv + 8, 16);           // AT_HWCAP
    wr<u32>(auxv + 12, kHwcapArmv7);
    wr<u32>(auxv + 16, 26);          // AT_HWCAP2
    wr<u32>(auxv + 20, kHwcap2Armv7);
    wr<u32>(auxv + 24, 25);          // AT_RANDOM
    wr<u32>(auxv + 28, rnd);
    wr<u32>(auxv + 32, 33);          // AT_SYSINFO_EHDR
    wr<u32>(auxv + 36, 0);
    wr<u32>(auxv + 40, 0);           // AT_NULL
    wr<u32>(auxv + 44, 0);

    gaddr argv0 = gstrdup("vemod");
    gaddr argv_arr = misc_alloc(2 * 4, 4);
    wr<u32>(argv_arr + 0, argv0);
    wr<u32>(argv_arr + 4, 0);

    gaddr env0 = gstrdup("ANDROID_DATA=/data");
    gaddr env1 = gstrdup("ANDROID_ROOT=/system");
    gaddr env_arr = misc_alloc(3 * 4, 4);
    wr<u32>(env_arr + 0, env0);
    wr<u32>(env_arr + 4, env1);
    wr<u32>(env_arr + 8, 0);

    gaddr kab = misc_alloc(16, 4);
    wr<u32>(kab + 0, 1);
    wr<u32>(kab + 4, argv_arr);
    wr<u32>(kab + 8, env_arr);
    wr<u32>(kab + 12, auxv);

    wr<u32>(t.tls + tlsoff::kSlotOpenGLApi, kab);
}

static gaddr s_rio_base = 0;
static void hook_rio_queue_timer(Ctx& c) {
    gaddr fn = s_rio_base + 0x469400;
    u32 r4_save = c.r[4];
    NB_LOGI("hook_rio_queue_timer: ENTER r0=%#x r1=%#x r4=%#x sp=%#x", c.r[0], c.r[1], c.r[4], c.r[13]);
    u32 ret = u32(call_guest(fn, {ga(c.r[0]), ga(c.r[1])}));
    NB_LOGI("hook_rio_queue_timer: LEAVE ret=%#x restoring r4=%#x (c.r[4]=%#x)", ret, r4_save, c.r[4]);
    c.r[4] = r4_save;
    c.r[0] = ret;
}

static void hook_unity_dlsym_fallback(Ctx& c) {
    u32 handle = c.a32();
    gaddr name_addr = c.a32();
    const char* name = name_addr ? hs(name_addr) : nullptr;
    if (!name) {
        c.ret(0);
        return;
    }
    u32 sym = guest_dlsym(handle, name);
    NB_LOGI("Unity embedded dlsym fallback: handle=%#x, name='%s' -> %#x", handle, name, sym);
    c.ret(sym);
}

void missing_import(Ctx& c) {
    NB_LOGW("guest called unimplemented import '%s' - returning 0", c.name);
    c.ret(0);
}

u32 arm_imm(u32 w) {  // ARM data-processing modified immediate
    u32 rot = ((w >> 8) & 15) * 2, imm = w & 0xff;
    return rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
}

static inline int64_t read_sleb128(const u8*& p, const u8* end) {
    int64_t result = 0;
    int shift = 0;
    u8 byte;
    do {
        if (p >= end) return 0;
        byte = *p++;
        result |= int64_t(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if ((shift < 64) && (byte & 0x40)) {
        result |= -(int64_t(1) << shift);
    }
    return result;
}

// Every call into an exported function goes through the PLT ("ldr pc, [ip, #n]!"), an indirect
// branch that dynarmic's arm64 backend can only resolve through a C++ hash lookup. Once the GOT
// is relocated the targets are fixed, so rewrite each entry into a direct "b target" that the
// JIT can link block-to-block.
void patch_plt(const u8* img, size_t size, gaddr base) {
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(img);
    if (!eh->e_shoff || eh->e_shoff + size_t(eh->e_shnum) * sizeof(Elf32_Shdr) > size) return;
    auto* sh = reinterpret_cast<const Elf32_Shdr*>(img + eh->e_shoff);
    const char* names = reinterpret_cast<const char*>(img + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; ++i) {
        if (strcmp(names + sh[i].sh_name, ".plt") != 0) continue;
        gaddr start = base + sh[i].sh_addr, end = start + sh[i].sh_size;
        u32 patched = 0, skipped = 0;
        for (gaddr a = start; a + 12 <= end; a += 4) {
            u32 w0 = rd<u32>(a), w1 = rd<u32>(a + 4), w2 = rd<u32>(a + 8);
            // add ip, pc, #i0 ; add ip, ip, #i1 ; ldr pc, [ip, #i2]!
            if ((w0 & 0xfffff000) != 0xe28fc000 || (w1 & 0xfffff000) != 0xe28cc000 || (w2 & 0xfffff000) != 0xe5bcf000)
                continue;
            gaddr got = a + 8 + arm_imm(w0) + arm_imm(w1) + (w2 & 0xfff);
            if (!is_mapped(got)) {
                ++skipped;
                continue;
            }
            gaddr target = rd<u32>(got);
            s64 off = (s64(target) - s64(a + 8)) / 4;
            if (!target || !is_mapped(target) || (target & 3) || off < -(1 << 23) || off >= (1 << 23)) {
                ++skipped;
                continue;
            }
            wr<u32>(a, 0xea000000u | (u32(off) & 0xffffff));
            ++patched;
            a += 8;
        }
        NB_LOGI("PLT: %u entries turned into direct branches (%u left indirect)", patched, skipped);
    }
}

std::shared_ptr<GuestModule> load_image(const u8* img, size_t size, gaddr base, const char* filepath) {
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(img);
    if (size < sizeof(Elf32_Ehdr) || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_machine != EM_ARM)
        fatal("embedded guest image is not a 32-bit ARM ELF");
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(img + eh->e_phoff);

    auto mod = std::make_shared<GuestModule>();
    mod->path = filepath ? filepath : "";
    if (filepath) {
        std::string p(filepath);
        auto slash = p.rfind('/');
        mod->soname = (slash != std::string::npos) ? p.substr(slash + 1) : p;
    }

    u32 max_va = 0;
    const Elf32_Phdr* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_LOAD) max_va = std::max(max_va, ph[i].p_vaddr + ph[i].p_memsz);
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
            mod->code_lo = base + ph[i].p_vaddr;
            mod->code_hi = base + ph[i].p_vaddr + ph[i].p_memsz;
        }
        if (ph[i].p_type == PT_NOTE && ph[i].p_offset + ph[i].p_filesz <= size) {
            u32 hsh = 2166136261u;
            for (u32 k = 0; k < ph[i].p_filesz; ++k) hsh = (hsh ^ img[ph[i].p_offset + k]) * 16777619u;
            mod->image_id = hsh ^ u32(size);
        }
        if (ph[i].p_type == PT_DYNAMIC) dyn = &ph[i];
        if (ph[i].p_type == PT_ARM_EXIDX) {
            mod->exidx = base + ph[i].p_vaddr;
            mod->exidx_count = ph[i].p_memsz / 8;
        }
    }
    if (mod->image_id == 0) {
        u32 hsh = 2166136261u;
        for (char c : mod->soname) hsh = (hsh ^ (u8)c) * 16777619u;
        u32 sample_sz = std::min<u32>(size, 4096);
        for (u32 k = 0; k < sample_sz; ++k) hsh = (hsh ^ img[k]) * 16777619u;
        mod->image_id = (hsh ^ u32(size)) ? (hsh ^ u32(size)) : 1;
    }
    mod->base = base;
    mod->image_size = page_round_up(max_va);
    s_next_lib_base = std::max(s_next_lib_base, page_round_up(base + mod->image_size + 0x100000));
    mem_protect(base, mod->image_size, PROT_READ | PROT_WRITE);
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) fatal("truncated guest image");
        memcpy(h(base + ph[i].p_vaddr), img + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz) {
            memset(h(base + ph[i].p_vaddr + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
        }
    }
    if (!dyn) fatal("guest image has no PT_DYNAMIC");

    gaddr symtab = 0, strtab = 0, rel = 0, jmprel = 0, init_array = 0, init_fn = 0;
    u32 relsz = 0, pltrelsz = 0, init_array_sz = 0, nsyms = 0, soname_off = 0;
    gaddr gnu_hash = 0, elf_hash_addr = 0;
    gaddr android_rel = 0, android_rela = 0;
    u32 android_relsz = 0, android_relasz = 0;
    std::vector<u32> dt_needed_offsets;

    if (eh->e_shoff && eh->e_shoff + size_t(eh->e_shnum) * sizeof(Elf32_Shdr) <= size) {
        auto* sh = reinterpret_cast<const Elf32_Shdr*>(img + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; ++i) {
            if (sh[i].sh_type == SHT_DYNSYM) {
                nsyms = sh[i].sh_size / sizeof(Elf32_Sym);
                break;
            }
        }
    }

    for (gaddr d = base + dyn->p_vaddr;; d += 8) {
        s32 tag = rd<s32>(d);
        u32 val = rd<u32>(d + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_SYMTAB: symtab = base + val; break;
        case DT_STRTAB: strtab = base + val; break;
        case DT_SONAME: soname_off = val; break;
        case DT_HASH: elf_hash_addr = base + val; if (!nsyms) nsyms = rd<u32>(elf_hash_addr + 4); break;
        case 0x6ffffef5: gnu_hash = base + val; break;  // DT_GNU_HASH
        case DT_REL: rel = base + val; break;
        case DT_RELSZ: relsz = val; break;
        case 0x6000000f: android_rel = base + val; break;   // DT_ANDROID_REL
        case 0x60000010: android_relsz = val; break;        // DT_ANDROID_RELSZ
        case 0x60000011: android_rela = base + val; break;  // DT_ANDROID_RELA
        case 0x60000012: android_relasz = val; break;       // DT_ANDROID_RELASZ
        case DT_JMPREL: jmprel = base + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT_ARRAY: init_array = base + val; break;
        case DT_INIT_ARRAYSZ: init_array_sz = val; break;
        case DT_INIT: init_fn = base + val; break;
        case DT_NEEDED: dt_needed_offsets.push_back(val); break;
        default: break;
        }
    }

    if (!nsyms && gnu_hash) {
        u32 nbuckets = rd<u32>(gnu_hash);
        u32 symoffset = rd<u32>(gnu_hash + 4);
        u32 bloom_size = rd<u32>(gnu_hash + 8);
        gaddr buckets = gnu_hash + 16 + bloom_size * 4;
        u32 max_sym = 0;
        for (u32 b = 0; b < nbuckets; ++b) {
            u32 s = rd<u32>(buckets + b * 4);
            if (s > max_sym) max_sym = s;
        }
        if (max_sym >= symoffset) {
            gaddr chains = buckets + nbuckets * 4;
            u32 cur = max_sym;
            while ((rd<u32>(chains + (cur - symoffset) * 4) & 1) == 0) {
                ++cur;
            }
            nsyms = cur + 1;
        } else {
            nsyms = symoffset;
        }
    }

    if (strtab && soname_off) {
        mod->soname = hs(strtab + soname_off);
    }

    auto sym = [&](u32 i) { return hp<Elf32_Sym>(symtab + i * sizeof(Elf32_Sym)); };
    for (u32 i = 1; i < nsyms; ++i) {
        Elf32_Sym* s = const_cast<Elf32_Sym*>(sym(i));
        if (s->st_shndx == SHN_UNDEF) continue;
        int bind = ELF32_ST_BIND(s->st_info), type = ELF32_ST_TYPE(s->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        const char* sym_name = hs(strtab + s->st_name);

        if (should_override_with_host(sym_name)) {
            gaddr stub = resolve_import(sym_name);
            if (stub) {
                mod->exports[sym_name] = stub;
                s->st_value = stub - base;
                NB_LOGI("Overrode %s export '%s' with stub %#x",
                        mod->soname.c_str(), sym_name, stub);
                continue;
            }
        }

        mod->exports[sym_name] = base + s->st_value;
        if (strcmp(sym_name, "JNI_OnLoad") == 0) {
            mod->jni_onload = base + s->st_value;
        }
        if (type == STT_FUNC) mod->funcs.push_back({(base + s->st_value) & ~1u, strtab + s->st_name});
    }
    std::sort(mod->funcs.begin(), mod->funcs.end());

    // Load DT_NEEDED shared library dependencies before resolving relocations
    for (u32 off : dt_needed_offsets) {
        if (!strtab) break;
        const char* dep_name = hs(strtab + off);
        if (!dep_name || !*dep_name) continue;

        // Check if already loaded
        bool already_loaded = false;
        for (const auto& m : g_modules) {
            if (m->soname == dep_name) {
                already_loaded = true;
                break;
            }
        }
        if (already_loaded || s_loading_libs.count(dep_name)) continue;

        // Try loading from guest lib dir (APK native libraries)
        if (!s_guest_lib_dir.empty()) {
            std::string full_path = s_guest_lib_dir + "/" + dep_name;
            if (access(full_path.c_str(), R_OK) == 0) {
                NB_LOGI("Loading DT_NEEDED dependency from guest lib dir: %s (needed by %s)",
                        dep_name, mod->soname.c_str());
                load_guest_library(full_path.c_str());
                continue;
            }
        }

        // Try loading from sysroot dir
        if (!s_sysroot_dir.empty()) {
            std::string full_path = s_sysroot_dir + "/" + dep_name;
            if (access(full_path.c_str(), R_OK) == 0) {
                NB_LOGI("Loading DT_NEEDED dependency from sysroot: %s (needed by %s)",
                        dep_name, mod->soname.c_str());
                load_guest_library(full_path.c_str());
                continue;
            }
        }
    }

    std::vector<gaddr> resolved(nsyms, ~0u);
    auto sym_addr = [&](u32 i) -> gaddr {
        if (i >= resolved.size()) resolved.resize(i + 1, ~0u);
        if (resolved[i] != ~0u) return resolved[i];
        const Elf32_Sym* s = sym(i);
        gaddr a = 0;
        if (s->st_shndx != SHN_UNDEF) {
            a = base + s->st_value;
        } else {
            const char* name = hs(strtab + s->st_name);
            if (should_override_with_host(name)) {
                a = resolve_import(name);
            } else {
                // 1. Prioritize custom bionic overrides if present
                for (const auto& m : g_modules) {
                    if (m->soname == "libbionic_guest.so") {
                        auto it = m->exports.find(name);
                        if (it != m->exports.end()) {
                            a = it->second;
                            break;
                        }
                        break;
                    }
                }
                // 2. Search existing guest modules
                if (!a) {
                    for (const auto& m : g_modules) {
                        auto it = m->exports.find(name);
                        if (it != m->exports.end()) {
                            a = it->second;
                            break;
                        }
                    }
                }
                // 3. Fall back to host bridge imports / stubs
                if (!a) {
                    if (find_handler(name) || find_data(name)) {
                        a = resolve_import(name);
                    } else if (ELF32_ST_BIND(s->st_info) == STB_WEAK) {
                        // Weak undefined symbols resolve to 0 (NULL) if not found anywhere
                        a = 0;
                    } else {
                        a = resolve_import(name);
                    }
                }
            }
        }
        if (s->st_shndx != SHN_UNDEF && ELF32_ST_TYPE(s->st_info) == STT_GNU_IFUNC) {
            gaddr impl = gaddr(call_guest(a, {}));
            NB_LOGI("ifunc %s resolved to %#x", hs(strtab + s->st_name), impl);
            a = impl;
        }
        return resolved[i] = a;
    };

    auto apply = [&](gaddr table, u32 bytes) {
        for (gaddr r = table; r < table + bytes; r += 8) {
            gaddr where = base + rd<u32>(r);
            u32 info = rd<u32>(r + 4);
            u32 type = ELF32_R_TYPE(info), si = ELF32_R_SYM(info);
            switch (type) {
            case R_ARM_RELATIVE: wr<u32>(where, rd<u32>(where) + base); break;
            case R_ARM_ABS32: wr<u32>(where, rd<u32>(where) + sym_addr(si)); break;
            case R_ARM_GLOB_DAT:
            case R_ARM_JUMP_SLOT: wr<u32>(where, sym_addr(si)); break;
            case 0: break;
            default: fatal("unsupported relocation type %u", type);
            }
        }
    };

    auto apply_android_relocs = [&](gaddr table, u32 bytes) {
        if (!table || bytes < 4) return;
        const u8* p = hp<u8>(table);
        const u8* end = p + bytes;
        if (memcmp(p, "APS2", 4) != 0) {
            NB_LOGW("packed relocations header is not APS2 in %s", mod->soname.c_str());
            return;
        }
        p += 4;
        int64_t num_relocs = read_sleb128(p, end);
        int64_t offset = read_sleb128(p, end);
        int64_t addend = 0;

        u32 count = 0;
        while (num_relocs > 0 && p < end) {
            int64_t num_in_group = read_sleb128(p, end);
            if (num_in_group > num_relocs) break;
            num_relocs -= num_in_group;

            int64_t flags = read_sleb128(p, end);
            bool grouped_by_info = (flags & 1) != 0;
            bool grouped_by_offset_delta = (flags & 2) != 0;
            bool grouped_by_addend = (flags & 4) != 0;
            bool group_has_addend = (flags & 8) != 0;

            int64_t group_offset_delta = 0;
            if (grouped_by_offset_delta) group_offset_delta = read_sleb128(p, end);

            int64_t group_r_info = 0;
            if (grouped_by_info) group_r_info = read_sleb128(p, end);

            if (grouped_by_addend && group_has_addend) addend += read_sleb128(p, end);
            if (!group_has_addend) addend = 0;

            for (int64_t i = 0; i < num_in_group && p <= end; ++i) {
                if (grouped_by_offset_delta) {
                    offset += group_offset_delta;
                } else {
                    offset += read_sleb128(p, end);
                }
                uint64_t r_info = grouped_by_info ? group_r_info : read_sleb128(p, end);
                if (group_has_addend && !grouped_by_addend) {
                    addend += read_sleb128(p, end);
                }

                gaddr where = base + gaddr(offset);
                u32 type = ELF32_R_TYPE(u32(r_info));
                u32 si = ELF32_R_SYM(u32(r_info));
                switch (type) {
                case R_ARM_RELATIVE:
                    wr<u32>(where, rd<u32>(where) + base);
                    break;
                case R_ARM_ABS32:
                    wr<u32>(where, rd<u32>(where) + sym_addr(si) + addend);
                    break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT:
                    wr<u32>(where, sym_addr(si) + addend);
                    break;
                case 0:
                    break;
                default:
                    fatal("unsupported packed relocation type %u in %s", type, mod->soname.c_str());
                }
                ++count;
            }
        }
        NB_LOGI("applied %u packed relocations for %s", count, mod->soname.c_str());
    };

    apply(rel, relsz);
    apply_android_relocs(android_rel, android_relsz);
    apply_android_relocs(android_rela, android_relasz);
    apply(jmprel, pltrelsz);
    patch_plt(img, size, base);
    if (mod->soname == "libAngryBirdsRio.so" || mod->path.find("AngryBirdsRio") != std::string::npos) {
        s_rio_base = base;
        gaddr stub = make_stub("rio_queue_timer", hook_rio_queue_timer);
        u32 svc_insn = rd<u32>(stub);
        wr<u32>(base + 0x28db2c, svc_insn);
        NB_LOGI("Installed Rio queue timer hook at %#x (replaced %#x with SVC %#x)",
                base + 0x28db2c, 0xeb076e33, svc_insn);
    }
    if (mod->soname == "libunity.so" || mod->soname == "libunity_proxy.so" || mod->path.find("libunity.so") != std::string::npos) {
        if (base + 0x1908 <= base + mod->image_size &&
            rd<u32>(base + 0x1810) == 0xe92d4070 && rd<u32>(base + 0x1900) == 0xe3a00000) {
            gaddr stub = make_stub("unity_dlsym_fallback", hook_unity_dlsym_fallback);
            u32 svc_insn = rd<u32>(stub);
            gaddr tramp = misc_alloc(24, 4);
            wr<u32>(tramp + 0, 0xe1a00004);  // mov r0, r4 (handle)
            wr<u32>(tramp + 4, 0xe1a01006);  // mov r1, r6 (name)
            wr<u32>(tramp + 8, svc_insn);    // SVC hook_unity_dlsym_fallback
            wr<u32>(tramp + 12, 0xe28dd008); // add sp, sp, #8
            wr<u32>(tramp + 16, 0xe8bd8070); // pop {r4, r5, r6, pc}

            wr<u32>(base + 0x1900, 0xe51ff004); // ldr pc, [pc, #-4]
            wr<u32>(base + 0x1904, tramp);
            NB_LOGI("Installed Unity embedded dlsym fallback at %#x -> tramp %#x (SVC %#x)",
                    base + 0x1900, tramp, svc_insn);
        }
        if (base + 0x3b8c38 <= base + mod->image_size &&
            rd<u32>(base + 0x3b8c14) == 0xe92d4070 &&
            rd<u32>(base + 0x3b8c34) == 0x0a00000e) {
            wr<u32>(base + 0x3b8c34, 0xe1a00000);  // nop (force FMOD AudioTrack driver registration)
            NB_LOGI("Patched Unity FMOD output driver check at %#x (replaced beq with nop)", base + 0x3b8c34);
        }
    }
    // ELF hash table for symbol lookup
    u32 nbucket = 0, nchain = 0;
    gaddr bucket = 0, chain = 0;
    if (elf_hash_addr) {
        nbucket = rd<u32>(elf_hash_addr);
        nchain = rd<u32>(elf_hash_addr + 4);
        bucket = elf_hash_addr + 8;
        chain = elf_hash_addr + 8 + nbucket * 4;
    } else if (nsyms && strtab && symtab) {
        nbucket = std::max(1u, nsyms / 4 + 1);
        nchain = nsyms;
        u32 tbl_sz = (2 + nbucket + nchain) * 4;
        gaddr synth = misc_alloc(tbl_sz, 4);
        wr<u32>(synth, nbucket);
        wr<u32>(synth + 4, nchain);
        bucket = synth + 8;
        chain = synth + 8 + nbucket * 4;
        memset(h(bucket), 0, (nbucket + nchain) * 4);
        const auto* syms = hp<Elf32_Sym>(symtab);
        for (u32 s = 1; s < nsyms; ++s) {
            if (syms[s].st_name) {
                const char* sname = hs(strtab + syms[s].st_name);
                u32 hsh = elf_hash(sname) % nbucket;
                wr<u32>(chain + s * 4, rd<u32>(bucket + hsh * 4));
                wr<u32>(bucket + hsh * 4, s);
            }
        }
    }

    // Create guest soinfo structure for embedded linkers (Unity, mono, etc.)
    gaddr soinfo_addr = misc_alloc(sizeof(GuestSoInfo));
    auto* si = hp<GuestSoInfo>(soinfo_addr);
    memset(si, 0, sizeof(GuestSoInfo));
    strncpy(si->name, mod->soname.c_str(), sizeof(si->name) - 1);
    si->phdr = base + eh->e_phoff;
    si->phnum = eh->e_phnum;
    si->entry = base + eh->e_entry;
    si->base = base;
    si->size = mod->image_size;
    si->dynamic = dyn ? (base + dyn->p_vaddr) : 0;
    si->flags = 2; // FLAG_LINKED
    si->strtab = strtab;
    si->symtab = symtab;
    si->nbucket = nbucket;
    si->nchain = nchain;
    si->bucket = bucket;
    si->chain = chain;
    si->plt_rel = jmprel;
    si->plt_rel_count = pltrelsz / sizeof(Elf32_Rel);
    si->rel = rel;
    si->rel_count = relsz / sizeof(Elf32_Rel);
    si->init_array = init_array;
    si->init_array_count = init_array_sz / sizeof(gaddr);
    si->init_func = init_fn;
    si->refcount = 1;
    si->constructors_called = 1;
    si->load_bias = 0;

    static gaddr s_last_soinfo = 0;
    if (s_last_soinfo) {
        hp<GuestSoInfo>(s_last_soinfo)->next = soinfo_addr;
    }
    s_last_soinfo = soinfo_addr;
    mod->soinfo = soinfo_addr;

    NB_LOGI("guest library %s loaded at %#x (%u bytes, %zu exports, soinfo %#x)", mod->soname.c_str(), base,
            mod->image_size, mod->exports.size(), soinfo_addr);

    if (init_fn) call_guest(init_fn, {});
    for (u32 i = 0; i < init_array_sz / 4; ++i) {
        gaddr f = rd<u32>(init_array + i * 4);
        if (f && f != ~0u) call_guest(f, {});
    }
    NB_LOGI("guest static constructors done (%u) for %s", init_array_sz / 4, mod->soname.c_str());
    return mod;
}

}  // namespace

u32 guest_image_id() {
    return g_modules.empty() ? 0 : g_modules.back()->image_id;
}

const char* guest_main_lib_name() {
    return g_modules.empty() ? "" : g_modules.back()->soname.c_str();
}

bool guest_is_code(gaddr pc) {
    for (const auto& mod : g_modules) {
        if (pc >= mod->code_lo && pc < mod->code_hi) return true;
    }
    return false;
}

gaddr guest_sym(const char* name) {
    for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
        auto exp = (*it)->exports.find(name);
        if (exp != (*it)->exports.end()) return exp->second;
    }
    return 0;
}

gaddr guest_last_jni_onload() {
    if (g_modules.empty()) return 0;
    return g_modules.back()->jni_onload;
}

gaddr resolve_import(const char* name) {
    if (should_override_with_host(name)) {
        auto it = g_stub_cache.find(name);
        if (it != g_stub_cache.end()) return it->second;
        Handler fn = find_handler(name);
        if (fn) {
            gaddr stub = make_stub(strdup(name), fn);
            g_stub_cache[name] = stub;
            return stub;
        }
    }
    if (gaddr d = find_data(name)) return d;
    for (const auto& mod : g_modules) {
        auto exp = mod->exports.find(name);
        if (exp != mod->exports.end()) return exp->second;
    }
    auto it = g_stub_cache.find(name);
    if (it != g_stub_cache.end()) return it->second;
    Handler fn = find_handler(name);
    if (!fn) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_WARN, "VemodBridge", "Unimplemented import: '%s'", name);
#endif
        NB_LOGW("import '%s' has no implementation; calls will abort", name);
        fn = missing_import;
    }
    gaddr stub = make_stub(strdup(name), fn);
    g_stub_cache[name] = stub;
    return stub;
}

void guest_exidx(gaddr pc, gaddr* start, u32* count) {
    for (const auto& mod : g_modules) {
        if (pc >= mod->base && pc < mod->base + mod->image_size) {
            *start = mod->exidx;
            *count = mod->exidx_count;
            return;
        }
    }
    if (!g_modules.empty()) {
        *start = g_modules.back()->exidx;
        *count = g_modules.back()->exidx_count;
    } else {
        *start = 0;
        *count = 0;
    }
}

std::string symbolize(gaddr pc) {
    char buf[512];
    for (const auto& mod : g_modules) {
        if (pc >= mod->base && pc < mod->base + mod->image_size) {
            if (mod->funcs.empty()) {
                snprintf(buf, sizeof buf, "%#x (%s+%#x)", pc, mod->soname.c_str(), pc - mod->base);
                return buf;
            }
            auto it = std::upper_bound(mod->funcs.begin(), mod->funcs.end(), std::make_pair(pc, ~0u));
            if (it == mod->funcs.begin()) {
                snprintf(buf, sizeof buf, "%#x (%s+%#x)", pc, mod->soname.c_str(), pc - mod->base);
                return buf;
            }
            --it;
            const char* mangled = hs(it->second);
            int st = 0;
            char* dem = abi::__cxa_demangle(mangled, nullptr, nullptr, &st);
            snprintf(buf, sizeof buf, "%#x (%s+%#x) %s+%#x", pc, mod->soname.c_str(), pc - mod->base,
                     dem ? dem : mangled, pc - it->first);
            free(dem);
            return buf;
        }
    }
    snprintf(buf, sizeof buf, "%#x", pc);
    return buf;
}

const std::unordered_map<std::string, gaddr>& get_guest_exports() {
    g_merged_exports.clear();
    for (const auto& mod : g_modules) {
        if (mod->soname == "libbionic_guest.so") continue;
        for (const auto& [name, addr] : mod->exports) {
            g_merged_exports[name] = addr;
        }
    }
    for (const auto& mod : g_modules) {
        if (mod->soname == "libbionic_guest.so") {
            for (const auto& [name, addr] : mod->exports) {
                g_merged_exports[name] = addr;
            }
            break;
        }
    }
    return g_merged_exports;
}

void set_sysroot_dir(const char* dir) {
    if (dir) s_sysroot_dir = dir;
}

const std::string& get_sysroot_dir() {
    return s_sysroot_dir;
}

const std::string& get_guest_lib_dir() {
    return s_guest_lib_dir;
}

static std::string s_container_dir;
static std::string s_guest_apk_path;

void set_container_dir(const char* dir) {
    if (dir) {
        s_container_dir = dir;
        s_guest_apk_path = s_container_dir + "/base.apk";
    }
}

const std::string& get_container_dir() {
    return s_container_dir;
}

const std::string& get_guest_apk_path() {
    return s_guest_apk_path;
}

bool is_proc_self_exe(const char* path) {
    if (!path) return false;
    return strcmp(path, "/proc/self/exe") == 0 ||
           strcmp(path, "/proc/thread-self/exe") == 0 ||
           strcmp(path, "/proc/self/exe/") == 0;
}

bool is_sysroot_loaded() {
    return s_sysroot_loaded;
}

void preload_sysroot(const char* dir) {
    if (s_sysroot_loaded) return;
    if (dir && *dir) s_sysroot_dir = dir;
    if (s_sysroot_dir.empty()) return;

    runtime_init();
    s_sysroot_loading = true;
    setup_kernel_args(cur_thread());

    static const char* kSysrootLibs[] = {
        "libbionic_guest.so",
        "libc.so",
        "libm.so",
        "libdl.so",
        "libstdc++.so",
        "libz.so",
        "libc++.so",
        "liblog.so",
        "libcutils.so",
        "libEGL.so",
        "libGLESv1_CM.so",
        "libGLESv2.so",
        "libGLESv3.so",
        "libandroid.so",
        "libjnigraphics.so",
        "libmediandk.so",
        "libOpenSLES.so"
    };

    for (const char* lib : kSysrootLibs) {
        std::string full_path = s_sysroot_dir + "/" + lib;
        if (access(full_path.c_str(), R_OK) == 0) {
            NB_LOGI("Preloading sysroot library: %s", full_path.c_str());
            load_guest_library(full_path.c_str());
        } else {
            NB_LOGW("Sysroot library not found: %s", full_path.c_str());
        }
    }
    s_sysroot_loaded = true;
    s_sysroot_loading = false;
    init_opensles_iids();
    NB_LOGI("Sysroot preload completed (%zu modules active)", g_modules.size());
}

bool load_guest_library(const char* filepath) {
    runtime_init();

    if (!s_sysroot_loaded && !s_sysroot_loading && !s_sysroot_dir.empty()) {
        preload_sysroot(s_sysroot_dir.c_str());
    }

    std::string p(filepath);
    auto slash = p.rfind('/');
    std::string base_name = (slash != std::string::npos) ? p.substr(slash + 1) : p;
    for (const auto& mod : g_modules) {
        if (mod->soname == base_name || mod->path == filepath) {
            NB_LOGI("guest library %s already loaded at %#x", filepath, mod->base);
            return true;
        }
    }

    if (s_loading_libs.count(base_name)) {
        NB_LOGI("guest library %s is currently being loaded (circular dependency handled)", base_name.c_str());
        return true;
    }

    struct LoadingGuard {
        std::string name;
        LoadingGuard(const std::string& n) : name(n) { s_loading_libs.insert(name); }
        ~LoadingGuard() { s_loading_libs.erase(name); }
    } guard(base_name);

    if (slash != std::string::npos) {
        s_guest_lib_dir = p.substr(0, slash);
    }

    std::ifstream f(filepath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        NB_LOGE("Failed to open guest library: %s", filepath);
        return false;
    }
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> buf(size);
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
        NB_LOGE("Failed to read guest library: %s", filepath);
        return false;
    }

    gaddr base = s_next_lib_base;
    NB_LOGI("Loading guest library %s at base %#x (%zu bytes)", filepath, base, buf.size());
    auto mod = load_image(buf.data(), buf.size(), base, filepath);
    s_next_lib_base = std::max(s_next_lib_base, page_round_up(base + mod->image_size + 0x100000));
    g_modules.push_back(mod);
    if (!s_sysroot_loading) {
        warmup_precompile();
    }
    return true;
}

u32 guest_dlopen(const char* name, int flags) {
    NB_LOGI("guest dlopen(\"%s\", 0x%x)", name ? name : "(null)", flags);
    if (!name) return 0x00d1ffff;

    if (strncmp(name, "/assets/", 8) == 0 || strncmp(name, "assets/", 7) == 0) {
        return 0;
    }

    std::string req(name);
    auto slash = req.rfind('/');
    std::string base_name = (slash != std::string::npos) ? req.substr(slash + 1) : req;

    if (base_name == "libGLESv2.so") {
        if (s_guest_lib_dir.find("com.polarbit.RecklessRacingLite") != std::string::npos) {
            NB_LOGI("guest dlopen(\"libGLESv2.so\") -> returning NULL for Reckless Racing GLES 1.1 fallback");
            return 0;
        }
    }


    for (size_t i = 0; i < g_modules.size(); ++i) {
        if (g_modules[i]->soname == base_name || g_modules[i]->path == name ||
            (g_modules[i]->path.length() >= base_name.length() &&
             g_modules[i]->path.compare(g_modules[i]->path.length() - base_name.length(), base_name.length(), base_name) == 0)) {
            u32 hdl = g_modules[i]->soinfo ? g_modules[i]->soinfo : (0x00d10001 + static_cast<u32>(i));
            NB_LOGI("guest dlopen(\"%s\") -> matched module %zu (%s) handle %#x", name, i, g_modules[i]->soname.c_str(), hdl);
            return hdl;
        }
    }

    if (!s_guest_lib_dir.empty()) {
        std::string full_path = s_guest_lib_dir + "/" + base_name;
        if (access(full_path.c_str(), R_OK) == 0 && load_guest_library(full_path.c_str())) {
            size_t idx = g_modules.size() - 1;
            u32 hdl = g_modules[idx]->soinfo ? g_modules[idx]->soinfo : (0x00d10001 + static_cast<u32>(idx));
            NB_LOGI("guest dlopen(\"%s\") -> dynamically loaded module %zu (%s) handle %#x", name, idx, full_path.c_str(), hdl);
            return hdl;
        }
    }

    if (!s_sysroot_dir.empty()) {
        std::string full_path = s_sysroot_dir + "/" + base_name;
        if (access(full_path.c_str(), R_OK) == 0 && load_guest_library(full_path.c_str())) {
            size_t idx = g_modules.size() - 1;
            u32 hdl = g_modules[idx]->soinfo ? g_modules[idx]->soinfo : (0x00d10001 + static_cast<u32>(idx));
            NB_LOGI("guest dlopen(\"%s\") -> dynamically loaded sysroot module %zu (%s) handle %#x", name, idx, full_path.c_str(), hdl);
            return hdl;
        }
    }

    NB_LOGW("guest dlopen(\"%s\") failed to find module", name);
    return 0;
}

u32 guest_dlsym(u32 handle, const char* name) {
    if (!name) return 0;

    // Handle JNI_OnLoad lookup (allow resolving real target library, prevent recursion into loader)
    if (strcmp(name, "JNI_OnLoad") == 0) {
        for (const auto& mod : g_modules) {
            if (mod->soinfo && mod->soinfo == handle) {
                auto it = mod->exports.find(name);
                if (it != mod->exports.end()) return it->second;
            }
        }
        if (handle >= 0x00d10001 && handle < 0x00d1ffff) {
            size_t idx = handle - 0x00d10001;
            if (idx < g_modules.size()) {
                auto it = g_modules[idx]->exports.find(name);
                if (it != g_modules[idx]->exports.end()) return it->second;
            }
        }
        // Fallback for Unity embedded linker: return JNI_OnLoad of the real extracted library
        for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
            auto exp = (*it)->exports.find(name);
            if (exp != (*it)->exports.end()) {
                if ((*it)->image_size > 100000 || (*it)->path.find("files") != std::string::npos || (*it)->path.find("storage") != std::string::npos) {
                    NB_LOGI("guest dlsym(\"JNI_OnLoad\") -> real module %s 0x%08x", (*it)->soname.c_str(), exp->second);
                    return exp->second;
                }
            }
        }
        return 0;
    }

    if (should_override_with_host(name)) {
        return resolve_import(name);
    }

    // Prioritize custom bionic overrides
    for (const auto& m : g_modules) {
        if (m->soname == "libbionic_guest.so") {
            auto exp = m->exports.find(name);
            if (exp != m->exports.end()) {
                NB_LOGI("guest dlsym(\"%s\") -> custom bionic 0x%08x", name, exp->second);
                return exp->second;
            }
            break;
        }
    }

    // Specific module lookup by soinfo pointer
    for (const auto& mod : g_modules) {
        if (mod->soinfo && mod->soinfo == handle) {
            auto it = mod->exports.find(name);
            if (it != mod->exports.end()) {
                NB_LOGI("guest dlsym(soinfo %s, \"%s\") -> 0x%08x", mod->soname.c_str(), name, it->second);
                return it->second;
            }
        }
    }

    // Specific module lookup by index handle
    if (handle >= 0x00d10001 && handle < 0x00d1ffff) {
        size_t idx = handle - 0x00d10001;
        if (idx < g_modules.size()) {
            auto it = g_modules[idx]->exports.find(name);
            if (it != g_modules[idx]->exports.end()) {
                NB_LOGI("guest dlsym(module %s, \"%s\") -> 0x%08x", g_modules[idx]->soname.c_str(), name, it->second);
                return it->second;
            }
        }
    }

    // Global / fallback search across all modules (reverse order)
    for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
        auto exp = (*it)->exports.find(name);
        if (exp != (*it)->exports.end()) {
            NB_LOGI("guest dlsym(\"%s\") -> module %s 0x%08x", name, (*it)->soname.c_str(), exp->second);
            return exp->second;
        }
    }

    // Fall back to host bridge imports / stubs ONLY if an implementation exists
    if (find_handler(name) || find_data(name)) {
        gaddr sym = resolve_import(name);
        if (sym) {
            NB_LOGI("guest dlsym(\"%s\") -> stub 0x%08x", name, sym);
            return sym;
        }
    }

    NB_LOGW("guest dlsym(\"%s\") not found anywhere", name);
    return 0;
}

int guest_dlclose(u32 handle) {
    NB_LOGI("guest dlclose(0x%08x)", handle);
    return 0;
}

void runtime_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        mem_init();
        cpu_init();
        register_libc();
        register_stdio();
        register_pthread();
        register_gles();
        register_zlib();
        register_android();
        register_opensles();
        register_jni();
    });
}

}  // namespace nb