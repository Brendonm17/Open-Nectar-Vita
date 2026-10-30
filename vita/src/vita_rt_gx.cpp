// Render-thread uniform sections (rtunif).
#include "vita_rt.h"
#include "pc_program_locations.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr uint32_t kRtGxMaxProg = 16385;  // vitaGL program ids 1..16384

// Program registry
struct RtGxProg {
    const ProgramLocations* loc;        // table written with
    const ProgramLocations* alias[2];   // equal tables of the same id
    uint32_t size[2];                   // translator image size per stage, 0 if none
};
static RtGxProg* sReg = nullptr;        // kRtGxMaxProg entries
static bool     sRegOk = true;
static VgxMirror sMir;                  // section inputs

static bool rt_reg_fail(const char* why, GLuint prog) {
    if (sRegOk) printf("[VITA RTUNIF] ERROR: registration failed: %s (program %u); the translator writes its own uniforms\n", why, (unsigned)prog);
    sRegOk = false;
    return false;
}
bool vita_rtgx_register(GLuint prog, const ProgramLocations* loc, const uint32_t imgSize[2]) {
    if (!sReg) {
        if (!sRegOk) return false;
        sReg = static_cast<RtGxProg*>(calloc(kRtGxMaxProg, sizeof(RtGxProg)));
        if (!sReg) return rt_reg_fail("no memory for the registry", prog);
        vgx_mirror_reset(sMir);
    }
    if (!sRegOk) return false;
    if (prog == 0 || prog >= kRtGxMaxProg || !loc) return true;
    RtGxProg& e = sReg[prog];
    if (!e.loc) {
        // Shadow must match the translator image size.
        for (int st = 0; st < 2; ++st) {
            if (!imgSize[st]) continue;
            uint32_t sz = 0;
            if (!vita_rt_shadow(prog, uint32_t(st), &sz) || sz != imgSize[st])
                return rt_reg_fail("a translator image and its vitaGL shadow differ in size", prog);
        }
        e.loc = loc;
        e.size[0] = imgSize[0]; e.size[1] = imgSize[1];
        return true;
    }
    if (e.loc == loc || e.alias[0] == loc || e.alias[1] == loc) return true;
    if (memcmp(e.loc, loc, sizeof(ProgramLocations)) != 0) return rt_reg_fail("two location tables of one program differ", prog);
    if (e.size[0] != imgSize[0] || e.size[1] != imgSize[1]) return rt_reg_fail("one program registered with two image sizes", prog);
    if (!e.alias[0]) e.alias[0] = loc;
    else if (!e.alias[1]) e.alias[1] = loc;
    else return rt_reg_fail("more than three location tables for one program", prog);
    return true;
}
bool vita_rtgx_registry_ok(void) { return sRegOk && sReg != nullptr; }
bool vita_rtgx_loc_is(GLuint prog, const ProgramLocations* loc) {
    if (!sReg || prog == 0 || prog >= kRtGxMaxProg || !loc) return false;
    const RtGxProg& e = sReg[prog];
    return e.loc && (e.loc == loc || e.alias[0] == loc || e.alias[1] == loc);
}

static volatile bool     sFailed = false;   // a record was lost (sticky)
static bool              sOn = false;       // rtunif on
bool vita_rtgx_failed(void) { return sFailed; }
static void rt_fail(const char* what) {
    if (!sFailed) printf("[VITA RTUNIF] ERROR: %s; the translator writes its own uniforms from its next present\n", what);
    sFailed = true;
}

