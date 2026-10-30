#ifdef __vita__
#ifdef __vita__
#include <arm_neon.h>
#endif
#include "vita_gxq_rename.h"   // renames pc_gfx_* to pc_gfx_*_impl
#include "vita_gxq.h"
#endif
#include "pc_gfx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#if defined(_WIN32) || defined(__vita__)
// glibc's backtrace() has no Windows equivalent worth pulling a dependency in
// for; the wild-vertex diagnostic below degrades to its raw dump instead.
#else
#include <execinfo.h>
#endif
#include <chrono>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include "../timing/pc_render_packet.h"
#include "pc_tev_shader.h"
#include "../timing/pc_render_phase.h"

#include "pc_opengl.h"
// ProgramLocations and the vgx_write_* uniform section writers.
#include "pc_program_locations.h"
#ifdef __vita__
#include "vita_gl_record.h"   // GL/GXM call recording
#include "settings/pc_settings.h"
#include "vita_imm_chain.h"   // immediate-path fold chain
#include <psp2/kernel/processmgr.h>
#endif

// ── GL Function Pointers (Loaded via SDL_GL_GetProcAddress) ──
typedef void (APIENTRYP PFNGLGENBUFFERSPROC) (GLsizei n, GLuint *buffers);
typedef void (APIENTRYP PFNGLBINDBUFFERPROC) (GLenum target, GLuint buffer);
typedef void (APIENTRYP PFNGLBUFFERDATAPROC) (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void (APIENTRYP PFNGLBUFFERSUBDATAPROC) (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
typedef GLuint (APIENTRYP PFNGLCREATESHADERPROC) (GLenum type);
typedef void (APIENTRYP PFNGLSHADERSOURCEPROC) (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
typedef void (APIENTRYP PFNGLCOMPILESHADERPROC) (GLuint shader);
typedef GLuint (APIENTRYP PFNGLCREATEPROGRAMPROC) (void);
typedef void (APIENTRYP PFNGLATTACHSHADERPROC) (GLuint program, GLuint shader);
typedef void (APIENTRYP PFNGLLINKPROGRAMPROC) (GLuint program);
typedef void (APIENTRYP PFNGLUSEPROGRAMPROC) (GLuint program);
typedef GLint (APIENTRYP PFNGLGETUNIFORMLOCATIONPROC) (GLuint program, const GLchar *name);
typedef GLint (APIENTRYP PFNGLGETATTRIBLOCATIONPROC) (GLuint program, const GLchar *name);
typedef void (APIENTRYP PFNGLUNIFORMMATRIX4FVPROC) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRYP PFNGLUNIFORM1IPROC) (GLint location, GLint v0);
typedef void (APIENTRYP PFNGLUNIFORM1FPROC) (GLint location, GLfloat v0);
typedef void (APIENTRYP PFNGLUNIFORM4FPROC) (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void (APIENTRYP PFNGLUNIFORM2IPROC) (GLint location, GLint v0, GLint v1);
typedef void (APIENTRYP PFNGLUNIFORM4IPROC) (GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
typedef void (APIENTRYP PFNGLENABLEVERTEXATTRIBARRAYPROC) (GLuint index);
typedef void (APIENTRYP PFNGLVERTEXATTRIBPOINTERPROC) (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
typedef void (APIENTRYP PCGLGENQUERIESPROC) (GLsizei n, GLuint* ids);
typedef void (APIENTRYP PCGLBEGINQUERYPROC) (GLenum target, GLuint id);
typedef void (APIENTRYP PCGLENDQUERYPROC) (GLenum target);
typedef void (APIENTRYP PCGLGETQUERYOBJECTIVPROC) (GLuint id, GLenum pname, GLint* params);
typedef void (APIENTRYP PCGLGETQUERYOBJECTUI64VPROC) (GLuint id, GLenum pname, GLuint64* params);

// Windows' opengl32 exports only OpenGL 1.1, so anything newer has to come
// through SDL_GL_GetProcAddress like the rest. On Linux these two happen to be
// declared by the system header, which is why they were called directly.
static PFNGLACTIVETEXTUREPROC glActiveTexture_ptr = nullptr;
static void apply_texture_filtering(u8 minAlpha, int width, int height);
// Per-texture storage shape.
struct PcTexShape { int w = 0, h = 0; GLenum fmt = 0, type = 0; };
static std::unordered_map<GLuint, PcTexShape> sTexShape;
// EFB copy targets, by game-memory address.
struct PcCopyTarget { GLuint tex = 0; u16 w = 0, h = 0; };
static std::unordered_map<const void*, PcCopyTarget> sCopyTargets;
static std::vector<PcCopyTarget> sCopyTargetPool;   // copy textures kept between levels (pc_gfx_level_flush)
static std::unordered_map<GLuint, bool> sEfbTextureIds;   // copy targets and the offscreen EFB
// Offscreen EFB.
static GLuint sEfbOffTex = 0;
static int sEfbOffW = 0, sEfbOffH = 0;
static bool sEfbOffscreen = false;
static unsigned sTargetEpoch = 1;   // render target epoch
static float sGxViewport[4] = { 0.0f, 0.0f, 640.0f, 480.0f };   // last GX-space viewport and scissor
static u32 sGxScissor[4] = { 0, 0, 640, 480 };
// Display copy vertical filter (GXSetCopyFilter), row weights /64.
static int sVFilterEdge = 0, sVFilterCentre = 64;
static void pc_tex_mip_forget(GLuint tex);

static bool sSectionClip43 = false;


// Fog.
static float sFogStart = 0.0f, sFogEnd = 0.0f, sFogNear = 0.0f, sFogFar = 0.0f;
static float sFogColour[3] = { 0.0f, 0.0f, 0.0f };
static bool  sFogOn = false;
// Vertex fog: vFog = w * K0 + K1.
static float sFogK0 = 0.0f, sFogK1 = 0.0f;

static uint32_t sFogGen = 1;   // fog state generation
#ifdef __vita__
// Fog state content hash.
static uint64_t sFogHash = 0;
static inline uint64_t hash_bytes(uint64_t h, const void* data, size_t bytes);
#endif
void pc_gfx_set_fog(bool enable, float startZ, float endZ, float nearZ, float farZ,
                    unsigned char r, unsigned char g, unsigned char b) {
    const bool usable = enable && farZ > nearZ && nearZ > 0.0f && endZ > startZ;
    const float ns = usable ? startZ : 0.0f, ne = usable ? endZ : 0.0f, nn = usable ? nearZ : 0.0f, nf = usable ? farZ : 0.0f;
    const float cr = usable ? r / 255.0f : 0.0f, cg = usable ? g / 255.0f : 0.0f, cb = usable ? b / 255.0f : 0.0f;
    const bool changed = sFogOn != usable || sFogStart != ns || sFogEnd != ne || sFogNear != nn || sFogFar != nf
                      || sFogColour[0] != cr || sFogColour[1] != cg || sFogColour[2] != cb;
    sFogOn = usable; sFogStart = ns; sFogEnd = ne; sFogNear = nn; sFogFar = nf;
    sFogColour[0] = cr; sFogColour[1] = cg; sFogColour[2] = cb;
    sFogK0 = usable ? 1.0f / (ne - ns) : 0.0f;
    sFogK1 = usable ? -ns / (ne - ns) : 0.0f;
#ifdef __vita__
    if (changed || sFogHash == 0) {
#else
    if (changed) {
#endif
        ++sFogGen;
#ifdef __vita__
        const float v[8] = { sFogOn ? 1.0f : 0.0f, sFogStart, sFogEnd, sFogNear, sFogFar, sFogColour[0], sFogColour[1], sFogColour[2] };
        sFogHash = hash_bytes(0xF0C5A7E3D1B29486ull, v, sizeof(v));
#endif
    }
}


#ifndef __vita__
typedef void (APIENTRYP PCGLGENERATEMIPMAPPROC) (GLenum target);
static PCGLGENERATEMIPMAPPROC glGenerateMipmap_ptr = nullptr;
#endif
static PFNGLBLENDEQUATIONPROC glBlendEquation_ptr = nullptr;
static PFNGLGENBUFFERSPROC glGenBuffers_ptr = nullptr;
static PFNGLBINDBUFFERPROC glBindBuffer_ptr = nullptr;
static PFNGLBUFFERDATAPROC glBufferData_ptr = nullptr;
static PFNGLBUFFERSUBDATAPROC glBufferSubData_ptr = nullptr;
static PFNGLCREATESHADERPROC glCreateShader_ptr = nullptr;
static PFNGLSHADERSOURCEPROC glShaderSource_ptr = nullptr;
static PFNGLCOMPILESHADERPROC glCompileShader_ptr = nullptr;
static PFNGLCREATEPROGRAMPROC glCreateProgram_ptr = nullptr;
static PFNGLATTACHSHADERPROC glAttachShader_ptr = nullptr;
static PFNGLLINKPROGRAMPROC glLinkProgram_ptr = nullptr;
static PFNGLUSEPROGRAMPROC glUseProgram_ptr = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_ptr = nullptr;
static PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation_ptr = nullptr;
static PFNGLBINDATTRIBLOCATIONPROC glBindAttribLocation_ptr = nullptr;
static PFNGLDELETESHADERPROC glDeleteShader_ptr = nullptr;
static PFNGLDELETEPROGRAMPROC glDeleteProgram_ptr = nullptr;
static PFNGLGETUNIFORMIVPROC glGetUniformiv_ptr = nullptr;
static PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv_ptr = nullptr;
typedef void (APIENTRYP PFNGLUNIFORMMATRIX3FVPROC) (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
static PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv_ptr = nullptr;
static PFNGLUNIFORM1IPROC glUniform1i_ptr = nullptr;
static PFNGLUNIFORM1FPROC glUniform1f_ptr = nullptr;
static PFNGLUNIFORM4FPROC glUniform4f_ptr = nullptr;
static PFNGLUNIFORM2IPROC glUniform2i_ptr = nullptr;
static PFNGLUNIFORM4IPROC glUniform4i_ptr = nullptr;
static PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_ptr = nullptr;
static PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer_ptr = nullptr;
static PFNGLGETSHADERIVPROC glGetShaderiv_ptr = nullptr;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_ptr = nullptr;
static PFNGLGETPROGRAMIVPROC glGetProgramiv_ptr = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_ptr = nullptr;
static PFNGLGETACTIVEUNIFORMPROC glGetActiveUniform_ptr = nullptr;
static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_ptr = nullptr;
static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_ptr = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_ptr = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_ptr = nullptr;
static PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer_ptr = nullptr;
static PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers_ptr = nullptr;
static PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer_ptr = nullptr;
static PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage_ptr = nullptr;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_ptr = nullptr;
static PCGLGENQUERIESPROC glGenQueries_ptr = nullptr;
static PCGLBEGINQUERYPROC glBeginQuery_ptr = nullptr;
static PCGLENDQUERYPROC glEndQuery_ptr = nullptr;
static PCGLGETQUERYOBJECTIVPROC glGetQueryObjectiv_ptr = nullptr;
static PCGLGETQUERYOBJECTUI64VPROC glGetQueryObjectui64v_ptr = nullptr;

#include <SDL2/SDL.h>
#ifdef __vita__
// vitaGL entry points.
#define SDL_GL_GetProcAddress vglGetProcAddress
#endif

static void load_gl_functions() {
    glActiveTexture_ptr = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
#ifndef __vita__
    glGenerateMipmap_ptr = (PCGLGENERATEMIPMAPPROC)SDL_GL_GetProcAddress("glGenerateMipmap");
#endif
    glBlendEquation_ptr = (PFNGLBLENDEQUATIONPROC)SDL_GL_GetProcAddress("glBlendEquation");
    glGenBuffers_ptr = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
    glBindBuffer_ptr = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
    glBufferData_ptr = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");
    glBufferSubData_ptr = (PFNGLBUFFERSUBDATAPROC)SDL_GL_GetProcAddress("glBufferSubData");
    glCreateShader_ptr = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
    glShaderSource_ptr = (PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress("glShaderSource");
    glCompileShader_ptr = (PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
    glCreateProgram_ptr = (PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
    glAttachShader_ptr = (PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
    glLinkProgram_ptr = (PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
    glUseProgram_ptr = (PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
    glGetUniformLocation_ptr = (PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
    glGetAttribLocation_ptr = (PFNGLGETATTRIBLOCATIONPROC)SDL_GL_GetProcAddress("glGetAttribLocation");
    glBindAttribLocation_ptr = (PFNGLBINDATTRIBLOCATIONPROC)SDL_GL_GetProcAddress("glBindAttribLocation");
    glDeleteShader_ptr = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
    glDeleteProgram_ptr = (PFNGLDELETEPROGRAMPROC)SDL_GL_GetProcAddress("glDeleteProgram");
    glGetUniformiv_ptr = (PFNGLGETUNIFORMIVPROC)SDL_GL_GetProcAddress("glGetUniformiv");
    glUniformMatrix4fv_ptr = (PFNGLUNIFORMMATRIX4FVPROC)SDL_GL_GetProcAddress("glUniformMatrix4fv");
    glUniformMatrix3fv_ptr = (PFNGLUNIFORMMATRIX3FVPROC)SDL_GL_GetProcAddress("glUniformMatrix3fv");
    glUniform1i_ptr = (PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
    glUniform1f_ptr = (PFNGLUNIFORM1FPROC)SDL_GL_GetProcAddress("glUniform1f");
    glUniform4f_ptr = (PFNGLUNIFORM4FPROC)SDL_GL_GetProcAddress("glUniform4f");
    glUniform2i_ptr = (PFNGLUNIFORM2IPROC)SDL_GL_GetProcAddress("glUniform2i");
    glUniform4i_ptr = (PFNGLUNIFORM4IPROC)SDL_GL_GetProcAddress("glUniform4i");
    glEnableVertexAttribArray_ptr = (PFNGLENABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
    glVertexAttribPointer_ptr = (PFNGLVERTEXATTRIBPOINTERPROC)SDL_GL_GetProcAddress("glVertexAttribPointer");
    glGetShaderiv_ptr = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
    glGetShaderInfoLog_ptr = (PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
    glGetProgramiv_ptr = (PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
    glGetProgramInfoLog_ptr = (PFNGLGETPROGRAMINFOLOGPROC)SDL_GL_GetProcAddress("glGetProgramInfoLog");
    glGetActiveUniform_ptr = (PFNGLGETACTIVEUNIFORMPROC)SDL_GL_GetProcAddress("glGetActiveUniform");
    glGenFramebuffers_ptr = (PFNGLGENFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glGenFramebuffers");
    glBindFramebuffer_ptr = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
    glFramebufferTexture2D_ptr = (PFNGLFRAMEBUFFERTEXTURE2DPROC)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    glCheckFramebufferStatus_ptr = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    glBlitFramebuffer_ptr = (PFNGLBLITFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBlitFramebuffer");
    glGenRenderbuffers_ptr = (PFNGLGENRENDERBUFFERSPROC)SDL_GL_GetProcAddress("glGenRenderbuffers");
    glBindRenderbuffer_ptr = (PFNGLBINDRENDERBUFFERPROC)SDL_GL_GetProcAddress("glBindRenderbuffer");
    glRenderbufferStorage_ptr = (PFNGLRENDERBUFFERSTORAGEPROC)SDL_GL_GetProcAddress("glRenderbufferStorage");
    glFramebufferRenderbuffer_ptr = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    glGenQueries_ptr = (PCGLGENQUERIESPROC)SDL_GL_GetProcAddress("glGenQueries");
    glBeginQuery_ptr = (PCGLBEGINQUERYPROC)SDL_GL_GetProcAddress("glBeginQuery");
    glEndQuery_ptr = (PCGLENDQUERYPROC)SDL_GL_GetProcAddress("glEndQuery");
    glGetQueryObjectiv_ptr = (PCGLGETQUERYOBJECTIVPROC)SDL_GL_GetProcAddress("glGetQueryObjectiv");
    glGetQueryObjectui64v_ptr = (PCGLGETQUERYOBJECTUI64VPROC)SDL_GL_GetProcAddress("glGetQueryObjectui64v");

    // Every pointer above is called without a null check, so a missing entry
    // point crashes the moment that feature is first used -- which can be deep
    // into a level rather than at startup. Windows' opengl32 exports only
    // OpenGL 1.1, so this is where a driver too old for the port shows up.
    // Name what is missing instead of leaving a silent landmine.
    {
        struct Entry { const char* name; const void* ptr; };
        const Entry entries[] = {
            { "glActiveTexture", (const void*)glActiveTexture_ptr },
            { "glBlendEquation", (const void*)glBlendEquation_ptr },
            { "glGenBuffers", (const void*)glGenBuffers_ptr },
            { "glBindBuffer", (const void*)glBindBuffer_ptr },
            { "glBufferData", (const void*)glBufferData_ptr },
            { "glCreateShader", (const void*)glCreateShader_ptr },
            { "glCreateProgram", (const void*)glCreateProgram_ptr },
            { "glUseProgram", (const void*)glUseProgram_ptr },
            { "glUniformMatrix4fv", (const void*)glUniformMatrix4fv_ptr },
        };
        int missing = 0;
        for (const Entry& entry : entries) {
            if (entry.ptr == nullptr) {
                fprintf(stderr, "[PC GX] OpenGL entry point missing: %s\n", entry.name);
                ++missing;
            }
        }
        if (missing != 0) {
            fprintf(stderr, "[PC GX] %d OpenGL entry point(s) unavailable. The driver is "
                            "too old for this port; expect a crash when they are used.\n",
                    missing);
        }
    }
}

// OpenGL uniform calls carry non-trivial validation/dispatch overhead. GX code
// commonly emits the same material state before many small primitives, so keep
// the last value for every location and only cross the driver boundary when it
// actually changes. Locations outside this deliberately generous range fall
// back to the raw call.
constexpr int PC_UNIFORM_CACHE_SIZE = 4096;
static_assert(PC_UNIFORM_CACHE_SIZE == kVgxUniformCacheSlots, "the render thread's copy of this cache (rtunif) must have the same slots");
// A location number only identifies a uniform within one program, and shader
// specialisation means several programs are now in play. Entries therefore
// carry the generation they were written in, and binding a different program
// bumps the generation: stale values are rejected in O(1) instead of clearing
// every table on each switch.
static uint32_t sUniformGeneration = 1;
// Diagnostic escape hatch: PIKMIN_NO_UNIFORM_CACHE=1 sends every write straight
// to the driver. If a symptom disappears with it set, the cache is at fault and
// not the generated shader.
static bool uniform_cache_disabled() {
    static const bool disabled = std::getenv("PIKMIN_NO_UNIFORM_CACHE") != nullptr;
    return disabled;
}
static void invalidate_uniform_cache() {
    if (++sUniformGeneration == 0) sUniformGeneration = 1;
}
static void invalidate_uniform_cache_on_program_switch() {
#ifndef __vita__
    invalidate_uniform_cache();
#endif
}
template <typename T> struct UniformCacheEntry {
    uint32_t generation = 0;
    T value {};
};
struct Uniform2iValue { GLint x, y; };
struct Uniform4iValue { GLint x, y, z, w; };
struct Uniform4fValue { GLfloat x, y, z, w; };
struct UniformMat4Value { GLfloat value[16]; };
struct UniformMat3Value { GLfloat value[9]; };
static UniformCacheEntry<GLint> sUniform1iCache[PC_UNIFORM_CACHE_SIZE];
static UniformCacheEntry<GLfloat> sUniform1fCache[PC_UNIFORM_CACHE_SIZE];
static UniformCacheEntry<Uniform2iValue> sUniform2iCache[PC_UNIFORM_CACHE_SIZE];
static UniformCacheEntry<Uniform4iValue> sUniform4iCache[PC_UNIFORM_CACHE_SIZE];
static UniformCacheEntry<Uniform4fValue> sUniform4fCache[PC_UNIFORM_CACHE_SIZE];
static UniformCacheEntry<UniformMat4Value> sUniformMat4Cache[PC_UNIFORM_CACHE_SIZE];
static UniformCacheEntry<UniformMat3Value> sUniformMat3Cache[PC_UNIFORM_CACHE_SIZE];

#ifdef __vita__
// Location owning each uniform cache slot.
static GLint sUniformSlotOwner[PC_UNIFORM_CACHE_SIZE];
static int uniform_cache_slot(GLint loc) {
    const int slot = int((static_cast<uint32_t>(loc) / 8u) % uint32_t(PC_UNIFORM_CACHE_SIZE));
    if (sUniformSlotOwner[slot] != loc) {
        sUniformSlotOwner[slot] = loc;
        sUniform1iCache[slot].generation = 0;
        sUniform1fCache[slot].generation = 0;
        sUniform2iCache[slot].generation = 0;
        sUniform4iCache[slot].generation = 0;
        sUniform4fCache[slot].generation = 0;
        sUniformMat4Cache[slot].generation = 0;
        sUniformMat3Cache[slot].generation = 0;
    }
    return slot;
}
#else
static inline int uniform_cache_slot(GLint loc) { return loc; }
#endif


// Names the exact uniform write that OpenGL rejects. A rejected write leaves
// the shader reading a stale value, which is indistinguishable from a wrong
// shader by looking at the picture alone. Enabled by PIKMIN_GL_CHECK.
static GLuint sCheckProgram = 0;
static bool uniform_check_enabled() {
    static const bool enabled = std::getenv("PIKMIN_GL_CHECK") != nullptr;
    return enabled;
}
static void check_uniform_write(GLint loc, const char* kind) {
    if (!uniform_check_enabled()) return;
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return;
    static int reported = 0;
    if (reported++ > 40) return;
    char name[128] = "?";
    GLint size = 0; GLenum type = 0; GLsizei length = 0;
    // Locations are not indices, so find the active uniform that owns this one.
    GLint count = 0;
    if (glGetProgramiv_ptr) glGetProgramiv_ptr(sCheckProgram, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
        char probe[128];
        if (!glGetActiveUniform_ptr) break;
        glGetActiveUniform_ptr(sCheckProgram, i, sizeof(probe), &length, &size, &type, probe);
        if (glGetUniformLocation_ptr(sCheckProgram, probe) == loc) {
            snprintf(name, sizeof(name), "%s", probe);
            break;
        }
    }
    printf("[PC GX uniform] %s write to location %d rejected (0x%04x) in program %u; that location holds '%s'\n",
           kind, loc, unsigned(error), unsigned(sCheckProgram), name);
    fflush(stdout);
}

#ifdef __vita__
// Repoint GL function pointers at the recorders.
static void vita_rt_override_gl_pointers() {
    glActiveTexture_ptr        = rec_glActiveTexture;
    glBlendEquation_ptr        = rec_glBlendEquation;
    glGenBuffers_ptr           = rec_glGenBuffers;
    glBindBuffer_ptr           = rec_glBindBuffer;
    glBufferData_ptr           = rec_glBufferData;
    glBufferSubData_ptr        = rec_glBufferSubData;
    glUniform1i_ptr            = rec_glUniform1i;
    glUniform1f_ptr            = rec_glUniform1f;
    glUniform2i_ptr            = rec_glUniform2i;
    glUniform4f_ptr            = rec_glUniform4f;
    glUniform4i_ptr            = rec_glUniform4i;
    glUniformMatrix3fv_ptr     = rec_glUniformMatrix3fv;
    glUniformMatrix4fv_ptr     = rec_glUniformMatrix4fv;
    glUseProgram_ptr           = rec_glUseProgram;
    glVertexAttribPointer_ptr  = rec_glVertexAttribPointer;
}
// Uniform images: encoded slots below -1.
static inline bool vita_slot_is_image(GLint loc) { return vgx_slot_is_image(loc); }
static void vita_img_write(GLint slot, const void* src, uint32_t words, bool srcInt);
#endif
static void cached_uniform1i(GLint loc, GLint value) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) { vita_img_write(loc, &value, 1, true); return; }
#endif
    if (loc < 0) return;
    if (uniform_cache_disabled()) { glUniform1i_ptr(loc, value); return; }
    const int slot = uniform_cache_slot(loc);
    if (slot >= PC_UNIFORM_CACHE_SIZE) { glUniform1i_ptr(loc, value); return; }
    auto& entry = sUniform1iCache[slot];
    if (entry.generation == sUniformGeneration && entry.value == value) return;
    entry.generation = sUniformGeneration; entry.value = value;
    glUniform1i_ptr(loc, value);
    check_uniform_write(loc, "1i");
}
static void cached_uniform1f(GLint loc, GLfloat value) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) { vita_img_write(loc, &value, 1, false); return; }
#endif
    if (loc < 0) return;
    if (uniform_cache_disabled()) { glUniform1f_ptr(loc, value); return; }
    const int slot = uniform_cache_slot(loc);
    if (slot >= PC_UNIFORM_CACHE_SIZE) { glUniform1f_ptr(loc, value); return; }
    auto& entry = sUniform1fCache[slot];
    if (entry.generation == sUniformGeneration && entry.value == value) return;
    entry.generation = sUniformGeneration; entry.value = value;
    glUniform1f_ptr(loc, value);
    check_uniform_write(loc, "1f");
}
static void cached_uniform2i(GLint loc, GLint x, GLint y) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) { const GLint v[2] = { x, y }; vita_img_write(loc, v, 2, true); return; }
#endif
    if (loc < 0) return;
    if (uniform_cache_disabled()) { glUniform2i_ptr(loc, x, y); return; }
    const int slot = uniform_cache_slot(loc);
    if (slot >= PC_UNIFORM_CACHE_SIZE) { glUniform2i_ptr(loc, x, y); return; }
    auto& entry = sUniform2iCache[slot];
    const Uniform2iValue value { x, y };
    if (entry.generation == sUniformGeneration && memcmp(&entry.value, &value, sizeof(value)) == 0) return;
    entry.generation = sUniformGeneration; entry.value = value;
    glUniform2i_ptr(loc, x, y);
    check_uniform_write(loc, "2i");
}
static void cached_uniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) { const GLint v[4] = { x, y, z, w }; vita_img_write(loc, v, 4, true); return; }
#endif
    if (loc < 0) return;
    if (uniform_cache_disabled()) { glUniform4i_ptr(loc, x, y, z, w); return; }
    const int slot = uniform_cache_slot(loc);
    if (slot >= PC_UNIFORM_CACHE_SIZE) { glUniform4i_ptr(loc, x, y, z, w); return; }
    auto& entry = sUniform4iCache[slot];
    const Uniform4iValue value { x, y, z, w };
    if (entry.generation == sUniformGeneration && memcmp(&entry.value, &value, sizeof(value)) == 0) return;
    entry.generation = sUniformGeneration; entry.value = value;
    glUniform4i_ptr(loc, x, y, z, w);
    check_uniform_write(loc, "4i");
}
static void cached_uniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) { const GLfloat v[4] = { x, y, z, w }; vita_img_write(loc, v, 4, false); return; }
#endif
    if (loc < 0) return;
    if (uniform_cache_disabled()) { glUniform4f_ptr(loc, x, y, z, w); return; }
    const int slot = uniform_cache_slot(loc);
    if (slot >= PC_UNIFORM_CACHE_SIZE) { glUniform4f_ptr(loc, x, y, z, w); return; }
    auto& entry = sUniform4fCache[slot];
    const Uniform4fValue value { x, y, z, w };
    if (entry.generation == sUniformGeneration && memcmp(&entry.value, &value, sizeof(value)) == 0) return;
    entry.generation = sUniformGeneration; entry.value = value;
    glUniform4f_ptr(loc, x, y, z, w);
    check_uniform_write(loc, "4f");
}
static void cached_uniform_mat4(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* value) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) {
        if (value && transpose == GL_FALSE && count > 0) vita_img_write(loc, value, uint32_t(count) * 16u, false);
        return;
    }
#endif
    if (loc < 0 || !value) return;
    if (uniform_cache_disabled()) { glUniformMatrix4fv_ptr(loc, count, transpose, value); return; }
    const int slot = uniform_cache_slot(loc);
    if (count != 1 || transpose != GL_FALSE || slot >= PC_UNIFORM_CACHE_SIZE) {
        glUniformMatrix4fv_ptr(loc, count, transpose, value); return;
    }
    auto& entry = sUniformMat4Cache[slot];
    if (entry.generation == sUniformGeneration && memcmp(entry.value.value, value, sizeof(entry.value.value)) == 0) return;
    entry.generation = sUniformGeneration; memcpy(entry.value.value, value, sizeof(entry.value.value));
    glUniformMatrix4fv_ptr(loc, count, transpose, value);
    check_uniform_write(loc, "mat4");
}
static void cached_uniform_mat3(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* value) {
#ifdef __vita__
    if (vita_slot_is_image(loc)) {
        if (value && transpose == GL_FALSE && count > 0) vita_img_write(loc, value, uint32_t(count) * 9u, false);
        return;
    }
#endif
    if (loc < 0 || !value) return;
    if (uniform_cache_disabled()) { glUniformMatrix3fv_ptr(loc, count, transpose, value); return; }
    const int slot = uniform_cache_slot(loc);
    if (count != 1 || transpose != GL_FALSE || slot >= PC_UNIFORM_CACHE_SIZE) {
        glUniformMatrix3fv_ptr(loc, count, transpose, value); return;
    }
    auto& entry = sUniformMat3Cache[slot];
    if (entry.generation == sUniformGeneration && memcmp(entry.value.value, value, sizeof(entry.value.value)) == 0) return;
    entry.generation = sUniformGeneration; memcpy(entry.value.value, value, sizeof(entry.value.value));
    glUniformMatrix3fv_ptr(loc, count, transpose, value);
    check_uniform_write(loc, "mat3");
}

#define glUniform1i_ptr cached_uniform1i
#define glUniform1f_ptr cached_uniform1f
#define glUniform2i_ptr cached_uniform2i
#define glUniform4i_ptr cached_uniform4i
#define glUniform4f_ptr cached_uniform4f
#define glUniformMatrix4fv_ptr cached_uniform_mat4
#define glUniformMatrix3fv_ptr cached_uniform_mat3

#ifdef __vita__
// Flat uniform array location mapping.
#include "vita_gx_shaders.h"
#include "vita_vs_key.h"
#undef glGetUniformLocation_ptr
#define glGetUniformLocation_ptr vita_gx_uniform_location
static void vita_build_all_programs();
static void vita_rtgx_init();
static void vita_rtgx_present();
static void vita_rotate_vbo_segment();
static void vita_dl_end_frame();         // vita_dl_cache.inl; called from pc_gfx_present
static void vita_dl_flush_all();         // vita_dl_cache.inl; pc_gfx_level_flush
static void vita_gxm_shadow_invalidate();
extern const size_t kVitaAttrOff[8];
extern const GLint  kVitaAttrSize[8];
void vita_rt_set_attr_layout(const uint32_t offsets[8], const int sizes[8], int stride);   // vita_rt.cpp
#endif

// ── State Storage ──
struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float r, g, b, a;
#ifdef __vita__
    float mtxIdx;   // matrix palette slot (ATTR7)
#endif
    // The current GLSL backend exposes four texture-coordinate varyings.
    // TEX4-TEX7 still have to be consumed from GX display lists, but retaining
    // them in every streamed vertex only inflated the upload by 32 bytes.
    float tex[4][2];
};
#ifdef __vita__
static_assert(sizeof(Vertex) == 76, "Vita vertex: 72 + palette slot");
#else
static_assert(sizeof(Vertex) == 72, "Keep the streamed GX vertex compact");
#endif

#ifdef __vita__
// Allocator that default-initialises new elements.
template <class T> struct VitaNoInitAlloc {
    using value_type = T;
    VitaNoInitAlloc() noexcept {}
    template <class U> VitaNoInitAlloc(const VitaNoInitAlloc<U>&) noexcept {}
    template <class U> struct rebind { using other = VitaNoInitAlloc<U>; };
    T* allocate(size_t n) { return std::allocator<T>().allocate(n); }
    void deallocate(T* p, size_t n) noexcept { std::allocator<T>().deallocate(p, n); }
    template <class U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <class U, class... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
};
template <class T, class U> inline bool operator==(const VitaNoInitAlloc<T>&, const VitaNoInitAlloc<U>&) noexcept { return true; }
template <class T, class U> inline bool operator!=(const VitaNoInitAlloc<T>&, const VitaNoInitAlloc<U>&) noexcept { return false; }
typedef std::vector<Vertex, VitaNoInitAlloc<Vertex>> PcVertexVec;
#else
typedef std::vector<Vertex> PcVertexVec;
#endif

static float sProjMatrix[16];
static float sPosMatrix[64][16];
static u32 sCurrentPosMtxId = 0;

// Normal matrices (3x3 rotation part, stored column-major)
static float sNrmMatrix[64][9];

// Texture coordinate generation (TexCoordGen: pc_program_locations.h)
static TexCoordGen sTexCoordGen[8];
static float sTexMatrices[64][16];
// Matrix contents change far less often than they are drawn with, so the batch
// key hashes a revision number per slot rather than the floats themselves.
// That removes ~600 of the ~2 KB the key used to walk on every primitive.
static uint32_t sPosMtxGen[64] = {};
static uint32_t sNrmMtxGen[64] = {};
static uint32_t sTexMtxGen[64] = {};
static uint32_t sProjMtxGen = 0;
#ifdef __vita__
// Per texture matrix: identity flag and its generation.
struct VitaTexMtxIdent { uint32_t gen; uint8_t valid, ident, pad[2]; };
static VitaTexMtxIdent sVitaTexMtxIdent[64];   // zeroed: nothing valid
#endif
static bool sTexMtxLoaded[64] = {};

// Lighting state (GfxLight: pc_program_locations.h)
static GfxLight sLights[8] = {};

// TEV swap mode state (TevSwapMode: pc_program_locations.h)
static TevSwapMode sTevSwapModes[4] = {
    { GX_CH_RED,   GX_CH_GREEN, GX_CH_BLUE,  GX_CH_ALPHA },
    { GX_CH_RED,   GX_CH_RED,   GX_CH_RED,   GX_CH_ALPHA },
    { GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA },
    { GX_CH_BLUE,  GX_CH_BLUE,  GX_CH_BLUE,  GX_CH_ALPHA },
};
static GXTevSwapSel sTevRasSwapSel[GX_MAXTEVSTAGE] = {};
static GXTevSwapSel sTevTexSwapSel[GX_MAXTEVSTAGE] = {};

// Lighting channels (GfxChannel: pc_program_locations.h)
static GfxChannel sChannels[2] = {}; // COLOR0/ALPHA0, COLOR1/ALPHA1

static constexpr GXAttnFn decode_xf_attn_fn(u32 control) {
    const bool bit9  = (control & (1u << 9)) != 0;
    const bool bit10 = (control & (1u << 10)) != 0;
    if (!bit10) return GX_AF_NONE;
    return bit9 ? GX_AF_SPOT : GX_AF_SPEC;
}

static constexpr u32 decode_xf_light_mask(u32 control) {
    return ((control >> 2) & 0x0Fu) | ((control >> 7) & 0xF0u);
}

static void decode_xf_channel_control(GfxChannel& ch, u32 control, bool alpha) {
    const GXColorSrc matSrc = (control & (1u << 0)) ? GX_SRC_VTX : GX_SRC_REG;
    const bool enabled = (control & (1u << 1)) != 0;
    const GXColorSrc ambSrc = (control & (1u << 6)) ? GX_SRC_VTX : GX_SRC_REG;
    const u32 lightMask = decode_xf_light_mask(control);
    const GXDiffuseFn diffFn = GXDiffuseFn((control >> 7) & 0x3);
    const GXAttnFn attnFn = decode_xf_attn_fn(control);
    if (alpha) {
        ch.alphaMatSrc = matSrc;
        ch.alphaEnabled = enabled;
        ch.alphaAmbSrc = ambSrc;
        ch.alphaLightMask = lightMask;
        ch.alphaDiffFn = diffFn;
        ch.alphaAttnFn = attnFn;
    } else {
        ch.matSrc = matSrc;
        ch.enabled = enabled;
        ch.ambSrc = ambSrc;
        ch.lightMask = lightMask;
        ch.diffFn = diffFn;
        ch.attnFn = attnFn;
    }
}

static_assert(decode_xf_light_mask((1u << 2) | (1u << 14)) == 0x81,
              "XF channel light-mask decoding must preserve lights 0 and 7");

static GLuint sVBO = 0;
static size_t sVboCapacity = 16 * 1024 * 1024;
#ifdef __vita__
// VBO ring for immediate-mode geometry.
static constexpr int    kVitaVboSegs = 5;
static constexpr size_t kVitaVboSeg  = sizeof(Vertex) * 20000;   // cached display lists carry the bulk
// vitaGL glDrawArrays vertex limit (MAX_IDX_NUMBER).
static constexpr GLsizei kVitaMaxDrawVerts = 0xC000;
#endif
// One-frame draw dump (PIKMIN_VDUMP_FRAME).
static unsigned dump_frame_number() {
#ifdef __vita__
    return 0u;
#else
    static const unsigned n = [] {
        const char* v = std::getenv("PIKMIN_VDUMP_FRAME");
        return v ? static_cast<unsigned>(std::atoi(v)) : 0u;
    }();
    return n;
#endif
}
static bool dump_frame_active(unsigned frame) {
    return dump_frame_number() != 0 && frame == dump_frame_number();
}
static unsigned  sVitaFrameNo = 0;
static bool      sVitaDumpActive = false;
static unsigned  sVitaDumpDraw = 0;
static int       sVitaBlendType = 0, sVitaBlendSrc = 0, sVitaBlendDst = 0;
static uint64_t  sVitaLastKeyHash = 0;
static size_t sVboWriteOffset = 0;
static GLuint sShaderProgram = 0;
static GLuint sNativeFramebuffer = 0;
static GLuint sNativeColorTexture = 0;
static GLuint sNativeDepthStencil = 0;
static bool sNativeFramebufferReady = false;
static int sRenderWidth = 640;
static int sRenderHeight = 480;
static float sRenderScale = 2.0f / 3.0f;
static bool sRenderScaleSet = false; // true once set by the settings module

// Aspect ratio support.
enum AspectRatioMode {
    ASPECT_AUTO = 0,    // Detect from window
    ASPECT_4_3 = 1,
    ASPECT_16_10 = 2,
    ASPECT_16_9 = 3,
    ASPECT_21_9 = 4,
    ASPECT_COUNT
};
static int sAspectRatioMode = ASPECT_AUTO;
static float sCurrentAspectRatio = 4.0f / 3.0f; // Current active aspect ratio
static GLuint sBoundTextures[8] = {};
static uint64_t sPerfDraws = 0;
static uint64_t sPerfVertices = 0;
static uint64_t sPerfDisplayLists = 0;
static uint64_t sPerfDisplayListBytes = 0;
static uint64_t sPerfTextureUploads = 0;
#ifdef __vita__
// Static display-list cache (vita_dl_cache.inl).
static bool     sVitaVatDirty = true, sVitaDescDirty = true;
// Triangles per spatial cell of a big cached list.
static constexpr int kVitaDlChunkTris = 600;
// Soft alpha test program tag.
static constexpr uint64_t kVitaSoftAlphaTag = 0x5AF7A1FA5AF7A1FAull;
static bool     sVitaZWriteOn = true;        // GXSetZMode update flag, as last set
// Alpha-test elision program tag.
static constexpr uint64_t kVitaElideAlphaTag = 0xE11DEA1FA0E11DEAull;
static float    sVitaRasAlphaMin = 0.0f;     // vertex alpha lower bound of the draw being applied
static std::unordered_map<GLuint, uint8_t> sTexMinAlpha;   // minimum texel alpha per GL texture (255 = opaque)
static uint8_t  sActiveTexMinAlpha[GX_MAX_TEXMAP] = {};    // per texmap, for the bound texture (0 = unknown)
// Per-texture 16x16 grid of minimum alpha.
struct VitaTexAlphaGrid { uint8_t cell[16][16]; uint8_t minAll; };   // minimum of all 256 cells
static std::unordered_map<GLuint, VitaTexAlphaGrid> sTexAlphaGrid;
static const VitaTexAlphaGrid* sActiveTexGrid[GX_MAX_TEXMAP] = {};
static uint16_t sActiveTexW[GX_MAX_TEXMAP] = {}, sActiveTexH[GX_MAX_TEXMAP] = {};
static uint8_t  sActiveTexWrapS[GX_MAX_TEXMAP] = {}, sActiveTexWrapT[GX_MAX_TEXMAP] = {};   // GXTexWrapMode
static VitaTexAlphaGrid sVitaUploadGrid;      // filled by the upload paths
static void vita_grid_reset() { memset(&sVitaUploadGrid, 255, sizeof(sVitaUploadGrid)); }
static void vita_grid_min_rect(int x0, int y0, int x1, int y1, int w, int h, uint8_t a) {
    if (w <= 0 || h <= 0) return;
    const int cx0 = std::max(0, x0 * 16 / w), cx1 = std::min(15, x1 * 16 / w);
    const int cy0 = std::max(0, y0 * 16 / h), cy1 = std::min(15, y1 * 16 / h);
    for (int cy = cy0; cy <= cy1; ++cy) for (int cx = cx0; cx <= cx1; ++cx) if (a < sVitaUploadGrid.cell[cy][cx]) sVitaUploadGrid.cell[cy][cx] = a;
}
// Grid from a row-major image whose alpha byte sits at `alpha` with `stride` bytes per texel.
static void vita_grid_from_alpha(const uint8_t* alpha, size_t stride, int w, int h) {
    for (int cy = 0; cy < 16; ++cy) for (int cx = 0; cx < 16; ++cx) {
        const int y0 = cy * h / 16, y1 = std::max(y0 + 1, (cy + 1) * h / 16);
        const int x0 = cx * w / 16, x1 = std::max(x0 + 1, (cx + 1) * w / 16);
        uint8_t m = 255;
        for (int y = y0; y < y1 && y < h; ++y) {
            const uint8_t* p = alpha + (size_t(y) * size_t(w) + size_t(x0)) * stride;
            for (int x = x0; x < x1 && x < w; ++x, p += stride) if (*p < m) m = *p;
        }
        sVitaUploadGrid.cell[cy][cx] = m;
    }
}
// Texcoord bounds of the cached list being applied, per attribute slot.
static float sVitaListUvMin[4][2], sVitaListUvMax[4][2];
static bool  sVitaListUvValid = false;
// Hash of those bounds.
static uint64_t sVitaListUvHash = 0;
static uint8_t sVitaStageLo[GX_MAXTEVSTAGE] = {};   // per stage: texture alpha lower bound (x255)
static bool  sVitaStageLoRegion = false;             // some stage's bound came from the sampled region
static uint8_t  sVitaProgSelKind[256] = {};   // per program-selection memo entry: 0 tested, 1 soft, 2 elided, 3 rejected
static bool     sVitaDrawRejected = false;    // the selected draw cannot pass its alpha test
static uint8_t  sVitaCurAlphaKind = 0xFF;     // selected program's alpha-test kind: 0 tested, 1 soft, 2 elided, 3 rejected, 0xFF no test
// Active texture alpha generation.
static uint32_t sActiveTexAlphaGen = 1;
// Per GL program: the batch key its uniforms hold.
static uint64_t sVitaProgramAppliedKeyArr[16385]; static uint8_t sVitaProgramAppliedKeyValid[16385];
static inline void vita_prog_applied_set(GLuint prog, uint64_t key) { if (prog < 16385u) { sVitaProgramAppliedKeyArr[prog] = key; sVitaProgramAppliedKeyValid[prog] = 1; } }
static inline bool vita_prog_applied_is(GLuint prog, uint64_t key) { return prog < 16385u && sVitaProgramAppliedKeyValid[prog] && sVitaProgramAppliedKeyArr[prog] == key; }
static inline void vita_prog_applied_clear(GLuint prog) { if (prog < 16385u) sVitaProgramAppliedKeyValid[prog] = 0; }
static size_t   sVitaDlBytes = 0;            // display-list cache bytes
// Batch key blocks.
enum : unsigned { kKeyTev = 1u, kKeyChan = 2u, kKeyAlpha = 4u, kKeyTexGen = 8u, kKeyAll = 15u };
static unsigned sKeyDirty = kKeyAll;
static uint64_t sKeyBlockTev = 0, sKeyBlockChan = 0, sKeyBlockAlpha = 0, sKeyBlockTexGen = 0;
static uint64_t sKeyBlockTevIdent = 0;   // program-identity part of the TEV block
static uint64_t sKeyCalls = 0;           // batch-key computation count
// Key call that last refreshed sVitaStageLo.
static uint64_t sVitaStageLoKeyCall = ~0ull;
void vita_key_dirty(unsigned mask) { sKeyDirty |= mask; }
static bool sChanSrcDirty = true;   // a channel's colour source changed
// Immediate-path fold chain (vita_imm_chain.h).
static uint32_t sVitaBatchSerial = 0;          // ++ at every batch open (pc_gfx_end_core)
// Last stream primitive's full-path outcome, for the fold chain.
struct VitaImmEndNote { uint8_t outcome, matFold, which, rgbConst, aConst, reg0Neg; unsigned reads; };
static VitaImmEndNote sVitaImmEnd;
static bool     sVitaImmNoting = false;
VitaImmChain g_vitaImmChain;
// Fast stream decode.
static float    sVitaU8F[256];                 // float(i) / 255.0f
static const char* sVitaRingGpu = nullptr;  // GPU address of the ring VBO (vitaGL glMapBuffer)
static bool     sVitaFrameFirstDraw = true; // first draw of the frame
#endif
static uint64_t sPerfFastDraws = 0;
static uint64_t sPerfSourcePrimitives = 0;
static uint64_t sPerfFastPathDraws[4] = {};
static uint64_t sPerfFastPathVertices[4] = {};
static uint64_t sPerfPrimitiveDraws[7] = {};
static bool sPerfStatsEnabled = false;
static constexpr unsigned PC_GPU_QUERY_RING_SIZE = 8;
struct PerfGpuQuery {
    GLuint scene = 0;
    GLuint blit = 0;
    bool pending = false;
};
static PerfGpuQuery sPerfGpuQueries[PC_GPU_QUERY_RING_SIZE] = {};
static unsigned sPerfGpuQueryWrite = 0;
static bool sPerfGpuQueriesReady = false;
static bool sPerfGpuSceneActive = false;
static uint64_t sPerfGpuSceneNs = 0;
static uint64_t sPerfGpuBlitNs = 0;
static uint64_t sPerfGpuSamples = 0;
struct PerfScope {
    const char* name = nullptr;
    uint64_t draws = 0;
    uint64_t vertices = 0;
    u8 pipeline5ReportCount = 0;
    u8 pipeline7ReportCount = 0;
};
static PerfScope sPerfScopes[8] = {};
static int sPerfCurrentScope = -1;
static uint64_t sPerfTevStageDraws[GX_MAXTEVSTAGE + 1] = {};
struct PerfTevPattern { uint64_t key = 0; uint64_t count = 0; };
static PerfTevPattern sPerfTevPatterns[128] = {};
struct PerfTevMultiPattern { uint64_t key = 0; uint64_t count = 0; u8 stages = 0; };
static PerfTevMultiPattern sPerfTevMultiPatterns[128] = {};

// Ubershader locations.
static ProgramLocations sUberLocations;
#ifdef __vita__
// Active program's locations.
static const ProgramLocations* sLocP = &sUberLocations;
#define sLoc (*sLocP)
#else
static ProgramLocations sLoc;
#endif
// Fixed vertex attribute slots, forced with glBindAttribLocation on every
// program so a single vertex array object describes them all.
enum : GLuint {
    kAttrPosIndex       = 0,
    kAttrColorIndex     = 1,
    kAttrNormalIndex    = 2,
    kAttrTexCoord0Index = 3, // through 6; the Vertex carries four of them
};
static constexpr int kVertexTexCoordCount = 4;
static GLint sAttrNormal = GLint(kAttrNormalIndex);
// Only the four coordinates the streamed Vertex actually carries get a slot;
// the rest stay -1 so nothing enables an array that reads past the struct.
static GLint sAttrTexCoord[8] = {
    GLint(kAttrTexCoord0Index + 0), GLint(kAttrTexCoord0Index + 1),
    GLint(kAttrTexCoord0Index + 2), GLint(kAttrTexCoord0Index + 3),
    -1, -1, -1, -1,
};
static GLint sAttrPos = GLint(kAttrPosIndex);
static GLint sAttrColor = GLint(kAttrColorIndex);

// Current program and the shared vertex shader.
static GLuint sCurrentProgram = 0;
static GLuint sSharedVertexShader = 0;
// Shared vertex shader with a 3-slot palette.
static GLuint sSharedVertexShaderSingle = 0;
// Drawing a cached list that indexes the palette.
static bool   sVitaNeedPalette = false;
// uPosPal slots the current draw indexes.
static int    sVitaPaletteSlots = 1;

#ifdef __vita__
// Uniform images: per-program CPU copies of the default uniform buffer.
struct VitaUniformImage {
    uint8_t* img[2]  = { nullptr, nullptr };        // [0] vertex, [1] fragment
    uint32_t size[2] = { 0, 0 };
    uint32_t lo[2]   = { UINT32_MAX, UINT32_MAX };  // pending (not yet sent) byte range
    uint32_t hi[2]   = { 0, 0 };
};
static constexpr int kVitaMaxPrograms = 16385;      // vitaGL program ids are 1..16384
// Per program: block hashes its uniform image holds.
struct VitaProgApplied { uint64_t tev = 0, chan = 0, alpha = 0, texgen = 0; uint32_t proj = 0; uint64_t fog = 0; bool valid = false; };   // fog: sFogHash
static VitaProgApplied sVitaProgApplied[kVitaMaxPrograms];
static_assert(kVitaMaxPrograms == 16385, "sVitaProgramAppliedKeyArr is sized for kVitaMaxPrograms");
static VitaUniformImage sVitaImg[kVitaMaxPrograms];
static bool     sVitaColor1FromAttr = false;   // register-1 fold in effect
// Palette provenance of the last cached-list palette upload.
static bool     sVitaPalProvValid = false;
// Palette provenance per program.
static unsigned sVitaPalProgPresents = 0;        // present counter
static void vita_palprog_forget(GLuint prog);   // vita_dl_cache.inl
static void vita_palprog_forget_all();
// Uniform sections on the render thread.
static bool     sVitaRtUnif = false;

static inline uint32_t vita_slot_code(GLint loc)   { return vgx_slot_code(loc); }
static inline int      vita_slot_stage(GLint loc)  { return vgx_slot_stage(loc); }
static inline bool     vita_slot_int(GLint loc)    { return vgx_slot_int(loc); }
static inline uint32_t vita_slot_words(GLint loc)  { return vgx_slot_words(loc); }
static inline uint32_t vita_slot_offset(GLint loc) { return vgx_slot_offset(loc); }

// vitaGL location to image slot, or the raw location.
static GLint vita_encode_slot(GLuint program, GLint loc) {
    if (loc == -1 || loc == 0 || program == 0 || program >= GLuint(kVitaMaxPrograms)) return loc;
    uint32_t bytes = 0; GLenum stage = 0; int type = 0;
    const int32_t off = vglGetUniformShadowOffset(program, loc, &bytes, &stage, &type);
    if (off < 0 || off > 0xFFFF || bytes == 0 || (bytes & 3u) != 0 || (bytes / 4u) > 0xFFFu) return loc;
    const bool isInt = type == SCE_GXM_PARAMETER_TYPE_S32 || type == SCE_GXM_PARAMETER_TYPE_U32;
    if (type != SCE_GXM_PARAMETER_TYPE_F32 && !isInt) return loc;   // F16 etc.
    const uint32_t code = (uint32_t(stage == GL_FRAGMENT_SHADER) << 29) | (uint32_t(isInt) << 28)
                        | ((bytes / 4u) << 16) | uint32_t(off);
    return GLint(-(int32_t(code) + 2));
}

// Write words 32-bit values into an image slot.
static void vita_img_write(GLint slot, const void* src, uint32_t words, bool srcInt) {
    if (sCurrentProgram >= GLuint(kVitaMaxPrograms)) return;
    VitaUniformImage& im = sVitaImg[sCurrentProgram];
    const int st = vita_slot_stage(slot);
    uint8_t* img = im.img[st];
    if (!img) return;
    uint32_t tmp[128];
    const void* data;
    uint32_t off, bytes;
    if (!vgx_img_prepare(slot, src, words, srcInt, im.size[st], tmp, &data, &off, &bytes)) return;
    uint8_t* dst = img + off;
    if (memcmp(dst, data, bytes) == 0) return;
    memcpy(dst, data, bytes);
    if (off < im.lo[st]) im.lo[st] = off;
    if (off + bytes > im.hi[st]) im.hi[st] = off + bytes;
}

// Send the pending uniform image ranges of a program.
static void vita_img_flush(GLuint program) {
    if (sVitaRtUnif) return;
    if (program == 0 || program >= GLuint(kVitaMaxPrograms)) return;
    VitaUniformImage& im = sVitaImg[program];
    for (int st = 0; st < 2; ++st) {
        if (im.hi[st] <= im.lo[st]) continue;
        const uint32_t off = im.lo[st], bytes = im.hi[st] - im.lo[st];
        uint8_t* p = static_cast<uint8_t*>(vita_rt_emit(RT_UNIFORM_IMAGE, 16 + size_t(bytes)));
        if (p) {
            rec_put(p, uint32_t(program)); rec_put(p, uint32_t(st)); rec_put(p, off); rec_put(p, bytes);
            memcpy(p, im.img[st] + off, bytes);
        }
        im.lo[st] = UINT32_MAX; im.hi[st] = 0;
    }
}

// glUniform4fv for the palette writers.
static inline void vita_uniform4fv(GLint loc, GLsizei count, const GLfloat* v) {
    if (vita_slot_is_image(loc)) { vita_img_write(loc, v, uint32_t(count) * 4u, false); return; }
    if (loc >= 0) glUniform4fv(loc, count, v);
}
#endif // __vita__

// Translator sink for the section writers.
struct PcXlatSink {
    void put(GLint loc, int kind, uint32_t words, const void* data) {
        const GLint* iv = static_cast<const GLint*>(data);
        const GLfloat* fv = static_cast<const GLfloat*>(data);
        (void)words;
        switch (kind) {
        case kVgxPut1i:   cached_uniform1i(loc, iv[0]); break;
        case kVgxPut1f:   cached_uniform1f(loc, fv[0]); break;
        case kVgxPut2i:   cached_uniform2i(loc, iv[0], iv[1]); break;
        case kVgxPut4i:   cached_uniform4i(loc, iv[0], iv[1], iv[2], iv[3]); break;
        case kVgxPut4f:   cached_uniform4f(loc, fv[0], fv[1], fv[2], fv[3]); break;
        case kVgxPutMat4: cached_uniform_mat4(loc, 1, GL_FALSE, fv); break;
#ifdef __vita__
        case kVgxPut4fv:  vita_uniform4fv(loc, GLsizei(words / 4u), fv); break;   // palette rows
#endif
        default: break;
        }
    }
};

// One generated program per TEV configuration instead of the interpreting
// ubershader. PIKMIN_TEV_SPECIALIZE=0 falls back to the ubershader for the
// whole session; PIKMIN_TEV_MAX_STAGES=N restricts specialisation to materials
// of at most N stages, which is useful for bisecting a suspected bad material.
static bool sSpecialiseShaders = true;
static int sSpecialiseMaxStages = 16;
static bool sSpecialiseFailed = false;
static uint64_t sPerfShaderCompiles = 0;

static PcVertexVec sVertexStream;   // resize()d only by vita_imm_decode_fast, which writes every vertex
static GXPrimitive sCurrentPrimType;
static u16 sExpectedVerts = 0;
// Raw FIFO bytes pushed inside a primitive.
static GXVtxFmt sCurVtxFmt = GX_VTXFMT0;
static std::vector<u8> sRawPush;
static void call_display_list_impl(const void* list, u32 nbytes);
static bool sInPrimitive = false;
static inline void raw_push_be(u32 v, int bytes) { if (!sInPrimitive) return; for (int i = bytes - 1; i >= 0; --i) sRawPush.push_back(u8(v >> (8 * i))); }
static bool sHaveVertex = false;
static bool sVerticesPretransformed = false;

void pc_gfx_flush_batch(void);
// Called from each GL-state setter *after* its redundancy guard and *before*
// it touches GL, so the pending batch is drawn under the state it was built
// with. Placing it after the guard matters: most of the game's state
// programming is redundant, and flushing on those would defeat batching.
static inline void pc_gfx_note_gl_state_change() {
    pc_gfx_flush_batch();
}

// ── Render Packet Capture System ──
// Captures immutable render packets at the GX -> OpenGL boundary.
// These packets can be replayed without re-entering game code.

static PcRenderPacketStore sPacketStore;
static bool sCaptureEnabled = false;
static bool sCaptureActive = false;
static uint64_t sCurrentCaptureSerial = 0;
static std::vector<uint8_t> sCaptureBuffer;
static float sReplayClearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
static float sReplayClearDepth = 1.0f;

void pc_gfx_begin_capture(uint64_t serial) {
    if (!sCaptureEnabled) return;
    sCaptureActive = true;
    sCurrentCaptureSerial = serial;
    sCaptureBuffer.clear();
    sPacketStore.beginAuthoritativeTick(serial);
}

void pc_gfx_end_capture() {
    sCaptureActive = false;
    sPacketStore.endAuthoritativeTick();
}

void pc_gfx_enable_capture(bool enabled) {
    sCaptureEnabled = enabled;
}

bool pc_gfx_is_capture_enabled() {
    return sCaptureEnabled;
}

PcRenderPacketStore& pc_gfx_get_packet_store() {
    return sPacketStore;
}

// Replay captured display list without re-entering game code
void pc_gfx_replay_display_list(const void* list, u32 nbytes) {
    if (!list || nbytes == 0) return;
    // Temporarily disable capture during replay to avoid infinite recursion
    bool wasCapturing = sCaptureActive;
    sCaptureActive = false;
    pc_gfx_call_display_list(list, nbytes);
    sCaptureActive = wasCapturing;
}

// Replay the entire authoritative tick's captured display lists into a cleared
// internal framebuffer and present it, WITHOUT re-entering any game code.
// This demonstrates that replaying the same immutable packet twice is visually
// identical. Returns false if there is nothing to replay.
bool pc_gfx_replay_captured_frame(void) {
    PcRenderPacketStore& store = sPacketStore;
    if (!sCaptureEnabled) return false;

    const uint64_t targetSerial = pc_render_tick_serial();
    if (!store.preparePresentation(targetSerial, 1.0)) return false;

    const PcRenderFrame* frame = store.getPresentationFrame();
    if (!frame || frame->packets.empty()) {
        store.clearPresentation();
        return false;
    }

    if (!sNativeFramebufferReady || !glBindFramebuffer_ptr) return false;

    // Bind the internal framebuffer and clear to the authoritative clear color.
    pc_gfx_flush_batch();
    glBindFramebuffer_ptr(GL_FRAMEBUFFER, sNativeFramebuffer);
    glClearColor(sReplayClearColor[0], sReplayClearColor[1], sReplayClearColor[2], sReplayClearColor[3]);
    glClearDepth(sReplayClearDepth);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Replay all captured display lists without touching game state.
    bool wasCapturing = sCaptureActive;
    sCaptureActive = false;
    for (const auto& packet : frame->packets) {
        if (packet->valid && !packet->displayListPayload.empty()) {
            pc_gfx_call_display_list(packet->displayListPayload.data(),
                                     static_cast<u32>(packet->displayListPayload.size()));
        }
    }
    sCaptureActive = wasCapturing;

    store.clearPresentation();

    // Present the replayed frame to the default framebuffer.
    pc_gfx_present();
    return true;
}

struct VertexArrayState {
    const u8* base = nullptr;
    u8 stride = 0;
};struct VertexFormatState {
    GXCompCnt count = GX_COMPCNT_NULL;
    GXCompType type = GX_F32;
    u8 frac = 0;
};
static GXAttrType sVtxDesc[GX_VA_MAX_ATTR] = {};
static VertexArrayState sVtxArrays[GX_VA_MAX_ATTR];
static VertexFormatState sVtxFormats[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];

// Active texture binding
// A stale vertex array -- one the game set for an earlier model and never
// reprogrammed -- reads plausible indices out of memory that now belongs to
// something else. Recording when each array was last set distinguishes that
// from a genuinely misaligned index, which is the other way to get garbage.
static uint64_t sFrameSerial = 0;
static uint64_t sArraySetFrame[GX_VA_MAX_ATTR] = {};
static uint64_t sArraySetSerial[GX_VA_MAX_ATTR] = {};
static uint64_t sArraySetCounter = 0;

static GLuint sActiveGLTextures[GX_MAX_TEXMAP] = {};
static bool sHasActiveTextures[GX_MAX_TEXMAP] = {};
static bool sUseMaterialRgb = false;
static bool sUseMaterialAlpha = false;
static float sMaterialColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
#ifdef __vita__
// Alpha reference offset by half an 8-bit level.
static float vita_alpha_ref_for_shader(int comp, float ref) {
    const float half8 = 0.5f / 255.0f;
    switch (comp) {
    case 1: case 6: return ref - half8;   // LESS, GEQUAL
    case 3: case 4: return ref + half8;   // LEQUAL, GREATER
    default: return ref;
    }
}
#endif
static GXCompare sAlphaComp0 = GX_ALWAYS;
static GXCompare sAlphaComp1 = GX_ALWAYS;
static GXAlphaOp sAlphaOp = GX_AOP_AND;
static float sAlphaRef0 = 0.0f;
static float sAlphaRef1 = 0.0f;
static bool sColorUpdate = true;
static bool sAlphaUpdate = true;
static float sCopyClearColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
static float sCopyClearDepth = 1.0f;
static u8 sNumTevStages = 1;

// BP TEV combiner register layout. Keep these helpers as the single source of
// truth for display-list decoding; the static assertions guard against the
// operand reversal that previously made the shader compensate for bad state.
static constexpr GXTevColorArg decode_tev_color_a(u32 value) { return GXTevColorArg((value >> 12) & 0xF); }
static constexpr GXTevColorArg decode_tev_color_b(u32 value) { return GXTevColorArg((value >> 8) & 0xF); }
static constexpr GXTevColorArg decode_tev_color_c(u32 value) { return GXTevColorArg((value >> 4) & 0xF); }
static constexpr GXTevColorArg decode_tev_color_d(u32 value) { return GXTevColorArg(value & 0xF); }
static constexpr GXTevAlphaArg decode_tev_alpha_a(u32 value) { return GXTevAlphaArg((value >> 13) & 0x7); }
static constexpr GXTevAlphaArg decode_tev_alpha_b(u32 value) { return GXTevAlphaArg((value >> 10) & 0x7); }
static constexpr GXTevAlphaArg decode_tev_alpha_c(u32 value) { return GXTevAlphaArg((value >> 7) & 0x7); }
static constexpr GXTevAlphaArg decode_tev_alpha_d(u32 value) { return GXTevAlphaArg((value >> 4) & 0x7); }

static_assert(decode_tev_color_a(0x1234) == GXTevColorArg(1));
static_assert(decode_tev_color_b(0x1234) == GXTevColorArg(2));
static_assert(decode_tev_color_c(0x1234) == GXTevColorArg(3));
static_assert(decode_tev_color_d(0x1234) == GXTevColorArg(4));
static_assert(decode_tev_alpha_a(0x29CF) == GXTevAlphaArg(1));
static_assert(decode_tev_alpha_b(0x29CF) == GXTevAlphaArg(2));
static_assert(decode_tev_alpha_c(0x29CF) == GXTevAlphaArg(3));
static_assert(decode_tev_alpha_d(0x29CF) == GXTevAlphaArg(4));

static TevStageState sTevStages[GX_MAXTEVSTAGE];
static float sTevRegisters[4][4] = {
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
};
static float sKonstColors[4][4] = {
	{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}
};
static GXTevKColorSel sKonstColorSel[GX_MAXTEVSTAGE] = {};
static GXTevKAlphaSel sKonstAlphaSel[GX_MAXTEVSTAGE] = {};

// Konst colour a stage's selectors resolve to.
static void resolve_tev_konst(u8 stage, float out[4]) {
    vgx_resolve_konst(sKonstColorSel[stage], sKonstAlphaSel[stage], sKonstColors, out);
}

// Map of GXTexObj pointers to OpenGL Texture IDs
static std::unordered_map<uintptr_t, GLuint> sTextureCache;

struct PcTextureSignature {
    const void* image = nullptr;
    u16 width = 0;
    u16 height = 0;
    u32 format = 0;
    GXTexWrapMode wrapS = GX_CLAMP;
    GXTexWrapMode wrapT = GX_CLAMP;
    bool indexed = false;
    u32 tlutName = 0;
    // Sample of the image contents.
    uint64_t sample = 0;

    bool operator==(const PcTextureSignature& other) const {
        return image == other.image && width == other.width && height == other.height
            && format == other.format && wrapS == other.wrapS && wrapT == other.wrapT
            && indexed == other.indexed && tlutName == other.tlutName && sample == other.sample;
    }
};
static std::unordered_map<uintptr_t, PcTextureSignature> sTextureSignatures;

// Content-keyed index over uploaded textures.
struct PcTexBySignature { PcTextureSignature sig; GLuint tex = 0; };
static std::unordered_map<uint64_t, PcTexBySignature> sTexBySignature;

// Idle texture eviction.
static std::unordered_map<GLuint, uint32_t> sTexLastUsed;   // frame each texture was last bound
static uint32_t sTexFrame = 0;
#ifdef __vita__
// GXLoadTexObj memo.
struct VitaTexLoadMemo { uintptr_t obj; uint32_t gen; GLuint tex; const VitaTexAlphaGrid* grid; uint16_t w, h; uint8_t minAlpha, wrapS, wrapT; uint32_t frame; };
static VitaTexLoadMemo sTexLoadMemo[1024];
static uint32_t sTexCacheGen = 1;
#endif
static constexpr uint32_t kTextureIdleFrames = 300;
static uint64_t sTexEvictions = 0;
#ifdef __vita__
// Free memory floor for idle texture eviction (KB).
static constexpr unsigned kVitaTexEvictFloorKb = 8 * 1024;
#endif
static void evict_idle_textures() {
    ++sTexFrame;
    if ((sTexFrame % 120) != 0) return;
#ifdef __vita__
    if (vita_rt_vram_free_kb() > kVitaTexEvictFloorKb) return;
    bool evicted = false;
#endif
    for (auto it = sTexLastUsed.begin(); it != sTexLastUsed.end();) {
        const GLuint tex = it->first;
        if (sEfbTextureIds.count(tex)) { it = sTexLastUsed.erase(it); continue; }   // copy target
        if (sTexFrame - it->second <= kTextureIdleFrames) { ++it; continue; }
        bool bound = false;
        for (int m = 0; m < 8; ++m) if (sBoundTextures[m] == tex || sActiveGLTextures[m] == tex) bound = true;
        if (bound) { ++it; continue; }
        glDeleteTextures(1, &tex);
#ifdef __vita__
        evicted = true;
#endif
        ++sTexEvictions;
#ifdef __vita__
        ++sTexCacheGen;
#endif
        sTexShape.erase(tex);
        pc_tex_mip_forget(tex);
#ifdef __vita__
        sTexMinAlpha.erase(tex);
        sTexAlphaGrid.erase(tex);
        ++sActiveTexAlphaGen;
#endif
        for (auto c = sTextureCache.begin(); c != sTextureCache.end();) {
            if (c->second == tex) c = sTextureCache.erase(c); else ++c;
        }
        for (auto b = sTexBySignature.begin(); b != sTexBySignature.end();) {
            if (b->second.tex == tex) b = sTexBySignature.erase(b); else ++b;
        }
        it = sTexLastUsed.erase(it);
    }
#ifdef __vita__
    // Rebind unit 0 to its mirrored texture.
    if (evicted && vita_rt_recording()) glBindTexture(GL_TEXTURE_2D, sBoundTextures[0]);
#endif
}
static uint64_t texture_signature_hash(const PcTextureSignature& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix((uint64_t)(uintptr_t)s.image); mix(s.width); mix(s.height); mix(s.format);
    mix((uint64_t)s.wrapS); mix((uint64_t)s.wrapT); mix(s.indexed ? 1 : 0); mix(s.tlutName);
    mix(s.sample);
    return h;
}

struct PcTlut {
    std::vector<u8> rgba;
};
// GX wrap mode to GL.
static inline GLint pc_gx_wrap_to_gl(GXTexWrapMode w) {
    return w == GX_REPEAT ? GL_REPEAT : (w == GX_MIRROR ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE);
}

struct PcCiTexture {
    const u8* image = nullptr;
    u16 width = 0;
    u16 height = 0;
    GXCITexFmt format = GX_TF_C4;
    GXTexWrapMode wrapS = GX_CLAMP;
    GXTexWrapMode wrapT = GX_CLAMP;
    u32 tlutName = 0;
};
static std::unordered_map<uintptr_t, PcTlut> sTlutObjects;
static std::unordered_map<u32, PcTlut> sLoadedTluts;
static std::unordered_map<uintptr_t, PcCiTexture> sCiTextures;
static int sDrawableWidth = 640;
static int sDrawableHeight = 480;

// Pikmin renders into the GameCube's 640x480 EFB coordinate space, but we scale
// it to the target aspect ratio. Translate GX's top-left origin to OpenGL's bottom-left.
static void map_gx_rect(float x, float y, float width, float height,
                        GLint& glX, GLint& glY, GLsizei& glWidth, GLsizei& glHeight) {
    if (sEfbOffscreen) {
        // Offscreen EFB: GX pixels 1:1, top-left origin.
        glX = (GLint)lroundf(x);
        glY = (GLint)lroundf(float(sEfbOffH) - y - height);
        glWidth = (GLsizei)std::max(0L, lroundf(width));
        glHeight = (GLsizei)std::max(0L, lroundf(height));
        return;
    }
    const float targetWidth = sNativeFramebufferReady ? float(sRenderWidth) : float(sDrawableWidth);
    const float targetHeight = sNativeFramebufferReady ? float(sRenderHeight) : float(sDrawableHeight);
    
    float aspect = sCurrentAspectRatio;
    int baseWidth = int(lroundf(480.0f * aspect));
    int baseHeight = 480;
    
    const float scale = fminf(targetWidth / float(baseWidth), targetHeight / float(baseHeight));
    const float offsetX = (targetWidth - float(baseWidth) * scale) * 0.5f;
    const float offsetY = (targetHeight - float(baseHeight) * scale) * 0.5f;
    
    // Scale from 640x480 GX coordinates to the target aspect ratio
    float scaleX = float(baseWidth) / 640.0f;
    
    glX = (GLint)lroundf(offsetX + x * scaleX * scale);
    glY = (GLint)lroundf(offsetY + (480.0f - y - height) * scale);
    glWidth = (GLsizei)std::max(0L, lroundf(width * scaleX * scale));
    glHeight = (GLsizei)std::max(0L, lroundf(height * scale));
}

// ── GLSL Shaders ──
static const char* vShaderSrc = 
    "#version 140\n"
    "in vec3 aPos;\n"
    "in vec3 aNormal;\n"
    "in vec4 aColor;\n"
    "in vec2 aTexCoord0;\n"
    "in vec2 aTexCoord1;\n"
    "in vec2 aTexCoord2;\n"
    "in vec2 aTexCoord3;\n"
    "uniform mat4 uProjMtx;\n"
    "uniform mat4 uPosMtx;\n"
    "uniform mat3 uNrmMtx;\n"
    "uniform int uNumLights;\n"
    "uniform vec4 uLightPos[4];\n"
    "uniform vec4 uLightColor[4];\n"
    "uniform vec4 uLightK[4];\n"     // distance attenuation k0,k1,k2,enabled
    "uniform vec4 uAmbColor;\n"
    "uniform int uChan0En;\n"
    "uniform int uChan1En;\n"
    "uniform int uChan0AttnFn;\n"  // GXAttnFn: 0=SPEC, 1=SPOT, 2=NONE
    "uniform int uChan1AttnFn;\n"
    "uniform int uNumLights1;\n"
    "uniform vec4 uLightPos1[4];\n"
    "uniform vec4 uLightColor1[4];\n"
    "uniform vec4 uLightK1[4];\n"
    "uniform vec4 uAmbColor1;\n"
    "uniform vec4 uSpecHalf1;\n"   // specular half-vector for channel 1
    "uniform vec4 uSpecAttn1;\n"   // specular a0,a1,a2 quadratic coefficients
    "uniform int uTcMode[8];\n"      // 0=direct uv, 1=position, 2=normal
    "uniform mat4 uTcMtx[8];\n"
    "out vec3 vLit0;\n"
    "out vec3 vLit1;\n"
    "out vec4 vColor;\n"
    "out vec2 vTexCoord0;\n"
    "out vec2 vTexCoord1;\n"
    "out vec2 vTexCoord2;\n"
    "out vec2 vTexCoord3;\n"
    "vec3 doLights(vec3 N, vec4 wp, int n, vec4 lp[4], vec4 lc[4], vec4 lk[4]) {\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int i = 0; i < n; i++) {\n"
    "        vec3 Ld = lp[i].xyz - wp.xyz;\n"
    "        float dist = length(Ld);\n"
    "        vec3 L = Ld / max(dist, 0.001);\n"
    "        float diff = max(dot(N, L), 0.0);\n"
    "        if (lk[i].w > 0.5) {\n"
    "            diff *= clamp(lk[i].x + lk[i].y * dist + lk[i].z * dist * dist, 0.0, 1.0);\n"
    "        }\n"
    "        sum += diff * lc[i].rgb;\n"
    "    }\n"
    "    return sum;\n"
    "}\n"
    "vec2 rawTc(int index) {\n"
    "    if (index == 0) return aTexCoord0;\n"
    "    if (index == 1) return aTexCoord1;\n"
    "    if (index == 2) return aTexCoord2;\n"
    "    if (index == 3) return aTexCoord3;\n"
    "    return aTexCoord3;\n"
    "}\n"
    "vec2 genTc(int slot, vec4 viewPos, vec3 nrm, vec2 uvIn, vec2 tc0, vec2 tc1, vec2 tc2, vec2 tc3) {\n"
    "    int mode = uTcMode[slot];\n"
    "    if (mode == 0) return uvIn;\n"
    "    vec4 src = (mode == 1) ? viewPos\n"
    "             : (mode == 2) ? vec4(nrm, 1.0)\n"
    "             : (mode >= 11) ? vec4((mode == 11) ? tc0 : (mode == 12) ? tc1 : (mode == 13) ? tc2 : tc3, 0.0, 1.0)\n"
    "             : vec4(rawTc(mode - 3), 0.0, 1.0);\n"
    "    return (uTcMtx[slot] * src).xy;\n"
    "}\n"
    "void main() {\n"
    "    vec4 worldPos = uPosMtx * vec4(aPos, 1.0);\n"
    "    gl_Position = uProjMtx * worldPos;\n"
    "    vec3 N = normalize(uNrmMtx * aNormal);\n"
    "    vec3 lit0 = uAmbColor.rgb + doLights(N, worldPos, uNumLights, uLightPos, uLightColor, uLightK);\n"
    "    vec3 spec1 = vec3(0.0);\n"
    "    if (uChan1AttnFn == 0 && uNumLights1 > 0) {\n"
    "        // Specular is a ratio of two quadratics in N.H: the angle\n"
    "        // coefficients over the distance ones.\n"
    "        float cosT = max(dot(N, normalize(uSpecHalf1.xyz)), 0.0);\n"
    "        vec3 quad = vec3(1.0, cosT, cosT * cosT);\n"
    "        float num = max(0.0, dot(uSpecAttn1.xyz, quad));\n"
    "        float den = dot(uLightK1[0].xyz, quad);\n"
    "        float att = (den > 1e-5) ? clamp(num / den, 0.0, 1.0) : 0.0;\n"
    "        spec1 = att * uLightColor1[0].rgb;\n"
    "    }\n"
    "    // GX_AF_SPEC uses the channel's attenuation function to produce the\n"
    "    // specular term.  Feeding the same light through the diffuse path as\n"
    "    // well double-counts COLOR1 and saturates specular materials white.\n"
    "    vec3 diffuse1 = (uChan1AttnFn == 0)\n"
    "        ? vec3(0.0)\n"
    "        : doLights(N, worldPos, uNumLights1, uLightPos1, uLightColor1, uLightK1);\n"
    "    vec3 lit1 = uAmbColor1.rgb + diffuse1 + spec1;\n"
    "    vLit0 = uChan0En != 0 ? clamp(lit0, 0.0, 1.0) : vec3(-1.0);\n"
    "    vLit1 = uChan1En != 0 ? clamp(lit1, 0.0, 1.0) : vec3(-1.0);\n"
    "    vColor = aColor;\n"
    // GX permits later texgens to use the output of an earlier texgen as
    // their source (GX_TG_TEXCOORD0..6). Evaluate in hardware order instead
    // of falling back to the usually absent raw attribute for that slot.
    "    vec2 tc0 = genTc(0, worldPos, N, aTexCoord0, vec2(0.0), vec2(0.0), vec2(0.0), vec2(0.0));\n"
    "    vec2 tc1 = genTc(1, worldPos, N, aTexCoord1, tc0, vec2(0.0), vec2(0.0), vec2(0.0));\n"
    "    vec2 tc2 = genTc(2, worldPos, N, aTexCoord2, tc0, tc1, vec2(0.0), vec2(0.0));\n"
    "    vec2 tc3 = genTc(3, worldPos, N, aTexCoord3, tc0, tc1, tc2, vec2(0.0));\n"
    "    vTexCoord0 = tc0;\n"
    "    vTexCoord1 = tc1;\n"
    "    vTexCoord2 = tc2;\n"
    "    vTexCoord3 = tc3;\n"
    "}\n";

static const char* fShaderSrc = 
    "#version 140\n"
    "in vec3 vLit0;\n"
    "in vec3 vLit1;\n"
    "in vec4 vColor;\n"
    "in vec2 vTexCoord0;\n"
    "in vec2 vTexCoord1;\n"
    "in vec2 vTexCoord2;\n"
    "in vec2 vTexCoord3;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex0;\n"
    "uniform sampler2D uTex1;\n"
    "uniform sampler2D uTex2;\n"
    "uniform sampler2D uTex3;\n"
    "uniform sampler2D uTex4;\n"
    "uniform sampler2D uTex5;\n"
    "uniform sampler2D uTex6;\n"
    "uniform sampler2D uTex7;\n"
    "uniform int uNumStages;\n"
    "uniform int uFastPath;\n" // 1=texture, 2=raster, 3=texture*raster
    "uniform vec4 uMaterialColor;\n"
    "uniform bool uUseMaterialRgb;\n"
    "uniform bool uUseMaterialAlpha;\n"
    "uniform vec4 uMaterialColor1;\n"
    "uniform bool uUseMaterialRgb1;\n"
    "uniform ivec4 uTevChan[16];\n"
    "uniform vec4 uTevPrev;\n"
    "uniform vec4 uTevReg0;\n"
    "uniform vec4 uTevReg1;\n"
    "uniform vec4 uTevReg2;\n"
    "uniform vec4 uKonst0;\n"
    "uniform vec4 uKonst1;\n"
    "uniform vec4 uKonst2;\n"
    "uniform vec4 uKonst3;\n"
    "uniform vec4 uTevKonst[16];\n"
    "uniform int uAlphaComp0;\n"
    "uniform int uAlphaComp1;\n"
    "uniform int uAlphaOp;\n"
    "uniform float uAlphaRef0;\n"
    "uniform float uAlphaRef1;\n"
    "uniform ivec4 uTevCSel[16];\n"
    "uniform ivec4 uTevASel[16];\n"
    "uniform ivec4 uTevCOps[16];\n"
    "uniform ivec4 uTevAOps[16];\n"
    "uniform ivec4 uTevTexInfo[16];\n"
    "uniform ivec4 uTevSwapTable[4];\n"
    "uniform ivec2 uTevSwapSel[16];\n"
    "bool alphaCompare(float value, float reference, int func) {\n"
    "    if (func == 0) return false;\n"
    "    if (func == 1) return value < reference;\n"
    "    if (func == 2) return abs(value - reference) < (0.5 / 255.0);\n"
    "    if (func == 3) return value <= reference;\n"
    "    if (func == 4) return value > reference;\n"
    "    if (func == 5) return abs(value - reference) >= (0.5 / 255.0);\n"
    "    if (func == 6) return value >= reference;\n"
    "    return true;\n"
    "}\n"
    "vec2 resolveCoord(int coordIdx) {\n"
    "    if (coordIdx == 0) return vTexCoord0;\n"
    "    if (coordIdx == 1) return vTexCoord1;\n"
    "    if (coordIdx == 2) return vTexCoord2;\n"
    "    if (coordIdx == 3) return vTexCoord3;\n"
    "    return vTexCoord3;\n"
    "}\n"
    "vec4 resolveTex(int texIdx, int coordIdx) {\n"
    "    vec2 uv = resolveCoord(coordIdx);\n"
    "    if (texIdx == 0) return texture(uTex0, uv);\n"
    "    if (texIdx == 1) return texture(uTex1, uv);\n"
    "    if (texIdx == 2) return texture(uTex2, uv);\n"
    "    if (texIdx == 3) return texture(uTex3, uv);\n"
    "    if (texIdx == 4) return texture(uTex4, uv);\n"
    "    if (texIdx == 5) return texture(uTex5, uv);\n"
    "    if (texIdx == 6) return texture(uTex6, uv);\n"
    "    return texture(uTex7, uv);\n"
    "}\n"
    "float swapComponent(vec4 value, int component) {\n"
    "    if (component == 0) return value.r;\n"
    "    if (component == 1) return value.g;\n"
    "    if (component == 2) return value.b;\n"
    "    return value.a;\n"
    "}\n"
    "vec4 applySwap(vec4 value, int tableIndex) {\n"
    "    ivec4 table = uTevSwapTable[clamp(tableIndex, 0, 3)];\n"
    "    return vec4(swapComponent(value, table.x), swapComponent(value, table.y),\n"
    "                swapComponent(value, table.z), swapComponent(value, table.w));\n"
    "}\n"
    "float packTevCompare(vec3 value, int components) {\n"
    "    vec3 q = floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5);\n"
    "    if (components == 1) return q.r;\n"
    "    if (components == 2) return q.r + q.g * 256.0;\n"
    "    return q.r + q.g * 256.0 + q.b * 65536.0;\n"
    "}\n"
    "vec3 compareTevColor(int op, vec3 a, vec3 b, vec3 c, vec3 d) {\n"
    "    bool isEqual = (op & 1) != 0;\n"
    "    if (op >= 14) {\n"
    "        vec3 qa = floor(clamp(a, 0.0, 1.0) * 255.0 + 0.5);\n"
    "        vec3 qb = floor(clamp(b, 0.0, 1.0) * 255.0 + 0.5);\n"
    "        bvec3 pass = isEqual ? equal(qa, qb) : greaterThan(qa, qb);\n"
    "        return d + vec3(pass.x ? c.r : 0.0, pass.y ? c.g : 0.0, pass.z ? c.b : 0.0);\n"
    "    }\n"
    "    int components = (op < 10) ? 1 : (op < 12) ? 2 : 3;\n"
    "    float av = packTevCompare(a, components);\n"
    "    float bv = packTevCompare(b, components);\n"
    "    bool pass = isEqual ? (av == bv) : (av > bv);\n"
    "    return d + (pass ? c : vec3(0.0));\n"
    "}\n"
    "vec4 resolveC(int id, vec4 prev, vec4 c0, vec4 c1, vec4 c2, vec4 konst, vec4 tex, vec4 rast, int stage) {\n"
    "    if (id == 0) return prev;\n"
    "    if (id == 1) return vec4(prev.a, prev.a, prev.a, 1.0);\n"
    "    if (id == 2) return c0;\n"
    "    if (id == 3) return vec4(c0.a, c0.a, c0.a, 1.0);\n"
    "    if (id == 4) return c1;\n"
    "    if (id == 5) return vec4(c1.a, c1.a, c1.a, 1.0);\n"
    "    if (id == 6) return c2;\n"
    "    if (id == 7) return vec4(c2.a, c2.a, c2.a, 1.0);\n"
    "    if (id == 8) return tex;\n"
    "    if (id == 9) return vec4(tex.a, tex.a, tex.a, 1.0);\n"
    "    if (id == 10) return rast;\n"
    "    if (id == 11) return vec4(rast.a, rast.a, rast.a, 1.0);\n"
    "    if (id == 12) return vec4(1.0);\n"
    "    if (id == 13) return vec4(0.5);\n"
    "    if (id == 14) return konst;\n"
    "    return vec4(0.0);\n"
    "}\n"
    "float resolveA(int id, vec4 prev, vec4 c0, vec4 c1, vec4 c2, vec4 konst, vec4 tex, vec4 rast, int stage) {\n"
    "    if (id == 0) return prev.a;\n"
    "    if (id == 1) return c0.a;\n"
    "    if (id == 2) return c1.a;\n"
    "    if (id == 3) return c2.a;\n"
    "    if (id == 4) return tex.a;\n"
    "    if (id == 5) return rast.a;\n"
    "    if (id == 6) return konst.a;\n"
    "    return 0.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 rast0;\n"
    "    vec4 base0 = vec4(uUseMaterialRgb ? uMaterialColor.rgb : vColor.rgb,\n"
    "                      uUseMaterialAlpha ? uMaterialColor.a : vColor.a);\n"
    "    if (vLit0.x < -0.5) {\n"
    "        // Disabling channel lighting does not force vertex color: GX\n"
    "        // still selects the raster value with matSrc.\n"
    "        rast0 = base0;\n"
    "    } else {\n"
    "        rast0 = vec4(clamp(base0.rgb * vLit0, 0.0, 1.0), base0.a);\n"
    "    }\n"
    "    vec4 base1 = vec4(uUseMaterialRgb1 ? uMaterialColor1.rgb : vColor.rgb, base0.a);\n"
    "    vec4 rast1 = vLit1.x < -0.5\n"
    "        ? base1\n"
    "        : vec4(clamp(base1.rgb * vLit1, 0.0, 1.0), base1.a);\n"
    "    if (uFastPath != 0) {\n"
    "        vec4 fastRast = (uTevChan[0].x == 1) ? rast1 : rast0;\n"
    "        vec4 fastCol;\n"
    "        if (uFastPath == 1) fastCol = texture(uTex0, vTexCoord0);\n"
    "        else if (uFastPath == 2) fastCol = fastRast;\n"
    "        else fastCol = texture(uTex0, vTexCoord0) * fastRast;\n"
    "        bool fastTest0 = alphaCompare(fastCol.a, uAlphaRef0, uAlphaComp0);\n"
    "        bool fastTest1 = alphaCompare(fastCol.a, uAlphaRef1, uAlphaComp1);\n"
    "        bool fastPass = (uAlphaOp == 0) ? (fastTest0 && fastTest1) :\n"
    "                        (uAlphaOp == 1) ? (fastTest0 || fastTest1) :\n"
    "                        (uAlphaOp == 2) ? (fastTest0 != fastTest1) : (fastTest0 == fastTest1);\n"
    "        if (!fastPass) discard;\n"
    "        fragColor = fastCol;\n"
    "        return;\n"
    "    }\n"
    // TEVPREV is a real programmable TEV register (BP E0/E1), not an
    // implicit zero value at the beginning of every primitive.
    "    vec4 prev = uTevPrev;\n"
    "    vec4 c0 = uTevReg0;\n"
    "    vec4 c1 = uTevReg1;\n"
    "    vec4 c2 = uTevReg2;\n"
    "    vec4 tex = vec4(1.0);\n"
    "    for (int i = 0; i < uNumStages; i++) {\n"
    "        vec4 konst = uTevKonst[i];\n"
    "        int tIdx = uTevTexInfo[i].x;\n"
    "        int tcIdx = uTevTexInfo[i].y;\n"
    "        vec4 rawTex = (tIdx >= 0) ? resolveTex(tIdx, tcIdx) : vec4(1.0);\n"
    "        tex = applySwap(rawTex, uTevSwapSel[i].y);\n"
    "        vec4 rawRast = (uTevChan[i].x < 0) ? vec4(0.0) : (uTevChan[i].x == 1) ? rast1 : rast0;\n"
    "        vec4 rast = applySwap(rawRast, uTevSwapSel[i].x);\n"
    "        vec4 a = resolveC(uTevCSel[i].x, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        vec4 b = resolveC(uTevCSel[i].y, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        vec4 cc = resolveC(uTevCSel[i].z, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        vec4 d = resolveC(uTevCSel[i].w, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        float bias = (uTevCOps[i].y == 1) ? 0.5 : (uTevCOps[i].y == 2) ? -0.5 : 0.0;\n"
    "        float scl = (uTevCOps[i].z == 1) ? 2.0 : (uTevCOps[i].z == 2) ? 4.0 : (uTevCOps[i].z == 3) ? 0.5 : 1.0;\n"
    "        vec3 cResult;\n"
    "        vec3 cMix = a.rgb * (vec3(1.0) - cc.rgb) + b.rgb * cc.rgb;\n"
    "        int colorOp = uTevCOps[i].x & 0xff;\n"
    "        if (colorOp >= 8) cResult = compareTevColor(colorOp, a.rgb, b.rgb, cc.rgb, d.rgb);\n"
    "        else if (colorOp == 0) cResult = d.rgb + cMix;\n"
    "        else cResult = d.rgb - cMix;\n"
    "        if (colorOp < 8) cResult = (cResult + bias) * scl;\n"
    "        cResult = ((uTevCOps[i].x & 0x100) != 0) ? clamp(cResult, 0.0, 1.0) : clamp(cResult, -4.0, 4.0);\n"
    "        float al = resolveA(uTevASel[i].x, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        float bl = resolveA(uTevASel[i].y, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        float ac = resolveA(uTevASel[i].z, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        float dl = resolveA(uTevASel[i].w, prev, c0, c1, c2, konst, tex, rast, i);\n"
    "        float abias = (uTevAOps[i].y == 1) ? 0.5 : (uTevAOps[i].y == 2) ? -0.5 : 0.0;\n"
    "        float ascl = (uTevAOps[i].z == 1) ? 2.0 : (uTevAOps[i].z == 2) ? 4.0 : (uTevAOps[i].z == 3) ? 0.5 : 1.0;\n"
    "        float aResult;\n"
    "        float aMix = al * (1.0 - ac) + bl * ac;\n"
    "        int alphaOp = uTevAOps[i].x & 0xff;\n"
    "        if (alphaOp >= 14) {\n"
    "            float qa = floor(clamp(al, 0.0, 1.0) * 255.0 + 0.5);\n"
    "            float qb = floor(clamp(bl, 0.0, 1.0) * 255.0 + 0.5);\n"
    "            bool pass = ((alphaOp & 1) != 0) ? (qa == qb) : (qa > qb);\n"
    "            aResult = dl + (pass ? ac : 0.0);\n"
    "        } else if (alphaOp == 0) aResult = dl + aMix;\n"
    "        else aResult = dl - aMix;\n"
    "        if (alphaOp < 8) aResult = (aResult + abias) * ascl;\n"
    "        aResult = ((uTevAOps[i].x & 0x100) != 0) ? clamp(aResult, 0.0, 1.0) : clamp(aResult, -4.0, 4.0);\n"
    "        int cReg = uTevCOps[i].w;\n"
    // GX routes the color and alpha combiner outputs independently. Writing a
    // complete vec4 here corrupts the destination register's alpha whenever
    // colorOutReg and alphaOutReg differ, which breaks later A0/A1/A2 inputs.
    "        if (cReg == 0) prev.rgb = cResult;\n"
    "        else if (cReg == 1) c0.rgb = cResult;\n"
    "        else if (cReg == 2) c1.rgb = cResult;\n"
    "        else c2.rgb = cResult;\n"
    "        int aReg = uTevAOps[i].w;\n"
    "        if (aReg == 0) prev.a = aResult;\n"
    "        else if (aReg == 1) c0.a = aResult;\n"
    "        else if (aReg == 2) c1.a = aResult;\n"
    "        else c2.a = aResult;\n"
    "    }\n"
    "    vec4 col = prev;\n"
    "    bool test0 = alphaCompare(col.a, uAlphaRef0, uAlphaComp0);\n"
    "    bool test1 = alphaCompare(col.a, uAlphaRef1, uAlphaComp1);\n"
    "    bool pass = (uAlphaOp == 0) ? (test0 && test1) :\n"
    "                (uAlphaOp == 1) ? (test0 || test1) :\n"
    "                (uAlphaOp == 2) ? (test0 != test1) : (test0 == test1);\n"
    "    if (!pass) discard;\n"
    "    fragColor = col;\n"
    "}\n";

void pc_gfx_set_render_scale(float scale) {
    sRenderScale = std::clamp(scale, 0.25f, 4.0f);
    sRenderScaleSet = true;
}

float pc_gfx_get_render_scale(void) {
    return sRenderScale;
}

static float calculate_aspect_ratio(int mode, float windowAspect) {
    switch (mode) {
        case ASPECT_4_3:   return 4.0f / 3.0f;
        case ASPECT_16_10: return 16.0f / 10.0f;
        case ASPECT_16_9:  return 16.0f / 9.0f;
        case ASPECT_21_9:  return 21.0f / 9.0f;
        case ASPECT_AUTO:
        default:
            return windowAspect;
    }
}

void pc_gfx_set_aspect_ratio_mode(int mode) {
    sAspectRatioMode = std::clamp(mode, 0, (int)ASPECT_COUNT - 1);
}

int pc_gfx_get_aspect_ratio_mode(void) {
    return sAspectRatioMode;
}

float pc_gfx_get_current_aspect_ratio(void) {
    return sCurrentAspectRatio;
}

static void calculate_output_area(int drawableWidth, int drawableHeight,
                                  float renderedAspect, GLint& outX, GLint& outY,
                                  GLint& outWidth, GLint& outHeight) {
    const float windowAspect = float(drawableWidth) / float(drawableHeight);
    if (renderedAspect >= windowAspect) {
        outWidth = drawableWidth;
        outHeight = GLint(lroundf(float(drawableWidth) / renderedAspect));
        outX = 0;
        outY = (drawableHeight - outHeight) / 2;
    } else {
        outHeight = drawableHeight;
        outWidth = GLint(lroundf(float(drawableHeight) * renderedAspect));
        outX = (drawableWidth - outWidth) / 2;
        outY = 0;
    }
}

// Attribute indices are forced to fixed slots for every program so the one
// vertex array object stays valid no matter which specialised program is
// bound. Must be called before linking.

// Startup GL errors are latched and only surface at the first draw, which says
// nothing about where they came from. These checkpoints drain and name the
// stage that raised one, so a stray error is attributable instead of ambient.
static void gl_error_checkpoint(const char* stage) {
    // Read the environment once. Two of these sit in the per-draw path, so a
    // getenv per call meant well over a thousand environment scans per frame.
    static const bool enabled = std::getenv("PIKMIN_GL_CHECK") != nullptr;
    if (!enabled) return;
    for (;;) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) break;
        const char* name = "UNKNOWN";
        switch (error) {
            case GL_INVALID_ENUM: name = "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: name = "GL_INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: name = "GL_INVALID_OPERATION"; break;
            case GL_OUT_OF_MEMORY: name = "GL_OUT_OF_MEMORY"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION: name = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
        }
        printf("[PC GX check] %s raised %s\n", stage, name);
    }
}

static void bind_fixed_attrib_locations(GLuint program) {
    if (!glBindAttribLocation_ptr) return;
    glBindAttribLocation_ptr(program, kAttrPosIndex, "aPos");
    glBindAttribLocation_ptr(program, kAttrColorIndex, "aColor");
    glBindAttribLocation_ptr(program, kAttrNormalIndex, "aNormal");
    static const char* const kTexCoordNames[8] = { "aTexCoord0", "aTexCoord1", "aTexCoord2", "aTexCoord3",
                                                   "aTexCoord4", "aTexCoord5", "aTexCoord6", "aTexCoord7" };
    static_assert(kVertexTexCoordCount <= 8, "kTexCoordNames covers the texcoord attributes");
    for (int i = 0; i < kVertexTexCoordCount; ++i) {
        glBindAttribLocation_ptr(program, kAttrTexCoord0Index + i, kTexCoordNames[i]);
    }
    // Matrix-palette index (ATTR7).
    glBindAttribLocation_ptr(program, 7, "aMtxIdx");
}

// Resolves every uniform this backend can feed. A specialised program bakes
// most of them away; those come back as -1 and the matching glUniform call is
// ignored by the driver, which is what lets one upload path serve both the
// ubershader and the generated programs.

#ifdef __vita__
// Uniforms a program declares.
struct ProgramUniformSet {
    void build(GLuint program) {
        mCount = 0;
        GLint n = 0;
        if (glGetProgramiv_ptr) glGetProgramiv_ptr(program, GL_ACTIVE_UNIFORMS, &n);
        for (GLint i = 0; i < n && mCount < kMax; ++i) {
            GLint size = 0; GLenum type = 0; GLsizei len = 0;
            mNames[mCount][0] = '\0';
            glGetActiveUniform(program, (GLuint)i, (GLsizei)kNameLen, &len, &size, &type, mNames[mCount]);
            if (mNames[mCount][0]) ++mCount;
        }
    }

    bool has(const char* name) const {
        char flat[kNameLen];
        const char* bracket = strchr(name, '[');
        if (bracket) {
            size_t base = (size_t)(bracket - name);
            if (base < sizeof(flat) - 8) {
                memcpy(flat, name, base);
                snprintf(flat + base, sizeof(flat) - base, "_%d", atoi(bracket + 1));
                name = flat;
            }
        }
        for (int i = 0; i < mCount; ++i) {
            if (strcmp(mNames[i], name) == 0) return true;
        }
        return false;
    }

    static const int kMax = 256;
    static const int kNameLen = 80;
    char mNames[kMax][kNameLen];
    int  mCount = 0;
};
#endif

// Every uniform a program may carry, as GLSL-style name and ProgramLocations field.
template <class Q>
static void program_location_sequence(ProgramLocations& out, Q&& QLOC) {
    out.projMtx = QLOC("uProjMtx");
    out.posMtx = QLOC("uPosMtx");
    out.posPal = QLOC("uPosPal");
    out.nrmPal = QLOC("uNrmPal");
    out.fogParams = QLOC("uFogParams");
    out.fogColour = QLOC("uFogColour");
    out.materialColor = QLOC("uMaterialColor");
    out.matSrc = QLOC("uMatSrc");
    out.useMaterialRgb = QLOC("uUseMaterialRgb");
    out.useMaterialAlpha = QLOC("uUseMaterialAlpha");
    out.alphaComp0 = QLOC("uAlphaComp0");
    out.alphaComp1 = QLOC("uAlphaComp1");
    out.alphaOp = QLOC("uAlphaOp");
    out.alphaRef0 = QLOC("uAlphaRef0");
    out.alphaRef1 = QLOC("uAlphaRef1");
    out.numStages = QLOC("uNumStages");
    out.fastPath = QLOC("uFastPath");
    out.tevPrev = QLOC("uTevPrev");
    out.tevReg0 = QLOC("uTevReg0");
    out.tevReg1 = QLOC("uTevReg1");
    out.tevReg2 = QLOC("uTevReg2");
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "uKonst%d", i);
        out.konst[i] = QLOC(buf);
    }
    for (int i = 0; i < GX_MAXTEVSTAGE; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "uTevCSel[%d]", i);
        out.tevCSel[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uTevASel[%d]", i);
        out.tevASel[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uTevCOps[%d]", i);
        out.tevCOps[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uTevAOps[%d]", i);
        out.tevAOps[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uTevTexInfo[%d]", i);
        out.tevTexInfo[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uTevKonst[%d]", i);
        out.tevKonst[i] = QLOC(buf);
    }
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "uTevSwapTable[%d]", i);
        out.tevSwapTable[i] = QLOC(buf);
    }
    for (int i = 0; i < GX_MAXTEVSTAGE; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "uTevSwapSel[%d]", i);
        out.tevSwapSel[i] = QLOC(buf);
    }
    for (int i = 0; i < 8; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "uTex%d", i);
        // Query only. The sampler values are written at the end of this
        // function, once the program is actually bound: writing them here sent
        // the new program's locations to whichever program was still current,
        // which GL either rejected or, when that location happened to hold an
        // integer uniform, silently overwrote with a sampler index.
        out.tex[i] = QLOC(buf);
    }

    for (int i = 0; i < 8; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "uTcMode[%d]", i);
        out.tcMode[i] = QLOC(name);
        snprintf(name, sizeof(name), "uTcMtx[%d]", i);
        out.tcMtx[i] = QLOC(name);
    }
    out.nrmMtx = QLOC("uNrmMtx");
    out.numLights = QLOC("uNumLights");
    out.ambColor = QLOC("uAmbColor");
    out.chan0En = QLOC("uChan0En");
    out.chan1En = QLOC("uChan1En");
    out.chan0AttnFn = QLOC("uChan0AttnFn");
    out.chan1AttnFn = QLOC("uChan1AttnFn");
    out.numLights1 = QLOC("uNumLights1");
    out.ambColor1 = QLOC("uAmbColor1");
    out.materialColor1 = QLOC("uMaterialColor1");
    out.useMaterialRgb1 = QLOC("uUseMaterialRgb1");
    out.specHalf1 = QLOC("uSpecHalf1");
    out.specAttn1 = QLOC("uSpecAttn1");
    for (int i = 0; i < 16; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "uTevChan[%d]", i);
        out.tevChan[i] = QLOC(buf);
    }
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "uLightPos[%d]", i);
        out.lightPos[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightColor[%d]", i);
        out.lightColor[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightK[%d]", i);
        out.lightK[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightPos1[%d]", i);
        out.lightPos1[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightColor1[%d]", i);
        out.lightColor1[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightK1[%d]", i);
        out.lightK1[i] = QLOC(buf);
    }
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "uLightPos[%d]", i);
        out.lightPos[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightColor[%d]", i);
        out.lightColor[i] = QLOC(buf);
        snprintf(buf, sizeof(buf), "uLightK[%d]", i);
        out.lightK[i] = QLOC(buf);
    }
}

#ifdef __vita__
// Flat uniform name to ProgramLocations field.
struct VitaLocName { char flat[48]; char glsl[48]; uint16_t field; };
static std::vector<VitaLocName> sVitaLocNames;   // sorted by flat name
static void vita_flatten_uniform_name(const char* name, char* flat, size_t cap) {
    const char* bracket = strchr(name, '[');
    if (bracket && size_t(bracket - name) < cap - 8) {
        const size_t base = size_t(bracket - name);
        memcpy(flat, name, base);
        snprintf(flat + base, cap - base, "_%d", atoi(bracket + 1));
    } else {
        snprintf(flat, cap, "%s", name);
    }
}
static void vita_build_location_names() {
    if (!sVitaLocNames.empty()) return;
    std::vector<std::string> names;
    ProgramLocations probe;
    program_location_sequence(probe, [&](const char* n) -> GLint {
        names.emplace_back(n);
        return GLint(-(int(names.size()) - 1 + 100));
    });
    const GLint* f = reinterpret_cast<const GLint*>(&probe);
    const size_t nf = sizeof(ProgramLocations) / sizeof(GLint);
    for (size_t i = 0; i < nf; ++i) {
        const int k = -int(f[i]) - 100;
        if (k < 0 || size_t(k) >= names.size()) continue;
        VitaLocName e{};
        snprintf(e.glsl, sizeof(e.glsl), "%s", names[size_t(k)].c_str());
        vita_flatten_uniform_name(e.glsl, e.flat, sizeof(e.flat));
        e.field = uint16_t(i);
        sVitaLocNames.push_back(e);
    }
    std::sort(sVitaLocNames.begin(), sVitaLocNames.end(),
              [](const VitaLocName& a, const VitaLocName& b) { return strcmp(a.flat, b.flat) < 0; });
}
#endif

static void query_program_locations(GLuint program, ProgramLocations& out) {
    out = ProgramLocations();
    invalidate_uniform_cache();
#ifdef __vita__
    sVitaPalProvValid = false;
    vita_palprog_forget_all();
    ProgramUniformSet present;
    present.build(program);
    // Uniform images.
    if (program < GLuint(kVitaMaxPrograms) && vglPrepareProgramShadow(program)) {
        VitaUniformImage& im = sVitaImg[program];
        for (int st = 0; st < 2; ++st) {
            uint32_t sz = 0;
            void* sh = vglGetProgramShadow(program, st ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, &sz);
            if (sh && sz && !im.img[st]) {
                im.img[st] = static_cast<uint8_t*>(calloc(sz, 1));
                im.size[st] = im.img[st] ? sz : 0;
            }
        }
    }
    vita_build_location_names();
    GLint* fields = reinterpret_cast<GLint*>(&out);
    for (int u = 0; u < present.mCount; ++u) {
        const char* name = present.mNames[u];
        auto it = std::lower_bound(sVitaLocNames.begin(), sVitaLocNames.end(), name,
                                   [](const VitaLocName& e, const char* key) { return strcmp(e.flat, key) < 0; });
        for (; it != sVitaLocNames.end() && strcmp(it->flat, name) == 0; ++it) {
            fields[it->field] = vita_encode_slot(program, glGetUniformLocation_ptr(program, it->glsl));
        }
    }
    // Location table self-check, one program in 64.
    static unsigned sLocCheck = 0;
    if ((sLocCheck++ & 63u) == 0) {
        ProgramLocations ref;
        program_location_sequence(ref, [&](const char* n) -> GLint {
            const GLint raw = present.has(n) ? glGetUniformLocation_ptr(program, n) : -1;
            return vita_encode_slot(program, raw);
        });
        if (memcmp(&ref, &out, sizeof(out)) != 0) {
            static int reported = 0;
            if (reported++ < 4) printf("ERROR: uniform location table mismatch for program %u; using the full query\n", (unsigned)program);
            out = ref;
        }
    }
#else
    program_location_sequence(out, [&](const char* n) -> GLint { return glGetUniformLocation_ptr(program, n); });
#endif
    // Sampler bindings are program state, so the program has to be current
    // before they are written. Leaving it out sent every glUniform1i to
    // whichever program happened to be bound: GL rejected them, the samplers
    // kept their default of zero, and every multi-texture stage sampled unit 0.
    glUseProgram_ptr(program);
    invalidate_uniform_cache();
    for (int i = 0; i < 8; i++) {
        if (out.tex[i] >= 0) glUniform1i_ptr(out.tex[i], i);
    }
}

#ifdef __vita__
void pc_gfx_init_impl(void) {
#else
void pc_gfx_init(void) {
#endif
    load_gl_functions();

    gl_error_checkpoint("context creation");
    if (const char* value = std::getenv("PIKMIN_TEV_SPECIALIZE")) {
        sSpecialiseShaders = value[0] != '0';
    }
    if (const char* value = std::getenv("PIKMIN_TEV_MAX_STAGES")) {
        sSpecialiseMaxStages = std::clamp(atoi(value), 0, 16);
    }
    if (const char* value = std::getenv("PIKMIN_RENDER_SCALE")) {
        if (!sRenderScaleSet) {
            sRenderScale = std::clamp(strtof(value, nullptr), 0.25f, 4.0f);
        }
    }

    // Default identity matrices
    for (int i = 0; i < 16; i++) {
        sProjMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    for (int m = 0; m < 64; m++) {
        for (int i = 0; i < 16; i++) {
            sPosMatrix[m][i] = (i % 5 == 0) ? 1.0f : 0.0f;
            sTexMatrices[m][i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
        sTexMtxLoaded[m] = false;
        for (int c = 0; c < 8; c++) {
            sTexCoordGen[c].active = false;
            sTexCoordGen[c].type = 1;
            sTexCoordGen[c].src = 4;
            sTexCoordGen[c].mtxIdx = 0;
        }
    }
#ifdef __vita__
    memset(sVitaTexMtxIdent, 0, sizeof(sVitaTexMtxIdent));
#endif

    if (!glCreateShader_ptr) {
        printf("[PC Port Error] OpenGL extension loader failed!\n");
        return;
    }

#ifdef __vita__
    // Precompiled GXP programs.
    GLuint vs = vita_gx_vertex_shader();
    sSharedVertexShader = vs;
    vita_build_all_programs();
#else
    // Compile Shaders
    GLuint vs = glCreateShader_ptr(GL_VERTEX_SHADER);
    glShaderSource_ptr(vs, 1, &vShaderSrc, NULL);
    glCompileShader_ptr(vs);

    GLuint fs = glCreateShader_ptr(GL_FRAGMENT_SHADER);
    glShaderSource_ptr(fs, 1, &fShaderSrc, NULL);
    glCompileShader_ptr(fs);

    // Report shader compile errors
    auto checkShader = [&](GLuint shader, const char* name) {
        GLint status = 0;
        if (glGetShaderiv_ptr) glGetShaderiv_ptr(shader, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE) {
            char log[4096] = { 0 };
            if (glGetShaderInfoLog_ptr) glGetShaderInfoLog_ptr(shader, sizeof(log), NULL, log);
            printf("[PC Port Shader Error] %s failed to compile:\n%s\n", name, log);
            return false;
        }
        return true;
    };
    bool vsOk = checkShader(vs, "vertex shader");
    bool fsOk = checkShader(fs, "fragment shader");
    if (!vsOk || !fsOk) {
        printf("[PC Port Error] Shader compilation failed - rendering will be broken!\n");
    }

    sShaderProgram = glCreateProgram_ptr();
    glAttachShader_ptr(sShaderProgram, vs);
    glAttachShader_ptr(sShaderProgram, fs);
    bind_fixed_attrib_locations(sShaderProgram);
    glLinkProgram_ptr(sShaderProgram);

    {
        GLint status = 0;
        if (glGetProgramiv_ptr) glGetProgramiv_ptr(sShaderProgram, GL_LINK_STATUS, &status);
        if (status != GL_TRUE) {
            char log[4096] = { 0 };
            if (glGetProgramInfoLog_ptr) glGetProgramInfoLog_ptr(sShaderProgram, sizeof(log), NULL, log);
            printf("[PC Port Shader Error] Program link failed:\n%s\n", log);
        } else {
            printf("[PC Port] TEV evaluator shaders compiled and linked OK\n");
        }
    }
#endif

    gl_error_checkpoint("ubershader link");
#ifdef __vita__
    query_program_locations(sShaderProgram, sUberLocations);
    gl_error_checkpoint("ubershader uniform locations");
    sLocP = &sUberLocations;
#else
    query_program_locations(sShaderProgram, sLoc);
    gl_error_checkpoint("ubershader uniform locations");
    sUberLocations = sLoc;
#endif
    sCurrentProgram = sShaderProgram;
    // The vertex shader is identical for every specialised program, so it is
    // compiled once here and reused when linking them.
    sSharedVertexShader = vs;

    gl_error_checkpoint("shader setup");
    glGenBuffers_ptr(1, &sVBO);
    glBindBuffer_ptr(GL_ARRAY_BUFFER, sVBO);
#ifdef __vita__
    // VBO ring.
    sVboCapacity = size_t(kVitaVboSegs) * kVitaVboSeg;
#endif
    glBufferData_ptr(GL_ARRAY_BUFFER, sVboCapacity, nullptr, GL_STREAM_DRAW);

    if (glGenFramebuffers_ptr && glBindFramebuffer_ptr && glFramebufferTexture2D_ptr
        && glGenRenderbuffers_ptr && glBindRenderbuffer_ptr && glRenderbufferStorage_ptr
        && glFramebufferRenderbuffer_ptr && glCheckFramebufferStatus_ptr && glBlitFramebuffer_ptr) {
        glGenFramebuffers_ptr(1, &sNativeFramebuffer);
        glBindFramebuffer_ptr(GL_FRAMEBUFFER, sNativeFramebuffer);
        glGenTextures(1, &sNativeColorTexture);
        glBindTexture(GL_TEXTURE_2D, sNativeColorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sRenderWidth, sRenderHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D_ptr(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   sNativeColorTexture, 0);
        glGenRenderbuffers_ptr(1, &sNativeDepthStencil);
        glBindRenderbuffer_ptr(GL_RENDERBUFFER, sNativeDepthStencil);
        glRenderbufferStorage_ptr(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, sRenderWidth, sRenderHeight);
        glFramebufferRenderbuffer_ptr(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, sNativeDepthStencil);
        sNativeFramebufferReady = glCheckFramebufferStatus_ptr(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
#ifdef __vita__
        // Render straight to the display.
        sNativeFramebufferReady = false;
#endif
        glBindFramebuffer_ptr(GL_FRAMEBUFFER, sNativeFramebufferReady ? sNativeFramebuffer : 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        sBoundTextures[0] = 0;
    }
    glEnableVertexAttribArray_ptr(sAttrPos);
    glVertexAttribPointer_ptr(sAttrPos, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, x));
    if (sAttrNormal >= 0) {
        glEnableVertexAttribArray_ptr(sAttrNormal);
        glVertexAttribPointer_ptr(sAttrNormal, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, nx));
    }
    glEnableVertexAttribArray_ptr(sAttrColor);
    glVertexAttribPointer_ptr(sAttrColor, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, r));
    for (int i = 0; i < 8; ++i) {
        if (sAttrTexCoord[i] >= 0) {
            glEnableVertexAttribArray_ptr(sAttrTexCoord[i]);
            const size_t offset = offsetof(Vertex, tex) + size_t(i) * 2 * sizeof(float);
            glVertexAttribPointer_ptr(sAttrTexCoord[i], 2, GL_FLOAT, GL_FALSE,
                                      sizeof(Vertex), reinterpret_cast<void*>(offset));
        }
    }
#ifdef __vita__
    // ATTR7: matrix palette slot.
    glBindBuffer_ptr(GL_ARRAY_BUFFER, sVBO);
    glEnableVertexAttribArray_ptr(7);
    glVertexAttribPointer_ptr(7, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, mtxIdx));
    sVitaRingGpu = static_cast<const char*>(glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY));
    glUnmapBuffer(GL_ARRAY_BUFFER);
    // GXM fast path.
    rec_fast_mode(1);
    // Uniform sections on the render thread.
    vita_rtgx_init();
#endif

    gl_error_checkpoint("vertex array setup");
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    // GX defines clockwise screen-space triangles as front-facing, whereas
    // OpenGL defaults to counter-clockwise.  Matching GX here keeps culling
    // from rejecting every front face in 3D scenes and cinematics.
    glFrontFace(GL_CW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
#ifdef __vita__
void pc_gfx_init(void) {
    vita_rt_run_init_gfx();
    vita_rt_override_gl_pointers();
    {
        uint32_t off[8]; int sz[8];
        for (int i = 0; i < 8; ++i) { off[i] = uint32_t(kVitaAttrOff[i]); sz[i] = kVitaAttrSize[i]; }
        vita_rt_set_attr_layout(off, sz, int(sizeof(Vertex)));
    }
    vita_rt_begin_recording();
    // Translator thread from here on.
    for (int i = 0; i < 256; ++i) sVitaU8F[i] = float(i) / 255.0f;
    vita_gxq_start();
}
#endif

void pc_gfx_perf_scope_begin(const char* name) {
    sPerfCurrentScope = -1;
    if (!sPerfStatsEnabled || !name) return;
    for (int i = 0; i < 8; ++i) {
        if (sPerfScopes[i].name == name || (sPerfScopes[i].name && strcmp(sPerfScopes[i].name, name) == 0)) {
            sPerfCurrentScope = i;
            return;
        }
        if (!sPerfScopes[i].name) {
            sPerfScopes[i].name = name;
            sPerfCurrentScope = i;
            return;
        }
    }
}

void pc_gfx_perf_scope_end(void) {
    sPerfCurrentScope = -1;
}

static bool perf_gpu_queries_supported() {
#ifdef GL_TIME_ELAPSED
    return glGenQueries_ptr && glBeginQuery_ptr && glEndQuery_ptr
        && glGetQueryObjectiv_ptr && glGetQueryObjectui64v_ptr;
#else
    return false;
#endif
}

static void perf_gpu_queries_init() {
    if (sPerfGpuQueriesReady || !perf_gpu_queries_supported()) return;
    GLuint ids[PC_GPU_QUERY_RING_SIZE * 2] = {};
    glGenQueries_ptr(GLsizei(PC_GPU_QUERY_RING_SIZE * 2), ids);
    for (unsigned i = 0; i < PC_GPU_QUERY_RING_SIZE; ++i) {
        sPerfGpuQueries[i].scene = ids[i * 2];
        sPerfGpuQueries[i].blit = ids[i * 2 + 1];
    }
    sPerfGpuQueriesReady = ids[0] != 0;
    if (sPerfGpuQueriesReady)
        fprintf(stderr, "[PERF GPU] asynchronous timer queries enabled\n");
}

static void perf_gpu_queries_poll() {
#ifdef GL_TIME_ELAPSED
    if (!sPerfGpuQueriesReady) return;
    // Never wait for the GPU. Only consume query pairs whose two results are
    // already available, leaving busy slots pending in the ring.
    for (PerfGpuQuery& query : sPerfGpuQueries) {
        if (!query.pending) continue;
        GLint sceneAvailable = GL_FALSE;
        GLint blitAvailable = GL_FALSE;
        glGetQueryObjectiv_ptr(query.scene, GL_QUERY_RESULT_AVAILABLE, &sceneAvailable);
        glGetQueryObjectiv_ptr(query.blit, GL_QUERY_RESULT_AVAILABLE, &blitAvailable);
        if (sceneAvailable != GL_TRUE || blitAvailable != GL_TRUE) continue;
        GLuint64 sceneNs = 0;
        GLuint64 blitNs = 0;
        glGetQueryObjectui64v_ptr(query.scene, GL_QUERY_RESULT, &sceneNs);
        glGetQueryObjectui64v_ptr(query.blit, GL_QUERY_RESULT, &blitNs);
        sPerfGpuSceneNs += uint64_t(sceneNs);
        sPerfGpuBlitNs += uint64_t(blitNs);
        ++sPerfGpuSamples;
        query.pending = false;
    }
#endif
}

static void perf_gpu_scene_begin() {
#ifdef GL_TIME_ELAPSED
    if (!sPerfStatsEnabled) return;
    perf_gpu_queries_init();
    perf_gpu_queries_poll();
    if (!sPerfGpuQueriesReady || sPerfGpuSceneActive) return;
    PerfGpuQuery& query = sPerfGpuQueries[sPerfGpuQueryWrite];
    if (query.pending) return;
    glBeginQuery_ptr(GL_TIME_ELAPSED, query.scene);
    sPerfGpuSceneActive = true;
#endif
}

void pc_gfx_begin_frame(void) {
#ifndef __vita__
    // Desktop frame report (PIKMIN_PERF_STATS).
    using Clock = std::chrono::steady_clock;
    static const bool perfStats = std::getenv("PIKMIN_PERF_STATS") != nullptr;
    sPerfStatsEnabled = perfStats;
    if (sPerfStatsEnabled) {
        perf_gpu_queries_init();
        perf_gpu_queries_poll();
    }
    static Clock::time_point lastFrame;
    static double elapsedMs = 0.0;
    static unsigned frames = 0;
    const Clock::time_point now = Clock::now();
    if (lastFrame.time_since_epoch().count() != 0) {
        elapsedMs += std::chrono::duration<double, std::milli>(now - lastFrame).count();
        ++frames;
    }
    lastFrame = now;
    if (perfStats && frames >= 120) {
        fprintf(stderr,
                "[PERF] %.1f fps %.2f ms | %.0f draws (%.0f source, %.0f fast) %.0f verts %.0f DL (%.2f MiB) %.2f tex uploads/frame\n",
                frames * 1000.0 / elapsedMs, elapsedMs / frames,
                double(sPerfDraws) / frames, double(sPerfSourcePrimitives) / frames,
                double(sPerfFastDraws) / frames,
                double(sPerfVertices) / frames,
                double(sPerfDisplayLists) / frames,
                double(sPerfDisplayListBytes) / frames / (1024.0 * 1024.0),
                double(sPerfTextureUploads) / frames);
        fprintf(stderr, "[PERF TEV] stages");
        for (int i = 0; i <= GX_MAXTEVSTAGE; ++i) {
            if (sPerfTevStageDraws[i])
                fprintf(stderr, " %d=%.0f", i, double(sPerfTevStageDraws[i]) / frames);
        }
        fprintf(stderr, "\n");
        if (sPerfGpuSamples != 0) {
            fprintf(stderr, "[PERF GPU] scene %.2f ms blit %.2f ms (%llu async samples)\n",
                    double(sPerfGpuSceneNs) / double(sPerfGpuSamples) / 1000000.0,
                    double(sPerfGpuBlitNs) / double(sPerfGpuSamples) / 1000000.0,
                    static_cast<unsigned long long>(sPerfGpuSamples));
        } else if (perf_gpu_queries_supported()) {
            fprintf(stderr, "[PERF GPU] results pending; CPU was not stalled\n");
        } else {
            fprintf(stderr, "[PERF GPU] timer queries unavailable on this OpenGL driver\n");
        }
        for (const PerfScope& scope : sPerfScopes) {
            if (scope.name && scope.draws) {
                fprintf(stderr, "[PERF ACTOR] %s %.0f draws %.0f verts\n", scope.name,
                        double(scope.draws) / frames, double(scope.vertices) / frames);
            }
        }
        fprintf(stderr, "[PERF FAST]");
        for (int i = 1; i < 4; ++i) {
            if (sPerfFastPathDraws[i]) {
                fprintf(stderr, " path%d=%.0f draws/%.2f verts", i,
                        double(sPerfFastPathDraws[i]) / frames,
                        double(sPerfFastPathVertices[i]) / double(sPerfFastPathDraws[i]));
            }
        }
        fprintf(stderr, " | prim tri=%.0f strip=%.0f fan=%.0f lines=%.0f line-strip=%.0f points=%.0f quads=%.0f\n",
                double(sPerfPrimitiveDraws[0]) / frames, double(sPerfPrimitiveDraws[1]) / frames,
                double(sPerfPrimitiveDraws[2]) / frames, double(sPerfPrimitiveDraws[3]) / frames,
                double(sPerfPrimitiveDraws[4]) / frames, double(sPerfPrimitiveDraws[5]) / frames,
                double(sPerfPrimitiveDraws[6]) / frames);
        for (int rank = 0; rank < 4; ++rank) {
            int best = -1;
            for (int i = 0; i < 128; ++i) {
                if (sPerfTevPatterns[i].count && (best < 0 || sPerfTevPatterns[i].count > sPerfTevPatterns[best].count))
                    best = i;
            }
            if (best < 0) break;
            const uint64_t key = sPerfTevPatterns[best].key;
            fprintf(stderr,
                    "[PERF TEV1] %.0f draws C=%u,%u,%u,%u A=%u,%u,%u,%u tex=%d tc=%d ras=%d op=%u/%u flags=0x%llx\n",
                    double(sPerfTevPatterns[best].count) / frames,
                    unsigned((key >> 0) & 15), unsigned((key >> 4) & 15),
                    unsigned((key >> 8) & 15), unsigned((key >> 12) & 15),
                    unsigned((key >> 16) & 7), unsigned((key >> 19) & 7),
                    unsigned((key >> 22) & 7), unsigned((key >> 25) & 7),
                    int((key >> 28) & 15) - 1, int((key >> 32) & 15) - 1,
                    int((key >> 36) & 3) - 1, unsigned((key >> 38) & 15),
                    unsigned((key >> 42) & 15),
                    static_cast<unsigned long long>(key >> 46));
            sPerfTevPatterns[best].count = 0;
        }
        for (int rank = 0; rank < 4; ++rank) {
            int best = -1;
            for (int i = 0; i < 128; ++i) {
                if (sPerfTevMultiPatterns[i].count
                    && (best < 0 || sPerfTevMultiPatterns[i].count > sPerfTevMultiPatterns[best].count))
                    best = i;
            }
            if (best < 0) break;
            const PerfTevMultiPattern& pattern = sPerfTevMultiPatterns[best];
            const uint64_t key = pattern.key;
            fprintf(stderr,
                    "[PERF TEVN] %.0f draws stages=%u final C=%u,%u,%u,%u A=%u,%u,%u,%u tex=%d tc=%d ras=%d op=%u/%u flags=0x%llx\n",
                    double(pattern.count) / frames, unsigned(pattern.stages),
                    unsigned((key >> 0) & 15), unsigned((key >> 4) & 15),
                    unsigned((key >> 8) & 15), unsigned((key >> 12) & 15),
                    unsigned((key >> 16) & 7), unsigned((key >> 19) & 7),
                    unsigned((key >> 22) & 7), unsigned((key >> 25) & 7),
                    int((key >> 28) & 15) - 1, int((key >> 32) & 15) - 1,
                    int((key >> 36) & 3) - 1, unsigned((key >> 38) & 15),
                    unsigned((key >> 42) & 15),
                    static_cast<unsigned long long>(key >> 46));
            sPerfTevMultiPatterns[best].count = 0;
        }
        frames = 0; elapsedMs = 0.0;
        sPerfDraws = sPerfFastDraws = sPerfVertices = sPerfDisplayLists = 0;
        sPerfSourcePrimitives = 0;
        sPerfDisplayListBytes = sPerfTextureUploads = 0;
        sPerfGpuSceneNs = sPerfGpuBlitNs = sPerfGpuSamples = 0;
        memset(sPerfTevStageDraws, 0, sizeof(sPerfTevStageDraws));
        memset(sPerfTevPatterns, 0, sizeof(sPerfTevPatterns));
        memset(sPerfTevMultiPatterns, 0, sizeof(sPerfTevMultiPatterns));
        memset(sPerfFastPathDraws, 0, sizeof(sPerfFastPathDraws));
        memset(sPerfFastPathVertices, 0, sizeof(sPerfFastPathVertices));
        memset(sPerfPrimitiveDraws, 0, sizeof(sPerfPrimitiveDraws));
        for (PerfScope& scope : sPerfScopes) {
            scope.draws = 0;
            scope.vertices = 0;
        }
    }
#endif // !__vita__

#ifdef __vita__
    sDrawableWidth = 960;
    sDrawableHeight = 544;
#else
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    if (window) SDL_GL_GetDrawableSize(window, &sDrawableWidth, &sDrawableHeight);
#endif
    // Frame aspect.
    if (sDrawableWidth > 0 && sDrawableHeight > 0) {
        const float windowAspect = float(sDrawableWidth) / float(sDrawableHeight);
        sCurrentAspectRatio = calculate_aspect_ratio(sAspectRatioMode, windowAspect);
    }
    if (sNativeFramebufferReady && sDrawableWidth > 0 && sDrawableHeight > 0) {
        
        // Scale the exact output viewport instead of first quantising the
        // aspect ratio through a 480-line base. The old calculation turned a
        // native 1920x1080 target into 1919x1080 and forced needless filtering.
        GLint outX, outY, outWidth, outHeight;
        calculate_output_area(sDrawableWidth, sDrawableHeight, sCurrentAspectRatio,
                              outX, outY, outWidth, outHeight);
        const int wantedWidth = std::max(160, int(lroundf(float(outWidth) * sRenderScale)));
        const int wantedHeight = std::max(120, int(lroundf(float(outHeight) * sRenderScale)));
        if (wantedWidth != sRenderWidth || wantedHeight != sRenderHeight) {
            sRenderWidth = wantedWidth;
            sRenderHeight = wantedHeight;
            glBindTexture(GL_TEXTURE_2D, sNativeColorTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sRenderWidth, sRenderHeight,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindRenderbuffer_ptr(GL_RENDERBUFFER, sNativeDepthStencil);
            glRenderbufferStorage_ptr(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                      sRenderWidth, sRenderHeight);
            glBindTexture(GL_TEXTURE_2D, 0);
            sBoundTextures[0] = 0;
            printf("[PC Port] Internal render resolution: %dx%d (aspect %.3f)\n", sRenderWidth, sRenderHeight, sCurrentAspectRatio);
        }
        glBindFramebuffer_ptr(GL_FRAMEBUFFER, sNativeFramebuffer);
    }

    if (!sVBO || !glBufferData_ptr) return;
#ifndef __vita__
    glBindBuffer_ptr(GL_ARRAY_BUFFER, sVBO);
    // Orphan once per frame. The driver can finish consuming the previous
    // storage asynchronously while the CPU streams the next frame contiguously.
    glBufferData_ptr(GL_ARRAY_BUFFER, sVboCapacity, nullptr, GL_STREAM_DRAW);
    sVboWriteOffset = 0;
#else
#endif
    perf_gpu_scene_begin();
}

// GXCopyTex
void pc_gfx_copy_tex(void* dest, GXBool clear, u16 srcX, u16 srcY, u16 srcW, u16 srcH, u16 dstW, u16 dstH, GXTexFmt fmt, GXBool mipmap) {
#ifdef __vita__
    if (!dest || !srcW || !srcH || !dstW || !dstH) return;
    pc_gfx_note_gl_state_change();
    PcCopyTarget& t = sCopyTargets[dest];
    if (!t.tex) {
        for (size_t i = 0; i < sCopyTargetPool.size(); ++i) {
            if (sCopyTargetPool[i].w == dstW && sCopyTargetPool[i].h == dstH) {
                t.tex = sCopyTargetPool[i].tex;
                sCopyTargetPool.erase(sCopyTargetPool.begin() + std::ptrdiff_t(i));
                break;
            }
        }
        if (!t.tex) glGenTextures(1, &t.tex);
        sEfbTextureIds[t.tex] = true;
        // Texture objects at this address switch to the copy's texture.
        for (auto& kv : sTextureSignatures) {
            if (kv.second.image == dest) sTextureCache[kv.first] = t.tex;
        }
        for (auto it = sTexBySignature.begin(); it != sTexBySignature.end();) {
            if (it->second.sig.image == dest) it = sTexBySignature.erase(it); else ++it;
        }
        ++sTexCacheGen;
    }
    t.w = dstW; t.h = dstH;
    GLint x, y; GLsizei w, h;
    map_gx_rect(float(srcX), float(srcY), float(srcW), float(srcH), x, y, w, h);
    const float tw = sEfbOffscreen ? float(sEfbOffW) : float(sNativeFramebufferReady ? sRenderWidth : sDrawableWidth);
    const float th = sEfbOffscreen ? float(sEfbOffH) : float(sNativeFramebufferReady ? sRenderHeight : sDrawableHeight);
    const float top = th - float(y + h);   // GL rects are bottom-up, texture rows top-down
    int program = RT_EFB_PROG_RGBA, depth = 0;
    switch (fmt) {
        case GX_TF_Z8: case GX_TF_Z16: case GX_TF_Z24X8: case GX_CTF_Z4: case GX_CTF_Z8M: case GX_CTF_Z8L: case GX_CTF_Z16L:
            program = RT_EFB_PROG_Z8; depth = 1; break;
        case GX_TF_I4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_IA8:
            program = RT_EFB_PROG_I8; break;
        case GX_TF_RGB565:
            program = RT_EFB_PROG_565; break;
        default: break;
    }
    rec_efb_copy(t.tex, dstW, dstH, program, depth, float(x) / tw, top / th, float(x + w) / tw, (top + float(h)) / th, mipmap ? 1 : 0);
    vita_gxm_shadow_invalidate();
    if (clear) {
        // GXCopyTex clear.
        const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(sCopyClearColor[0], sCopyClearColor[1], sCopyClearColor[2], sCopyClearColor[3]);
        glClearDepth(sCopyClearDepth);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST);
    }
#else
    (void)dest; (void)clear; (void)srcX; (void)srcY; (void)srcW; (void)srcH; (void)dstW; (void)dstH; (void)fmt; (void)mipmap;
#endif
}

// Full-screen composite from copy targets (blur, knockout grey-out).
void pc_gfx_efb_composite(int kind, const void* dest0, const void* dest1, float p0, float p1) {
#ifdef __vita__
    auto t0 = sCopyTargets.find(dest0);
    if (t0 == sCopyTargets.end()) return;   // not copied yet
    GLuint tex1 = 0;
    if (dest1) {
        auto t1 = sCopyTargets.find(dest1);
        if (t1 == sCopyTargets.end()) return;
        tex1 = t1->second.tex;
    }
    pc_gfx_note_gl_state_change();
    rec_efb_composite(kind, t0->second.tex, tex1, p0, p1);
    vita_gxm_shadow_invalidate();
#else
    (void)kind; (void)dest0; (void)dest1; (void)p0; (void)p1;
#endif
}

// Level memory freed.
#ifdef __vita__
// Is p in one of n sorted, disjoint [start, end) pairs?
static bool pc_addr_in_ranges(const uintptr_t* r, int n, uintptr_t p) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) >> 1;
        if (p < r[2 * mid]) hi = mid - 1;
        else if (p >= r[2 * mid + 1]) lo = mid + 1;
        else return true;
    }
    return false;
}

// Release textures in freed memory ranges.
static void pc_tex_release_freed(const uintptr_t* ranges, int n) {
    auto gone = [&](const void* p) { return p && pc_addr_in_ranges(ranges, n, uintptr_t(p)); };
    size_t keysDropped = 0;
    for (auto it = sTextureSignatures.begin(); it != sTextureSignatures.end();) {
        if (gone(reinterpret_cast<const void*>(it->first)) || gone(it->second.image)) {
            sTextureCache.erase(it->first);
            it = sTextureSignatures.erase(it);
            ++keysDropped;
        } else ++it;
    }
    for (auto it = sCiTextures.begin(); it != sCiTextures.end();) {
        if (gone(reinterpret_cast<const void*>(it->first)) || gone(it->second.image)) {
            sTextureCache.erase(it->first);
            it = sCiTextures.erase(it);
            ++keysDropped;
        } else ++it;
    }
    for (auto it = sTextureCache.begin(); it != sTextureCache.end();) {
        if (gone(reinterpret_cast<const void*>(it->first))) { it = sTextureCache.erase(it); ++keysDropped; }
        else ++it;
    }
    for (auto it = sTlutObjects.begin(); it != sTlutObjects.end();) {
        if (gone(reinterpret_cast<const void*>(it->first))) it = sTlutObjects.erase(it); else ++it;
    }
    for (auto it = sTexBySignature.begin(); it != sTexBySignature.end();) {
        if (gone(it->second.sig.image)) it = sTexBySignature.erase(it); else ++it;
    }

    std::unordered_set<GLuint> live;
    live.reserve(sTextureCache.size() + sTexBySignature.size());
    for (const auto& kv : sTextureCache) live.insert(kv.second);
    for (const auto& kv : sTexBySignature) live.insert(kv.second.tex);
    std::vector<GLuint> dead;
    for (const auto& kv : sTexLastUsed) {
        if (!live.count(kv.first) && !sEfbTextureIds.count(kv.first)) dead.push_back(kv.first);
    }
    for (GLuint tex : dead) {
        for (int m = 0; m < 8; ++m) if (sBoundTextures[m] == tex) sBoundTextures[m] = 0;
        for (int m = 0; m < GX_MAX_TEXMAP; ++m) {
            if (sActiveGLTextures[m] == tex) {
                sActiveGLTextures[m] = 0;
                sHasActiveTextures[m] = false;
                sActiveTexGrid[m] = nullptr;
            }
        }
        glDeleteTextures(1, &tex);
        ++sTexEvictions;
        sTexShape.erase(tex);
        pc_tex_mip_forget(tex);
        sTexMinAlpha.erase(tex);
        sTexAlphaGrid.erase(tex);
        sTexLastUsed.erase(tex);
    }
    ++sActiveTexAlphaGen;
    // Rebind unit 0 to its mirrored texture.
    if (!dead.empty() && vita_rt_recording()) glBindTexture(GL_TEXTURE_2D, sBoundTextures[0]);
    printf("[LEVEL] freed %d ranges: %u texture keys, %u GL textures released (%u kept, VRAM free %u KB)\n",
           n, unsigned(keysDropped), unsigned(dead.size()), unsigned(sTexLastUsed.size()), vita_rt_vram_free_kb());
}
#endif

void pc_gfx_level_flush(const void* rangesIn, int count) {
    const uintptr_t* ranges = static_cast<const uintptr_t*>(rangesIn);
#ifdef __vita__
    pc_gfx_note_gl_state_change();
    vita_dl_flush_all();
    for (auto& kv : sCopyTargets) {
        for (auto it = sTextureCache.begin(); it != sTextureCache.end();) {
            if (it->second == kv.second.tex) { sTextureSignatures.erase(it->first); it = sTextureCache.erase(it); }
            else ++it;
        }
        sCopyTargetPool.push_back(kv.second);
    }
    sCopyTargets.clear();
    if (ranges && count > 0) pc_tex_release_freed(ranges, count);
    ++sTexCacheGen;
#endif
    std::free(const_cast<uintptr_t*>(ranges));
}

// Offscreen EFB target on or off.
void pc_gfx_efb_target(int on, int w, int h) {
#ifdef __vita__
    // Display GX viewport and scissor.
    static float savedVp[4] = { 0.0f, 0.0f, 640.0f, 480.0f };
    static u32 savedSc[4] = { 0, 0, 640, 480 };
    pc_gfx_note_gl_state_change();
    if (on && w > 0 && h > 0) {
        if (!sEfbOffTex) { glGenTextures(1, &sEfbOffTex); sEfbTextureIds[sEfbOffTex] = true; }
        if (!sEfbOffscreen) {
            memcpy(savedVp, sGxViewport, sizeof(savedVp));
            memcpy(savedSc, sGxScissor, sizeof(savedSc));
        }
        sEfbOffscreen = true; sEfbOffW = w; sEfbOffH = h;
        rec_efb_target(sEfbOffTex, w, h);
        vita_gxm_shadow_invalidate();
    } else {
        if (!sEfbOffscreen) return;
        sEfbOffscreen = false;
        rec_efb_target(0, 0, 0);
        vita_gxm_shadow_invalidate();
        memcpy(sGxViewport, savedVp, sizeof(savedVp));
        memcpy(sGxScissor, savedSc, sizeof(savedSc));
    }
    ++sTargetEpoch;
    pc_gfx_set_viewport(sGxViewport[0], sGxViewport[1], sGxViewport[2], sGxViewport[3], 0.0f, 1.0f);
    pc_gfx_set_scissor(sGxScissor[0], sGxScissor[1], sGxScissor[2], sGxScissor[3]);
#else
    (void)on; (void)w; (void)h;
#endif
}

// GXSetCopyFilter vertical filter. Rows y-1, y, y+1 get (f0+f1), (f2+f3+f4), (f5+f6) of 64.
void pc_gfx_set_copy_filter(int vertFilter, const u8 vfilter[7]) {
    if (!vertFilter || !vfilter) { sVFilterEdge = 0; sVFilterCentre = 64; return; }
    sVFilterEdge = int(vfilter[0]) + int(vfilter[1]);
    sVFilterCentre = int(vfilter[2]) + int(vfilter[3]) + int(vfilter[4]);
}

extern "C" int pc_vi_get_black(void);   // vi_stubs.cpp
void pc_gfx_present(void) {
    pc_gfx_flush_batch();
    // VISetBlack frame count.
    static int sViBlackFrames = 0;
    sViBlackFrames = pc_vi_get_black() ? sViBlackFrames + 1 : 0;
    if (sViBlackFrames > 0 && sViBlackFrames <= 30) {
        const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST);
    }
    evict_idle_textures();
#ifdef __vita__
    // Display copy vertical filter.
    if (pc_settings_get_display_filter() && sVFilterEdge > 0 && !sEfbOffscreen) {
        const float rowStep = 1.0f / 480.0f;   // one EFB line
        rec_efb_vfilter(rowStep, float(sVFilterEdge) / 64.0f, float(sVFilterCentre) / 64.0f);
    }
    vita_rtgx_present();
    vita_rt_present();     // swap on the render thread
    vita_rotate_vbo_segment();   // fence the VBO ring
    vita_dl_end_frame();
    sVitaFrameFirstDraw = true;
#endif
    ++sVitaFrameNo;
    if (sVitaDumpActive) printf("[VDUMP] end of frame %u: %u draws\n", sVitaFrameNo - 1, sVitaDumpDraw);
    sVitaDumpActive = dump_frame_active(sVitaFrameNo);
    if (sVitaDumpActive) { sVitaDumpDraw = 0; printf("[VDUMP] begin frame %u\n", sVitaFrameNo); }
#ifdef GL_TIME_ELAPSED
    if (sPerfGpuSceneActive) {
        glEndQuery_ptr(GL_TIME_ELAPSED);
        sPerfGpuSceneActive = false;
    }
#endif
    if (!sNativeFramebufferReady || !glBindFramebuffer_ptr || !glBlitFramebuffer_ptr) return;
    if (sDrawableWidth <= 0 || sDrawableHeight <= 0) return;
#ifdef GL_TIME_ELAPSED
    PerfGpuQuery* gpuQuery = nullptr;
    if (sPerfStatsEnabled && sPerfGpuQueriesReady) {
        PerfGpuQuery& candidate = sPerfGpuQueries[sPerfGpuQueryWrite];
        if (!candidate.pending) {
            gpuQuery = &candidate;
            glBeginQuery_ptr(GL_TIME_ELAPSED, gpuQuery->blit);
        }
    }
#endif
    glBindFramebuffer_ptr(GL_READ_FRAMEBUFFER, sNativeFramebuffer);
    glBindFramebuffer_ptr(GL_DRAW_FRAMEBUFFER, 0);
    // Game renders to the target aspect ratio. Display it centered in the window.
    float windowAspect = float(sDrawableWidth) / float(sDrawableHeight);
    sCurrentAspectRatio = calculate_aspect_ratio(sAspectRatioMode, windowAspect);

    GLint outX, outY, outWidth, outHeight;
    calculate_output_area(sDrawableWidth, sDrawableHeight, sCurrentAspectRatio,
                          outX, outY, outWidth, outHeight);

    glDisable(GL_SCISSOR_TEST);
    // Clearing a full 4K backbuffer immediately before covering every pixel
    // with the blit wastes bandwidth. Clear only when letter/pillarbox bars
    // are actually visible.
    if (outX != 0 || outY != 0 || outWidth != sDrawableWidth || outHeight != sDrawableHeight) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    
    glBlitFramebuffer_ptr(0, 0, sRenderWidth, sRenderHeight, outX, outY, outX + outWidth, outY + outHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
#ifdef GL_TIME_ELAPSED
    if (gpuQuery) {
        glEndQuery_ptr(GL_TIME_ELAPSED);
        gpuQuery->pending = true;
        sPerfGpuQueryWrite = (sPerfGpuQueryWrite + 1) % PC_GPU_QUERY_RING_SIZE;
    }
#endif
    glBindFramebuffer_ptr(GL_FRAMEBUFFER, sNativeFramebuffer);
    glEnable(GL_SCISSOR_TEST);
}

#ifdef __vita__
// Projection interning.
struct VitaProjIntern { float m[16]; uint32_t id, lastUse; bool valid; };
static VitaProjIntern sVitaProjTab[8];
static uint32_t sVitaProjNextId = 0, sVitaProjUseClock = 0;
static uint32_t vita_proj_intern(const float* m) {
    if (!sVitaProjTab[0].valid && sVitaProjNextId == 0) {
        for (int i = 0; i < 16; ++i) sVitaProjTab[0].m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        sVitaProjTab[0].id = 0; sVitaProjTab[0].valid = true;
    }
    ++sVitaProjUseClock;
    int victim = 0;
    for (int i = 0; i < 8; ++i) {
        VitaProjIntern& e = sVitaProjTab[i];
        if (e.valid && memcmp(e.m, m, sizeof(e.m)) == 0) { e.lastUse = sVitaProjUseClock; return e.id; }
        if (!e.valid) victim = i;
        else if (sVitaProjTab[victim].valid && e.lastUse < sVitaProjTab[victim].lastUse) victim = i;
    }
    VitaProjIntern& e = sVitaProjTab[victim];
    memcpy(e.m, m, sizeof(e.m));
    e.id = ++sVitaProjNextId;
    e.lastUse = sVitaProjUseClock;
    e.valid = true;
    return e.id;
}
#endif

void pc_gfx_set_projection(const Mtx44 mtx, GXProjectionType type) {
    (void)type;
    if (mtx) {
        // Dolphin matrices are indexed [row][column], while OpenGL consumes a
        // column-major float array when transpose is GL_FALSE.
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                sProjMatrix[column * 4 + row] = mtx[row][column];
            }
        }
#ifdef __vita__
        sProjMtxGen = vita_proj_intern(sProjMatrix);
#else
        ++sProjMtxGen;
#endif
    }
}

void pc_gfx_set_current_mtx(u32 id) {
    if (id < 64) sCurrentPosMtxId = id;
}

void pc_gfx_set_viewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ) {
    (void)nearZ; (void)farZ;
    sGxViewport[0] = xOrig; sGxViewport[1] = yOrig; sGxViewport[2] = wd; sGxViewport[3] = ht;
    GLint x, y; GLsizei width, height;
    map_gx_rect(xOrig, yOrig, wd, ht, x, y, width, height);
    static GLint lastX = 0, lastY = 0;
    static GLsizei lastWidth = 0, lastHeight = 0;
    static unsigned lastEpoch = 0;
    if (lastEpoch == sTargetEpoch && x == lastX && y == lastY && width == lastWidth && height == lastHeight) return;
    lastEpoch = sTargetEpoch;
    lastX = x; lastY = y; lastWidth = width; lastHeight = height;
    pc_gfx_note_gl_state_change();
    glViewport(x, y, width, height);
}

// Scissor to the whole panel.
void pc_gfx_set_scissor_panel(void) {
    const bool was = sSectionClip43;
    sSectionClip43 = false;
    pc_gfx_set_scissor(0, 0, 640, 480);
    sSectionClip43 = was;
}
// Scissor to the interface box.
void pc_gfx_set_scissor_ui_box(void) {
    if (sSectionClip43) {
        pc_gfx_set_scissor(0, 0, 640, 480);
    } else {
        int bx, bw;
        pc_gfx_ui_box(&bx, &bw);
        pc_gfx_set_scissor(u32(bx), 0, u32(bw), 480);
    }
}
void pc_gfx_set_scissor(u32 xOrig, u32 yOrig, u32 wd, u32 ht) {
    sGxScissor[0] = xOrig; sGxScissor[1] = yOrig; sGxScissor[2] = wd; sGxScissor[3] = ht;
    GLint x, y; GLsizei width, height;
    float gx = (float)xOrig, gw = (float)wd;
    if (sSectionClip43) {
        // 4:3 menu section: rect inside the interface box.
        int bx, bw;
        pc_gfx_ui_box(&bx, &bw);
        const float sx = float(bw) / 640.0f;
        gx = float(bx) + gx * sx;
        gw = gw * sx;
    }
    map_gx_rect(gx, (float)yOrig, gw, (float)ht, x, y, width, height);
    static GLint lastX = -1, lastY = -1;
    static GLsizei lastWidth = -1, lastHeight = -1;
    static unsigned lastEpoch = 0;
    if (lastEpoch == sTargetEpoch && x == lastX && y == lastY && width == lastWidth && height == lastHeight) return;
    lastEpoch = sTargetEpoch;
    lastX = x; lastY = y; lastWidth = width; lastHeight = height;
    pc_gfx_note_gl_state_change();
    glScissor(x, y, width, height);
    if (!glIsEnabled(GL_SCISSOR_TEST)) glEnable(GL_SCISSOR_TEST);
}

// 4:3 menu section clip.
void pc_gfx_set_section_clip_43(int on) { sSectionClip43 = on != 0; }
static bool sMovieLinearPlanes = false;   // attract movie packs its planes linear
void pc_gfx_set_movie_planes_linear(int on) { sMovieLinearPlanes = on != 0; }

// Line width in panel pixels.
static float sLineWidthPx = 1.0f;
void pc_gfx_set_line_width(u8 width) {
    const float targetH = sNativeFramebufferReady ? float(sRenderHeight) : float(sDrawableHeight);
    float px = float(width) / 6.0f * (targetH / 480.0f);
    if (px < 1.0f) px = 1.0f;
    if (px == sLineWidthPx) return;
    sLineWidthPx = px;
    pc_gfx_note_gl_state_change();
    glLineWidth(px);
}
int pc_gfx_get_section_clip_43(void) { return sSectionClip43 ? 1 : 0; }

void pc_gfx_ui_box(int* x0, int* w) {
    const float sx = pc_gfx_ui_projection_scale_x();
    if (x0) *x0 = int(320.0f * (1.0f - sx) + 0.5f);
    if (w)  *w  = int(640.0f * sx + 0.5f);
}

float pc_gfx_ui_full_width_extent(void) {
    const float ui = 4.0f / 3.0f;
    if (sCurrentAspectRatio <= ui) return 0.0f;
    return (sCurrentAspectRatio / ui - 1.0f) * 640.0f * 0.5f;
}

float pc_gfx_ui_projection_scale_x(void) {
    const float ui = 4.0f / 3.0f;
    return sCurrentAspectRatio > ui ? ui / sCurrentAspectRatio : 1.0f;
}

void pc_gfx_load_pos_mtx(const Mtx mtx, u32 id) {
    if (id < 64 && mtx) {
        float* dst = sPosMatrix[id];
        dst[0] = mtx[0][0]; dst[1] = mtx[1][0]; dst[2] = mtx[2][0]; dst[3] = 0.0f;
        dst[4] = mtx[0][1]; dst[5] = mtx[1][1]; dst[6] = mtx[2][1]; dst[7] = 0.0f;
        dst[8] = mtx[0][2]; dst[9] = mtx[1][2]; dst[10] = mtx[2][2]; dst[11] = 0.0f;
        dst[12] = mtx[0][3]; dst[13] = mtx[1][3]; dst[14] = mtx[2][3]; dst[15] = 1.0f;
        ++sPosMtxGen[id];
    }
}

void pc_gfx_load_nrm_mtx(const Mtx mtx, u32 id) {
    if (id >= 64 || !mtx) return;
    float* d = sNrmMatrix[id];
    d[0] = mtx[0][0]; d[1] = mtx[1][0]; d[2] = mtx[2][0];
    d[3] = mtx[0][1]; d[4] = mtx[1][1]; d[5] = mtx[2][1];
    d[6] = mtx[0][2]; d[7] = mtx[1][2]; d[8] = mtx[2][2];
    ++sNrmMtxGen[id];
}
void pc_gfx_load_tex_mtx(const Mtx mtx, u32 id) {
    if (id >= 64 || !mtx) return;
    float* d = sTexMatrices[id];
    // 3x4 row-major GX matrix -> column-major 4x4
    d[0]  = mtx[0][0]; d[1]  = mtx[1][0]; d[2]  = mtx[2][0]; d[3]  = 0.0f;
    d[4]  = mtx[0][1]; d[5]  = mtx[1][1]; d[6]  = mtx[2][1]; d[7]  = 0.0f;
    d[8]  = mtx[0][2]; d[9]  = mtx[1][2]; d[10] = mtx[2][2]; d[11] = 0.0f;
    d[12] = mtx[0][3]; d[13] = mtx[1][3]; d[14] = mtx[2][3]; d[15] = 1.0f;
    sTexMtxLoaded[id] = true;
    ++sTexMtxGen[id];
}

// ── Texture coordinate generation ──
void pc_gfx_set_tex_coord_gen(GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src,
                              u32 matrixIdx) {
    if (coord < 0 || coord >= 8) return;
    sTexCoordGen[coord].active = true;
    sTexCoordGen[coord].type = type;
    sTexCoordGen[coord].src = src;
    sTexCoordGen[coord].mtxIdx = matrixIdx;
}

void pc_gfx_set_z_mode(GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    static bool valid = false;
    static GXBool lastCompare = GX_FALSE, lastUpdate = GX_FALSE;
    static GXCompare lastFunc = GX_NEVER;
    if (valid && lastCompare == compareEnable && lastFunc == func && lastUpdate == updateEnable) return;
    valid = true; lastCompare = compareEnable; lastFunc = func; lastUpdate = updateEnable;
#ifdef __vita__
    sVitaZWriteOn = updateEnable != GX_FALSE;
#endif
    pc_gfx_note_gl_state_change();
    // Depth state last sent to GL (-1: nothing yet).
    static int glDepthTestSent = -1, glDepthMaskSent = -1;
    static GLenum glDepthFuncSent = GLenum(~0u);
    // Depth write without compare is GL_ALWAYS.
    if (compareEnable || updateEnable) {
        if (glDepthTestSent != 1) { glEnable(GL_DEPTH_TEST); glDepthTestSent = 1; }
        GLenum glfunc = GL_LEQUAL;
        switch (compareEnable ? func : GX_ALWAYS) {
            case GX_NEVER: glfunc = GL_NEVER; break;
            case GX_LESS: glfunc = GL_LESS; break;
            case GX_EQUAL: glfunc = GL_EQUAL; break;
            case GX_LEQUAL: glfunc = GL_LEQUAL; break;
            case GX_GREATER: glfunc = GL_GREATER; break;
            case GX_NEQUAL: glfunc = GL_NOTEQUAL; break;
            case GX_GEQUAL: glfunc = GL_GEQUAL; break;
            case GX_ALWAYS: glfunc = GL_ALWAYS; break;
        }
        if (glDepthFuncSent != glfunc) { glDepthFunc(glfunc); glDepthFuncSent = glfunc; }
    } else if (glDepthTestSent != 0) {
        glDisable(GL_DEPTH_TEST);
        glDepthTestSent = 0;
    }
    const int mask = updateEnable ? 1 : 0;
    if (glDepthMaskSent != mask) { glDepthMask(mask ? GL_TRUE : GL_FALSE); glDepthMaskSent = mask; }
}

void pc_gfx_set_blend_mode(GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    static bool valid = false;
    static GXBlendMode lastType = GX_BM_NONE;
    static GXBlendFactor lastSrc = GX_BL_ZERO, lastDst = GX_BL_ZERO;
    static GXLogicOp lastOp = GX_LO_CLEAR;
    if (valid && lastType == type && lastSrc == srcFactor && lastDst == dstFactor && lastOp == op) return;
    valid = true; lastType = type; lastSrc = srcFactor; lastDst = dstFactor; lastOp = op;
    sVitaBlendType = int(type); sVitaBlendSrc = int(srcFactor); sVitaBlendDst = int(dstFactor);
    pc_gfx_note_gl_state_change();
#ifndef __vita__
    glDisable(GL_COLOR_LOGIC_OP);
#endif
    static GLenum glBlendEquationSent = GLenum(~0u);   // nothing sent yet
    // GX subtract is destination - source.
    const GLenum equation = type == GX_BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
    if (glBlendEquationSent != equation) { glBlendEquation_ptr(equation); glBlendEquationSent = equation; }
    if (type == GX_BM_BLEND) {
        glEnable(GL_BLEND);
        GLenum s = GL_ONE, d = GL_ZERO;
        switch (srcFactor) {
            case GX_BL_ZERO: s = GL_ZERO; break;
            case GX_BL_ONE: s = GL_ONE; break;
            case GX_BL_SRCCOL: s = GL_DST_COLOR; break;
            case GX_BL_INVSRCCOL: s = GL_ONE_MINUS_DST_COLOR; break;
            case GX_BL_SRCALPHA: s = GL_SRC_ALPHA; break;
            case GX_BL_INVSRCALPHA: s = GL_ONE_MINUS_SRC_ALPHA; break;
            case GX_BL_DSTALPHA: s = GL_DST_ALPHA; break;
            case GX_BL_INVDSTALPHA: s = GL_ONE_MINUS_DST_ALPHA; break;
        }
        switch (dstFactor) {
            case GX_BL_ZERO: d = GL_ZERO; break;
            case GX_BL_ONE: d = GL_ONE; break;
            case GX_BL_DSTCOL: d = GL_SRC_COLOR; break;
            case GX_BL_INVDSTCOL: d = GL_ONE_MINUS_SRC_COLOR; break;
            case GX_BL_SRCALPHA: d = GL_SRC_ALPHA; break;
            case GX_BL_INVSRCALPHA: d = GL_ONE_MINUS_SRC_ALPHA; break;
            case GX_BL_DSTALPHA: d = GL_DST_ALPHA; break;
            case GX_BL_INVDSTALPHA: d = GL_ONE_MINUS_DST_ALPHA; break;
        }
        glBlendFunc(s, d);
    } else if (type == GX_BM_SUBTRACT) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
    } else if (type == GX_BM_LOGIC) {
        glDisable(GL_BLEND);
#ifndef __vita__
        glEnable(GL_COLOR_LOGIC_OP);
#endif
        static const GLenum logicOps[] = {
            GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP,
            GL_XOR, GL_OR, GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE,
            GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET
        };
        const unsigned index = static_cast<unsigned>(op);
        glLogicOp(index < sizeof(logicOps) / sizeof(logicOps[0]) ? logicOps[index] : GL_COPY);
    } else {
        glDisable(GL_BLEND);
    }
}

// GX cull mode as last set.
static bool sGxCullValid = false;
static GXCullMode sGxCullMode = GX_CULL_NONE;
void pc_gfx_set_cull_mode(GXCullMode mode) {
    if (sGxCullValid && sGxCullMode == mode) return;
    sGxCullValid = true; sGxCullMode = mode;
    pc_gfx_note_gl_state_change();
    if (mode == GX_CULL_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(mode == GX_CULL_FRONT ? GL_FRONT : GL_BACK);
    }
}

void pc_gfx_set_color_update(GXBool updateEnable) {
    if (sColorUpdate == updateEnable) return;
    sColorUpdate = updateEnable;
    pc_gfx_note_gl_state_change();
    glColorMask(sColorUpdate, sColorUpdate, sColorUpdate, sAlphaUpdate);
}

void pc_gfx_set_alpha_update(GXBool updateEnable) {
    if (sAlphaUpdate == updateEnable) return;
    sAlphaUpdate = updateEnable;
    pc_gfx_note_gl_state_change();
    glColorMask(sColorUpdate, sColorUpdate, sColorUpdate, sAlphaUpdate);
}

void pc_gfx_set_alpha_compare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    sAlphaComp0 = comp0;
    sAlphaComp1 = comp1;
    sAlphaOp = op;
    sAlphaRef0 = ref0 / 255.0f;
    sAlphaRef1 = ref1 / 255.0f;
}

void pc_gfx_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc,
                          u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn) {
    GfxChannel* ch = nullptr;
    if (chan == GX_COLOR0 || chan == GX_ALPHA0 || chan == GX_COLOR0A0) ch = &sChannels[0];
    else if (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) ch = &sChannels[1];
    if (!ch) return;
    const GfxChannel old = *ch;
    if (chan == GX_COLOR0 || chan == GX_COLOR1 || chan == GX_COLOR0A0 || chan == GX_COLOR1A1) {
        ch->enabled = enable;
        ch->matSrc = matSrc;
        ch->ambSrc = ambSrc;
        ch->lightMask = lightMask;
        ch->diffFn = diffFn;
        ch->attnFn = attnFn;
    }
    if (chan == GX_ALPHA0 || chan == GX_ALPHA1 || chan == GX_COLOR0A0 || chan == GX_COLOR1A1) {
        ch->alphaEnabled = enable;
        ch->alphaMatSrc = matSrc;
        ch->alphaAmbSrc = ambSrc;
        ch->alphaLightMask = lightMask;
        ch->alphaDiffFn = diffFn;
        ch->alphaAttnFn = attnFn;
    }
    if (memcmp(&old, ch, sizeof(*ch)) != 0) {
        vita_key_dirty(kKeyChan);
        if (old.matSrc != ch->matSrc || old.alphaMatSrc != ch->alphaMatSrc) sChanSrcDirty = true;
    }
}

// Any active TEV stage reads colour channel 1.
static inline bool vita_chan1_read() {
    const int stages = sNumTevStages < GX_MAXTEVSTAGE ? sNumTevStages : GX_MAXTEVSTAGE;
    for (int i = 0; i < stages; ++i) if (sTevStages[i].rasChannel == 1) return true;
    return false;
}
void pc_gfx_set_chan_amb_color(GXChannelID chan, GXColor color) {
    GfxChannel* ch = nullptr;
    if (chan == GX_COLOR0 || chan == GX_ALPHA0 || chan == GX_COLOR0A0) ch = &sChannels[0];
    else if (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) ch = &sChannels[1];
    if (!ch) return;
    const float c[4] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    const bool changed = memcmp(ch->ambColor, c, sizeof(c)) != 0;
    memcpy(ch->ambColor, c, sizeof(c));
    if (changed && (ch->enabled || ch->alphaEnabled) && (ch == &sChannels[0] || vita_chan1_read())) vita_key_dirty(kKeyChan);
}

void pc_gfx_set_chan_mat_color(GXChannelID chan, GXColor color) {
    GfxChannel* ch = nullptr;
    if (chan == GX_COLOR0 || chan == GX_ALPHA0 || chan == GX_COLOR0A0) ch = &sChannels[0];
    else if (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) ch = &sChannels[1];
    if (!ch) return;
    const float c[4] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    const bool changed = memcmp(ch->matColor, c, sizeof(c)) != 0;
    memcpy(ch->matColor, c, sizeof(c));
    if (changed && (ch->matSrc == GX_SRC_REG || ch->alphaMatSrc == GX_SRC_REG) && (ch == &sChannels[0] || vita_chan1_read())) vita_key_dirty(kKeyChan);
}

// ── Light management ──
void pc_gfx_init_light_pos(void* ltObj, f32 x, f32 y, f32 z) {
    if (!ltObj) return;
    // GXLightObjPriv at +0x28 is lpos[3]
    u8* raw = static_cast<u8*>(ltObj);
    f32* lpos = reinterpret_cast<f32*>(raw + 0x28);
    lpos[0] = x; lpos[1] = y; lpos[2] = z;
}
void pc_gfx_init_light_dir(void* ltObj, f32 x, f32 y, f32 z) {
    if (!ltObj) return;
    u8* raw = static_cast<u8*>(ltObj);
    f32* ldir = reinterpret_cast<f32*>(raw + 0x34);
    ldir[0] = x; ldir[1] = y; ldir[2] = z;
}
void pc_gfx_init_light_color(void* ltObj, GXColor color) {
    if (!ltObj) return;
    u8* raw = static_cast<u8*>(ltObj);
    raw[0x0C] = color.r; raw[0x0D] = color.g; raw[0x0E] = color.b; raw[0x0F] = color.a;
}
void pc_gfx_init_light_attn(void* ltObj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    if (!ltObj) return;
    u8* raw = static_cast<u8*>(ltObj);
    f32* a = reinterpret_cast<f32*>(raw + 0x10);
    a[0] = a0; a[1] = a1; a[2] = a2;
    f32* k = reinterpret_cast<f32*>(raw + 0x1C);
    k[0] = k0; k[1] = k1; k[2] = k2;
}
void pc_gfx_init_light_attn_a(void* ltObj, f32 a0, f32 a1, f32 a2) {
    if (!ltObj) return;
    u8* raw = static_cast<u8*>(ltObj);
    f32* a = reinterpret_cast<f32*>(raw + 0x10);
    a[0] = a0; a[1] = a1; a[2] = a2;
}
void pc_gfx_init_light_attn_k(void* ltObj, f32 k0, f32 k1, f32 k2) {
    if (!ltObj) return;
    u8* raw = static_cast<u8*>(ltObj);
    f32* k = reinterpret_cast<f32*>(raw + 0x1C);
    k[0] = k0; k[1] = k1; k[2] = k2;
}
void pc_gfx_init_specular_dir(void* ltObj, f32 x, f32 y, f32 z) {
    if (!ltObj) return;
    u8* raw = static_cast<u8*>(ltObj);
    // Specular direction: half-angle vector in the direction, scaled direction in the position.
    f32 vx = -x, vy = -y, vz = -z + 1.0f;
    const f32 mag = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (mag > 1e-6f) { const f32 inv = 1.0f / mag; vx *= inv; vy *= inv; vz *= inv; }
    else { vx = 0.0f; vy = 0.0f; vz = 1.0f; }   // light straight at the eye
    f32* ldir = reinterpret_cast<f32*>(raw + 0x34);
    ldir[0] = vx; ldir[1] = vy; ldir[2] = vz;
    const f32 kSpecularPosScale = 1024.0f * 1024.0f;
    f32* lpos = reinterpret_cast<f32*>(raw + 0x28);
    lpos[0] = -x * kSpecularPosScale;
    lpos[1] = -y * kSpecularPosScale;
    lpos[2] = -z * kSpecularPosScale;
}
void pc_gfx_load_light(void* ltObj, u32 lightMask) {
    if (!ltObj) return;
    for (int i = 0; i < 8; i++) {
        if (lightMask & (1 << i)) {
            u8* raw = static_cast<u8*>(ltObj);
            f32* lpos = reinterpret_cast<f32*>(raw + 0x28);
            f32* lk   = reinterpret_cast<f32*>(raw + 0x1C);
            u8* col  = raw + 0x0C;
            sLights[i].pos[0] = lpos[0]; sLights[i].pos[1] = lpos[1]; sLights[i].pos[2] = lpos[2];
            sLights[i].k[0] = lk[0]; sLights[i].k[1] = lk[1]; sLights[i].k[2] = lk[2];
            f32* ld = reinterpret_cast<f32*>(raw + 0x34);
            // Half-vector for specular (light 7) is stored directly in the direction field
            // It's already normalized, use it directly
            sLights[i].dir[0] = ld[0];
            sLights[i].dir[1] = ld[1];
            sLights[i].dir[2] = ld[2];
            f32* la = reinterpret_cast<f32*>(raw + 0x10);
            sLights[i].a[0] = la[0]; sLights[i].a[1] = la[1]; sLights[i].a[2] = la[2];
            sLights[i].color[0] = col[0] / 255.0f;
            sLights[i].color[1] = col[1] / 255.0f;
            sLights[i].color[2] = col[2] / 255.0f;
            sLights[i].color[3] = col[3] / 255.0f;
            sLights[i].active = true;
        }
    }
}

void pc_gfx_set_tev_order(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sTevStages[stage].texMap = map;
        sTevStages[stage].texCoord = coord;
        sTevStages[stage].textureEnabled = map >= GX_TEXMAP0 && map < GX_MAX_TEXMAP;
        if (chan == GX_COLOR_NULL || chan == GX_COLOR_ZERO) sTevStages[stage].rasChannel = -1;
        else if (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) sTevStages[stage].rasChannel = 1;
        else sTevStages[stage].rasChannel = 0;
    }
}

void pc_gfx_set_tev_op(GXTevStageID stage, GXTevMode mode) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        TevStageState& st = sTevStages[stage];
        st.colorOp = st.alphaOp = GX_TEV_ADD;
        st.colorBias = st.alphaBias = GX_TB_ZERO;
        st.colorScale = st.alphaScale = GX_CS_SCALE_1;
        st.colorClamp = st.alphaClamp = GX_TRUE;
        st.colorOutReg = st.alphaOutReg = GX_TEVPREV;
        if (mode == GX_REPLACE) {
            st.colorIn[0] = GX_CC_ZERO; st.colorIn[1] = GX_CC_ZERO;
            st.colorIn[2] = GX_CC_ZERO; st.colorIn[3] = GX_CC_TEXC;
            st.alphaIn[0] = GX_CA_ZERO; st.alphaIn[1] = GX_CA_ZERO;
            st.alphaIn[2] = GX_CA_ZERO; st.alphaIn[3] = GX_CA_TEXA;
        } else if (mode == GX_PASSCLR) {
            st.colorIn[0] = GX_CC_ZERO; st.colorIn[1] = GX_CC_ZERO;
            st.colorIn[2] = GX_CC_ZERO; st.colorIn[3] = GX_CC_RASC;
            st.alphaIn[0] = GX_CA_ZERO; st.alphaIn[1] = GX_CA_ZERO;
            st.alphaIn[2] = GX_CA_ZERO; st.alphaIn[3] = GX_CA_RASA;
            st.texMap = GX_TEXMAP_NULL;
            st.textureEnabled = false;
        } else if (mode == GX_MODULATE) {
            st.colorIn[0] = GX_CC_ZERO; st.colorIn[1] = GX_CC_TEXC;
            st.colorIn[2] = GX_CC_RASC; st.colorIn[3] = GX_CC_ZERO;
            st.alphaIn[0] = GX_CA_ZERO; st.alphaIn[1] = GX_CA_TEXA;
            st.alphaIn[2] = GX_CA_RASA; st.alphaIn[3] = GX_CA_ZERO;
        } else if (mode == GX_DECAL) {
            st.colorIn[0] = GX_CC_RASC; st.colorIn[1] = GX_CC_TEXC;
            st.colorIn[2] = GX_CC_TEXA; st.colorIn[3] = GX_CC_ZERO;
            st.alphaIn[0] = GX_CA_ZERO; st.alphaIn[1] = GX_CA_ZERO;
            st.alphaIn[2] = GX_CA_ZERO; st.alphaIn[3] = GX_CA_RASA;
        } else if (mode == GX_BLEND) {
            st.colorIn[0] = GX_CC_RASC; st.colorIn[1] = GX_CC_ONE;
            st.colorIn[2] = GX_CC_TEXC; st.colorIn[3] = GX_CC_ZERO;
            st.alphaIn[0] = GX_CA_ZERO; st.alphaIn[1] = GX_CA_TEXA;
            st.alphaIn[2] = GX_CA_RASA; st.alphaIn[3] = GX_CA_ZERO;
        }
    }
}

void pc_gfx_set_num_tev_stages(u8 num) {
    sNumTevStages = std::min<u8>(num, GX_MAXTEVSTAGE);
}

void pc_gfx_set_tev_color_in(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                             GXTevColorArg c, GXTevColorArg d) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sTevStages[stage].colorIn[0] = a;
        sTevStages[stage].colorIn[1] = b;
        sTevStages[stage].colorIn[2] = c;
        sTevStages[stage].colorIn[3] = d;
    }
}

void pc_gfx_set_tev_alpha_in(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                             GXTevAlphaArg c, GXTevAlphaArg d) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sTevStages[stage].alphaIn[0] = a;
        sTevStages[stage].alphaIn[1] = b;
        sTevStages[stage].alphaIn[2] = c;
        sTevStages[stage].alphaIn[3] = d;
    }
}

void pc_gfx_set_tev_color_op(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                             GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sTevStages[stage].colorOp = op;
        sTevStages[stage].colorBias = bias;
        sTevStages[stage].colorScale = scale;
        sTevStages[stage].colorClamp = clamp;
        sTevStages[stage].colorOutReg = outReg;
    }
}

void pc_gfx_set_tev_alpha_op(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                             GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sTevStages[stage].alphaOp = op;
        sTevStages[stage].alphaBias = bias;
        sTevStages[stage].alphaScale = scale;
        sTevStages[stage].alphaClamp = clamp;
        sTevStages[stage].alphaOutReg = outReg;
    }
}

void pc_gfx_set_tev_color(GXTevRegID reg, GXColor color) {
    if (reg < GX_TEVPREV || reg >= GX_MAX_TEVREG) return;
    sTevRegisters[reg][0] = color.r / 255.0f;
    sTevRegisters[reg][1] = color.g / 255.0f;
    sTevRegisters[reg][2] = color.b / 255.0f;
    sTevRegisters[reg][3] = color.a / 255.0f;
#ifdef PC_GFX_TRACE
    {
        static FILE* tf = nullptr;
        if (!tf) tf = fopen("/tmp/opencode/tevreg.log", "a");
        if (tf) { fprintf(tf, "[API] reg=%d u8 -> (%.3f,%.3f,%.3f,%.3f)\n",
                          (int)reg, color.r / 255.0f, color.g / 255.0f,
                          color.b / 255.0f, color.a / 255.0f); fflush(tf); }
    }
#endif
}

void pc_gfx_set_tev_color_s10(GXTevRegID reg, GXColorS10 color) {
    if (reg < GX_TEVPREV || reg >= GX_MAX_TEVREG) return;
    sTevRegisters[reg][0] = color.r / 255.0f;
    sTevRegisters[reg][1] = color.g / 255.0f;
    sTevRegisters[reg][2] = color.b / 255.0f;
    sTevRegisters[reg][3] = color.a / 255.0f;
#ifdef PC_GFX_TRACE
    {
        static FILE* tf = nullptr;
        if (!tf) tf = fopen("/tmp/opencode/tevreg.log", "a");
        if (tf) { fprintf(tf, "[API] reg=%d s10 -> (%.3f,%.3f,%.3f,%.3f)\n",
                          (int)reg, color.r / 255.0f, color.g / 255.0f,
                          color.b / 255.0f, color.a / 255.0f); fflush(tf); }
    }
#endif
}

void pc_gfx_set_tev_kcolor(GXTevKColorID id, GXColor color) {
    if (id < 0 || id > 3) return;
    sKonstColors[id][0] = color.r / 255.0f;
    sKonstColors[id][1] = color.g / 255.0f;
    sKonstColors[id][2] = color.b / 255.0f;
    sKonstColors[id][3] = color.a / 255.0f;
}

void pc_gfx_set_tev_kcolor_sel(GXTevStageID stage, GXTevKColorSel sel) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sKonstColorSel[stage] = sel;
    }
}

void pc_gfx_set_tev_kalpha_sel(GXTevStageID stage, GXTevKAlphaSel sel) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sKonstAlphaSel[stage] = sel;
    }
}

void pc_gfx_set_tev_swap_mode(GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
    if (stage >= GX_TEVSTAGE0 && stage < GX_MAXTEVSTAGE) {
        sTevRasSwapSel[stage] = rasSel;
        sTevTexSwapSel[stage] = texSel;
    }
}

void pc_gfx_set_tev_swap_mode_table(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha) {
    if (table >= GX_TEV_SWAP0 && table <= GX_TEV_SWAP3) {
        int idx = table - GX_TEV_SWAP0;
        sTevSwapModes[idx].red = red;
        sTevSwapModes[idx].green = green;
        sTevSwapModes[idx].blue = blue;
        sTevSwapModes[idx].alpha = alpha;
    }
}

#ifdef __vita__
// Same-shape re-upload as a sub-upload (desktop).
static void upload_2d(GLuint texId, GLint internal, int w, int h, GLenum fmt, GLenum type, const void* data) {
#ifndef __vita__
    auto it = sTexShape.find(texId);
    if (it != sTexShape.end() && it->second.w == w && it->second.h == h && it->second.fmt == fmt && it->second.type == type) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, data);
        return;
    }
#else
    // Vita: always a full glTexImage2D.
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, data);
    PcTexShape& sh = sTexShape[texId];
    sh.w = w; sh.h = h; sh.fmt = fmt; sh.type = type;
}
static GLuint sUploadTexId = 0;   // the object the native path is filling (set by the caller)

static bool vita_upload_native_texture(const void* imagePtr, int width, int height, GXTexFmt format, u8* minAlphaOut) {
    const u8* source = static_cast<const u8*>(imagePtr);
    static std::vector<u8> out;
    u8 mn = 255;
    vita_grid_reset();
    if (format == GX_TF_CMPR) {
        if ((width & 3) || (height & 3)) return false;
        const int bw = width / 4, bh = height / 4;
        out.resize(size_t(bw) * size_t(bh) * 8u);
        size_t tileOffset = 0;
        for (int tileY = 0; tileY < height; tileY += 8) {
            for (int tileX = 0; tileX < width; tileX += 8, tileOffset += 32) {
                for (int b = 0; b < 4; ++b) {
                    const int bx = tileX / 4 + (b & 1), by = tileY / 4 + (b >> 1);
                    if (bx >= bw || by >= bh) continue;
                    const u8* src = source + tileOffset + b * 8;
                    {
                        // c0 <= c1: 3-colour + transparent mode, selector 3 is alpha 0.
                        const u16 c0 = u16((src[0] << 8) | src[1]), c1 = u16((src[2] << 8) | src[3]);
                        if (c0 <= c1) {
                            bool transparent = false;
                            for (int r = 0; r < 4 && !transparent; ++r)
                                for (int p = 0; p < 4; ++p) if (((src[4 + r] >> (6 - p * 2)) & 3) == 3) { transparent = true; break; }
                            if (transparent) { mn = 0; vita_grid_min_rect(bx * 4, by * 4, bx * 4 + 3, by * 4 + 3, width, height, 0); }
                        }
                    }
                    u8* dst = &out[(size_t(by) * size_t(bw) + size_t(bx)) * 8u];
                    dst[0] = src[1]; dst[1] = src[0];   // c0, c1: big-endian to little-endian
                    dst[2] = src[3]; dst[3] = src[2];
                    for (int r = 0; r < 4; ++r) {
                        const u8 s = src[4 + r];
                        dst[4 + r] = u8(((s >> 6) & 3) | (((s >> 4) & 3) << 2) | (((s >> 2) & 3) << 4) | ((s & 3) << 6));
                    }
                }
            }
        }
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, width, height, 0, GLsizei(out.size()), out.data());
        *minAlphaOut = mn;
        return true;
    }
#ifdef __vita__
    // Attract movie planes, linear.
    if (sMovieLinearPlanes && ((format == GX_TF_I8 && width == 640 && height == 480) || (format == GX_TF_IA8 && width == 320 && height == 240))) {
        const size_t count = size_t(width) * size_t(height);
        out.resize(count * 2u);
        uint8x16_t vmin = vdupq_n_u8(255);
        if (format == GX_TF_I8) {
            for (size_t k = 0; k < count; k += 16) {
                const uint8x16_t y = vld1q_u8(source + k);
                vmin = vminq_u8(vmin, y);
                uint8x16x2_t la; la.val[0] = y; la.val[1] = y;
                vst2q_u8(out.data() + k * 2, la);
            }
        } else {
            for (size_t k = 0; k < count; k += 16) {
                const uint8x16x2_t uv = vld2q_u8(source + k * 2);   // val[0] = U (the GX alpha byte), val[1] = V
                vmin = vminq_u8(vmin, uv.val[0]);
                uint8x16x2_t la; la.val[0] = uv.val[1]; la.val[1] = uv.val[0];
                vst2q_u8(out.data() + k * 2, la);
            }
        }
        uint8_t mins[16]; vst1q_u8(mins, vmin);
        for (int k = 0; k < 16; ++k) if (mins[k] < mn) mn = mins[k];
        for (int cy = 0; cy < 16; ++cy) for (int cx = 0; cx < 16; ++cx) sVitaUploadGrid.cell[cy][cx] = mn;
        upload_2d(sUploadTexId, GL_LUMINANCE_ALPHA, width, height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, out.data());
        *minAlphaOut = mn;
        return true;
    }
#endif
    if (format == GX_TF_I4 || format == GX_TF_I8 || format == GX_TF_IA4 || format == GX_TF_IA8) {
        const int tileWidth = (format == GX_TF_IA8) ? 4 : 8;
        const int tileHeight = (format == GX_TF_I4) ? 8 : 4;
        out.resize(size_t(width) * size_t(height) * 2u);
        size_t tileOffset = 0;
        for (int tileY = 0; tileY < height; tileY += tileHeight) {
            for (int tileX = 0; tileX < width; tileX += tileWidth, tileOffset += 32) {
                for (int y = 0; y < tileHeight; ++y) {
                    if (tileY + y >= height) break;
                    for (int x = 0; x < tileWidth; ++x) {
                        if (tileX + x >= width) break;
                        const int index = y * tileWidth + x;
                        u8 l = 255, a = 255;
                        if (format == GX_TF_I4) {
                            const u8 packed = source[tileOffset + index / 2];
                            l = a = u8((((index & 1) ? packed : packed >> 4) & 0x0f) * 17);
                        } else if (format == GX_TF_I8) {
                            l = a = source[tileOffset + index];
                        } else if (format == GX_TF_IA4) {
                            const u8 packed = source[tileOffset + index];
                            a = u8((packed >> 4) * 17); l = u8((packed & 0x0f) * 17);
                        } else {
                            // GX IA8: alpha first, then intensity.
                            a = source[tileOffset + index * 2]; l = source[tileOffset + index * 2 + 1];
                        }
                        u8* dst = &out[(size_t(tileY + y) * size_t(width) + size_t(tileX + x)) * 2u];
                        dst[0] = l; dst[1] = a;
                        if (a < mn) mn = a;
                    }
                }
            }
        }
        vita_grid_from_alpha(out.data() + 1, 2, width, height);
        upload_2d(sUploadTexId, GL_LUMINANCE_ALPHA, width, height, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, out.data());
        *minAlphaOut = mn;
        return true;
    }
    if (format == GX_TF_RGB565 || format == GX_TF_RGB5A3) {
        out.resize(size_t(width) * size_t(height) * 2u);
        bool allOpaque555 = true;
        size_t tileOffset = 0;
        for (int tileY = 0; tileY < height; tileY += 4) {
            for (int tileX = 0; tileX < width; tileX += 4, tileOffset += 32) {
                for (int y = 0; y < 4; ++y) {
                    if (tileY + y >= height) break;
                    for (int x = 0; x < 4; ++x) {
                        if (tileX + x >= width) break;
                        const int index = y * 4 + x;
                        u16 value = u16((source[tileOffset + index * 2] << 8) | source[tileOffset + index * 2 + 1]);
                        if (format == GX_TF_RGB5A3) {
                            if (!(value & 0x8000)) { allOpaque555 = false; break; }
                            value = u16(((value & 0x7fff) << 1) | 1u);   // 1RRRRRGGGGGBBBBB to RRRRRGGGGGBBBBB1
                        }
                        u8* dst = &out[(size_t(tileY + y) * size_t(width) + size_t(tileX + x)) * 2u];
                        dst[0] = u8(value & 0xff); dst[1] = u8(value >> 8);   // little-endian for the GPU
                    }
                    if (!allOpaque555) break;
                }
                if (!allOpaque555) break;
            }
            if (!allOpaque555) break;
        }
        if (!allOpaque555) return false;   // mixed 555/4443 pixels
        if (format == GX_TF_RGB565)
            upload_2d(sUploadTexId, GL_RGB, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, out.data());
        else
            upload_2d(sUploadTexId, GL_RGBA, width, height, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, out.data());
        *minAlphaOut = 255;
        return true;
    }
    return false;
}
#endif

// Texture decoding and binding.
static void pc_decode_gx_tiles(const u8* source, int width, int height, GXTexFmt format,
                               int tileWidth, int tileHeight, int bytesPerTile, std::vector<u8>& rgba) {
    auto writePixel = [&](int x, int y, u8 r, u8 g, u8 b, u8 a) {
        if (x >= width || y >= height) return;
        size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        rgba[offset] = r; rgba[offset + 1] = g;
        rgba[offset + 2] = b; rgba[offset + 3] = a;
    };
    auto expand5 = [](u16 value) -> u8 { return (value << 3) | (value >> 2); };
    auto expand6 = [](u16 value) -> u8 { return (value << 2) | (value >> 4); };
    {
        size_t tileOffset = 0;
        for (int tileY = 0; tileY < height; tileY += tileHeight) {
            for (int tileX = 0; tileX < width; tileX += tileWidth, tileOffset += bytesPerTile) {
                for (int y = 0; y < tileHeight; ++y) {
                    for (int x = 0; x < tileWidth; ++x) {
                        int index = y * tileWidth + x;
                        u8 r = 255, g = 255, b = 255, a = 255;
                        if (format == GX_TF_I4) {
                            u8 packed = source[tileOffset + index / 2];
                            u8 intensity = ((index & 1) ? packed : packed >> 4) & 0x0f;
                            r = g = b = a = intensity * 17;
                        } else if (format == GX_TF_I8) {
                            r = g = b = a = source[tileOffset + index];
                        } else if (format == GX_TF_IA4) {
                            u8 packed = source[tileOffset + index];
							a = (packed >> 4) * 17; r = g = b = (packed & 0x0f) * 17;
                        } else if (format == GX_TF_IA8) {
                            // Alpha first, intensity second.
							a = source[tileOffset + index * 2];
							r = g = b = source[tileOffset + index * 2 + 1];
                        } else if (format == GX_TF_RGB565 || format == GX_TF_RGB5A3) {
                            u16 value = (source[tileOffset + index * 2] << 8) | source[tileOffset + index * 2 + 1];
                            if (format == GX_TF_RGB565) {
                                r = expand5(value >> 11); g = expand6((value >> 5) & 0x3f); b = expand5(value & 0x1f);
                            } else if (value & 0x8000) {
                                r = expand5((value >> 10) & 0x1f); g = expand5((value >> 5) & 0x1f); b = expand5(value & 0x1f);
                            } else {
                                a = ((value >> 12) & 7) * 255 / 7;
                                r = ((value >> 8) & 15) * 17; g = ((value >> 4) & 15) * 17; b = (value & 15) * 17;
                            }
                        } else if (format == GX_TF_RGBA8) {
                            a = source[tileOffset + index * 2]; r = source[tileOffset + index * 2 + 1];
                            g = source[tileOffset + 32 + index * 2]; b = source[tileOffset + 33 + index * 2];
                        } else if (format == GX_TF_CMPR) {
                            // GameCube CMPR is DXT1 arranged as four 4x4
                            // sub-blocks inside every 8x8 tile. Endpoints are
                            // big-endian and selectors are stored two bits per
                            // pixel, most-significant pair first.
                            const int block = (y / 4) * 2 + (x / 4);
                            const u8* encoded = source + tileOffset + block * 8;
                            const u16 c0 = (u16(encoded[0]) << 8) | encoded[1];
                            const u16 c1 = (u16(encoded[2]) << 8) | encoded[3];
                            u8 colors[4][4] = {};
                            colors[0][0] = expand5(c0 >> 11); colors[0][1] = expand6((c0 >> 5) & 0x3f);
                            colors[0][2] = expand5(c0 & 0x1f); colors[0][3] = 255;
                            colors[1][0] = expand5(c1 >> 11); colors[1][1] = expand6((c1 >> 5) & 0x3f);
                            colors[1][2] = expand5(c1 & 0x1f); colors[1][3] = 255;
                            if (c0 > c1) {
                                for (int channel = 0; channel < 3; ++channel) {
                                    colors[2][channel] = (2 * colors[0][channel] + colors[1][channel]) / 3;
                                    colors[3][channel] = (colors[0][channel] + 2 * colors[1][channel]) / 3;
                                }
                                colors[2][3] = colors[3][3] = 255;
                            } else {
                                for (int channel = 0; channel < 3; ++channel) {
                                    colors[2][channel] = (colors[0][channel] + colors[1][channel]) / 2;
                                }
                                colors[2][3] = 255;
                                colors[3][3] = 0;
                            }
                            const int localX = x & 3;
                            const int localY = y & 3;
                            const int selector = (encoded[4 + localY] >> (6 - localX * 2)) & 3;
                            r = colors[selector][0]; g = colors[selector][1];
                            b = colors[selector][2]; a = colors[selector][3];
                        }
                        writePixel(tileX + x, tileY + y, r, g, b, a);
                    }
                }
            }
        }
    }

}

// Tile geometry of a GX texture format: 0 bytes per tile = not decodable here.
static void pc_gx_tile_geometry(GXTexFmt format, int& tileWidth, int& tileHeight, int& bytesPerTile) {
    tileWidth = 1; tileHeight = 1; bytesPerTile = 0;
    switch (format) {
        case GX_TF_I4: tileWidth = 8; tileHeight = 8; bytesPerTile = 32; break;
        case GX_TF_I8: case GX_TF_IA4: tileWidth = 8; tileHeight = 4; bytesPerTile = 32; break;
        case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3:
            tileWidth = 4; tileHeight = 4; bytesPerTile = 32; break;
        case GX_TF_RGBA8: tileWidth = 4; tileHeight = 4; bytesPerTile = 64; break;
        case GX_TF_CMPR: tileWidth = 8; tileHeight = 8; bytesPerTile = 32; break;
        default: break;
    }
}

// Content sample: nine 32-bit words spread over a tiled GX image.
static uint64_t pc_tex_content_sample(const void* image, int width, int height, int tileW, int tileH, int bytesPerTile) {
    if (!image || tileW <= 0 || tileH <= 0 || bytesPerTile <= 0) return 0;
    const size_t bytes = size_t((width + tileW - 1) / tileW) * size_t((height + tileH - 1) / tileH) * size_t(bytesPerTile);
    const size_t words = bytes / 4;
    if (words == 0) return 0;
    const u8* p = static_cast<const u8*>(image);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i <= 8; ++i) {
        const size_t w = (words - 1) * size_t(i) / 8u;
        u32 v;
        memcpy(&v, p + w * 4, 4);
        h ^= v; h *= 1099511628211ull;
    }
    return h;
}

void pc_gfx_init_tex_obj(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap) {
    (void)mipmap;
    if (!obj || !imagePtr || width == 0 || height == 0) return;
    {
        // GXCopyTex destination: bind the copy's texture.
        auto ct = sCopyTargets.find(imagePtr);
        if (ct != sCopyTargets.end()) {
            const uintptr_t k = (uintptr_t)obj;
            sTextureCache[k] = ct->second.tex;
            sTextureSignatures[k] = PcTextureSignature { imagePtr, width, height, static_cast<u32>(format), wrapS, wrapT, false, 0 };
#ifdef __vita__
            ++sTexCacheGen;
#endif
            return;
        }
    }

    uintptr_t key = (uintptr_t)obj;
    int sTileW = 1, sTileH = 1, sTileBytes = 0;
    pc_gx_tile_geometry(format, sTileW, sTileH, sTileBytes);
    const PcTextureSignature signature {
        imagePtr, width, height, static_cast<u32>(format), wrapS, wrapT, false, 0,
        pc_tex_content_sample(imagePtr, width, height, sTileW, sTileH, sTileBytes)
    };
    const auto signatureIt = sTextureSignatures.find(key);
    if (signatureIt != sTextureSignatures.end() && signatureIt->second == signature
        && sTextureCache.find(key) != sTextureCache.end()) {
        return;
    }

    // Same image already uploaded under another obj.
    {
        auto sit = sTexBySignature.find(texture_signature_hash(signature));
        if (sit != sTexBySignature.end() && sit->second.sig == signature && sit->second.tex) {
            
#ifdef __vita__
        ++sTexCacheGen;
#endif
        sTextureCache[key] = sit->second.tex;
            
#ifdef __vita__
    ++sTexCacheGen;
#endif
    sTextureSignatures[key] = signature;
            return;
        }
    }

    GLuint texId = 0;

    auto it = sTextureCache.find(key);
    if (it != sTextureCache.end()) {
        texId = it->second;
        // Drop the content-index entry for the old signature.
        if (signatureIt != sTextureSignatures.end()) {
            auto old = sTexBySignature.find(texture_signature_hash(signatureIt->second));
            if (old != sTexBySignature.end() && old->second.tex == texId) sTexBySignature.erase(old);
        }
    } else {
        glGenTextures(1, &texId);
        
#ifdef __vita__
        ++sTexCacheGen;
#endif
        sTextureCache[key] = texId;
    }

    pc_tex_mip_forget(texId);   // a new level 0 replaces any chain
    pc_gfx_flush_batch();
    glActiveTexture_ptr(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    sBoundTextures[0] = texId;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, pc_gx_wrap_to_gl(wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, pc_gx_wrap_to_gl(wrapT));

    bool nativeDone = false;
    u8 texMinAlpha = 0;
#ifdef __vita__
    sUploadTexId = texId;
    nativeDone = vita_upload_native_texture(imagePtr, width, height, format, &texMinAlpha);
#endif
    if (!nativeDone) {
    std::vector<u8> rgba(width * height * 4, 255);
    const u8* source = static_cast<const u8*>(imagePtr);
    auto writePixel = [&](int x, int y, u8 r, u8 g, u8 b, u8 a) {
        if (x >= width || y >= height) return;
        size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        rgba[offset] = r; rgba[offset + 1] = g;
        rgba[offset + 2] = b; rgba[offset + 3] = a;
    };
    auto expand5 = [](u16 value) -> u8 { return (value << 3) | (value >> 2); };
    auto expand6 = [](u16 value) -> u8 { return (value << 2) | (value >> 4); };

    int tileWidth = 1, tileHeight = 1, bytesPerTile = 0;
    switch (format) {
        case GX_TF_I4: tileWidth = 8; tileHeight = 8; bytesPerTile = 32; break;
        case GX_TF_I8: case GX_TF_IA4: tileWidth = 8; tileHeight = 4; bytesPerTile = 32; break;
        case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3:
            tileWidth = 4; tileHeight = 4; bytesPerTile = 32; break;
        case GX_TF_RGBA8: tileWidth = 4; tileHeight = 4; bytesPerTile = 64; break;
        case GX_TF_CMPR: tileWidth = 8; tileHeight = 8; bytesPerTile = 32; break;
        default: break;
    }

    if (bytesPerTile == 0) {
        // Unsupported format warning, once per format.
        static u32 reportedFormats = 0;
        const u32 formatBit = format < 32 ? (1u << format) : 0x80000000u;
        const bool firstTime = (reportedFormats & formatBit) == 0;
        reportedFormats |= formatBit;

        // Z formats.
        const bool isDepthFormat = format == GX_TF_Z8 || format == GX_TF_Z16
                                || format == GX_TF_Z24X8;
        if (firstTime) {
            printf("[PC GX Warning] Formato de textura no decodificado 0x%X (%dx%d): %s\n",
                   static_cast<unsigned>(format), width, height,
                   isDepthFormat
                       ? "formato Z, su origen (GXCopyTex) es un stub; se deja neutro"
                       : "se dibujará transparente en vez de blanco");
            fflush(stdout);
        }
        if (!isDepthFormat) {
            std::fill(rgba.begin(), rgba.end(), static_cast<u8>(0));
        }
    }

    if (bytesPerTile) pc_decode_gx_tiles(source, width, height, format, tileWidth, tileHeight, bytesPerTile, rgba);

    upload_2d(texId, GL_RGBA, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    { u8 mn = 255; for (size_t i = 3; i < rgba.size(); i += 4) if (rgba[i] < mn) mn = rgba[i]; texMinAlpha = mn; }
#ifdef __vita__
    vita_grid_reset();
    vita_grid_from_alpha(rgba.data() + 3, 4, width, height);
#endif
    }   // !nativeDone
    apply_texture_filtering(texMinAlpha, width, height);
#ifdef __vita__
    
#ifdef __vita__
    ++sTexCacheGen;
#endif
    sTexMinAlpha[texId] = texMinAlpha;
    {
        uint8_t mn = 255;
        for (int cy = 0; cy < 16; ++cy) for (int cx = 0; cx < 16; ++cx) if (sVitaUploadGrid.cell[cy][cx] < mn) mn = sVitaUploadGrid.cell[cy][cx];
        sVitaUploadGrid.minAll = mn;
    }
    sTexAlphaGrid[texId] = sVitaUploadGrid;
    ++sActiveTexAlphaGen;
#endif
    ++sPerfTextureUploads;
    
#ifdef __vita__
    ++sTexCacheGen;
#endif
    sTextureSignatures[key] = signature;
    sTexBySignature[texture_signature_hash(signature)] = PcTexBySignature { signature, texId };
    sTexLastUsed[texId] = sTexFrame;
    if (sVitaDumpActive)
        printf("[VDUMP] TEXUPLOAD obj=%p img=%p %ux%u fmt=%u -> tex %u\n", (void*)obj, imagePtr,
               (unsigned)width, (unsigned)height, (unsigned)format, (unsigned)texId);
}

// GXInitTexObjLOD mip chains.
static std::unordered_map<GLuint, uint64_t> sTexMipSig;   // GL texture to the image and LOD settings of its chain
void pc_gfx_init_tex_obj_lod(GXTexObj* obj, GXTexFilter minFilt, GXTexFilter magFilt, f32 minLOD, f32 maxLOD, f32 lodBias) {
    (void)magFilt; (void)minLOD;
    if (!obj) return;
    GLenum glMin = 0;
    switch (minFilt) {
        case GX_NEAR_MIP_NEAR: glMin = GL_NEAREST_MIPMAP_NEAREST; break;
        case GX_LIN_MIP_NEAR:  glMin = GL_LINEAR_MIPMAP_NEAREST; break;
        case GX_NEAR_MIP_LIN:  glMin = GL_NEAREST_MIPMAP_LINEAR; break;
        case GX_LIN_MIP_LIN:   glMin = GL_LINEAR_MIPMAP_LINEAR; break;
        default: return;   // no mip filtering
    }
    const uintptr_t key = (uintptr_t)obj;
    auto sigIt = sTextureSignatures.find(key);
    auto texIt = sTextureCache.find(key);
    if (sigIt == sTextureSignatures.end() || texIt == sTextureCache.end()) return;
    const PcTextureSignature sig = sigIt->second;
    if (sig.indexed || !sig.image) return;
    const GLuint texId = texIt->second;
    const uint64_t want = texture_signature_hash(sig) ^ (uint64_t(uint32_t(lroundf(maxLOD * 16.0f))) << 48)
                        ^ (uint64_t(uint32_t(lroundf(lodBias * 256.0f) & 0xFFFF)) << 32) ^ uint64_t(glMin);
    auto done = sTexMipSig.find(texId);
    if (done != sTexMipSig.end() && done->second == want) return;
    int tw = 1, th = 1, bpt = 0;
    pc_gx_tile_geometry((GXTexFmt)sig.format, tw, th, bpt);
    const int w0 = sig.width, h0 = sig.height;
    const bool po2 = w0 > 0 && h0 > 0 && (w0 & (w0 - 1)) == 0 && (h0 & (h0 - 1)) == 0;
    int levels = int(maxLOD);
    while (levels > 0 && ((w0 >> levels) < 1 || (h0 >> levels) < 1)) --levels;
    if (!bpt || !po2 || levels < 1) return;

    pc_gfx_flush_batch();
    glActiveTexture_ptr(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    sBoundTextures[0] = texId;
    const u8* src = static_cast<const u8*>(sig.image);
    std::vector<u8> rgba;
#ifdef __vita__
    std::vector<u8> chain;   // every level, packed, for vglTexImage2DMipChain
#endif
    u8 minA = 255;
    size_t offset = 0;
    for (int l = 0; l <= levels; ++l) {
        const int w = std::max(1, w0 >> l), h = std::max(1, h0 >> l);
        rgba.assign(size_t(w) * size_t(h) * 4, 255);
        pc_decode_gx_tiles(src + offset, w, h, (GXTexFmt)sig.format, tw, th, bpt, rgba);
        for (size_t i = 3; i < rgba.size(); i += 4) if (rgba[i] < minA) minA = rgba[i];
#ifdef __vita__
        chain.insert(chain.end(), rgba.begin(), rgba.end());
#else
        if (l == 0) upload_2d(texId, GL_RGBA, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        else glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
#endif
        offset += size_t((w + tw - 1) / tw) * size_t((h + th - 1) / th) * size_t(bpt);
    }
#ifdef __vita__
    rec_tex_mipchain(texId, w0, h0, levels + 1, chain.data(), uint32_t(chain.size()));
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glMin);
#ifdef __vita__
    // LOD bias in eighths of a level.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, int(lroundf(lodBias * 8.0f)));
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lodBias);
#endif
#ifdef __vita__
    // Minimum alpha over every level.
    {
        auto ma = sTexMinAlpha.find(texId);
        if (ma == sTexMinAlpha.end() || minA < ma->second) sTexMinAlpha[texId] = minA;
        auto gr = sTexAlphaGrid.find(texId);
        if (gr != sTexAlphaGrid.end()) {
            uint8_t mn = 255;
            for (int cy = 0; cy < 16; ++cy) for (int cx = 0; cx < 16; ++cx) {
                if (minA < gr->second.cell[cy][cx]) gr->second.cell[cy][cx] = minA;
                if (gr->second.cell[cy][cx] < mn) mn = gr->second.cell[cy][cx];
            }
            gr->second.minAll = mn;
        }
        ++sActiveTexAlphaGen;
    }
#endif
    sTexMipSig[texId] = want;
}
static void pc_tex_mip_forget(GLuint tex) { sTexMipSig.erase(tex); }

void pc_gfx_init_tlut_obj(GXTlutObj* obj, void* lut, GXTlutFmt format, u16 numEntries) {
    if (!obj || !lut || numEntries == 0) return;
    PcTlut decoded;
    decoded.rgba.resize(static_cast<size_t>(numEntries) * 4);
    const u8* source = static_cast<const u8*>(lut);
    auto expand5 = [](u16 value) -> u8 { return (value << 3) | (value >> 2); };
    auto expand6 = [](u16 value) -> u8 { return (value << 2) | (value >> 4); };
    for (u16 i = 0; i < numEntries; ++i) {
        const u16 value = (u16(source[i * 2]) << 8) | source[i * 2 + 1];
        u8 r = 255, g = 255, b = 255, a = 255;
        if (format == GX_TL_IA8) {
            r = g = b = u8(value >> 8);
            a = u8(value & 0xFF);
        } else if (format == GX_TL_RGB565) {
            r = expand5(value >> 11);
            g = expand6((value >> 5) & 0x3F);
            b = expand5(value & 0x1F);
        } else if (value & 0x8000) {
            r = expand5((value >> 10) & 0x1F);
            g = expand5((value >> 5) & 0x1F);
            b = expand5(value & 0x1F);
        } else {
            a = u8(((value >> 12) & 0x7) * 255 / 7);
            r = u8(((value >> 8) & 0xF) * 17);
            g = u8(((value >> 4) & 0xF) * 17);
            b = u8((value & 0xF) * 17);
        }
        const size_t offset = static_cast<size_t>(i) * 4;
        decoded.rgba[offset] = r; decoded.rgba[offset + 1] = g;
        decoded.rgba[offset + 2] = b; decoded.rgba[offset + 3] = a;
    }
    sTlutObjects[reinterpret_cast<uintptr_t>(obj)] = std::move(decoded);
}

void pc_gfx_load_tlut(GXTlutObj* obj, u32 tlutName) {
    if (!obj) return;
    auto it = sTlutObjects.find(reinterpret_cast<uintptr_t>(obj));
    if (it != sTlutObjects.end()) sLoadedTluts[tlutName] = it->second;
}

// Minification filtering for opaque textures.
static void apply_texture_filtering(u8 minAlpha, int width, int height) {
#ifdef __vita__
    (void)minAlpha; (void)width; (void)height;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#else
    // Power of two only.
    const bool po2 = width > 0 && height > 0 && (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
    bool wantMips = minAlpha == 255 && po2 && width >= 8 && height >= 8;
    if (!glGenerateMipmap_ptr) wantMips = false;
    if (wantMips) {
        glGenerateMipmap_ptr(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#endif
}

static bool upload_ci_texture(GXTexObj* obj, const PcCiTexture& ci) {
    auto paletteIt = sLoadedTluts.find(ci.tlutName);
    if (!obj || !ci.image || paletteIt == sLoadedTluts.end() || paletteIt->second.rgba.empty()) return false;
    const PcTlut& palette = paletteIt->second;
    std::vector<u8> rgba(static_cast<size_t>(ci.width) * ci.height * 4, 0);
    int tileWidth = ci.format == GX_TF_C4 || ci.format == GX_TF_C8 ? 8 : 4;
    int tileHeight = ci.format == GX_TF_C4 ? 8 : 4;
    int bytesPerTile = 32;
    size_t tileOffset = 0;
    for (int tileY = 0; tileY < ci.height; tileY += tileHeight) {
        for (int tileX = 0; tileX < ci.width; tileX += tileWidth, tileOffset += bytesPerTile) {
            for (int y = 0; y < tileHeight; ++y) {
                for (int x = 0; x < tileWidth; ++x) {
                    const int local = y * tileWidth + x;
                    u32 paletteIndex = 0;
                    if (ci.format == GX_TF_C4) {
                        const u8 packed = ci.image[tileOffset + local / 2];
                        paletteIndex = (local & 1) ? (packed & 0xF) : (packed >> 4);
                    } else if (ci.format == GX_TF_C8) {
                        paletteIndex = ci.image[tileOffset + local];
                    } else {
                        paletteIndex = ((u32(ci.image[tileOffset + local * 2]) << 8)
                                      | ci.image[tileOffset + local * 2 + 1]) & 0x3FFF;
                    }
                    const int dstX = tileX + x, dstY = tileY + y;
                    const size_t paletteOffset = static_cast<size_t>(paletteIndex) * 4;
                    if (dstX >= ci.width || dstY >= ci.height || paletteOffset + 3 >= palette.rgba.size()) continue;
                    const size_t dst = (static_cast<size_t>(dstY) * ci.width + dstX) * 4;
                    memcpy(&rgba[dst], &palette.rgba[paletteOffset], 4);
                }
            }
        }
    }

    const uintptr_t key = reinterpret_cast<uintptr_t>(obj);
    GLuint texId = 0;
    auto textureIt = sTextureCache.find(key);
    if (textureIt == sTextureCache.end()) {
        glGenTextures(1, &texId);
        
#ifdef __vita__
        ++sTexCacheGen;
#endif
        sTextureCache[key] = texId;
    } else {
        texId = textureIt->second;
    }
    pc_tex_mip_forget(texId);   // a new level 0 replaces any chain
    pc_gfx_flush_batch();  // about to rebind and rewrite texture unit 0
    glActiveTexture_ptr(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    sBoundTextures[0] = texId;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, pc_gx_wrap_to_gl(ci.wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, pc_gx_wrap_to_gl(ci.wrapT));
    upload_2d(texId, GL_RGBA, ci.width, ci.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    { u8 mn = 255; for (size_t i = 3; i < rgba.size(); i += 4) if (rgba[i] < mn) mn = rgba[i];
      apply_texture_filtering(mn, ci.width, ci.height); }
#ifdef __vita__
    { u8 mn = 255; for (size_t i = 3; i < rgba.size(); i += 4) if (rgba[i] < mn) mn = rgba[i]; 
#ifdef __vita__
    ++sTexCacheGen;
#endif
    sTexMinAlpha[texId] = mn; ++sActiveTexAlphaGen; }
#endif
    ++sPerfTextureUploads;
    sTexLastUsed[texId] = sTexFrame;
    return true;
}

void pc_gfx_init_tex_obj_ci(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXCITexFmt format,
                            GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlutName) {
    (void)mipmap;
    if (!obj || !imagePtr || width == 0 || height == 0) return;
    const uintptr_t key = reinterpret_cast<uintptr_t>(obj);
    // C4 8x8, C8 8x4, C14X2 4x4 texels per 32-byte tile.
    const int ciTileW = format == GX_TF_C14X2 ? 4 : 8;
    const int ciTileH = format == GX_TF_C4 ? 8 : 4;
    const PcTextureSignature signature {
        imagePtr, width, height, static_cast<u32>(format), wrapS, wrapT, true, tlutName,
        pc_tex_content_sample(imagePtr, width, height, ciTileW, ciTileH, 32)
    };
    const auto signatureIt = sTextureSignatures.find(key);
    if (signatureIt != sTextureSignatures.end() && signatureIt->second == signature
        && sTextureCache.find(key) != sTextureCache.end()) {
        return;
    }

    PcCiTexture ci { static_cast<const u8*>(imagePtr), width, height, format, wrapS, wrapT, tlutName };
    sCiTextures[key] = ci;
    if (upload_ci_texture(obj, ci)) {
        
#ifdef __vita__
    ++sTexCacheGen;
#endif
    sTextureSignatures[key] = signature;
    }
}

#ifdef __vita__
// Alpha description of the texture on one map.
struct VitaTexAlphaWatch {
    int id; const VitaTexAlphaGrid* grid; uint16_t w, h; uint8_t minA, ws, wt;
    explicit VitaTexAlphaWatch(int m) : id(m), grid(sActiveTexGrid[m]), w(sActiveTexW[m]), h(sActiveTexH[m]),
        minA(sActiveTexMinAlpha[m]), ws(sActiveTexWrapS[m]), wt(sActiveTexWrapT[m]) {}
    ~VitaTexAlphaWatch() {
        if (grid != sActiveTexGrid[id] || w != sActiveTexW[id] || h != sActiveTexH[id] || minA != sActiveTexMinAlpha[id]
            || ws != sActiveTexWrapS[id] || wt != sActiveTexWrapT[id]) ++sActiveTexAlphaGen;
    }
};
#endif
void pc_gfx_load_tex_obj(GXTexObj* obj, GXTexMapID id) {
    if (id < GX_TEXMAP0 || id >= GX_MAX_TEXMAP) return;
#ifdef __vita__
    const VitaTexAlphaWatch alphaWatch(int(id));
#endif
    if (!obj) {
        sHasActiveTextures[id] = false;
        return;
    }

    uintptr_t key = (uintptr_t)obj;
#ifdef __vita__
    VitaTexLoadMemo& memo = sTexLoadMemo[(key >> 4) & 1023u];
    if (memo.obj == key && memo.gen == sTexCacheGen) {
        sActiveGLTextures[id] = memo.tex;
        sHasActiveTextures[id] = true;
        sActiveTexMinAlpha[id] = memo.minAlpha;
        sActiveTexGrid[id] = memo.grid;
        sActiveTexW[id] = memo.w; sActiveTexH[id] = memo.h;
        sActiveTexWrapS[id] = memo.wrapS; sActiveTexWrapT[id] = memo.wrapT;
        if (memo.frame != sTexFrame) { sTexLastUsed[memo.tex] = sTexFrame; memo.frame = sTexFrame; }
        return;
    }
#endif
    auto it = sTextureCache.find(key);
    if (it == sTextureCache.end()) {
        auto ci = sCiTextures.find(key);
        if (ci != sCiTextures.end() && upload_ci_texture(obj, ci->second)) it = sTextureCache.find(key);
    }
    if (it == sTextureCache.end()) {
        // Evicted: re-upload from the init parameters.
        auto sig = sTextureSignatures.find(key);
        if (sig != sTextureSignatures.end() && !sig->second.indexed && sig->second.image) {
            const PcTextureSignature sg = sig->second;
            pc_gfx_init_tex_obj(obj, const_cast<void*>(sg.image), sg.width, sg.height,
                                static_cast<GXTexFmt>(sg.format), sg.wrapS, sg.wrapT, GX_FALSE);
            it = sTextureCache.find(key);
        }
    }
    if (it != sTextureCache.end()) {
        sActiveGLTextures[id] = it->second;
        sHasActiveTextures[id] = true;
#ifdef __vita__
        { auto ma = sTexMinAlpha.find(it->second); sActiveTexMinAlpha[id] = ma != sTexMinAlpha.end() ? ma->second : 0; }
        {
            auto g = sTexAlphaGrid.find(it->second);
            auto sg = sTextureSignatures.find(key);
            if (g != sTexAlphaGrid.end() && sg != sTextureSignatures.end()) {
                sActiveTexGrid[id] = &g->second;
                sActiveTexW[id] = sg->second.width; sActiveTexH[id] = sg->second.height;
                sActiveTexWrapS[id] = uint8_t(sg->second.wrapS); sActiveTexWrapT[id] = uint8_t(sg->second.wrapT);
            } else {
                sActiveTexGrid[id] = nullptr;
            }
        }
        memo.obj = key; memo.gen = sTexCacheGen; memo.tex = it->second; memo.grid = sActiveTexGrid[id];
        memo.w = sActiveTexW[id]; memo.h = sActiveTexH[id]; memo.minAlpha = sActiveTexMinAlpha[id];
        memo.wrapS = sActiveTexWrapS[id]; memo.wrapT = sActiveTexWrapT[id]; memo.frame = sTexFrame;
#endif
        sTexLastUsed[it->second] = sTexFrame;
    } else {
        // Hasta ahora esto era mudo, y una textura que nunca llegó a subirse
        // se manifestaba solo como un artefacto en pantalla.
        static u32 reported = 0;
        if (reported < 8) {
            ++reported;
            printf("[PC GX Warning] Se pidió la textura %p para el mapa %d, "
                   "pero nunca se subió; se dibujará transparente.\n",
                   static_cast<void*>(obj), static_cast<int>(id));
            fflush(stdout);
        }
        sActiveGLTextures[id] = 0;
        sHasActiveTextures[id] = false;
    }
}

// ── Vertex Descriptor & Setup ──
// The game caches which vertex attributes it has already programmed and skips
// redundant GXSetVtxDesc calls (DGXGraphics::setupVtxDesc). That cache assumes
// nothing else touches the descriptor. Recording the history lets a misparsed
// vertex say which calls actually reached us before it.
//
// Off unless asked for. setupVtxDesc programs every attribute for every mesh
// (see the comment on it in dgxGraphics.cpp), so this runs about fifteen times
// per mesh in every frame of a normal session, purely to keep a history that
// only the wild-vertex report ever reads.
bool pc_gfx_gx_diagnostics_enabled(void) {
    static const bool enabled = [] {
        const char* value = getenv("PIKMIN_WILD_VERTS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct VtxDescEvent { uint32_t serial; int attr; int type; bool cleared; };
static VtxDescEvent sVtxDescLog[16] = {};
static uint32_t sVtxDescSerial = 0;
static inline void log_vtx_desc(int attr, int type, bool cleared) {
    if (!pc_gfx_gx_diagnostics_enabled()) return;
    sVtxDescLog[sVtxDescSerial % 16] = { sVtxDescSerial, attr, type, cleared };
    ++sVtxDescSerial;
}
void pc_gfx_dump_vtx_desc_history(const char* why) {
    fprintf(stderr, "[PC GX] vtxdesc history (%s), most recent last, %u calls total:\n", why, sVtxDescSerial);
    const uint32_t first = sVtxDescSerial > 16 ? sVtxDescSerial - 16 : 0;
    for (uint32_t i = first; i < sVtxDescSerial; ++i) {
        const VtxDescEvent& e = sVtxDescLog[i % 16];
        if (e.cleared) fprintf(stderr, "    #%u  CLEAR ALL\n", e.serial);
        else fprintf(stderr, "    #%u  attr a%d = %s\n", e.serial, e.attr,
                     e.type == GX_NONE ? "NONE" : e.type == GX_DIRECT ? "DIRECT"
                     : e.type == GX_INDEX8 ? "IDX8" : "IDX16");
    }
}

void pc_gfx_clear_vtx_desc(void) {
    for (int i = 0; i < GX_VA_MAX_ATTR; ++i) {
#ifdef __vita__
        if (sVtxDesc[i] != GX_NONE) sVitaDescDirty = true;
#endif
        sVtxDesc[i] = GX_NONE;
    }
    log_vtx_desc(0, 0, true);
}
void pc_gfx_set_vtx_desc(GXAttr attr, GXAttrType type) {
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
#ifdef __vita__
        if (sVtxDesc[attr] != type) sVitaDescDirty = true;
#endif
        sVtxDesc[attr] = type;
    }
    log_vtx_desc(int(attr), int(type), false);
}
void pc_gfx_set_vtx_attr_fmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    if (fmt >= 0 && fmt < GX_MAX_VTXFMT && attr >= 0 && attr < GX_VA_MAX_ATTR) {
#ifdef __vita__
        const VertexFormatState& cur = sVtxFormats[fmt][attr];
        if (cur.count != cnt || cur.type != type || cur.frac != frac) sVitaVatDirty = true;
#endif
        sVtxFormats[fmt][attr] = { cnt, type, frac };
    }
}
void pc_gfx_set_array(GXAttr attr, void* basePtr, u8 stride) {
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
#ifdef __vita__
        if (sVtxArrays[attr].base != static_cast<const u8*>(basePtr) || sVtxArrays[attr].stride != stride)
            sVitaDescDirty = true;
#endif
        sVtxArrays[attr] = { static_cast<const u8*>(basePtr), stride };
        sArraySetFrame[attr]  = sFrameSerial;
        sArraySetSerial[attr] = ++sArraySetCounter;
    }
}

#ifdef __vita__
// Volatile arrays (GXSetArray).
static uint32_t sVitaArrayVolatileMask = 0;
extern "C" void pc_gfx_note_array_volatile(int attr, bool contentsChange) {
    if (attr < 0 || attr >= 32) return;
    if (contentsChange) sVitaArrayVolatileMask |= 1u << attr;
    else sVitaArrayVolatileMask &= ~(1u << attr);
}
static bool vita_dl_arrays_volatile() {
    if (!sVitaArrayVolatileMask) return false;
    for (int a = 0; a < 32 && a < GX_VA_MAX_ATTR; ++a) {
        if ((sVitaArrayVolatileMask >> a & 1u) && sVtxDesc[a] == GX_INDEX8) return true;
        if ((sVitaArrayVolatileMask >> a & 1u) && sVtxDesc[a] == GX_INDEX16) return true;
    }
    return false;
}
#endif

static Vertex sCurVertex = {};

// ── Drawing & FIFO Stream Parser ──
void pc_gfx_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    sCurVtxFmt = vtxfmt;
    sRawPush.clear();
    sCurrentPrimType = type;
    sExpectedVerts = nverts;
    sVertexStream.clear();
    if (sVertexStream.capacity() < nverts) sVertexStream.reserve(nverts);
    sInPrimitive = true;
    sHaveVertex = false;
    sVerticesPretransformed = false;
#ifdef __vita__
    sVitaNeedPalette = false;
    sVitaPaletteSlots = 1;
#endif
    sCurVertex.r = 1.0f;
    sCurVertex.g = 1.0f;
    sCurVertex.b = 1.0f;
    sCurVertex.a = 1.0f;
#ifdef __vita__
    sCurVertex.mtxIdx = 0.0f;
#endif
    sCurVertex.nx = 0.0f;
    sCurVertex.ny = 0.0f;
    sCurVertex.nz = 1.0f;
    for (int i = 0; i < 4; ++i) {
        sCurVertex.tex[i][0] = 0.0f;
        sCurVertex.tex[i][1] = 0.0f;
    }
}
static int sAttrStep = 0;

void pc_gfx_position(f32 x, f32 y, f32 z) {
    if (!sInPrimitive) return;
    if (sHaveVertex) sVertexStream.push_back(sCurVertex);
    sCurVertex.x = x;
    sCurVertex.y = y;
    sCurVertex.z = z;
    sHaveVertex = true;
}

void pc_gfx_color(u8 r, u8 g, u8 b, u8 a) {
    sCurVertex.r = r / 255.0f;
    sCurVertex.g = g / 255.0f;
    sCurVertex.b = b / 255.0f;
    sCurVertex.a = a / 255.0f;
}

void pc_gfx_texcoord(f32 u, f32 v) {
    sCurVertex.tex[0][0] = u;
    sCurVertex.tex[0][1] = v;
}

void pc_gfx_push_f32(f32 val) {
    if (!sInPrimitive) return;

    if (sAttrStep == 0) sCurVertex.x = val;
    else if (sAttrStep == 1) sCurVertex.y = val;
    else if (sAttrStep == 2) {
        sCurVertex.z = val;
        // Push vertex to stream
        sVertexStream.push_back(sCurVertex);
        sAttrStep = -1; // Reset step for next vertex
    }
    sAttrStep++;
}

void pc_gfx_push_u8(u8 val) { raw_push_be(val, 1); }
void pc_gfx_push_u16(u16 val) { raw_push_be(val, 2); }
void pc_gfx_push_u32(u32 val) { raw_push_be(val, 4); }
void pc_gfx_push_s8(s8 val) { raw_push_be(u8(val), 1); }
void pc_gfx_push_s16(s16 val) { raw_push_be(u16(val), 2); }
void pc_gfx_push_s32(s32 val) { raw_push_be(u32(val), 4); }

// Transform one pushed vertex.
static inline void vita_xform_into(Vertex& v, const float* M, const float* N) {
    const float x = v.x, y = v.y, z = v.z;
    v.x = M[0] * x + M[4] * y + M[8] * z + M[12];
    v.y = M[1] * x + M[5] * y + M[9] * z + M[13];
    v.z = M[2] * x + M[6] * y + M[10] * z + M[14];
    const float nx = v.nx, ny = v.ny, nz = v.nz;
    v.nx = N[0] * nx + N[3] * ny + N[6] * nz;
    v.ny = N[1] * nx + N[4] * ny + N[7] * nz;
    v.nz = N[2] * nx + N[5] * ny + N[8] * nz;
}
static inline void vita_push_transformed(const float* M, const float* N) {
    Vertex v = sCurVertex;
    vita_xform_into(v, M, N);
    sVertexStream.push_back(v);
}
#ifdef __vita__
// Fast stream decode.
struct VitaImmLayout { unsigned stride, offC, offT, nv; };   // offC / offT: colour / texcoord tag offset, 0 when absent
static bool vita_imm_layout(const unsigned char* s, unsigned bytes, VitaImmLayout& L) {
    if (bytes < 13u || s[0] != 1) return false;
    unsigned stride = 13, offC = 0, offT = 0;
    if (stride < bytes && s[stride] == 2) { offC = stride; stride += 5; }
    if (stride < bytes && s[stride] == 3) { offT = stride; stride += 9; }
    if (bytes % stride != 0) return false;
    const unsigned nv = bytes / stride;
    for (unsigned k = 0; k < nv; ++k) {
        const unsigned char* p = s + size_t(k) * stride;
        if (p[0] != 1 || (offC && p[offC] != 2) || (offT && p[offT] != 3)) return false;
    }
    L.stride = stride; L.offC = offC; L.offT = offT; L.nv = nv;
    return true;
}
static void vita_imm_decode_fast(const unsigned char* s, const VitaImmLayout& L, const float* M, const float* N,
                                 PcVertexVec& out, Vertex& last) {
    const Vertex t = sCurVertex;
    out.clear();
    out.resize(L.nv);
    Vertex* d = out.data();
    for (unsigned k = 0; k < L.nv; ++k) {
        const unsigned char* p = s + size_t(k) * L.stride;
        Vertex& v = d[k];
        v = t;
        memcpy(&v.x, p + 1, 12);
        if (L.offC) {
            const unsigned char* c = p + L.offC + 1;
            v.r = sVitaU8F[c[0]]; v.g = sVitaU8F[c[1]]; v.b = sVitaU8F[c[2]]; v.a = sVitaU8F[c[3]];
        }
        if (L.offT) memcpy(v.tex[0], p + L.offT + 1, 8);
        if (k + 1 == L.nv) last = v;
        vita_xform_into(v, M, N);
    }
}
static void vita_imm_chain_end(int flags, bool xf);
#endif
void pc_gfx_prim_stream(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts, const unsigned char* stream, unsigned bytes, int flags) {
    if (flags & 1) pc_gfx_begin(type, vtxfmt, nverts);
    const bool xf = sInPrimitive;
    const u32 mtxId = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
    const float* M = sPosMatrix[mtxId];
    const float* N = sNrmMatrix[mtxId];
    bool decoded = false;
#ifdef __vita__
    // Single-chunk pretransformed primitive: fast decode.
    if (flags == 3 && xf) {
        VitaImmLayout lay;
        if (vita_imm_layout(stream, bytes, lay)) {
            vita_imm_decode_fast(stream, lay, M, N, sVertexStream, sCurVertex);
            sHaveVertex = false;
            decoded = true;
        }
    }
#endif
    if (!decoded) {
        const unsigned char* r = stream;
        const unsigned char* end = stream + bytes;
        while (r < end) {
            const unsigned char tag = *r++;
            if (tag == 1) {
                if (sInPrimitive) {
                    if (sHaveVertex) { if (xf) vita_push_transformed(M, N); else sVertexStream.push_back(sCurVertex); }
                    memcpy(&sCurVertex.x, r, 12);
                    sHaveVertex = true;
                }
                r += 12;
            } else if (tag == 2) {
                sCurVertex.r = r[0] / 255.0f; sCurVertex.g = r[1] / 255.0f; sCurVertex.b = r[2] / 255.0f; sCurVertex.a = r[3] / 255.0f;
                r += 4;
            } else if (tag == 3) {
                memcpy(sCurVertex.tex[0], r, 8);
                r += 8;
            } else if (tag == 4) {
                float f; memcpy(&f, r, 4); r += 4;
                pc_gfx_push_f32(f);
            } else if (tag == 5) {
                pc_gfx_push_u8(r[0]); r += 1;
            } else break;
        }
        if (xf && sInPrimitive && sHaveVertex) { vita_push_transformed(M, N); sHaveVertex = false; }
    }
    if (flags & 2) {
        if (xf) sVerticesPretransformed = true;
#ifdef __vita__
        vita_imm_chain_end(flags, xf);
        return;
#endif
        pc_gfx_end();
    }
}


// ── Specialised TEV programs ──
// The ubershader interprets the TEV configuration per pixel. That
// configuration is constant for a draw, so it belongs in the program: each
// distinct configuration gets its own generated shader with the selectors
// resolved, and the results are cached because a scene reuses a small number
// of materials. The ubershader stays as the fallback and as an A/B reference.

struct TevProgramEntry {
#ifndef __vita__
    PcTevShaderKey key;
#endif
    uint64_t hash = 0;
    GLuint fragShader = 0;         // config fragment shader object
    GLuint program = 0;            // ten-slot matrix palette
    ProgramLocations locations;
#ifndef __vita__
    GLuint programSingle = 0;      // one-slot palette
    ProgramLocations locationsSingle;
#endif
};

static std::vector<TevProgramEntry> sTevPrograms;
// Consecutive draws almost always share a material, so remember the last
// configuration and skip both the hash and the table scan when it repeats.
static PcTevShaderKey sLastTevKey;
static bool sLastTevKeyValid = false;

// Builds the key describing what the fragment stage must compute. Only
// configuration goes in; per-draw values stay as uniforms so materials that
// differ solely by colour still share one program.
static void build_tev_shader_key(PcTevShaderKey& key) {
    key = PcTevShaderKey();
    const int stages = std::clamp<int>(sNumTevStages, 1, GX_MAXTEVSTAGE);
    key.numStages = uint8_t(stages);
    for (int i = 0; i < stages; ++i) {
        const TevStageState& st = sTevStages[i];
        PcTevStageKey& out = key.stages[i];
        for (int arg = 0; arg < 4; ++arg) {
            out.colorIn[arg] = uint8_t(st.colorIn[arg]);
            out.alphaIn[arg] = uint8_t(st.alphaIn[arg]);
        }
        out.colorOp     = uint8_t(st.colorOp);
        out.colorBias   = uint8_t(st.colorBias);
        out.colorScale  = uint8_t(st.colorScale);
        out.colorClamp  = uint8_t(st.colorClamp ? 1 : 0);
        out.colorOutReg = uint8_t(st.colorOutReg);
        out.alphaOp     = uint8_t(st.alphaOp);
        out.alphaBias   = uint8_t(st.alphaBias);
        out.alphaScale  = uint8_t(st.alphaScale);
        out.alphaClamp  = uint8_t(st.alphaClamp ? 1 : 0);
        out.alphaOutReg = uint8_t(st.alphaOutReg);
        const bool textured = st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < GX_MAX_TEXMAP;
        out.texMap     = textured ? int8_t(st.texMap) : int8_t(-1);
        // The ubershader clamps the varying set it carries to four slots.
        out.texCoord   = uint8_t(std::clamp<int>(int(st.texCoord), 0, 3));
        out.rasChannel = int8_t(st.rasChannel);
        out.rasSwapSel = uint8_t(sTevRasSwapSel[i] & 3);
        out.texSwapSel = uint8_t(sTevTexSwapSel[i] & 3);
    }
    for (int t = 0; t < 4; ++t) {
        key.swapTable[t][0] = uint8_t(sTevSwapModes[t].red);
        key.swapTable[t][1] = uint8_t(sTevSwapModes[t].green);
        key.swapTable[t][2] = uint8_t(sTevSwapModes[t].blue);
        key.swapTable[t][3] = uint8_t(sTevSwapModes[t].alpha);
    }
    key.alphaComp0       = uint8_t(sAlphaComp0);
    key.alphaComp1       = uint8_t(sAlphaComp1);
    key.alphaTestOp      = uint8_t(sAlphaOp);
    key.useMaterialRgb   = uint8_t(sChannels[0].matSrc == GX_SRC_REG ? 1 : 0);
    key.useMaterialAlpha = uint8_t(sChannels[0].alphaMatSrc == GX_SRC_REG ? 1 : 0);
    key.useMaterialRgb1  = uint8_t(sVitaColor1FromAttr ? 2 : (sChannels[1].matSrc == GX_SRC_REG ? 1 : 0));
}

// Compiles and links one specialised program. Returns 0 on failure, which
// sends the caller back to the ubershader rather than dropping the draw.
static GLuint compile_specialised_program(const PcTevShaderKey& key) {
    if (!glCreateShader_ptr || !sSharedVertexShader) return 0;

    const std::string source = pc_tev_build_fragment_source(key);
    const char* sourcePtr = source.c_str();

    GLuint fragment = glCreateShader_ptr(GL_FRAGMENT_SHADER);
    glShaderSource_ptr(fragment, 1, &sourcePtr, nullptr);
    glCompileShader_ptr(fragment);
    GLint status = 0;
    if (glGetShaderiv_ptr) glGetShaderiv_ptr(fragment, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048] = { 0 };
        if (glGetShaderInfoLog_ptr) glGetShaderInfoLog_ptr(fragment, sizeof(log), nullptr, log);
        fprintf(stderr, "[PC GX] Specialised TEV shader failed to compile:\n%s\nSource:\n%s\n",
                log, source.c_str());
        glDeleteShader_ptr(fragment);
        return 0;
    }

    GLuint program = glCreateProgram_ptr();
    glAttachShader_ptr(program, sSharedVertexShader);
    glAttachShader_ptr(program, fragment);
    bind_fixed_attrib_locations(program);
    glLinkProgram_ptr(program);
    if (glGetProgramiv_ptr) glGetProgramiv_ptr(program, GL_LINK_STATUS, &status);
    glDeleteShader_ptr(fragment);
    if (status != GL_TRUE) {
        char log[2048] = { 0 };
        if (glGetProgramInfoLog_ptr) glGetProgramInfoLog_ptr(program, sizeof(log), nullptr, log);
        fprintf(stderr, "[PC GX] Specialised TEV program failed to link:\n%s\n", log);
        glDeleteProgram_ptr(program);
        return 0;
    }
    ++sPerfShaderCompiles;
    gl_error_checkpoint("specialised program creation");
    if (std::getenv("PIKMIN_DUMP_SHADERS")) {
        char path[256];
        snprintf(path, sizeof(path), "/tmp/pikmin_tev_%llu.frag",
                 (unsigned long long)sPerfShaderCompiles);
        if (FILE* out = fopen(path, "w")) {
            fputs(source.c_str(), out);
            fclose(out);
        }
    }
    return program;
}

// Selects the program for the current TEV state and makes its uniform
// locations active. Falls back to the ubershader whenever generation fails.
#ifdef __vita__
// Fallback programs.
static GLuint sVitaFbNoatProg = 0, sVitaFbAtProg = 0;
static ProgramLocations sVitaFbNoatLoc, sVitaFbAtLoc;
static GLuint sVitaFbNoatProgS = 0, sVitaFbAtProgS = 0;
static ProgramLocations sVitaFbNoatLocS, sVitaFbAtLocS;

static GLuint vita_link_program(GLuint fs, GLuint vs) {
    if (!fs || !vs) return 0;
    GLuint p = glCreateProgram_ptr();
    if (p == 0 || p >= GLuint(kVitaMaxPrograms)) {
        static int reported = 0;
        if (reported < 4) {
            ++reported;
            printf("[VITA GX] OUT OF PROGRAM HANDLES (glCreateProgram -> %u, cap %d). Raise MAX_CUSTOM_PROGRAMS in the vitaGL fork.\n", (unsigned)p, kVitaMaxPrograms);
        }
        return 0;
    }
    glAttachShader_ptr(p, vs);
    glAttachShader_ptr(p, fs);
    bind_fixed_attrib_locations(p);
    glLinkProgram_ptr(p);
    GLint status = 0;
    if (glGetProgramiv_ptr) glGetProgramiv_ptr(p, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        printf("[VITA GX] program link failed for fs=%u\n", (unsigned)fs);
        glDeleteProgram_ptr(p);
        return 0;
    }
    return p;
}

// Baked per-config programs by hash.
static std::unordered_map<uint64_t, int> sTevProgramByHash;
// Program tables frozen after the build.
static bool   sVitaProgramTablesFrozen = false;
static void vita_program_tables_check(const char* where) {
    if (!sVitaProgramTablesFrozen) return;
    printf("[VITA GX] FATAL: program table grew after init (%s); cached location pointers would dangle\n", where);
    fflush(stdout);
    abort();
}

static void vita_build_pair_programs();

static void vita_build_all_programs() {
    sSharedVertexShaderSingle = vita_gx_vertex_shader_single();
    sVitaFbNoatProg = vita_link_program(vita_gx_fallback_fragment(false), sSharedVertexShader);
    if (sVitaFbNoatProg) query_program_locations(sVitaFbNoatProg, sVitaFbNoatLoc);
    sVitaFbAtProg = vita_link_program(vita_gx_fallback_fragment(true), sSharedVertexShader);
    if (sVitaFbAtProg) query_program_locations(sVitaFbAtProg, sVitaFbAtLoc);
    sVitaFbNoatProgS = vita_link_program(vita_gx_fallback_fragment(false), sSharedVertexShaderSingle);
    if (sVitaFbNoatProgS) query_program_locations(sVitaFbNoatProgS, sVitaFbNoatLocS);
    sVitaFbAtProgS = vita_link_program(vita_gx_fallback_fragment(true), sSharedVertexShaderSingle);
    if (sVitaFbAtProgS) query_program_locations(sVitaFbAtProgS, sVitaFbAtLocS);

    sShaderProgram = sVitaFbNoatProg;
    sLocP = &sVitaFbNoatLoc;
    sUberLocations = sVitaFbNoatLoc;
    sCurrentProgram = sShaderProgram;

    const int n = vita_gx_config_count();
    sTevPrograms.reserve(size_t(n));
    sTevProgramByHash.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const GLuint frag = vita_gx_config_fragment(i);
        GLuint prog = vita_link_program(frag, sSharedVertexShader);
        if (!prog) continue;
        TevProgramEntry e;
        e.hash = vita_gx_config_hash(i);
        e.fragShader = frag;
        e.program = prog;
        query_program_locations(prog, e.locations);
        sTevProgramByHash[e.hash] = int(sTevPrograms.size());
        vita_program_tables_check("config programs");
        sTevPrograms.push_back(e);
    }
    vita_build_pair_programs();
    sVitaProgramTablesFrozen = true;
}


// Program selection memo.
struct VitaProgMemo {
    uint64_t key     = 0;
    uint64_t keyHash = 0;
    int      progIdx = -3;   // >=0 baked index, -1 fbNoat, -2 fbAt, -3 empty
};
static VitaProgMemo sVitaProgMemo[2048];
// Batch key of the current apply.
static uint64_t sVitaApplyKey      = 0;
static bool     sVitaApplyKeyValid = false;


static inline uint64_t hash_bytes(uint64_t h, const void* data, size_t bytes);

// Fingerprint of the state that selects a program.
static inline uint64_t vita_mix64(uint64_t h, uint64_t w) { h = (h ^ w) * 1099511628211ull; return h ^ (h >> 29); }
static inline uint64_t vita_tev_stage_word(int i) {
    const uint64_t h = hash_bytes(0x243F6A8885A308D3ull, &sTevStages[i], sizeof(sTevStages[i]));
    const int32_t sw[2] = { int32_t(sTevRasSwapSel[i]), int32_t(sTevTexSwapSel[i]) };
    return hash_bytes(h, sw, sizeof(sw));
}
static inline int vita_fp_stage_count() { return sNumTevStages < GX_MAXTEVSTAGE ? (sNumTevStages < 1 ? 1 : sNumTevStages) : GX_MAXTEVSTAGE; }
static inline uint64_t vita_fp_stage_tail(uint64_t h) {
    h = hash_bytes(h, sTevSwapModes, sizeof(sTevSwapModes));
    const uint8_t c1 = sVitaColor1FromAttr ? 1 : 0;
    return hash_bytes(h, &c1, 1);
}
static inline uint64_t vita_fp_finish(uint64_t stagePart) {
    const int32_t cmp[3] = { int32_t(sAlphaComp0), int32_t(sAlphaComp1), int32_t(sAlphaOp) };
    const uint8_t mats[4] = {
        (uint8_t)(sChannels[0].matSrc == GX_SRC_REG ? 1 : 0),
        (uint8_t)(sChannels[0].alphaMatSrc == GX_SRC_REG ? 1 : 0),
        (uint8_t)(sChannels[1].matSrc == GX_SRC_REG ? 1 : 0),
        (uint8_t)(sVitaColor1FromAttr ? 1 : 0),
    };
    return hash_bytes(hash_bytes(stagePart, cmp, sizeof(cmp)), mats, sizeof(mats));
}
static uint64_t tev_config_fingerprint() {
    const int n = vita_fp_stage_count();
    uint64_t f = vita_mix64(14695981039346656037ull, uint64_t(n));
    for (int i = 0; i < n; ++i) f = vita_mix64(f, vita_tev_stage_word(i));
    return vita_fp_finish(vita_fp_stage_tail(f));
}

// Vertex-shader variants.
static const float kVitaIdentity16[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };

// Vertex key memo.
struct VitaVsKeyMemo { uint64_t chan, texgen; int slots; bool c1; bool valid; bool vhValid; VitaVsKey key; uint64_t vh; };
static VitaVsKeyMemo sVitaVsKeyMemo[64];
// Vertex key from the live state.
static const uint8_t kVitaNibbleBits[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
static inline int vita_light_count4(u32 mask) {
    const int n = kVitaNibbleBits[mask & 15u] + kVitaNibbleBits[(mask >> 4) & 15u];
    return n < 4 ? n : 4;
}
static inline bool vita_texmtx_identity(u32 m) {
    VitaTexMtxIdent& c = sVitaTexMtxIdent[m];
    if (!c.valid || c.gen != sTexMtxGen[m]) {
        c.ident = memcmp(sTexMatrices[m], kVitaIdentity16, sizeof(kVitaIdentity16)) == 0 ? 1 : 0;
        c.gen = sTexMtxGen[m];
        c.valid = 1;
    }
    return c.ident != 0;
}
static VitaVsKey vita_vs_key_miss() {
    VitaVsKey k;
    const int slots = sVitaPaletteSlots;
    k.paletteSlots = uint8_t(slots <= 1 ? 1 : slots <= 3 ? 3 : slots <= 6 ? 6 : 10);
    if (sChannels[0].enabled) {
        k.chan0En = 1;
        const int n = vita_light_count4(sChannels[0].lightMask);
        k.numLights0 = uint8_t(n);
        k.attn0 = (n > 0 && sChannels[0].attnFn != GX_AF_NONE) ? 1 : 0;
    }
    if (sChannels[1].enabled) {
        k.chan1En = 1;
        const int n = vita_light_count4(sChannels[1].lightMask);
        if (n == 0)                                 k.chan1Mode = 2;   // ambient only
        else if (sChannels[1].attnFn == GX_AF_SPEC) k.chan1Mode = 1;   // half-vector specular from light colour 0
        else { k.chan1Mode = 0; k.numLights1 = uint8_t(n); k.attn1 = sChannels[1].attnFn != GX_AF_NONE ? 1 : 0; }
    }
    for (int slot = 0; slot < 4; ++slot) {
        int mode = 0;
        if (sTexCoordGen[slot].active) {
            const GXTexGenSrc src = static_cast<GXTexGenSrc>(sTexCoordGen[slot].src);
            if (src == GX_TG_POS) mode = 1;
            else if (src == GX_TG_NRM) mode = 2;
            else if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) mode = 3 + int(src - GX_TG_TEX0);
            else if (src >= GX_TG_TEXCOORD0 && src <= GX_TG_TEXCOORD6) mode = 11 + int(src - GX_TG_TEXCOORD0);
        }
        if (mode >= 3 && mode <= 10) {
            // Identity texture matrix.
            u32 mtxIdx = sTexCoordGen[slot].mtxIdx;
            if (mtxIdx >= 64) mtxIdx = 0;
            if (vita_texmtx_identity(mtxIdx)) mode = 20 + (mode - 3);
        }
        k.tcMode[slot] = uint8_t(mode);
    }
    k.pad[0] = sVitaColor1FromAttr ? 1 : 0;   // register-1 fold
    return k;
}
// Vertex key for the current state.
static VitaVsKey vita_vs_key_lookup(uint64_t* vhOut) {
    VitaVsKeyMemo& mm = sVitaVsKeyMemo[size_t((sKeyBlockChan ^ (sKeyBlockTexGen * 31u)) >> 7) & 63u];
    if (sKeyDirty == 0 && mm.valid && mm.chan == sKeyBlockChan && mm.texgen == sKeyBlockTexGen
        && mm.slots == sVitaPaletteSlots && mm.c1 == sVitaColor1FromAttr) {
        if (vhOut) *vhOut = mm.vhValid ? mm.vh : vita_vs_hash_key(mm.key);
        return mm.key;
    }
    const VitaVsKey k = vita_vs_key_miss();
    const bool keep = sKeyDirty == 0;
    const bool wantVh = vhOut || keep;
    const uint64_t vh = wantVh ? vita_vs_hash_key(k) : 0;
    if (keep) {
        mm.valid = true; mm.chan = sKeyBlockChan; mm.texgen = sKeyBlockTexGen; mm.slots = sVitaPaletteSlots; mm.c1 = sVitaColor1FromAttr;
        mm.key = k; mm.vh = vh; mm.vhValid = wantVh;
    }
    if (vhOut) *vhOut = vh;
    return k;
}
static VitaVsKey vita_vs_key_from_state() { return vita_vs_key_lookup(nullptr); }
static inline VitaVsKey vita_vs_key_and_hash(uint64_t* vh) { return vita_vs_key_lookup(vh); }

struct VitaPairEntry { uint64_t tev = 0, vs = 0; GLuint program = 0; ProgramLocations loc; };
static std::vector<VitaPairEntry> sVitaPairs;
static std::unordered_map<uint64_t, int> sVitaPairByKey;
static inline uint64_t vita_pair_key(uint64_t tev, uint64_t vs) { return tev ^ (vs * 0x9E3779B97F4A7C15ull) ^ (vs >> 7); }

// One program per baked (TEV, VS) pair.
static void vita_build_pair_programs() {
    const int nvs = vita_gx_vs_count();
    std::vector<GLuint> vsObj(size_t(nvs > 0 ? nvs : 0), 0);
    std::unordered_map<uint64_t, int> vsIdx;
    for (int i = 0; i < nvs; ++i) { vsObj[size_t(i)] = vita_gx_vs_shader(i); vsIdx[vita_gx_vs_hash(i)] = i; }
    const int np = vita_gx_pair_count();
    sVitaPairs.reserve(size_t(np > 0 ? np : 0));
    sVitaPairByKey.reserve(size_t(np > 0 ? np : 0));
    for (int i = 0; i < np; ++i) {
        uint64_t th = 0, vh = 0, ph = 0;
        vita_gx_pair(i, &th, &vh, &ph);
        auto ft = sTevProgramByHash.find(th);
        auto fv = vsIdx.find(ph);
        if (ft == sTevProgramByHash.end()) continue;
        if (fv == vsIdx.end() || !vsObj[size_t(fv->second)]) continue;
        GLuint prog = vita_link_program(sTevPrograms[size_t(ft->second)].fragShader, vsObj[size_t(fv->second)]);
        if (!prog) continue;
        VitaPairEntry pe;
        pe.tev = th; pe.vs = vh; pe.program = prog;
        query_program_locations(prog, pe.loc);
        sVitaPairByKey[vita_pair_key(th, vh)] = int(sVitaPairs.size());
        vita_program_tables_check("pair programs");
        sVitaPairs.push_back(pe);
    }
}

// Program selection memo, keyed by fingerprint.
struct VitaProgSel { uint64_t fp = 0, hash = 0; int progIdx = -3; int pair = -1; };   // progIdx: >=0 baked config, -1 fbNoat, -2 fbAt, -3 empty; pair: sVitaPairs index or -1
static VitaProgSel sVitaProgSel[256];
static inline VitaProgSel& vita_prog_sel(uint64_t fp) { return sVitaProgSel[size_t(fp ^ (fp >> 29) ^ (fp >> 47)) & 255]; }

// Bind the program for a memo entry.
static void vita_bind_program_for_index(int progIdx, int pairIdx) {
    if (pairIdx >= 0) {
        const VitaPairEntry& pe = sVitaPairs[size_t(pairIdx)];
        if (sCurrentProgram != pe.program) {
            glUseProgram_ptr(pe.program);
            invalidate_uniform_cache_on_program_switch();
            sCurrentProgram = pe.program;
            sCheckProgram = pe.program;
            sLocP = &pe.loc;
        }
        return;
    }
    GLuint prog = 0;
    if (progIdx >= 0)       prog = sTevPrograms[size_t(progIdx)].program;
    else if (progIdx == -1) prog = sVitaNeedPalette ? sVitaFbNoatProg : sVitaFbNoatProgS;
    else                    prog = sVitaNeedPalette ? sVitaFbAtProg : sVitaFbAtProgS;
    if (!prog || sCurrentProgram == prog) return;
    glUseProgram_ptr(prog);
    invalidate_uniform_cache_on_program_switch();
    sCurrentProgram = prog;
    sCheckProgram = prog;
    if (progIdx >= 0)       sLocP = &sTevPrograms[size_t(progIdx)].locations;
    else if (progIdx == -1) sLocP = sVitaNeedPalette ? &sVitaFbNoatLoc : &sVitaFbNoatLocS;
    else                    sLocP = sVitaNeedPalette ? &sVitaFbAtLoc : &sVitaFbAtLocS;
}

// Alpha combiner interval analysis: 1 always passes, 2 always fails, 0 undecided.
static VitaVsKey vita_vs_key_from_state();
// Minimum alpha of the grid cells over [c0, c1].
static uint8_t vita_grid_region_min(const VitaTexAlphaGrid* grid, const int c0[2], const int c1[2], uint8_t globalByte) {
    uint8_t mn = 255;
    if (c0[0] == 0 && c0[1] == 0 && c1[0] == 15 && c1[1] == 15) {
        mn = grid->minAll;
    } else {
        for (int cy = c0[1]; cy <= c1[1] && mn > globalByte; ++cy)
            for (int cx = c0[0]; cx <= c1[0]; ++cx) {
                const uint8_t v = grid->cell[cy][cx];
                if (v < mn) { mn = v; if (mn <= globalByte) break; }
            }
    }
    return mn;
}
static float vita_stage_tex_alpha_lo(int texMap, int texCoordSlot, const VitaVsKey& vk, bool* usedRegion) {
    const float global = sActiveTexMinAlpha[texMap] / 255.0f;
    if (!sVitaListUvValid || texCoordSlot < 0 || texCoordSlot > 3) return global;
    const VitaTexAlphaGrid* grid = sActiveTexGrid[texMap];
    const int mode = vk.tcMode[texCoordSlot];
    if (!grid || mode < 20 || mode > 27 || sActiveTexW[texMap] == 0 || sActiveTexH[texMap] == 0) return global;
    const int attr = mode - 20;
    if (attr > 3) return global;
    int c0[2], c1[2];
    for (int axis = 0; axis < 2; ++axis) {
        const float texels = float(axis ? sActiveTexH[texMap] : sActiveTexW[texMap]);
        float lo = sVitaListUvMin[attr][axis] - 1.0f / texels, hi = sVitaListUvMax[attr][axis] + 1.0f / texels;
        if (!(lo <= hi) || hi - lo >= 1.0f) { lo = 0.0f; hi = 1.0f; }
        const int wrap = axis ? sActiveTexWrapT[texMap] : sActiveTexWrapS[texMap];
        if (wrap == int(GX_CLAMP)) {
            lo = std::min(std::max(lo, 0.0f), 1.0f); hi = std::min(std::max(hi, 0.0f), 1.0f);
        } else {
            // Repeat: one period at most; mirror only inside the first period.
            const float flo = floorf(lo), fhi = floorf(hi);
            if (flo != fhi || (wrap == int(GX_MIRROR) && flo != 0.0f)) { lo = 0.0f; hi = 1.0f; }
            else { lo -= flo; hi -= flo; }
        }
        c0[axis] = std::min(std::max(int(lo * 16.0f), 0), 15);
        c1[axis] = std::min(std::max(int(hi * 16.0f), 0), 15);
        if (c1[axis] < c0[axis]) c1[axis] = c0[axis];
    }
    const uint8_t mn = vita_grid_region_min(grid, c0, c1, sActiveTexMinAlpha[texMap]);
    const float region = mn / 255.0f;
    if (region > global && usedRegion) *usedRegion = true;
    return std::max(global, region);
}
// Per-stage texture alpha bounds for the draw being applied.
static void vita_alpha_env_refresh(const VitaVsKey* vkOpt) {
    memset(sVitaStageLo, 0, sizeof(sVitaStageLo));
    sVitaStageLoRegion = false;
    if (sAlphaComp0 == GX_ALWAYS && sAlphaComp1 == GX_ALWAYS) return;
    const VitaVsKey vk = vkOpt ? *vkOpt : vita_vs_key_from_state();
    const int n = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
    for (int s = 0; s < n; ++s) {
        const TevStageState& st = sTevStages[s];
        if (!(st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < GX_MAX_TEXMAP && sHasActiveTextures[int(st.texMap)])) continue;
        const int slot = (int(st.texCoord) >= 0 && int(st.texCoord) <= 3) ? int(st.texCoord) : -1;
        sVitaStageLo[s] = uint8_t(vita_stage_tex_alpha_lo(int(st.texMap), slot, vk, &sVitaStageLoRegion) * 255.0f + 0.5f);
    }
}
// Alpha-test verdict inputs beyond the config fingerprint.
static uint64_t vita_verdict_input_hash() {
    uint64_t h = 0x3D1CEA11F0A1B2C3ull;
    const float regA[4] = { sTevRegisters[0][3], sTevRegisters[1][3], sTevRegisters[2][3], sTevRegisters[3][3] };
    h = hash_bytes(h, regA, sizeof(regA));
    const int n = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
    if (n > 0) {
        float konstA[GX_MAXTEVSTAGE];
        for (int s = 0; s < n; ++s) { float k[4]; resolve_tev_konst(u8(s), k); konstA[s] = k[3]; }
        h = hash_bytes(h, konstA, sizeof(konstA[0]) * size_t(n));
    }
    {
        const GfxChannel& ch = sChannels[0];
        const bool regSrc = ch.alphaMatSrc == GX_SRC_REG;
        const float matA = regSrc ? ch.matColor[3] : 0.0f;
        const int32_t src = int32_t(ch.alphaMatSrc);
        h = hash_bytes(h, &src, sizeof(src));
        h = hash_bytes(h, &matA, sizeof(matA));
    }
    const float refs[2] = { sAlphaRef0, sAlphaRef1 };
    const int32_t cmp[3] = { int32_t(sAlphaComp0), int32_t(sAlphaComp1), int32_t(sAlphaOp) };
    h = hash_bytes(h, refs, sizeof(refs));
    h = hash_bytes(h, cmp, sizeof(cmp));
    h = hash_bytes(h, &sVitaRasAlphaMin, sizeof(sVitaRasAlphaMin));
    h = hash_bytes(h, sVitaStageLo, sizeof(sVitaStageLo));
    return h;
}
// Alpha test that only rejects alpha 0.
static bool vita_alpha_test_rejects_zero_only() {
    auto nz = [](GXCompare c, float ref) {
        return (c == GX_GREATER && ref < 0.5f / 255.0f) || (c == GX_GEQUAL && ref > 0.5f / 255.0f && ref < 1.5f / 255.0f);
    };
    auto always = [](GXCompare c, float ref) {
        return c == GX_ALWAYS || (c == GX_LEQUAL && ref >= 1.0f - 0.5f / 255.0f) || (c == GX_GEQUAL && ref < 0.5f / 255.0f);
    };
    auto never = [](GXCompare c, float ref) {
        return c == GX_NEVER || (c == GX_GREATER && ref >= 1.0f - 0.5f / 255.0f) || (c == GX_LESS && ref < 0.5f / 255.0f);
    };
    const bool nz0 = nz(sAlphaComp0, sAlphaRef0), nz1 = nz(sAlphaComp1, sAlphaRef1);
    const bool al0 = always(sAlphaComp0, sAlphaRef0), al1 = always(sAlphaComp1, sAlphaRef1);
    const bool nv0 = never(sAlphaComp0, sAlphaRef0), nv1 = never(sAlphaComp1, sAlphaRef1);
    if (sAlphaOp == GX_AOP_AND) return (nz0 && al1) || (al0 && nz1) || (nz0 && nz1);
    if (sAlphaOp == GX_AOP_OR)  return (nz0 && nv1) || (nv0 && nz1) || (nz0 && nz1);
    return false;
}
// Alpha-test variant bits: bit 0 soft test, bit 1 zero-only elision.
static unsigned vita_alpha_variant_bits() {
    const bool tested = !(sAlphaComp0 == GX_ALWAYS && sAlphaComp1 == GX_ALWAYS);
    if (!tested || sVitaZWriteOn) return 0;
    // Soft alpha test.
    const bool soft = (sVitaBlendType == int(GX_BM_BLEND)
                       && (sVitaBlendDst == int(GX_BL_ONE) || sVitaBlendDst == int(GX_BL_INVSRCALPHA) || sVitaBlendDst == int(GX_BL_INVDSTCOL)))
                      || sVitaBlendType == int(GX_BM_SUBTRACT);
    // Zero-only alpha test elision.
    const bool zeroOnly = sVitaBlendType == int(GX_BM_BLEND)
        && sVitaBlendSrc == int(GX_BL_SRCALPHA) && (sVitaBlendDst == int(GX_BL_INVSRCALPHA) || sVitaBlendDst == int(GX_BL_ONE))
        && vita_alpha_test_rejects_zero_only();
    return (soft ? 1u : 0u) | (zeroOnly ? 2u : 0u);
}
static int vita_alpha_test_verdict(float* outLo, float* outHi) {
    if (outLo) *outLo = 0.0f;
    if (outHi) *outHi = 1.0f;
    if (sAlphaComp0 == GX_ALWAYS && sAlphaComp1 == GX_ALWAYS) return 1;
    float lo[4], hi[4];
    for (int r = 0; r < 4; ++r) lo[r] = hi[r] = sTevRegisters[r][3];
    const GfxChannel& ch = sChannels[0];
    float rasLo = sVitaRasAlphaMin, rasHi = 1.0f;
    if (ch.alphaMatSrc == GX_SRC_REG) rasLo = rasHi = ch.matColor[3];
    const int n = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
    for (int s = 0; s < n; ++s) {
        const TevStageState& st = sTevStages[s];
        float texLo = 0.0f, texHi = 1.0f;
        if (st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < GX_MAX_TEXMAP && sHasActiveTextures[int(st.texMap)])
            texLo = sVitaStageLo[s] / 255.0f;   // whole texture, or the sampled region of it
        if (sTevSwapModes[int(sTevTexSwapSel[s]) & 3].alpha != GX_CH_ALPHA) texLo = 0.0f;
        float sRasLo = rasLo, sRasHi = rasHi;
        if (sTevSwapModes[int(sTevRasSwapSel[s]) & 3].alpha != GX_CH_ALPHA) { sRasLo = 0.0f; sRasHi = 1.0f; }
        float k[4]; resolve_tev_konst(u8(s), k);
        auto arg = [&](int id, float& a, float& b) {
            switch (id) {
            case 0: case 1: case 2: case 3: a = lo[id]; b = hi[id]; break;   // APREV, A0, A1, A2
            case 4: a = texLo; b = texHi; break;                              // TEXA
            case 5: a = sRasLo; b = sRasHi; break;                            // RASA
            case 6: a = k[3]; b = k[3]; break;                                // KONST
            default: a = 0.0f; b = 0.0f; break;                               // ZERO
            }
        };
        float aLo, aHi, bLo, bHi, cLo, cHi, dLo, dHi;
        arg(int(st.alphaIn[0]), aLo, aHi); arg(int(st.alphaIn[1]), bLo, bHi);
        arg(int(st.alphaIn[2]), cLo, cHi); arg(int(st.alphaIn[3]), dLo, dHi);
        float rLo, rHi;
        if (int(st.alphaOp) >= 8) {   // compare ops: d + (cond ? c : 0)
            rLo = dLo + std::min(0.0f, cLo); rHi = dHi + std::max(0.0f, cHi);
        } else {
            float mLo = 1.0e30f, mHi = -1.0e30f;
            for (int corner = 0; corner < 8; ++corner) {
                const float a = (corner & 1) ? aHi : aLo, b = (corner & 2) ? bHi : bLo, c = (corner & 4) ? cHi : cLo;
                const float v = a * (1.0f - c) + b * c;
                mLo = std::min(mLo, v); mHi = std::max(mHi, v);
            }
            if (int(st.alphaOp) == 0) { rLo = dLo + mLo; rHi = dHi + mHi; }
            else                      { rLo = dLo - mHi; rHi = dHi - mLo; }
            const float bias  = st.alphaBias == GX_TB_ADDHALF ? 0.5f : st.alphaBias == GX_TB_SUBHALF ? -0.5f : 0.0f;
            const float scale = st.alphaScale == GX_CS_SCALE_2 ? 2.0f : st.alphaScale == GX_CS_SCALE_4 ? 4.0f
                              : st.alphaScale == GX_CS_DIVIDE_2 ? 0.5f : 1.0f;
            rLo = (rLo + bias) * scale; rHi = (rHi + bias) * scale;
        }
        const float cl = st.alphaClamp ? 0.0f : -4.0f, cu = st.alphaClamp ? 1.0f : 4.0f;
        rLo = std::min(std::max(rLo, cl), cu); rHi = std::min(std::max(rHi, cl), cu);
        const int out = int(st.alphaOutReg) & 3;
        lo[out] = rLo; hi[out] = rHi;
    }
    const float pl = lo[0], ph = hi[0];   // final alpha is TEVPREV's
    if (outLo) *outLo = pl;
    if (outHi) *outHi = ph;
    // 8-bit rounding plus a half-precision margin, except at 0 and 1.
    const float half8 = 0.5f / 255.0f, m = 1.5f / 255.0f;
    const float mLo = (pl <= 0.0f || pl >= 1.0f) ? 0.0f : m;
    const float mHi = (ph <= 0.0f || ph >= 1.0f) ? 0.0f : m;
    auto judge = [&](GXCompare c, float ref, int& pass, int& fail) {
        pass = fail = 0;
        switch (int(c)) {
        case GX_NEVER:   fail = 1; break;
        case GX_ALWAYS:  pass = 1; break;
        case GX_GEQUAL:  if (pl >= ref - half8 + mLo) pass = 1; else if (ph < ref - half8 - mHi) fail = 1; break;
        case GX_GREATER: if (pl >= ref + half8 + mLo) pass = 1; else if (ph < ref + half8 - mHi) fail = 1; break;
        case GX_LESS:    if (ph < ref - half8 - mHi) pass = 1; else if (pl >= ref - half8 + mLo) fail = 1; break;
        case GX_LEQUAL:  if (ph < ref + half8 - mHi) pass = 1; else if (pl >= ref + half8 + mLo) fail = 1; break;
        default: break;   // EQUAL / NEQUAL: undecided
        }
    };
    int p0, f0, p1, f1;
    judge(sAlphaComp0, sAlphaRef0, p0, f0);
    judge(sAlphaComp1, sAlphaRef1, p1, f1);
    switch (int(sAlphaOp)) {
    case GX_AOP_AND: if (p0 && p1) return 1; if (f0 || f1) return 2; return 0;
    case GX_AOP_OR:  if (p0 || p1) return 1; if (f0 && f1) return 2; return 0;
    default: return 0;
    }
}

static void use_program_for_current_state_vita() {
    uint64_t vh;
    const VitaVsKey vk = vita_vs_key_and_hash(&vh);
    const uint64_t ident = (sKeyDirty & (kKeyTev | kKeyChan | kKeyAlpha)) ? tev_config_fingerprint() : sKeyBlockTevIdent;
    if (!(sVitaStageLoKeyCall == sKeyCalls && sKeyDirty == 0 && !sChanSrcDirty)) vita_alpha_env_refresh(&vk);
    const unsigned variant = vita_alpha_variant_bits();
    const bool soft = (variant & 1u) != 0;
    const bool tested = !(sAlphaComp0 == GX_ALWAYS && sAlphaComp1 == GX_ALWAYS);
    const bool zeroOnly = (variant & 2u) != 0;
    uint64_t env = 0;
    if (tested) {
        const int n = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
        for (int s = 0; s < n; ++s) env = env * 257u + sVitaStageLo[s];
        env = (env * 257u + uint64_t(sVitaRasAlphaMin * 255.0f)) * 0x9E3779B97F4A7C15ull;
        if (zeroOnly) env ^= 0x2E7A5C1D9B3F0481ull;
        env ^= vita_verdict_input_hash() * 0xC2B2AE3D27D4EB4Full;
    }
    if (sTevRegisters[GX_TEVREG0][0] < 0.0f) env ^= 0x6D0F1E5A3C2B4987ull;
    const uint64_t fp = ident ^ (vh * 0x9E3779B97F4A7C15ull) ^ (soft ? kVitaSoftAlphaTag : 0ull) ^ env;
    VitaProgSel& sel = vita_prog_sel(fp);
    const size_t selIdx = size_t(&sel - sVitaProgSel);
    if (sel.progIdx != -3 && sel.fp == fp) {
        const uint8_t kind = sVitaProgSelKind[selIdx];
        sVitaDrawRejected = kind == 3;
        sVitaCurAlphaKind = tested ? kind : 0xFF;
        vita_bind_program_for_index(sel.progIdx, sel.pair);
        sVitaLastKeyHash = sel.hash;
        return;
    }
    PcTevShaderKey key;
    build_tev_shader_key(key);
    const uint64_t hash = pc_tev_hash_key(key);
    vita_gx_capture_key(key, hash);
    const int verdict = tested ? vita_alpha_test_verdict(nullptr, nullptr) : 0;
    const bool elide = verdict == 1 || zeroOnly;
    sVitaDrawRejected = verdict == 2;
    sVitaProgSelKind[selIdx] = uint8_t(verdict == 2 ? 3 : elide ? 2 : soft ? 1 : 0);
    sVitaCurAlphaKind = tested ? sVitaProgSelKind[selIdx] : 0xFF;
    const uint64_t lookup = elide ? (hash ^ kVitaElideAlphaTag) : soft ? (hash ^ kVitaSoftAlphaTag) : hash;
    sVitaLastKeyHash = lookup;

    // Baked pair.
    auto pit = sVitaPairByKey.find(vita_pair_key(lookup, vh));
    if (pit != sVitaPairByKey.end()) {
        vita_bind_program_for_index(0, pit->second);
        sel.fp = fp; sel.hash = lookup; sel.progIdx = 0; sel.pair = pit->second;
        return;
    }
    // Unbaked pair: capture it and use the ubershader vertex program.
    vita_gx_capture_vs_pair(hash, vk);
    {
        // Missing pairs already reported.
        static uint64_t reportedPairs[64][2]; static unsigned nPairs = 0;
        bool known = false;
        for (unsigned i = 0; i < nPairs; ++i) if (reportedPairs[i][0] == lookup && reportedPairs[i][1] == vh) { known = true; break; }
        if (!known && nPairs < 64) {
            reportedPairs[nPairs][0] = lookup; reportedPairs[nPairs][1] = vh; ++nPairs;
            printf("[VITA VS] pair (config %llu, vertex %llu) NOT BAKED -> ubershader vertex program\n",
                   (unsigned long long)lookup, (unsigned long long)vh);
        }
    }

    // Baked per-config program.
    auto found = sTevProgramByHash.find(lookup);
    if (found != sTevProgramByHash.end()) {
        TevProgramEntry& entry = sTevPrograms[size_t(found->second)];
        const GLuint want = entry.program;
        if (sCurrentProgram != want) {
            glUseProgram_ptr(want);
            invalidate_uniform_cache_on_program_switch();
            sCurrentProgram = want;
            sCheckProgram = want;
            sLocP = &entry.locations;
        }
        sel.fp = fp; sel.hash = lookup; sel.progIdx = found->second; sel.pair = -1;
        return;
    }

    // Unbaked config: fallback program.
    {
        static uint64_t reported[64]; static unsigned nReported = 0;
        bool known = false;
        for (unsigned i = 0; i < nReported; ++i) if (reported[i] == hash) { known = true; break; }
        if (!known && nReported < 64) {
            reported[nReported++] = hash;
            printf("[VITA TEV] config %llu NOT BAKED -> ubershader (stages=%u)\n",
                   (unsigned long long)hash, (unsigned)key.numStages);
        }
    }
    const bool at = vita_gx_key_has_alpha_test(key);
    GLuint prog = sVitaNeedPalette ? (at ? sVitaFbAtProg : sVitaFbNoatProg)
                                   : (at ? sVitaFbAtProgS : sVitaFbNoatProgS);
    if (prog && sCurrentProgram != prog) {
        glUseProgram_ptr(prog);
        invalidate_uniform_cache_on_program_switch();
        sCurrentProgram = prog;
        sCheckProgram = prog;
        sLocP = sVitaNeedPalette ? (at ? &sVitaFbAtLoc : &sVitaFbNoatLoc)
                                 : (at ? &sVitaFbAtLocS : &sVitaFbNoatLocS);
    }
    if (prog) { sel.fp = fp; sel.hash = hash; sel.progIdx = at ? -2 : -1; sel.pair = -1; }
}
#endif // __vita__


#ifndef __vita__
// Desktop TEV config capture (NECTAR_TEV_CAPTURE).
static void pc_tev_capture(const PcTevShaderKey& key, uint64_t hash) {
    static const char* path = std::getenv("NECTAR_TEV_CAPTURE");
    if (!path) return;
    static std::unordered_set<uint64_t> seen;
    if (!seen.insert(hash).second) return;
    FILE* f = fopen(path, "ab");
    if (!f) return;
    const uint16_t n = (uint16_t)sizeof(PcTevShaderKey);
    fwrite(&hash, sizeof(hash), 1, f);
    fwrite(&n, sizeof(n), 1, f);
    fwrite(&key, n, 1, f);
    fclose(f);
    fprintf(stderr, "[PC TEV] captured config %llu stages=%u (%u unique so far)\n",
            (unsigned long long)hash, (unsigned)key.numStages, (unsigned)seen.size());
}
#endif

static void use_program_for_current_state() {
#ifdef __vita__
    use_program_for_current_state_vita();
#else
    if (std::getenv("NECTAR_TEV_CAPTURE")) {
        PcTevShaderKey capKey;
        build_tev_shader_key(capKey);
        pc_tev_capture(capKey, pc_tev_hash_key(capKey));
    }
    if (!sSpecialiseShaders || sSpecialiseFailed
        || int(sNumTevStages) > sSpecialiseMaxStages) {
        if (sCurrentProgram != sShaderProgram) {
            glUseProgram_ptr(sShaderProgram);
            invalidate_uniform_cache();
            sCurrentProgram = sShaderProgram;
            sCheckProgram = sShaderProgram;
            sLoc = sUberLocations;
            sLastTevKeyValid = false;
        }
        return;
    }

    PcTevShaderKey key;
    build_tev_shader_key(key);
    if (sLastTevKeyValid && sCurrentProgram != sShaderProgram && key == sLastTevKey) {
        return;
    }
    const uint64_t hash = pc_tev_hash_key(key);
    sVitaLastKeyHash = hash;

    for (TevProgramEntry& entry : sTevPrograms) {
        if (entry.hash != hash || !(entry.key == key)) continue;
        if (sCurrentProgram != entry.program) {
            glUseProgram_ptr(entry.program);
            invalidate_uniform_cache();
            sCurrentProgram = entry.program;
            sCheckProgram = entry.program;
            sLoc = entry.locations;
        }
        sLastTevKey = key;
        sLastTevKeyValid = true;
        return;
    }

    const GLuint program = compile_specialised_program(key);
    if (!program) {
        // One failure is treated as a permanent fallback: a configuration this
        // generator cannot express must not be retried for every draw.
        sSpecialiseFailed = true;
        sLastTevKeyValid = false;
        fprintf(stderr, "[PC GX] Falling back to the TEV ubershader for the rest of the session.\n");
        glUseProgram_ptr(sShaderProgram);
        invalidate_uniform_cache();
        sCurrentProgram = sShaderProgram;
        sLoc = sUberLocations;
        return;
    }

    TevProgramEntry entry;
    entry.key = key;
    entry.hash = hash;
    entry.program = program;
    query_program_locations(program, entry.locations);
    sTevPrograms.push_back(entry);

    if (std::getenv("PIKMIN_DUMP_SHADERS")) {
        printf("[PC GX dump] program %llu stages=%d; samplers:",
               (unsigned long long)sPerfShaderCompiles, int(key.numStages));
        for (int i = 0; i < 8; ++i) {
            if (entry.locations.tex[i] < 0) continue;
            GLint bound = -1;
            if (glGetUniformiv_ptr) {
                glGetUniformiv_ptr(program, entry.locations.tex[i], &bound);
            }
            printf(" uTex%d=%d", i, bound);
        }
        // The stages that actually sample, so the two can be compared.
        printf(" | wants:");
        for (int i = 0; i < int(key.numStages); ++i) {
            printf(" s%d:map=%d,tc=%d", i, int(key.stages[i].texMap), int(key.stages[i].texCoord));
        }
        printf("\n");
        fflush(stdout);
    }

    glUseProgram_ptr(program);
    invalidate_uniform_cache();
    sCurrentProgram = program;
    sCheckProgram = program;
    sLoc = entry.locations;
    sLastTevKey = key;
    sLastTevKeyValid = true;
#endif
}

void pc_gfx_set_shader_specialisation(bool enabled) {
    pc_gfx_flush_batch();  // changes which program the next batch compiles to
    sSpecialiseShaders = enabled;
}

bool pc_gfx_get_shader_specialisation(void) {
    return sSpecialiseShaders && !sSpecialiseFailed;
}

size_t pc_gfx_get_specialised_program_count(void) {
    return sTevPrograms.size();
}

// Captured at the position fetch so a wild vertex can say which of the three
// inputs to `base + index * stride` was wrong.
static u16      sLastPosIndex  = 0;
static const u8* sLastPosBase  = nullptr;
static u8       sLastPosStride = 0;

// The one-shot message said a desync happened but not which list, where in it,
// or what the bytes look like -- which is the difference between "the game
// handed us a buffer that was already garbage" and "the parser drifted part
// way through a valid list". Reports the first few with context, then stops.
static u8 attr_inline_size(GXAttr attr, const VertexFormatState& fmt);
static int sDesyncReports = 0;
static void report_desync(const char* what, const void* list, size_t offset, u32 nbytes) {
    if (sDesyncReports >= 8) return;
    ++sDesyncReports;
    const u8* base = static_cast<const u8*>(list);
    fprintf(stderr, "[PC GX] DESYNC #%d %s at byte %zu/%u  head:", sDesyncReports, what, offset, nbytes);
    for (size_t i = 0; i < 12 && i < nbytes; ++i) fprintf(stderr, " %02X", base[i]);
    fprintf(stderr, "  near:");
    const size_t from = offset > 6 ? offset - 6 : 0;
    for (size_t i = from; i < from + 12 && i < nbytes; ++i) {
        fprintf(stderr, "%s%02X", i == offset ? " >" : " ", base[i]);
    }
    fprintf(stderr, "\n");

    // The stride is what actually went wrong: the head of every list parses,
    // so the parser is consuming the wrong number of bytes per vertex and
    // landing mid-data. Print the descriptor that produced it.
    fprintf(stderr, "[PC GX] DESYNC #%d vtxdesc:", sDesyncReports);
    unsigned stride = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
        const GXAttrType d = sVtxDesc[a];
        if (d == GX_NONE) continue;
        const char* kind = d == GX_DIRECT ? "direct" : d == GX_INDEX8 ? "idx8"
                         : d == GX_INDEX16 ? "idx16" : "?";
        unsigned bytes = d == GX_DIRECT
                             ? attr_inline_size(static_cast<GXAttr>(a), sVtxFormats[0][a])
                             : (d == GX_INDEX8 ? 1u : 2u);
        stride += bytes;
        fprintf(stderr, " a%d=%s(%u)", a, kind, bytes);
    }
    fprintf(stderr, "  stride=%u bytes\n", stride);
}

// Eight bytes per round rather than one. This runs for every GX primitive --
// 25.000 times in a heavy frame -- because the batcher, not just the profiler,
// depends on it: a byte-at-a-time FNV over the ~2 KB of live state measured
// 42 ms/frame and became the bottleneck it was meant to remove.
static inline uint64_t hash_bytes(uint64_t h, const void* data, size_t bytes) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    while (bytes >= 8) {
        uint64_t word;
        memcpy(&word, p, 8);
        h = (h ^ word) * 1099511628211ull;
        h ^= h >> 29;
        p += 8;
        bytes -= 8;
    }
    if (bytes) {
        uint64_t tail = 0;
        memcpy(&tail, p, bytes);
        h = (h ^ tail) * 1099511628211ull;
        h ^= h >> 29;
    }
    return h;
}

// Everything the uniform block below reads, plus the bound textures and the
// primitive class. Deliberately over-inclusive: a key that is too coarse would
// report runs that a real batcher could not merge, which is the one error that
// would make this measurement worse than useless.
// TEV constants the active stages read.
static inline unsigned vita_tev_stage_reads(const TevStageState& st) {
    unsigned m = 0;
    for (int a = 0; a < 4; ++a) {
        switch (st.colorIn[a]) {
        case GX_CC_C0: case GX_CC_A0: m |= 1u; break;
        case GX_CC_C1: case GX_CC_A1: m |= 2u; break;
        case GX_CC_C2: case GX_CC_A2: m |= 4u; break;
        case GX_CC_KONST:             m |= 8u; break;
        default: break;
        }
        switch (st.alphaIn[a]) {
        case GX_CA_A0:    m |= 1u; break;
        case GX_CA_A1:    m |= 2u; break;
        case GX_CA_A2:    m |= 4u; break;
        case GX_CA_KONST: m |= 8u; break;
        default: break;
        }
    }
    return m;
}
static unsigned vita_tev_reads_mask() {
    unsigned m = 0;
    const int stages = sNumTevStages < GX_MAXTEVSTAGE ? sNumTevStages : GX_MAXTEVSTAGE;
    for (int i = 0; i < stages; ++i) m |= vita_tev_stage_reads(sTevStages[i]);
    return m;
}
static uint64_t hash_tev_constants_m(uint64_t h, unsigned m) {
    h = hash_bytes(h, &m, sizeof(m));
    h = hash_bytes(h, sTevRegisters[GX_TEVPREV], sizeof(sTevRegisters[GX_TEVPREV]));
    if (m & 1u) h = hash_bytes(h, sTevRegisters[GX_TEVREG0], sizeof(sTevRegisters[GX_TEVREG0]));
    if (m & 2u) h = hash_bytes(h, sTevRegisters[GX_TEVREG1], sizeof(sTevRegisters[GX_TEVREG1]));
    if (m & 4u) h = hash_bytes(h, sTevRegisters[GX_TEVREG2], sizeof(sTevRegisters[GX_TEVREG2]));
    if (m & 8u) h = hash_bytes(h, sKonstColors, sizeof(sKonstColors));
    return h;
}
static uint64_t hash_tev_constants(uint64_t h) { return hash_tev_constants_m(h, vita_tev_reads_mask()); }
#ifdef __vita__
// Canonical batch key.
static unsigned sVitaUsedMapMask = 0xFFu;   // maps an enabled stage samples
static bool     sVitaChanBlockC1 = false;   // channel block hashed with channel 1 read
static inline uint8_t vita_palette_bucket(int slots) { return uint8_t(slots <= 1 ? 1 : slots <= 3 ? 3 : slots <= 6 ? 6 : 10); }
static inline unsigned vita_used_map_mask_live() {
    unsigned m = 0;
    for (u8 stage = 0; stage < sNumTevStages && stage < GX_MAXTEVSTAGE; ++stage) {
        const TevStageState& st = sTevStages[stage];
        if (st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < 8) m |= 1u << unsigned(st.texMap);
    }
    return m;
}
static inline void vita_key_textures(GLuint tex[8], uint8_t has[8], unsigned mask) {
    for (int i = 0; i < 8; ++i) {
        const bool keep = (mask >> i) & 1u;
        tex[i] = keep ? sActiveGLTextures[i] : 0;
        has[i] = (keep && sHasActiveTextures[i]) ? 1 : 0;
    }
}
#endif
#ifndef __vita__
static uint64_t compute_batch_state_key() {
    uint64_t h = 14695981039346656037ull;
    h = hash_bytes(h, &sCurrentProgram, sizeof(sCurrentProgram));
    h = hash_bytes(h, &sProjMtxGen, sizeof(sProjMtxGen));
    h = hash_bytes(h, &sVerticesPretransformed, sizeof(sVerticesPretransformed));
    // Only the matrices actually selected: hashing all 64 slots would report a
    // break every time an unrelated model loaded its own matrix.
    if (!sVerticesPretransformed) {
        const u32 id = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
        const uint64_t mtxRevision = (uint64_t(id) << 40) ^ (uint64_t(sPosMtxGen[id]) << 20)
                                   ^ uint64_t(sNrmMtxGen[id]);
        h = hash_bytes(h, &mtxRevision, sizeof(mtxRevision));
    }
    h = hash_bytes(h, sChannels, sizeof(sChannels));
    // Only the lights the enabled channels actually select: hashing all eight
    // would report a break whenever an unrelated, unreferenced light moved.
    for (int ch = 0; ch < 2; ++ch) {
        if (!sChannels[ch].enabled) continue;
        const u32 mask = sChannels[ch].lightMask;
        for (int i = 0; i < 8; ++i) {
            if (mask & (1u << i)) h = hash_bytes(h, &sLights[i], sizeof(sLights[i]));
        }
    }
    h = hash_bytes(h, &sAlphaComp0, sizeof(sAlphaComp0));
    h = hash_bytes(h, &sAlphaComp1, sizeof(sAlphaComp1));
    h = hash_bytes(h, &sAlphaOp, sizeof(sAlphaOp));
    h = hash_bytes(h, &sAlphaRef0, sizeof(sAlphaRef0));
    h = hash_bytes(h, &sAlphaRef1, sizeof(sAlphaRef1));
    h = hash_tev_constants(h);
    h = hash_bytes(h, &sNumTevStages, sizeof(sNumTevStages));
    const int stages = sNumTevStages < GX_MAXTEVSTAGE ? sNumTevStages : GX_MAXTEVSTAGE;
    for (int i = 0; i < stages; ++i) {
        h = hash_bytes(h, &sTevStages[i], sizeof(sTevStages[i]));
        h = hash_bytes(h, &sTevRasSwapSel[i], sizeof(sTevRasSwapSel[i]));
        h = hash_bytes(h, &sTevTexSwapSel[i], sizeof(sTevTexSwapSel[i]));
    }
    h = hash_bytes(h, sTevSwapModes, sizeof(sTevSwapModes));
    { const uint8_t c1 = sVitaColor1FromAttr ? 1 : 0; h = hash_bytes(h, &c1, 1); }
    for (int slot = 0; slot < 8; ++slot) {
        h = hash_bytes(h, &sTexCoordGen[slot], sizeof(sTexCoordGen[slot]));
        if (sTexCoordGen[slot].active) {
            const u32 m = sTexCoordGen[slot].mtxIdx < 64 ? sTexCoordGen[slot].mtxIdx : 0;
            const uint64_t texRevision = (uint64_t(m) << 32) ^ uint64_t(sTexMtxGen[m]);
            h = hash_bytes(h, &texRevision, sizeof(texRevision));
        }
    }
    h = hash_bytes(h, sActiveGLTextures, sizeof(sActiveGLTextures));
    h = hash_bytes(h, sHasActiveTextures, sizeof(sHasActiveTextures));
    // Triangles, strips, fans and quads can all become GL_TRIANGLES and so may
    // share a batch; lines and points cannot mix with them or each other.
    int primClass = 0;
    switch (sCurrentPrimType) {
        case GX_LINES: case GX_LINESTRIP: primClass = 1; break;
        case GX_POINTS:                   primClass = 2; break;
        default:                          primClass = 0; break;
    }
    h = hash_bytes(h, &primClass, sizeof(primClass));
    return h;
}
#else
// Block-cached batch key.
struct VitaKeyParts {
    uint64_t tev, chan, alpha, texgen, mtx;
    uint64_t fog;           // sFogHash
    uint32_t proj;
    GLuint   tex[8];
    uint8_t  has[8];
    int      primClass;
    uint8_t  pretransformed, palette, pad[2];
    uint8_t  stageLo[16];   // per-stage texture alpha lower bounds
    uint8_t  alphaVariant;  // soft / zero-only alpha test bits
    uint8_t  reg0Neg;       // register 0 negative
    uint8_t  pad2[2];
    uint32_t rasBits;       // vertex alpha bound float bits, tested draws only
};

// Fast path inputs.
struct VitaDrawInputs { uint32_t proj; uint64_t mtx; GLuint tex[8]; uint8_t has[8]; int primClass; uint8_t pretransformed, palette, rasLo, pad;
                        uint64_t fog, uv; uint8_t alphaVariant, reg0Neg, pad2[2]; uint32_t texAlphaGen, rasBits; };
static VitaDrawInputs sVitaLastIn;
// Key from its parts.
static inline uint64_t vita_key_fold(const VitaKeyParts& k) {
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full;
    uint64_t t[4], sl[2], has;
    memcpy(t, k.tex, sizeof(t));
    memcpy(sl, k.stageLo, sizeof(sl));
    memcpy(&has, k.has, sizeof(has));
    static_assert(sizeof(k.tex) == sizeof(t) && sizeof(k.stageLo) == sizeof(sl) && sizeof(k.has) == sizeof(has), "key fold word layout");
    a = vita_mix64(a, k.tev);    b = vita_mix64(b, k.chan);
    a = vita_mix64(a, k.alpha);  b = vita_mix64(b, k.texgen);
    a = vita_mix64(a, k.mtx);    b = vita_mix64(b, k.fog);
    a = vita_mix64(a, t[0]);     b = vita_mix64(b, t[1]);
    a = vita_mix64(a, t[2]);     b = vita_mix64(b, t[3]);
    a = vita_mix64(a, has);      b = vita_mix64(b, sl[0]);
    a = vita_mix64(a, sl[1]);
    b = vita_mix64(b, (uint64_t(k.proj) << 32) | (uint64_t(uint8_t(k.primClass)) << 24) | (uint64_t(k.pretransformed) << 16)
                      | (uint64_t(k.palette) << 8) | uint64_t(k.pad[0]));
    a = vita_mix64(a, (uint64_t(k.rasBits) << 32) | (uint64_t(k.pad[1]) << 16) | (uint64_t(k.alphaVariant) << 8) | uint64_t(k.reg0Neg));
    return vita_mix64(vita_mix64(a, b), 0x94D049BB133111EBull);
}
static uint64_t sVitaLastFast = 0;
static bool sVitaLastFastValid = false;
static uint64_t compute_batch_state_key() {
    ++sKeyCalls;
    // Fast path: nothing moved, same key.
    VitaDrawInputs in;
    memset(&in, 0, sizeof(in));
    in.proj = sProjMtxGen;
    if (!sVerticesPretransformed) {
        const u32 id = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
        in.mtx = (uint64_t(id) << 40) ^ (uint64_t(sPosMtxGen[id]) << 20) ^ uint64_t(sNrmMtxGen[id]);
    }
    vita_key_textures(in.tex, in.has, sVitaUsedMapMask);
    switch (sCurrentPrimType) {
        case GX_LINES: case GX_LINESTRIP: in.primClass = 1; break;
        case GX_POINTS:                   in.primClass = 2; break;
        default:                          in.primClass = 0; break;
    }
    const bool elisionReads = !(sAlphaComp0 == GX_ALWAYS && sAlphaComp1 == GX_ALWAYS);
    in.pretransformed = sVerticesPretransformed ? 1 : 0;
    in.palette = vita_palette_bucket(sVitaPaletteSlots);
    in.rasLo = elisionReads ? uint8_t(sVitaRasAlphaMin * 255.0f) : 0;
    if (elisionReads) { memcpy(&in.rasBits, &sVitaRasAlphaMin, 4); in.texAlphaGen = sActiveTexAlphaGen; }
    in.fog = sFogHash;
    in.alphaVariant = uint8_t(vita_alpha_variant_bits());
    in.reg0Neg = sTevRegisters[GX_TEVREG0][0] < 0.0f ? 1 : 0;
    if (sVitaListUvValid && elisionReads) in.uv = sVitaListUvHash;
    if (sVitaLastFastValid && sKeyDirty == 0 && !sChanSrcDirty && memcmp(&in, &sVitaLastIn, sizeof(in)) == 0) return sVitaLastFast;
    bool tevFresh = false;
    uint64_t tevStagePart = 0;
    if (sKeyDirty & kKeyTev) {
        // One walk over the stages.
        bool c1Read = false;
        const int n = sNumTevStages < GX_MAXTEVSTAGE ? int(sNumTevStages) : GX_MAXTEVSTAGE;
        const int fpN = vita_fp_stage_count();   // >= n: the fingerprint always covers stage 0
        uint64_t h = vita_mix64(14695981039346656037ull, uint64_t(sNumTevStages));
        uint64_t f = vita_mix64(14695981039346656037ull, uint64_t(fpN));
        unsigned reads = 0, used = 0;
        for (int i = 0; i < fpN; ++i) {
            const uint64_t w = vita_tev_stage_word(i);
            f = vita_mix64(f, w);
            if (i >= n) continue;
            const TevStageState& st = sTevStages[i];
            h = vita_mix64(h, w);
            // konst selectors
            h = vita_mix64(h, (uint64_t(uint32_t(sKonstColorSel[i])) << 32) | uint64_t(uint32_t(sKonstAlphaSel[i])));
            reads |= vita_tev_stage_reads(st);
            if (st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < 8) used |= 1u << unsigned(st.texMap);
            if (st.rasChannel == 1) c1Read = true;
        }
        h = vita_fp_stage_tail(h);   // swap tables and the register-1 flag
        sKeyBlockTev = hash_tev_constants_m(h, reads);
        sVitaUsedMapMask = used;
        tevStagePart = vita_fp_stage_tail(f);
        tevFresh = true;
        if (c1Read != sVitaChanBlockC1) sKeyDirty |= kKeyChan;
    }
    if (sKeyDirty & kKeyChan) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        // Channel state that reaches the pixels.
        GfxChannel canon[2];
        memcpy(canon, sChannels, sizeof(canon));
        {
            bool chan1Read = false;
            const int stages = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
            for (int i = 0; i < stages; ++i) if (sTevStages[i].rasChannel == 1) { chan1Read = true; break; }
            sVitaChanBlockC1 = chan1Read;
            if (!chan1Read) canon[1] = GfxChannel();
            for (int ch = 0; ch < 2; ++ch) {
                GfxChannel& c = canon[ch];
                if (!c.enabled) { c.ambSrc = GX_SRC_REG; c.lightMask = 0; c.diffFn = GX_DF_NONE; c.attnFn = GX_AF_NONE; c.ambColor[0] = c.ambColor[1] = c.ambColor[2] = 0.0f; }
                if (!c.alphaEnabled) { c.alphaAmbSrc = GX_SRC_REG; c.alphaLightMask = 0; c.alphaDiffFn = GX_DF_NONE; c.alphaAttnFn = GX_AF_NONE; c.ambColor[3] = 0.0f; }
                if (c.matSrc == GX_SRC_VTX) c.matColor[0] = c.matColor[1] = c.matColor[2] = 1.0f;
                if (c.alphaMatSrc == GX_SRC_VTX) c.matColor[3] = 1.0f;
            }
        }
        h = hash_bytes(h, canon, sizeof(canon));
        for (int ch = 0; ch < 2; ++ch) {
            if (!canon[ch].enabled) continue;
            const u32 mask = canon[ch].lightMask;
            for (int i = 0; i < 8; ++i) {
                if (mask & (1u << i)) h = hash_bytes(h, &sLights[i], sizeof(sLights[i]));
            }
        }
        sKeyBlockChan = h;
    }
    if (sKeyDirty & kKeyAlpha) {
        uint64_t h = 0xC2B2AE3D27D4EB4Full;
        h = hash_bytes(h, &sAlphaComp0, sizeof(sAlphaComp0));
        h = hash_bytes(h, &sAlphaComp1, sizeof(sAlphaComp1));
        h = hash_bytes(h, &sAlphaOp, sizeof(sAlphaOp));
        h = hash_bytes(h, &sAlphaRef0, sizeof(sAlphaRef0));
        h = hash_bytes(h, &sAlphaRef1, sizeof(sAlphaRef1));
        sKeyBlockAlpha = h;
    }
    if (sKeyDirty & kKeyTexGen) {
        uint64_t h = 0x51A3B7C9D2E4F601ull;
        for (int slot = 0; slot < 8; ++slot) {
            h = hash_bytes(h, &sTexCoordGen[slot], sizeof(sTexCoordGen[slot]));
            if (sTexCoordGen[slot].active) {
                const u32 m = sTexCoordGen[slot].mtxIdx < 64 ? sTexCoordGen[slot].mtxIdx : 0;
                const uint64_t texRevision = (uint64_t(m) << 32) ^ uint64_t(sTexMtxGen[m]);
                h = hash_bytes(h, &texRevision, sizeof(texRevision));
            }
        }
        sKeyBlockTexGen = h;
    }
    // Program identity refresh.
    if ((sKeyDirty & (kKeyTev | kKeyAlpha)) || sChanSrcDirty) {
        sKeyBlockTevIdent = tevFresh ? vita_fp_finish(tevStagePart) : tev_config_fingerprint();
        sChanSrcDirty = false;
    }
    sKeyDirty = 0;

    VitaKeyParts k;
    memset(&k, 0, sizeof(k));
    k.tev = sKeyBlockTev; k.chan = sKeyBlockChan; k.alpha = sKeyBlockAlpha; k.texgen = sKeyBlockTexGen;
    if (!sVerticesPretransformed) {
        const u32 id = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
        k.mtx = (uint64_t(id) << 40) ^ (uint64_t(sPosMtxGen[id]) << 20) ^ uint64_t(sNrmMtxGen[id]);
    }
    k.fog = in.fog;
    k.alphaVariant = in.alphaVariant;
    k.reg0Neg = in.reg0Neg;
    k.proj = sProjMtxGen;
    vita_key_textures(in.tex, in.has, sVitaUsedMapMask);
    memcpy(k.tex, in.tex, sizeof(k.tex));
    memcpy(k.has, in.has, sizeof(k.has));
    k.primClass = in.primClass;
    k.pretransformed = in.pretransformed;
    k.palette = in.palette;
    k.pad[0] = in.rasLo;      // alpha-test elision input
    k.rasBits = in.rasBits;
    vita_alpha_env_refresh(nullptr);
    sVitaStageLoKeyCall = sKeyCalls;
    memcpy(k.stageLo, sVitaStageLo, sizeof(k.stageLo) < sizeof(sVitaStageLo) ? sizeof(k.stageLo) : sizeof(sVitaStageLo));
    const uint64_t fast = vita_key_fold(k);
    sVitaLastIn = in; sVitaLastFast = fast; sVitaLastFastValid = true;
    return fast;
}
#endif


// ── Primitive batching (PERF-NATIVE-002) ────────────────────────────────────
//
// The port used to emit one glDrawArrays per GX primitive: ~8.000 draws of ten
// vertices per frame, each preceded by the whole uniform block. Measurement
// (see SIGUIENTE_SESION.md) showed consecutive primitives share state in runs
// of ~20, and 60-80 in the heavy frames, so the draws collapse to a few
// hundred.
//
// The scheme rests on one invariant, and every flush point below exists to
// keep it: BETWEEN OPENING A BATCH AND DRAWING IT, NOTHING MAY TOUCH GL STATE
// OR UNIFORMS. The uniforms for a batch are written once, when it opens; the
// draw happens later, and reads whatever the GPU still holds. So a batch must
// be flushed before any GL mutation, and drawn *before* the next batch's
// uniforms are written. Material state needs no flush of its own: it is
// covered by the state key, which is compared on every primitive.
// Batch vertices.
static PcVertexVec sBatchVerts;
static bool     sBatchOpen = false;
static uint64_t sBatchKey  = 0;
static GLenum   sBatchMode = GL_TRIANGLES;
static uint32_t sBatchPrims = 0;   // primitives merged into the open batch

// Everything becomes GL_TRIANGLES (or GL_LINES) so that consecutive primitives
// of different topologies can still share one draw. Order is preserved
// exactly, and so is winding: a strip's odd triangles keep the swap that
// GL_TRIANGLE_STRIP would have applied, or every other face would flip.
#ifdef __vita__
// Vertex count the batch expansion appends.
static inline size_t vita_expand_count(GXPrimitive type, size_t n) {
    switch (type) {
    case GX_TRIANGLES: case GX_LINES: case GX_POINTS: return n;
    case GX_TRIANGLESTRIP: case GX_TRIANGLEFAN:       return n > 2 ? (n - 2) * 3 : 0;
    case GX_QUADS:                                    return (n / 4) * 6;
    case GX_LINESTRIP:                                return n > 1 ? (n - 1) * 2 : 0;
    default:                                          return 0;
    }
}
static void vita_expand_into(GXPrimitive type, const Vertex* v, size_t n, Vertex* d) {
    switch (type) {
    case GX_TRIANGLES: case GX_LINES: case GX_POINTS:
        if (n) memcpy(d, v, n * sizeof(Vertex));
        break;
    case GX_TRIANGLESTRIP:
        for (size_t i = 2; i < n; ++i, d += 3) {
            if ((i & 1) == 0) { d[0] = v[i - 2]; d[1] = v[i - 1]; }
            else              { d[0] = v[i - 1]; d[1] = v[i - 2]; }
            d[2] = v[i];
        }
        break;
    case GX_TRIANGLEFAN:
        for (size_t i = 2; i < n; ++i, d += 3) { d[0] = v[0]; d[1] = v[i - 1]; d[2] = v[i]; }
        break;
    case GX_QUADS:
        for (size_t i = 0; i + 3 < n; i += 4, d += 6) {
            d[0] = v[i]; d[1] = v[i + 1]; d[2] = v[i + 2];
            d[3] = v[i]; d[4] = v[i + 2]; d[5] = v[i + 3];
        }
        break;
    case GX_LINESTRIP:
        for (size_t i = 1; i < n; ++i, d += 2) { d[0] = v[i - 1]; d[1] = v[i]; }
        break;
    default:
        break;
    }
}
#endif
static void append_primitive_to_batch() {
    const PcVertexVec& v = sVertexStream;
    const size_t n = v.size();
#ifdef __vita__
    const size_t cnt = vita_expand_count(sCurrentPrimType, n);
    const size_t base = sBatchVerts.size();
    sBatchVerts.resize(base + cnt);
    vita_expand_into(sCurrentPrimType, v.data(), n, sBatchVerts.data() + base);
#else
    switch (sCurrentPrimType) {
    case GX_TRIANGLES:
        sBatchVerts.insert(sBatchVerts.end(), v.begin(), v.end());
        break;
    case GX_TRIANGLESTRIP:
        for (size_t i = 2; i < n; ++i) {
            if ((i & 1) == 0) {
                sBatchVerts.push_back(v[i - 2]);
                sBatchVerts.push_back(v[i - 1]);
            } else {
                sBatchVerts.push_back(v[i - 1]);
                sBatchVerts.push_back(v[i - 2]);
            }
            sBatchVerts.push_back(v[i]);
        }
        break;
    case GX_TRIANGLEFAN:
        for (size_t i = 2; i < n; ++i) {
            sBatchVerts.push_back(v[0]);
            sBatchVerts.push_back(v[i - 1]);
            sBatchVerts.push_back(v[i]);
        }
        break;
    case GX_QUADS: {
        // Six copies per quad.
        const size_t quads = n / 4;
        const size_t base = sBatchVerts.size();
        sBatchVerts.resize(base + quads * 6);
        Vertex* d = sBatchVerts.data() + base;
        for (size_t i = 0; i + 3 < n; i += 4, d += 6) {
            d[0] = v[i]; d[1] = v[i + 1]; d[2] = v[i + 2];
            d[3] = v[i]; d[4] = v[i + 2]; d[5] = v[i + 3];
        }
        break;
    }
    case GX_LINES:
        sBatchVerts.insert(sBatchVerts.end(), v.begin(), v.end());
        break;
    case GX_LINESTRIP:
        for (size_t i = 1; i < n; ++i) {
            sBatchVerts.push_back(v[i - 1]);
            sBatchVerts.push_back(v[i]);
        }
        break;
    case GX_POINTS:
        sBatchVerts.insert(sBatchVerts.end(), v.begin(), v.end());
        break;
    }
#endif
}

// PIKMIN_BATCH=0 draws every primitive on its own, exactly as before this
// change. It is the A/B for the failure mode this design can produce: a
// missed flush point corrupts pixels in a way that depends on scene content,
// and comparing the two modes in the same scene says in one step whether a
// visual defect belongs to batching or was already there.
static bool batching_enabled() {
    static const bool enabled = [] {
        const char* value = getenv("PIKMIN_BATCH");
        return !(value != nullptr && value[0] == '0');
    }();
    return enabled;
}

static GLenum batch_mode_for(GXPrimitive prim) {
    switch (prim) {
    case GX_LINES: case GX_LINESTRIP: return GL_LINES;
    case GX_POINTS:                   return GL_POINTS;
    default:                          return GL_TRIANGLES;
    }
}

#ifdef __vita__
// Vita vertex submission: VBO ring.
static int      sVitaSeg = 0;           // segment written this frame
static size_t   sVitaSegWrite = 0;      // bytes written into it so far

// Next VBO ring segment.
static void vita_rotate_vbo_segment() {
    sVitaSeg = (sVitaSeg + 1) % kVitaVboSegs;
    sVitaSegWrite = 0;
}

// GXM fast path.
static GLuint      sVitaGxmBound[8] = {};            // textures set via sceGxmSetFragmentTexture
// Reset the GXM texture shadow after the frame's first draw.
static inline void vita_gxm_shadow_after_first_draw() {
    for (int m = 0; m < 8; ++m) sVitaGxmBound[m] = 0;
}
static void vita_gxm_shadow_invalidate() { for (int m = 0; m < 8; ++m) sVitaGxmBound[m] = 0; }
const size_t kVitaAttrOff[8] = {
    offsetof(Vertex, x), offsetof(Vertex, r), offsetof(Vertex, nx),
    offsetof(Vertex, tex), offsetof(Vertex, tex) + 8, offsetof(Vertex, tex) + 16, offsetof(Vertex, tex) + 24,
    offsetof(Vertex, mtxIdx) };
const GLint kVitaAttrSize[8] = { 3, 4, 3, 2, 2, 2, 2, 1 };

// Indexed draw from a cached display list.
static void vita_fast_draw_indexed(GLuint vbo, GLuint ibo, GLsizei firstIndex, GLsizei indexCount) {
    if (indexCount <= 0) return;
    vita_img_flush(sCurrentProgram);
    if (sVitaFrameFirstDraw) {
        // First draw of the frame: vitaGL normal path.
        glBindBuffer_ptr(GL_ARRAY_BUFFER, vbo);
        for (int i = 0; i < 8; ++i)
            glVertexAttribPointer_ptr(GLuint(i), kVitaAttrSize[i], GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                      static_cast<const char*>(nullptr) + kVitaAttrOff[i]);
        glBindBuffer_ptr(GL_ELEMENT_ARRAY_BUFFER, ibo);
        sVitaFrameFirstDraw = false;
        rec_fast_mode(0);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, static_cast<const char*>(nullptr) + size_t(firstIndex) * 2u);
        rec_fast_mode(1);
        vita_gxm_shadow_after_first_draw();
        return;
    }
    rec_fast_draw_indexed(vbo, ibo, indexCount, firstIndex);
}

// Draw count vertices whose vertex 0 is at byteOffset in vbo and at gpuBase.
static void vita_fast_draw(GLuint vbo, size_t byteOffset, const char* gpuBase, GLenum mode, GLsizei count) {
    vita_img_flush(sCurrentProgram);
    if (!sVitaFrameFirstDraw) {
        for (GLsizei start = 0; start < count; start += kVitaMaxDrawVerts) {
            const GLsizei n = (count - start) < kVitaMaxDrawVerts ? (count - start) : kVitaMaxDrawVerts;
            const size_t rel = size_t(start) * sizeof(Vertex);
            const char* vaoBase = (vbo ? static_cast<const char*>(nullptr) + byteOffset : gpuBase) + rel;
            rec_fast_draw_arrays(vbo, vaoBase, gpuBase + rel, mode, n);
        }
        return;
    }
    glBindBuffer_ptr(GL_ARRAY_BUFFER, vbo);
    for (GLsizei start = 0; start < count; start += kVitaMaxDrawVerts) {
        const GLsizei n = (count - start) < kVitaMaxDrawVerts ? (count - start) : kVitaMaxDrawVerts;
        const size_t rel = size_t(start) * sizeof(Vertex);
        const char* vaoBase = (vbo ? static_cast<const char*>(nullptr) + byteOffset : gpuBase) + rel;
        for (int i = 0; i < 8; ++i)
            glVertexAttribPointer_ptr(GLuint(i), kVitaAttrSize[i], GL_FLOAT, GL_FALSE, sizeof(Vertex), vaoBase + kVitaAttrOff[i]);
        if (sVitaFrameFirstDraw) {
            sVitaFrameFirstDraw = false;
            rec_fast_mode(0);
            glDrawArrays(mode, 0, n);
            rec_fast_mode(1);
            vita_gxm_shadow_after_first_draw();
            continue;
        }
        for (int i = 0; i < 8; ++i)
            rec_gxm_vertex_stream_abs(unsigned(i), gpuBase + rel + kVitaAttrOff[i]);
        glDrawArrays(mode, 0, n);
    }
}
#endif


// Draws whatever has accumulated. Safe to call at any time: a no-op when no
// batch is open, which is what makes it cheap to place at every flush point.
void pc_gfx_flush_batch(void) {
    if (!sBatchOpen || sBatchVerts.empty()) {
        sBatchOpen  = false;
        sBatchPrims = 0;
        sBatchVerts.clear();
        return;
    }

    const size_t vertexBytes = sBatchVerts.size() * sizeof(Vertex);
#ifndef __vita__
    if (sVitaDumpActive) {
        float aMin = 1.0f, aMax = 0.0f, rMin = 1.0f, rMax = 0.0f;
        float xMin = 1e30f, xMax = -1e30f, yMin = 1e30f, yMax = -1e30f, zMin = 1e30f, zMax = -1e30f;
        for (const Vertex& v : sBatchVerts) {
            if (v.a < aMin) aMin = v.a; if (v.a > aMax) aMax = v.a;
            if (v.r < rMin) rMin = v.r; if (v.r > rMax) rMax = v.r;
            if (v.x < xMin) xMin = v.x; if (v.x > xMax) xMax = v.x;
            if (v.y < yMin) yMin = v.y; if (v.y > yMax) yMax = v.y;
            if (v.z < zMin) zMin = v.z; if (v.z > zMax) zMax = v.z;
        }
        const char* scope = (sPerfCurrentScope >= 0 && sPerfScopes[sPerfCurrentScope].name)
                            ? sPerfScopes[sPerfCurrentScope].name : "-";
        const Vertex& v0 = sBatchVerts[0];
        printf("[VDUMP] #%u mode=%s n=%u blend=%d(%d,%d) key=%llu alpha=[%.2f..%.2f] red=[%.2f..%.2f] "
               "pos=[%.1f..%.1f %.1f..%.1f %.1f..%.1f] v0=(%.2f %.2f %.2f) tex0=%u model=%s\n",
               sVitaDumpDraw++, sBatchMode == GL_TRIANGLES ? "tri" : sBatchMode == GL_LINES ? "LINE" : "pt",
               (unsigned)sBatchVerts.size(), sVitaBlendType, sVitaBlendSrc, sVitaBlendDst,
               (unsigned long long)sVitaLastKeyHash, aMin, aMax, rMin, rMax,
               xMin, xMax, yMin, yMax, zMin, zMax, v0.x, v0.y, v0.z,
               (unsigned)sActiveGLTextures[0], scope);
    }
#endif
#ifdef __vita__
    // VBO ring.
    const bool useRing = vertexBytes <= kVitaVboSeg - sVitaSegWrite;
    // Lift culling around line and point batches.
    const bool cullLift = sBatchMode != GL_TRIANGLES && sGxCullValid && sGxCullMode != GX_CULL_NONE;
    if (cullLift) glDisable(GL_CULL_FACE);
    if (useRing) {
        const size_t off = size_t(sVitaSeg) * kVitaVboSeg + sVitaSegWrite;   // multiple of sizeof(Vertex)
        glBindBuffer_ptr(GL_ARRAY_BUFFER, sVBO);
        glBufferSubData_ptr(GL_ARRAY_BUFFER, off, vertexBytes, sBatchVerts.data());
        vita_fast_draw(sVBO, off, sVitaRingGpu + off, sBatchMode, (GLsizei)sBatchVerts.size());
        sVitaSegWrite += vertexBytes;
    } else {
        // Overflow: vitaGL scratch pool.
        vita_img_flush(sCurrentProgram);
        if (sVitaFrameFirstDraw) {
            // First draw of the frame: vitaGL normal path.
            sVitaFrameFirstDraw = false;
            rec_fast_mode(0);
            rec_draw_client(sBatchMode, (GLsizei)sBatchVerts.size(), sBatchVerts.data(), vertexBytes);
            rec_fast_mode(1);
            vita_gxm_shadow_after_first_draw();
        } else {
            rec_draw_client(sBatchMode, (GLsizei)sBatchVerts.size(), sBatchVerts.data(), vertexBytes);
        }
    }
    if (cullLift) glEnable(GL_CULL_FACE);
#else
    if (vertexBytes > sVboCapacity - sVboWriteOffset) {
        if (vertexBytes > sVboCapacity) {
            while (sVboCapacity < vertexBytes) sVboCapacity *= 2;
        }
        // Orphaning is safe here: draws already queued retain the old storage.
        glBufferData_ptr(GL_ARRAY_BUFFER, sVboCapacity, nullptr, GL_STREAM_DRAW);
        sVboWriteOffset = 0;
    }
    glBufferSubData_ptr(GL_ARRAY_BUFFER, sVboWriteOffset, vertexBytes, sBatchVerts.data());

    gl_error_checkpoint("batch upload");
    const GLint firstVertex = static_cast<GLint>(sVboWriteOffset / sizeof(Vertex));
    glDrawArrays(sBatchMode, firstVertex, (GLsizei)sBatchVerts.size());
    gl_error_checkpoint("batch draw");

    ++sPerfDraws;
    sPerfVertices += sBatchVerts.size();
    if (sPerfCurrentScope >= 0) {
        ++sPerfScopes[sPerfCurrentScope].draws;
        sPerfScopes[sPerfCurrentScope].vertices += sBatchVerts.size();
    }
#endif
    sVboWriteOffset += vertexBytes;

    sBatchVerts.clear();
    sBatchOpen  = false;
    sBatchPrims = 0;
}

#ifdef __vita__
static uint64_t sVitaAppliedKey = 0;        // batch-state key whose uniforms/textures are bound
static bool     sVitaAppliedKeyValid = false;
static bool     sVitaApplyingForCache = false;   // apply_batch_state runs with no CPU vertices
static void vita_upload_palette_slot0(const float* posColMajor4x4, const float* nrmColMajor3x3);

// Apply memo: two ways per set, most recently used first.
struct VitaApplyMemo {
    uint64_t key = 0; uint64_t keyHash = 0; GLuint program = 0; bool valid = false;
    const ProgramLocations* loc = nullptr;
    bool rejected = false; uint8_t alphaKind = 0xFF;   // alpha-test verdict for this key
};
static VitaApplyMemo sVitaApplyMemo[1024][2];
static inline VitaApplyMemo* vita_apply_memo_set(uint64_t key) { return sVitaApplyMemo[size_t(key ^ (key >> 29)) & 1023u]; }
static VitaApplyMemo* vita_apply_memo_find(uint64_t key) {
    VitaApplyMemo* set = vita_apply_memo_set(key);
    if (set[0].valid && set[0].key == key && set[0].program) return &set[0];
    if (set[1].valid && set[1].key == key && set[1].program) {
        std::swap(set[0], set[1]);   // most recently used first
        return &set[0];
    }
    return nullptr;
}
static void vita_apply_memo_store(uint64_t key) {
    VitaApplyMemo* set = vita_apply_memo_set(key);
    if (!(set[0].valid && set[0].key == key)) {
        if (set[1].valid && set[1].key == key) std::swap(set[0], set[1]);
        else set[1] = set[0];
    }
    VitaApplyMemo& m = set[0];
    m.valid = true; m.key = key; m.keyHash = sVitaLastKeyHash; m.program = sCurrentProgram; m.loc = sLocP;
    m.rejected = sVitaDrawRejected; m.alphaKind = sVitaCurAlphaKind;
}
// Bind what the memo holds for a key.
static void vita_apply_memo_bind(const VitaApplyMemo& m) {
    if (sCurrentProgram != m.program) {
        glUseProgram_ptr(m.program);
        sCurrentProgram = m.program;
        sCheckProgram = m.program;
    }
    sLocP = m.loc;
    sVitaLastKeyHash = m.keyHash;
    sVitaDrawRejected = m.rejected;
    sVitaCurAlphaKind = m.alphaKind;
}
// Memo hit on a program that no longer holds the key's uniforms.
#else
static constexpr bool sVitaApplyingForCache = false;
#endif

// Live GX state for the section writers.
static inline void pc_gx_src_live(VgxSrc& g) {
    g.proj = sProjMatrix;
    g.numStages = int32_t(sNumTevStages);
    g.fastPath = 0;
    g.swapModes = sTevSwapModes;
    g.st = sTevStages;
    g.rasSwap = sTevRasSwapSel;
    g.texSwap = sTevTexSwapSel;
    g.kc = sKonstColorSel;
    g.ka = sKonstAlphaSel;
    g.reg = sTevRegisters;
    g.konst = sKonstColors;
    g.af = nullptr;
    g.ch = sChannels;
    g.light = sLights;
    g.tg = sTexCoordGen;
    g.texMtx = sTexMatrices;
}
// TEV section alpha and fog scalars.
static inline void pc_gx_alpha_fog(VgxAlphaFog& af) {
    af.comp0 = int32_t(sAlphaComp0);
    af.comp1 = int32_t(sAlphaComp1);
    af.op = int32_t(sAlphaOp);
    af.fogImg = 1;
#ifdef __vita__
    af.ref0 = vita_alpha_ref_for_shader(int(sAlphaComp0), sAlphaRef0);
    af.ref1 = vita_alpha_ref_for_shader(int(sAlphaComp1), sAlphaRef1);
    af.fog[0] = sFogK0; af.fog[1] = sFogK1; af.fog[2] = 0.0f; af.fog[3] = 0.0f;
    af.fogA = sFogOn ? 1.0f : 0.0f;
#else
    af.ref0 = sAlphaRef0;
    af.ref1 = sAlphaRef1;
    af.fog[0] = sFogStart; af.fog[1] = sFogEnd; af.fog[2] = sFogNear; af.fog[3] = sFogFar;
    af.fogA = 1.0f;
#endif
    af.fogColour[0] = sFogColour[0]; af.fogColour[1] = sFogColour[1]; af.fogColour[2] = sFogColour[2];
}

#ifdef __vita__
// Uniform sections on the render thread (rtunif).
struct VitaShipValid {
    bool     proj, hdr, swap, reg, konst, af, chan, tg;
    uint8_t  stN, rasN, texN, kcN, kaN;   // prefix blocks: entries [0, n) held
    uint8_t  light;                      // bit i: light i held
    uint64_t texMtx, pos, nrm;           // bit m: matrix m held
};
// posGen / nrmGen / texGen: generation of each committed matrix block
struct VitaShipModel { VgxMirror m; VitaShipValid v; uint32_t posGen[64], nrmGen[64], texGen[64]; };
static VitaShipModel sVitaShip;          // nothing held
static bool     sVitaRtGxXlatFailed = false;  // translator-side error: rtunif off at the present

// Blocks of one record.
struct VitaShipList {
    int n;
    uint32_t bytes;   // blocks with their 4-byte headers
    bool overflow;
    uint8_t id[64], idx[64];
    uint16_t len[64];
    const void* src[64];
};
static inline void vita_ship_add(VitaShipList& L, uint8_t id, uint8_t idx, const void* src, uint32_t len) {
    if (L.n >= 64) { L.overflow = true; return; }
    L.id[L.n] = id; L.idx[L.n] = idx; L.len[L.n] = uint16_t(len); L.src[L.n] = src;
    ++L.n; L.bytes += 4u + len;
}
static inline void vita_ship_single(VitaShipList& L, bool held, const void* model, const void* live, uint32_t len, uint8_t id) {
    if (held && memcmp(model, live, len) == 0) return;
    vita_ship_add(L, id, 0, live, len);
}
static inline void vita_ship_prefix(VitaShipList& L, uint8_t heldN, const void* model, const void* live, uint32_t elem, int n, uint8_t id) {
    if (n <= 0) return;
    if (n <= int(heldN) && memcmp(model, live, uint32_t(n) * elem) == 0) return;
    vita_ship_add(L, id, uint8_t(n), live, uint32_t(n) * elem);
}
static inline void vita_ship_indexed(VitaShipList& L, uint64_t heldBits, const void* model, const void* live, uint32_t len, uint8_t id, u32 idx) {
    if (((heldBits >> idx) & 1u) && memcmp(model, live, len) == 0) return;
    vita_ship_add(L, id, uint8_t(idx), live, len);
}
// Matrix block, held by generation.
static inline void vita_ship_matrix(VitaShipList& L, uint64_t heldBits, uint32_t* modelGen, const uint32_t* liveGen,
                                    const void* model, const void* live, uint32_t len, uint8_t id, u32 idx) {
    if ((heldBits >> idx) & 1u) {
        if (modelGen[idx] == liveGen[idx]) return;
        if (memcmp(model, live, len) == 0) {
            modelGen[idx] = liveGen[idx];
            return;
        }
    }
    vita_ship_add(L, id, uint8_t(idx), live, len);
}
// Commit a shipped record to the model.
static void vita_ship_commit(const VitaShipList& L) {
    VitaShipModel& M = sVitaShip;
    for (int i = 0; i < L.n; ++i) {
        const uint8_t id = L.id[i], idx = L.idx[i];
        void* dst = vgx_block_dst(M.m, id, idx, L.len[i]);
        if (!dst) continue;
        memcpy(dst, L.src[i], L.len[i]);
        switch (id) {
        case kVgxBlkProj:      M.v.proj = true; break;
        case kVgxBlkTevHdr:    M.v.hdr = true; break;
        case kVgxBlkSwapModes: M.v.swap = true; break;
        case kVgxBlkStages:    if (idx > M.v.stN) M.v.stN = idx; break;
        case kVgxBlkRasSwap:   if (idx > M.v.rasN) M.v.rasN = idx; break;
        case kVgxBlkTexSwap:   if (idx > M.v.texN) M.v.texN = idx; break;
        case kVgxBlkKSelC:     if (idx > M.v.kcN) M.v.kcN = idx; break;
        case kVgxBlkKSelA:     if (idx > M.v.kaN) M.v.kaN = idx; break;
        case kVgxBlkReg:       M.v.reg = true; break;
        case kVgxBlkKonst:     M.v.konst = true; break;
        case kVgxBlkAlphaFog:  M.v.af = true; break;
        case kVgxBlkChan:      M.v.chan = true; break;
        case kVgxBlkLight:     M.v.light |= uint8_t(1u << idx); break;
        case kVgxBlkTexGen:    M.v.tg = true; break;
        case kVgxBlkTexMtx:    M.v.texMtx |= 1ull << idx; M.texGen[idx] = sTexMtxGen[idx]; break;   // idx < 64: vgx_block_dst
        case kVgxBlkPos:       M.v.pos |= 1ull << idx; M.posGen[idx] = sPosMtxGen[idx]; break;
        case kVgxBlkNrm:       M.v.nrm |= 1ull << idx; M.nrmGen[idx] = sNrmMtxGen[idx]; break;
        default: break;
        }
    }
}
static inline uint8_t* vita_ship_write_blocks(const VitaShipList& L, uint8_t* p) {
    for (int i = 0; i < L.n; ++i) {
        p[0] = L.id[i]; p[1] = L.idx[i];
        memcpy(p + 2, &L.len[i], 2);
        memcpy(p + 4, L.src[i], L.len[i]);
        p += 4u + L.len[i];
    }
    return p;
}
// Location flags of the table this record is written with.
static uint8_t vita_rtgx_loc_flags(const ProgramLocations& loc) {
    if (vita_rtgx_loc_is(sCurrentProgram, &loc)) return 0;
    if (!sVitaRtGxXlatFailed)
        printf("[VITA RTUNIF] ERROR: program %u is bound with a location table it never registered; that record carries the "
               "table, and rtunif turns off at the next present\n", (unsigned)sCurrentProgram);
    sVitaRtGxXlatFailed = true;
    return kVgxRecInlineLoc;
}

// One apply's record.
static void vita_rtgx_ship_apply(const ProgramLocations& loc, unsigned sections, uint8_t slot0, u32 slot0Idx, const VgxSrc& g) {
    const uint8_t flags = vita_rtgx_loc_flags(loc);
    VitaShipModel& M = sVitaShip;
    VitaShipList L;
    L.n = 0; L.bytes = 0; L.overflow = false;
    VgxTevHdr hdr;
    if (sections & kVgxSecProj)
        vita_ship_single(L, M.v.proj, M.m.proj, g.proj, sizeof(M.m.proj), kVgxBlkProj);
    if ((sections & kVgxSecSlot0) && slot0 == 2) {
        vita_ship_matrix(L, M.v.pos, M.posGen, sPosMtxGen, M.m.pos[slot0Idx], sPosMatrix[slot0Idx], sizeof(M.m.pos[0]), kVgxBlkPos, slot0Idx);
        vita_ship_matrix(L, M.v.nrm, M.nrmGen, sNrmMtxGen, M.m.nrm[slot0Idx], sNrmMatrix[slot0Idx], sizeof(M.m.nrm[0]), kVgxBlkNrm, slot0Idx);
    }
    if (sections & kVgxSecTev) {
        hdr.numStages = g.numStages; hdr.fastPath = g.fastPath;
        vita_ship_single(L, M.v.hdr, &M.m.hdr, &hdr, sizeof(hdr), kVgxBlkTevHdr);
        vita_ship_single(L, M.v.swap, M.m.swapModes, g.swapModes, sizeof(M.m.swapModes), kVgxBlkSwapModes);
        // Stages the section walks.
        const int n = g.numStages < GX_MAXTEVSTAGE ? g.numStages : GX_MAXTEVSTAGE;
        vita_ship_prefix(L, M.v.stN, M.m.st, g.st, sizeof(TevStageState), n, kVgxBlkStages);
        vita_ship_prefix(L, M.v.rasN, M.m.rasSwap, g.rasSwap, sizeof(GXTevSwapSel), n, kVgxBlkRasSwap);
        vita_ship_prefix(L, M.v.texN, M.m.texSwap, g.texSwap, sizeof(GXTevSwapSel), n, kVgxBlkTexSwap);
        vita_ship_prefix(L, M.v.kcN, M.m.kc, g.kc, sizeof(GXTevKColorSel), n, kVgxBlkKSelC);
        vita_ship_prefix(L, M.v.kaN, M.m.ka, g.ka, sizeof(GXTevKAlphaSel), n, kVgxBlkKSelA);
        vita_ship_single(L, M.v.reg, M.m.reg, g.reg, sizeof(M.m.reg), kVgxBlkReg);
        vita_ship_single(L, M.v.konst, M.m.konst, g.konst, sizeof(M.m.konst), kVgxBlkKonst);
        vita_ship_single(L, M.v.af, &M.m.af, g.af, sizeof(M.m.af), kVgxBlkAlphaFog);
    }
    if (sections & kVgxSecChan) {
        vita_ship_single(L, M.v.chan, M.m.ch, g.ch, sizeof(M.m.ch), kVgxBlkChan);
        // Lights an enabled channel's mask selects.
        const u32 lights = (g.ch[0].enabled ? g.ch[0].lightMask : 0u) | (g.ch[1].enabled ? g.ch[1].lightMask : 0u);
        for (u32 i = 0; i < 8; ++i)
            if (lights & (1u << i))
                vita_ship_indexed(L, M.v.light, &M.m.light[i], &g.light[i], sizeof(GfxLight), kVgxBlkLight, i);
    }
    if (sections & kVgxSecTexGen) {
        vita_ship_single(L, M.v.tg, M.m.tg, g.tg, sizeof(M.m.tg), kVgxBlkTexGen);
        uint64_t queued = 0;   // matrices already queued
        for (int slot = 0; slot < 8; ++slot) {
            if (vgx_texgen_mode(g.tg[slot]) == 0 || loc.tcMtx[slot] == -1) continue;
            u32 m = g.tg[slot].mtxIdx;
            if (m >= 64) m = 0;
            if ((queued >> m) & 1u) continue;
            queued |= 1ull << m;
            vita_ship_matrix(L, M.v.texMtx, M.texGen, sTexMtxGen, M.m.texMtx[m], g.texMtx[m], sizeof(M.m.texMtx[0]), kVgxBlkTexMtx, m);
        }
    }
    if (L.overflow) {
        if (!sVitaRtGxXlatFailed) printf("[VITA RTUNIF] ERROR: a record outgrew its block list; rtunif turns off at the next present\n");
        sVitaRtGxXlatFailed = true;
        return;
    }
    const bool inl = (flags & kVgxRecInlineLoc) != 0;
    const uint32_t total = uint32_t(sizeof(VgxApplyRec)) + L.bytes + (inl ? uint32_t(sizeof(ProgramLocations)) : 0u);
    uint8_t* p = static_cast<uint8_t*>(vita_rt_emit(RT_GX_APPLY, total));
    if (!p) {
        if (!sVitaRtGxXlatFailed) printf("[VITA RTUNIF] ERROR: the ring refused an apply record; rtunif turns off at the next present\n");
        sVitaRtGxXlatFailed = true;
        return;
    }
    VgxApplyRec h;
    h.program = uint32_t(sCurrentProgram); h.sections = uint8_t(sections); h.slot0 = slot0; h.slot0Idx = uint8_t(slot0Idx);
    h.flags = flags; h.nBlocks = uint16_t(L.n); h.pad = 0;
    memcpy(p, &h, sizeof(h));
    p = vita_ship_write_blocks(L, p + sizeof(h));
    if (inl) memcpy(p, &loc, sizeof(ProgramLocations));
    vita_ship_commit(L);
}

// Cached list palette upload record.
static bool vita_rtgx_ship_palette(int slots, bool useCurrentMatrix) {
    const ProgramLocations& loc = sLoc;
    const uint8_t flags = vita_rtgx_loc_flags(loc);
    VitaShipModel& M = sVitaShip;
    VitaShipList L;
    L.n = 0; L.bytes = 0; L.overflow = false;
    const u32 current = sCurrentPosMtxId;
    uint64_t queued = 0;
    for (int s = 0; s < slots && s < kVgxMaxPalSlots; ++s) {
        u32 gx = useCurrentMatrix ? current : u32(s) * 3;
        if (gx >= 64) gx = 0;
        if ((queued >> gx) & 1u) continue;
        queued |= 1ull << gx;
        vita_ship_matrix(L, M.v.pos, M.posGen, sPosMtxGen, M.m.pos[gx], sPosMatrix[gx], sizeof(M.m.pos[0]), kVgxBlkPos, gx);
        vita_ship_matrix(L, M.v.nrm, M.nrmGen, sNrmMtxGen, M.m.nrm[gx], sNrmMatrix[gx], sizeof(M.m.nrm[0]), kVgxBlkNrm, gx);
    }
    if (L.overflow) {
        if (!sVitaRtGxXlatFailed) printf("[VITA RTUNIF] ERROR: a palette record outgrew its block list; rtunif turns off at the next present\n");
        sVitaRtGxXlatFailed = true;
        return false;
    }
    const bool inl = (flags & kVgxRecInlineLoc) != 0;
    const uint32_t total = uint32_t(sizeof(VgxPaletteRec)) + L.bytes + (inl ? uint32_t(sizeof(ProgramLocations)) : 0u);
    uint8_t* p = static_cast<uint8_t*>(vita_rt_emit(RT_GX_PALETTE, total));
    if (!p) {
        if (!sVitaRtGxXlatFailed) printf("[VITA RTUNIF] ERROR: the ring refused a palette record; rtunif turns off at the next present\n");
        sVitaRtGxXlatFailed = true;
        return false;
    }
    VgxPaletteRec h;
    h.program = uint32_t(sCurrentProgram); h.slots = uint8_t(slots < 0 ? 0 : slots > 255 ? 255 : slots);
    h.useCurrent = useCurrentMatrix ? 1 : 0; h.flags = flags; h.pad = 0; h.current = current;
    h.nBlocks = uint16_t(L.n); h.pad2 = 0;
    memcpy(p, &h, sizeof(h));
    p = vita_ship_write_blocks(L, p + sizeof(h));
    if (inl) memcpy(p, &loc, sizeof(ProgramLocations));
    vita_ship_commit(L);
    return true;
}

// Turn rtunif off.
static void vita_rtgx_fall_back(const char* why) {
    vita_rt_emit(RT_GX_SYNC_BACK, 0);
    vita_rt_sync();
    sVitaRtUnif = false;
    for (int p = 0; p < kVitaMaxPrograms; ++p) {
        sVitaProgApplied[p].valid = false;
        sVitaProgramAppliedKeyValid[p] = 0;
        VitaUniformImage& im = sVitaImg[p];
        im.lo[0] = im.lo[1] = UINT32_MAX;
        im.hi[0] = im.hi[1] = 0;
    }
    sVitaAppliedKeyValid = false;
    sVitaPalProvValid = false;
    vita_palprog_forget_all();
    invalidate_uniform_cache();
    printf("[VITA RTUNIF] ERROR: %s: the translator writes its own uniforms from here on\n", why);
}
// rtunif per-present step.
static void vita_rtgx_present() {
    if (!sVitaRtUnif) return;
    if (vita_rtgx_failed() || sVitaRtGxXlatFailed) {
        vita_rtgx_fall_back(sVitaRtGxXlatFailed ? "a record could not be shipped" : "the render thread lost a record");
        return;
    }
    if ((++sVitaPalProgPresents & 0xFFFFu) == 0) vita_palprog_forget_all();
    memset(&sVitaShip.v, 0, sizeof(sVitaShip.v));
}

// Translator image of one program stage.
uint8_t* vita_gx_xlat_image(GLuint prog, int stage, uint32_t* size) {
    if (prog == 0 || prog >= GLuint(kVitaMaxPrograms) || stage < 0 || stage > 1) { if (size) *size = 0; return nullptr; }
    VitaUniformImage& im = sVitaImg[prog];
    if (size) *size = im.img[stage] ? im.size[stage] : 0;
    return im.img[stage];
}

// Register one (program, table) with the render thread.
static bool vita_rtgx_register_one(GLuint prog, const ProgramLocations& loc) {
    if (prog == 0 || prog >= GLuint(kVitaMaxPrograms)) return true;
    const VitaUniformImage& im = sVitaImg[prog];
    const uint32_t size[2] = { im.img[0] ? im.size[0] : 0u, im.img[1] ? im.size[1] : 0u };
    return vita_rtgx_register(prog, &loc, size);
}
static void vita_rtgx_init() {
    sVitaRtUnif = false;
    bool ok = true;
    ok = ok && vita_rtgx_register_one(sVitaFbNoatProg, sVitaFbNoatLoc);
    ok = ok && vita_rtgx_register_one(sVitaFbAtProg, sVitaFbAtLoc);
    ok = ok && vita_rtgx_register_one(sVitaFbNoatProgS, sVitaFbNoatLocS);
    ok = ok && vita_rtgx_register_one(sVitaFbAtProgS, sVitaFbAtLocS);
    ok = ok && vita_rtgx_register_one(sShaderProgram, sUberLocations);
    for (size_t i = 0; ok && i < sTevPrograms.size(); ++i) {
        ok = vita_rtgx_register_one(sTevPrograms[i].program, sTevPrograms[i].locations);
    }
    for (size_t i = 0; ok && i < sVitaPairs.size(); ++i) ok = vita_rtgx_register_one(sVitaPairs[i].program, sVitaPairs[i].loc);
    ok = ok && vita_rtgx_registry_ok();
    ok = ok && vita_rtgx_configure(true, uniform_cache_disabled());
    if (!ok) {
        vita_rtgx_configure(false, uniform_cache_disabled());
        printf("[VITA RTUNIF] ERROR: the uniform sections stay on the translator: the registration or its memory did not check out\n");
        return;
    }
    sVitaRtUnif = true;
}
#endif // __vita__

// Program selection, uniforms and texture binds for the current material state.
static void apply_batch_state(bool programBound = false) {
#ifndef __vita__
    const bool tevStats = sPerfStatsEnabled;
#endif
#ifdef __vita__
    if (sKeyDirty) compute_batch_state_key();
#endif
    // Pick the program for this material first: every uniform below is written
    // through sLoc, which describes whichever program is now bound.
    if (!programBound) use_program_for_current_state();
    const ProgramLocations& loc = sLoc;
#ifdef __vita__
    // Block memo for the bound program.
    if (sKeyDirty) compute_batch_state_key();
    VitaProgApplied& pa = sVitaProgApplied[sCurrentProgram < GLuint(kVitaMaxPrograms) ? sCurrentProgram : 0];
    const bool skipTev    = pa.valid && pa.tev == sKeyBlockTev && pa.alpha == sKeyBlockAlpha && pa.fog == sFogHash;
    const bool skipChan   = pa.valid && pa.chan == sKeyBlockChan;
    const bool skipTexGen = pa.valid && pa.texgen == sKeyBlockTexGen;
    const bool skipProj   = pa.valid && pa.proj == sProjMtxGen;
    const bool rtShip    = sVitaRtUnif;
    const bool ownWrites = !sVitaRtUnif;
    unsigned   rtSections = 0;
    uint8_t    rtSlot0 = 0;   // 0 none, 1 identity, 2 matrix rtSlot0Idx
    u32        rtSlot0Idx = 0;
#else
    const bool skipTev = false, skipChan = false, skipTexGen = false, skipProj = false;
    const bool ownWrites = true;
#endif
    VgxSrc src;
    pc_gx_src_live(src);
    VgxAlphaFog alphaFog;
    PcXlatSink xs;

    if (!skipProj) {
#ifdef __vita__
        if (rtShip) rtSections |= kVgxSecProj;
#endif
        if (ownWrites) vgx_write_proj(src, loc, xs);
    }
    static const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
#ifdef __vita__
    // Palette slot 0, except for the display-list cache.
    if (!sVitaApplyingForCache) {
        const u32 mtxId = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
        if (rtShip) {
            sVitaPalProvValid = false;
            vita_palprog_forget(sCurrentProgram);
            rtSections |= kVgxSecSlot0;
            rtSlot0 = sVerticesPretransformed ? 1 : 2;
            rtSlot0Idx = mtxId;
        }
        if (ownWrites)
            vita_upload_palette_slot0(sVerticesPretransformed ? kVgxIdentity16 : sPosMatrix[mtxId],
                                      sVerticesPretransformed ? kVgxIdentityNrm9 : sNrmMatrix[mtxId]);
    }
#else
    glUniformMatrix4fv_ptr(loc.posMtx, 1, GL_FALSE,
                          sVerticesPretransformed ? identity : sPosMatrix[sCurrentPosMtxId]);
#endif
    if (!skipTev) {
    // TEV section fast path.
    int fastPath = 0;
#ifdef __vita__
    if (sNumTevStages == 1 && loc.fastPath != -1) {
#else
    if (sNumTevStages == 1 && (loc.fastPath != -1 || tevStats)) {
#endif
        const TevStageState& st = sTevStages[0];
        const bool simpleOp = st.colorOp == GX_TEV_ADD && st.alphaOp == GX_TEV_ADD
            && st.colorBias == GX_TB_ZERO && st.alphaBias == GX_TB_ZERO
            && st.colorScale == GX_CS_SCALE_1 && st.alphaScale == GX_CS_SCALE_1
            && st.colorOutReg == GX_TEVPREV && st.alphaOutReg == GX_TEVPREV
            && sTevRasSwapSel[0] == GX_TEV_SWAP0 && sTevTexSwapSel[0] == GX_TEV_SWAP0;
        const bool directTexture = st.textureEnabled && st.texMap == GX_TEXMAP0
            && st.texCoord == GX_TEXCOORD0
            && ((st.colorIn[0] == GX_CC_ZERO && st.colorIn[1] == GX_CC_ZERO
                 && st.colorIn[2] == GX_CC_ZERO && st.colorIn[3] == GX_CC_TEXC)
                || (st.colorIn[0] == GX_CC_TEXC && st.colorIn[1] == GX_CC_ZERO
                    && st.colorIn[2] == GX_CC_ZERO && st.colorIn[3] == GX_CC_ZERO))
            && ((st.alphaIn[0] == GX_CA_ZERO && st.alphaIn[1] == GX_CA_ZERO
                 && st.alphaIn[2] == GX_CA_ZERO && st.alphaIn[3] == GX_CA_TEXA)
                || (st.alphaIn[0] == GX_CA_TEXA && st.alphaIn[1] == GX_CA_ZERO
                    && st.alphaIn[2] == GX_CA_ZERO && st.alphaIn[3] == GX_CA_ZERO));
        const bool rasterInD = st.colorIn[0] == GX_CC_ZERO && st.colorIn[1] == GX_CC_ZERO
            && st.colorIn[2] == GX_CC_ZERO && st.colorIn[3] == GX_CC_RASC
            && st.alphaIn[0] == GX_CA_ZERO && st.alphaIn[1] == GX_CA_ZERO
            && st.alphaIn[2] == GX_CA_ZERO && st.alphaIn[3] == GX_CA_RASA;
        // Some original display lists encode the same pass-through as
        // A=RASC, B=C=D=0. With add/bias-zero/scale-one the TEV equation is
        // still exactly RASC (and RASA for alpha).
        const bool rasterInA = st.colorIn[0] == GX_CC_RASC && st.colorIn[1] == GX_CC_ZERO
            && st.colorIn[2] == GX_CC_ZERO && st.colorIn[3] == GX_CC_ZERO
            && st.alphaIn[0] == GX_CA_RASA && st.alphaIn[1] == GX_CA_ZERO
            && st.alphaIn[2] == GX_CA_ZERO && st.alphaIn[3] == GX_CA_ZERO;
        const bool directRaster = !st.textureEnabled && (rasterInD || rasterInA);
        // GX_MODULATE, heavily used by UI glyphs, particles and simple model
        // materials. It is exactly texture * raster for both color and alpha;
        // running the generic TEV loop for every covered pixel is unnecessary.
        const bool modulateTextureRaster = st.textureEnabled && st.texMap == GX_TEXMAP0
            && st.texCoord == GX_TEXCOORD0
            && st.colorIn[0] == GX_CC_ZERO && st.colorIn[1] == GX_CC_TEXC
            && st.colorIn[2] == GX_CC_RASC && st.colorIn[3] == GX_CC_ZERO
            && st.alphaIn[0] == GX_CA_ZERO && st.alphaIn[1] == GX_CA_TEXA
            && st.alphaIn[2] == GX_CA_RASA && st.alphaIn[3] == GX_CA_ZERO;
        if (simpleOp && directTexture) fastPath = 1;
        else if (simpleOp && directRaster) fastPath = 2;
        else if (simpleOp && modulateTextureRaster) fastPath = 3;
    }
    src.fastPath = fastPath;
    pc_gx_alpha_fog(alphaFog);
    src.af = &alphaFog;
#ifdef __vita__
    if (rtShip) rtSections |= kVgxSecTev;
    if (ownWrites) vgx_write_tev(src, loc, xs);
#else
    vgx_write_tev(src, loc, xs);
    if (fastPath != 0) {
        ++sPerfFastDraws;
        ++sPerfFastPathDraws[fastPath];
        sPerfFastPathVertices[fastPath] += sVertexStream.size();
    }
    if (tevStats) {
        sPerfTevStageDraws[std::min<int>(sNumTevStages, GX_MAXTEVSTAGE)]++;
        // One bounded snapshot of each title-scene pipeline is substantially
        // more useful than logging thousands of identical draws. It exposes
        // where CPREV/A1 are produced while keeping the hot path allocation-
        // free and the log small.
        PerfScope* pipelineScope = sPerfCurrentScope >= 0 ? &sPerfScopes[sPerfCurrentScope] : nullptr;
        u8* pipelineReportCount = !pipelineScope ? nullptr
                                : sNumTevStages == 5 ? &pipelineScope->pipeline5ReportCount
                                : sNumTevStages == 7 ? &pipelineScope->pipeline7ReportCount : nullptr;
        if (pipelineReportCount && *pipelineReportCount < 4 && !sVitaApplyingForCache) {
            const unsigned pipelineIndex = unsigned((*pipelineReportCount)++);
            if (pipelineIndex == 0) {
                fprintf(stderr, "[PERF TEVPIPE%u %s] regs", unsigned(sNumTevStages), pipelineScope->name);
                for (int reg = 0; reg < 4; ++reg) {
                    fprintf(stderr, " r%d=(%.3f,%.3f,%.3f,%.3f)", reg,
                            sTevRegisters[reg][0], sTevRegisters[reg][1],
                            sTevRegisters[reg][2], sTevRegisters[reg][3]);
                }
                fprintf(stderr, "\n");
                for (u8 stage = 0; stage < sNumTevStages; ++stage) {
                    const TevStageState& pipe = sTevStages[stage];
                    fprintf(stderr,
                            "[PERF TEVPIPE%u %s] s%u C=%d,%d,%d,%d->%d A=%d,%d,%d,%d->%d tex=%d/%d/%d ras=%d op=%d/%d bias=%d/%d scale=%d/%d clamp=%d/%d\n",
                            unsigned(sNumTevStages), pipelineScope->name, unsigned(stage),
                            int(pipe.colorIn[0]), int(pipe.colorIn[1]), int(pipe.colorIn[2]), int(pipe.colorIn[3]), int(pipe.colorOutReg),
                            int(pipe.alphaIn[0]), int(pipe.alphaIn[1]), int(pipe.alphaIn[2]), int(pipe.alphaIn[3]), int(pipe.alphaOutReg),
                            pipe.textureEnabled ? 1 : 0, int(pipe.texMap), int(pipe.texCoord), pipe.rasChannel,
                            int(pipe.colorOp), int(pipe.alphaOp), int(pipe.colorBias), int(pipe.alphaBias),
                            int(pipe.colorScale), int(pipe.alphaScale), int(pipe.colorClamp), int(pipe.alphaClamp));
                }
            }

            // Identify what this pipeline actually covers before attributing
            // a visual defect to it. Report projected NDC bounds and the UV
            // ranges consumed by its active texture coordinates. This is
            // diagnostic-only and does not alter material state or pixels.
            float ndcMinX = 1.0e30f, ndcMinY = 1.0e30f;
            float ndcMaxX = -1.0e30f, ndcMaxY = -1.0e30f;
            float uvMin[4][2];
            float uvMax[4][2];
            for (int tc = 0; tc < 4; ++tc) {
                uvMin[tc][0] = uvMin[tc][1] = 1.0e30f;
                uvMax[tc][0] = uvMax[tc][1] = -1.0e30f;
            }
            const float* model = sVerticesPretransformed ? identity : sPosMatrix[sCurrentPosMtxId];
            for (const Vertex& vertex : sVertexStream) {
                const float mx = model[0] * vertex.x + model[4] * vertex.y + model[8] * vertex.z + model[12];
                const float my = model[1] * vertex.x + model[5] * vertex.y + model[9] * vertex.z + model[13];
                const float mz = model[2] * vertex.x + model[6] * vertex.y + model[10] * vertex.z + model[14];
                const float mw = model[3] * vertex.x + model[7] * vertex.y + model[11] * vertex.z + model[15];
                const float cx = sProjMatrix[0] * mx + sProjMatrix[4] * my + sProjMatrix[8] * mz + sProjMatrix[12] * mw;
                const float cy = sProjMatrix[1] * mx + sProjMatrix[5] * my + sProjMatrix[9] * mz + sProjMatrix[13] * mw;
                const float cw = sProjMatrix[3] * mx + sProjMatrix[7] * my + sProjMatrix[11] * mz + sProjMatrix[15] * mw;
                if (fabsf(cw) > 1.0e-8f) {
                    const float nx = cx / cw;
                    const float ny = cy / cw;
                    ndcMinX = std::min(ndcMinX, nx);
                    ndcMaxX = std::max(ndcMaxX, nx);
                    ndcMinY = std::min(ndcMinY, ny);
                    ndcMaxY = std::max(ndcMaxY, ny);
                }
                for (int tc = 0; tc < 4; ++tc) {
                    uvMin[tc][0] = std::min(uvMin[tc][0], vertex.tex[tc][0]);
                    uvMax[tc][0] = std::max(uvMax[tc][0], vertex.tex[tc][0]);
                    uvMin[tc][1] = std::min(uvMin[tc][1], vertex.tex[tc][1]);
                    uvMax[tc][1] = std::max(uvMax[tc][1], vertex.tex[tc][1]);
                }
            }
            fprintf(stderr, "[PERF TEVGEOM%u %s #%u] verts=%zu ndc=(%.3f,%.3f)-(%.3f,%.3f)",
                    unsigned(sNumTevStages), pipelineScope->name, pipelineIndex, sVertexStream.size(),
                    ndcMinX, ndcMinY, ndcMaxX, ndcMaxY);
            bool reportedTc[4] = {};
            for (u8 stage = 0; stage < sNumTevStages; ++stage) {
                const int tc = int(sTevStages[stage].texCoord);
                if (sTevStages[stage].textureEnabled && tc >= 0 && tc < 4 && !reportedTc[tc]) {
                    reportedTc[tc] = true;
                    fprintf(stderr, " uv%d=(%.3f,%.3f)-(%.3f,%.3f)", tc,
                            uvMin[tc][0], uvMin[tc][1], uvMax[tc][0], uvMax[tc][1]);
                }
            }
            fprintf(stderr, "\n");
        }
        if (sNumTevStages == 1) {
            const TevStageState& st = sTevStages[0];
            uint64_t key = uint64_t(st.colorIn[0] & 15)
                | (uint64_t(st.colorIn[1] & 15) << 4)
                | (uint64_t(st.colorIn[2] & 15) << 8)
                | (uint64_t(st.colorIn[3] & 15) << 12)
                | (uint64_t(st.alphaIn[0] & 7) << 16)
                | (uint64_t(st.alphaIn[1] & 7) << 19)
                | (uint64_t(st.alphaIn[2] & 7) << 22)
                | (uint64_t(st.alphaIn[3] & 7) << 25)
                | (uint64_t((int(st.texMap) + 1) & 15) << 28)
                | (uint64_t((int(st.texCoord) + 1) & 15) << 32)
                | (uint64_t((st.rasChannel + 1) & 3) << 36)
                | (uint64_t(st.colorOp & 15) << 38)
                | (uint64_t(st.alphaOp & 15) << 42);
            uint64_t flags = uint64_t(st.colorBias & 3)
                | (uint64_t(st.alphaBias & 3) << 2)
                | (uint64_t(st.colorScale & 3) << 4)
                | (uint64_t(st.alphaScale & 3) << 6)
                | (uint64_t(st.colorClamp ? 1 : 0) << 8)
                | (uint64_t(st.alphaClamp ? 1 : 0) << 9)
                | (uint64_t(st.colorOutReg & 3) << 10)
                | (uint64_t(st.alphaOutReg & 3) << 12);
            key |= flags << 46;
            size_t slot = size_t((key ^ (key >> 33) ^ (key >> 17)) & 127);
            for (size_t probe = 0; probe < 128; ++probe) {
                PerfTevPattern& entry = sPerfTevPatterns[(slot + probe) & 127];
                if (entry.count == 0 || entry.key == key) {
                    entry.key = key;
                    entry.count++;
                    break;
                }
            }
        } else if (sNumTevStages > 1) {
            // The final stage must produce TEVPREV, which is the fragment
            // shader output. Grouping its signature exposes broken multi-stage
            // materials without logging every draw or tying diagnostics to a
            // particular model.
            const TevStageState& st = sTevStages[std::min<int>(sNumTevStages, GX_MAXTEVSTAGE) - 1];
            uint64_t key = uint64_t(st.colorIn[0] & 15)
                | (uint64_t(st.colorIn[1] & 15) << 4)
                | (uint64_t(st.colorIn[2] & 15) << 8)
                | (uint64_t(st.colorIn[3] & 15) << 12)
                | (uint64_t(st.alphaIn[0] & 7) << 16)
                | (uint64_t(st.alphaIn[1] & 7) << 19)
                | (uint64_t(st.alphaIn[2] & 7) << 22)
                | (uint64_t(st.alphaIn[3] & 7) << 25)
                | (uint64_t((int(st.texMap) + 1) & 15) << 28)
                | (uint64_t((int(st.texCoord) + 1) & 15) << 32)
                | (uint64_t((st.rasChannel + 1) & 3) << 36)
                | (uint64_t(st.colorOp & 15) << 38)
                | (uint64_t(st.alphaOp & 15) << 42);
            uint64_t flags = uint64_t(st.colorBias & 3)
                | (uint64_t(st.alphaBias & 3) << 2)
                | (uint64_t(st.colorScale & 3) << 4)
                | (uint64_t(st.alphaScale & 3) << 6)
                | (uint64_t(st.colorClamp ? 1 : 0) << 8)
                | (uint64_t(st.alphaClamp ? 1 : 0) << 9)
                | (uint64_t(st.colorOutReg & 3) << 10)
                | (uint64_t(st.alphaOutReg & 3) << 12);
            key |= flags << 46;
            const u8 stages = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
            size_t slot = size_t((key ^ (key >> 33) ^ (key >> 17) ^ stages) & 127);
            for (size_t probe = 0; probe < 128; ++probe) {
                PerfTevMultiPattern& entry = sPerfTevMultiPatterns[(slot + probe) & 127];
                if (entry.count == 0 || (entry.key == key && entry.stages == stages)) {
                    entry.key = key;
                    entry.stages = stages;
                    entry.count++;
                    break;
                }
            }
        }
    }
#endif
    }   // !skipTev
    // Only touch texture units referenced by an active TEV stage. Most Pikmin
    // materials use one map; rebinding all eight for every tiny primitive was
    // a substantial driver overhead. Sampler locations are fixed at init.
    bool usedTextureMaps[8] = {};
    for (u8 stage = 0; stage < sNumTevStages && stage < GX_MAXTEVSTAGE; ++stage) {
        const TevStageState& st = sTevStages[stage];
        if (st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < 8) {
            usedTextureMaps[static_cast<int>(st.texMap)] = true;
        }
    }
    int lastActiveMap = 0;
    for (int map = 0; map < 8; ++map) {
        if (!usedTextureMaps[map]) continue;
        const GLuint wanted = (sHasActiveTextures[map] ? sActiveGLTextures[map] : 0);
        if (sBoundTextures[map] != wanted) {
            glActiveTexture_ptr(GL_TEXTURE0 + map);
            glBindTexture(GL_TEXTURE_2D, wanted);
            sBoundTextures[map] = wanted;
            lastActiveMap = map;
        }
#ifdef __vita__
        // Fast draw mode texture bind.
        if (wanted && !sVitaFrameFirstDraw && sVitaGxmBound[map] != wanted) {
            rec_gxm_fragment_texture(unsigned(map), wanted);
            sVitaGxmBound[map] = wanted;
        }
#endif
    }
    if (lastActiveMap != 0) glActiveTexture_ptr(GL_TEXTURE0);

    // Upload lighting
#ifndef __vita__
    if (loc.nrmMtx != -1) {
        static const float identityNrm[9] = {
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f
        };
        const float* nrmMtx = sVerticesPretransformed
            ? identityNrm
            : sNrmMatrix[sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0];
        glUniformMatrix3fv_ptr(loc.nrmMtx, 1, GL_FALSE, nrmMtx);
    }
#endif
    if (!skipChan) {
#ifdef __vita__
        if (rtShip) rtSections |= kVgxSecChan;
        if (ownWrites) vgx_write_chan(src, loc, xs);
#else
        vgx_write_chan(src, loc, xs);
#endif
    }   // !skipChan

    // Texture coordinate generation for every GX slot. TEX0-TEX7 sources are
    // independent from the destination slot and may use different matrices.
    if (!skipTexGen) {
#ifdef __vita__
        if (rtShip) rtSections |= kVgxSecTexGen;
        if (ownWrites) vgx_write_texgen(src, loc, xs);
#else
        vgx_write_texgen(src, loc, xs);
#endif
    }
#ifdef __vita__
    // rtunif apply record.
    if (rtShip && rtSections) vita_rtgx_ship_apply(loc, rtSections, rtSlot0, rtSlot0Idx, src);
#endif
#ifdef __vita__
    pa.valid = true; pa.tev = sKeyBlockTev; pa.alpha = sKeyBlockAlpha; pa.chan = sKeyBlockChan; pa.texgen = sKeyBlockTexGen; pa.proj = sProjMtxGen; pa.fog = sFogHash;
#else
    switch (sCurrentPrimType) {
        case GX_TRIANGLES: ++sPerfPrimitiveDraws[0]; break;
        case GX_TRIANGLESTRIP: ++sPerfPrimitiveDraws[1]; break;
        case GX_TRIANGLEFAN: ++sPerfPrimitiveDraws[2]; break;
        case GX_LINES: ++sPerfPrimitiveDraws[3]; break;
        case GX_LINESTRIP: ++sPerfPrimitiveDraws[4]; break;
        case GX_POINTS: ++sPerfPrimitiveDraws[5]; break;
        case GX_QUADS: ++sPerfPrimitiveDraws[6]; break;
    }
#endif

    gl_error_checkpoint("uniform upload");
}

#ifdef __vita__
// Constant channel colour folded into the vertices.
static uint64_t sVitaFoldMemoIdent = 0;   // config fingerprint of the memo
static uint64_t sVitaFoldIdent = 0;       // this primitive's fingerprint
static int      sVitaFoldMemoMat1 = -1;
static int      sVitaFoldBaked = -1;   // 0 not baked, 1 baked
static inline float vita_clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
// Every light in the mask is unloaded or black.
static bool vita_lights_dead(u32 mask, bool alpha) {
    for (int i = 0; i < 8; ++i) {
        if (!(mask & (1u << i))) continue;
        const GfxLight& l = sLights[i];
        if (!l.active) continue;
        if (alpha ? (l.color[3] != 0.0f) : (l.color[0] != 0.0f || l.color[1] != 0.0f || l.color[2] != 0.0f)) return false;
    }
    return true;
}
// Material fold outcome.
enum { kVitaMatFoldOk = 0, kVitaMatFoldLit = 1, kVitaMatFoldVtx = 2 };
static int vita_fold_material_values(const GfxChannel& c0, float rgb[3], float& alpha, bool& rgbConst, bool& aConst) {
    rgbConst = false; aConst = false;
    rgb[0] = rgb[1] = rgb[2] = 1.0f; alpha = 1.0f;
    if (c0.matSrc == GX_SRC_REG) {
        if (!c0.enabled) { rgb[0] = c0.matColor[0]; rgb[1] = c0.matColor[1]; rgb[2] = c0.matColor[2]; rgbConst = true; }
        else if (c0.ambSrc == GX_SRC_REG && vita_lights_dead(c0.lightMask, false)) {
            for (int k = 0; k < 3; ++k) rgb[k] = vita_clamp01(c0.matColor[k] * vita_clamp01(c0.ambColor[k]));
            rgbConst = true;
        } else return kVitaMatFoldLit;
    } else if (c0.enabled) return kVitaMatFoldLit;
    if (c0.alphaMatSrc == GX_SRC_REG) {
        if (!c0.alphaEnabled) { alpha = c0.matColor[3]; aConst = true; }
        else if (c0.alphaAmbSrc == GX_SRC_REG && vita_lights_dead(c0.alphaLightMask, true)) {
            alpha = vita_clamp01(c0.matColor[3] * vita_clamp01(c0.ambColor[3]));
            aConst = true;
        } else return kVitaMatFoldLit;
    } else if (c0.alphaEnabled) return kVitaMatFoldLit;
    if (!rgbConst && !aConst) return kVitaMatFoldVtx;   // vertex colours already
    return kVitaMatFoldOk;
}
// Material colour fold.
static bool vita_fold_material_colour(GfxChannel saved[2]) {
    const int stages = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
    for (int i = 0; i < stages; ++i) if (sTevStages[i].rasChannel == 1) return false;
    bool rgbConst = false, aConst = false;
    float rgb[3], alpha;
    switch (vita_fold_material_values(sChannels[0], rgb, alpha, rgbConst, aConst)) {
    case kVitaMatFoldLit: return false;
    case kVitaMatFoldVtx: return false;   // vertex colours already
    default: break;
    }
    // Vertex-colour twin baked?
    memcpy(saved, sChannels, sizeof(sChannels));
    sChannels[0].matSrc = GX_SRC_VTX;
    sChannels[0].alphaMatSrc = GX_SRC_VTX;
    if (sVitaFoldBaked < 0 || sVitaFoldMemoIdent != sVitaFoldIdent) {
        PcTevShaderKey key;
        build_tev_shader_key(key);
        const uint64_t hash = pc_tev_hash_key(key);
        sVitaFoldBaked = sTevProgramByHash.find(hash) != sTevProgramByHash.end() ? 1 : 0;
        if (!sVitaFoldBaked) vita_gx_capture_key(key, hash);
        sVitaFoldMemoIdent = sVitaFoldIdent;
    }
    if (!sVitaFoldBaked) {
        memcpy(sChannels, saved, sizeof(sChannels));
        return false;
    }
    for (Vertex& v : sVertexStream) {
        if (rgbConst) { v.r = rgb[0]; v.g = rgb[1]; v.b = rgb[2]; }
        if (aConst) v.a = alpha;
    }
    // Canonical unlit vertex-colour channel 0.
    GfxChannel& f = sChannels[0];
    f.enabled = false; f.alphaEnabled = false; f.lightMask = 0; f.alphaLightMask = 0;
    f.ambSrc = GX_SRC_REG; f.alphaAmbSrc = GX_SRC_REG;
    f.diffFn = GX_DF_NONE; f.attnFn = GX_AF_NONE; f.alphaDiffFn = GX_DF_NONE; f.alphaAttnFn = GX_AF_NONE;
    for (int k = 0; k < 4; ++k) { f.matColor[k] = 1.0f; f.ambColor[k] = 0.0f; }
    vita_key_dirty(kKeyChan);
    sChanSrcDirty = true;
    sVitaImmEnd.rgbConst = rgbConst ? 1 : 0;
    sVitaImmEnd.aConst = aConst ? 1 : 0;
    return true;
}
// TEV register colour fold.
struct VitaRegFoldSaved { TevStageState stages[GX_MAXTEVSTAGE]; float reg0[4]; float reg1[4]; GfxChannel chan[2]; bool c1Attr; int nStages; };
static uint64_t sVitaRegFoldMemoIdent = 0; static int sVitaRegFoldMemoWhich = -1; static int sVitaRegFoldBaked = -1;
static int sVitaRegFoldLastWhich = 0;   // bit 0: register 0 folded, bit 1: register 1
static uint64_t sVitaRegFoldMemoHash = 0, sVitaRegFoldMemoVh = 0; static int sVitaRegFoldPairOk = 0;   // the twin's hash; its pair with the current vertex key
// Fold registers 0 and 1 into the vertices.
static bool vita_fold_register_colour(VitaRegFoldSaved& saved) {
    const int stages = std::min<int>(sNumTevStages, GX_MAXTEVSTAGE);
    bool reads0 = false, reads1 = false, writes0 = false, writes1 = false;
    for (int i = 0; i < stages; ++i) {
        const TevStageState& st = sTevStages[i];
        if (st.rasChannel == 1) return false;
        if (st.colorOutReg == GX_TEVREG0 || st.alphaOutReg == GX_TEVREG0) writes0 = true;
        if (st.colorOutReg == GX_TEVREG1 || st.alphaOutReg == GX_TEVREG1) writes1 = true;
        for (int a = 0; a < 4; ++a) {
            if (st.colorIn[a] == GX_CC_RASC || st.colorIn[a] == GX_CC_RASA || st.alphaIn[a] == GX_CA_RASA) return false;
            if (st.colorIn[a] == GX_CC_C0 || st.colorIn[a] == GX_CC_A0 || st.alphaIn[a] == GX_CA_A0) reads0 = true;
            if (st.colorIn[a] == GX_CC_C1 || st.colorIn[a] == GX_CC_A1 || st.alphaIn[a] == GX_CA_A1) reads1 = true;
        }
    }
    if (writes0) reads0 = false;   // a stage output, not a constant
    if (writes1) reads1 = false;
    // Register 1 in the normal and matrix-index attributes.
    bool can1 = reads1 && sVitaPaletteSlots <= 1 && !sChannels[1].enabled && !sChannels[1].alphaEnabled;
    // Normal-based texgens some stage samples.
    unsigned tcRead = 0;
    for (int i = 0; i < stages; ++i)
        if (sTevStages[i].textureEnabled && sTevStages[i].texCoord >= GX_TEXCOORD0 && sTevStages[i].texCoord < 8)
            tcRead |= 1u << unsigned(sTevStages[i].texCoord);
    for (int slot = 0; slot < 4 && can1; ++slot)
        if (((tcRead >> slot) & 1u) && sTexCoordGen[slot].active && sTexCoordGen[slot].src == GX_TG_NRM) can1 = false;
    const float* r0 = sTevRegisters[GX_TEVREG0];
    const float* r1 = sTevRegisters[GX_TEVREG1];
    for (int k = 0; k < 4 && reads0; ++k) if (!(r0[k] >= 0.0f && r0[k] <= 1.0f)) reads0 = false;   // outside the attribute's range
    for (int k = 0; k < 4 && can1; ++k) if (!(r1[k] >= 0.0f && r1[k] <= 1.0f)) can1 = false;
    if (!reads0 && !can1) return false;
    const int which = (reads0 ? 1 : 0) | (can1 ? 2 : 0);
    sVitaRegFoldLastWhich = which;
    saved.nStages = stages;
    memcpy(saved.stages, sTevStages, sizeof(TevStageState) * size_t(stages));
    memcpy(saved.reg0, r0, sizeof(saved.reg0));
    memcpy(saved.reg1, r1, sizeof(saved.reg1));
    memcpy(saved.chan, sChannels, sizeof(saved.chan));
    saved.c1Attr = sVitaColor1FromAttr;
    if (reads0) {
        for (int i = 0; i < stages; ++i) {
            TevStageState& st = sTevStages[i];
            for (int a = 0; a < 4; ++a) {
                if (st.colorIn[a] == GX_CC_C0) st.colorIn[a] = GX_CC_RASC;
                else if (st.colorIn[a] == GX_CC_A0) st.colorIn[a] = GX_CC_RASA;
                if (st.alphaIn[a] == GX_CA_A0) st.alphaIn[a] = GX_CA_RASA;
            }
            st.rasChannel = 0;
        }
        // Canonical unlit vertex-colour channel 0.
        GfxChannel& f = sChannels[0];
        f.matSrc = GX_SRC_VTX; f.alphaMatSrc = GX_SRC_VTX;
        f.enabled = false; f.alphaEnabled = false; f.lightMask = 0; f.alphaLightMask = 0;
        f.ambSrc = GX_SRC_REG; f.alphaAmbSrc = GX_SRC_REG;
        f.diffFn = GX_DF_NONE; f.attnFn = GX_AF_NONE; f.alphaDiffFn = GX_DF_NONE; f.alphaAttnFn = GX_AF_NONE;
        for (int k = 0; k < 4; ++k) { f.matColor[k] = 1.0f; f.ambColor[k] = 0.0f; }
    }
    if (can1) sVitaColor1FromAttr = true;
    vita_key_dirty(kKeyTev | kKeyChan);
    sChanSrcDirty = true;
    // Twin baked?
    if (sVitaRegFoldBaked < 0 || sVitaRegFoldMemoIdent != sVitaFoldIdent || sVitaRegFoldMemoWhich != which) {
        PcTevShaderKey key;
        build_tev_shader_key(key);
        const uint64_t hash = pc_tev_hash_key(key);
        sVitaRegFoldBaked = sTevProgramByHash.find(hash) != sTevProgramByHash.end() ? 1 : 0;
        if (!sVitaRegFoldBaked) vita_gx_capture_key(key, hash);
        sVitaRegFoldMemoIdent = sVitaFoldIdent; sVitaRegFoldMemoWhich = which;
        sVitaRegFoldMemoHash = hash; sVitaRegFoldMemoVh = 0; sVitaRegFoldPairOk = 0;
    }
    // Register 1 fold needs a baked pair.
    bool pairOk = true;
    if (sVitaRegFoldBaked && can1) {
        uint64_t vh;
        const VitaVsKey vk = vita_vs_key_and_hash(&vh);
        if (sVitaRegFoldMemoVh != vh) {
            sVitaRegFoldMemoVh = vh;
            sVitaRegFoldPairOk = sVitaPairByKey.find(vita_pair_key(sVitaRegFoldMemoHash, vh)) != sVitaPairByKey.end() ? 1 : 0;
            if (!sVitaRegFoldPairOk) vita_gx_capture_vs_pair(sVitaRegFoldMemoHash, vk);
        }
        pairOk = sVitaRegFoldPairOk != 0;
    }
    if (!sVitaRegFoldBaked || !pairOk) {
        memcpy(sTevStages, saved.stages, sizeof(TevStageState) * size_t(saved.nStages));
        memcpy(sChannels, saved.chan, sizeof(saved.chan));
        sVitaColor1FromAttr = saved.c1Attr;
        vita_key_dirty(kKeyTev | kKeyChan);
        sChanSrcDirty = true;
        return false;
    }
    for (Vertex& v : sVertexStream) {
        if (reads0) { v.r = r0[0]; v.g = r0[1]; v.b = r0[2]; v.a = r0[3]; }
        if (can1)   { v.nx = r1[0]; v.ny = r1[1]; v.nz = r1[2]; v.mtxIdx = r1[3]; }
    }
    // Folded registers hash as constants.
    if (reads0) { float* w = sTevRegisters[GX_TEVREG0]; w[0] = w[1] = w[2] = w[3] = 0.0f; }
    if (can1)   { float* w = sTevRegisters[GX_TEVREG1]; w[0] = w[1] = w[2] = w[3] = 0.0f; }
    return true;
}
static void pc_gfx_end_core(void);
// Fold chain note.
static inline void vita_imm_end_note(uint8_t outcome) {
    sVitaImmEnd.outcome = outcome;
    sVitaImmEnd.reads = vita_tev_reads_mask();
    sVitaImmEnd.reg0Neg = sTevRegisters[GX_TEVREG0][0] < 0.0f ? 1 : 0;
}
// Fold block hash memo.
struct VitaFoldHashMemo { uint64_t key, tev, chan, ident; unsigned used; bool valid; };
static VitaFoldHashMemo sVitaFoldHashMemo[128];
static uint64_t vita_fold_memo_key(uint64_t ident, int which, bool matFold) {
    uint64_t h = 0x7A3D9E1B5C2F8046ull ^ ident;
    const uint8_t tag[2] = { uint8_t(which), uint8_t(matFold ? 1 : 0) };
    h = hash_bytes(h, tag, sizeof(tag));
    h = hash_bytes(h, sTevRegisters[GX_TEVPREV], sizeof(sTevRegisters[0]));
    h = hash_bytes(h, sTevRegisters[GX_TEVREG2], sizeof(sTevRegisters[0]));
    if (!(which & 1)) h = hash_bytes(h, sTevRegisters[GX_TEVREG0], sizeof(sTevRegisters[0]));
    if (!(which & 2)) h = hash_bytes(h, sTevRegisters[GX_TEVREG1], sizeof(sTevRegisters[0]));
    h = hash_bytes(h, sKonstColors, sizeof(sKonstColors));
    const int ns = sNumTevStages < GX_MAXTEVSTAGE ? int(sNumTevStages) : GX_MAXTEVSTAGE;
    h = hash_bytes(h, sKonstColorSel, sizeof(sKonstColorSel[0]) * size_t(ns));
    h = hash_bytes(h, sKonstAlphaSel, sizeof(sKonstAlphaSel[0]) * size_t(ns));
    h = hash_bytes(h, sTevSwapModes, sizeof(sTevSwapModes));
    h = hash_bytes(h, sTevRasSwapSel, sizeof(sTevRasSwapSel[0]) * size_t(ns));
    h = hash_bytes(h, sTevTexSwapSel, sizeof(sTevTexSwapSel[0]) * size_t(ns));
    h = hash_bytes(h, &sNumTevStages, sizeof(sNumTevStages));
    // channel 1 material source
    const uint8_t m1 = sChannels[1].matSrc == GX_SRC_REG ? 1 : 0;
    h = hash_bytes(h, &m1, 1);
    return h;
}
void pc_gfx_end(void) {
    if (sInPrimitive && sHaveVertex) {
        sVertexStream.push_back(sCurVertex);
        sHaveVertex = false;
    }
    GfxChannel saved[2];
    const bool canFold = sInPrimitive && !sVertexStream.empty();
    // Pre-fold block hashes.
    const bool preClean = canFold && (sKeyDirty & (kKeyTev | kKeyChan)) == 0 && !sChanSrcDirty;
    const uint64_t preTev = sKeyBlockTev, preChan = sKeyBlockChan, preIdent = sKeyBlockTevIdent;
    if (canFold) sVitaFoldIdent = (sKeyDirty & (kKeyTev | kKeyChan | kKeyAlpha)) ? tev_config_fingerprint() : sKeyBlockTevIdent;
    const bool folded = canFold && vita_fold_material_colour(saved);
    static VitaRegFoldSaved regSaved;
    const bool regFolded = sInPrimitive && !sVertexStream.empty() && vita_fold_register_colour(regSaved);
    VitaFoldHashMemo* memo = nullptr;
    const int which = regFolded ? sVitaRegFoldLastWhich : 0;
    if (sVitaImmNoting) { sVitaImmEnd.matFold = folded ? 1 : 0; sVitaImmEnd.which = uint8_t(which); }
    if (folded || (which & 1)) {
        const uint64_t mk = vita_fold_memo_key(sVitaFoldIdent, which, folded);
        memo = &sVitaFoldHashMemo[size_t(mk ^ (mk >> 31)) & 127u];
        if (memo->valid && memo->key == mk) {
            sKeyBlockTev = memo->tev; sKeyBlockChan = memo->chan; sKeyBlockTevIdent = memo->ident;
            sVitaUsedMapMask = memo->used;
            sVitaChanBlockC1 = false;
            sKeyDirty &= ~unsigned(kKeyTev | kKeyChan);
            sChanSrcDirty = false;
            sVitaLastFastValid = false;
            memo = nullptr;
        } else { memo->key = mk; memo->valid = false; }
    }
    pc_gfx_end_core();
    if (memo && (sKeyDirty & (kKeyTev | kKeyChan)) == 0 && !sChanSrcDirty) {
        memo->tev = sKeyBlockTev; memo->chan = sKeyBlockChan; memo->ident = sKeyBlockTevIdent;
        memo->used = sVitaUsedMapMask; memo->valid = true;
    }
    if (regFolded) {
        memcpy(sTevStages, regSaved.stages, sizeof(TevStageState) * size_t(regSaved.nStages));
        memcpy(sTevRegisters[GX_TEVREG0], regSaved.reg0, sizeof(regSaved.reg0));
        memcpy(sTevRegisters[GX_TEVREG1], regSaved.reg1, sizeof(regSaved.reg1));
        memcpy(sChannels, regSaved.chan, sizeof(regSaved.chan));
        sVitaColor1FromAttr = regSaved.c1Attr;
    }
    if (folded) memcpy(sChannels, saved, sizeof(sChannels));
    if (folded || regFolded) {
        if (preClean) {
            sKeyBlockTev = preTev; sKeyBlockChan = preChan; sKeyBlockTevIdent = preIdent;
            sKeyDirty &= ~unsigned(kKeyTev | kKeyChan);
            sChanSrcDirty = false;
            sVitaLastFastValid = false;
        } else {
            vita_key_dirty(kKeyTev | kKeyChan);
            sChanSrcDirty = true;
        }
    }
}

// Fold values for a chained primitive.
static bool vita_imm_chain_values(Vertex* v, size_t n) {
    const VitaImmChain& g = g_vitaImmChain;
    float rgb[3] = { 1.0f, 1.0f, 1.0f }, alpha = 1.0f;
    bool rgbC = false, aC = false;
    if (g.matFold) {
        if (vita_fold_material_values(sChannels[0], rgb, alpha, rgbC, aC) != kVitaMatFoldOk) return false;
        if (uint8_t(rgbC ? 1 : 0) != g.rgbConst || uint8_t(aC ? 1 : 0) != g.aConst) return false;
    }
    const float* r0 = sTevRegisters[GX_TEVREG0];
    const float* r1 = sTevRegisters[GX_TEVREG1];
    for (int k = 0; k < 4; ++k) {
        if ((g.which & 1) && !(r0[k] >= 0.0f && r0[k] <= 1.0f)) return false;
        if ((g.which & 2) && !(r1[k] >= 0.0f && r1[k] <= 1.0f)) return false;
    }
    for (size_t i = 0; i < n; ++i) {
        Vertex& x = v[i];
        if (rgbC) { x.r = rgb[0]; x.g = rgb[1]; x.b = rgb[2]; }
        if (aC) x.a = alpha;
        if (g.which & 1) { x.r = r0[0]; x.g = r0[1]; x.b = r0[2]; x.a = r0[3]; }
        if (g.which & 2) { x.nx = r1[0]; x.ny = r1[1]; x.nz = r1[2]; x.mtxIdx = r1[3]; }
    }
    return true;
}
// Fold chain join.
static bool vita_imm_chain_join() {
    if (!vita_imm_chain_values(sVertexStream.data(), sVertexStream.size())) return false;
    append_primitive_to_batch();
    ++sBatchPrims;
    sInPrimitive = false;
    sAttrStep = 0;
    return true;
}
// End of a stream primitive.
static void vita_imm_chain_end(int flags, bool xf) {
    VitaImmChain& g = g_vitaImmChain;
    const bool chainable = flags == 3 && !sVertexStream.empty() && xf && batching_enabled();
    if (chainable && g.armed && sBatchOpen && sVitaBatchSerial == g.serial && sBatchKey == g.key
        && uint32_t(batch_mode_for(sCurrentPrimType)) == g.mode && vita_imm_chain_join())
        return;
    // Full path.
    sVitaImmEnd = VitaImmEndNote();
    sVitaImmNoting = true;
    pc_gfx_end();
    sVitaImmNoting = false;
    const VitaImmEndNote& e = sVitaImmEnd;
    // Arm the chain.
    g.armed = 0;
    if (chainable && (e.outcome == 1 || e.outcome == 2) && sBatchOpen) {
        const GfxChannel& c0 = sChannels[0];
        g.matFold = e.matFold; g.which = e.which; g.rgbConst = e.rgbConst; g.aConst = e.aConst;
        g.reg0Ok = ((e.which & 1) || (!(e.reads & 1u) && !e.reg0Neg)) ? 1 : 0;   // folded, or unread with sign 0
        g.reg1Ok = ((e.which & 2) || !(e.reads & 2u)) ? 1 : 0;                   // folded, or unread
        g.mat0Ok = (e.matFold || (e.which & 1) || (c0.matSrc == GX_SRC_VTX && c0.alphaMatSrc == GX_SRC_VTX)) ? 1 : 0;
        g.amb0Ok = (e.matFold || (e.which & 1) || (!c0.enabled && !c0.alphaEnabled)) ? 1 : 0;
        g.mode = uint32_t(sBatchMode); g.key = sBatchKey; g.serial = sVitaBatchSerial;
        g.armed = 1;
    }
}
static void pc_gfx_end_core(void) {
#else
void pc_gfx_end(void) {
#endif
    if (sInPrimitive && sHaveVertex) {
        sVertexStream.push_back(sCurVertex);
        sHaveVertex = false;
    }
    if (sInPrimitive && sVertexStream.empty() && !sRawPush.empty()) {
        // Raw FIFO bytes primitive decoded as a display list.
        static std::vector<u8> list;
        list.clear();
        list.push_back(u8(u8(sCurrentPrimType) & GX_OPCODE_MASK) | u8(int(sCurVtxFmt) & GX_VAT_MASK));
        list.push_back(u8(sExpectedVerts >> 8));
        list.push_back(u8(sExpectedVerts & 0xFF));
        list.insert(list.end(), sRawPush.begin(), sRawPush.end());
        sRawPush.clear();
        sInPrimitive = false;
        call_display_list_impl(list.data(), u32(list.size()));
        return;
    }
    if (!sInPrimitive || sVertexStream.empty()) {
        sInPrimitive = false;
        return;
    }

    // Capture immutable packet before emitting to OpenGL (disabled - use display list capture)
    // captureCurrentPacket();

    // Same state key: join the open batch.
#ifdef __vita__
    // CPU model-view transform of immediate-mode primitives.
    if (!sVerticesPretransformed) {
        const u32 mtxId = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
        const float* M = sPosMatrix[mtxId];
        const float* N = sNrmMatrix[mtxId];
        for (Vertex& v : sVertexStream) {
            const float x = v.x, y = v.y, z = v.z;
            v.x = M[0] * x + M[4] * y + M[8] * z + M[12];
            v.y = M[1] * x + M[5] * y + M[9] * z + M[13];
            v.z = M[2] * x + M[6] * y + M[10] * z + M[14];
            const float nx = v.nx, ny = v.ny, nz = v.nz;
            v.nx = N[0] * nx + N[3] * ny + N[6] * nz;
            v.ny = N[1] * nx + N[4] * ny + N[7] * nz;
            v.nz = N[2] * nx + N[5] * ny + N[8] * nz;
        }
        sVerticesPretransformed = true;
    }
#endif
    const uint64_t stateKey  = compute_batch_state_key();
    const GLenum   batchMode = batch_mode_for(sCurrentPrimType);

    if (sBatchOpen && batching_enabled() && stateKey == sBatchKey && batchMode == sBatchMode) {
        append_primitive_to_batch();
        ++sBatchPrims;
#ifdef __vita__
        if (sVitaImmNoting) vita_imm_end_note(1);
#endif
        sInPrimitive = false;
        sAttrStep = 0;
        return;
    }

    pc_gfx_flush_batch();

#ifdef __vita__
    sVitaApplyKey = stateKey; sVitaApplyKeyValid = true;
    // Apply memo hit.
    if (VitaApplyMemo* memo = vita_apply_memo_find(stateKey)) {
        vita_apply_memo_bind(*memo);
        apply_batch_state(true);
    } else {
        apply_batch_state();
        vita_apply_memo_store(stateKey);
    }
    sVitaApplyKeyValid = false;
    sVitaAppliedKey = stateKey;
    vita_prog_applied_set(sCurrentProgram, stateKey);
    sVitaAppliedKeyValid = true;
#else
    apply_batch_state();
#endif

    // Uniforms for this state are now on the GPU. Open the batch; the draw
    // happens at the next flush, with these same uniforms still bound.
    sBatchKey   = stateKey;
    sBatchMode  = batchMode;
    sBatchOpen  = true;
    sBatchPrims = 1;
#ifdef __vita__
    ++sVitaBatchSerial;                        // new batch
#endif
    append_primitive_to_batch();
#ifdef __vita__
    if (sVitaImmNoting) vita_imm_end_note(2);
#endif
    if (!batching_enabled()) pc_gfx_flush_batch();

#ifndef __vita__
    static bool reportedFirstDraw = false;
    if (!reportedFirstDraw) {
        GLenum error = glGetError();
        printf("[PC Port] First GX draw: %zu vertices, GL status 0x%04x\n",
               sVertexStream.size(), static_cast<unsigned>(error));
        if (error != GL_NO_ERROR) {
            const char* errStr = "UNKNOWN";
            switch (error) {
                case GL_INVALID_ENUM: errStr = "GL_INVALID_ENUM"; break;
                case GL_INVALID_VALUE: errStr = "GL_INVALID_VALUE"; break;
                case GL_INVALID_OPERATION: errStr = "GL_INVALID_OPERATION"; break;
                case GL_STACK_OVERFLOW: errStr = "GL_STACK_OVERFLOW"; break;
                case GL_STACK_UNDERFLOW: errStr = "GL_STACK_UNDERFLOW"; break;
                case GL_OUT_OF_MEMORY: errStr = "GL_OUT_OF_MEMORY"; break;
                case GL_INVALID_FRAMEBUFFER_OPERATION: errStr = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
            }
            printf("[PC Port] GL Error: %s\n", errStr);
        }
        // Check shader program
        GLint linkStatus;
        glGetProgramiv_ptr(sShaderProgram, GL_LINK_STATUS, &linkStatus);
        if (!linkStatus) {
            char log[4096];
            if (glGetProgramInfoLog_ptr) {
                glGetProgramInfoLog_ptr(sShaderProgram, sizeof(log), NULL, log);
            }
            printf("[PC Port] Shader link error: %s\n", log);
        }
        // Check active uniforms
        GLint numUniforms = 0;
        glGetProgramiv_ptr(sShaderProgram, GL_ACTIVE_UNIFORMS, &numUniforms);
        printf("[PC Port] Active uniforms: %d\n", numUniforms);
        for (int i = 0; i < numUniforms; i++) {
            char name[256];
            GLsizei length;
            GLint size;
            GLenum type;
            if (glGetActiveUniform_ptr) {
                glGetActiveUniform_ptr(sShaderProgram, i, sizeof(name), &length, &size, &type, name);
            } else {
                name[0] = '\0';
            }
            printf("[PC Port] Uniform %d: %s (type 0x%04x)\n", i, name, type);
        }
        const Vertex& first = sVertexStream.front();
        printf("[PC Port] First vertex: (%.2f, %.2f, %.2f); projection scale=(%.4f, %.4f), offset=(%.4f, %.4f); view offset=(%.2f, %.2f, %.2f)\n",
               first.x, first.y, first.z, sProjMatrix[0], sProjMatrix[5],
               sProjMatrix[12], sProjMatrix[13], sPosMatrix[sCurrentPosMtxId][12],
               sPosMatrix[sCurrentPosMtxId][13], sPosMatrix[sCurrentPosMtxId][14]);
        reportedFirstDraw = true;
    }
#endif

    sInPrimitive = false;
    sAttrStep = 0;
}

static bool read_be_u16(const u8*& cursor, const u8* end, u16& value) {
    if (end - cursor < 2) return false;
    value = (u16(cursor[0]) << 8) | cursor[1];
    cursor += 2;
    return true;
}

static bool read_attribute_index(GXAttrType type, const u8*& cursor, const u8* end, u16& index) {
    if (type == GX_INDEX8) {
        if (cursor >= end) return false;
        index = *cursor++;
        return true;
    }
    if (type == GX_INDEX16) return read_be_u16(cursor, end, index);
    return false;
}

static inline float read_attr_float(const u8* ptr, GXCompType type, u8 frac) {
    if (type == GX_F32) {
        float f;
        memcpy(&f, ptr, sizeof(float));
        return f;
    } else if (type == GX_S16) {
        s16 val;
        memcpy(&val, ptr, sizeof(s16));
        return static_cast<float>(val) / static_cast<float>(1 << frac);
    } else if (type == GX_U16) {
        u16 val;
        memcpy(&val, ptr, sizeof(u16));
        return static_cast<float>(val) / static_cast<float>(1 << frac);
    } else if (type == GX_S8) {
        s8 val = static_cast<s8>(*ptr);
        return static_cast<float>(val) / static_cast<float>(1 << frac);
    } else if (type == GX_U8) {
        u8 val = *ptr;
        return static_cast<float>(val) / static_cast<float>(1 << frac);
    }
    return 0.0f;
}

static inline u8 get_comptype_size(GXCompType type) {
    if (type == GX_F32) return 4;
    if (type == GX_S16 || type == GX_U16) return 2;
    return 1;
}

static inline u8 get_color_size(GXCompType type) {
    if (type == GX_RGB565 || type == GX_RGBA4) return 2;
    if (type == GX_RGB8 || type == GX_RGBA6) return 3;
    return 4; // GX_RGBA8 / GX_RGBX8
}

static inline void read_attr_color(const u8* ptr, GXCompType type, u8& r, u8& g, u8& b, u8& a) {
    if (type == GX_RGBA8) {
        r = ptr[0]; g = ptr[1]; b = ptr[2]; a = ptr[3];
    } else if (type == GX_RGBX8) {
        r = ptr[0]; g = ptr[1]; b = ptr[2]; a = 255;   // X = padding, not alpha
    } else if (type == GX_RGB8) {
        r = ptr[0]; g = ptr[1]; b = ptr[2]; a = 255;
    } else if (type == GX_RGBA6) {
        // 24 bits, 6 per channel, MSB first: RRRRRRGG GGGGBBBB BBAAAAAA.
        const u32 v = (u32(ptr[0]) << 16) | (u32(ptr[1]) << 8) | u32(ptr[2]);
        r = (v >> 18) & 0x3F; r = u8((r << 2) | (r >> 4));
        g = (v >> 12) & 0x3F; g = u8((g << 2) | (g >> 4));
        b = (v >> 6)  & 0x3F; b = u8((b << 2) | (b >> 4));
        a =  v        & 0x3F; a = u8((a << 2) | (a >> 4));
    } else if (type == GX_RGB565) {
        u16 val;
        memcpy(&val, ptr, sizeof(u16));
        r = (val >> 11) & 0x1F; r = (r << 3) | (r >> 2);
        g = (val >> 5) & 0x3F;  g = (g << 2) | (g >> 4);
        b = val & 0x1F;         b = (b << 3) | (b >> 2);
        a = 255;
    } else if (type == GX_RGBA4) {
        u16 val;
        memcpy(&val, ptr, sizeof(u16));
        r = (val >> 12) & 0xF; r = (r << 4) | r;
        g = (val >> 8) & 0xF;  g = (g << 4) | g;
        b = (val >> 4) & 0xF;  b = (b << 4) | b;
        a = val & 0xF;         a = (a << 4) | a;
    } else {
        r = 255; g = 255; b = 255; a = 255;
    }
}

static void transform_position(u8 matrixId, float x, float y, float z,
                               float& outX, float& outY, float& outZ) {
    const float* m = sPosMatrix[matrixId < 64 ? matrixId : 0];
    outX = m[0] * x + m[4] * y + m[8] * z + m[12];
    outY = m[1] * x + m[5] * y + m[9] * z + m[13];
    outZ = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static void transform_normal(u8 matrixId, float x, float y, float z,
                             float& outX, float& outY, float& outZ) {
    const float* m = sNrmMatrix[matrixId < 64 ? matrixId : 0];
    outX = m[0] * x + m[3] * y + m[6] * z;
    outY = m[1] * x + m[4] * y + m[7] * z;
    outZ = m[2] * x + m[5] * y + m[8] * z;
}

// Byte size of one vertex attribute when stored directly in the stream.
static u8 attr_inline_size(GXAttr attr, const VertexFormatState& fmt) {
    if (attr >= GX_VA_PNMTXIDX && attr <= GX_VA_TEX7MTXIDX) return 1;
    if (attr == GX_VA_POS) return fmt.count * get_comptype_size(fmt.type);
    if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
        if (fmt.count == GX_NRM_NBT) return 9 * get_comptype_size(fmt.type);
        return 3 * get_comptype_size(fmt.type); // XYZ and NBT3 both lead with 3 values
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) return get_color_size(fmt.type);
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        return (fmt.count == GX_TEX_ST ? 2 : 1) * get_comptype_size(fmt.type);
    }
    return 0;
}

// ── Embedded GP command stream handlers ──
// Display lists carry more than vertex streams: material state (BP registers),
// vertex descriptors (CP registers), matrices/lighting (XF registers) and even
// nested display lists travel inline. Interpreting them here reproduces how
// the game bakes per-material configuration before its geometry.

static bool read_be_u32(const u8*& cursor, const u8* end, u32& value) {
    if (end - cursor < 4) return false;
    value = (u32(cursor[0]) << 24) | (u32(cursor[1]) << 16) | (u32(cursor[2]) << 8) | cursor[3];
    cursor += 4;
    return true;
}

static inline float sext11_to_float(u32 v) {
    v &= 0x7FF;
    if (v & 0x400) v -= 0x800; // sign extend 11-bit
    return static_cast<float>(static_cast<s32>(v)) / 256.0f;
}

static inline float bits_to_float(u32 bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static void handle_bp_reg(u32 hex) {
#ifdef __vita__
    vita_key_dirty(kKeyAll);
#endif
    const u32 reg = (hex >> 24) & 0xFF;
    switch (reg) {
    case 0x41: { // PE color mode: blend/logic operation and write masks
        const bool blend = (hex & 0x1) != 0;
        const bool logic = ((hex >> 1) & 0x1) != 0;
        const bool subtract = ((hex >> 11) & 0x1) != 0;
        GXBlendMode mode = subtract ? GX_BM_SUBTRACT : logic ? GX_BM_LOGIC : blend ? GX_BM_BLEND : GX_BM_NONE;
        pc_gfx_set_blend_mode(mode, GXBlendFactor((hex >> 8) & 0x7),
                              GXBlendFactor((hex >> 5) & 0x7), GXLogicOp((hex >> 12) & 0xF));
        pc_gfx_set_color_update(GXBool((hex >> 3) & 0x1));
        pc_gfx_set_alpha_update(GXBool((hex >> 4) & 0x1));
        break;
    }
    case 0xE0 ... 0xE7: { // TEV/Konst colors: RA and BG word pairs
        // GXTevRegID is PREV=0, REG0=1, REG1=2, REG2=3, exactly matching
        // BP pairs E0/E1 through E6/E7. Bit 23 distinguishes 8-bit Konst
        // writes from signed 11-bit TEV-register writes.
        const u32 pairIdx = (reg - 0xE0) >> 1;
        const bool isRaWord = ((reg - 0xE0) & 1) == 0;
        static u32 pendingRa[4] = {};
        if (isRaWord) {
            pendingRa[pairIdx] = hex & 0xFFFFFF;
        } else {
            const u32 ra = pendingRa[pairIdx];
            const u32 bg = hex & 0xFFFFFF;
            if ((ra & 0x800000) != 0 || (bg & 0x800000) != 0) {
                float* dst = sKonstColors[pairIdx];
                dst[0] = float(ra & 0xFF) / 255.0f;
                dst[1] = float((bg >> 12) & 0xFF) / 255.0f;
                dst[2] = float(bg & 0xFF) / 255.0f;
                dst[3] = float((ra >> 12) & 0xFF) / 255.0f;
            } else {
                float* dst = sTevRegisters[pairIdx];
                dst[0] = sext11_to_float(ra & 0x7FF);
                dst[1] = sext11_to_float((bg >> 12) & 0x7FF);
                dst[2] = sext11_to_float(bg & 0x7FF);
                dst[3] = sext11_to_float((ra >> 12) & 0x7FF);
            }
#ifdef PC_GFX_TRACE
            {
                static FILE* tf = nullptr;
                if (!tf) tf = fopen("/tmp/opencode/tevreg.log", "w");
                if (tf) { fprintf(tf, "[BP ] pair%u RA=%06X BG=%06X kind=%s\n",
                                  pairIdx, ra, bg, (ra & 0x800000) ? "konst" : "tev"); fflush(tf); }
            }
#endif
        }
        break;
    }
    case 0x80 ... 0x87: { // TREF: TEV order, two stages per register
        const u32 base = reg - 0x80;
        const u32 s0 = base * 2, s1 = base * 2 + 1;
        if (s0 < GX_MAXTEVSTAGE) {
            sTevStages[s0].texMap = GXTexMapID(hex & 0x7);
            sTevStages[s0].texCoord = GXTexCoordID((hex >> 3) & 0x7);
            sTevStages[s0].textureEnabled = ((hex >> 6) & 1) != 0;
            const u32 ras = (hex >> 7) & 0x7;
            sTevStages[s0].rasChannel = ras == 7 ? -1 : ras == 1 ? 1 : 0;
        }
        if (s1 < GX_MAXTEVSTAGE) {
            sTevStages[s1].texMap = GXTexMapID((hex >> 12) & 0x7);
            sTevStages[s1].texCoord = GXTexCoordID((hex >> 15) & 0x7);
            sTevStages[s1].textureEnabled = ((hex >> 18) & 1) != 0;
            const u32 ras = (hex >> 19) & 0x7;
            sTevStages[s1].rasChannel = ras == 7 ? -1 : ras == 1 ? 1 : 0;
        }
        break;
    }
    case 0xC0 ... 0xDF: { // TEV combiner environment, color+alpha per stage
        const u32 idx = reg - 0xC0;
        const u32 stage = idx >> 1;
        if (stage >= GX_MAXTEVSTAGE) break;
        TevStageState& st = sTevStages[stage];
        if ((idx & 1) == 0) { // color combiner
            st.colorIn[0] = decode_tev_color_a(hex);
            st.colorIn[1] = decode_tev_color_b(hex);
            st.colorIn[2] = decode_tev_color_c(hex);
            st.colorIn[3] = decode_tev_color_d(hex);
            const u32 biasBits = (hex >> 16) & 0x3;
            const u32 opBit = (hex >> 18) & 0x1;
            const u32 scaleBits = (hex >> 20) & 0x3;
            st.colorOp = biasBits == 3 ? GXTevOp(8 + scaleBits * 2 + opBit)
                                       : (opBit ? GX_TEV_SUB : GX_TEV_ADD);
            st.colorBias = biasBits == 3 ? GX_TB_ZERO : GXTevBias(biasBits);
            st.colorClamp = GXBool((hex >> 19) & 0x1);
            st.colorScale = biasBits == 3 ? GX_CS_SCALE_1 : GXTevScale(scaleBits);
            st.colorOutReg = GXTevRegID((hex >> 22) & 0x3);
        } else { // alpha combiner
            // Bits 0..3 are the texture/raster swap selectors, not alpha inputs.
            sTevRasSwapSel[stage] = GXTevSwapSel(hex & 0x3);
            sTevTexSwapSel[stage] = GXTevSwapSel((hex >> 2) & 0x3);
            st.alphaIn[0] = decode_tev_alpha_a(hex);
            st.alphaIn[1] = decode_tev_alpha_b(hex);
            st.alphaIn[2] = decode_tev_alpha_c(hex);
            st.alphaIn[3] = decode_tev_alpha_d(hex);
            const u32 biasBits = (hex >> 16) & 0x3;
            const u32 opBit = (hex >> 18) & 0x1;
            const u32 scaleBits = (hex >> 20) & 0x3;
            st.alphaOp = biasBits == 3 ? GXTevOp(8 + scaleBits * 2 + opBit)
                                       : (opBit ? GX_TEV_SUB : GX_TEV_ADD);
            st.alphaBias = biasBits == 3 ? GX_TB_ZERO : GXTevBias(biasBits);
            st.alphaClamp = GXBool((hex >> 19) & 0x1);
            st.alphaScale = biasBits == 3 ? GX_CS_SCALE_1 : GXTevScale(scaleBits);
            st.alphaOutReg = GXTevRegID((hex >> 22) & 0x3);
        }
        break;
    }
    case 0xF6 ... 0xFD: { // KSEL: konst selection plus two channels of a swap table
        const u32 index = reg - 0xF6;
        const u32 evenStage = index * 2;
        if (evenStage < GX_MAXTEVSTAGE) {
            sKonstColorSel[evenStage] = GXTevKColorSel((hex >> 4) & 0x1F);
            sKonstAlphaSel[evenStage] = GXTevKAlphaSel((hex >> 9) & 0x1F);
        }
        if (evenStage + 1 < GX_MAXTEVSTAGE) {
            sKonstColorSel[evenStage + 1] = GXTevKColorSel((hex >> 14) & 0x1F);
            sKonstAlphaSel[evenStage + 1] = GXTevKAlphaSel((hex >> 19) & 0x1F);
        }
        TevSwapMode& table = sTevSwapModes[index >> 1];
        if ((index & 1) == 0) {
            table.red = GXTevColorChan(hex & 0x3);
            table.green = GXTevColorChan((hex >> 2) & 0x3);
        } else {
            table.blue = GXTevColorChan(hex & 0x3);
            table.alpha = GXTevColorChan((hex >> 2) & 0x3);
        }
        break;
    }
    case 0xF3: { // PE alpha compare
        pc_gfx_set_alpha_compare(GXCompare((hex >> 16) & 0x7), u8(hex & 0xFF),
                                 GXAlphaOp((hex >> 22) & 0x3),
                                 GXCompare((hex >> 19) & 0x7), u8((hex >> 8) & 0xFF));
        break;
    }
    case 0xFE: { // NumChans/NumTex/NumTev packed update
        sNumTevStages = u8((hex >> 10) & 0xF) + 1;
        break;
    }
    default:
        break;
    }
}

static void handle_xf_regs(u32 addrBase, u32 numWords, const u32* words) {
#ifdef __vita__
    vita_key_dirty(kKeyAll);
#endif
    // Display lists embed whole matrix loads. Position matrices live at
    // XF addresses 12*n (3x4 words each); normal matrices at 0x400 + 9*n
    // (3x3 words each). GX matrix ids are 3*n to match PNMTXIDX values.
    if (addrBase < 0x0180 && numWords == 12 && (addrBase % 12) == 0) {
        const u32 mtxId = addrBase / 4; // 12*n / 4 == 3*n
        if (mtxId < 64) {
            float* dst = sPosMatrix[mtxId];
            // GX stores rows sequentially; OpenGL consumes column-major.
            for (int col = 0; col < 4; ++col) {
                dst[col * 4 + 0] = bits_to_float(words[col]);
                dst[col * 4 + 1] = bits_to_float(words[col + 4]);
                dst[col * 4 + 2] = bits_to_float(words[col + 8]);
            }
            dst[3] = 0.0f; dst[7] = 0.0f; dst[11] = 0.0f; dst[15] = 1.0f;
            ++sPosMtxGen[mtxId];
        }
        return;
    }
    if (addrBase >= 0x0400 && addrBase < 0x0400 + 24 * 9 && numWords == 9 &&
        ((addrBase - 0x0400) % 9) == 0) {
        const u32 mtxId = ((addrBase - 0x0400) / 9) * 3;
        if (mtxId < 64) {
            float* d = sNrmMatrix[mtxId];
            d[0] = bits_to_float(words[0]); d[1] = bits_to_float(words[3]); d[2] = bits_to_float(words[6]);
            d[3] = bits_to_float(words[1]); d[4] = bits_to_float(words[4]); d[5] = bits_to_float(words[7]);
            d[6] = bits_to_float(words[2]); d[7] = bits_to_float(words[5]); d[8] = bits_to_float(words[8]);
            ++sNrmMtxGen[mtxId];
        }
        return;
    }
    for (u32 w = 0; w < numWords; ++w) {
        const u32 addr = addrBase + w;
        const u32 hex = words[w];
        switch (addr) {
        case GX_XF_REG_AMBIENT0: case GX_XF_REG_AMBIENT1: {
            GfxChannel& ch = sChannels[addr - GX_XF_REG_AMBIENT0];
            ch.ambColor[0] = ((hex >> 24) & 0xFF) / 255.0f;
            ch.ambColor[1] = ((hex >> 16) & 0xFF) / 255.0f;
            ch.ambColor[2] = ((hex >> 8) & 0xFF) / 255.0f;
            ch.ambColor[3] = (hex & 0xFF) / 255.0f;
            break;
        }
        case GX_XF_REG_MATERIAL0: case GX_XF_REG_MATERIAL1: {
            GfxChannel& ch = sChannels[addr - GX_XF_REG_MATERIAL0];
            ch.matColor[0] = ((hex >> 24) & 0xFF) / 255.0f;
            ch.matColor[1] = ((hex >> 16) & 0xFF) / 255.0f;
            ch.matColor[2] = ((hex >> 8) & 0xFF) / 255.0f;
            ch.matColor[3] = (hex & 0xFF) / 255.0f;
            break;
        }
        case GX_XF_REG_COLOR0CNTRL: case GX_XF_REG_COLOR1CNTRL: {
            GfxChannel& ch = sChannels[addr - GX_XF_REG_COLOR0CNTRL];
            // GXSetChanCtrl packs these XF fields as follows: material source
            // in bit 0, enable in bit 1, ambient source in bit 6, diffuse in
            // bits 7-8, attenuation in bits 9-10 and the two halves of the
            // light mask in bits 2-5 / 11-14.  These display-list writes are
            // used heavily by model materials, so confusing bit 0 with bit 1
            // silently disabled lighting for any material using GX_SRC_REG.
            decode_xf_channel_control(ch, hex, false);
            break;
        }
        case GX_XF_REG_ALPHA0CNTRL: case GX_XF_REG_ALPHA1CNTRL: {
            GfxChannel& ch = sChannels[addr - GX_XF_REG_ALPHA0CNTRL];
            decode_xf_channel_control(ch, hex, true);
            break;
        }
        default:
            break;
        }
    }
}

#ifdef __vita__
#include "vita_dl_cache.inl"
#else
extern "C" int pc_gx_dl_bounds(const void*, u32, const void*, float*, float*, int*) { return 0; }
#endif
static void call_display_list_impl(const void* list, u32 nbytes);
void pc_gfx_call_display_list(const void* list, u32 nbytes) {
#ifdef __vita__
    g_vitaImmChain.armed = 0;
    if (vita_dl_try_draw(list, nbytes)) return;
#endif
    call_display_list_impl(list, nbytes);
}
static void call_display_list_impl(const void* list, u32 nbytes) {
    // On its own switch rather than the tick profiler's: tying it to
    // PIKMIN_TICK_STATS would put the cost back exactly when measuring, which
    // is the one time it must not be there. Shares the switch with the vertex
    // descriptor history, which exists only to be printed by this report.
    const bool profilingWildVerts = pc_gfx_gx_diagnostics_enabled();
    static bool reportedUnsupportedCommand = false;
    static bool reportedMalformedVertex = false;
    static bool reportedInvalidMatrix = false;
#ifdef __vita__
    g_vitaImmChain.armed = 0;
#endif
    if (!list || nbytes == 0) return;

    // Capture display list if capture is active
    if (sCaptureActive) {
        sPacketStore.captureDisplayList(list, nbytes);
    }

#ifndef __vita__
    ++sPerfDisplayLists;
    sPerfDisplayListBytes += nbytes;
#endif
    const u8* cursor = static_cast<const u8*>(list);
    const u8* end = cursor + nbytes;
    bool pendingTriangleStrip = false;
    auto flushPendingStrip = [&]() {
        if (pendingTriangleStrip) {
            pc_gfx_end();
            pendingTriangleStrip = false;
        }
    };

    while (cursor < end) {
        const u8 command = *cursor++;
        if (command == 0) continue; // 32-byte display-list padding.
        if (command == GX_CMD_NOP) continue;

        // ── Non-draw GP commands: interpret and keep parsing ──
        if (command == GX_CMD_LOAD_BP_REG) {
#ifdef __vita__
            vita_gxq_note_dl_register_write();
#endif
            flushPendingStrip();
            u32 hex;
            if (!read_be_u32(cursor, end, hex)) break;
            handle_bp_reg(hex);
            continue;
        }
        if (command == GX_CMD_LOAD_XF_REG) {
#ifdef __vita__
            vita_gxq_note_dl_register_write();
#endif
            flushPendingStrip();
            u32 header;
            if (!read_be_u32(cursor, end, header)) break;
            const u32 numWords = (header >> 16) & 0xFFFF;
            const u32 addrBase = header & 0xFFFF;
#ifdef PC_GFX_TRACE
            {
                static FILE* xf = nullptr;
                if (!xf) xf = fopen("/tmp/opencode/xfdl.log", "w");
                if (xf) {
                    fprintf(xf, "XF num=%u addr=%04X w0=%08X w1=%08X\n", numWords, addrBase,
                            cursor[0] << 24 | cursor[1] << 16 | cursor[2] << 8 | cursor[3],
                            cursor[4] << 24 | cursor[5] << 16 | cursor[6] << 8 | cursor[7]);
                    fflush(xf);
                }
            }
#endif
            if (end - cursor < ptrdiff_t(numWords * 4)) break;
            handle_xf_regs(addrBase, numWords, reinterpret_cast<const u32*>(cursor));
            cursor += numWords * 4;
            continue;
        }
        if (command == GX_CMD_CALL_DL) {
            flushPendingStrip();
            u32 addr, size;
            if (!read_be_u32(cursor, end, addr) || !read_be_u32(cursor, end, size)) break;
            // Nested display lists: the address is relative to a base pointer
            // tracked by the game. For now we log and continue - the primary
            // issue (flat materials) is fixed by XF/BP handlers above.
            continue;
        }
        if (command == GX_CMD_LOAD_CP_REG || command == GX_CMD_INVL_VC ||
            (command >= GX_CMD_LOAD_INDX_A && command <= GX_CMD_LOAD_INDX_D + 0x07)) {
            flushPendingStrip();
            // CP reg: 1 byte address + 4 byte value. Indexed loads: 4 byte
            // header + payload. Both are safe to skip for rendering state we
            // already track globally.
            u32 skip;
            if (!read_be_u32(cursor, end, skip)) break;
            continue;
        }

        const u8 opcode = command & GX_OPCODE_MASK;
        const u8 format = command & GX_VAT_MASK;
        if (opcode < GX_CMD_DRAW_QUADS || opcode > GX_CMD_DRAW_POINTS) {
            report_desync("unsupported opcode", list,
                          size_t(cursor - static_cast<const u8*>(list) - 1), nbytes);
            flushPendingStrip();
            if (!reportedUnsupportedCommand) {
                fprintf(stderr, "[PC GX] Unsupported display-list command 0x%02X at byte %zu/%u\n",
                        command, size_t(cursor - static_cast<const u8*>(list) - 1), nbytes);
                reportedUnsupportedCommand = true;
            }
            break;
        }

        u16 vertexCount = 0;
        if (!read_be_u16(cursor, end, vertexCount)) break;
        ++sPerfSourcePrimitives;
        const bool continueTriangleStrip = opcode == GX_TRIANGLESTRIP && pendingTriangleStrip;
        if (!continueTriangleStrip) {
            flushPendingStrip();
            pc_gfx_begin(static_cast<GXPrimitive>(opcode), static_cast<GXVtxFmt>(format), vertexCount);
        } else {
            // Join independent strips using degenerate triangles. An extra
            // duplicate for odd-length strips preserves the winding parity of
            // the first real triangle in the following strip.
            const Vertex last = sVertexStream.back();
            if (sVertexStream.size() & 1) sVertexStream.push_back(last);
            sVertexStream.push_back(last);
            sVertexStream.reserve(sVertexStream.size() + vertexCount + 1);
        }
        sVerticesPretransformed = true;

        bool malformed = false;
        for (u16 vertex = 0; vertex < vertexCount && !malformed; ++vertex) {
            const u8* const vertexStart = cursor;
            Vertex v;
            v.x = 0.0f; v.y = 0.0f; v.z = 0.0f;
            v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
            v.r = 1.0f; v.g = 1.0f; v.b = 1.0f; v.a = 1.0f;
#ifdef __vita__
            v.mtxIdx = 0.0f;
#endif
            for (int tc = 0; tc < 4; ++tc) {
                v.tex[tc][0] = 0.0f;
                v.tex[tc][1] = 0.0f;
            }
            u8 matrixId = static_cast<u8>(sCurrentPosMtxId);

            for (int attrNumber = GX_VA_PNMTXIDX; attrNumber <= GX_VA_TEX7; ++attrNumber) {
                GXAttr attr = static_cast<GXAttr>(attrNumber);
                GXAttrType desc = sVtxDesc[attr];
                if (desc == GX_NONE) {
                    // NBT replaces the normal attribute in the hardware stream.
                    if (attr == GX_VA_NRM && sVtxDesc[GX_VA_NBT] != GX_NONE) {
                        attr = GX_VA_NBT;
                        desc = sVtxDesc[attr];
                    } else {
                        continue;
                    }
                }

                if (attr <= GX_VA_TEX7MTXIDX) {
                    if (desc != GX_DIRECT || cursor >= end) { malformed = true; break; }
                    const u8 value = *cursor++;
                    if (attr == GX_VA_PNMTXIDX) {
                        matrixId = value;
                        if (matrixId >= 64 && !reportedInvalidMatrix) {
                            fprintf(stderr, "[PC GX] Invalid PNMTXIDX %u in display list\n", matrixId);
                            reportedInvalidMatrix = true;
                        }
                    }
                    continue;
                }

                const u8* element = nullptr;
                if (desc == GX_DIRECT) {
                    // Inline vertex data embedded in the display list itself.
                    const u8 inlineSize = attr_inline_size(attr, sVtxFormats[format][attr]);
                    if (inlineSize == 0 || end - cursor < inlineSize) { malformed = true; break; }
                    element = cursor;
                    cursor += inlineSize;
                } else {
                    u16 index = 0;
                    if (!read_attribute_index(desc, cursor, end, index)) {
                        malformed = true;
                        break;
                    }
                    const VertexArrayState& array = sVtxArrays[attr];
                    if (!array.base || array.stride == 0) continue;
                    element = array.base + size_t(index) * array.stride;
                    // Only the wild-vertex report below reads these, and this
                    // is the innermost loop in the whole renderer: three stores
                    // per vertex, hundreds of thousands of vertices a frame.
                    if (profilingWildVerts && attr == GX_VA_POS) {
                        sLastPosIndex  = index;
                        sLastPosBase   = array.base;
                        sLastPosStride = array.stride;
                    }
                }

                if (attr == GX_VA_POS) {
                    const VertexFormatState& fmtState = sVtxFormats[format][attr];
                    u8 compSize = get_comptype_size(fmtState.type);
                    float x = read_attr_float(element, fmtState.type, fmtState.frac);
                    float y = read_attr_float(element + compSize, fmtState.type, fmtState.frac);
                    float z = (fmtState.count == GX_POS_XYZ) ? read_attr_float(element + 2 * compSize, fmtState.type, fmtState.frac) : 0.0f;
                    
                    transform_position(matrixId, x, y, z, v.x, v.y, v.z);
                    // The map is a few thousand units across; anything beyond
                    // this, or non-finite, did not come from real model data.
                    // Diagnostic, and it runs per vertex, so it must not run at
                    // all unless asked for: left ungated it cost the title
                    // screen more than half its frame rate.
                    if (profilingWildVerts
                        && (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)
                            || fabsf(v.x) > 1.0e6f || fabsf(v.y) > 1.0e6f || fabsf(v.z) > 1.0e6f)) {
                        // Separate the two possible sources: the model data we
                        // read, or the matrix we multiplied it by. Printing
                        // both says which one is garbage without guessing.
                        static int reports = 0;
                        if (reports < 6) {
                            ++reports;
                            const float* m = sPosMatrix[matrixId < 64 ? matrixId : 0];
                            fprintf(stderr,
                                    "[PC GX] WILD #%d mtx=%u in=(%g,%g,%g) out=(%g,%g,%g)\n",
                                    reports, unsigned(matrixId), x, y, z, v.x, v.y, v.z);
                            (void)m;
                            const VertexFormatState& pf = sVtxFormats[format][GX_VA_POS];
                            fprintf(stderr,
                                    "[PC GX] WILD #%d pos idx=%u base=%p stride=%u -> elem=%p (offset %zu)  fmt: count=%d type=%d frac=%u\n",
                                    reports, unsigned(sLastPosIndex), (const void*)sLastPosBase,
                                    unsigned(sLastPosStride),
                                    (const void*)(sLastPosBase + size_t(sLastPosIndex) * sLastPosStride),
                                    size_t(sLastPosIndex) * sLastPosStride,
                                    int(pf.count), int(pf.type), unsigned(pf.frac));
                            fprintf(stderr,
                                    "[PC GX] WILD #%d POS array last set on frame %llu (now %llu, %llu frames ago); set #%llu of %llu total\n",
                                    reports,
                                    (unsigned long long)sArraySetFrame[GX_VA_POS],
                                    (unsigned long long)sFrameSerial,
                                    (unsigned long long)(sFrameSerial - sArraySetFrame[GX_VA_POS]),
                                    (unsigned long long)sArraySetSerial[GX_VA_POS],
                                    (unsigned long long)sArraySetCounter);
                            // The array and the format are right, so the
                            // remaining suspect is the layout we are stepping
                            // through. Print the descriptor and the actual
                            // stream bytes for this vertex so the true stride
                            // can be read off by hand.
                            fprintf(stderr, "[PC GX] WILD #%d vtxdesc:", reports);
                            unsigned wstride = 0;
                            for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
                                const GXAttrType d = sVtxDesc[a];
                                if (d == GX_NONE) continue;
                                const char* kind = d == GX_DIRECT ? "direct" : d == GX_INDEX8 ? "idx8"
                                                 : d == GX_INDEX16 ? "idx16" : "?";
                                unsigned bytes = d == GX_DIRECT
                                    ? attr_inline_size(static_cast<GXAttr>(a), sVtxFormats[format][a])
                                    : (d == GX_INDEX8 ? 1u : 2u);
                                wstride += bytes;
                                fprintf(stderr, " a%d=%s(%u)", a, kind, bytes);
                            }
                            fprintf(stderr, "  stride=%u  vat=%u  vert %u/%u at list byte %zu/%u\n",
                                    wstride, unsigned(format), unsigned(vertex), unsigned(vertexCount),
                                    size_t(vertexStart - static_cast<const u8*>(list)), nbytes);
                            fprintf(stderr, "[PC GX] WILD #%d stream:", reports);
                            for (int b = 0; b < 20 && vertexStart + b < end; ++b) {
                                fprintf(stderr, " %02X", vertexStart[b]);
                            }
                            fprintf(stderr, "\n");
                            if (reports == 1) {
                                pc_gfx_dump_vtx_desc_history("first wild vertex");
                                // The same list parses correctly at 30 Hz, so
                                // the descriptor is not wrong for the mesh --
                                // this list is reaching us from a draw path
                                // that never programmed one. Name that path
                                // instead of inferring it.
#if defined(_WIN32) || defined(__vita__)
                                fprintf(stderr, "[PC GX] wild draw call path: "
                                                "unavailable on this platform\n");
#else
                                void* frames[16];
                                const int n = backtrace(frames, 16);
                                char** names = backtrace_symbols(frames, n);
                                fprintf(stderr, "[PC GX] wild draw call path:\n");
                                for (int f = 0; f < n; ++f) {
                                    fprintf(stderr, "    %s\n", names ? names[f] : "?");
                                }
                                free(names);
#endif
                            }
                            fprintf(stderr, "[PC GX] WILD #%d raw:", reports);
                            for (int b = 0; b < 12; ++b) fprintf(stderr, " %02X", element[b]);
                            fprintf(stderr, "\n");
                        }
                    }
                } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                    const VertexFormatState& fmtState = sVtxFormats[format][attr];
                    u8 r, g, b, a;
                    read_attr_color(element, fmtState.type, r, g, b, a);
                    if (attr == GX_VA_CLR0) {
                        v.r = r / 255.0f;
                        v.g = g / 255.0f;
                        v.b = b / 255.0f;
                        v.a = a / 255.0f;
                    }
                } else if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
                    const VertexFormatState& fmtState = sVtxFormats[format][attr];
                    u8 compSize = get_comptype_size(fmtState.type);
                    // Every normal encoding (XYZ, NBT, NBT3) stores the normal
                    // vector in the first three slots; NBT/NBT3 just append
                    // binormal/tangent data we can safely ignore.
                    v.nx = read_attr_float(element, fmtState.type, fmtState.frac);
                    v.ny = read_attr_float(element + compSize, fmtState.type, fmtState.frac);
                    v.nz = read_attr_float(element + 2 * compSize, fmtState.type, fmtState.frac);
                } else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
                    const VertexFormatState& fmtState = sVtxFormats[format][attr];
                    u8 compSize = get_comptype_size(fmtState.type);
                    const int tc = int(attr) - int(GX_VA_TEX0);
                    // The parser must consume all eight GX attributes, while
                    // the current shader backend only exposes TEX0-TEX3.
                    if (tc < 4) {
                        v.tex[tc][0] = read_attr_float(element, fmtState.type, fmtState.frac);
                        v.tex[tc][1] = (fmtState.count == GX_TEX_ST)
                            ? read_attr_float(element + compSize, fmtState.type, fmtState.frac) : 0.0f;
                    }
                }
            }

            // Display-list vertices are transformed to view space on the CPU
            // so that a primitive can contain several PNMTXIDX values.  Their
            // normals must follow the same per-vertex matrix; applying one
            // global normal matrix later gives skinned joints unrelated light
            // directions (notably Olimar/Pikmin heads and articulated ships).
            float nx, ny, nz;
            transform_normal(matrixId, v.nx, v.ny, v.nz, nx, ny, nz);
            v.nx = nx;
            v.ny = ny;
            v.nz = nz;

            if (continueTriangleStrip && vertex == 0) sVertexStream.push_back(v);
            sVertexStream.push_back(v);
        }

        if (malformed) {
            report_desync("truncated vertex", list,
                          size_t(cursor - static_cast<const u8*>(list)), nbytes);
            if (!reportedMalformedVertex) {
                fprintf(stderr, "[PC GX] Malformed vertex stream at byte %zu/%u (format %u)\n",
                        size_t(cursor - static_cast<const u8*>(list)), nbytes, format);
                reportedMalformedVertex = true;
            }
            sInPrimitive = false;
            sVertexStream.clear();
            pendingTriangleStrip = false;
            break;
        }
        if (opcode == GX_TRIANGLESTRIP) pendingTriangleStrip = true;
        else pc_gfx_end();
    }
    flushPendingStrip();
}

void pc_gfx_copy_disp(void* dest, GXBool clear) {
    pc_gfx_note_gl_state_change();
    (void)dest;
    if (clear) {
        const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(sCopyClearColor[0], sCopyClearColor[1], sCopyClearColor[2], sCopyClearColor[3]);
        glClearDepth(sCopyClearDepth);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST);
    }
}

void pc_gfx_set_copy_clear(GXColor color, u32 clearZ) {
    sCopyClearColor[0] = color.r / 255.0f;
    sCopyClearColor[1] = color.g / 255.0f;
    sCopyClearColor[2] = color.b / 255.0f;
    sCopyClearColor[3] = color.a / 255.0f;
    sCopyClearDepth = (clearZ & 0x00ffffffu) / 16777215.0;
    // Keep the replay clear in sync with the authoritative clear color.
    sReplayClearColor[0] = sCopyClearColor[0];
    sReplayClearColor[1] = sCopyClearColor[1];
    sReplayClearColor[2] = sCopyClearColor[2];
    sReplayClearColor[3] = sCopyClearColor[3];
    sReplayClearDepth    = sCopyClearDepth;
}
