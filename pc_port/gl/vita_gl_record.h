// Vita: recorders for the GL/GXM calls pc_gfx.cpp makes.
#pragma once
#include <string.h>
#include "../../vita/src/vita_rt.h"

static inline void* rec_payload(VitaRtOp op, size_t bytes) { return vita_rt_emit(op, bytes); }
template <typename T> static inline void rec_put(uint8_t*& p, T v) { memcpy(p, &v, sizeof(T)); p += sizeof(T); }

static void rec_glUseProgram(GLuint prog) {
    if (!vita_rt_recording()) { glUseProgram(prog); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_USE_PROGRAM, 4); if (p) rec_put(p, uint32_t(prog));
}
static void rec_glUniform1i(GLint loc, GLint v) {
    if (!vita_rt_recording()) { glUniform1i(loc, v); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_1I, 8); if (p) { rec_put(p, int32_t(loc)); rec_put(p, int32_t(v)); }
}
static void rec_glUniform1f(GLint loc, GLfloat v) {
    if (!vita_rt_recording()) { glUniform1f(loc, v); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_1F, 8); if (p) { rec_put(p, int32_t(loc)); rec_put(p, float(v)); }
}
static void rec_glUniform2i(GLint loc, GLint x, GLint y) {
    if (!vita_rt_recording()) { glUniform2i(loc, x, y); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_2I, 12); if (p) { rec_put(p, int32_t(loc)); rec_put(p, int32_t(x)); rec_put(p, int32_t(y)); }
}
static void rec_glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w) {
    if (!vita_rt_recording()) { glUniform4i(loc, x, y, z, w); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_4I, 20);
    if (p) { rec_put(p, int32_t(loc)); rec_put(p, int32_t(x)); rec_put(p, int32_t(y)); rec_put(p, int32_t(z)); rec_put(p, int32_t(w)); }
}
static void rec_glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (!vita_rt_recording()) { glUniform4f(loc, x, y, z, w); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_4F, 20);
    if (p) { rec_put(p, int32_t(loc)); rec_put(p, float(x)); rec_put(p, float(y)); rec_put(p, float(z)); rec_put(p, float(w)); }
}
static void rec_glUniform4fv(GLint loc, GLsizei count, const GLfloat* v) {
    if (!vita_rt_recording()) { glUniform4fv(loc, count, v); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_4FV, 8 + size_t(count) * 16);
    if (p) { rec_put(p, int32_t(loc)); rec_put(p, int32_t(count)); memcpy(p, v, size_t(count) * 16); }
}
static void rec_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (!vita_rt_recording()) { glUniformMatrix4fv(loc, count, transpose, v); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_MAT4, 12 + size_t(count) * 64);
    if (p) { rec_put(p, int32_t(loc)); rec_put(p, int32_t(count)); rec_put(p, uint32_t(transpose)); memcpy(p, v, size_t(count) * 64); }
}
static void rec_glUniformMatrix3fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* v) {
    if (!vita_rt_recording()) { glUniformMatrix3fv(loc, count, transpose, v); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_UNIFORM_MAT3, 12 + size_t(count) * 36);
    if (p) { rec_put(p, int32_t(loc)); rec_put(p, int32_t(count)); rec_put(p, uint32_t(transpose)); memcpy(p, v, size_t(count) * 36); }
}
static void rec_glActiveTexture(GLenum unit) {
    if (!vita_rt_recording()) { glActiveTexture(unit); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_ACTIVE_TEXTURE, 4); if (p) rec_put(p, uint32_t(unit));
}
static void rec_glBindTexture(GLenum target, GLuint id) {
    if (!vita_rt_recording()) { glBindTexture(target, id); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_BIND_TEXTURE, 8); if (p) { rec_put(p, uint32_t(target)); rec_put(p, uint32_t(id)); }
}
static void rec_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (!vita_rt_recording()) { glTexParameteri(target, pname, param); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_TEX_PARAMETER_I, 12); if (p) { rec_put(p, uint32_t(target)); rec_put(p, uint32_t(pname)); rec_put(p, int32_t(param)); }
}
static void rec_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                GLenum fmt, GLenum type, const void* data) {
    if (!vita_rt_recording()) { glTexSubImage2D(target, level, x, y, w, h, fmt, type, data); return; }
    uint32_t bpp = 4;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_4_4_4_4) bpp = 2;
    else if (fmt == GL_LUMINANCE_ALPHA) bpp = 2;
    else if (fmt == GL_LUMINANCE || fmt == GL_ALPHA) bpp = 1;
    else if (fmt == GL_RGB) bpp = 3;
    const uint32_t bytes = data ? uint32_t(w) * uint32_t(h) * bpp : 0u;
    uint8_t* p = (uint8_t*)rec_payload(RT_TEX_SUB_IMAGE_2D, 36 + bytes);
    if (!p) return;
    rec_put(p, uint32_t(target)); rec_put(p, int32_t(level)); rec_put(p, int32_t(x)); rec_put(p, int32_t(y));
    rec_put(p, int32_t(w)); rec_put(p, int32_t(h)); rec_put(p, uint32_t(fmt)); rec_put(p, uint32_t(type)); rec_put(p, bytes);
    if (bytes) memcpy(p, data, bytes);
}
static void rec_glLineWidth(GLfloat w) {
    if (!vita_rt_recording()) { glLineWidth(w); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_LINE_WIDTH, 4); if (p) { rec_put(p, float(w)); }
}
// GXCopyTex and offscreen EFB ops.
static void rec_efb_copy(GLuint tex, int w, int h, int program, int depth, float u0, float v0, float u1, float v1, int box) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_EFB_COPY, 40);
    if (p) { rec_put(p, uint32_t(tex)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h)); rec_put(p, int32_t(program)); rec_put(p, int32_t(depth));
             rec_put(p, u0); rec_put(p, v0); rec_put(p, u1); rec_put(p, v1); rec_put(p, int32_t(box)); }
}
static void rec_efb_target(GLuint tex, int w, int h) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_EFB_TARGET, 12);
    if (p) { rec_put(p, uint32_t(tex)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h)); }
}
static void rec_efb_composite(int kind, GLuint tex0, GLuint tex1, float p0, float p1) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_EFB_COMPOSITE, 20);
    if (p) { rec_put(p, int32_t(kind)); rec_put(p, uint32_t(tex0)); rec_put(p, uint32_t(tex1)); rec_put(p, p0); rec_put(p, p1); }
}
static void rec_efb_vfilter(float rowStep, float edge, float centre) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_EFB_VFILTER, 12);
    if (p) { rec_put(p, rowStep); rec_put(p, edge); rec_put(p, centre); }
}
static void rec_tex_mipchain(GLuint tex, int w, int h, int levels, const void* data, uint32_t bytes) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_TEX_MIPCHAIN, 16 + bytes);
    if (p) { rec_put(p, uint32_t(tex)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h)); rec_put(p, int32_t(levels)); memcpy(p, data, bytes); }
}
static void rec_glGenerateMipmap(GLenum target) {
    if (!vita_rt_recording()) { glGenerateMipmap(target); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_GENERATE_MIPMAP, 4); if (p) { rec_put(p, uint32_t(target)); }
}
static void rec_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                             GLenum fmt, GLenum type, const void* data) {
    (void)border;
    if (!vita_rt_recording()) { glTexImage2D(target, level, internal, w, h, 0, fmt, type, data); return; }
    uint32_t bpp = 4;   // RGBA8 unless 16-bit or luminance
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_4_4_4_4) bpp = 2;
    else if (fmt == GL_LUMINANCE_ALPHA) bpp = 2;
    else if (fmt == GL_LUMINANCE || fmt == GL_ALPHA) bpp = 1;
    const uint32_t bytes = data ? uint32_t(w) * uint32_t(h) * bpp : 0u;
    uint8_t* p = (uint8_t*)rec_payload(RT_TEX_IMAGE_2D, 32 + bytes);
    if (!p) return;
    rec_put(p, uint32_t(target)); rec_put(p, int32_t(level)); rec_put(p, int32_t(internal)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h));
    rec_put(p, uint32_t(fmt)); rec_put(p, uint32_t(type)); rec_put(p, bytes);
    if (bytes) memcpy(p, data, bytes);
}
static void rec_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                                       GLsizei imageSize, const void* data) {
    (void)border;
    if (!vita_rt_recording()) { glCompressedTexImage2D(target, level, internal, w, h, 0, imageSize, data); return; }
    const uint32_t bytes = data ? uint32_t(imageSize) : 0u;
    uint8_t* p = (uint8_t*)rec_payload(RT_COMPRESSED_TEX_IMAGE_2D, 24 + bytes);
    if (!p) return;
    rec_put(p, uint32_t(target)); rec_put(p, int32_t(level)); rec_put(p, int32_t(internal)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h));
    rec_put(p, bytes);
    if (bytes) memcpy(p, data, bytes);
}
static void rec_glGenTextures(GLsizei n, GLuint* ids) {
    if (!vita_rt_recording()) { glGenTextures(n, ids); return; }
    for (GLsizei i = 0; i < n; ++i) ids[i] = vita_rt_alloc_texture_id();
}
void vita_rt_release_texture_id(GLuint id);
void vita_rt_release_buffer_id(GLuint id);
static void rec_glDeleteTextures(GLsizei n, const GLuint* ids) {
    if (!vita_rt_recording()) { glDeleteTextures(n, ids); return; }
    for (GLsizei i = 0; i < n; ++i) {
        rec_glBindTexture(GL_TEXTURE_2D, ids[i]);
        static const uint32_t px = 0;
        rec_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
        vita_rt_release_texture_id(ids[i]);
    }
}
static void rec_glGenBuffers(GLsizei n, GLuint* ids) {
    if (!vita_rt_recording()) { glGenBuffers(n, ids); return; }
    for (GLsizei i = 0; i < n; ++i) ids[i] = vita_rt_alloc_buffer_id();
}
static void rec_glBindBuffer(GLenum target, GLuint id) {
    if (!vita_rt_recording()) { glBindBuffer(target, id); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_BIND_BUFFER, 8); if (p) { rec_put(p, uint32_t(target)); rec_put(p, uint32_t(id)); }
}
static void rec_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (!vita_rt_recording()) { glBufferData(target, size, data, usage); return; }
    const uint32_t bytes = data ? uint32_t(size) : 0u;
    uint8_t* p = (uint8_t*)rec_payload(RT_BUFFER_DATA, 16 + bytes);
    if (!p) return;
    rec_put(p, uint32_t(target)); rec_put(p, bytes); rec_put(p, uint32_t(usage)); rec_put(p, uint32_t(size));
    if (bytes) memcpy(p, data, bytes);
}
// glBufferData + glMapBuffer; the render thread keeps the mapped address.
static void rec_glBufferDataMapped(GLenum target, GLsizeiptr size, const void* data, GLenum usage, GLuint bufId) {
    if (!vita_rt_recording()) {
        glBufferData(target, size, data, usage);
        return;
    }
    uint8_t* p = (uint8_t*)rec_payload(RT_BUFFER_DATA_MAP, 16 + size_t(size));
    if (!p) return;
    rec_put(p, uint32_t(target)); rec_put(p, uint32_t(size)); rec_put(p, uint32_t(usage)); rec_put(p, uint32_t(bufId));
    memcpy(p, data, size_t(size));
}
static void rec_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (!vita_rt_recording()) { glBufferSubData(target, offset, size, data); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_BUFFER_SUB_DATA, 12 + size_t(size));
    if (!p) return;
    rec_put(p, uint32_t(target)); rec_put(p, uint32_t(offset)); rec_put(p, uint32_t(size));
    memcpy(p, data, size_t(size));
}
static void rec_glDeleteBuffers(GLsizei n, const GLuint* ids) {
    if (!vita_rt_recording()) { glDeleteBuffers(n, ids); return; }
    for (GLsizei i = 0; i < n; ++i) {
        rec_glBindBuffer(GL_ARRAY_BUFFER, ids[i]);
        static const uint32_t word = 0;
        rec_glBufferData(GL_ARRAY_BUFFER, 4, &word, GL_STATIC_DRAW);
        vita_rt_release_buffer_id(ids[i]);
    }
}
static void rec_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer) {
    if (!vita_rt_recording()) { glVertexAttribPointer(index, size, type, normalized, stride, pointer); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_VERTEX_ATTRIB_POINTER, 24);
    if (p) { rec_put(p, uint32_t(index)); rec_put(p, int32_t(size)); rec_put(p, uint32_t(type)); rec_put(p, uint32_t(normalized)); rec_put(p, int32_t(stride)); rec_put(p, uint32_t(uintptr_t(pointer))); }
}
static bool sRecScissorEnabled = false;   // shadow of GL_SCISSOR_TEST
static void rec_glEnable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) sRecScissorEnabled = true;
    if (!vita_rt_recording()) { glEnable(cap); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_ENABLE, 4); if (p) rec_put(p, uint32_t(cap));
}
static void rec_glDisable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) sRecScissorEnabled = false;
    if (!vita_rt_recording()) { glDisable(cap); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_DISABLE, 4); if (p) rec_put(p, uint32_t(cap));
}
static GLboolean rec_glIsEnabled(GLenum cap) {
    if (!vita_rt_recording()) return glIsEnabled(cap);
    return cap == GL_SCISSOR_TEST ? (sRecScissorEnabled ? GL_TRUE : GL_FALSE) : GL_FALSE;
}
static void rec_glBlendFunc(GLenum s, GLenum d) {
    if (!vita_rt_recording()) { glBlendFunc(s, d); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_BLEND_FUNC, 8); if (p) { rec_put(p, uint32_t(s)); rec_put(p, uint32_t(d)); }
}
static void rec_glBlendEquation(GLenum mode) {
    if (!vita_rt_recording()) { glBlendEquation(mode); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_BLEND_EQUATION, 4); if (p) rec_put(p, uint32_t(mode));
}
static void rec_glDepthFunc(GLenum f) {
    if (!vita_rt_recording()) { glDepthFunc(f); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_DEPTH_FUNC, 4); if (p) rec_put(p, uint32_t(f));
}
static void rec_glDepthMask(GLboolean f) {
    if (!vita_rt_recording()) { glDepthMask(f); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_DEPTH_MASK, 4); if (p) rec_put(p, uint32_t(f));
}
static void rec_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    if (!vita_rt_recording()) { glColorMask(r, g, b, a); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_COLOR_MASK, 4);
    if (p) rec_put(p, uint32_t((r ? 1u : 0u) | (g ? 2u : 0u) | (b ? 4u : 0u) | (a ? 8u : 0u)));
}
static void rec_glCullFace(GLenum m) {
    if (!vita_rt_recording()) { glCullFace(m); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_CULL_FACE, 4); if (p) rec_put(p, uint32_t(m));
}
static void rec_glFrontFace(GLenum m) {
    if (!vita_rt_recording()) { glFrontFace(m); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_FRONT_FACE, 4); if (p) rec_put(p, uint32_t(m));
}
static void rec_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!vita_rt_recording()) { glViewport(x, y, w, h); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_VIEWPORT, 16); if (p) { rec_put(p, int32_t(x)); rec_put(p, int32_t(y)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h)); }
}
static void rec_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!vita_rt_recording()) { glScissor(x, y, w, h); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_SCISSOR, 16); if (p) { rec_put(p, int32_t(x)); rec_put(p, int32_t(y)); rec_put(p, int32_t(w)); rec_put(p, int32_t(h)); }
}
static void rec_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (!vita_rt_recording()) { glClearColor(r, g, b, a); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_CLEAR_COLOR, 16); if (p) { rec_put(p, float(r)); rec_put(p, float(g)); rec_put(p, float(b)); rec_put(p, float(a)); }
}
static void rec_glClearDepth(GLdouble d) {
    if (!vita_rt_recording()) { glClearDepth(d); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_CLEAR_DEPTH, 4); if (p) rec_put(p, float(d));
}
static void rec_glClear(GLbitfield mask) {
    if (!vita_rt_recording()) { glClear(mask); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_CLEAR, 4); if (p) rec_put(p, uint32_t(mask));
}
static void rec_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!vita_rt_recording()) { glDrawArrays(mode, first, count); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_DRAW_ARRAYS, 12); if (p) { rec_put(p, uint32_t(mode)); rec_put(p, int32_t(first)); rec_put(p, int32_t(count)); }
}
static void rec_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!vita_rt_recording()) { glDrawElements(mode, count, type, indices); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_DRAW_ELEMENTS, 16);
    if (p) { rec_put(p, uint32_t(mode)); rec_put(p, int32_t(count)); rec_put(p, uint32_t(type)); rec_put(p, uint32_t(uintptr_t(indices))); }
}
// Client-array overflow draw.
static void rec_draw_client(GLenum mode, GLsizei count, const void* verts, size_t bytes) {
    uint8_t* p = (uint8_t*)rec_payload(RT_DRAW_CLIENT, 12 + bytes);
    if (!p) return;
    rec_put(p, uint32_t(mode)); rec_put(p, int32_t(count)); rec_put(p, uint32_t(bytes));
    memcpy(p, verts, bytes);
}
static void rec_gxm_vertex_stream_abs(unsigned index, const void* ptr) {
    if (!vita_rt_recording()) { sceGxmSetVertexStream(vglGetGxmContext(), index, ptr); return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_GXM_VERTEX_STREAM_ABS, 8);
    if (p) { rec_put(p, uint32_t(index)); rec_put(p, ptr); }
}
static void rec_gxm_fragment_texture(unsigned unit, GLuint texId) {
    if (!vita_rt_recording()) {
        const SceGxmTexture* t = vglGetGxmTextureById(texId);
        if (t) sceGxmSetFragmentTexture(vglGetGxmContext(), unit, t);
        return;
    }
    uint8_t* p = (uint8_t*)rec_payload(RT_GXM_FRAGMENT_TEXTURE, 8); if (p) { rec_put(p, uint32_t(unit)); rec_put(p, uint32_t(texId)); }
}
static void rec_fast_draw_indexed(GLuint vbo, GLuint ibo, int count, int firstIndex) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_FAST_DRAW_INDEXED, 16);
    if (p) { rec_put(p, uint32_t(vbo)); rec_put(p, uint32_t(ibo)); rec_put(p, int32_t(count)); rec_put(p, int32_t(firstIndex)); }
}
static void rec_fast_draw_arrays(GLuint vbo, const void* attrBase, const void* gpuBase, GLenum mode, GLsizei count) {
    if (!vita_rt_recording()) return;
    uint8_t* p = (uint8_t*)rec_payload(RT_FAST_DRAW_ARRAYS, 20);
    if (p) { rec_put(p, uint32_t(vbo)); rec_put(p, attrBase); rec_put(p, gpuBase); rec_put(p, uint32_t(mode)); rec_put(p, int32_t(count)); }
}
static void rec_fast_mode(int on) {
    if (!vita_rt_recording()) { vgl_fast_draw_mode = on; return; }
    uint8_t* p = (uint8_t*)rec_payload(RT_FAST_MODE, 4); if (p) rec_put(p, int32_t(on));
}

