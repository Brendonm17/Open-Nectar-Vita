// PS Vita render thread: command queue replayed into vitaGL
#include "vita_rt.h"
#include "vita_platform.h"

#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include "timing/pc_prof.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Queue
static constexpr int      kChunks    = 3;
static constexpr uint32_t kChunkSize = 6u * 1024u * 1024u;   // max payload is this minus a header
struct RtHeader { uint16_t op; uint16_t pad; uint32_t size; };  // size: payload bytes, pad: kRtMagic
// Ring header magic
static constexpr uint16_t kRtMagic = 0x5A3C;
struct RtChunk  { uint8_t* data = nullptr; uint32_t used = 0; uint32_t presents = 0, syncs = 0; };
static RtChunk  sChunks[kChunks];
static int      sWriteChunk = 0;         // game thread
// Frames in flight per boundary
extern "C" int vita_gxq_pipe_depth(void);
static SceUID   sSemaReady = -1, sSemaFree = -1, sSemaFrame = -1, sSemaSync = -1;
static SceUID   sThread = -1;
static volatile bool sRecording = false;
static volatile bool sReady = false;
static volatile unsigned sVramFreeKb = 0;   // free VRAM at the last present
// First-occurrence log gate
static inline bool rt_first_time(unsigned& said) {
    if (said) return false;
    said = 1u;
    return true;
}

// Streamed textures
struct RtStreamSlot { GLuint id; uint32_t w, h, bytes; void* buf[2]; int next; };
static RtStreamSlot sStreamSlots[4] = {};
static GLuint sRtBound2D = 0;   // last replayed GL_TEXTURE_2D binding
static void rt_stream_free(RtStreamSlot& e) {
    if (e.buf[0]) vglFree(e.buf[0]);
    if (e.buf[1]) vglFree(e.buf[1]);
    e.buf[0] = e.buf[1] = nullptr; e.id = 0; e.w = e.h = e.bytes = 0; e.next = 0;
}
static void rt_stream_release(GLuint id) {
    for (RtStreamSlot& e : sStreamSlots) if (e.id == id) rt_stream_free(e);
}
static void rt_check_texture_upload(int w, int h, uint32_t fmt, uint32_t bytes) {
    const GLenum e = glGetError();
    if (e == GL_NO_ERROR) return;
    printf("ERROR: texture upload failed (GL 0x%x): %dx%d fmt 0x%x, %u bytes; VRAM free %u KB, vitaGL RAM free %u KB\n",
           (unsigned)e, w, h, (unsigned)fmt, (unsigned)bytes,
           (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024));
}
static bool rt_stream_texture(uint32_t w, uint32_t h, uint32_t fmt, uint32_t ty, uint32_t bytes, const void* data) {
    if (fmt != GL_LUMINANCE_ALPHA || ty != GL_UNSIGNED_BYTE || !data || bytes < 65536u || (w & 7u) || bytes != w * h * 2u) return false;
    SceGxmTexture* gt = vglGetGxmTexture(GL_TEXTURE_2D);
    if (!gt || sceGxmTextureGetWidth(gt) != w || sceGxmTextureGetHeight(gt) != h) return false;   // first upload
    if (sceGxmTextureGetFormat(gt) != SCE_GXM_TEXTURE_FORMAT_A8L8 || sceGxmTextureGetType(gt) != SCE_GXM_TEXTURE_LINEAR) return false;
    RtStreamSlot* slot = nullptr; RtStreamSlot* spare = nullptr;
    for (RtStreamSlot& e : sStreamSlots) { if (e.id && e.id == sRtBound2D) slot = &e; else if (!e.id && !spare) spare = &e; }
    if (slot && slot->bytes != bytes) rt_stream_free(*slot), slot = nullptr;
    if (!slot) {
        if (!spare) return false;
        slot = spare;
        slot->buf[0] = vglAlloc(bytes, VGL_MEM_VRAM);
        slot->buf[1] = vglAlloc(bytes, VGL_MEM_VRAM);
        if (!slot->buf[0] || !slot->buf[1]) {
            static unsigned said = 0;
            rt_stream_free(*slot);
            if (rt_first_time(said)) printf("[VITA RT] stream: no VRAM for 2x%u bytes\n", (unsigned)bytes);
            return false;
        }
        slot->id = sRtBound2D; slot->w = w; slot->h = h; slot->bytes = bytes; slot->next = 0;
    }
    void* dst = slot->buf[slot->next]; slot->next ^= 1;
    memcpy(dst, data, bytes);
    sceGxmTextureSetData(gt, dst);
    return true;
}
unsigned vita_rt_vram_free_kb(void) { return sVramFreeKb; }

// Id pools
static constexpr int kPool = 4096;
static GLuint sTexPool[kPool]; static int sTexTop = 0;
static GLuint sBufPool[kPool]; static int sBufTop = 0;
static GLuint sTexRecycle[kPool]; static int sTexRecycleTop = 0;
static GLuint sBufRecycle[kPool]; static int sBufRecycleTop = 0;

static inline uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

bool vita_rt_recording(void) { return sRecording; }

static void close_chunk() {
    RtChunk& c = sChunks[sWriteChunk];
    if (c.used == 0) return;
    sceKernelSignalSema(sSemaReady, 1);
    sWriteChunk = (sWriteChunk + 1) % kChunks;
    { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaFree, 1, NULL); pc_prof_idle_set(pvIdle); }
    RtChunk& w = sChunks[sWriteChunk];
    w.used = 0; w.presents = 0; w.syncs = 0;
}

