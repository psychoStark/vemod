// OpenGL ES 1.1 / 2.0 / 3.0 & EGL imports.
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <sys/mman.h>

#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "nb.h"

namespace nb {
namespace {

thread_local GLuint t_array_buffer = 0;
thread_local GLuint t_element_array_buffer = 0;

// Frame hashing disabled for maximum 3D rendering throughput and low latency.
u64 g_frame_hash = 1469598103934665603ull;
u64 g_upload_nonce = 0;

inline void mix(const void*, size_t) {}
template <class T>
inline void mixv(T) {}

struct Attrib {
    bool enabled = false;
    GLint size = 0;
    GLenum type = 0;
    GLsizei stride = 0;
    const u8* client = nullptr;  // null when sourced from a VBO
};
Attrib g_attribs[16];

template <class R, class... A>
constexpr u32 arity(R (*)(A...)) {
    return u32(sizeof...(A));
}

// Thunk for scalar-only GL calls.
template <auto F>
void hashed(Ctx& c) {
    auto_thunk<F>(c);
}

void x_glBindBuffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER) t_array_buffer = buf;
    if (target == GL_ELEMENT_ARRAY_BUFFER) t_element_array_buffer = buf;
    glBindBuffer(target, buf);
}

void x_glDrawElements(Ctx& c) {
    GLenum mode = c.a32();
    GLsizei count = GLsizei(c.a32());
    GLenum type = c.a32();
    u32 indices = c.a32();
    const void* ptr = t_element_array_buffer ? reinterpret_cast<const void*>(uintptr_t(indices)) : (indices ? h(indices) : nullptr);
    glDrawElements(mode, count, type, ptr);
}

void x_glEnableVertexAttribArray(GLuint i) {
    if (i < 16) g_attribs[i].enabled = true;
    glEnableVertexAttribArray(i);
}
void x_glDisableVertexAttribArray(GLuint i) {
    if (i < 16) g_attribs[i].enabled = false;
    glDisableVertexAttribArray(i);
}

size_t gl_type_size(GLenum t) {
    return t == GL_BYTE || t == GL_UNSIGNED_BYTE ? 1 : t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 4;
}

void x_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    glDrawArrays(mode, first, count);
}

void x_glUniform1fv(GLint loc, GLsizei n, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 4);
    glUniform1fv(loc, n, v);
}
void x_glUniform2fv(GLint loc, GLsizei n, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 8);
    glUniform2fv(loc, n, v);
}
void x_glUniform3fv(GLint loc, GLsizei n, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 12);
    glUniform3fv(loc, n, v);
}
void x_glUniform4fv(GLint loc, GLsizei n, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 16);
    glUniform4fv(loc, n, v);
}
void x_glUniform1iv(GLint loc, GLsizei n, const GLint* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 4);
    glUniform1iv(loc, n, v);
}
void x_glUniform2iv(GLint loc, GLsizei n, const GLint* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 8);
    glUniform2iv(loc, n, v);
}
void x_glUniform3iv(GLint loc, GLsizei n, const GLint* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 12);
    glUniform3iv(loc, n, v);
}
void x_glUniform4iv(GLint loc, GLsizei n, const GLint* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 16);
    glUniform4iv(loc, n, v);
}
void x_glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean t, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 16);
    glUniformMatrix2fv(loc, n, t, v);
}
void x_glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 36);
    glUniformMatrix3fv(loc, n, t, v);
}
void x_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat* v) {
    mixv(loc);
    if (n > 0) mix(v, size_t(n) * 64);
    glUniformMatrix4fv(loc, n, t, v);
}

// Uploads always count as a change (hashing whole textures every frame would be wasteful).
void x_glTexImage2D(GLenum t, GLint l, GLint inf, GLsizei w, GLsizei hh, GLint b, GLenum f, GLenum ty, const void* px) {
    mixv(++g_upload_nonce);
    glTexImage2D(t, l, inf, w, hh, b, f, ty, px);
}
void x_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei hh, GLenum f, GLenum ty, const void* px) {
    mixv(++g_upload_nonce);
    glTexSubImage2D(t, l, x, y, w, hh, f, ty, px);
}

void x_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                             GLsizei width, GLsizei height, GLint border,
                             GLsizei imageSize, const void* data) {
    mixv(++g_upload_nonce);
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}
void x_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                GLsizei width, GLsizei height, GLenum format,
                                GLsizei imageSize, const void* data) {
    mixv(++g_upload_nonce);
    glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
}

void x_glVertexAttribPointer(Ctx& c) {
    GLuint index = c.a32();
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLboolean norm = GLboolean(c.a32());
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    // With a VBO bound the "pointer" is a byte offset into it; otherwise a client-side array.
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : h(ptr);
    if (index < 16) {
        Attrib& a = g_attribs[index];
        a.size = size;
        a.type = type;
        a.stride = stride;
        a.client = t_array_buffer ? nullptr : static_cast<const u8*>(p);
    }
    u32 key[5] = {index, u32(size), type, u32(stride), t_array_buffer ? ptr : 0};
    mix(key, sizeof key);
    glVertexAttribPointer(index, size, type, norm, stride, p);
}

void x_glBufferData(GLenum target, u32 size, const void* data, GLenum usage) {
    mixv(++g_upload_nonce);
    glBufferData(target, GLsizeiptr(size), data, usage);
}

void x_glBufferSubData(GLenum target, u32 offset, u32 size, const void* data) {
    mixv(++g_upload_nonce);
    glBufferSubData(target, GLintptr(offset), GLsizeiptr(size), data);
}

void x_glShaderSource(GLuint shader, GLsizei count, u32 strings, const GLint* lengths) {
    std::vector<const GLchar*> v(count > 0 ? size_t(count) : 0);
    for (GLsizei i = 0; i < count; ++i) v[size_t(i)] = hs(rd<u32>(strings + u32(i) * 4));
    glShaderSource(shader, count, v.data(), lengths);
}

void x_glGetVertexAttribPointerv(GLuint index, GLenum pname, u32* pointer) {
    void* ptr = nullptr;
    glGetVertexAttribPointerv(index, pname, &ptr);
    if (pointer) *pointer = static_cast<u32>(reinterpret_cast<uintptr_t>(ptr));
}

std::mutex g_str_mu;
std::unordered_map<GLenum, gaddr> g_strings;
u32 x_glGetString(GLenum name) {
    const GLubyte* s = glGetString(name);
    if (!s) return 0;
    std::lock_guard lk(g_str_mu);
    auto& slot = g_strings[name];
    if (!slot) slot = gstrdup(reinterpret_cast<const char*>(s));
    return slot;
}

static thread_local int t_gl_err_burst = 0;
GLenum x_glGetError() {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        t_gl_err_burst++;
        if (t_gl_err_burst <= 10) {
            NB_LOGE("x_glGetError() -> 0x%x (burst %d)", err, t_gl_err_burst);
            return err;
        } else {
            if (t_gl_err_burst == 11) {
                NB_LOGE("x_glGetError() -> 0x%x; suppressing further errors to break infinite loop", err);
            }
            return GL_NO_ERROR;
        }
    }
    t_gl_err_burst = 0;
    return GL_NO_ERROR;
}

// ------------------------------------------------------------- GLES 1.1 pointer wrappers
void x_glVertexPointer(Ctx& c) {
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    glVertexPointer(size, type, stride, p);
}
void x_glColorPointer(Ctx& c) {
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    glColorPointer(size, type, stride, p);
}
void x_glTexCoordPointer(Ctx& c) {
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    glTexCoordPointer(size, type, stride, p);
}
void x_glNormalPointer(GLenum type, GLsizei stride, u32 ptr) {
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    glNormalPointer(type, stride, p);
}
void x_glPointSizePointerOES(GLenum type, GLsizei stride, u32 ptr) {
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    glPointSizePointerOES(type, stride, p);
}

void x_glMatrixIndexPointerOES(Ctx& c) {
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    static auto fn = (PFNGLMATRIXINDEXPOINTEROESPROC)eglGetProcAddress("glMatrixIndexPointerOES");
    if (fn) fn(size, type, stride, p);
}

void x_glWeightPointerOES(Ctx& c) {
    GLint size = GLint(c.a32());
    GLenum type = c.a32();
    GLsizei stride = GLsizei(c.a32());
    u32 ptr = c.a32();
    const void* p = t_array_buffer ? reinterpret_cast<const void*>(uintptr_t(ptr)) : (ptr ? h(ptr) : nullptr);
    static auto fn = (PFNGLWEIGHTPOINTEROESPROC)eglGetProcAddress("glWeightPointerOES");
    if (fn) fn(size, type, stride, p);
}

void x_glCurrentPaletteMatrixOES(GLuint idx) {
    static auto fn = (PFNGLCURRENTPALETTEMATRIXOESPROC)eglGetProcAddress("glCurrentPaletteMatrixOES");
    if (fn) fn(idx);
}

void x_glLoadPaletteFromModelViewMatrixOES() {
    static auto fn = (PFNGLLOADPALETTEFROMMODELVIEWMATRIXOESPROC)eglGetProcAddress("glLoadPaletteFromModelViewMatrixOES");
    if (fn) fn();
}