// Per-location uniform cache, mirroring pc_gfx.cpp cached_uniform*.
static constexpr uint32_t kRtRawSlots = uint32_t(kVgxUniformCacheSlots);
struct RtRawSlot { GLint owner; uint8_t kind; uint8_t pad[3]; uint32_t v[16]; };
static RtRawSlot* sRawCache = nullptr;
static bool       sRawCacheOff = false;         // PIKMIN_NO_UNIFORM_CACHE
static inline RtRawSlot& rt_raw_slot(GLint loc) { return sRawCache[(static_cast<uint32_t>(loc) / 8u) % kRtRawSlots]; }
// True when the translator cache would skip this write.
static bool rt_raw_cached(GLint loc, int kind, uint32_t words, const void* data) {
    if (sRawCacheOff || !sRawCache || words > 16) return false;
    RtRawSlot& e = rt_raw_slot(loc);
    if (e.owner != loc) { e.owner = loc; e.kind = 0; }
    bool same = false;
    if (e.kind == kind) {
        if (kind == kVgxPut1f) { float a, b; memcpy(&a, e.v, 4); memcpy(&b, data, 4); same = a == b; }
        else same = memcmp(e.v, data, words * 4u) == 0;
    }
    if (same) return true;
    e.kind = uint8_t(kind);
    memcpy(e.v, data, words * 4u);
    return false;
}
// Write to a location outside the image.
static void rt_raw_send(GLint loc, int kind, uint32_t words, const void* data) {
    if (kind == kVgxPut4fv) {
        if (loc >= 0) glUniform4fv(loc, GLsizei(words / 4u), static_cast<const GLfloat*>(data));
        return;
    }
    if (loc < 0) return;
    if (rt_raw_cached(loc, kind, words, data)) return;
    const GLint* iv = static_cast<const GLint*>(data);
    const GLfloat* fv = static_cast<const GLfloat*>(data);
    switch (kind) {
    case kVgxPut1i:   glUniform1i(loc, iv[0]); break;
    case kVgxPut1f:   glUniform1f(loc, fv[0]); break;
    case kVgxPut2i:   glUniform2i(loc, iv[0], iv[1]); break;
    case kVgxPut4i:   glUniform4i(loc, iv[0], iv[1], iv[2], iv[3]); break;
    case kVgxPut4f:   glUniform4f(loc, fv[0], fv[1], fv[2], fv[3]); break;
    case kVgxPutMat4: glUniformMatrix4fv(loc, 1, GL_FALSE, fv); break;
    default: return;
    }
}

// Shadow sink
struct RtSink {
    uint32_t prog;
    uint32_t size[2];
    uint8_t* sh[2];
    bool     changed[2];
    void put(GLint loc, int kind, uint32_t words, const void* data) {
        if (vgx_slot_is_image(loc)) {
            const int st = vgx_slot_stage(loc);
            uint8_t* img = sh[st];
            if (!img) return;
            uint32_t tmp[128]; const void* d; uint32_t off, bytes;
            if (!vgx_img_prepare(loc, data, words, vgx_put_is_int(kind), size[st], tmp, &d, &off, &bytes)) return;
            if (memcmp(img + off, d, bytes) == 0) return;
            memcpy(img + off, d, bytes);
            changed[st] = true;
            return;
        }
        rt_raw_send(loc, kind, words, data);
    }
};
static inline void rt_sink_init(RtSink& s, uint32_t prog) {
    s.prog = prog;
    for (int st = 0; st < 2; ++st) {
        s.size[st] = (sReg && prog < kRtGxMaxProg) ? sReg[prog].size[st] : 0u;
        s.sh[st] = s.size[st] ? vita_rt_shadow(prog, uint32_t(st), nullptr) : nullptr;
        s.changed[st] = false;
    }
}
// Marks changed stages dirty.
static inline void rt_sink_finish(const RtSink& s) {
    for (int st = 0; st < 2; ++st) {
        if (!s.changed[st]) continue;
        vglMarkProgramShadowDirty(s.prog, st ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    }
}

bool vita_rtgx_configure(bool on, bool rawCacheOff) {
    sRawCacheOff = rawCacheOff;
    sOn = false;
    if (!on) return true;
    if (!sRawCacheOff && !sRawCache) {
        sRawCache = static_cast<RtRawSlot*>(calloc(kRtRawSlots, sizeof(RtRawSlot)));
        if (!sRawCache) { printf("[VITA RTUNIF] ERROR: no memory for the per-location cache; the translator writes its own uniforms\n"); return false; }
    }
    sOn = true;
    return true;
}

// Ring damage during replay.
void vita_rtgx_note_ring_damage(void) {
    if (sOn) rt_fail("the ring lost ops during replay (RING BROKEN), records perhaps among them");
}

// Records
// Block validation; a bad record is dropped whole.
static bool rt_blocks_ok(const uint8_t* p, const uint8_t* end, uint16_t n, const uint8_t** after) {
    for (uint16_t i = 0; i < n; ++i) {
        if (p + 4 > end) return false;
        uint16_t len; memcpy(&len, p + 2, 2);
        if ((len & 3u) != 0 || p + 4 + len > end || !vgx_block_dst(sMir, p[0], p[1], len)) return false;
        p += 4 + len;
    }
    *after = p;
    return true;
}
static void rt_blocks_apply(const uint8_t* p, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t len; memcpy(&len, p + 2, 2);
        memcpy(vgx_block_dst(sMir, p[0], p[1], len), p + 4, len);
        p += 4 + len;
    }
}
static ProgramLocations sInlineLoc;
// Runs one record's writers into the shadows.
template <class W>
static void rt_run(uint32_t prog, W&& write) {
    RtSink s;
    rt_sink_init(s, prog);
    write(s);
    rt_sink_finish(s);
}
// Location table for a record: registered or inline.
static const ProgramLocations* rt_record_loc(uint32_t prog, uint8_t flags, const uint8_t* inl) {
    if (flags & kVgxRecInlineLoc) {
        memcpy(&sInlineLoc, inl, sizeof(sInlineLoc));
        return &sInlineLoc;
    }
    if (!sReg || prog >= kRtGxMaxProg || !sReg[prog].loc) { rt_fail("a record naming a program with no registered table was dropped"); return nullptr; }
    return sReg[prog].loc;
}

