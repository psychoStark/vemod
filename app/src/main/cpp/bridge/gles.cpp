// OpenGL ES 2.0 imports. All prototypes use 32-bit scalar types, so most are bound directly with
// automatic pointer translation; the few with host-width or pointer-array parameters are wrapped.
#include <GLES2/gl2.h>

#include <mutex>
#include <unordered_map>

#include "nb.h"

namespace nb {
namespace {

thread_local GLuint t_array_buffer = 0;
thread_local GLuint t_element_array_buffer = 0;

// ---------------------------------------------------------------- frame fingerprint
// A hash of everything that affects the rendered image (state, uniforms, draws and the vertex data
// they read). If two consecutive frames hash the same, the picture is static and the Java side can
// stop requesting frames until something happens.

u64 g_frame_hash = 1469598103934665603ull;
u64 g_upload_nonce = 0;

inline void mix(const void* p, size_t n) {
    auto* b = static_cast<const u8*>(p);
    u64 x = g_frame_hash;
    for (size_t i = 0; i < n; ++i) x = (x ^ b[i]) * 1099511628211ull;
    g_frame_hash = x;
}
template <class T>
inline void mixv(T v) {
    mix(&v, sizeof v);
}

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

// Thunk for scalar-only GL calls (every argument is one 32-bit word) that also folds the call and
// its arguments into the frame hash.
template <auto F>
void hashed(Ctx& c) {
    constexpr u32 n = arity(F);
    mixv(reinterpret_cast<uintptr_t>(F));
    for (u32 i = 0; i < n; ++i) mixv(i < 4 ? c.r[i] : rd<u32>(c.r[13] + (i - 4) * 4));
    auto_thunk<F>(c);
}

void x_glBindBuffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER) t_array_buffer = buf;
    if (target == GL_ELEMENT_ARRAY_BUFFER) t_element_array_buffer = buf;
    mixv(target);
    mixv(buf);
    glBindBuffer(target, buf);
}

void x_glDrawElements(Ctx& c) {
    GLenum mode = c.a32();
    GLsizei count = GLsizei(c.a32());
    GLenum type = c.a32();
    u32 indices = c.a32();
    const void* ptr = t_element_array_buffer ? reinterpret_cast<const void*>(uintptr_t(indices)) : (indices ? h(indices) : nullptr);
    mixv(mode);
    mixv(count);
    mixv(type);
    mixv(indices);
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
    mixv(mode);
    mixv(first);
    mixv(count);
    for (u32 i = 0; i < 16; ++i) {
        const Attrib& a = g_attribs[i];
        if (!a.enabled || !a.client || count <= 0) continue;
        size_t elem = size_t(a.size) * gl_type_size(a.type);
        size_t stride = a.stride ? size_t(a.stride) : elem;
        const u8* p = a.client + size_t(first) * stride;
        if (stride == elem) {
            mix(p, elem * size_t(count));
        } else {
            for (GLsizei k = 0; k < count; ++k) mix(p + size_t(k) * stride, elem);
        }
    }
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
    reg("glDisable", &hashed<&glDisable>);
    NB_REG("glDisableVertexAttribArray", x_glDisableVertexAttribArray);
    NB_REG("glDrawArrays", x_glDrawArrays);
    reg("glDrawElements", x_glDrawElements);
    reg("glEnable", &hashed<&glEnable>);
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
    NB_REG("glGetError", &glGetError);
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
}

}  // namespace nb