void x_glDrawTexsOES(GLshort x, GLshort y, GLshort z, GLshort w, GLshort hh) {
    static auto fn = (PFNGLDRAWTEXSOESPROC)eglGetProcAddress("glDrawTexsOES");
    if (fn) fn(x, y, z, w, hh);
}
void x_glDrawTexiOES(GLint x, GLint y, GLint z, GLint w, GLint hh) {
    static auto fn = (PFNGLDRAWTEXIOESPROC)eglGetProcAddress("glDrawTexiOES");
    if (fn) fn(x, y, z, w, hh);
}
void x_glDrawTexxOES(GLfixed x, GLfixed y, GLfixed z, GLfixed w, GLfixed hh) {
    static auto fn = (PFNGLDRAWTEXXOESPROC)eglGetProcAddress("glDrawTexxOES");
    if (fn) fn(x, y, z, w, hh);
}
void x_glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat w, GLfloat hh) {
    static auto fn = (PFNGLDRAWTEXFOESPROC)eglGetProcAddress("glDrawTexfOES");
    if (fn) fn(x, y, z, w, hh);
}
void x_glDrawTexsvOES(gaddr p) {
    if (!p) return;
    const GLshort* coords = reinterpret_cast<const GLshort*>(h(p));
    static auto fn = (PFNGLDRAWTEXSVOESPROC)eglGetProcAddress("glDrawTexsvOES");
    if (fn) fn(coords);
}
void x_glDrawTexivOES(gaddr p) {
    if (!p) return;
    const GLint* coords = reinterpret_cast<const GLint*>(h(p));
    static auto fn = (PFNGLDRAWTEXIVOESPROC)eglGetProcAddress("glDrawTexivOES");
    if (fn) fn(coords);
}
void x_glDrawTexxvOES(gaddr p) {
    if (!p) return;
    const GLfixed* coords = reinterpret_cast<const GLfixed*>(h(p));
    static auto fn = (PFNGLDRAWTEXXVOESPROC)eglGetProcAddress("glDrawTexxvOES");
    if (fn) fn(coords);
}
void x_glDrawTexfvOES(gaddr p) {
    if (!p) return;
    const GLfloat* coords = reinterpret_cast<const GLfloat*>(h(p));
    static auto fn = (PFNGLDRAWTEXFVOESPROC)eglGetProcAddress("glDrawTexfvOES");
    if (fn) fn(coords);
}