void vita_rtgx_apply(const uint8_t* q, uint32_t bytes) {
    const uint8_t* const end = q + bytes;
    VgxApplyRec h;
    if (bytes < sizeof(h)) { rt_fail("an apply record shorter than its header was dropped"); return; }
    memcpy(&h, q, sizeof(h));
    const uint8_t* blocks = q + sizeof(h);
    const uint8_t* after = nullptr;
    if (!rt_blocks_ok(blocks, end, h.nBlocks, &after)
        || ((h.flags & kVgxRecInlineLoc) && after + sizeof(ProgramLocations) > end)) {
        rt_fail("an apply record that does not parse was dropped");
        return;
    }
    rt_blocks_apply(blocks, h.nBlocks);
    const ProgramLocations* loc = rt_record_loc(h.program, h.flags, after);
    if (!loc) return;
    unsigned sections = h.sections;
    const float* s0p = nullptr; const float* s0n = nullptr;
    if (h.slot0 == 1) { s0p = kVgxIdentity16; s0n = kVgxIdentityNrm9; }
    else if (h.slot0 == 2 && h.slot0Idx < 64) { s0p = sMir.pos[h.slot0Idx]; s0n = sMir.nrm[h.slot0Idx]; }
    else sections &= ~unsigned(kVgxSecSlot0);
    const VgxSrc g = vgx_src_of(sMir);
    rt_run(h.program, [&](auto& s) { vgx_write_sections(g, *loc, sections, s0p, s0n, s); });
}

void vita_rtgx_palette(const uint8_t* q, uint32_t bytes) {
    const uint8_t* const end = q + bytes;
    VgxPaletteRec h;
    if (bytes < sizeof(h)) { rt_fail("a palette record shorter than its header was dropped"); return; }
    memcpy(&h, q, sizeof(h));
    const uint8_t* blocks = q + sizeof(h);
    const uint8_t* after = nullptr;
    if (!rt_blocks_ok(blocks, end, h.nBlocks, &after)
        || ((h.flags & kVgxRecInlineLoc) && after + sizeof(ProgramLocations) > end)) {
        rt_fail("a palette record that does not parse was dropped");
        return;
    }
    rt_blocks_apply(blocks, h.nBlocks);
    const ProgramLocations* loc = rt_record_loc(h.program, h.flags, after);
    if (!loc) return;
    rt_run(h.program, [&](auto& s) { vgx_write_palette(sMir.pos, sMir.nrm, h.current, int(h.slots), h.useCurrent != 0, *loc, s); });
}

// Copies shadows back into the translator images.
void vita_rtgx_sync_back(void) {
    sOn = false;
    if (!sReg) return;
    for (uint32_t prog = 1; prog < kRtGxMaxProg; ++prog) {
        if (!sReg[prog].loc) continue;
        for (int st = 0; st < 2; ++st) {
            if (!sReg[prog].size[st]) continue;
            uint32_t xs = 0, ss = 0;
            uint8_t* x = vita_gx_xlat_image(prog, st, &xs);
            const uint8_t* sh = vita_rt_shadow(prog, uint32_t(st), &ss);
            if (x && sh && xs == ss) memcpy(x, sh, xs);
        }
    }
}