void* vita_rt_emit(VitaRtOp op, size_t payloadBytes) {
    if (!sRecording) return nullptr;
    const uint32_t need = uint32_t(sizeof(RtHeader)) + align4(uint32_t(payloadBytes));
    if (need > kChunkSize) {
        static unsigned said = 0;
        if (rt_first_time(said)) printf("[VITA RT] payload too large: %u bytes (op %u)\n", (unsigned)payloadBytes, (unsigned)op);
        return nullptr;
    }
    RtChunk* c = &sChunks[sWriteChunk];
    if (c->used + need > kChunkSize) {
        close_chunk();
        c = &sChunks[sWriteChunk];
    }
    RtHeader* h = reinterpret_cast<RtHeader*>(c->data + c->used);
    h->op = op; h->pad = kRtMagic; h->size = align4(uint32_t(payloadBytes));
    void* payload = c->data + c->used + sizeof(RtHeader);
    c->used += need;
    if (op == RT_PRESENT) ++c->presents; else if (op == RT_SYNC || op == RT_INIT_GFX) ++c->syncs;
    return payload;
}

void vita_rt_present(void) {
    if (!sRecording) return;
    vita_rt_emit(RT_PRESENT, 0);
    close_chunk();
    // One frame in flight
    { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaFrame, 1, NULL); pc_prof_idle_set(pvIdle); }
}

void vita_rt_sync(void) {
    if (!sRecording) return;
    vita_rt_emit(RT_SYNC, 0);
    close_chunk();
    { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaSync, 1, NULL); pc_prof_idle_set(pvIdle); }
}

GLuint vita_rt_alloc_texture_id(void) {
    if (sTexRecycleTop > 0) return sTexRecycle[--sTexRecycleTop];
    if (sTexTop < kPool) return sTexPool[sTexTop++];
    static unsigned said = 0;
    if (rt_first_time(said)) printf("[VITA RT] texture id pool exhausted\n");
    return 0;
}
GLuint vita_rt_alloc_buffer_id(void) {
    if (sBufRecycleTop > 0) return sBufRecycle[--sBufRecycleTop];
    if (sBufTop < kPool) return sBufPool[sBufTop++];
    static unsigned said = 0;
    if (rt_first_time(said)) printf("[VITA RT] buffer id pool exhausted\n");
    return 0;
}
void vita_rt_release_texture_id(GLuint id) { if (id && sTexRecycleTop < kPool) sTexRecycle[sTexRecycleTop++] = id; }
void vita_rt_release_buffer_id(GLuint id)  { if (id && sBufRecycleTop < kPool) sBufRecycle[sBufRecycleTop++] = id; }

// Replay
struct RtAttrLayout { uint32_t offset[8]; int size[8]; int stride; };
static RtAttrLayout sLayout;

// Uniform images
static constexpr int kRtMaxPrograms = 16385;
static uint8_t* sRtShadow[kRtMaxPrograms][2];
static uint32_t sRtShadowSize[kRtMaxPrograms][2];
static uint8_t  sRtShadowKnown[kRtMaxPrograms];
static inline uint8_t* rt_shadow(uint32_t prog, uint32_t st) {
    if (prog == 0 || prog >= uint32_t(kRtMaxPrograms) || st > 1) return nullptr;
    if (!sRtShadowKnown[prog]) {
        for (int s = 0; s < 2; ++s) {
            uint32_t sz = 0;
            sRtShadow[prog][s] = static_cast<uint8_t*>(
                vglGetProgramShadow(prog, s ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, &sz));
            sRtShadowSize[prog][s] = sRtShadow[prog][s] ? sz : 0;
        }
        sRtShadowKnown[prog] = 1;
    }
    return sRtShadow[prog][st];
}
uint8_t* vita_rt_shadow(uint32_t prog, uint32_t stage, uint32_t* size) {
    uint8_t* sh = rt_shadow(prog, stage);
    if (size) *size = sh ? sRtShadowSize[prog][stage] : 0;
    return sh;
}

static bool     sAttrsDirty = true;
static uint32_t sLastProgram = 0xFFFFFFFFu;
static inline bool rt_attrs_stale(void) { return sAttrsDirty || vgl_fast_draw_last_prog() != sLastProgram; }