// Direct-call redirects for pc_gfx.cpp.
#define glBindTexture      rec_glBindTexture
#define glTexParameteri    rec_glTexParameteri
#define glTexImage2D       rec_glTexImage2D
#define glGenerateMipmap   rec_glGenerateMipmap
#define glLineWidth        rec_glLineWidth
#define glTexSubImage2D    rec_glTexSubImage2D
#define glCompressedTexImage2D rec_glCompressedTexImage2D
#define glGenTextures      rec_glGenTextures
#define glDeleteTextures   rec_glDeleteTextures
#define glDeleteBuffers    rec_glDeleteBuffers
#define glUniform4fv       rec_glUniform4fv
#define glEnable           rec_glEnable
#define glDisable          rec_glDisable
#define glIsEnabled        rec_glIsEnabled
#define glBlendFunc        rec_glBlendFunc
#define glDepthFunc        rec_glDepthFunc
#define glDepthMask        rec_glDepthMask
#define glColorMask        rec_glColorMask
#define glCullFace         rec_glCullFace
#define glFrontFace        rec_glFrontFace
#define glViewport         rec_glViewport
#define glScissor          rec_glScissor
#define glClearColor       rec_glClearColor
#define glClearDepth       rec_glClearDepth
#define glClear            rec_glClear
#define glDrawArrays       rec_glDrawArrays
#define glDrawElements     rec_glDrawElements
#define glGetError()       (GL_NO_ERROR)
