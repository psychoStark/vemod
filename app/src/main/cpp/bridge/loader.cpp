#include <cxxabi.h>
#include <elf.h>
#include <sys/mman.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
    std::unordered_map<std::string, gaddr> exports;
    std::vector<std::pair<gaddr, gaddr>> funcs;  // (address, guest name pointer), sorted
};

std::vector<std::shared_ptr<GuestModule>> g_modules;
std::string s_guest_lib_dir;
std::unordered_map<std::string, gaddr> g_merged_exports;
std::unordered_map<std::string, gaddr> g_stub_cache;
gaddr s_next_lib_base = layout::kLibBase;

void missing_import(Ctx& c) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "VemodBridge", "GUEST CRASH: unimplemented import called: '%s'", c.name);
#endif
    c.cpu.dump("missing import");
    fatal("guest called unimplemented import '%s'", c.name);
}

u32 arm_imm(u32 w) {  // ARM data-processing modified immediate
    u32 rot = ((w >> 8) & 15) * 2, imm = w & 0xff;
    return rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
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
    mod->base = base;
    mod->image_size = page_round_up(max_va);
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
    u32 relsz = 0, pltrelsz = 0, init_array_sz = 0, nsyms = 0;
    for (gaddr d = base + dyn->p_vaddr;; d += 8) {
        s32 tag = rd<s32>(d);
        u32 val = rd<u32>(d + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_SYMTAB: symtab = base + val; break;
        case DT_STRTAB: strtab = base + val; break;
        case DT_SONAME: mod->soname = hs(base + val); break;
        case DT_HASH: nsyms = rd<u32>(base + val + 4); break;
        case DT_REL: rel = base + val; break;
        case DT_RELSZ: relsz = val; break;
        case DT_JMPREL: jmprel = base + val; break;
        case DT_PLTRELSZ: pltrelsz = val; break;
        case DT_INIT_ARRAY: init_array = base + val; break;
        case DT_INIT_ARRAYSZ: init_array_sz = val; break;
        case DT_INIT: init_fn = base + val; break;
        case DT_NEEDED: break;
        default: break;
        }
    }

    auto sym = [&](u32 i) { return hp<Elf32_Sym>(symtab + i * sizeof(Elf32_Sym)); };
    for (u32 i = 1; i < nsyms; ++i) {
        const Elf32_Sym* s = sym(i);
        if (s->st_shndx == SHN_UNDEF) continue;
        int bind = ELF32_ST_BIND(s->st_info), type = ELF32_ST_TYPE(s->st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        const char* sym_name = hs(strtab + s->st_name);
        mod->exports[sym_name] = base + s->st_value;
        if (strcmp(sym_name, "JNI_OnLoad") == 0) {
            mod->jni_onload = base + s->st_value;
        }
        if (type == STT_FUNC) mod->funcs.push_back({(base + s->st_value) & ~1u, strtab + s->st_name});
    }
    std::sort(mod->funcs.begin(), mod->funcs.end());

    std::vector<gaddr> resolved(nsyms, ~0u);
    auto sym_addr = [&](u32 i) -> gaddr {
        if (resolved[i] != ~0u) return resolved[i];
        const Elf32_Sym* s = sym(i);
        gaddr a = 0;
        if (s->st_shndx != SHN_UNDEF) {
            a = base + s->st_value;
        } else {
            const char* name = hs(strtab + s->st_name);
            // 1. Search existing guest modules
            for (const auto& m : g_modules) {
                auto it = m->exports.find(name);
                if (it != m->exports.end()) {
                    a = it->second;
                    break;
                }
            }
            // 2. Fall back to host bridge imports / stubs
            if (!a) {
                a = resolve_import(name);
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
    apply(rel, relsz);
    apply(jmprel, pltrelsz);
    patch_plt(img, size, base);
    NB_LOGI("guest library %s loaded at %#x (%u bytes, %zu exports)", mod->soname.c_str(), base,
            mod->image_size, mod->exports.size());

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
        for (const auto& [name, addr] : mod->exports) {
            g_merged_exports[name] = addr;
        }
    }
    return g_merged_exports;
}

bool load_guest_library(const char* filepath) {
    runtime_init();

    std::string p(filepath);
    auto slash = p.rfind('/');
    std::string base_name = (slash != std::string::npos) ? p.substr(slash + 1) : p;
    for (const auto& mod : g_modules) {
        if (mod->soname == base_name || mod->path == filepath) {
            NB_LOGI("guest library %s already loaded at %#x", filepath, mod->base);
            return true;
        }
    }

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
    s_next_lib_base = page_round_up(base + mod->image_size + 0x100000); // 1MB spacing
    g_modules.push_back(mod);
    return true;
}

u32 guest_dlopen(const char* name, int flags) {
    NB_LOGI("guest dlopen(\"%s\", 0x%x)", name ? name : "(null)", flags);
    if (!name) return 0x00d1ffff;

    std::string req(name);
    auto slash = req.rfind('/');
    std::string base_name = (slash != std::string::npos) ? req.substr(slash + 1) : req;

    for (size_t i = 0; i < g_modules.size(); ++i) {
        if (g_modules[i]->soname == base_name || g_modules[i]->path == name ||
            (g_modules[i]->path.length() >= base_name.length() &&
             g_modules[i]->path.compare(g_modules[i]->path.length() - base_name.length(), base_name.length(), base_name) == 0)) {
            NB_LOGI("guest dlopen(\"%s\") -> matched module %zu (%s)", name, i, g_modules[i]->soname.c_str());
            return 0x00d10001 + static_cast<u32>(i);
        }
    }

    if (!s_guest_lib_dir.empty()) {
        std::string full_path = s_guest_lib_dir + "/" + base_name;
        if (load_guest_library(full_path.c_str())) {
            size_t idx = g_modules.size() - 1;
            NB_LOGI("guest dlopen(\"%s\") -> dynamically loaded module %zu (%s)", name, idx, full_path.c_str());
            return 0x00d10001 + static_cast<u32>(idx);
        }
    }

    NB_LOGW("guest dlopen(\"%s\") failed to find module", name);
    return 0;
}

u32 guest_dlsym(u32 handle, const char* name) {
    if (!name) return 0;

    // Specific module lookup
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

    // Fall back to host bridge imports / stubs
    gaddr sym = resolve_import(name);
    if (sym) {
        NB_LOGI("guest dlsym(\"%s\") -> stub 0x%08x", name, sym);
        return sym;
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
        register_jni();
    });
}

}  // namespace nb