struct RtStreamMap { uint8_t n; uint8_t slot[8]; };
static RtStreamMap rt_stream_map_for(GLuint prog) {
    static const char* const kNames[8] = { "aPos", "aColor", "aNormal", "aTexCoord0", "aTexCoord1", "aTexCoord2", "aTexCoord3", "aMtxIdx" };
    RtStreamMap m; m.n = 0;
    bool present[8] = {};
    for (int i = 0; i < 8; ++i) {
        const GLint loc = glGetAttribLocation(prog, kNames[i]);
        if (loc >= 0 && loc < 8) present[loc] = true;
    }
    for (int slot = 0; slot < 8; ++slot) if (present[slot]) m.slot[m.n++] = uint8_t(slot);
    if (m.n == 0) { for (int i = 0; i < 8; ++i) m.slot[i] = uint8_t(i); m.n = 8; }   // unknown program
    return m;
}
static const RtStreamMap kIdentityStreams = { 8, { 0, 1, 2, 3, 4, 5, 6, 7 } };
static RtStreamMap sCurStreams = kIdentityStreams;
static RtStreamMap sMapByProg[kRtMaxPrograms];
static void rt_note_program(GLuint prog) {
    if (prog - 1u < uint32_t(kRtMaxPrograms - 1)) {
        RtStreamMap& m = sMapByProg[prog];
        if (m.n == 0) m = rt_stream_map_for(prog);
        sCurStreams = m;
        return;
    }
    sCurStreams = kIdentityStreams;
}
// Fork fast path
static bool sFixedTried = false;   // rt_fixed_setup ran (render thread, once)
static bool sFixedOn = false;
static int8_t sCurFixed = 0;       // the current program
static bool rt_fixed_ask(void);
static inline bool rt_fixed_active(void) {
    if (!sFixedOn || !vgl_fast_draw_mode) return false;
    if (sCurFixed == 0) sCurFixed = rt_fixed_ask() ? 1 : -1;
    return sCurFixed > 0;
}
#include "vita_efbcopy_gxp.h"
#include <unordered_map>
namespace {
enum { kEfbFpRgba = 0, kEfbFp565, kEfbFpI8, kEfbFpZ8, kEfbFpVFilter, kEfbFpCount };
int sEfbState = 0;   // 0 untried, 1 ready, -1 unavailable
struct RtEfbTarget { GLuint fbo = 0; int w = 0, h = 0; };
std::unordered_map<GLuint, RtEfbTarget> sEfbTargets;
GLuint sEfbFilterTex = 0;   // the display filter's full-screen copy
static int sEfbSplitFrames = 1000;
static bool sEfbOffscreenBound = false;   // draws target the offscreen EFB
}
static bool rt_efb_ready() {
    if (sEfbState == 0) {
        const SceGxmProgram* fps[kEfbFpCount] = {
            reinterpret_cast<const SceGxmProgram*>(gxp_efbcopy_f_rgba),
            reinterpret_cast<const SceGxmProgram*>(gxp_efbcopy_f_565),
            reinterpret_cast<const SceGxmProgram*>(gxp_efbcopy_f_i8),
            reinterpret_cast<const SceGxmProgram*>(gxp_efbcopy_f_z8),
            reinterpret_cast<const SceGxmProgram*>(gxp_efbcopy_f_vfilter),
        };
        int r = vglEfbCopySetPrograms(reinterpret_cast<const SceGxmProgram*>(gxp_efbcopy_v), fps, kEfbFpCount);
        if (r == 0) {
            const SceGxmProgram* cps[2] = {   // RT_EFB_COMP_BLUR, RT_EFB_COMP_GREY
                reinterpret_cast<const SceGxmProgram*>(gxp_efbcomp_f_blur),
                reinterpret_cast<const SceGxmProgram*>(gxp_efbcomp_f_grey),
            };
            r = vglEfbSetCompositePrograms(cps, 2);
            if (r != 0) r -= 100;
        }
        sEfbState = r == 0 ? 1 : -1;
        if (r != 0) printf("ERROR: EFB copy programs could not be set up (%d); screen copies are off\n", r);
    }
    return sEfbState == 1;
}
static GLuint rt_efb_fbo(GLuint tex, int w, int h) {
    RtEfbTarget& t = sEfbTargets[tex];
    if (t.fbo && t.w == w && t.h == h) return t.fbo;
    void* zero = calloc(size_t(w) * size_t(h), 4);
    glTextureImage2D(tex, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    free(zero);
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!t.fbo) glCreateFramebuffers(1, &t.fbo);
    glNamedFramebufferTexture2D(t.fbo, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    t.w = w; t.h = h;
    return t.fbo;
}
static void rt_efb_copy(GLuint tex, int w, int h, int program, int depth, float u0, float v0, float u1, float v1, int box) {
    if (!tex || w <= 0 || h <= 0 || !rt_efb_ready()) return;
    const GLuint fbo = rt_efb_fbo(tex, w, h);
    if (depth || !sEfbOffscreenBound) sEfbSplitFrames = 0;
    const int r = vglEfbCopy(fbo, program, depth, u0, v0, u1, v1, box);
    static int reported = 0;
    if (r < 0 && r != -4 && reported < 4) { ++reported; printf("ERROR: EFB copy refused (%d)\n", r); }
    sAttrsDirty = true;
}
static void rt_efb_target(GLuint tex, int w, int h) {
    sEfbOffscreenBound = tex && w > 0 && h > 0;
    if (sEfbOffscreenBound) glBindFramebuffer(GL_FRAMEBUFFER, rt_efb_fbo(tex, w, h));
    else glBindFramebuffer(GL_FRAMEBUFFER, 0);
    sAttrsDirty = true;
}
static void rt_efb_vfilter(float rowStep, float edge, float centre) {
    if (!rt_efb_ready()) return;
    if (!sEfbFilterTex) glGenTextures(1, &sEfbFilterTex);
    const GLuint fbo = rt_efb_fbo(sEfbFilterTex, NECTAR_VITA_SCREEN_W, NECTAR_VITA_SCREEN_H);
    const int r = vglEfbFilterDisplay(fbo, kEfbFpRgba, kEfbFpVFilter, rowStep, edge, centre);
    static int reported = 0;
    if (r < 0 && reported < 4) { ++reported; printf("ERROR: display filter refused (%d)\n", r); }
    sAttrsDirty = true;
}
static void rt_efb_frame_end() {
    if (sEfbSplitFrames < 1000) ++sEfbSplitFrames;
    vglSetDisplayDepthStore(sEfbSplitFrames <= 8 ? GL_TRUE : GL_FALSE);
}

static inline void rt_vstream(SceGxmContext* ctx, unsigned idx, const void* ptr) {
    if (sFixedOn) vglSetVertexStreamCached(GLuint(idx), ptr);
    else sceGxmSetVertexStream(ctx, idx, ptr);
}
static inline void rt_set_streams(SceGxmContext* ctx, const char* base) {
    for (int j = 0; j < sCurStreams.n; ++j)
        rt_vstream(ctx, unsigned(j), base + sLayout.offset[sCurStreams.slot[j]]);
}
static inline void rt_setup_attrs_from_layout(const void* base) {
    for (int i = 0; i < 8; ++i)
        glVertexAttribPointer(GLuint(i), sLayout.size[i], GL_FLOAT, GL_FALSE, sLayout.stride,
                              static_cast<const char*>(base) + sLayout.offset[i]);
    sAttrsDirty = false;
}
void vita_rt_set_attr_layout(const uint32_t offsets[8], const int sizes[8], int stride) {
    for (int i = 0; i < 8; ++i) { sLayout.offset[i] = offsets[i]; sLayout.size[i] = sizes[i]; }
    sLayout.stride = stride;
}
static bool rt_fixed_ask(void) {
    const int v = vglFixedLayoutReady(sLastProgram);
    if (v == VGL_FIXED_READY) return true;
    if (v == VGL_FIXED_OFF) return false;   // no linked program by that id
    static bool misfitLogged = false, patchLogged = false;
    bool& logged = v == VGL_FIXED_PATCH_FAILED ? patchLogged : misfitLogged;
    if (!logged) {
        logged = true;
        printf("[VITA FIXEDVS] ERROR: no fixed vertex layout program for program %u: %s; its draws take the per-attribute path (exact)\n",
               (unsigned)sLastProgram,
               v == VGL_FIXED_PATCH_FAILED ? "the shader patcher could not patch it (pool exhausted)"
                                           : "it reads an attribute the layout does not describe, or a disabled array");
    }
    return false;
}
static void rt_fixed_off(void) {
    sFixedOn = false;
    sCurFixed = -1;
    sAttrsDirty = true;
    vglSetFixedVertexLayout(0, nullptr, nullptr);
}
static bool rt_fixed_draw_refused(void) {
    if (!vglFixedDrawEnd()) return false;
    printf("[VITA FIXEDVS] ERROR: the fork refused an armed draw of program %u, which it had reported ready; the draw is "
           "re-issued on the per-attribute path and the fixed layout is off for the rest of the run\n",
           (unsigned)sLastProgram);
    rt_fixed_off();
    return true;
}
static void rt_fixed_setup(void) {
    sFixedTried = true;
    uint16_t off[8]; uint8_t comps[8];
    bool ok = sLayout.stride == 76;
    for (int i = 0; i < 8; ++i) {
        const int n = sLayout.size[i];
        ok = ok && n >= 1 && n <= 4 && sLayout.offset[i] + uint32_t(n) * 4u <= uint32_t(sLayout.stride);
        off[i] = uint16_t(sLayout.offset[i]);
        comps[i] = uint8_t(n);
    }
    if (!ok) {
        printf("[VITA FIXEDVS] ERROR: vertex layout invalid: stride %d (the Vertex is 76 bytes), offsets %u %u %u %u %u %u %u %u; "
               "per-attribute streams as before\n", sLayout.stride,
               (unsigned)sLayout.offset[0], (unsigned)sLayout.offset[1], (unsigned)sLayout.offset[2], (unsigned)sLayout.offset[3],
               (unsigned)sLayout.offset[4], (unsigned)sLayout.offset[5], (unsigned)sLayout.offset[6], (unsigned)sLayout.offset[7]);
        return;
    }
    vglSetFixedVertexLayout(uint16_t(sLayout.stride), off, comps);
    sFixedOn = true;
    sCurFixed = 0;
}

template <typename T> static inline T rd(const uint8_t*& p) { T v; memcpy(&v, p, sizeof(T)); p += sizeof(T); return v; }

static inline const char* rt_buf_gpu(GLuint id) {
    return static_cast<const char*>(vglBufferPtrStamp(id));
}

static void replay(int ci, const uint8_t* begin, const uint8_t* end) {
    SceGxmContext* ctx = vglGetGxmContext();
    if (!sFixedTried && sLayout.stride) rt_fixed_setup();
    const uint8_t* p = begin;
    uint32_t presentsDone = 0, syncsDone = 0, opIndex = 0;
    while (p + sizeof(RtHeader) <= end) {
        RtHeader h; memcpy(&h, p, sizeof(h));
        if (h.pad != kRtMagic || h.op >= RT_OP_COUNT || p + sizeof(h) + h.size > end) {
            static unsigned brokenSaid = 0;
            const bool say = rt_first_time(brokenSaid);
            if (say) {
                printf("[VITA RT] RING BROKEN during replay: chunk %d op#%u at off %u of %u: op %u pad 0x%04X size %u\n",
                       ci, opIndex, unsigned(p - begin), unsigned(end - begin), (unsigned)h.op, (unsigned)h.pad, h.size);
            }
            vita_rtgx_note_ring_damage();
            {
                const uint8_t* found = nullptr;
                for (const uint8_t* r = p + 4; r + sizeof(RtHeader) <= end; r += 4) {
                    RtHeader a; memcpy(&a, r, sizeof(a));
                    if (a.pad != kRtMagic || a.op >= RT_OP_COUNT || r + sizeof(a) + a.size > end) continue;
                    const uint8_t* r2 = r + sizeof(a) + a.size;
                    if (r2 == end) { found = r; break; }
                    if (r2 + sizeof(RtHeader) <= end) {
                        RtHeader b; memcpy(&b, r2, sizeof(b));
                        if (b.pad == kRtMagic && b.op < RT_OP_COUNT && r2 + sizeof(b) + b.size <= end) { found = r; break; }
                    }
                }
                if (found) {
                    if (say) printf("[VITA RT]   resynced %u bytes on, at off %u: replay continues\n", unsigned(found - p), unsigned(found - begin));
                    p = found;
                    continue;
                }
                if (say) printf("[VITA RT]   no valid header follows: abandoning the rest of the chunk\n");
            }
            const RtChunk& c = sChunks[ci];
            for (uint32_t k = presentsDone; k < c.presents; ++k) sceKernelSignalSema(sSemaFrame, 1);
            for (uint32_t k = syncsDone; k < c.syncs; ++k) sceKernelSignalSema(sSemaSync, 1);
            return;
        }
        ++opIndex;
        if (h.op == RT_PRESENT) ++presentsDone; else if (h.op == RT_SYNC || h.op == RT_INIT_GFX) ++syncsDone;
        p += sizeof(h);
        const uint8_t* q = p;
        p += h.size;
        switch (h.op) {
        case RT_NOP: break;
        case RT_USE_PROGRAM:      { uint32_t prog = rd<uint32_t>(q); glUseProgram(prog);
                                    if (prog != sLastProgram) { sLastProgram = prog; sAttrsDirty = true; rt_note_program(prog); sCurFixed = 0; } } break;
        case RT_UNIFORM_1I:       { int32_t l = rd<int32_t>(q); int32_t v = rd<int32_t>(q); glUniform1i(l, v); } break;
        case RT_UNIFORM_1F:       { int32_t l = rd<int32_t>(q); float v = rd<float>(q); glUniform1f(l, v); } break;
        case RT_UNIFORM_2I:       { int32_t l = rd<int32_t>(q); int32_t x = rd<int32_t>(q), y = rd<int32_t>(q); glUniform2i(l, x, y); } break;
        case RT_UNIFORM_4I:       { int32_t l = rd<int32_t>(q); int32_t x = rd<int32_t>(q), y = rd<int32_t>(q), z = rd<int32_t>(q), w = rd<int32_t>(q); glUniform4i(l, x, y, z, w); } break;
        case RT_UNIFORM_4F:       { int32_t l = rd<int32_t>(q); float x = rd<float>(q), y = rd<float>(q), z = rd<float>(q), w = rd<float>(q); glUniform4f(l, x, y, z, w); } break;
        case RT_UNIFORM_4FV:      { int32_t l = rd<int32_t>(q); int32_t n = rd<int32_t>(q); glUniform4fv(l, n, reinterpret_cast<const float*>(q)); } break;
        case RT_UNIFORM_MAT4:     { int32_t l = rd<int32_t>(q); int32_t n = rd<int32_t>(q); uint32_t t = rd<uint32_t>(q); glUniformMatrix4fv(l, n, GLboolean(t), reinterpret_cast<const float*>(q)); } break;
        case RT_UNIFORM_MAT3:     { int32_t l = rd<int32_t>(q); int32_t n = rd<int32_t>(q); uint32_t t = rd<uint32_t>(q); glUniformMatrix3fv(l, n, GLboolean(t), reinterpret_cast<const float*>(q)); } break;
        case RT_UNIFORM_IMAGE:    { uint32_t prog = rd<uint32_t>(q); uint32_t st = rd<uint32_t>(q); uint32_t off = rd<uint32_t>(q); uint32_t bytes = rd<uint32_t>(q);
                                    uint8_t* sh = rt_shadow(prog, st);
                                    if (sh && off + bytes <= sRtShadowSize[prog][st]) {
                                        memcpy(sh + off, q, bytes);
                                        vglMarkProgramShadowDirty(prog, st ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
                                    } } break;
        case RT_ACTIVE_TEXTURE:   glActiveTexture(rd<uint32_t>(q)); break;
        case RT_BIND_TEXTURE:     { uint32_t t = rd<uint32_t>(q); uint32_t id = rd<uint32_t>(q); glBindTexture(t, id); if (t == GL_TEXTURE_2D) sRtBound2D = id; } break;
        case RT_TEX_PARAMETER_I:  { uint32_t t = rd<uint32_t>(q); uint32_t pn = rd<uint32_t>(q); int32_t v = rd<int32_t>(q); glTexParameteri(t, pn, v); } break;
        case RT_TEX_IMAGE_2D:     { uint32_t t = rd<uint32_t>(q); int32_t lvl = rd<int32_t>(q); int32_t in = rd<int32_t>(q); int32_t w = rd<int32_t>(q); int32_t hh = rd<int32_t>(q);
                                    uint32_t fmt = rd<uint32_t>(q); uint32_t ty = rd<uint32_t>(q); uint32_t bytes = rd<uint32_t>(q);
                                    if (!rt_stream_texture(uint32_t(w), uint32_t(hh), fmt, ty, bytes, q)) {
                                        glTexImage2D(t, lvl, in, w, hh, 0, fmt, ty, bytes ? q : nullptr);
                                        rt_check_texture_upload(w, hh, fmt, bytes);
                                    } } break;
        case RT_COMPRESSED_TEX_IMAGE_2D: { uint32_t t = rd<uint32_t>(q); int32_t lvl = rd<int32_t>(q); int32_t in = rd<int32_t>(q); int32_t w = rd<int32_t>(q); int32_t hh = rd<int32_t>(q);
                                    uint32_t bytes = rd<uint32_t>(q);
                                    glCompressedTexImage2D(t, lvl, in, w, hh, 0, GLsizei(bytes), bytes ? q : nullptr);
                                    rt_check_texture_upload(w, hh, uint32_t(in), bytes); } break;
        case RT_GENERATE_MIPMAP:  glGenerateMipmap(rd<uint32_t>(q)); break;
        case RT_LINE_WIDTH:       glLineWidth(rd<float>(q)); break;
        case RT_EFB_COPY:         { uint32_t tex = rd<uint32_t>(q); int32_t w = rd<int32_t>(q), hh = rd<int32_t>(q), prog = rd<int32_t>(q), depth = rd<int32_t>(q);
                                    float u0 = rd<float>(q), v0 = rd<float>(q), u1 = rd<float>(q), v1 = rd<float>(q); int32_t box = rd<int32_t>(q);
                                    rt_efb_copy(tex, w, hh, prog, depth, u0, v0, u1, v1, box); } break;
        case RT_TEX_MIPCHAIN:     { uint32_t tex = rd<uint32_t>(q); int32_t w = rd<int32_t>(q), hh = rd<int32_t>(q), lv = rd<int32_t>(q);
                                    const void* lvl[16]; const uint8_t* c = q;
                                    if (lv < 1) lv = 1;
                                    if (lv > 16) lv = 16;
                                    for (int i = 0; i < lv; ++i) { lvl[i] = c; const int lw = (w >> i) > 0 ? (w >> i) : 1, lh = (hh >> i) > 0 ? (hh >> i) : 1; c += size_t(lw) * size_t(lh) * 4u; }
                                    const int r = vglTexImage2DMipChain(tex, w, hh, lv, lvl);
                                    if (r < 0) printf("ERROR: mip chain upload refused (%d) for texture %u\n", r, (unsigned)tex); } break;
        case RT_EFB_TARGET:       { uint32_t tex = rd<uint32_t>(q); int32_t w = rd<int32_t>(q), hh = rd<int32_t>(q); rt_efb_target(tex, w, hh); } break;
        case RT_EFB_COMPOSITE:    { int32_t kind = rd<int32_t>(q); uint32_t t0 = rd<uint32_t>(q), t1 = rd<uint32_t>(q); float p0 = rd<float>(q), p1 = rd<float>(q);
                                    sEfbSplitFrames = 0;
                                    if (rt_efb_ready()) { const int r = vglEfbComposite(kind, t0, t1, p0, p1); static int rep = 0;
                                        if (r < 0 && rep < 4) { ++rep; printf("ERROR: EFB composite refused (%d)\n", r); } }
                                    sAttrsDirty = true; } break;
        case RT_EFB_VFILTER:      { float st = rd<float>(q), ed = rd<float>(q), ce = rd<float>(q); rt_efb_vfilter(st, ed, ce); } break;
        case RT_TEX_SUB_IMAGE_2D: { uint32_t t = rd<uint32_t>(q); int32_t lvl = rd<int32_t>(q); int32_t x = rd<int32_t>(q); int32_t y = rd<int32_t>(q);
                                    int32_t w = rd<int32_t>(q); int32_t hh = rd<int32_t>(q); uint32_t fmt = rd<uint32_t>(q); uint32_t ty = rd<uint32_t>(q); uint32_t bytes = rd<uint32_t>(q);
                                    glTexSubImage2D(t, lvl, x, y, w, hh, fmt, ty, bytes ? q : nullptr); } break;
        case RT_DELETE_TEXTURE:   { uint32_t id = rd<uint32_t>(q); rt_stream_release(id); glDeleteTextures(1, &id); } break;
        case RT_BIND_BUFFER:      { uint32_t t = rd<uint32_t>(q); uint32_t id = rd<uint32_t>(q); glBindBuffer(t, id); } break;
        case RT_BUFFER_DATA:      { uint32_t t = rd<uint32_t>(q); uint32_t bytes = rd<uint32_t>(q); uint32_t usage = rd<uint32_t>(q); uint32_t size = rd<uint32_t>(q);
                                    glBufferData(t, size, bytes ? q : nullptr, usage); } break;
        case RT_BUFFER_DATA_MAP:  { uint32_t t = rd<uint32_t>(q); uint32_t bytes = rd<uint32_t>(q); uint32_t usage = rd<uint32_t>(q); (void)rd<uint32_t>(q);
                                    // Upload only
                                    glBufferData(t, bytes, q, usage); } break;
        case RT_BUFFER_SUB_DATA:  { uint32_t t = rd<uint32_t>(q); uint32_t off = rd<uint32_t>(q); uint32_t bytes = rd<uint32_t>(q); glBufferSubData(t, off, bytes, q); } break;
        case RT_DELETE_BUFFER:    { uint32_t id = rd<uint32_t>(q); glDeleteBuffers(1, &id); } break;
        case RT_VERTEX_ATTRIB_POINTER: { uint32_t i = rd<uint32_t>(q); int32_t sz = rd<int32_t>(q); uint32_t ty = rd<uint32_t>(q); uint32_t norm = rd<uint32_t>(q); int32_t st = rd<int32_t>(q); uint32_t off = rd<uint32_t>(q);
                                    glVertexAttribPointer(i, sz, ty, GLboolean(norm), st, reinterpret_cast<const void*>(uintptr_t(off)));
                                    sAttrsDirty = true; } break;
        case RT_ENABLE:           { uint32_t cap = rd<uint32_t>(q); glEnable(cap); if (cap == GL_SCISSOR_TEST) sAttrsDirty = true; } break;
        case RT_DISABLE:          { uint32_t cap = rd<uint32_t>(q); glDisable(cap); if (cap == GL_SCISSOR_TEST) sAttrsDirty = true; } break;
        case RT_BLEND_FUNC:       { uint32_t s = rd<uint32_t>(q); uint32_t d = rd<uint32_t>(q); glBlendFunc(s, d); } break;
        case RT_BLEND_EQUATION:   glBlendEquation(rd<uint32_t>(q)); break;
        case RT_DEPTH_FUNC:       glDepthFunc(rd<uint32_t>(q)); break;
        case RT_DEPTH_MASK:       glDepthMask(GLboolean(rd<uint32_t>(q))); break;
        case RT_COLOR_MASK:       { uint32_t m = rd<uint32_t>(q); glColorMask(GLboolean(m & 1), GLboolean((m >> 1) & 1), GLboolean((m >> 2) & 1), GLboolean((m >> 3) & 1)); } break;
        case RT_CULL_FACE:        glCullFace(rd<uint32_t>(q)); break;
        case RT_FRONT_FACE:       glFrontFace(rd<uint32_t>(q)); break;
        case RT_VIEWPORT:         { int32_t x = rd<int32_t>(q), y = rd<int32_t>(q), w = rd<int32_t>(q), hh = rd<int32_t>(q); glViewport(x, y, w, hh); } break;
        case RT_SCISSOR:          { int32_t x = rd<int32_t>(q), y = rd<int32_t>(q), w = rd<int32_t>(q), hh = rd<int32_t>(q); glScissor(x, y, w, hh); sAttrsDirty = true; } break;
        case RT_CLEAR_COLOR:      { float r = rd<float>(q), g = rd<float>(q), b = rd<float>(q), a = rd<float>(q); glClearColor(r, g, b, a); } break;
        case RT_CLEAR_DEPTH:      glClearDepth(rd<float>(q)); break;
        case RT_CLEAR:            glClear(rd<uint32_t>(q)); sAttrsDirty = true; break;
        case RT_DRAW_ARRAYS:      { uint32_t m = rd<uint32_t>(q); int32_t f = rd<int32_t>(q); int32_t n = rd<int32_t>(q); glDrawArrays(m, f, n); } break;
        case RT_DRAW_ELEMENTS:    { uint32_t m = rd<uint32_t>(q); int32_t n = rd<int32_t>(q); uint32_t ty = rd<uint32_t>(q); uint32_t off = rd<uint32_t>(q);
                                    glDrawElements(m, n, ty, reinterpret_cast<const void*>(uintptr_t(off))); } break;
        case RT_DRAW_CLIENT:      { uint32_t m = rd<uint32_t>(q); int32_t n = rd<int32_t>(q); uint32_t bytes = rd<uint32_t>(q);
                                    void* tmp = vglAllocFromScratch(bytes);
                                    if (tmp) {
                                        memcpy(tmp, q, bytes);
                                        const char* base = static_cast<const char*>(tmp);
                                        if (rt_fixed_active()) {
                                            vglSetFixedStreamBase(base);
                                            glDrawArrays(m, 0, n);
                                            if (!rt_fixed_draw_refused()) break;
                                            // Refused: per-attribute draw below
                                        }
                                        glBindBuffer(GL_ARRAY_BUFFER, 0);
                                        // Scratch memory
                                        rt_setup_attrs_from_layout(base);
                                        sAttrsDirty = true;
                                        rt_set_streams(ctx, base);
                                        glDrawArrays(m, 0, n);
                                    } } break;
        case RT_GXM_VERTEX_STREAM: { uint32_t i = rd<uint32_t>(q); uint32_t bufId = rd<uint32_t>(q); uint32_t off = rd<uint32_t>(q);
                                    glBindBuffer(GL_ARRAY_BUFFER, bufId);
                                    const char* base = rt_buf_gpu(bufId);
                                    if (base) rt_vstream(ctx, i, base + off); } break;
        case RT_GXM_VERTEX_STREAM_ABS: { uint32_t i = rd<uint32_t>(q); const void* ptr = rd<const void*>(q); rt_vstream(ctx, i, ptr); } break;
        case RT_GXM_FRAGMENT_TEXTURE: { uint32_t u = rd<uint32_t>(q); uint32_t id = rd<uint32_t>(q);
                                    const SceGxmTexture* t = vglGetGxmTextureById(id);
                                    if (t) sceGxmSetFragmentTexture(ctx, u, t); } break;
        case RT_FAST_MODE:        vgl_fast_draw_mode = rd<int32_t>(q); break;
        case RT_FAST_DRAW_ARRAYS: { uint32_t vbo = rd<uint32_t>(q); const void* attrBase = rd<const void*>(q); const char* gpu = rd<const char*>(q);
                                    uint32_t mode = rd<uint32_t>(q); int32_t n = rd<int32_t>(q);
                                    bool drawn = false;
                                    if (rt_fixed_active()) {
                                        if (vbo) (void)vglBufferPtrStamp(vbo);
                                        vglSetFixedStreamBase(gpu);
                                        glDrawArrays(mode, 0, n);
                                        // Refused: per-attribute draw below
                                        drawn = !rt_fixed_draw_refused();
                                    }
                                    if (!drawn) {
                                        if (rt_attrs_stale()) {
                                            glBindBuffer(GL_ARRAY_BUFFER, vbo);
                                            rt_setup_attrs_from_layout(attrBase);
                                        }
                                        rt_set_streams(ctx, gpu);
                                        glDrawArrays(mode, 0, n);
                                    }
                                    if (mode == GL_LINES || mode == GL_POINTS) {
                                        sceGxmSetFrontPolygonMode(ctx, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
                                        sceGxmSetBackPolygonMode(ctx, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
                                    } } break;
        case RT_FAST_DRAW_INDEXED: { uint32_t vbo = rd<uint32_t>(q); uint32_t ibo = rd<uint32_t>(q); int32_t n = rd<int32_t>(q); int32_t first = rd<int32_t>(q);
                                    const char* base = rt_buf_gpu(vbo);
                                    // No geometry for this buffer
                                    if (!base) break;
                                    if (rt_fixed_active()) {
                                        vglSetFixedStreamBase(base);
                                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
                                        glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(uintptr_t(first) * 2u));
                                        if (!rt_fixed_draw_refused()) break;
                                        // Refused: per-attribute draw below
                                    }
                                    if (rt_attrs_stale()) {
                                        glBindBuffer(GL_ARRAY_BUFFER, vbo);
                                        rt_setup_attrs_from_layout(nullptr);
                                    }
                                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
                                    rt_set_streams(ctx, base);
                                    glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(uintptr_t(first) * 2u)); } break;
        case RT_PRESENT:          rt_efb_frame_end();
                                    vita_platform_present_impl();
                                    sAttrsDirty = true;
                                    sVramFreeKb = unsigned(vglMemFree(VGL_MEM_VRAM) / 1024);
                                    sceKernelSignalSema(sSemaFrame, 1);
                                    break;
        case RT_INIT_GFX:         pc_gfx_init_impl();
                                    vglSetUniformVersioning(GL_TRUE, GL_FALSE);
                                    vglSetFragmentProgramCache(GL_TRUE, GL_FALSE);
                                    sceKernelSignalSema(sSemaSync, 1); break;
        case RT_SYNC:             sceKernelSignalSema(sSemaSync, 1); break;
        case RT_GX_APPLY:         vita_rtgx_apply(q, h.size); break;
        case RT_GX_PALETTE:       vita_rtgx_palette(q, h.size); break;
        case RT_GX_SYNC_BACK:     vita_rtgx_sync_back(); break;
        default:
            {
                static unsigned said = 0;
                if (rt_first_time(said)) printf("[VITA RT] unknown op %u\n", (unsigned)h.op);
            }
            break;
        }
    }
}

static int render_thread_main(SceSize, void*) {
    int w = 0, hgt = 0;
    if (!vita_platform_gl_init(&w, &hgt)) {
        printf("[VITA RT] vitaGL init failed on the render thread\n");
        sReady = true;
        return 0;
    }
    glGenTextures(kPool, sTexPool);
    glGenBuffers(kPool, sBufPool);
    sReady = true;
    int readChunk = 0;
    for (;;) {
        sceKernelWaitSema(sSemaReady, 1, NULL);
        RtChunk& c = sChunks[readChunk];
        replay(readChunk, c.data, c.data + c.used);
        readChunk = (readChunk + 1) % kChunks;
        sceKernelSignalSema(sSemaFree, 1);
    }
    return 0;
}

bool vita_rt_start(void) {
    for (int i = 0; i < kChunks; ++i) {
        sChunks[i].data = static_cast<uint8_t*>(malloc(kChunkSize));
        if (!sChunks[i].data) { printf("[VITA RT] chunk alloc failed\n"); return false; }
    }
    sSemaReady = sceKernelCreateSema("rt_ready", 0, 0, kChunks, NULL);
    sSemaFree  = sceKernelCreateSema("rt_free", 0, kChunks - 1, kChunks, NULL);   // the write chunk is ours
    sSemaFrame = sceKernelCreateSema("rt_frame", 0, vita_gxq_pipe_depth(), vita_gxq_pipe_depth() + 1, NULL);   // frames in flight
    sSemaSync  = sceKernelCreateSema("rt_sync", 0, 0, 1, NULL);
    // Core affinity
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sThread = sceKernelCreateThread("nectar_render", render_thread_main, 0x40, 0x40000,
                                    0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (sThread < 0) { printf("[VITA RT] thread create failed 0x%08X\n", (unsigned)sThread); return false; }
    sceKernelStartThread(sThread, 0, NULL);
    while (!sReady) sceKernelDelayThread(1000);
    return true;
}

void vita_rt_run_init_gfx(void) {
    sRecording = true;
    vita_rt_emit(RT_INIT_GFX, 0);
    close_chunk();
    sRecording = false;
    sceKernelWaitSema(sSemaSync, 1, NULL);
}

void vita_rt_begin_recording(void) {
    sRecording = true;
}