void x_glTexEnvx(GLenum target, GLenum pname, GLfixed param) {
    glTexEnvx(target, pname, param);
}
void x_glLoadMatrixf(gaddr m) {
    if (m) glLoadMatrixf(reinterpret_cast<const GLfloat*>(h(m)));
}
void x_glMultMatrixf(gaddr m) {
    if (m) glMultMatrixf(reinterpret_cast<const GLfloat*>(h(m)));
}
void x_glLoadMatrixx(gaddr m) {
    if (m) glLoadMatrixx(reinterpret_cast<const GLfixed*>(h(m)));
}
void x_glMultMatrixx(gaddr m) {
    if (m) glMultMatrixx(reinterpret_cast<const GLfixed*>(h(m)));
}
void x_glMaterialfv(GLenum face, GLenum pname, gaddr p) {
    if (p) glMaterialfv(face, pname, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glMaterialxv(GLenum face, GLenum pname, gaddr p) {
    if (p) glMaterialxv(face, pname, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glLightfv(GLenum light, GLenum pname, gaddr p) {
    if (p) glLightfv(light, pname, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glLightxv(GLenum light, GLenum pname, gaddr p) {
    if (p) glLightxv(light, pname, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glLightModelfv(GLenum pname, gaddr p) {
    if (p) glLightModelfv(pname, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glLightModelxv(GLenum pname, gaddr p) {
    if (p) glLightModelxv(pname, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glTexEnvfv(GLenum target, GLenum pname, gaddr p) {
    if (p) glTexEnvfv(target, pname, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glTexEnviv(GLenum target, GLenum pname, gaddr p) {
    if (p) glTexEnviv(target, pname, reinterpret_cast<const GLint*>(h(p)));
}
void x_glTexEnvxv(GLenum target, GLenum pname, gaddr p) {
    if (p) glTexEnvxv(target, pname, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glFogfv(GLenum pname, gaddr p) {
    if (p) glFogfv(pname, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glFogxv(GLenum pname, gaddr p) {
    if (p) glFogxv(pname, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glFogi(GLenum pname, GLint param) {
    glFogf(pname, static_cast<GLfloat>(param));
}

// ------------------------------------------------------------- EGL Object Mapping
static std::mutex g_egl_mu;
static EGLDisplay g_host_display = EGL_NO_DISPLAY;
static std::unordered_map<u32, void*> g_egl_handles;
static std::unordered_map<void*, u32> g_egl_rev_handles;
static u32 g_next_egl_handle = 10;

}  // namespace

u32 to_guest_egl(void* host_ptr) {
    if (!host_ptr) return 0;
    std::lock_guard lk(g_egl_mu);
    auto it = g_egl_rev_handles.find(host_ptr);
    if (it != g_egl_rev_handles.end()) return it->second;
    u32 h_val = g_next_egl_handle++;
    g_egl_handles[h_val] = host_ptr;
    g_egl_rev_handles[host_ptr] = h_val;
    return h_val;
}

void* to_host_egl(u32 guest_h) {
    if (!guest_h) return nullptr;
    std::lock_guard lk(g_egl_mu);
    auto it = g_egl_handles.find(guest_h);
    if (it != g_egl_handles.end()) return it->second;
    return nullptr;
}

namespace {

static u32 x_eglGetDisplay(u32 display_id) {
    EGLDisplay dpy = eglGetDisplay(display_id == 0 ? EGL_DEFAULT_DISPLAY : (EGLNativeDisplayType)(uintptr_t)display_id);
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetCurrentDisplay();
    }
    if (dpy != EGL_NO_DISPLAY) {
        g_host_display = dpy;
        return to_guest_egl(dpy);
    }
    return 1; // Non-zero fallback handle to prevent [EGL] null display aborts
}

static u32 x_eglInitialize(u32 dpy_h, gaddr major_ptr, gaddr minor_ptr) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) {
        dpy = g_host_display != EGL_NO_DISPLAY ? g_host_display : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    EGLint major = 1, minor = 4;
    EGLBoolean ok = dpy != EGL_NO_DISPLAY ? eglInitialize(dpy, &major, &minor) : EGL_TRUE;
    if (ok) {
        if (major_ptr) wr<s32>(major_ptr, major);
        if (minor_ptr) wr<s32>(minor_ptr, minor);
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

static u32 x_eglTerminate(u32) {
    return EGL_TRUE;
}

static u32 x_eglQueryString(u32 dpy_h, s32 name) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    const char* s = dpy != EGL_NO_DISPLAY ? eglQueryString(dpy, name) : nullptr;
    if (!s) {
        if (name == EGL_VERSION) s = "1.4 Vemod";
        else if (name == EGL_VENDOR) s = "Android";
        else s = "";
    }
    std::lock_guard lk(g_str_mu);
    auto& slot = g_strings[0x10000 + name];
    if (!slot) slot = gstrdup(s);
    return slot;
}

static u32 x_eglGetConfigs(u32 dpy_h, gaddr configs_out, s32 config_size, gaddr num_config_out) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    std::vector<EGLConfig> host_configs(config_size > 0 ? config_size : 1);
    EGLint num_config = 0;
    EGLConfig* out_configs = (configs_out && config_size > 0) ? host_configs.data() : nullptr;
    EGLBoolean ok = eglGetConfigs(dpy, out_configs, config_size, &num_config);
    if (ok) {
        if (num_config_out) wr<s32>(num_config_out, num_config);
        if (out_configs) {
            s32 to_write = std::min(config_size, (s32)num_config);
            for (s32 i = 0; i < to_write; ++i) {
                wr<u32>(configs_out + i * 4, to_guest_egl(host_configs[i]));
            }
        }
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

static u32 x_eglChooseConfig(u32 dpy_h, gaddr attrib_list, gaddr configs_out, s32 config_size, gaddr num_config_out) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    const EGLint* attribs = attrib_list ? reinterpret_cast<const EGLint*>(h(attrib_list)) : nullptr;
    std::vector<EGLConfig> host_configs(config_size > 0 ? config_size : 1);
    EGLint num_config = 0;
    EGLConfig* out_configs = (configs_out && config_size > 0) ? host_configs.data() : nullptr;
    EGLBoolean ok = eglChooseConfig(dpy, attribs, out_configs, config_size, &num_config);
    if (ok) {
        if (num_config_out) wr<s32>(num_config_out, num_config);
        if (out_configs) {
            s32 to_write = std::min(config_size, (s32)num_config);
            for (s32 i = 0; i < to_write; ++i) {
                wr<u32>(configs_out + i * 4, to_guest_egl(host_configs[i]));
            }
        }
        return EGL_TRUE;
    }
    return EGL_FALSE;
}

static u32 x_eglGetConfigAttrib(u32 dpy_h, u32 config_h, s32 attribute, gaddr value_out) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg = (EGLConfig)to_host_egl(config_h);
    EGLint val = 0;
    EGLBoolean ok = eglGetConfigAttrib(dpy, cfg, attribute, &val);
    if (ok && value_out) wr<s32>(value_out, val);
    return ok ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglCreateContext(u32 dpy_h, u32 config_h, u32 share_ctx_h, gaddr attrib_list) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg = (EGLConfig)to_host_egl(config_h);
    EGLContext share = (EGLContext)to_host_egl(share_ctx_h);
    const EGLint* attribs = attrib_list ? reinterpret_cast<const EGLint*>(h(attrib_list)) : nullptr;
    EGLContext ctx = eglCreateContext(dpy, cfg, share, attribs);
    return to_guest_egl(ctx);
}

static u32 x_eglCreateWindowSurface(u32 dpy_h, u32 config_h, gaddr win, gaddr attrib_list) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg = (EGLConfig)to_host_egl(config_h);
    const EGLint* attribs = attrib_list ? reinterpret_cast<const EGLint*>(h(attrib_list)) : nullptr;
    ANativeWindow* native_win = (ANativeWindow*)to_host_egl(win);
    if (!native_win && win) {
        native_win = reinterpret_cast<ANativeWindow*>(h(win));
    }
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, native_win, attribs);
    if (surf == EGL_NO_SURFACE) {
        EGLint err = eglGetError();
        NB_LOGE("eglCreateWindowSurface failed: 0x%x (win=%u, host_win=%p)", err, win, native_win);
    } else {
        NB_LOGI("eglCreateWindowSurface success -> surf %p (handle %u)", surf, to_guest_egl(surf));
    }
    return to_guest_egl(surf);
}

static u32 x_eglCreatePbufferSurface(u32 dpy_h, u32 config_h, gaddr attrib_list) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg = (EGLConfig)to_host_egl(config_h);
    const EGLint* attribs = attrib_list ? reinterpret_cast<const EGLint*>(h(attrib_list)) : nullptr;
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, attribs);
    return to_guest_egl(surf);
}

static u32 x_eglDestroySurface(u32 dpy_h, u32 surf_h) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface surf = (EGLSurface)to_host_egl(surf_h);
    return eglDestroySurface(dpy, surf) ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglDestroyContext(u32 dpy_h, u32 ctx_h) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLContext ctx = (EGLContext)to_host_egl(ctx_h);
    return eglDestroyContext(dpy, ctx) ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglMakeCurrent(u32 dpy_h, u32 draw_h, u32 read_h, u32 ctx_h) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface draw = (EGLSurface)to_host_egl(draw_h);
    EGLSurface read = (EGLSurface)to_host_egl(read_h);
    EGLContext ctx = (EGLContext)to_host_egl(ctx_h);
    return eglMakeCurrent(dpy, draw, read, ctx) ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglSwapBuffers(u32 dpy_h, u32 surf_h) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface surf = (EGLSurface)to_host_egl(surf_h);
    warmup_note_frame();
    return eglSwapBuffers(dpy, surf) ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglSwapInterval(u32 dpy_h, s32 interval) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    return eglSwapInterval(dpy, interval) ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglBindAPI(u32 /*api*/) { return EGL_TRUE; }
static u32 x_eglQueryAPI() { return EGL_OPENGL_ES_API; }
static u32 x_eglWaitClient() { return EGL_TRUE; }
static u32 x_eglWaitGL() { return EGL_TRUE; }
static u32 x_eglWaitNative(s32 /*engine*/) { return EGL_TRUE; }
static u32 x_eglReleaseThread() { return EGL_TRUE; }

static u32 x_eglSurfaceAttrib(u32 dpy_h, u32 surf_h, s32 attribute, s32 value) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface surf = (EGLSurface)to_host_egl(surf_h);
    return eglSurfaceAttrib(dpy, surf, attribute, value) ? EGL_TRUE : EGL_FALSE;
}

static u32 x_eglQuerySurface(u32 dpy_h, u32 surf_h, s32 attribute, gaddr value_out) {
    EGLDisplay dpy = (EGLDisplay)to_host_egl(dpy_h);
    if (!dpy || dpy == (EGLDisplay)(uintptr_t)1) dpy = g_host_display;
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLSurface surf = (EGLSurface)to_host_egl(surf_h);
    EGLint val = 0;
    EGLBoolean ok = eglQuerySurface(dpy, surf, attribute, &val);
    if (ok && value_out) wr<s32>(value_out, val);
    return ok ? EGL_TRUE : EGL_FALSE;
}

// ------------------------------------------------------------- GLES 1.1 Vector & Getter helpers
void x_glPointParameterfv(GLenum pname, gaddr p) {
    if (p) glPointParameterfv(pname, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glPointParameterxv(GLenum pname, gaddr p) {
    if (p) glPointParameterxv(pname, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glClipPlanef(GLenum plane, gaddr p) {
    if (p) glClipPlanef(plane, reinterpret_cast<const GLfloat*>(h(p)));
}
void x_glClipPlanex(GLenum plane, gaddr p) {
    if (p) glClipPlanex(plane, reinterpret_cast<const GLfixed*>(h(p)));
}
void x_glGetLightfv(GLenum light, GLenum pname, gaddr p) {
    if (p) glGetLightfv(light, pname, reinterpret_cast<GLfloat*>(h(p)));
}
void x_glGetLightxv(GLenum light, GLenum pname, gaddr p) {
    if (p) glGetLightxv(light, pname, reinterpret_cast<GLfixed*>(h(p)));
}
void x_glGetMaterialfv(GLenum face, GLenum pname, gaddr p) {
    if (p) glGetMaterialfv(face, pname, reinterpret_cast<GLfloat*>(h(p)));
}
void x_glGetMaterialxv(GLenum face, GLenum pname, gaddr p) {
    if (p) glGetMaterialxv(face, pname, reinterpret_cast<GLfixed*>(h(p)));
}
void x_glGetTexEnvfv(GLenum env, GLenum pname, gaddr p) {
    if (p) glGetTexEnvfv(env, pname, reinterpret_cast<GLfloat*>(h(p)));
}
void x_glGetTexEnviv(GLenum env, GLenum pname, gaddr p) {
    if (p) glGetTexEnviv(env, pname, reinterpret_cast<GLint*>(h(p)));
}
void x_glGetTexEnvxv(GLenum env, GLenum pname, gaddr p) {
    if (p) glGetTexEnvxv(env, pname, reinterpret_cast<GLfixed*>(h(p)));
}
void x_glGetClipPlanef(GLenum pname, gaddr p) {
    if (p) glGetClipPlanef(pname, reinterpret_cast<GLfloat*>(h(p)));
}
void x_glGetClipPlanex(GLenum pname, gaddr p) {
    if (p) glGetClipPlanex(pname, reinterpret_cast<GLfixed*>(h(p)));
}
void x_glGetFixedv(GLenum pname, gaddr p) {
    if (p) glGetFixedv(pname, reinterpret_cast<GLfixed*>(h(p)));
}
void x_glGetPointerv(GLenum /*pname*/, gaddr p) {
    if (p) wr<u32>(p, 0);
}

static gaddr x_eglGetProcAddress(gaddr procname_addr) {
    if (!procname_addr) return 0;
    const char* name = hs(procname_addr);
    if (strcmp(name, "glAlphaFuncQCOM") == 0) {
        return resolve_import("glAlphaFuncQCOM");
    }
    if (strcmp(name, "glAlphaFunc") == 0) {
        return resolve_import("glAlphaFunc");
    }
    Handler h_fn = find_handler(name);
    if (h_fn) {
        NB_LOGI("eglGetProcAddress(\"%s\") -> handler found", name);
        return resolve_import(name);
    }
    void* host_ptr = (void*)eglGetProcAddress(name);
    if (host_ptr) {
        NB_LOGI("eglGetProcAddress(\"%s\") -> host GPU symbol found", name);
        return resolve_import(name);
    }
    if (strncmp(name, "gl", 2) == 0 || strncmp(name, "egl", 3) == 0) {
        NB_LOGW("eglGetProcAddress(\"%s\") -> fallback stub created", name);
        return resolve_import(name);
    }
    NB_LOGW("eglGetProcAddress(\"%s\") -> not found", name);
    return 0;
}

static u32 x_eglGetCurrentContext() {
    EGLContext ctx = eglGetCurrentContext();
    return to_guest_egl(ctx);
}
static u32 x_eglGetCurrentDisplay() {
    EGLDisplay dpy = eglGetCurrentDisplay();
    if (dpy != EGL_NO_DISPLAY) g_host_display = dpy;
    return to_guest_egl(dpy);
}
static u32 x_eglGetCurrentSurface(int readdraw) {
    EGLSurface surf = eglGetCurrentSurface(readdraw);
    return to_guest_egl(surf);
}
static int x_eglGetError() {
    return eglGetError();
}

static PFNGLBINDVERTEXARRAYOESPROC p_glBindVertexArrayOES = nullptr;
static PFNGLDELETEVERTEXARRAYSOESPROC p_glDeleteVertexArraysOES = nullptr;
static PFNGLGENVERTEXARRAYSOESPROC p_glGenVertexArraysOES = nullptr;
static PFNGLISVERTEXARRAYOESPROC p_glIsVertexArrayOES = nullptr;

static void x_glBindVertexArrayOES(GLuint array) {
    if (!p_glBindVertexArrayOES) p_glBindVertexArrayOES = (PFNGLBINDVERTEXARRAYOESPROC)eglGetProcAddress("glBindVertexArrayOES");
    if (p_glBindVertexArrayOES) p_glBindVertexArrayOES(array);
}
static void x_glDeleteVertexArraysOES(GLsizei n, gaddr arrays) {
    if (!p_glDeleteVertexArraysOES) p_glDeleteVertexArraysOES = (PFNGLDELETEVERTEXARRAYSOESPROC)eglGetProcAddress("glDeleteVertexArraysOES");
    if (p_glDeleteVertexArraysOES && arrays) p_glDeleteVertexArraysOES(n, reinterpret_cast<GLuint*>(h(arrays)));
}
static void x_glGenVertexArraysOES(GLsizei n, gaddr arrays) {
    if (!p_glGenVertexArraysOES) p_glGenVertexArraysOES = (PFNGLGENVERTEXARRAYSOESPROC)eglGetProcAddress("glGenVertexArraysOES");
    if (p_glGenVertexArraysOES && arrays) p_glGenVertexArraysOES(n, reinterpret_cast<GLuint*>(h(arrays)));
}
static GLboolean x_glIsVertexArrayOES(GLuint array) {
    if (!p_glIsVertexArrayOES) p_glIsVertexArrayOES = (PFNGLISVERTEXARRAYOESPROC)eglGetProcAddress("glIsVertexArrayOES");
    return p_glIsVertexArrayOES ? p_glIsVertexArrayOES(array) : GL_FALSE;
}

// Fixed-point texture parameters (OpenGL ES 1.1 fixed-function pipeline)
static void x_glTexParameterx(GLenum target, GLenum pname, GLfixed param) {
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case 0x8191: /* GL_GENERATE_MIPMAP */
            glTexParameteri(target, pname, param);
            break;
        default:
            glTexParameterf(target, pname, static_cast<GLfloat>(param) / 65536.0f);
            break;
    }
}

static void x_glTexParameterxv(GLenum target, GLenum pname, gaddr params_addr) {
    if (!params_addr || !is_mapped(params_addr)) return;
    GLfixed val = rd<GLfixed>(params_addr);
    x_glTexParameterx(target, pname, val);
}

static void x_glGetTexParameterxv(GLenum target, GLenum pname, gaddr params_addr) {
    if (!params_addr || !is_mapped(params_addr)) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case 0x8191: /* GL_GENERATE_MIPMAP */ {
            GLint ival = 0;
            glGetTexParameteriv(target, pname, &ival);
            wr<GLfixed>(params_addr, ival);
            break;
        }
        default: {
            GLfloat fval = 0.0f;
            glGetTexParameterfv(target, pname, &fval);
            wr<GLfixed>(params_addr, static_cast<GLfixed>(fval * 65536.0f));
            break;
        }
    }
}

// Instanced rendering (Hardware instancing extensions)
static void x_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    static auto fn = (PFNGLDRAWARRAYSINSTANCEDPROC)eglGetProcAddress("glDrawArraysInstanced");
    if (!fn) fn = (PFNGLDRAWARRAYSINSTANCEDPROC)eglGetProcAddress("glDrawArraysInstancedEXT");
    if (!fn) fn = (PFNGLDRAWARRAYSINSTANCEDPROC)eglGetProcAddress("glDrawArraysInstancedNV");
    if (fn) {
        fn(mode, first, count, instancecount);
    } else {
        for (GLsizei i = 0; i < instancecount; ++i) {
            glDrawArrays(mode, first, count);
        }
    }
}

static void x_glDrawElementsInstanced(Ctx& c) {
    GLenum mode = c.a32();
    GLsizei count = GLsizei(c.a32());
    GLenum type = c.a32();
    u32 indices = c.a32();
    GLsizei instancecount = GLsizei(c.a32());
    const void* ptr = t_element_array_buffer ? reinterpret_cast<const void*>(uintptr_t(indices)) : (indices ? h(indices) : nullptr);
    static auto fn = (PFNGLDRAWELEMENTSINSTANCEDPROC)eglGetProcAddress("glDrawElementsInstanced");
    if (!fn) fn = (PFNGLDRAWELEMENTSINSTANCEDPROC)eglGetProcAddress("glDrawElementsInstancedEXT");
    if (!fn) fn = (PFNGLDRAWELEMENTSINSTANCEDPROC)eglGetProcAddress("glDrawElementsInstancedNV");
    if (fn) {
        fn(mode, count, type, ptr, instancecount);
    } else {
        for (GLsizei i = 0; i < instancecount; ++i) {
            glDrawElements(mode, count, type, ptr);
        }
    }
}

static void x_glVertexAttribDivisor(GLuint index, GLuint divisor) {
    static auto fn = (PFNGLVERTEXATTRIBDIVISORPROC)eglGetProcAddress("glVertexAttribDivisor");
    if (!fn) fn = (PFNGLVERTEXATTRIBDIVISORPROC)eglGetProcAddress("glVertexAttribDivisorEXT");
    if (!fn) fn = (PFNGLVERTEXATTRIBDIVISORPROC)eglGetProcAddress("glVertexAttribDivisorNV");
    if (fn) fn(index, divisor);
}

// 32-bit Buffer Mapping with shadow memory to avoid 64-bit pointer truncation
struct MappedBufferRecord {
    GLenum target;
    GLintptr offset;
    GLsizeiptr length;
    GLbitfield access;
    void* host_ptr;
    gaddr guest_addr;
};
static std::mutex g_map_buf_mu;
static std::unordered_map<GLenum, MappedBufferRecord> g_mapped_buffers;

static gaddr x_glMapBufferRangeEXT(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    static auto fn = (PFNGLMAPBUFFERRANGEEXTPROC)eglGetProcAddress("glMapBufferRangeEXT");
    if (!fn) fn = (PFNGLMAPBUFFERRANGEEXTPROC)eglGetProcAddress("glMapBufferRange");
    if (!fn || length <= 0) return 0;
    void* host_ptr = fn(target, offset, length, access);
    if (!host_ptr) return 0;

    std::lock_guard lk(g_map_buf_mu);
    u32 alloc_sz = host_page_round_up(static_cast<u32>(length));
    gaddr ga = map_alloc(alloc_sz, PROT_READ | PROT_WRITE);
    if (!ga) {
        NB_LOGE("glMapBufferRange: failed to allocate guest shadow buffer (%u bytes)", alloc_sz);
        static auto unmap_fn = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBufferOES");
        if (!unmap_fn) unmap_fn = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBuffer");
        if (unmap_fn) unmap_fn(target);
        return 0;
    }
    if (access & 0x0001 /* GL_MAP_READ_BIT */) {
        memcpy(h(ga), host_ptr, length);
    }
    g_mapped_buffers[target] = {target, offset, length, access, host_ptr, ga};
    return ga;
}

static gaddr x_glMapBufferOES(GLenum target, GLenum access) {
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (size <= 0) return 0;
    GLbitfield flags = 0;
    if (access == 0x88B8 /* GL_READ_ONLY */) flags = 0x0001;
    else if (access == 0x88B9 /* GL_WRITE_ONLY */) flags = 0x0002;
    else flags = 0x0001 | 0x0002;
    return x_glMapBufferRangeEXT(target, 0, size, flags);
}

static GLboolean x_glUnmapBufferOES(GLenum target) {
    std::lock_guard lk(g_map_buf_mu);
    auto it = g_mapped_buffers.find(target);
    if (it == g_mapped_buffers.end()) return GL_TRUE;
    auto& rec = it->second;
    if ((rec.access & 0x0002 /* GL_MAP_WRITE_BIT */) && rec.host_ptr && rec.guest_addr) {
        memcpy(rec.host_ptr, h(rec.guest_addr), rec.length);
    }
    static auto unmap_fn = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBufferOES");
    if (!unmap_fn) unmap_fn = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBuffer");
    GLboolean res = unmap_fn ? unmap_fn(target) : GL_TRUE;
    u32 alloc_sz = host_page_round_up(static_cast<u32>(rec.length));
    map_free(rec.guest_addr, alloc_sz);
    g_mapped_buffers.erase(it);
    return res;
}

static void x_glFlushMappedBufferRangeEXT(GLenum target, GLintptr offset, GLsizeiptr length) {
    std::lock_guard lk(g_map_buf_mu);
    auto it = g_mapped_buffers.find(target);
    if (it != g_mapped_buffers.end() && it->second.host_ptr && it->second.guest_addr) {
        if (offset + length <= it->second.length) {
            memcpy(reinterpret_cast<u8*>(it->second.host_ptr) + offset,
                   reinterpret_cast<const u8*>(h(it->second.guest_addr)) + offset, length);
        }
    }
    static auto fn = (PFNGLFLUSHMAPPEDBUFFERRANGEEXTPROC)eglGetProcAddress("glFlushMappedBufferRangeEXT");
    if (!fn) fn = (PFNGLFLUSHMAPPEDBUFFERRANGEEXTPROC)eglGetProcAddress("glFlushMappedBufferRange");
    if (fn) fn(target, offset, length);
}

// Framebuffer blit & discard
static void x_glBlitFramebufferNV(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                  GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                  GLbitfield mask, GLenum filter) {
    static auto fn = (PFNGLBLITFRAMEBUFFERPROC)eglGetProcAddress("glBlitFramebuffer");
    if (!fn) fn = (PFNGLBLITFRAMEBUFFERPROC)eglGetProcAddress("glBlitFramebufferNV");
    if (!fn) fn = (PFNGLBLITFRAMEBUFFERPROC)eglGetProcAddress("glBlitFramebufferEXT");
    if (!fn) fn = (PFNGLBLITFRAMEBUFFERPROC)eglGetProcAddress("glBlitFramebufferANGLE");
    if (fn) fn(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

static void x_glDiscardFramebufferEXT(GLenum target, GLsizei numAttachments, gaddr attachments) {
    if (!attachments || numAttachments <= 0) return;
    static auto fn = (PFNGLDISCARDFRAMEBUFFEREXTPROC)eglGetProcAddress("glDiscardFramebufferEXT");
    if (!fn) fn = (PFNGLDISCARDFRAMEBUFFEREXTPROC)eglGetProcAddress("glInvalidateFramebuffer");
    if (fn) {
        fn(target, numAttachments, reinterpret_cast<const GLenum*>(h(attachments)));
    }
}

// Queries
static void x_glGenQueriesEXT(GLsizei n, gaddr ids) {
    if (!ids || n <= 0) return;
    static auto fn = (PFNGLGENQUERIESEXTPROC)eglGetProcAddress("glGenQueriesEXT");
    if (!fn) fn = (PFNGLGENQUERIESEXTPROC)eglGetProcAddress("glGenQueries");
    if (fn) fn(n, reinterpret_cast<GLuint*>(h(ids)));
}

static void x_glDeleteQueriesEXT(GLsizei n, gaddr ids) {
    if (!ids || n <= 0) return;
    static auto fn = (PFNGLDELETEQUERIESEXTPROC)eglGetProcAddress("glDeleteQueriesEXT");
    if (!fn) fn = (PFNGLDELETEQUERIESEXTPROC)eglGetProcAddress("glDeleteQueries");
    if (fn) fn(n, reinterpret_cast<const GLuint*>(h(ids)));
}

static void x_glBeginQueryEXT(GLenum target, GLuint id) {
    static auto fn = (PFNGLBEGINQUERYEXTPROC)eglGetProcAddress("glBeginQueryEXT");
    if (!fn) fn = (PFNGLBEGINQUERYEXTPROC)eglGetProcAddress("glBeginQuery");
    if (fn) fn(target, id);
}

static void x_glEndQueryEXT(GLenum target) {
    static auto fn = (PFNGLENDQUERYEXTPROC)eglGetProcAddress("glEndQueryEXT");
    if (!fn) fn = (PFNGLENDQUERYEXTPROC)eglGetProcAddress("glEndQuery");
    if (fn) fn(target);
}

static void x_glGetQueryObjectuivEXT(GLuint id, GLenum pname, gaddr params) {
    if (!params) return;
    static auto fn = (PFNGLGETQUERYOBJECTUIVEXTPROC)eglGetProcAddress("glGetQueryObjectuivEXT");
    if (!fn) fn = (PFNGLGETQUERYOBJECTUIVEXTPROC)eglGetProcAddress("glGetQueryObjectuiv");
    if (fn) fn(id, pname, reinterpret_cast<GLuint*>(h(params)));
}

static void x_glGetQueryObjectivEXT(GLuint id, GLenum pname, gaddr params) {
    if (!params) return;
    static auto fn = (PFNGLGETQUERYOBJECTIVEXTPROC)eglGetProcAddress("glGetQueryObjectivEXT");
    if (!fn) fn = (PFNGLGETQUERYOBJECTIVEXTPROC)eglGetProcAddress("glGetQueryObjectiv");
    if (fn) fn(id, pname, reinterpret_cast<GLint*>(h(params)));
}

// Fences & Sync
static void x_glGenFencesNV(GLsizei n, gaddr fences) {
    if (!fences || n <= 0) return;
    static auto fn = (void(*)(GLsizei, GLuint*))eglGetProcAddress("glGenFencesNV");
    if (fn) {
        fn(n, reinterpret_cast<GLuint*>(h(fences)));
    } else {
        static GLuint s_fence_id = 1;
        GLuint* ptr = reinterpret_cast<GLuint*>(h(fences));
        for (GLsizei i = 0; i < n; ++i) ptr[i] = s_fence_id++;
    }
}

static void x_glDeleteFencesNV(GLsizei n, gaddr fences) {
    if (!fences || n <= 0) return;
    static auto fn = (void(*)(GLsizei, const GLuint*))eglGetProcAddress("glDeleteFencesNV");
    if (fn) fn(n, reinterpret_cast<const GLuint*>(h(fences)));
}

static void x_glSetFenceNV(GLuint fence, GLenum condition) {
    static auto fn = (void(*)(GLuint, GLenum))eglGetProcAddress("glSetFenceNV");
    if (fn) fn(fence, condition);
}

static GLboolean x_glTestFenceNV(GLuint fence) {
    static auto fn = (GLboolean(*)(GLuint))eglGetProcAddress("glTestFenceNV");
    return fn ? fn(fence) : GL_TRUE;
}

static void x_glFinishFenceNV(GLuint fence) {
    static auto fn = (void(*)(GLuint))eglGetProcAddress("glFinishFenceNV");
    if (fn) fn(fence);
    else glFinish();
}

static GLboolean x_glIsFenceNV(GLuint fence) {
    static auto fn = (GLboolean(*)(GLuint))eglGetProcAddress("glIsFenceNV");
    return fn ? fn(fence) : GL_TRUE;
}

static void x_glGetFenceivNV(GLuint fence, GLenum pname, gaddr params) {
    if (!params) return;
    static auto fn = (void(*)(GLuint, GLenum, GLint*))eglGetProcAddress("glGetFenceivNV");
    if (fn) {
        fn(fence, pname, reinterpret_cast<GLint*>(h(params)));
    } else {
        wr<GLint>(params, 1);
    }
}

// Draw buffers & read buffer
static void x_glDrawBuffers(GLsizei n, gaddr bufs) {
    if (!bufs || n <= 0) return;
    static auto fn = (void(*)(GLsizei, const GLenum*))eglGetProcAddress("glDrawBuffers");
    if (!fn) fn = (void(*)(GLsizei, const GLenum*))eglGetProcAddress("glDrawBuffersEXT");
    if (!fn) fn = (void(*)(GLsizei, const GLenum*))eglGetProcAddress("glDrawBuffersNV");
    if (fn) fn(n, reinterpret_cast<const GLenum*>(h(bufs)));
}

static void x_glReadBuffer(GLenum mode) {
    static auto fn = (void(*)(GLenum))eglGetProcAddress("glReadBuffer");
    if (!fn) fn = (void(*)(GLenum))eglGetProcAddress("glReadBufferNV");
    if (fn) fn(mode);
}

// Transform feedback
static void x_glGenTransformFeedbacks(GLsizei n, gaddr ids) {
    if (!ids || n <= 0) return;
    static auto fn = (void(*)(GLsizei, GLuint*))eglGetProcAddress("glGenTransformFeedbacks");
    if (!fn) fn = (void(*)(GLsizei, GLuint*))eglGetProcAddress("glGenTransformFeedbacksNV");
    if (fn) fn(n, reinterpret_cast<GLuint*>(h(ids)));
}

static void x_glBindTransformFeedback(GLenum target, GLuint id) {
    static auto fn = (void(*)(GLenum, GLuint))eglGetProcAddress("glBindTransformFeedback");
    if (!fn) fn = (void(*)(GLenum, GLuint))eglGetProcAddress("glBindTransformFeedbackNV");
    if (fn) fn(target, id);
}

static void x_glDeleteTransformFeedbacks(GLsizei n, gaddr ids) {
    if (!ids || n <= 0) return;
    static auto fn = (void(*)(GLsizei, const GLuint*))eglGetProcAddress("glDeleteTransformFeedbacks");
    if (!fn) fn = (void(*)(GLsizei, const GLuint*))eglGetProcAddress("glDeleteTransformFeedbacksNV");
    if (fn) fn(n, reinterpret_cast<const GLuint*>(h(ids)));
}

static void x_glBeginTransformFeedback(GLenum primitiveMode) {
    static auto fn = (void(*)(GLenum))eglGetProcAddress("glBeginTransformFeedback");
    if (!fn) fn = (void(*)(GLenum))eglGetProcAddress("glBeginTransformFeedbackNV");
    if (fn) fn(primitiveMode);
}

static void x_glEndTransformFeedback() {
    static auto fn = (void(*)())eglGetProcAddress("glEndTransformFeedback");
    if (!fn) fn = (void(*)())eglGetProcAddress("glEndTransformFeedbackNV");
    if (fn) fn();
}

// Texture storage
static void x_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    static auto fn = (void(*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei))eglGetProcAddress("glTexStorage2D");
    if (!fn) fn = (void(*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei))eglGetProcAddress("glTexStorage2DEXT");
    if (fn) fn(target, levels, internalformat, width, height);
}

static void x_glAlphaFuncQCOM(GLenum func, GLclampf ref) {
    glAlphaFunc(func, ref);
}

static void x_glEnable(GLenum cap) {
    glEnable(cap);
    if (cap == 0x0DE1 /* GL_TEXTURE_2D */) {
        glGetError();
    }
}

static void x_glDisable(GLenum cap) {
    glDisable(cap);
    if (cap == 0x0DE1 /* GL_TEXTURE_2D */) {
        glGetError();
    }
}

}  // namespace

bool gles_take_frame_changed() {
    static u64 prev = 0;
    bool changed = g_frame_hash != prev;
    prev = g_frame_hash;
    g_frame_hash = 1469598103934665603ull;
    return changed;
}

void register_gles() {
    reg("glActiveTexture", &hashed<&glActiveTexture>);
    NB_REG("glAttachShader", &glAttachShader);
    NB_REG("glBindAttribLocation", &glBindAttribLocation);
    NB_REG("glBindBuffer", x_glBindBuffer);
    reg("glBindFramebuffer", &hashed<&glBindFramebuffer>);
    reg("glBindRenderbuffer", &hashed<&glBindRenderbuffer>);
    reg("glBindTexture", &hashed<&glBindTexture>);
    reg("glBlendColor", &hashed<&glBlendColor>);
    reg("glBlendEquation", &hashed<&glBlendEquation>);
    reg("glBlendEquationSeparate", &hashed<&glBlendEquationSeparate>);
    reg("glBlendFunc", &hashed<&glBlendFunc>);
    reg("glBlendFuncSeparate", &hashed<&glBlendFuncSeparate>);
    NB_REG("glBufferData", x_glBufferData);
    NB_REG("glBufferSubData", x_glBufferSubData);
    NB_REG("glCheckFramebufferStatus", &glCheckFramebufferStatus);
    reg("glClear", &hashed<&glClear>);
    reg("glClearColor", &hashed<&glClearColor>);
    reg("glClearDepthf", &hashed<&glClearDepthf>);
    reg("glClearStencil", &hashed<&glClearStencil>);
    reg("glColorMask", &hashed<&glColorMask>);
    NB_REG("glCompileShader", &glCompileShader);
    NB_REG("glCompressedTexImage2D", x_glCompressedTexImage2D);
    NB_REG("glCompressedTexSubImage2D", x_glCompressedTexSubImage2D);
    reg("glCopyTexImage2D", &hashed<&glCopyTexImage2D>);
    reg("glCopyTexSubImage2D", &hashed<&glCopyTexSubImage2D>);
    NB_REG("glCreateProgram", &glCreateProgram);
    NB_REG("glCreateShader", &glCreateShader);
    reg("glCullFace", &hashed<&glCullFace>);
    NB_REG("glDeleteBuffers", &glDeleteBuffers);
    NB_REG("glDeleteFramebuffers", &glDeleteFramebuffers);
    NB_REG("glDeleteProgram", &glDeleteProgram);
    NB_REG("glDeleteRenderbuffers", &glDeleteRenderbuffers);
    NB_REG("glDeleteShader", &glDeleteShader);
    NB_REG("glDeleteTextures", &glDeleteTextures);
    reg("glDepthFunc", &hashed<&glDepthFunc>);
    reg("glDepthMask", &hashed<&glDepthMask>);
    reg("glDepthRangef", &hashed<&glDepthRangef>);
    NB_REG("glDetachShader", &glDetachShader);
    reg("glDisable", &hashed<&x_glDisable>);
    NB_REG("glDisableVertexAttribArray", x_glDisableVertexAttribArray);
    NB_REG("glDrawArrays", x_glDrawArrays);
    reg("glDrawElements", x_glDrawElements);
    reg("glEnable", &hashed<&x_glEnable>);
    NB_REG("glEnableVertexAttribArray", x_glEnableVertexAttribArray);
    NB_REG("glFinish", &glFinish);
    NB_REG("glFlush", &glFlush);
    reg("glFramebufferRenderbuffer", &hashed<&glFramebufferRenderbuffer>);
    reg("glFramebufferTexture2D", &hashed<&glFramebufferTexture2D>);
    reg("glFrontFace", &hashed<&glFrontFace>);
    NB_REG("glGenBuffers", &glGenBuffers);
    NB_REG("glGenFramebuffers", &glGenFramebuffers);
    NB_REG("glGenRenderbuffers", &glGenRenderbuffers);
    NB_REG("glGenTextures", &glGenTextures);
    reg("glGenerateMipmap", &hashed<&glGenerateMipmap>);
    NB_REG("glGetActiveAttrib", &glGetActiveAttrib);
    NB_REG("glGetActiveUniform", &glGetActiveUniform);
    NB_REG("glGetAttachedShaders", &glGetAttachedShaders);
    NB_REG("glGetAttribLocation", &glGetAttribLocation);
    NB_REG("glGetBooleanv", &glGetBooleanv);
    NB_REG("glGetBufferParameteriv", &glGetBufferParameteriv);
    NB_REG("glGetError", x_glGetError);
    NB_REG("glGetFloatv", &glGetFloatv);
    NB_REG("glGetFramebufferAttachmentParameteriv", &glGetFramebufferAttachmentParameteriv);
    NB_REG("glGetIntegerv", &glGetIntegerv);
    NB_REG("glGetProgramInfoLog", &glGetProgramInfoLog);
    NB_REG("glGetProgramiv", &glGetProgramiv);
    NB_REG("glGetRenderbufferParameteriv", &glGetRenderbufferParameteriv);
    NB_REG("glGetShaderInfoLog", &glGetShaderInfoLog);
    NB_REG("glGetShaderiv", &glGetShaderiv);
    NB_REG("glGetShaderPrecisionFormat", &glGetShaderPrecisionFormat);
    NB_REG("glGetShaderSource", &glGetShaderSource);
    NB_REG("glGetString", x_glGetString);
    NB_REG("glGetTexParameterfv", &glGetTexParameterfv);
    NB_REG("glGetTexParameteriv", &glGetTexParameteriv);
    NB_REG("glGetUniformfv", &glGetUniformfv);
    NB_REG("glGetUniformiv", &glGetUniformiv);
    NB_REG("glGetUniformLocation", &glGetUniformLocation);
    NB_REG("glGetVertexAttribfv", &glGetVertexAttribfv);
    NB_REG("glGetVertexAttribiv", &glGetVertexAttribiv);
    NB_REG("glGetVertexAttribPointerv", x_glGetVertexAttribPointerv);
    reg("glHint", &hashed<&glHint>);
    NB_REG("glIsBuffer", &glIsBuffer);
    NB_REG("glIsEnabled", &glIsEnabled);
    NB_REG("glIsFramebuffer", &glIsFramebuffer);
    NB_REG("glIsProgram", &glIsProgram);
    NB_REG("glIsRenderbuffer", &glIsRenderbuffer);
    NB_REG("glIsShader", &glIsShader);
    NB_REG("glIsTexture", &glIsTexture);
    reg("glLineWidth", &hashed<&glLineWidth>);
    NB_REG("glLinkProgram", &glLinkProgram);
    reg("glPixelStorei", &hashed<&glPixelStorei>);
    reg("glPolygonOffset", &hashed<&glPolygonOffset>);
    NB_REG("glReadPixels", &glReadPixels);
    NB_REG("glReleaseShaderCompiler", &glReleaseShaderCompiler);
    NB_REG("glRenderbufferStorage", &glRenderbufferStorage);
    reg("glSampleCoverage", &hashed<&glSampleCoverage>);
    reg("glScissor", &hashed<&glScissor>);
    NB_REG("glShaderBinary", &glShaderBinary);
    NB_REG("glShaderSource", x_glShaderSource);
    reg("glStencilFunc", &hashed<&glStencilFunc>);
    reg("glStencilFuncSeparate", &hashed<&glStencilFuncSeparate>);
    reg("glStencilMask", &hashed<&glStencilMask>);
    reg("glStencilMaskSeparate", &hashed<&glStencilMaskSeparate>);
    reg("glStencilOp", &hashed<&glStencilOp>);
    reg("glStencilOpSeparate", &hashed<&glStencilOpSeparate>);
    NB_REG("glTexImage2D", x_glTexImage2D);
    reg("glTexParameterf", &hashed<&glTexParameterf>);
    NB_REG("glTexParameterfv", &glTexParameterfv);
    reg("glTexParameteri", &hashed<&glTexParameteri>);
    NB_REG("glTexParameteriv", &glTexParameteriv);
    NB_REG("glTexSubImage2D", x_glTexSubImage2D);
    reg("glUniform1f", &hashed<&glUniform1f>);
    NB_REG("glUniform1fv", x_glUniform1fv);
    reg("glUniform1i", &hashed<&glUniform1i>);
    NB_REG("glUniform1iv", x_glUniform1iv);
    reg("glUniform2f", &hashed<&glUniform2f>);
    NB_REG("glUniform2fv", x_glUniform2fv);
    reg("glUniform2i", &hashed<&glUniform2i>);
    NB_REG("glUniform2iv", x_glUniform2iv);
    reg("glUniform3f", &hashed<&glUniform3f>);
    NB_REG("glUniform3fv", x_glUniform3fv);
    reg("glUniform3i", &hashed<&glUniform3i>);
    NB_REG("glUniform3iv", x_glUniform3iv);
    reg("glUniform4f", &hashed<&glUniform4f>);
    NB_REG("glUniform4fv", x_glUniform4fv);
    reg("glUniform4i", &hashed<&glUniform4i>);
    NB_REG("glUniform4iv", x_glUniform4iv);
    NB_REG("glUniformMatrix2fv", x_glUniformMatrix2fv);
    NB_REG("glUniformMatrix3fv", x_glUniformMatrix3fv);
    NB_REG("glUniformMatrix4fv", x_glUniformMatrix4fv);
    reg("glUseProgram", &hashed<&glUseProgram>);
    NB_REG("glValidateProgram", &glValidateProgram);
    reg("glVertexAttrib1f", &hashed<&glVertexAttrib1f>);
    NB_REG("glVertexAttrib1fv", &glVertexAttrib1fv);
    reg("glVertexAttrib2f", &hashed<&glVertexAttrib2f>);
    NB_REG("glVertexAttrib2fv", &glVertexAttrib2fv);
    reg("glVertexAttrib3f", &hashed<&glVertexAttrib3f>);
    NB_REG("glVertexAttrib3fv", &glVertexAttrib3fv);
    reg("glVertexAttrib4f", &hashed<&glVertexAttrib4f>);
    NB_REG("glVertexAttrib4fv", &glVertexAttrib4fv);
    reg("glVertexAttribPointer", x_glVertexAttribPointer);
    reg("glViewport", &hashed<&glViewport>);

    // EGL
    NB_REG("eglGetDisplay", x_eglGetDisplay);
    NB_REG("eglInitialize", x_eglInitialize);
    NB_REG("eglTerminate", x_eglTerminate);
    NB_REG("eglQueryString", x_eglQueryString);
    NB_REG("eglGetConfigs", x_eglGetConfigs);
    NB_REG("eglChooseConfig", x_eglChooseConfig);
    NB_REG("eglGetConfigAttrib", x_eglGetConfigAttrib);
    NB_REG("eglCreateContext", x_eglCreateContext);
    NB_REG("eglCreateWindowSurface", x_eglCreateWindowSurface);
    NB_REG("eglCreatePbufferSurface", x_eglCreatePbufferSurface);
    NB_REG("eglDestroySurface", x_eglDestroySurface);
    NB_REG("eglDestroyContext", x_eglDestroyContext);
    NB_REG("eglMakeCurrent", x_eglMakeCurrent);
    NB_REG("eglSwapBuffers", x_eglSwapBuffers);
    NB_REG("eglSwapInterval", x_eglSwapInterval);
    NB_REG("eglBindAPI", x_eglBindAPI);
    NB_REG("eglQueryAPI", x_eglQueryAPI);
    NB_REG("eglWaitClient", x_eglWaitClient);
    NB_REG("eglWaitGL", x_eglWaitGL);
    NB_REG("eglWaitNative", x_eglWaitNative);
    NB_REG("eglReleaseThread", x_eglReleaseThread);
    NB_REG("eglSurfaceAttrib", x_eglSurfaceAttrib);
    NB_REG("eglQuerySurface", x_eglQuerySurface);
    NB_REG("eglGetProcAddress", x_eglGetProcAddress);
    NB_REG("eglGetCurrentContext", x_eglGetCurrentContext);
    NB_REG("eglGetCurrentDisplay", x_eglGetCurrentDisplay);
    NB_REG("eglGetCurrentSurface", x_eglGetCurrentSurface);
    NB_REG("eglGetError", x_eglGetError);

    // GLES 1.1 Client state & pointers
    NB_REG("glEnableClientState", &glEnableClientState);
    NB_REG("glDisableClientState", &glDisableClientState);
    NB_REG("glClientActiveTexture", &glClientActiveTexture);
    reg("glVertexPointer", x_glVertexPointer);
    reg("glColorPointer", x_glColorPointer);
    reg("glTexCoordPointer", x_glTexCoordPointer);
    NB_REG("glNormalPointer", x_glNormalPointer);
    NB_REG("glPointSizePointerOES", x_glPointSizePointerOES);
    reg("glMatrixIndexPointerOES", x_glMatrixIndexPointerOES);
    reg("glWeightPointerOES", x_glWeightPointerOES);
    NB_REG("glCurrentPaletteMatrixOES", x_glCurrentPaletteMatrixOES);
    NB_REG("glLoadPaletteFromModelViewMatrixOES", x_glLoadPaletteFromModelViewMatrixOES);
    NB_REG("glDrawTexsOES", x_glDrawTexsOES);
    NB_REG("glDrawTexiOES", x_glDrawTexiOES);
    NB_REG("glDrawTexxOES", x_glDrawTexxOES);
    NB_REG("glDrawTexfOES", x_glDrawTexfOES);
    NB_REG("glDrawTexsvOES", x_glDrawTexsvOES);
    NB_REG("glDrawTexivOES", x_glDrawTexivOES);
    NB_REG("glDrawTexxvOES", x_glDrawTexxvOES);
    NB_REG("glDrawTexfvOES", x_glDrawTexfvOES);

    // GLES 1.1 Matrices & Transforms
    NB_REG("glMatrixMode", &glMatrixMode);
    NB_REG("glLoadIdentity", &glLoadIdentity);
    NB_REG("glPushMatrix", &glPushMatrix);
    NB_REG("glPopMatrix", &glPopMatrix);
    NB_REG("glOrthof", &glOrthof);
    NB_REG("glOrthox", &glOrthox);
    NB_REG("glFrustumf", &glFrustumf);
    NB_REG("glFrustumx", &glFrustumx);
    NB_REG("glTranslatef", &glTranslatef);
    NB_REG("glTranslatex", &glTranslatex);
    NB_REG("glRotatef", &glRotatef);
    NB_REG("glRotatex", &glRotatex);
    NB_REG("glScalef", &glScalef);
    NB_REG("glScalex", &glScalex);
    NB_REG("glLoadMatrixf", x_glLoadMatrixf);
    NB_REG("glMultMatrixf", x_glMultMatrixf);
    NB_REG("glLoadMatrixx", x_glLoadMatrixx);
    NB_REG("glMultMatrixx", x_glMultMatrixx);

    // GLES 1.1 Color, Lighting & Materials
    NB_REG("glColor4f", &glColor4f);
    NB_REG("glColor4ub", &glColor4ub);
    NB_REG("glColor4x", &glColor4x);
    NB_REG("glNormal3f", &glNormal3f);
    NB_REG("glNormal3x", &glNormal3x);
    NB_REG("glAlphaFunc", &glAlphaFunc);
    NB_REG("glAlphaFuncx", &glAlphaFuncx);
    NB_REG("glAlphaFuncQCOM", x_glAlphaFuncQCOM);
    NB_REG("glMapBufferOES", x_glMapBufferOES);
    NB_REG("glUnmapBufferOES", x_glUnmapBufferOES);
    NB_REG("glShadeModel", &glShadeModel);
    NB_REG("glMaterialf", &glMaterialf);
    NB_REG("glMaterialx", &glMaterialx);
    NB_REG("glMaterialfv", x_glMaterialfv);
    NB_REG("glMaterialxv", x_glMaterialxv);
    NB_REG("glLightf", &glLightf);
    NB_REG("glLightx", &glLightx);
    NB_REG("glLightfv", x_glLightfv);
    NB_REG("glLightxv", x_glLightxv);
    NB_REG("glLightModelf", &glLightModelf);
    NB_REG("glLightModelx", &glLightModelx);
    NB_REG("glLightModelfv", x_glLightModelfv);
    NB_REG("glLightModelxv", x_glLightModelxv);

    // GLES 1.1 Texture Environment & Fog
    NB_REG("glTexEnvf", &glTexEnvf);
    NB_REG("glTexEnvi", &glTexEnvi);
    NB_REG("glTexEnvx", x_glTexEnvx);
    NB_REG("glTexEnvfv", x_glTexEnvfv);
    NB_REG("glTexEnviv", x_glTexEnviv);
    NB_REG("glTexEnvxv", x_glTexEnvxv);
    NB_REG("glFogf", &glFogf);
    NB_REG("glFogi", x_glFogi);
    NB_REG("glFogx", &glFogx);
    NB_REG("glFogfv", x_glFogfv);
    NB_REG("glFogxv", x_glFogxv);
    NB_REG("glPointSize", &glPointSize);
    NB_REG("glPointSizex", &glPointSizex);
    NB_REG("glPointParameterf", &glPointParameterf);
    NB_REG("glPointParameterx", &glPointParameterx);
    NB_REG("glPointParameterfv", x_glPointParameterfv);
    NB_REG("glPointParameterxv", x_glPointParameterxv);
    NB_REG("glLineWidthx", &glLineWidthx);
    NB_REG("glClearColorx", &glClearColorx);
    NB_REG("glClearDepthx", &glClearDepthx);
    NB_REG("glDepthRangex", &glDepthRangex);
    NB_REG("glPolygonOffsetx", &glPolygonOffsetx);
    NB_REG("glSampleCoveragex", &glSampleCoveragex);
    NB_REG("glLogicOp", &glLogicOp);
    NB_REG("glClipPlanef", x_glClipPlanef);
    NB_REG("glClipPlanex", x_glClipPlanex);
    NB_REG("glMultiTexCoord4f", &glMultiTexCoord4f);
    NB_REG("glMultiTexCoord4x", &glMultiTexCoord4x);

    // GLES 1.1 Getters
    NB_REG("glGetLightfv", x_glGetLightfv);
    NB_REG("glGetLightxv", x_glGetLightxv);
    NB_REG("glGetMaterialfv", x_glGetMaterialfv);
    NB_REG("glGetMaterialxv", x_glGetMaterialxv);
    NB_REG("glGetTexEnvfv", x_glGetTexEnvfv);
    NB_REG("glGetTexEnviv", x_glGetTexEnviv);
    NB_REG("glGetTexEnvxv", x_glGetTexEnvxv);
    NB_REG("glGetClipPlanef", x_glGetClipPlanef);
    NB_REG("glGetClipPlanex", x_glGetClipPlanex);
    NB_REG("glGetFixedv", x_glGetFixedv);
    NB_REG("glGetPointerv", x_glGetPointerv);

    // OES aliases
    NB_REG("glBindFramebufferOES", &glBindFramebuffer);
    NB_REG("glBindRenderbufferOES", &glBindRenderbuffer);
    NB_REG("glCheckFramebufferStatusOES", &glCheckFramebufferStatus);
    NB_REG("glDeleteFramebuffersOES", &glDeleteFramebuffers);
    NB_REG("glDeleteRenderbuffersOES", &glDeleteRenderbuffers);
    NB_REG("glFramebufferRenderbufferOES", &glFramebufferRenderbuffer);
    NB_REG("glFramebufferTexture2DOES", &glFramebufferTexture2D);
    NB_REG("glGenFramebuffersOES", &glGenFramebuffers);
    NB_REG("glGenRenderbuffersOES", &glGenRenderbuffers);
    reg("glGenerateMipmapOES", &hashed<&glGenerateMipmap>);
    NB_REG("glGetFramebufferAttachmentParameterivOES", &glGetFramebufferAttachmentParameteriv);
    NB_REG("glGetRenderbufferParameterivOES", &glGetRenderbufferParameteriv);
    NB_REG("glIsFramebufferOES", &glIsFramebuffer);
    NB_REG("glIsRenderbufferOES", &glIsRenderbuffer);
    NB_REG("glRenderbufferStorageOES", &glRenderbufferStorage);

    NB_REG("glBindVertexArrayOES", x_glBindVertexArrayOES);
    NB_REG("glDeleteVertexArraysOES", x_glDeleteVertexArraysOES);
    NB_REG("glGenVertexArraysOES", x_glGenVertexArraysOES);
    NB_REG("glIsVertexArrayOES", x_glIsVertexArrayOES);
    NB_REG("glDiscardFramebufferEXT", x_glDiscardFramebufferEXT);

    // Fixed-point texture parameters (OpenGL ES 1.1 fixed-function pipeline)
    NB_REG("glTexParameterx", x_glTexParameterx);
    NB_REG("glTexParameterxv", x_glTexParameterxv);
    NB_REG("glGetTexParameterxv", x_glGetTexParameterxv);

    // Instanced rendering (Hardware instancing extensions)
    NB_REG("glDrawArraysInstanced", x_glDrawArraysInstanced);
    NB_REG("glDrawArraysInstancedEXT", x_glDrawArraysInstanced);
    NB_REG("glDrawArraysInstancedARB", x_glDrawArraysInstanced);
    NB_REG("glDrawArraysInstancedNV", x_glDrawArraysInstanced);
    reg("glDrawElementsInstanced", x_glDrawElementsInstanced);
    reg("glDrawElementsInstancedEXT", x_glDrawElementsInstanced);
    reg("glDrawElementsInstancedARB", x_glDrawElementsInstanced);
    reg("glDrawElementsInstancedNV", x_glDrawElementsInstanced);
    NB_REG("glVertexAttribDivisor", x_glVertexAttribDivisor);
    NB_REG("glVertexAttribDivisorEXT", x_glVertexAttribDivisor);
    NB_REG("glVertexAttribDivisorARB", x_glVertexAttribDivisor);
    NB_REG("glVertexAttribDivisorNV", x_glVertexAttribDivisor);

    // Buffer mapping
    NB_REG("glMapBufferRange", x_glMapBufferRangeEXT);
    NB_REG("glMapBufferRangeEXT", x_glMapBufferRangeEXT);
    NB_REG("glMapBuffer", x_glMapBufferOES);
    NB_REG("glMapBufferOES", x_glMapBufferOES);
    NB_REG("glMapBufferARB", x_glMapBufferOES);
    NB_REG("glUnmapBuffer", x_glUnmapBufferOES);
    NB_REG("glUnmapBufferOES", x_glUnmapBufferOES);
    NB_REG("glUnmapBufferARB", x_glUnmapBufferOES);
    NB_REG("glFlushMappedBufferRange", x_glFlushMappedBufferRangeEXT);
    NB_REG("glFlushMappedBufferRangeEXT", x_glFlushMappedBufferRangeEXT);

    // Framebuffer blit & discard
    NB_REG("glBlitFramebuffer", x_glBlitFramebufferNV);
    NB_REG("glBlitFramebufferNV", x_glBlitFramebufferNV);
    NB_REG("glBlitFramebufferEXT", x_glBlitFramebufferNV);
    NB_REG("glBlitFramebufferANGLE", x_glBlitFramebufferNV);
    NB_REG("glInvalidateFramebuffer", x_glDiscardFramebufferEXT);

    // Queries
    NB_REG("glGenQueries", x_glGenQueriesEXT);
    NB_REG("glGenQueriesEXT", x_glGenQueriesEXT);
    NB_REG("glDeleteQueries", x_glDeleteQueriesEXT);
    NB_REG("glDeleteQueriesEXT", x_glDeleteQueriesEXT);
    NB_REG("glBeginQuery", x_glBeginQueryEXT);
    NB_REG("glBeginQueryEXT", x_glBeginQueryEXT);
    NB_REG("glBeginQueryARB", x_glBeginQueryEXT);
    NB_REG("glEndQuery", x_glEndQueryEXT);
    NB_REG("glEndQueryEXT", x_glEndQueryEXT);
    NB_REG("glEndQueryARB", x_glEndQueryEXT);
    NB_REG("glGetQueryObjectuiv", x_glGetQueryObjectuivEXT);
    NB_REG("glGetQueryObjectuivEXT", x_glGetQueryObjectuivEXT);
    NB_REG("glGetQueryObjectuivARB", x_glGetQueryObjectuivEXT);
    NB_REG("glGetQueryObjectiv", x_glGetQueryObjectivEXT);
    NB_REG("glGetQueryObjectivEXT", x_glGetQueryObjectivEXT);
    NB_REG("glGetQueryObjectivARB", x_glGetQueryObjectivEXT);

    // Fences & Sync
    NB_REG("glGenFencesNV", x_glGenFencesNV);
    NB_REG("glGenFencesAPPLE", x_glGenFencesNV);
    NB_REG("glDeleteFencesNV", x_glDeleteFencesNV);
    NB_REG("glDeleteFencesAPPLE", x_glDeleteFencesNV);
    NB_REG("glSetFenceNV", x_glSetFenceNV);
    NB_REG("glSetFenceAPPLE", x_glSetFenceNV);
    NB_REG("glTestFenceNV", x_glTestFenceNV);
    NB_REG("glTestFenceAPPLE", x_glTestFenceNV);
    NB_REG("glFinishFenceNV", x_glFinishFenceNV);
    NB_REG("glFinishFenceAPPLE", x_glFinishFenceNV);
    NB_REG("glIsFenceNV", x_glIsFenceNV);
    NB_REG("glGetFenceivNV", x_glGetFenceivNV);

    // Draw buffers & read buffer
    NB_REG("glDrawBuffers", x_glDrawBuffers);
    NB_REG("glDrawBuffersEXT", x_glDrawBuffers);
    NB_REG("glDrawBuffersNV", x_glDrawBuffers);
    NB_REG("glReadBuffer", x_glReadBuffer);
    NB_REG("glReadBufferNV", x_glReadBuffer);

    // Transform feedback
    NB_REG("glGenTransformFeedbacks", x_glGenTransformFeedbacks);
    NB_REG("glGenTransformFeedbacksNV", x_glGenTransformFeedbacks);
    NB_REG("glBindTransformFeedback", x_glBindTransformFeedback);
    NB_REG("glBindTransformFeedbackNV", x_glBindTransformFeedback);
    NB_REG("glDeleteTransformFeedbacks", x_glDeleteTransformFeedbacks);
    NB_REG("glDeleteTransformFeedbacksNV", x_glDeleteTransformFeedbacks);
    NB_REG("glBeginTransformFeedback", x_glBeginTransformFeedback);
    NB_REG("glBeginTransformFeedbackNV", x_glBeginTransformFeedback);
    NB_REG("glEndTransformFeedback", x_glEndTransformFeedback);
    NB_REG("glEndTransformFeedbackNV", x_glEndTransformFeedback);

    // Texture storage
    NB_REG("glTexStorage2D", x_glTexStorage2D);
    NB_REG("glTexStorage2DEXT", x_glTexStorage2D);
}

}  // namespace nb