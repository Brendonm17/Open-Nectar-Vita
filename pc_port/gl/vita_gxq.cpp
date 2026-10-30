// PS Vita: GX-level command queue. Game thread records, translator thread replays.
#include "pc_gfx.h"
#include "vita_gxq.h"
#include "vita_gxq_ctx.h"
#include "vita_gxq_impl.h"
#include "vita_imm_chain.h"
#include "vita_drawrec.h"   // draw-record join hooks
#include "settings/pc_settings.h"   // Fog row (GXSetFog)

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include "timing/pc_prof.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
extern "C" void nectar_gil_release(void);   // cooperative lock
extern "C" void nectar_gil_acquire(void);

enum GxqOp : uint16_t { GXQ_NOP = 0, GXQ_BEGIN_FRAME, GXQ_PRESENT, GXQ_PERF_SCOPE_BEGIN, GXQ_PERF_SCOPE_END, GXQ_SET_RENDER_SCALE, GXQ_SET_ASPECT_RATIO_MODE, GXQ_SET_PROJECTION, GXQ_SET_VIEWPORT, GXQ_SET_SCISSOR, GXQ_LOAD_POS_MTX, GXQ_SET_CURRENT_MTX, GXQ_LOAD_NRM_MTX, GXQ_LOAD_TEX_MTX, GXQ_SET_TEX_COORD_GEN, GXQ_SET_Z_MODE, GXQ_SET_BLEND_MODE, GXQ_SET_CULL_MODE, GXQ_SET_COLOR_UPDATE, GXQ_SET_ALPHA_UPDATE, GXQ_SET_ALPHA_COMPARE, GXQ_SET_CHAN_CTRL, GXQ_SET_CHAN_MAT_COLOR, GXQ_SET_CHAN_AMB_COLOR, GXQ_LOAD_LIGHT, GXQ_SET_TEV_ORDER, GXQ_SET_TEV_OP, GXQ_SET_NUM_TEV_STAGES, GXQ_SET_TEV_COLOR_IN, GXQ_SET_TEV_ALPHA_IN, GXQ_SET_TEV_COLOR_OP, GXQ_SET_TEV_ALPHA_OP, GXQ_SET_TEV_COLOR, GXQ_SET_TEV_COLOR_S10, GXQ_SET_TEV_KCOLOR, GXQ_SET_TEV_KCOLOR_SEL, GXQ_SET_TEV_KALPHA_SEL, GXQ_SET_TEV_SWAP_MODE, GXQ_SET_TEV_SWAP_MODE_TABLE, GXQ_INIT_TEX_OBJ, GXQ_INIT_TEX_OBJ_CI, GXQ_INIT_TLUT_OBJ, GXQ_LOAD_TLUT, GXQ_LOAD_TEX_OBJ, GXQ_CLEAR_VTX_DESC, GXQ_SET_VTX_DESC, GXQ_SET_VTX_ATTR_FMT, GXQ_SET_ARRAY, GXQ_BEGIN, GXQ_PUSH_U8, GXQ_PUSH_U16, GXQ_PUSH_U32, GXQ_PUSH_S8, GXQ_PUSH_S16, GXQ_PUSH_S32, GXQ_PUSH_F32, GXQ_POSITION, GXQ_COLOR, GXQ_TEXCOORD, GXQ_END, GXQ_CALL_DISPLAY_LIST, GXQ_COPY_DISP, GXQ_SET_COPY_CLEAR, GXQ_BEGIN_CAPTURE, GXQ_END_CAPTURE, GXQ_ENABLE_CAPTURE, GXQ_SET_SHADER_SPECIALISATION, GXQ_FLUSH_BATCH, GXQ_SYNC, GXQ_SET_FOG, GXQ_SET_SECTION_CLIP_43, GXQ_SET_SCISSOR_PANEL, GXQ_SET_SCISSOR_UI_BOX, GXQ_SET_MOVIE_PLANES_LINEAR, GXQ_PRIM_STREAM, GXQ_LOAD_POSNRM_GROUP, GXQ_CALL_SUBSTREAM, GXQ_SUB_END, GXQ_LOAD_TEX_MTX_SPLICE, GXQ_SET_LINE_WIDTH, GXQ_INIT_TEX_OBJ_LOD, GXQ_COPY_TEX, GXQ_EFB_TARGET, GXQ_SET_COPY_FILTER, GXQ_EFB_COMPOSITE, GXQ_LEVEL_FLUSH, GXQ_COUNT };
void vita_key_dirty(unsigned mask);   // block-cached batch key


// Recorders inlined, outside NECTAR_PROF instrumentation.
#define GXQ_REC static inline __attribute__((no_instrument_function))
// Lambdas for gxq_each, outside NECTAR_PROF instrumentation.
#define GXQ_NI __attribute__((no_instrument_function))
// Public recorders of the hot recorders.
#if NECTAR_GXQ_LEAN
#define GXQ_HOT_REC(name) gxq_r_##name
#else
#define GXQ_HOT_REC(name) rec_##name
#endif

// The contexts
#define GXQ_X26(v) v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v, v
GxqCtx   gGxqMain = { nullptr, nullptr, 0u, 1u, kGxqSinkRing, 0u, false, nullptr, 0u, 0u,
                      { 1u, { GXQ_X26(0xFFFFFFFFu) } } };
#undef GXQ_X26
static_assert(sizeof(GxqCtx) % 32u == 0u && alignof(GxqCtx) == 32u, "the context is whole cache lines");
#if NECTAR_GXQ_LEAN
static_assert(int(kGxqGenPe) == int(PCGX_GEN_PE) && int(kGxqGenKCol) == int(PCGX_GEN_KCOL) && int(kGxqGenTev) == int(PCGX_GEN_TEV)
                  && int(kGxqGenChan) == int(PCGX_GEN_CHAN) && int(kGxqGenTevReg) == int(PCGX_GEN_TEVREG),
              "vita_gxq_rec.h's generation categories are pc_gfx.h's");
#endif
volatile uint8_t gDrawrecKill = 0;
static uint32_t sEpochSource = 1;   // shadow epoch (atomic)
// Main context immediate-mode primitive buffer.
static uint8_t  sPrimBuf[60 * 1024];

// Sticky stage switches.
static volatile uint8_t gVtxImageOn = 0, gChanRule = 0, gGenOnEmit = 0;

static constexpr int      kChunks    = 4;
static constexpr uint32_t kChunkSize = 1u << 20;
struct GxqHeader { uint16_t op; uint16_t size; };     // size = payload bytes (4-aligned)
struct GxqChunk  { uint8_t* data = nullptr; uint32_t used = 0; };
static GxqChunk sChunks[kChunks];
static int      sWriteChunk = 0;
static SceUID   sSemaReady = -1, sSemaFree = -1, sSemaFrame = -1, sSemaSync = -1;
static SceUID   sThread = -1, sGameThread = -1;
static volatile bool sRecording = false;
// Frames in flight at each stage boundary.
int vita_gxq_pipe_depth(void) { return 2; }

static volatile bool sReady = false;

// Record-time filter for material state
template <int N> static inline bool gxq_valid(const GxqCtx& R, const GxqSh<N>& s) { return s.ep == R.d.ep; }
template <int N> static inline bool gxq_same(GxqCtx& R, GxqSh<N>& s, int n, const int32_t* a) {
    if (s.ep == R.d.ep && memcmp(s.v, a, size_t(n) * 4u) == 0) return true;
    memcpy(s.v, a, size_t(n) * 4u);
    s.ep = R.d.ep;
    return false;
}
// Projection shadow.
static volatile uint32_t sStaleGen = 0;       // display lists that wrote a register
static volatile uint32_t sDlRegWrites = 0;
// Translator-written inputs of the record side.
static inline uint32_t gxq_rd_stale(const GxqCtx&) { return sStaleGen; }
static inline uint32_t gxq_rd_dl_reg_writes(const GxqCtx&) { return sDlRegWrites; }
// GXChannelID to shadow slot 0..5, -1 otherwise.
static inline int gxq_chan_slot(int chan) {
    switch (chan) {
    case GX_COLOR0: return 0; case GX_COLOR1: return 1;
    case GX_ALPHA0: return 2; case GX_ALPHA1: return 3;
    case GX_COLOR0A0: return 4; case GX_COLOR1A1: return 5;
    default: return -1;
    }
}
// Material state generations, and generation-on-emit (S4).
#define GXQ_GEN_EMIT(R, cat) (++(R).d.stateGenEmit[cat])


// S4: channel shadow rule

// Deferred logs and the main-only guard
static inline bool gxq_may_print(const GxqCtx& R) { return R.isMain != 0; }
static inline void gxq_defer(GxqCtx& R, uint32_t what, const char* why) {
    R.deferred |= what;
    if (!R.deferredWhy) R.deferredWhy = why;
}
void gxq_main_only_violation(GxqCtx& R) {
    gxq_defer(R, kGxqDeferMainOnly, "a frame-level GX call reached a non-main recorder");
    gDrawrecKill = 1;
}

// Off-lock workers
struct GxqOffLockStack { uintptr_t lo, hi; };
static GxqOffLockStack sOffLock[4];
static int sOffLockCount = 0;
GxqOffLockBound gGxqOffLockBound = { 0u, 0u };
extern "C" void vita_offlock_register(uintptr_t lo, uintptr_t hi) {
    const int n = __atomic_load_n(&sOffLockCount, __ATOMIC_RELAXED);
    for (int i = 0; i < n; ++i) if (sOffLock[i].lo == lo && sOffLock[i].hi == hi) return;
    if (n >= 4 || hi <= lo) return;
    sOffLock[n].lo = lo;
    sOffLock[n].hi = hi;
    __atomic_store_n(&sOffLockCount, n + 1, __ATOMIC_RELEASE);
    uintptr_t blo = lo, bhi = hi;
    for (int i = 0; i < n; ++i) {
        if (sOffLock[i].lo < blo) blo = sOffLock[i].lo;
        if (sOffLock[i].hi > bhi) bhi = sOffLock[i].hi;
    }
    __atomic_store_n(&gGxqOffLockBound.span, bhi - blo, __ATOMIC_RELEASE);
    __atomic_store_n(&gGxqOffLockBound.lo, blo, __ATOMIC_RELEASE);
}
__attribute__((noinline)) bool vita_offlock_precise(uintptr_t sp) {
    const int n = __atomic_load_n(&sOffLockCount, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; ++i) if (sp - sOffLock[i].lo < sOffLock[i].hi - sOffLock[i].lo) return true;
    return false;
}
// Off-lock worker test for the C side.
extern "C" int vita_offlock_on_worker(void) { return vita_offlock_worker() ? 1 : 0; }
// Refused call from an off-lock worker. Any thread.
static volatile uint32_t sForeignHeal = 0;
void gxq_foreign_refused(GxqCtx&) {
    gDrawrecKill = 1;
    __atomic_store_n(&sForeignHeal, 1u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&sStaleGen, 1u, __ATOMIC_RELEASE);
}
static __attribute__((noinline)) uint8_t* gxq_foreign(GxqCtx& R) {
    gxq_foreign_refused(R);
    return nullptr;
}

static void gxq_shadow_reset(GxqCtx& R) {
    R.d.ep = __atomic_add_fetch(&sEpochSource, 1u, __ATOMIC_RELAXED);
    for (unsigned& g : R.d.stateGen) ++g;
    for (unsigned& g : R.d.stateGenEmit) ++g;
}
// Vertex shadow heal after a refused foreign call.
static void gxq_vtx_rearm(GxqCtx& R) {
    GxqDecision& d = R.d;
    memset(d.shDesc, 0xFF, sizeof(d.shDesc));
    memset(d.shFmt, 0xFF, sizeof(d.shFmt));
    memset(d.shArrayBase, 0xFF, sizeof(d.shArrayBase));
    memset(d.shArrayStride, 0xFF, sizeof(d.shArrayStride));
    d.pendDescMask = (1u << GX_VA_MAX_ATTR) - 1u;
    for (uint32_t& m : d.pendFmtMask) m = (1u << GX_VA_MAX_ATTR) - 1u;
    d.pendDirty = true;
    ++d.vtxGen;
    ++d.matGen;
}
static __attribute__((noinline, cold)) void gxq_foreign_heal(GxqCtx& R) {
    if (!__atomic_exchange_n(&sForeignHeal, 0u, __ATOMIC_ACQ_REL)) return;
    gxq_vtx_rearm(R);
    printf("[VITA GXQ] an off-lock worker reached the game thread's recorder: every shadow forgotten and the vertex state "
           "re-sent; drawrec lanes off for good\n");
}
// Ends the channel rule.
static __attribute__((noinline)) void gxq_chan_rule_off(GxqCtx& R, const char* why) {
    if (!R.d.chanRule) return;
    gChanRule = 0; gGenOnEmit = 0;
    R.d.chanRule = 0; R.d.genOnEmit = 0;
    for (GxqSh<6>& s : R.d.chanCtrl) s.ep = 0;
    ++R.d.stateGen[PCGX_GEN_CHAN];
    ++R.d.stateGenEmit[PCGX_GEN_CHAN];
    if (gxq_may_print(R)) printf("[VITA GXQ] channel shadow rule OFF (generation-on-emit with it): %s\n", why);
    else gxq_defer(R, kGxqDeferChanRule, why);
}
// Channel rule disqualifier.
static uint32_t sDlRegWritesAtStart = 0;   // not recording
static inline void gxq_chan_rule_guard(GxqCtx& R) {
    if (!R.d.chanRule) return;
    if (__builtin_expect(gxq_rd_dl_reg_writes(R) == sDlRegWritesAtStart, 1)) return;
    gxq_chan_rule_off(R, "a display list wrote a BP/XF register (the channel fields may have moved behind the shadows)");
}
static inline void gxq_shadow_check(GxqCtx& R) {
    const uint32_t stale = gxq_rd_stale(R);
    if (R.d.staleSeen != stale) {
        R.d.staleSeen = stale;
        gxq_shadow_reset(R);
        if (__builtin_expect(sForeignHeal != 0u, 0) && R.isMain) gxq_foreign_heal(R);
    }
    gxq_chan_rule_guard(R);
}
// Translator: a decoded display list wrote a register.
void vita_gxq_note_dl_register_write(void) {
    ++sDlRegWrites;
    __atomic_add_fetch(&sStaleGen, 1u, __ATOMIC_RELEASE);
}
// DGXGraphics::setMaterial: a register-list material was recorded.
GXQ_REC void gxb_note_register_list(GxqCtx& R) {
    if (R.d.chanRule) gxq_chan_rule_off(R, "a register-list material was recorded (its list may write the channel registers)");
}
void gxr_note_register_list(GxqCtx& R) {
    gxb_note_register_list(R);
}

static inline uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

// Ops with no recorder, on gGxqMain.
template <class F> static inline __attribute__((always_inline, no_instrument_function)) void gxq_each(F&& f) {
    f(gGxqMain);
}

bool vita_gxq_recording(void) { return sRecording; }


// Draw-record segment hand-over counters.
static int sSegCallChunkCloses = -1;
static bool sSegCallInCurChunk = false;

// Lean core cursor.
static void gxq_sync_used(void) {
#if NECTAR_GXQ_LEAN
    if (gGxqMain.wr) {
        GxqChunk& c = sChunks[sWriteChunk];
        c.used      = uint32_t(gGxqMain.wr - c.data);
    }
#endif
}
static void close_chunk() {
    gxq_sync_used();
    GxqChunk& c = sChunks[sWriteChunk];
    if (c.used == 0) return;
    sceKernelSignalSema(sSemaReady, 1);
    sSegCallInCurChunk = false;
    // Join an open segment before waiting on its CALL chunk.
    if (sSegCallChunkCloses >= 0 && ++sSegCallChunkCloses >= 3) vita_drawrec_chunk_closed();
    sWriteChunk = (sWriteChunk + 1) % kChunks;
    { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaFree, 1, NULL); pc_prof_idle_set(pvIdle); }   // the next chunk must have been translated
    sChunks[sWriteChunk].used = 0;
#if NECTAR_GXQ_LEAN
    gGxqMain.wr  = sChunks[sWriteChunk].data;
    gGxqMain.end = sChunks[sWriteChunk].data + kChunkSize;
#endif
}
// Draw-record segment pool: 64 KB blocks, ready and free SPSC rings.
static constexpr uint32_t kSegBlockSize = 64u * 1024u;
struct GxqSegBlock {
    uint8_t* data;
    uint32_t used;    // bytes of ops
    uint16_t seg;     // owning segment
    uint8_t  last;    // last block of the segment
};
struct GxqSegRing { int* slot; uint32_t cap, head, tail; };   // SPSC; head the consumer's, tail the producer's
static GxqSegBlock* sSegBlocks = nullptr;
static int          sSegBlockCount = 0;
static GxqSegRing   sSegReadyRing, sSegFreeRing;
static SceUID       sSemaSegReady = -1, sSemaSegFree = -1, sSemaDrJ = -1;
static volatile uint32_t sSegStarved = 0;       // a roll found no free block (taken by the join)
static volatile uint32_t sSegInUse = 0;         // blocks in use
static uint16_t     sSegNext = 0;               // last segment number
static uint8_t      sSubPrimBuf[16 * 1024];     // gGxqSub immediate-mode primitive buffer
GxqCtx              gGxqSub;

static inline void gxq_ring_push(GxqSegRing& r, int v) {
    const uint32_t t = r.tail;
    r.slot[t % r.cap] = v;
    __atomic_store_n(&r.tail, t + 1u, __ATOMIC_RELEASE);
}
static inline int gxq_ring_pop(GxqSegRing& r) {
    const uint32_t h = r.head;
    if (__atomic_load_n(&r.tail, __ATOMIC_ACQUIRE) == h) return -1;
    const int v = r.slot[h % r.cap];
    __atomic_store_n(&r.head, h + 1u, __ATOMIC_RELEASE);
    return v;
}
static bool gxq_seg_pool_init(int blocks) {
    if (blocks < 4) blocks = 4;
    if (blocks > 256) blocks = 256;
    uint8_t* mem = static_cast<uint8_t*>(malloc(size_t(blocks) * kSegBlockSize));
    sSegBlocks   = static_cast<GxqSegBlock*>(calloc(size_t(blocks), sizeof(GxqSegBlock)));
    int* slots   = static_cast<int*>(malloc(size_t(2 * (blocks + 1)) * sizeof(int)));
    if (!mem || !sSegBlocks || !slots) {
        free(mem); free(sSegBlocks); free(slots);
        sSegBlocks = nullptr;
        printf("[VITA DRAWREC] segment pool NOT allocated (%d x 64 KB): drawrec stays off\n", blocks);
        return false;
    }
    sSemaSegReady = sceKernelCreateSema("gxq_seg_ready", 0, 0, blocks, NULL);
    sSemaSegFree  = sceKernelCreateSema("gxq_seg_free", 0, blocks, blocks, NULL);
    sSemaDrJ      = sceKernelCreateSema("drawrec_join", 0, 0, 0x7FFF, NULL);
    if (sSemaSegReady < 0 || sSemaSegFree < 0 || sSemaDrJ < 0) {
        printf("[VITA DRAWREC] segment semaphores NOT created: drawrec stays off\n");
        free(mem); free(sSegBlocks); free(slots);
        sSegBlocks = nullptr;
        return false;
    }
    sSegReadyRing = { slots, uint32_t(blocks + 1), 0u, 0u };
    sSegFreeRing  = { slots + blocks + 1, uint32_t(blocks + 1), 0u, 0u };
    for (int i = 0; i < blocks; ++i) {
        sSegBlocks[i].data = mem + size_t(i) * kSegBlockSize;
        gxq_ring_push(sSegFreeRing, i);
    }
    sSegBlockCount = blocks;
    // Segment context setup.
    GxqCtx& S   = gGxqSub;
    S.isMain    = 0;
    S.sink      = kGxqSinkSeg;
    S.recording = 1;
    S.primBuf   = sSubPrimBuf;
    S.primCap   = sizeof(sSubPrimBuf);
    S.segBlk    = -1;
    S.wr = S.end = nullptr;
    return true;
}
bool     vita_gxq_seg_pool_ready(void) { return sSegBlockCount > 0; }
unsigned vita_gxq_seg_free_blocks(void) {
    if (!sSegBlockCount) return 0u;
    const uint32_t used = __atomic_load_n(&sSegInUse, __ATOMIC_ACQUIRE);
    return used < uint32_t(sSegBlockCount) ? uint32_t(sSegBlockCount) - used : 0u;
}
unsigned vita_gxq_seg_block_bytes(void) { return kSegBlockSize; }
void vita_gxq_seg_signal_j(void) { if (sSemaDrJ >= 0) sceKernelSignalSema(sSemaDrJ, 1); }
int vita_gxq_seg_wait_j(unsigned timeoutUs) {
    if (sSemaDrJ < 0) return -1;
    SceUInt t = timeoutUs;
    const int pvIdle = pc_prof_idle_set(1);
    const int r = sceKernelWaitSema(sSemaDrJ, 1, &t);
    pc_prof_idle_set(pvIdle);
    return r < 0 ? -1 : 0;
}
bool vita_gxq_seg_take_starved(void) { return __atomic_exchange_n(&sSegStarved, 0u, __ATOMIC_ACQ_REL) != 0u; }
bool vita_gxq_seg_call_open(void) { return sSegCallInCurChunk; }
int vita_gxq_seg_call_chunk_closes(void) { return sSegCallChunkCloses; }

// Seals a segment context's current block.
static void gxq_seg_seal(GxqCtx& R, bool last) {
    if (R.segBlk < 0) return;
    GxqSegBlock& b = sSegBlocks[R.segBlk];
    b.seg  = R.seg;
    b.last = last ? 1 : 0;
    __atomic_store_n(&b.used, uint32_t(R.wr - b.data), __ATOMIC_RELEASE);
    gxq_ring_push(sSegReadyRing, R.segBlk);
    sceKernelSignalSema(sSemaSegReady, 1);
    R.segBlk = -1;
    R.wr = R.end = nullptr;
}
// Free block for a segment context.
static void gxq_seg_take(GxqCtx& R) {
    if (sceKernelPollSema(sSemaSegFree, 1) < 0) {
        __atomic_store_n(&sSegStarved, 1u, __ATOMIC_RELEASE);
        vita_gxq_seg_signal_j();
        if (sceKernelGetThreadId() == sGameThread) {
            // Inline recorder: close the chunk here.
            if (sSegCallInCurChunk) close_chunk();
        }
        const int pvIdle = pc_prof_idle_set(1);
        sceKernelWaitSema(sSemaSegFree, 1, NULL);
        pc_prof_idle_set(pvIdle);
    }
    int b = gxq_ring_pop(sSegFreeRing);
    if (b < 0) {
        while ((b = gxq_ring_pop(sSegFreeRing)) < 0) sceKernelDelayThread(50);
    }
    __atomic_add_fetch(&sSegInUse, 1u, __ATOMIC_ACQ_REL);
    R.segBlk = b;
    R.wr     = sSegBlocks[b].data;
    R.end    = sSegBlocks[b].data + kSegBlockSize;
}
// Room for one op of need bytes in a segment block.
static uint8_t* gxq_seg_room(GxqCtx& R, uint32_t need) {
    if (R.segBlk < 0 || R.wr + need > R.end) {
        if (R.segBlk >= 0) gxq_seg_seal(R, false);
        gxq_seg_take(R);
    }
    uint8_t* h = R.wr;
    R.wr += need;
    return h;
}
// Frame-level op in a segment context: deferred.
static __attribute__((noinline, cold)) uint8_t* gxq_seg_reject(GxqCtx& R, GxqOp op) {
    R.deferred |= kGxqDeferMainOnly;
    if (!R.deferredWhy) R.deferredWhy = "a frame-level op (present, sync, begin-frame, copy or a nested call) reached the segment recorder";
    gDrawrecKill = 1;
    (void)op;
    return nullptr;
}

// Immediate-mode primitive stream; consecutive independent-vertex primitives merge.
enum : uint8_t { PS_POSITION = 1, PS_COLOR = 2, PS_TEXCOORD = 3, PS_PUSH_F32 = 4, PS_RAW8 = 5 };
#if NECTAR_GXQ_LEAN
static_assert(int(PS_POSITION) == int(kGxqPsPosition) && int(PS_COLOR) == int(kGxqPsColor) && int(PS_TEXCOORD) == int(kGxqPsTexcoord),
              "vita_gxq_rec.h's immediate-mode tags are the stream's");
#endif
static void prim_stream_emit(GxqCtx& R, bool end);
// Flush a pending primitive before any other op.
static __attribute__((noinline)) void gxq_prim_close_for_op(GxqCtx& R) {
    if (R.primEnded) { prim_stream_emit(R, true); R.primOpen = false; R.primEnded = false; }
    else prim_stream_emit(R, false);
}
static uint8_t* gxq_emit(GxqCtx& R, GxqOp op, uint32_t payload) {
    // Off-lock workers never write here.
    if (__builtin_expect(vita_offlock_worker(), 0) && R.isMain) return gxq_foreign(R);
    if (R.primOpen && op != GXQ_PRIM_STREAM) gxq_prim_close_for_op(R);
    if (payload > 0xFFF0u) {
        if (!gxq_may_print(R)) { gxq_defer(R, kGxqDeferPayload, "payload too large"); return nullptr; }
        static bool said = false;   // logged once
        if (!said) { said = true; printf("[VITA GXQ] payload too large: %u bytes (op %u)\n", (unsigned)payload, (unsigned)op); }
        return nullptr;
    }
    const uint32_t need = uint32_t(sizeof(GxqHeader)) + align4(payload);
    uint8_t* h;
    if (__builtin_expect(R.sink == kGxqSinkRing, 1)) {
#if NECTAR_GXQ_LEAN
        // Lean core cursor.
        h = R.wr;
        if (h + need > R.end) { close_chunk(); h = R.wr; }
        R.wr = h + need;
#else
        GxqChunk* c = &sChunks[sWriteChunk];
        if (c->used + need > kChunkSize) {
            close_chunk();
            c = &sChunks[sWriteChunk];
        }
        h = c->data + c->used;
        c->used += need;
#endif
    }
    else if (R.sink == kGxqSinkSeg) {
        // Segment blocks.
        if (op == GXQ_PRESENT || op == GXQ_SYNC || op == GXQ_BEGIN_FRAME || op == GXQ_COPY_DISP || op == GXQ_CALL_SUBSTREAM)
            return gxq_seg_reject(R, op);
        h = gxq_seg_room(R, need);
        R.bytes += need;
    }
    else {
        __builtin_trap();
    }
    const uint32_t hdr = uint32_t(op) | (align4(payload) << 16);   // GxqHeader { op, size }, little-endian
    memcpy(h, &hdr, 4);
    ++R.ops;
    return h + sizeof(GxqHeader);
}

#if NECTAR_GXQ_LEAN
// Lean core emit
static inline __attribute__((always_inline, no_instrument_function)) uint8_t* gxq_put(GxqCtx& R, GxqOp op, uint32_t bytes) {
    if (__builtin_expect(R.sink != kGxqSinkRing, 0)) return gxq_emit(R, op, bytes);
    if (R.primOpen) gxq_prim_close_for_op(R);
    uint8_t* w = R.wr;
    if (__builtin_expect(w + 4u + bytes > R.end, 0)) {
        close_chunk();
        w = R.wr;
    }
    const uint32_t hdr = uint32_t(op) | (bytes << 16);
    memcpy(w, &hdr, 4);
    R.wr = w + 4u + bytes;
    ++R.ops;
    return w + 4;
}
// Slow-half shadow stores.
template <int N> static inline __attribute__((no_instrument_function)) void gxq_st1(GxqCtx& R, GxqSh<N>& s, uint32_t a0) {
    s.v[0] = int32_t(a0);
    s.ep   = R.d.ep;
}
template <int N> static inline __attribute__((no_instrument_function)) void gxq_st2(GxqCtx& R, GxqSh<N>& s, uint32_t a0, uint32_t a1) {
    s.v[0] = int32_t(a0); s.v[1] = int32_t(a1);
    s.ep   = R.d.ep;
}
template <int N> static inline __attribute__((no_instrument_function)) void gxq_st3(GxqCtx& R, GxqSh<N>& s, uint32_t a0, uint32_t a1, uint32_t a2) {
    s.v[0] = int32_t(a0); s.v[1] = int32_t(a1); s.v[2] = int32_t(a2);
    s.ep   = R.d.ep;
}
template <int N> static inline __attribute__((no_instrument_function)) void gxq_st4(GxqCtx& R, GxqSh<N>& s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    s.v[0] = int32_t(a0); s.v[1] = int32_t(a1); s.v[2] = int32_t(a2); s.v[3] = int32_t(a3);
    s.ep   = R.d.ep;
}
template <int N> static inline __attribute__((no_instrument_function)) void gxq_st5(GxqCtx& R, GxqSh<N>& s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    s.v[0] = int32_t(a0); s.v[1] = int32_t(a1); s.v[2] = int32_t(a2); s.v[3] = int32_t(a3); s.v[4] = int32_t(a4);
    s.ep   = R.d.ep;
}
static inline __attribute__((no_instrument_function)) void gxq_st12(GxqCtx& R, GxqSh<12>& s, const void* m) {
    memcpy(s.v, m, 48);
    s.ep = R.d.ep;
}
#endif

static inline void wr_i32(uint8_t*& p, int32_t v)  { memcpy(p, &v, 4); p += 4; }
static inline void wr_u32(uint8_t*& p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
static inline void wr_f32(uint8_t*& p, float v)    { memcpy(p, &v, 4); p += 4; }
static inline int32_t  rd_i32(const uint8_t*& q) { int32_t v;  memcpy(&v, q, 4); q += 4; return v; }
static inline uint32_t rd_u32(const uint8_t*& q) { uint32_t v; memcpy(&v, q, 4); q += 4; return v; }
static inline float    rd_f32(const uint8_t*& q) { float v;    memcpy(&v, q, 4); q += 4; return v; }

// Vertex descriptor and formats, sent as a diff before use.
static void flush_vtx_desc(GxqCtx& R) {
    GxqDecision& d = R.d;
    if (!d.pendDirty) return;
    if (gxq_refuse(R)) return;   // off-lock worker
    d.pendDirty = false;
    for (uint32_t m = d.pendDescMask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        if (d.pendDesc[i] == d.shDesc[i]) continue;
        d.shDesc[i] = d.pendDesc[i];
        uint8_t* p = gxq_emit(R, GXQ_SET_VTX_DESC, 8);
        if (!p) return;
        wr_i32(p, i); wr_i32(p, (int32_t)d.pendDesc[i]);
    }
    for (int f = 0; f < GX_MAX_VTXFMT; ++f) {
        for (uint32_t m = d.pendFmtMask[f]; m; m &= m - 1) {
            const int a = __builtin_ctz(m);
            if (d.pendFmt[f][a] == d.shFmt[f][a]) continue;
            d.shFmt[f][a] = d.pendFmt[f][a];
            const uint32_t packed = d.pendFmt[f][a];
            uint8_t* p = gxq_emit(R, GXQ_SET_VTX_ATTR_FMT, 20);
            if (!p) return;
            wr_i32(p, f); wr_i32(p, a); wr_i32(p, (int32_t)(packed >> 16)); wr_i32(p, (int32_t)((packed >> 8) & 0xFFu)); wr_i32(p, (int32_t)(packed & 0xFFu));
        }
    }
    d.pendDescMask = 0;
    memset(d.pendFmtMask, 0, sizeof(d.pendFmtMask));
}

// S1: vertex-state images
static_assert(GX_VA_MAX_ATTR == 26, "GxqVtxImage (vita_gxq.h) holds 26 attributes");
static inline void vtx_image_apply(const GxqVtxImage& img, uint32_t* pendDesc, uint32_t (*pendFmt)[GX_VA_MAX_ATTR],
                                   uint32_t& descMask, uint32_t* fmtMask, bool& dirty) {
    uint32_t ch = 0;
    for (uint32_t m = img.descMask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        if (pendDesc[i] != img.desc[i]) { pendDesc[i] = img.desc[i]; ch |= 1u << i; }
    }
    if (ch) { descMask |= ch; dirty = true; }
    if (img.fmtMask && img.fmtIdx < (uint32_t)GX_MAX_VTXFMT) {
        uint32_t* pf = pendFmt[img.fmtIdx];
        uint32_t fch = 0;
        for (uint32_t m = img.fmtMask; m; m &= m - 1) {
            const int a = __builtin_ctz(m);
            if (pf[a] != img.fmt[a]) { pf[a] = img.fmt[a]; fch |= 1u << a; }
        }
        if (fch) { fmtMask[img.fmtIdx] |= fch; dirty = true; }
    }
}

GXQ_REC int gxb_vtx_image(GxqCtx& R, const GxqVtxImage& img) {
    if (!R.d.vtxImageOn) return kGxqVtxLegacy;   // (only ever set once recording, see vita_gxq_start)
    GxqDecision& d = R.d;
    vtx_image_apply(img, d.pendDesc, d.pendFmt, d.pendDescMask, d.pendFmtMask, d.pendDirty);
    d.vtxGen += img.gens;
    return kGxqVtxApplied;
}

// DGXGraphics entry.
int gxr_vtx_image(GxqCtx& R, const GxqVtxImage& img) {
    if (gxq_refuse(R)) return kGxqVtxApplied;   // off-lock worker
    return gxb_vtx_image(R, img);
}

// S4: channel rule record-side step. Returns true when dropped.
static bool gxq_chan_rule(GxqCtx& R, int slot, const int32_t* a) {
    gxq_chan_rule_guard(R);
    if (!R.d.chanRule) return false;
    const int  n  = slot & 1;
    GxqSh<6>& sh = R.d.chanCtrl[slot];
    if (gxq_valid(R, sh) && memcmp(sh.v, a, 6 * sizeof(int32_t)) == 0) return true;
    // Sent: this id and the ids it rewrites.
    memcpy(sh.v, a, 6 * sizeof(int32_t)); sh.ep = R.d.ep;
    if (slot >= 4) {
        GxqSh<6>& c  = R.d.chanCtrl[n];     memcpy(c.v, a, 6 * sizeof(int32_t));  c.ep  = R.d.ep;
        GxqSh<6>& al = R.d.chanCtrl[2 + n]; memcpy(al.v, a, 6 * sizeof(int32_t)); al.ep = R.d.ep;
    } else {
        R.d.chanCtrl[4 + n].ep = 0;
    }
    return false;
}

// Recorders (game thread)
GXQ_REC void rec_begin_frame(GxqCtx& R) {
    if (!sRecording) { pc_gfx_begin_frame_impl(); return; }
    // Stage latches.
    R.d.vtxImageOn = gVtxImageOn; R.d.chanRule = gChanRule; R.d.genOnEmit = gGenOnEmit;
    gxq_shadow_check(R);
    uint8_t* p = gxq_emit(R, GXQ_BEGIN_FRAME, 0);
    if (!p) return;
    (void)p;
}
void pc_gfx_begin_frame(void) {
    rec_begin_frame(gGxqMain);
}

void pc_gfx_perf_scope_begin(const char* name) {
    if (!sRecording) { pc_gfx_perf_scope_begin_impl(name); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_PERF_SCOPE_BEGIN, 4);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)name);
    });
}

void pc_gfx_perf_scope_end(void) {
    if (!sRecording) { pc_gfx_perf_scope_end_impl(); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_PERF_SCOPE_END, 0);
        if (!p) return;
        (void)p;
    });
}

void pc_gfx_set_render_scale(float scale) {
    if (!sRecording) { pc_gfx_set_render_scale_impl(scale); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_RENDER_SCALE, 4);
        if (!p) return;
        wr_f32(p, (float)scale);
    });
}

void pc_gfx_set_aspect_ratio_mode(int mode) {
    if (!sRecording) { pc_gfx_set_aspect_ratio_mode_impl(mode); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_ASPECT_RATIO_MODE, 4);
        if (!p) return;
        wr_i32(p, (int32_t)mode);
    });
}

GXQ_REC void rec_set_projection(GxqCtx& R, const Mtx44 mtx, GXProjectionType type) {
    if (!sRecording) { pc_gfx_set_projection_impl(mtx, type); return; }
    if (mtx) {
        int32_t a[17];
        memcpy(a, mtx, 64);
        a[16] = (int32_t)type;
        if (R.d.projEp == R.d.ep && memcmp(R.d.proj, a, sizeof(a)) == 0) return;
        memcpy(R.d.proj, a, sizeof(a));
        R.d.projEp = R.d.ep;
    }
    uint8_t* p = gxq_emit(R, GXQ_SET_PROJECTION, 68);
    if (!p) return;
    memcpy(p, mtx, 64); p += 64;
    wr_i32(p, (int32_t)type);
}
void pc_gfx_set_projection(const Mtx44 mtx, GXProjectionType type) {
    rec_set_projection(gGxqMain, mtx, type);
}

void pc_gfx_set_viewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ) {
    if (!sRecording) { pc_gfx_set_viewport_impl(xOrig, yOrig, wd, ht, nearZ, farZ); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_VIEWPORT, 24);
        if (!p) return;
        wr_f32(p, (float)xOrig);
        wr_f32(p, (float)yOrig);
        wr_f32(p, (float)wd);
        wr_f32(p, (float)ht);
        wr_f32(p, (float)nearZ);
        wr_f32(p, (float)farZ);
    });
}

void pc_gfx_set_scissor(u32 xOrig, u32 yOrig, u32 wd, u32 ht) {
    if (!sRecording) { pc_gfx_set_scissor_impl(xOrig, yOrig, wd, ht); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_SCISSOR, 16);
        if (!p) return;
        wr_i32(p, (int32_t)xOrig);
        wr_i32(p, (int32_t)yOrig);
        wr_i32(p, (int32_t)wd);
        wr_i32(p, (int32_t)ht);
    });
}

GXQ_REC void rec_load_pos_mtx(GxqCtx& R, const Mtx mtx, u32 id) {
    if (!sRecording) { pc_gfx_load_pos_mtx_impl(mtx, id); return; }
    if (mtx && id < 64 && gxq_same(R, R.d.posMtx[id], 12, (const int32_t*)mtx)) return;
    uint8_t* p = gxq_emit(R, GXQ_LOAD_POS_MTX, 52);
    if (!p) return;
    memcpy(p, mtx, 48); p += 48;
    wr_i32(p, (int32_t)id);
}
void pc_gfx_load_pos_mtx(const Mtx mtx, u32 id) {
    GXQ_HOT_REC(load_pos_mtx)(gGxqMain, mtx, id);
}

GXQ_REC void rec_set_current_mtx(GxqCtx& R, u32 id) {
    if (!sRecording) { pc_gfx_set_current_mtx_impl(id); return; }
    { const int32_t a[1] = { (int32_t)id }; if (gxq_same(R, R.d.curMtx, 1, a)) return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_CURRENT_MTX, 4);
    if (!p) return;
    wr_i32(p, (int32_t)id);
}
void pc_gfx_set_current_mtx(u32 id) {
    GXQ_HOT_REC(set_current_mtx)(gGxqMain, id);
}

GXQ_REC void rec_load_nrm_mtx(GxqCtx& R, const Mtx mtx, u32 id) {
    if (!sRecording) { pc_gfx_load_nrm_mtx_impl(mtx, id); return; }
    if (mtx && id < 64 && gxq_same(R, R.d.nrmMtx[id], 12, (const int32_t*)mtx)) return;
    uint8_t* p = gxq_emit(R, GXQ_LOAD_NRM_MTX, 52);
    if (!p) return;
    memcpy(p, mtx, 48); p += 48;
    wr_i32(p, (int32_t)id);
}
void pc_gfx_load_nrm_mtx(const Mtx mtx, u32 id) {
    GXQ_HOT_REC(load_nrm_mtx)(gGxqMain, mtx, id);
}

// Matrix group position and normal loads as one op.
static constexpr int kGxqMtxGroupMax = 16;
GXQ_REC void gxb_load_posnrm_group(GxqCtx& R, int n, const u32* ids, const f32 (*const* mtx)[4]) {
    if (!sRecording) {
        for (int i = 0; i < n; ++i) { pc_gfx_load_pos_mtx_impl(mtx[i], ids[i]); pc_gfx_load_nrm_mtx_impl(mtx[i], ids[i]); }
        return;
    }
    bool plain = n > kGxqMtxGroupMax;
    for (int i = 0; i < n && !plain; ++i) plain = ids[i] >= 0x40000000u;
    if (plain) {
        for (int i = 0; i < n; ++i) { rec_load_pos_mtx(R, mtx[i], ids[i]); rec_load_nrm_mtx(R, mtx[i], ids[i]); }
        return;
    }
    uint32_t words[kGxqMtxGroupMax];
    uint32_t loaded = 0;
    for (int i = 0; i < n; ++i) {
        const u32 id = ids[i];
        const int32_t* m = reinterpret_cast<const int32_t*>(mtx[i]);
        uint32_t w = id;
        if (!(id < 64 && gxq_same(R, R.d.posMtx[id], 12, m))) w |= 0x80000000u;
        if (!(id < 64 && gxq_same(R, R.d.nrmMtx[id], 12, m))) w |= 0x40000000u;
        words[i] = w;
        if (w & 0xC0000000u) ++loaded;
    }
    if (loaded == 0) return;
    uint8_t* p = gxq_emit(R, GXQ_LOAD_POSNRM_GROUP, 4u + 52u * loaded);
    if (!p) return;
    wr_u32(p, loaded);
    for (int i = 0; i < n; ++i) {
        if (!(words[i] & 0xC0000000u)) continue;
        wr_u32(p, words[i]);
        memcpy(p, mtx[i], 48); p += 48;
    }
}
#if NECTAR_GXQ_LEAN
// Lean core matrix group.
static void gxq_l_load_posnrm_group(GxqCtx& R, int n, const u32* ids, const f32 (*const* mtx)[4]) {
    if (!sRecording) {
        for (int i = 0; i < n; ++i) { pc_gfx_load_pos_mtx_impl(mtx[i], ids[i]); pc_gfx_load_nrm_mtx_impl(mtx[i], ids[i]); }
        return;
    }
    bool plain = n > kGxqMtxGroupMax;
    for (int i = 0; i < n && !plain; ++i) plain = ids[i] >= 0x40000000u;
    if (plain) {
        for (int i = 0; i < n; ++i) { gxq_r_load_pos_mtx(R, mtx[i], ids[i]); gxq_r_load_nrm_mtx(R, mtx[i], ids[i]); }
        return;
    }
    uint32_t words[kGxqMtxGroupMax];
    uint32_t loaded = 0;
    for (int i = 0; i < n; ++i) {
        const u32 id = ids[i];
        const f32 (*m)[4] = mtx[i];
        uint32_t w = id;
        if (!(id < 64u && gxq_hit12(R, R.d.posMtx[id], m))) { if (id < 64u) gxq_st12(R, R.d.posMtx[id], m); w |= 0x80000000u; }
        if (!(id < 64u && gxq_hit12(R, R.d.nrmMtx[id], m))) { if (id < 64u) gxq_st12(R, R.d.nrmMtx[id], m); w |= 0x40000000u; }
        words[i] = w;
        if (w & 0xC0000000u) ++loaded;
    }
    if (loaded == 0) return;
    uint8_t* p = gxq_put(R, GXQ_LOAD_POSNRM_GROUP, 4u + 52u * loaded);
    if (!p) return;
    wr_u32(p, loaded);
    for (int i = 0; i < n; ++i) {
        if (!(words[i] & 0xC0000000u)) continue;
        wr_u32(p, words[i]);
        memcpy(p, mtx[i], 48); p += 48;
    }
}
#endif
void gxr_load_posnrm_group(GxqCtx& R, int n, const u32* ids, const f32 (*const* mtx)[4]) {
    if (gxq_refuse(R)) return;   // off-lock worker
#if NECTAR_GXQ_LEAN
    gxq_l_load_posnrm_group(R, n, ids, mtx);
#else
    gxb_load_posnrm_group(R, n, ids, mtx);
#endif
}

GXQ_REC void rec_load_tex_mtx(GxqCtx& R, const Mtx mtx, u32 id) {
    if (!sRecording) { pc_gfx_load_tex_mtx_impl(mtx, id); return; }
    if (mtx && id < 64 && gxq_same(R, R.d.texMtx[id], 12, (const int32_t*)mtx)) return;
    uint8_t* p = gxq_emit(R, GXQ_LOAD_TEX_MTX, 52);
    if (!p) return;
    memcpy(p, mtx, 48); p += 48;
    wr_i32(p, (int32_t)id);
}
void pc_gfx_load_tex_mtx(const Mtx mtx, u32 id) {
    GXQ_HOT_REC(load_tex_mtx)(gGxqMain, mtx, id);
}

GXQ_REC void rec_set_tex_coord_gen(GxqCtx& R, GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx) {
    if (!sRecording) { pc_gfx_set_tex_coord_gen_impl(coord, type, src, matrixIdx); return; }
    if (coord >= 0 && coord < 8) { const int32_t a[3] = { (int32_t)type, (int32_t)src, (int32_t)matrixIdx }; if (gxq_same(R, R.d.texGen[coord], 3, a)) return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_TEX_COORD_GEN, 16);
    if (!p) return;
    wr_i32(p, (int32_t)coord);
    wr_i32(p, (int32_t)type);
    wr_i32(p, (int32_t)src);
    wr_i32(p, (int32_t)matrixIdx);
}
void pc_gfx_set_tex_coord_gen(GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx) {
    GXQ_HOT_REC(set_tex_coord_gen)(gGxqMain, coord, type, src, matrixIdx);
}

GXQ_REC void rec_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    ++R.d.stateGen[PCGX_GEN_PE];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_PE); pc_gfx_set_z_mode_impl(compareEnable, func, updateEnable); return; }
    { const int32_t a[3] = { (int32_t)compareEnable, (int32_t)func, (int32_t)updateEnable }; if (gxq_same(R, R.d.zMode, 3, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_PE);
    uint8_t* p = gxq_emit(R, GXQ_SET_Z_MODE, 12);
    if (!p) return;
    wr_i32(p, (int32_t)compareEnable);
    wr_i32(p, (int32_t)func);
    wr_i32(p, (int32_t)updateEnable);
}
void pc_gfx_set_z_mode(GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    GXQ_HOT_REC(set_z_mode)(gGxqMain, compareEnable, func, updateEnable);
}

GXQ_REC void rec_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    ++R.d.stateGen[PCGX_GEN_PE];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_PE); pc_gfx_set_blend_mode_impl(type, srcFactor, dstFactor, op); return; }
    { const int32_t a[4] = { (int32_t)type, (int32_t)srcFactor, (int32_t)dstFactor, (int32_t)op }; if (gxq_same(R, R.d.blend, 4, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_PE);
    uint8_t* p = gxq_emit(R, GXQ_SET_BLEND_MODE, 16);
    if (!p) return;
    wr_i32(p, (int32_t)type);
    wr_i32(p, (int32_t)srcFactor);
    wr_i32(p, (int32_t)dstFactor);
    wr_i32(p, (int32_t)op);
}
void pc_gfx_set_blend_mode(GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    GXQ_HOT_REC(set_blend_mode)(gGxqMain, type, srcFactor, dstFactor, op);
}

GXQ_REC void rec_set_cull_mode(GxqCtx& R, GXCullMode mode) {
    if (!sRecording) { pc_gfx_set_cull_mode_impl(mode); return; }
    { const int32_t a[1] = { (int32_t)mode }; if (gxq_same(R, R.d.cull, 1, a)) return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_CULL_MODE, 4);
    if (!p) return;
    wr_i32(p, (int32_t)mode);
}
void pc_gfx_set_cull_mode(GXCullMode mode) {
    GXQ_HOT_REC(set_cull_mode)(gGxqMain, mode);
}

GXQ_REC void rec_set_color_update(GxqCtx& R, GXBool updateEnable) {
    if (!sRecording) { pc_gfx_set_color_update_impl(updateEnable); return; }
    { const int32_t a[1] = { (int32_t)updateEnable }; if (gxq_same(R, R.d.colorUpd, 1, a)) return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_COLOR_UPDATE, 4);
    if (!p) return;
    wr_i32(p, (int32_t)updateEnable);
}
void pc_gfx_set_color_update(GXBool updateEnable) {
    GXQ_HOT_REC(set_color_update)(gGxqMain, updateEnable);
}

GXQ_REC void rec_set_alpha_update(GxqCtx& R, GXBool updateEnable) {
    if (!sRecording) { pc_gfx_set_alpha_update_impl(updateEnable); return; }
    { const int32_t a[1] = { (int32_t)updateEnable }; if (gxq_same(R, R.d.alphaUpd, 1, a)) return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_ALPHA_UPDATE, 4);
    if (!p) return;
    wr_i32(p, (int32_t)updateEnable);
}
void pc_gfx_set_alpha_update(GXBool updateEnable) {
    GXQ_HOT_REC(set_alpha_update)(gGxqMain, updateEnable);
}

GXQ_REC void rec_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    ++R.d.stateGen[PCGX_GEN_PE];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_PE); pc_gfx_set_alpha_compare_impl(comp0, ref0, op, comp1, ref1); return; }
    { const int32_t a[5] = { (int32_t)comp0, (int32_t)ref0, (int32_t)op, (int32_t)comp1, (int32_t)ref1 }; if (gxq_same(R, R.d.alphaCmp, 5, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_PE);
    uint8_t* p = gxq_emit(R, GXQ_SET_ALPHA_COMPARE, 20);
    if (!p) return;
    wr_i32(p, (int32_t)comp0);
    wr_i32(p, (int32_t)ref0);
    wr_i32(p, (int32_t)op);
    wr_i32(p, (int32_t)comp1);
    wr_i32(p, (int32_t)ref1);
}
void pc_gfx_set_alpha_compare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    GXQ_HOT_REC(set_alpha_compare)(gGxqMain, comp0, ref0, op, comp1, ref1);
}

GXQ_REC void rec_set_chan_ctrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask,
                                     GXDiffuseFn diffFn, GXAttnFn attnFn) {
    ++R.d.stateGen[PCGX_GEN_CHAN];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_CHAN); pc_gfx_set_chan_ctrl_impl(chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn); return; }
    {
        const int slot = gxq_chan_slot((int)chan);
        if (slot >= 0) {
            const int32_t a[6] = { (int32_t)enable, (int32_t)ambSrc, (int32_t)matSrc, (int32_t)lightMask, (int32_t)diffFn, (int32_t)attnFn };
            if (R.d.chanRule) {
                // Channel shadow rule (S4).
                if (gxq_chan_rule(R, slot, a)) return;
            } else {
                if (gxq_same(R, R.d.chanCtrl[slot], 6, a)) return;
                for (int s = slot & 1; s < 6; s += 2) if (s != slot) R.d.chanCtrl[s].ep = 0;   // the other ids of this channel share fields
            }
        }
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_CHAN);
    uint8_t* p = gxq_emit(R, GXQ_SET_CHAN_CTRL, 28);
    if (!p) return;
    wr_i32(p, (int32_t)chan);
    wr_i32(p, (int32_t)enable);
    wr_i32(p, (int32_t)ambSrc);
    wr_i32(p, (int32_t)matSrc);
    wr_i32(p, (int32_t)lightMask);
    wr_i32(p, (int32_t)diffFn);
    wr_i32(p, (int32_t)attnFn);
}
void pc_gfx_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn) {
    rec_set_chan_ctrl(gGxqMain, chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}

GXQ_REC void rec_set_chan_mat_color(GxqCtx& R, GXChannelID chan, GXColor color) {
    if (!sRecording) { pc_gfx_set_chan_mat_color_impl(chan, color); return; }
    { const int slot = gxq_chan_slot((int)chan); if (slot >= 0) { int32_t a[1]; memcpy(a, &color, 4); if (gxq_same(R, R.d.chanMat[slot & 1], 1, a)) return; } }
    uint8_t* p = gxq_emit(R, GXQ_SET_CHAN_MAT_COLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)chan);
    memcpy(p, &color, 4); p += 4;
}
void pc_gfx_set_chan_mat_color(GXChannelID chan, GXColor color) {
    GXQ_HOT_REC(set_chan_mat_color)(gGxqMain, chan, color);
}

GXQ_REC void rec_set_chan_amb_color(GxqCtx& R, GXChannelID chan, GXColor color) {
    if (!sRecording) { pc_gfx_set_chan_amb_color_impl(chan, color); return; }
    { const int slot = gxq_chan_slot((int)chan); if (slot >= 0) { int32_t a[1]; memcpy(a, &color, 4); if (gxq_same(R, R.d.chanAmb[slot & 1], 1, a)) return; } }
    uint8_t* p = gxq_emit(R, GXQ_SET_CHAN_AMB_COLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)chan);
    memcpy(p, &color, 4); p += 4;
}
void pc_gfx_set_chan_amb_color(GXChannelID chan, GXColor color) {
    GXQ_HOT_REC(set_chan_amb_color)(gGxqMain, chan, color);
}

GXQ_REC void rec_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_order_impl(stage, coord, map, chan); return; }
    if (stage >= 0 && stage < 16) { const int32_t a[3] = { (int32_t)coord, (int32_t)map, (int32_t)chan }; if (gxq_same(R, R.d.tevOrder[stage], 3, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_ORDER, 16);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)coord);
    wr_i32(p, (int32_t)map);
    wr_i32(p, (int32_t)chan);
}
void pc_gfx_set_tev_order(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    GXQ_HOT_REC(set_tev_order)(gGxqMain, stage, coord, map, chan);
}

GXQ_REC void rec_set_tev_op(GxqCtx& R, GXTevStageID stage, GXTevMode mode) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_op_impl(stage, mode); return; }
    if (stage >= 0 && stage < 16) {
        const int32_t a[1] = { (int32_t)mode };
        if (gxq_same(R, R.d.tevOp[stage], 1, a)) return;
        R.d.tevCIn[stage].ep = R.d.tevAIn[stage].ep = R.d.tevCOp[stage].ep = R.d.tevAOp[stage].ep = 0;   // GXSetTevOp rewrites all four
        // GX_PASSCLR also sets the stage's texture map.
        R.d.tevOrder[stage].ep = 0;
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_OP, 8);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)mode);
}
void pc_gfx_set_tev_op(GXTevStageID stage, GXTevMode mode) {
    rec_set_tev_op(gGxqMain, stage, mode);
}

GXQ_REC void rec_set_num_tev_stages(GxqCtx& R, u8 num) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_num_tev_stages_impl(num); return; }
    { const int32_t a[1] = { (int32_t)num }; if (gxq_same(R, R.d.numStages, 1, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_NUM_TEV_STAGES, 4);
    if (!p) return;
    wr_i32(p, (int32_t)num);
}
void pc_gfx_set_num_tev_stages(u8 num) {
    GXQ_HOT_REC(set_num_tev_stages)(gGxqMain, num);
}

GXQ_REC void rec_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_color_in_impl(stage, a, b, c, d); return; }
    if (stage >= 0 && stage < 16) { const int32_t v[4] = { (int32_t)a, (int32_t)b, (int32_t)c, (int32_t)d }; if (gxq_same(R, R.d.tevCIn[stage], 4, v)) return; R.d.tevOp[stage].ep = 0; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_COLOR_IN, 20);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)a);
    wr_i32(p, (int32_t)b);
    wr_i32(p, (int32_t)c);
    wr_i32(p, (int32_t)d);
}
void pc_gfx_set_tev_color_in(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    GXQ_HOT_REC(set_tev_color_in)(gGxqMain, stage, a, b, c, d);
}

GXQ_REC void rec_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_alpha_in_impl(stage, a, b, c, d); return; }
    if (stage >= 0 && stage < 16) { const int32_t v[4] = { (int32_t)a, (int32_t)b, (int32_t)c, (int32_t)d }; if (gxq_same(R, R.d.tevAIn[stage], 4, v)) return; R.d.tevOp[stage].ep = 0; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_ALPHA_IN, 20);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)a);
    wr_i32(p, (int32_t)b);
    wr_i32(p, (int32_t)c);
    wr_i32(p, (int32_t)d);
}
void pc_gfx_set_tev_alpha_in(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    GXQ_HOT_REC(set_tev_alpha_in)(gGxqMain, stage, a, b, c, d);
}

GXQ_REC void rec_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_color_op_impl(stage, op, bias, scale, clamp, outReg); return; }
    if (stage >= 0 && stage < 16) { const int32_t v[5] = { (int32_t)op, (int32_t)bias, (int32_t)scale, (int32_t)clamp, (int32_t)outReg }; if (gxq_same(R, R.d.tevCOp[stage], 5, v)) return; R.d.tevOp[stage].ep = 0; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_COLOR_OP, 24);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)op);
    wr_i32(p, (int32_t)bias);
    wr_i32(p, (int32_t)scale);
    wr_i32(p, (int32_t)clamp);
    wr_i32(p, (int32_t)outReg);
}
void pc_gfx_set_tev_color_op(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXQ_HOT_REC(set_tev_color_op)(gGxqMain, stage, op, bias, scale, clamp, outReg);
}

GXQ_REC void rec_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_alpha_op_impl(stage, op, bias, scale, clamp, outReg); return; }
    if (stage >= 0 && stage < 16) { const int32_t v[5] = { (int32_t)op, (int32_t)bias, (int32_t)scale, (int32_t)clamp, (int32_t)outReg }; if (gxq_same(R, R.d.tevAOp[stage], 5, v)) return; R.d.tevOp[stage].ep = 0; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_ALPHA_OP, 24);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)op);
    wr_i32(p, (int32_t)bias);
    wr_i32(p, (int32_t)scale);
    wr_i32(p, (int32_t)clamp);
    wr_i32(p, (int32_t)outReg);
}
void pc_gfx_set_tev_alpha_op(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXQ_HOT_REC(set_tev_alpha_op)(gGxqMain, stage, op, bias, scale, clamp, outReg);
}

GXQ_REC void rec_set_tev_color(GxqCtx& R, GXTevRegID reg, GXColor color) {
    ++R.d.stateGen[PCGX_GEN_TEVREG];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG); pc_gfx_set_tev_color_impl(reg, color); return; }
    if (reg >= 0 && reg < 4) { int32_t a[2] = { 0, 0 }; memcpy(&a[1], &color, 4); if (gxq_same(R, R.d.tevReg[reg], 2, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_COLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)reg);
    memcpy(p, &color, 4); p += 4;
}
void pc_gfx_set_tev_color(GXTevRegID reg, GXColor color) {
    GXQ_HOT_REC(set_tev_color)(gGxqMain, reg, color);
}

GXQ_REC void rec_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) {
    ++R.d.stateGen[PCGX_GEN_TEVREG];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG); pc_gfx_set_tev_color_s10_impl(reg, color); return; }
    if (reg >= 0 && reg < 4) { int32_t a[3] = { 1, 0, 0 }; memcpy(&a[1], &color, 8); if (gxq_same(R, R.d.tevReg[reg], 3, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_COLOR_S10, 12);
    if (!p) return;
    wr_i32(p, (int32_t)reg);
    memcpy(p, &color, 8); p += 8;
}
void pc_gfx_set_tev_color_s10(GXTevRegID reg, GXColorS10 color) {
    GXQ_HOT_REC(set_tev_color_s10)(gGxqMain, reg, color);
}

GXQ_REC void rec_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) {
    ++R.d.stateGen[PCGX_GEN_KCOL];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_KCOL); pc_gfx_set_tev_kcolor_impl(id, color); return; }
    if (id >= 0 && id < 4) { int32_t a[1]; memcpy(a, &color, 4); if (gxq_same(R, R.d.kColor[id], 1, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_KCOL);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_KCOLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)id);
    memcpy(p, &color, 4); p += 4;
}
void pc_gfx_set_tev_kcolor(GXTevKColorID id, GXColor color) {
    GXQ_HOT_REC(set_tev_kcolor)(gGxqMain, id, color);
}

GXQ_REC void rec_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_kcolor_sel_impl(stage, sel); return; }
    if (stage >= 0 && stage < 16) { const int32_t a[1] = { (int32_t)sel }; if (gxq_same(R, R.d.kcSel[stage], 1, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_KCOLOR_SEL, 8);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)sel);
}
void pc_gfx_set_tev_kcolor_sel(GXTevStageID stage, GXTevKColorSel sel) {
    GXQ_HOT_REC(set_tev_kcolor_sel)(gGxqMain, stage, sel);
}

GXQ_REC void rec_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) {
    ++R.d.stateGen[PCGX_GEN_TEV];
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_kalpha_sel_impl(stage, sel); return; }
    if (stage >= 0 && stage < 16) { const int32_t a[1] = { (int32_t)sel }; if (gxq_same(R, R.d.kaSel[stage], 1, a)) return; }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_KALPHA_SEL, 8);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)sel);
}
void pc_gfx_set_tev_kalpha_sel(GXTevStageID stage, GXTevKAlphaSel sel) {
    GXQ_HOT_REC(set_tev_kalpha_sel)(gGxqMain, stage, sel);
}

GXQ_REC void rec_set_tev_swap_mode(GxqCtx& R, GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
    if (!sRecording) { pc_gfx_set_tev_swap_mode_impl(stage, rasSel, texSel); return; }
    if (stage >= 0 && stage < 16) { const int32_t a[2] = { (int32_t)rasSel, (int32_t)texSel }; if (gxq_same(R, R.d.swap[stage], 2, a)) return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_SWAP_MODE, 12);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)rasSel);
    wr_i32(p, (int32_t)texSel);
}
void pc_gfx_set_tev_swap_mode(GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
    GXQ_HOT_REC(set_tev_swap_mode)(gGxqMain, stage, rasSel, texSel);
}

GXQ_REC void rec_set_tev_swap_mode_table(GxqCtx& R, GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha) {
    if (!sRecording) { pc_gfx_set_tev_swap_mode_table_impl(table, red, green, blue, alpha); return; }
    { const int t = (int)table - (int)GX_TEV_SWAP0; if (t >= 0 && t < 4) { const int32_t a[4] = { (int32_t)red, (int32_t)green, (int32_t)blue, (int32_t)alpha }; if (gxq_same(R, R.d.swapTable[t], 4, a)) return; } }
    uint8_t* p = gxq_emit(R, GXQ_SET_TEV_SWAP_MODE_TABLE, 20);
    if (!p) return;
    wr_i32(p, (int32_t)table);
    wr_i32(p, (int32_t)red);
    wr_i32(p, (int32_t)green);
    wr_i32(p, (int32_t)blue);
    wr_i32(p, (int32_t)alpha);
}
void pc_gfx_set_tev_swap_mode_table(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha) {
    GXQ_HOT_REC(set_tev_swap_mode_table)(gGxqMain, table, red, green, blue, alpha);
}

void pc_gfx_copy_tex(void* dest, GXBool clear, u16 srcX, u16 srcY, u16 srcW, u16 srcH, u16 dstW, u16 dstH, GXTexFmt fmt, GXBool mipmap) {
    if (!sRecording) { pc_gfx_copy_tex_impl(dest, clear, srcX, srcY, srcW, srcH, dstW, dstH, fmt, mipmap); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_COPY_TEX, 40);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)dest);
        wr_i32(p, (int32_t)clear);
        wr_i32(p, (int32_t)srcX); wr_i32(p, (int32_t)srcY); wr_i32(p, (int32_t)srcW); wr_i32(p, (int32_t)srcH);
        wr_i32(p, (int32_t)dstW); wr_i32(p, (int32_t)dstH);
        wr_i32(p, (int32_t)fmt); wr_i32(p, (int32_t)mipmap);
    });
}
void pc_gfx_level_flush(const void* ranges, int count) {
    if (!sRecording) { pc_gfx_level_flush_impl(ranges, count); return; }
    bool sent = false;
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_LEVEL_FLUSH, 8);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)ranges);
        wr_i32(p, (int32_t)count);
        sent = true;
    });
    if (!sent) std::free(const_cast<void*>(ranges));
}
void pc_gfx_efb_composite(int kind, const void* dest0, const void* dest1, float p0, float p1) {
    if (!sRecording) { pc_gfx_efb_composite_impl(kind, dest0, dest1, p0, p1); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_EFB_COMPOSITE, 20);
        if (!p) return;
        wr_i32(p, (int32_t)kind);
        wr_u32(p, (uint32_t)(uintptr_t)dest0);
        wr_u32(p, (uint32_t)(uintptr_t)dest1);
        wr_f32(p, p0); wr_f32(p, p1);
    });
}
void pc_gfx_efb_target(int on, int w, int h) {
    if (!sRecording) { pc_gfx_efb_target_impl(on, w, h); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_EFB_TARGET, 12);
        if (!p) return;
        wr_i32(p, (int32_t)on); wr_i32(p, (int32_t)w); wr_i32(p, (int32_t)h);
    });
}
void pc_gfx_set_copy_filter(int vertFilter, const u8 vfilter[7]) {
    if (!sRecording) { pc_gfx_set_copy_filter_impl(vertFilter, vfilter); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_COPY_FILTER, 32);
        if (!p) return;
        wr_i32(p, (int32_t)(vertFilter && vfilter ? 1 : 0));
        for (int i = 0; i < 7; ++i) wr_i32(p, vfilter ? (int32_t)vfilter[i] : 0);
    });
}
void pc_gfx_init_tex_obj_lod(GXTexObj* obj, GXTexFilter minFilt, GXTexFilter magFilt, f32 minLOD, f32 maxLOD, f32 lodBias) {
    if (!sRecording) { pc_gfx_init_tex_obj_lod_impl(obj, minFilt, magFilt, minLOD, maxLOD, lodBias); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_INIT_TEX_OBJ_LOD, 24);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)obj);
        wr_i32(p, (int32_t)minFilt);
        wr_i32(p, (int32_t)magFilt);
        wr_f32(p, minLOD);
        wr_f32(p, maxLOD);
        wr_f32(p, lodBias);
    });
}
void pc_gfx_init_tex_obj(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap) {
    if (!sRecording) { pc_gfx_init_tex_obj_impl(obj, imagePtr, width, height, format, wrapS, wrapT, mipmap); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_INIT_TEX_OBJ, 32);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)obj);
        wr_u32(p, (uint32_t)(uintptr_t)imagePtr);
        wr_i32(p, (int32_t)width);
        wr_i32(p, (int32_t)height);
        wr_i32(p, (int32_t)format);
        wr_i32(p, (int32_t)wrapS);
        wr_i32(p, (int32_t)wrapT);
        wr_i32(p, (int32_t)mipmap);
    });
}

void pc_gfx_init_tex_obj_ci(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlutName) {
    if (!sRecording) { pc_gfx_init_tex_obj_ci_impl(obj, imagePtr, width, height, format, wrapS, wrapT, mipmap, tlutName); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_INIT_TEX_OBJ_CI, 36);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)obj);
        wr_u32(p, (uint32_t)(uintptr_t)imagePtr);
        wr_i32(p, (int32_t)width);
        wr_i32(p, (int32_t)height);
        wr_i32(p, (int32_t)format);
        wr_i32(p, (int32_t)wrapS);
        wr_i32(p, (int32_t)wrapT);
        wr_i32(p, (int32_t)mipmap);
        wr_i32(p, (int32_t)tlutName);
    });
}

void pc_gfx_load_tlut(GXTlutObj* obj, u32 tlutName) {
    if (!sRecording) { pc_gfx_load_tlut_impl(obj, tlutName); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_LOAD_TLUT, 8);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)obj);
        wr_i32(p, (int32_t)tlutName);
    });
}

GXQ_REC void rec_load_tex_obj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) {
    if (!sRecording) { pc_gfx_load_tex_obj_impl(obj, id); return; }
    uint8_t* p = gxq_emit(R, GXQ_LOAD_TEX_OBJ, 8);
    if (!p) return;
    wr_u32(p, (uint32_t)(uintptr_t)obj);
    wr_i32(p, (int32_t)id);
}
void pc_gfx_load_tex_obj(GXTexObj* obj, GXTexMapID id) {
    GXQ_HOT_REC(load_tex_obj)(gGxqMain, obj, id);
}

// Immediate-mode primitives, one op each
static void prim_stream_emit(GxqCtx& R, bool end) {
    if (end && !R.primBegun && !R.primHasPos) { R.primUsed = 0; return; }
    uint8_t* p = gxq_emit(R, GXQ_PRIM_STREAM, 20 + R.primUsed);
    if (p) {
        wr_i32(p, (R.primBegun ? 0 : 1) | (end ? 2 : 0));
        wr_i32(p, R.primHdr[0]); wr_i32(p, R.primHdr[1]); wr_i32(p, R.primHdr[2]);
        wr_u32(p, R.primUsed);
        memcpy(p, R.primBuf, R.primUsed);
    }
    R.primBegun = true;
    R.primUsed = 0;
}
static inline uint8_t* prim_stream_room(GxqCtx& R, uint32_t need) {
    if (R.primUsed + need > R.primCap) prim_stream_emit(R, false);
    uint8_t* d = R.primBuf + R.primUsed;
    R.primUsed += need;
    return d;
}
// One raw FIFO byte into the open primitive, big-endian.
static inline void prim_stream_raw(GxqCtx& R, uint32_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) { uint8_t* d = prim_stream_room(R, 2); d[0] = PS_RAW8; d[1] = uint8_t(v >> (8 * i)); }
    R.primHasPos = true;
}
static inline bool prim_mergeable(int32_t type) {
    return type == (int32_t)GX_QUADS || type == (int32_t)GX_TRIANGLES || type == (int32_t)GX_LINES || type == (int32_t)GX_POINTS;
}
GXQ_REC void rec_begin(GxqCtx& R, GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    if (gxq_refuse(R)) return;   // off-lock worker
    if (!sRecording) { pc_gfx_begin_impl(type, vtxfmt, nverts); return; }
    flush_vtx_desc(R);   // descriptor change goes out first
    if (R.primOpen && R.primEnded && R.ops == R.primEndOps && R.primHdr[0] == (int32_t)type && R.primHdr[1] == (int32_t)vtxfmt && prim_mergeable((int32_t)type)) {
        R.primEnded = false;   // same primitive continues
        R.primHdr[2] += (int32_t)nverts;
        return;
    }
    if (R.primOpen) { prim_stream_emit(R, true); R.primOpen = false; R.primEnded = false; }
    R.primOpen = true; R.primBegun = false; R.primUsed = 0; R.primHasPos = false;
    R.primHdr[0] = (int32_t)type; R.primHdr[1] = (int32_t)vtxfmt; R.primHdr[2] = (int32_t)nverts;
}
void pc_gfx_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    rec_begin(gGxqMain, type, vtxfmt, nverts);
}

void pc_gfx_push_u8(u8 val) {
    if (!sRecording) { pc_gfx_push_u8_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { prim_stream_raw(R, val, 1); return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_U8, 4);
        if (!p) return;
        wr_i32(p, (int32_t)val);
    });
}

void pc_gfx_push_u16(u16 val) {
    if (!sRecording) { pc_gfx_push_u16_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { prim_stream_raw(R, val, 2); return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_U16, 4);
        if (!p) return;
        wr_i32(p, (int32_t)val);
    });
}

void pc_gfx_push_u32(u32 val) {
    if (!sRecording) { pc_gfx_push_u32_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { prim_stream_raw(R, val, 4); return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_U32, 4);
        if (!p) return;
        wr_i32(p, (int32_t)val);
    });
}

void pc_gfx_push_s8(s8 val) {
    if (!sRecording) { pc_gfx_push_s8_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { prim_stream_raw(R, (uint8_t)val, 1); return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_S8, 4);
        if (!p) return;
        wr_i32(p, (int32_t)val);
    });
}

void pc_gfx_push_s16(s16 val) {
    if (!sRecording) { pc_gfx_push_s16_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { prim_stream_raw(R, (uint16_t)val, 2); return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_S16, 4);
        if (!p) return;
        wr_i32(p, (int32_t)val);
    });
}

void pc_gfx_push_s32(s32 val) {
    if (!sRecording) { pc_gfx_push_s32_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { prim_stream_raw(R, (uint32_t)val, 4); return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_S32, 4);
        if (!p) return;
        wr_i32(p, (int32_t)val);
    });
}

void pc_gfx_push_f32(f32 val) {
    if (!sRecording) { pc_gfx_push_f32_impl(val); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        if (R.primOpen) { uint8_t* d = prim_stream_room(R, 5); d[0] = PS_PUSH_F32; const float f = (float)val; memcpy(d + 1, &f, 4); R.primHasPos = true; return; }
        uint8_t* p = gxq_emit(R, GXQ_PUSH_F32, 4);
        if (!p) return;
        wr_f32(p, (float)val);
    });
}

GXQ_REC void rec_position(GxqCtx& R, f32 x, f32 y, f32 z) {
    if (!sRecording) { pc_gfx_position_impl(x, y, z); return; }
    if (R.primOpen) { uint8_t* d = prim_stream_room(R, 13); d[0] = PS_POSITION; const float v[3] = { (float)x, (float)y, (float)z }; memcpy(d + 1, v, 12); R.primHasPos = true; return; }
    uint8_t* p = gxq_emit(R, GXQ_POSITION, 12);
    if (!p) return;
    wr_f32(p, (float)x);
    wr_f32(p, (float)y);
    wr_f32(p, (float)z);
}
void pc_gfx_position(f32 x, f32 y, f32 z) {
    GXQ_HOT_REC(position)(gGxqMain, x, y, z);
}

GXQ_REC void rec_color(GxqCtx& R, u8 r, u8 g, u8 b, u8 a) {
    if (!sRecording) { pc_gfx_color_impl(r, g, b, a); return; }
    if (R.primOpen) { uint8_t* d = prim_stream_room(R, 5); d[0] = PS_COLOR; d[1] = r; d[2] = g; d[3] = b; d[4] = a; return; }
    uint8_t* p = gxq_emit(R, GXQ_COLOR, 16);
    if (!p) return;
    wr_i32(p, (int32_t)r);
    wr_i32(p, (int32_t)g);
    wr_i32(p, (int32_t)b);
    wr_i32(p, (int32_t)a);
}
void pc_gfx_color(u8 r, u8 g, u8 b, u8 a) {
    GXQ_HOT_REC(color)(gGxqMain, r, g, b, a);
}

GXQ_REC void rec_texcoord(GxqCtx& R, f32 u, f32 v) {
    if (!sRecording) { pc_gfx_texcoord_impl(u, v); return; }
    if (R.primOpen) { uint8_t* d = prim_stream_room(R, 9); d[0] = PS_TEXCOORD; const float t[2] = { (float)u, (float)v }; memcpy(d + 1, t, 8); return; }
    uint8_t* p = gxq_emit(R, GXQ_TEXCOORD, 8);
    if (!p) return;
    wr_f32(p, (float)u);
    wr_f32(p, (float)v);
}
void pc_gfx_texcoord(f32 u, f32 v) {
    GXQ_HOT_REC(texcoord)(gGxqMain, u, v);
}

GXQ_REC void rec_end(GxqCtx& R) {
    if (gxq_refuse(R)) return;
    if (!sRecording) { pc_gfx_end_impl(); return; }
    if (R.primOpen) {
        if (prim_mergeable(R.primHdr[0])) { R.primEnded = true; R.primEndOps = R.ops; }   // held for a merge
        else { prim_stream_emit(R, true); R.primOpen = false; }
        return;
    }
    uint8_t* p = gxq_emit(R, GXQ_END, 0);
    if (!p) return;
    (void)p;
}
void pc_gfx_end(void) {
    rec_end(gGxqMain);
}

GXQ_REC void rec_call_display_list(GxqCtx& R, const void* list, u32 nbytes) {
    if (!sRecording) { pc_gfx_call_display_list_impl(list, nbytes); return; }
    gxq_shadow_check(R);
    flush_vtx_desc(R);
    uint8_t* p = gxq_emit(R, GXQ_CALL_DISPLAY_LIST, 8);
    if (!p) return;
    wr_u32(p, (uint32_t)(uintptr_t)list);
    wr_i32(p, (int32_t)nbytes);
}
void pc_gfx_call_display_list(const void* list, u32 nbytes) {
    rec_call_display_list(gGxqMain, list, nbytes);
}

void pc_gfx_copy_disp(void* dest, GXBool clear) {
    if (!sRecording) { pc_gfx_copy_disp_impl(dest, clear); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_COPY_DISP, 8);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)dest);
        wr_i32(p, (int32_t)clear);
    });
}

void pc_gfx_set_copy_clear(GXColor color, u32 clearZ) {
    if (!sRecording) { pc_gfx_set_copy_clear_impl(color, clearZ); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_COPY_CLEAR, 8);
        if (!p) return;
        memcpy(p, &color, 4); p += 4;
        wr_i32(p, (int32_t)clearZ);
    });
}

void pc_gfx_begin_capture(uint64_t serial) {
    if (!sRecording) { pc_gfx_begin_capture_impl(serial); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_BEGIN_CAPTURE, 8);
        if (!p) return;
        memcpy(p, &serial, 8); p += 8;
    });
}

void pc_gfx_end_capture(void) {
    if (!sRecording) { pc_gfx_end_capture_impl(); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_END_CAPTURE, 0);
        if (!p) return;
        (void)p;
    });
}

void pc_gfx_enable_capture(bool enabled) {
    if (!sRecording) { pc_gfx_enable_capture_impl(enabled); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_ENABLE_CAPTURE, 4);
        if (!p) return;
        wr_i32(p, (int32_t)enabled);
    });
}

void pc_gfx_set_shader_specialisation(bool enabled) {
    if (!sRecording) { pc_gfx_set_shader_specialisation_impl(enabled); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_SHADER_SPECIALISATION, 4);
        if (!p) return;
        wr_i32(p, (int32_t)enabled);
    });
}

void pc_gfx_flush_batch(void) {
    if (!sRecording) { pc_gfx_flush_batch_impl(); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_FLUSH_BATCH, 0);
        if (!p) return;
        (void)p;
    });
}

void pc_gfx_present(void) {
    if (!sRecording) { pc_gfx_present_impl(); return; }
    // Join an open segment before the frame ends.
    vita_drawrec_join(kDrJoinPresent);
    gxq_each([](GxqCtx& R) GXQ_NI { gxq_emit(R, GXQ_PRESENT, 0); });
    close_chunk();
    // At most vita_gxq_pipe_depth() frames in flight.
    nectar_gil_release();
    sceKernelWaitSema(sSemaFrame, 1, NULL);
    nectar_gil_acquire();
}

// Copy the light object.
GXQ_REC void rec_load_light(GxqCtx& R, void* ltObj, u32 lightMask) {
    if (!sRecording) { pc_gfx_load_light_impl(ltObj, lightMask); return; }
    if (!ltObj) return;
    {
        bool same = (lightMask & 0xFFu) != 0;
        for (int i = 0; i < 8; ++i) if (lightMask & (1u << i)) { if (R.d.lightEp[i] != R.d.ep || memcmp(R.d.light[i], ltObj, 0x40) != 0) { same = false; break; } }
        if (same) return;
        for (int i = 0; i < 8; ++i) if (lightMask & (1u << i)) { memcpy(R.d.light[i], ltObj, 0x40); R.d.lightEp[i] = R.d.ep; }
    }
    uint8_t* p = gxq_emit(R, GXQ_LOAD_LIGHT, 4 + 0x40);
    if (!p) return;
    wr_u32(p, lightMask);
    memcpy(p, ltObj, 0x40);
}
void pc_gfx_load_light(void* ltObj, u32 lightMask) {
    rec_load_light(gGxqMain, ltObj, lightMask);
}

// Copy the TLUT entries.
void pc_gfx_init_tlut_obj(GXTlutObj* obj, void* lut, GXTlutFmt format, u16 numEntries) {
    if (!sRecording) { pc_gfx_init_tlut_obj_impl(obj, lut, format, numEntries); return; }
    if (!obj || !lut || numEntries == 0) return;
    const uint32_t bytes = uint32_t(numEntries) * 2u;
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_INIT_TLUT_OBJ, 12 + bytes);
        if (!p) return;
        wr_u32(p, (uint32_t)(uintptr_t)obj);
        wr_i32(p, (int32_t)format);
        wr_i32(p, (int32_t)numEntries);
        memcpy(p, lut, bytes);
    });
}

// Drop identical vertex-format calls.
GXQ_REC void rec_clear_vtx_desc(GxqCtx& R) {
    if (gxq_refuse(R)) return;   // off-lock worker
    if (!sRecording) { pc_gfx_clear_vtx_desc_impl(); return; }
    GxqDecision& d = R.d;
    for (int i = 0; i < GX_VA_MAX_ATTR; ++i) { if (d.pendDesc[i] != (uint32_t)GX_NONE) { d.pendDesc[i] = (uint32_t)GX_NONE; d.pendDirty = true; d.pendDescMask |= 1u << i; } }
}
void pc_gfx_clear_vtx_desc(void) {
    rec_clear_vtx_desc(gGxqMain);
}
GXQ_REC void rec_set_vtx_desc(GxqCtx& R, GXAttr attr, GXAttrType type) {
    if (!sRecording) { pc_gfx_set_vtx_desc_impl(attr, type); return; }
    GxqDecision& d = R.d;
    if (attr >= 0 && attr < GX_VA_MAX_ATTR && d.pendDesc[attr] != (uint32_t)type) {
        if (gxq_refuse(R)) return;   // off-lock worker
        d.pendDesc[attr] = (uint32_t)type; d.pendDirty = true; d.pendDescMask |= 1u << attr;
    }
}
void pc_gfx_set_vtx_desc(GXAttr attr, GXAttrType type) {
    GXQ_HOT_REC(set_vtx_desc)(gGxqMain, attr, type);
}
GXQ_REC void rec_set_vtx_attr_fmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    if (!sRecording) { pc_gfx_set_vtx_attr_fmt_impl(fmt, attr, cnt, type, frac); return; }
    GxqDecision& d = R.d;
    if (fmt >= 0 && fmt < GX_MAX_VTXFMT && attr >= 0 && attr < GX_VA_MAX_ATTR) {
        const uint32_t packed = ((uint32_t)cnt << 16) | ((uint32_t)type << 8) | frac;
        if (d.pendFmt[fmt][attr] != packed) {
            if (gxq_refuse(R)) return;
            d.pendFmt[fmt][attr] = packed; d.pendDirty = true; d.pendFmtMask[fmt] |= 1u << attr;
        }
    }
}
void pc_gfx_set_vtx_attr_fmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    GXQ_HOT_REC(set_vtx_attr_fmt)(gGxqMain, fmt, attr, cnt, type, frac);
}
GXQ_REC void rec_set_array(GxqCtx& R, GXAttr attr, void* basePtr, u8 stride) {
    if (!sRecording) { pc_gfx_set_array_impl(attr, basePtr, stride); return; }
    GxqDecision& d = R.d;
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
        if (d.shArrayBase[attr] == (uintptr_t)basePtr && d.shArrayStride[attr] == stride) return;
        d.shArrayBase[attr] = (uintptr_t)basePtr; d.shArrayStride[attr] = stride;
    }
    uint8_t* p = gxq_emit(R, GXQ_SET_ARRAY, 12);
    if (!p) return;
    wr_i32(p, (int32_t)attr); wr_u32(p, (uint32_t)(uintptr_t)basePtr); wr_i32(p, (int32_t)stride);
}
void pc_gfx_set_array(GXAttr attr, void* basePtr, u8 stride) {
    GXQ_HOT_REC(set_array)(gGxqMain, attr, basePtr, stride);
}

#if NECTAR_GXQ_LEAN
// Lean core slow halves
#define GXQ_SLOW_GUARD(R) \
    do { if (gxq_refuse(R)) return; } while (0)
void gxq_s_load_pos_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_load_pos_mtx_impl(mtx, id); return; }
    if (mtx && id < 64u) gxq_st12(R, R.d.posMtx[id], mtx);
    uint8_t* p = gxq_put(R, GXQ_LOAD_POS_MTX, 52);
    if (!p) return;
    memcpy(p, mtx, 48); p += 48;
    wr_i32(p, (int32_t)id);
}
void gxq_s_load_nrm_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_load_nrm_mtx_impl(mtx, id); return; }
    if (mtx && id < 64u) gxq_st12(R, R.d.nrmMtx[id], mtx);
    uint8_t* p = gxq_put(R, GXQ_LOAD_NRM_MTX, 52);
    if (!p) return;
    memcpy(p, mtx, 48); p += 48;
    wr_i32(p, (int32_t)id);
}
void gxq_s_load_tex_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_load_tex_mtx_impl(mtx, id); return; }
    if (mtx && id < 64u) gxq_st12(R, R.d.texMtx[id], mtx);
    uint8_t* p = gxq_put(R, GXQ_LOAD_TEX_MTX, 52);
    if (!p) return;
    memcpy(p, mtx, 48); p += 48;
    wr_i32(p, (int32_t)id);
}
void gxq_s_set_current_mtx(GxqCtx& R, u32 id) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_current_mtx_impl(id); return; }
    gxq_st1(R, R.d.curMtx, id);
    uint8_t* p = gxq_put(R, GXQ_SET_CURRENT_MTX, 4);
    if (!p) return;
    wr_i32(p, (int32_t)id);
}
void gxq_s_set_tex_coord_gen(GxqCtx& R, GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_tex_coord_gen_impl(coord, type, src, matrixIdx); return; }
    if (unsigned(coord) < 8u) gxq_st3(R, R.d.texGen[coord], uint32_t(type), uint32_t(src), matrixIdx);
    uint8_t* p = gxq_put(R, GXQ_SET_TEX_COORD_GEN, 16);
    if (!p) return;
    wr_i32(p, (int32_t)coord);
    wr_i32(p, (int32_t)type);
    wr_i32(p, (int32_t)src);
    wr_i32(p, (int32_t)matrixIdx);
}
void gxq_s_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_PE); pc_gfx_set_z_mode_impl(compareEnable, func, updateEnable); return; }
    gxq_st3(R, R.d.zMode, uint32_t(compareEnable), uint32_t(func), uint32_t(updateEnable));
    GXQ_GEN_EMIT(R, PCGX_GEN_PE);
    uint8_t* p = gxq_put(R, GXQ_SET_Z_MODE, 12);
    if (!p) return;
    wr_i32(p, (int32_t)compareEnable);
    wr_i32(p, (int32_t)func);
    wr_i32(p, (int32_t)updateEnable);
}
void gxq_s_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_PE); pc_gfx_set_blend_mode_impl(type, srcFactor, dstFactor, op); return; }
    gxq_st4(R, R.d.blend, uint32_t(type), uint32_t(srcFactor), uint32_t(dstFactor), uint32_t(op));
    GXQ_GEN_EMIT(R, PCGX_GEN_PE);
    uint8_t* p = gxq_put(R, GXQ_SET_BLEND_MODE, 16);
    if (!p) return;
    wr_i32(p, (int32_t)type);
    wr_i32(p, (int32_t)srcFactor);
    wr_i32(p, (int32_t)dstFactor);
    wr_i32(p, (int32_t)op);
}
void gxq_s_set_line_width(GxqCtx& R, u8 width) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_line_width_impl(width); return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_LINE_WIDTH, 4);
    if (!p) return;
    wr_i32(p, (int32_t)width);
}
void gxq_s_set_cull_mode(GxqCtx& R, GXCullMode mode) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_cull_mode_impl(mode); return; }
    gxq_st1(R, R.d.cull, uint32_t(mode));
    uint8_t* p = gxq_put(R, GXQ_SET_CULL_MODE, 4);
    if (!p) return;
    wr_i32(p, (int32_t)mode);
}
void gxq_s_set_color_update(GxqCtx& R, GXBool updateEnable) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_color_update_impl(updateEnable); return; }
    gxq_st1(R, R.d.colorUpd, uint32_t(updateEnable));
    uint8_t* p = gxq_put(R, GXQ_SET_COLOR_UPDATE, 4);
    if (!p) return;
    wr_i32(p, (int32_t)updateEnable);
}
void gxq_s_set_alpha_update(GxqCtx& R, GXBool updateEnable) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_alpha_update_impl(updateEnable); return; }
    gxq_st1(R, R.d.alphaUpd, uint32_t(updateEnable));
    uint8_t* p = gxq_put(R, GXQ_SET_ALPHA_UPDATE, 4);
    if (!p) return;
    wr_i32(p, (int32_t)updateEnable);
}
void gxq_s_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_PE); pc_gfx_set_alpha_compare_impl(comp0, ref0, op, comp1, ref1); return; }
    gxq_st5(R, R.d.alphaCmp, uint32_t(comp0), uint32_t(ref0), uint32_t(op), uint32_t(comp1), uint32_t(ref1));
    GXQ_GEN_EMIT(R, PCGX_GEN_PE);
    uint8_t* p = gxq_put(R, GXQ_SET_ALPHA_COMPARE, 20);
    if (!p) return;
    wr_i32(p, (int32_t)comp0);
    wr_i32(p, (int32_t)ref0);
    wr_i32(p, (int32_t)op);
    wr_i32(p, (int32_t)comp1);
    wr_i32(p, (int32_t)ref1);
}
void gxq_s_set_chan_mat_color(GxqCtx& R, GXChannelID chan, GXColor color) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_chan_mat_color_impl(chan, color); return; }
    if (unsigned(chan) < 6u) gxq_st1(R, R.d.chanMat[unsigned(chan) & 1u], gxq_color_word(color));
    uint8_t* p = gxq_put(R, GXQ_SET_CHAN_MAT_COLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)chan);
    memcpy(p, &color, 4);
}
void gxq_s_set_chan_amb_color(GxqCtx& R, GXChannelID chan, GXColor color) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_chan_amb_color_impl(chan, color); return; }
    if (unsigned(chan) < 6u) gxq_st1(R, R.d.chanAmb[unsigned(chan) & 1u], gxq_color_word(color));
    uint8_t* p = gxq_put(R, GXQ_SET_CHAN_AMB_COLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)chan);
    memcpy(p, &color, 4);
}
void gxq_s_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_order_impl(stage, coord, map, chan); return; }
    if (unsigned(stage) < 16u) gxq_st3(R, R.d.tevOrder[stage], uint32_t(coord), uint32_t(map), uint32_t(chan));
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_ORDER, 16);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)coord);
    wr_i32(p, (int32_t)map);
    wr_i32(p, (int32_t)chan);
}
void gxq_s_set_num_tev_stages(GxqCtx& R, u8 num) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_num_tev_stages_impl(num); return; }
    gxq_st1(R, R.d.numStages, uint32_t(num));
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_NUM_TEV_STAGES, 4);
    if (!p) return;
    wr_i32(p, (int32_t)num);
}
void gxq_s_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_color_in_impl(stage, a, b, c, d); return; }
    if (unsigned(stage) < 16u) {
        gxq_st4(R, R.d.tevCIn[stage], uint32_t(a), uint32_t(b), uint32_t(c), uint32_t(d));
        R.d.tevOp[stage].ep = 0;
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_COLOR_IN, 20);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)a);
    wr_i32(p, (int32_t)b);
    wr_i32(p, (int32_t)c);
    wr_i32(p, (int32_t)d);
}
void gxq_s_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_alpha_in_impl(stage, a, b, c, d); return; }
    if (unsigned(stage) < 16u) {
        gxq_st4(R, R.d.tevAIn[stage], uint32_t(a), uint32_t(b), uint32_t(c), uint32_t(d));
        R.d.tevOp[stage].ep = 0;
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_ALPHA_IN, 20);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)a);
    wr_i32(p, (int32_t)b);
    wr_i32(p, (int32_t)c);
    wr_i32(p, (int32_t)d);
}
void gxq_s_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_color_op_impl(stage, op, bias, scale, clamp, outReg); return; }
    if (unsigned(stage) < 16u) {
        gxq_st5(R, R.d.tevCOp[stage], uint32_t(op), uint32_t(bias), uint32_t(scale), uint32_t(clamp), uint32_t(outReg));
        R.d.tevOp[stage].ep = 0;
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_COLOR_OP, 24);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)op);
    wr_i32(p, (int32_t)bias);
    wr_i32(p, (int32_t)scale);
    wr_i32(p, (int32_t)clamp);
    wr_i32(p, (int32_t)outReg);
}
void gxq_s_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_alpha_op_impl(stage, op, bias, scale, clamp, outReg); return; }
    if (unsigned(stage) < 16u) {
        gxq_st5(R, R.d.tevAOp[stage], uint32_t(op), uint32_t(bias), uint32_t(scale), uint32_t(clamp), uint32_t(outReg));
        R.d.tevOp[stage].ep = 0;
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_ALPHA_OP, 24);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)op);
    wr_i32(p, (int32_t)bias);
    wr_i32(p, (int32_t)scale);
    wr_i32(p, (int32_t)clamp);
    wr_i32(p, (int32_t)outReg);
}
// Register shadow: tag 0 and the colour.
void gxq_s_set_tev_color(GxqCtx& R, GXTevRegID reg, GXColor color) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG); pc_gfx_set_tev_color_impl(reg, color); return; }
    if (unsigned(reg) < 4u) gxq_st3(R, R.d.tevReg[reg], 0u, gxq_color_word(color), 0u);
    GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_COLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)reg);
    memcpy(p, &color, 4);
}
void gxq_s_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG); pc_gfx_set_tev_color_s10_impl(reg, color); return; }
    if (unsigned(reg) < 4u) {
        uint32_t w[2];
        memcpy(w, &color, 8);
        gxq_st3(R, R.d.tevReg[reg], 1u, w[0], w[1]);
    }
    GXQ_GEN_EMIT(R, PCGX_GEN_TEVREG);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_COLOR_S10, 12);
    if (!p) return;
    wr_i32(p, (int32_t)reg);
    memcpy(p, &color, 8);
}
void gxq_s_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_KCOL); pc_gfx_set_tev_kcolor_impl(id, color); return; }
    if (unsigned(id) < 4u) gxq_st1(R, R.d.kColor[id], gxq_color_word(color));
    GXQ_GEN_EMIT(R, PCGX_GEN_KCOL);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_KCOLOR, 8);
    if (!p) return;
    wr_i32(p, (int32_t)id);
    memcpy(p, &color, 4);
}
void gxq_s_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_kcolor_sel_impl(stage, sel); return; }
    if (unsigned(stage) < 16u) gxq_st1(R, R.d.kcSel[stage], uint32_t(sel));
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_KCOLOR_SEL, 8);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)sel);
}
void gxq_s_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { GXQ_GEN_EMIT(R, PCGX_GEN_TEV); pc_gfx_set_tev_kalpha_sel_impl(stage, sel); return; }
    if (unsigned(stage) < 16u) gxq_st1(R, R.d.kaSel[stage], uint32_t(sel));
    GXQ_GEN_EMIT(R, PCGX_GEN_TEV);
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_KALPHA_SEL, 8);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)sel);
}
void gxq_s_set_tev_swap_mode(GxqCtx& R, GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_tev_swap_mode_impl(stage, rasSel, texSel); return; }
    if (unsigned(stage) < 16u) gxq_st2(R, R.d.swap[stage], uint32_t(rasSel), uint32_t(texSel));
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_SWAP_MODE, 12);
    if (!p) return;
    wr_i32(p, (int32_t)stage);
    wr_i32(p, (int32_t)rasSel);
    wr_i32(p, (int32_t)texSel);
}
void gxq_s_set_tev_swap_mode_table(GxqCtx& R, GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                                   GXTevColorChan alpha) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_set_tev_swap_mode_table_impl(table, red, green, blue, alpha); return; }
    const unsigned t = unsigned(int(table) - int(GX_TEV_SWAP0));
    if (t < 4u) gxq_st4(R, R.d.swapTable[t], uint32_t(red), uint32_t(green), uint32_t(blue), uint32_t(alpha));
    uint8_t* p = gxq_put(R, GXQ_SET_TEV_SWAP_MODE_TABLE, 20);
    if (!p) return;
    wr_i32(p, (int32_t)table);
    wr_i32(p, (int32_t)red);
    wr_i32(p, (int32_t)green);
    wr_i32(p, (int32_t)blue);
    wr_i32(p, (int32_t)alpha);
}
void gxq_s_set_array(GxqCtx& R, GXAttr attr, void* basePtr, uint32_t stride) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) {
        pc_gfx_set_array_impl(attr, basePtr, u8(stride));
        pc_gfx_note_array_volatile(int(attr), (stride & kGxqArrayVolatile) != 0);
        return;
    }
    if (unsigned(attr) < 26u) {
        R.d.shArrayBase[attr]   = uintptr_t(basePtr);
        R.d.shArrayStride[attr] = stride;
    }
    uint8_t* p = gxq_put(R, GXQ_SET_ARRAY, 12);
    if (!p) return;
    wr_i32(p, (int32_t)attr); wr_u32(p, (uint32_t)(uintptr_t)basePtr); wr_i32(p, (int32_t)stride);
}
// Vertex setter slow halves.
void gxq_s_set_vtx_desc(GxqCtx& R, GXAttr attr, GXAttrType type) { GXQ_SLOW_GUARD(R); rec_set_vtx_desc(R, attr, type); }
void gxq_s_set_vtx_attr_fmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    GXQ_SLOW_GUARD(R);
    rec_set_vtx_attr_fmt(R, fmt, attr, cnt, type, frac);
}
void gxq_s_load_tex_obj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) {
    GXQ_SLOW_GUARD(R);
    if (!sRecording) { pc_gfx_load_tex_obj_impl(obj, id); return; }
    uint8_t* p = gxq_put(R, GXQ_LOAD_TEX_OBJ, 8);
    if (!p) return;
    wr_u32(p, (uint32_t)(uintptr_t)obj);
    wr_i32(p, (int32_t)id);
}
// Immediate component slow halves.
void gxq_s_position(GxqCtx& R, f32 x, f32 y, f32 z) { GXQ_SLOW_GUARD(R); rec_position(R, x, y, z); }
void gxq_s_color(GxqCtx& R, u8 r, u8 g, u8 b, u8 a) { GXQ_SLOW_GUARD(R); rec_color(R, r, g, b, a); }
void gxq_s_texcoord(GxqCtx& R, f32 u, f32 v) { GXQ_SLOW_GUARD(R); rec_texcoord(R, u, v); }
#endif

// GX-level entries
#define GXR_VTXGEN(R) (++(R).d.vtxGen)
#define GXR_MATGEN(R) (++(R).d.matGen)
GXQ_REC void gxb_set_vtx_attr_fmtv_calls(GxqCtx& R, int fmt, const _GXVtxAttrFmtList* list) {
    GXR_VTXGEN(R);
    if (!list) return;
    for (; list->attr != GX_VA_NULL; ++list) rec_set_vtx_attr_fmt(R, GXVtxFmt(fmt), list->attr, list->count, list->type, list->frac);
}
GXQ_REC void gxb_set_vtx_descv_calls(GxqCtx& R, const _GXVtxDescList* attrList) {
    GXR_VTXGEN(R);
    if (!attrList) return;
    rec_clear_vtx_desc(R);
    for (; attrList->attr != GX_VA_NULL; ++attrList) rec_set_vtx_desc(R, attrList->attr, attrList->type);
}
GXQ_REC void gxb_GXClearVtxDesc(GxqCtx& R) { GXR_VTXGEN(R); rec_clear_vtx_desc(R); }
GXQ_REC void gxb_GXSetVtxDesc(GxqCtx& R, GXAttr attr, GXAttrType type) { GXR_VTXGEN(R); rec_set_vtx_desc(R, attr, type); }
GXQ_REC void gxb_GXSetVtxAttrFmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    GXR_VTXGEN(R);
    rec_set_vtx_attr_fmt(R, fmt, attr, cnt, type, frac);
}
// GX list setters, per call.
GXQ_REC void gxb_GXSetVtxAttrFmtv(GxqCtx& R, GXVtxFmt fmt, const _GXVtxAttrFmtList* list) {
    gxb_set_vtx_attr_fmtv_calls(R, fmt, list);
}
GXQ_REC void gxb_GXSetVtxDescv(GxqCtx& R, const _GXVtxDescList* list) {
    gxb_set_vtx_descv_calls(R, list);
}
GXQ_REC void gxb_GXSetArray(GxqCtx& R, GXAttr attr, const void* basePtr, u8 stride) { GXR_VTXGEN(R); rec_set_array(R, attr, const_cast<void*>(basePtr), stride); }
GXQ_REC void gxb_GXInvalidateVtxCache(GxqCtx& R) { rec_begin_frame(R); }
GXQ_REC void gxb_GXLoadPosMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id) { rec_load_pos_mtx(R, mtx, id); }
GXQ_REC void gxb_GXLoadNrmMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id) { rec_load_nrm_mtx(R, mtx, id); }
// No MATGEN.
GXQ_REC void gxb_GXLoadTexMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id, GXTexMtxType type) { (void)type; rec_load_tex_mtx(R, mtx, id); }
GXQ_REC void gxb_GXSetCurrentMtx(GxqCtx& R, u32 id) {
    if (id < 64) R.d.gxCurMtx = id;
    rec_set_current_mtx(R, id);
}
GXQ_REC void gxb_GXSetProjection(GxqCtx& R, const f32 mtx[4][4], GXProjectionType type) {
    if (mtx) for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) R.d.gxProj[r][c] = mtx[r][c];
    rec_set_projection(R, mtx, type);
}
GXQ_REC void gxb_GXSetZMode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    GXR_MATGEN(R);
    rec_set_z_mode(R, compareEnable, func, updateEnable);
}
GXQ_REC void gxb_GXSetBlendMode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    GXR_MATGEN(R);
    rec_set_blend_mode(R, type, srcFactor, dstFactor, op);
}
GXQ_REC void gxb_GXSetAlphaCompare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    GXR_MATGEN(R);
    rec_set_alpha_compare(R, comp0, ref0, op, comp1, ref1);
}
GXQ_REC void gxb_GXSetColorUpdate(GxqCtx& R, GXBool updateEnable) { GXR_MATGEN(R); rec_set_color_update(R, updateEnable); }
GXQ_REC void gxb_GXSetZCompLoc(GxqCtx& R, GXBool beforeTex) { GXR_MATGEN(R); (void)beforeTex; }
// No MATGEN.
GXQ_REC void gxb_GXSetCullMode(GxqCtx& R, GXCullMode mode) { rec_set_cull_mode(R, mode); }
GXQ_REC void gxb_GXSetLineWidth(GxqCtx& R, u8 width, GXTexOffset texOffset) { (void)texOffset; gxq_s_set_line_width(R, width); }
GXQ_REC void gxb_GXSetPointSize(GxqCtx& R, u8 size, GXTexOffset texOffset) { (void)R; (void)size; (void)texOffset; }
// Fog.
GXQ_REC void rec_set_fog(GxqCtx& R, bool enable, float startZ, float endZ, float nearZ, float farZ, unsigned char r, unsigned char g, unsigned char b);
GXQ_REC void gxb_GXSetFog(GxqCtx& R, GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    GXR_MATGEN(R);
    rec_set_fog(R, type != GX_FOG_NONE && pc_settings_get_fog() != 0, startz, endz, nearz, farz, color.r, color.g, color.b);
}
GXQ_REC void gxb_GXSetTevOrder(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    GXR_MATGEN(R);
    rec_set_tev_order(R, stage, coord, map, chan);
}
// GXSetTevOp expanded into the low-level TEV setters (libogc).
GXQ_REC void gxb_GXSetTevOp(GxqCtx& R, GXTevStageID stage, GXTevMode mode) {
    GXR_MATGEN(R);
    const bool first = (stage == GX_TEVSTAGE0);
    switch (mode) {
    case GX_MODULATE:
        rec_set_tev_color_in(R, stage, first ? GX_CC_ZERO : GX_CC_CPREV, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
        rec_set_tev_alpha_in(R, stage, first ? GX_CA_ZERO : GX_CA_APREV, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        break;
    case GX_DECAL:
        rec_set_tev_color_in(R, stage, GX_CC_TEXA, GX_CC_TEXC, GX_CC_RASC, first ? GX_CC_TEXA : GX_CC_CPREV);
        rec_set_tev_alpha_in(R, stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, first ? GX_CA_TEXA : GX_CA_APREV);
        break;
    case GX_BLEND:
        rec_set_tev_color_in(R, stage, first ? GX_CC_ZERO : GX_CC_CPREV, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
        rec_set_tev_alpha_in(R, stage, first ? GX_CA_ZERO : GX_CA_APREV, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        rec_set_tev_color_in(R, stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        rec_set_tev_alpha_in(R, stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    case GX_PASSCLR:
    default:
        // "Pass color": hand the rasterized vertex color through untouched.
        rec_set_tev_color_in(R, stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        rec_set_tev_alpha_in(R, stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    }
    rec_set_tev_color_op(R, stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    rec_set_tev_alpha_op(R, stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
GXQ_REC void gxb_GXSetTevColorIn(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    GXR_MATGEN(R);
    rec_set_tev_color_in(R, stage, a, b, c, d);
}
GXQ_REC void gxb_GXSetTevAlphaIn(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    GXR_MATGEN(R);
    rec_set_tev_alpha_in(R, stage, a, b, c, d);
}
GXQ_REC void gxb_GXSetTevColorOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXR_MATGEN(R);
    rec_set_tev_color_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_REC void gxb_GXSetTevAlphaOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    GXR_MATGEN(R);
    rec_set_tev_alpha_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_REC void gxb_GXSetTevColor(GxqCtx& R, GXTevRegID reg, GXColor color) { GXR_MATGEN(R); rec_set_tev_color(R, reg, color); }
GXQ_REC void gxb_GXSetTevColorS10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) { GXR_MATGEN(R); rec_set_tev_color_s10(R, reg, color); }
GXQ_REC void gxb_GXSetTevKColor(GxqCtx& R, GXTevKColorID id, GXColor color) { GXR_MATGEN(R); rec_set_tev_kcolor(R, id, color); }
GXQ_REC void gxb_GXSetTevKColorSel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) { GXR_MATGEN(R); rec_set_tev_kcolor_sel(R, stage, sel); }
GXQ_REC void gxb_GXSetTevKAlphaSel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) { GXR_MATGEN(R); rec_set_tev_kalpha_sel(R, stage, sel); }
GXQ_REC void gxb_GXSetNumTevStages(GxqCtx& R, u8 num) { GXR_MATGEN(R); rec_set_num_tev_stages(R, num); }
GXQ_REC void gxb_GXSetNumTexGens(GxqCtx& R, u8 num) { GXR_MATGEN(R); (void)num; }
GXQ_REC void gxb_GXSetTexCoordGen2(GxqCtx& R, GXTexCoordID dstCoord, GXTexGenType func, GXTexGenSrc srcParam, u32 mtx, GXBool normalize, u32 postMtx) {
    GXR_MATGEN(R);
    (void)normalize; (void)postMtx;
    rec_set_tex_coord_gen(R, dstCoord, func, srcParam, mtx);
}
GXQ_REC void gxb_GXSetNumChans(GxqCtx& R, u8 num) { GXR_MATGEN(R); (void)num; }
GXQ_REC void gxb_GXSetChanCtrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn,
                       GXAttnFn attnFn) {
    GXR_MATGEN(R);
    rec_set_chan_ctrl(R, chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}
GXQ_REC void gxb_GXSetChanMatColor(GxqCtx& R, GXChannelID chan, GXColor matColor) { GXR_MATGEN(R); rec_set_chan_mat_color(R, chan, matColor); }
GXQ_REC void gxb_GXSetChanAmbColor(GxqCtx& R, GXChannelID chan, GXColor ambColor) { GXR_MATGEN(R); rec_set_chan_amb_color(R, chan, ambColor); }
GXQ_REC void gxb_GXLoadLightObjImm(GxqCtx& R, GXLightObj* ltObj, GXLightID ltId) { rec_load_light(R, ltObj, static_cast<u32>(ltId)); }
GXQ_REC void gxb_GXLoadTexObj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) { GXR_MATGEN(R); rec_load_tex_obj(R, obj, id); }
GXQ_REC void gxb_GXCallDisplayList(GxqCtx& R, const void* list, u32 nbytes) { rec_call_display_list(R, list, nbytes); }
GXQ_REC void gxb_GXBegin(GxqCtx& R, GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) { rec_begin(R, type, vtxfmt, nverts); }
GXQ_REC void gxb_GXEnd(GxqCtx& R) { rec_end(R); }
GXQ_REC void gxb_GXPosition3f32(GxqCtx& R, f32 x, f32 y, f32 z) { rec_position(R, x, y, z); }
GXQ_REC void gxb_GXNormal3f32(GxqCtx& R, f32 x, f32 y, f32 z) { (void)R; (void)x; (void)y; (void)z; }
// GXColor1u32: a Colour's four bytes, RGBA.
GXQ_REC void gxb_GXColor1u32(GxqCtx& R, u32 c) { rec_color(R, (u8)c, (u8)(c >> 8), (u8)(c >> 16), (u8)(c >> 24)); }
GXQ_REC void gxb_GXTexCoord2f32(GxqCtx& R, f32 u, f32 v) { rec_texcoord(R, u, v); }

// Recorder-level entries: DGXGraphics' S4 block re-issues.
GXQ_REC void gxb_pc_gfx_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    rec_set_blend_mode(R, type, srcFactor, dstFactor, op);
}
GXQ_REC void gxb_pc_gfx_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    rec_set_alpha_compare(R, comp0, ref0, op, comp1, ref1);
}
GXQ_REC void gxb_pc_gfx_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) { rec_set_z_mode(R, compareEnable, func, updateEnable); }
GXQ_REC void gxb_pc_gfx_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) { rec_set_tev_kcolor(R, id, color); }
GXQ_REC void gxb_pc_gfx_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    rec_set_tev_color_in(R, stage, a, b, c, d);
}
GXQ_REC void gxb_pc_gfx_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    rec_set_tev_alpha_in(R, stage, a, b, c, d);
}
GXQ_REC void gxb_pc_gfx_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    rec_set_tev_color_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_REC void gxb_pc_gfx_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    rec_set_tev_alpha_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_REC void gxb_pc_gfx_set_num_tev_stages(GxqCtx& R, u8 num) { rec_set_num_tev_stages(R, num); }
GXQ_REC void gxb_pc_gfx_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    rec_set_tev_order(R, stage, coord, map, chan);
}
GXQ_REC void gxb_pc_gfx_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) { rec_set_tev_kcolor_sel(R, stage, sel); }
GXQ_REC void gxb_pc_gfx_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) { rec_set_tev_kalpha_sel(R, stage, sel); }
GXQ_REC void gxb_pc_gfx_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) { rec_set_tev_color_s10(R, reg, color); }
GXQ_REC void gxb_pc_gfx_set_chan_ctrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask,
                              GXDiffuseFn diffFn, GXAttnFn attnFn) {
    rec_set_chan_ctrl(R, chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}

// Entries
#if !NECTAR_GXQ_LEAN
void gxr_GXSetVtxDesc(GxqCtx& R, GXAttr attr, GXAttrType type) { gxb_GXSetVtxDesc(R, attr, type); }
void gxr_GXSetVtxAttrFmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) { gxb_GXSetVtxAttrFmt(R, fmt, attr, cnt, type, frac); }
void gxr_GXSetArray(GxqCtx& R, GXAttr attr, const void* basePtr, u8 stride) { gxb_GXSetArray(R, attr, basePtr, stride); }
void gxr_GXLoadPosMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id) { gxb_GXLoadPosMtxImm(R, mtx, id); }
void gxr_GXLoadNrmMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id) { gxb_GXLoadNrmMtxImm(R, mtx, id); }
void gxr_GXLoadTexMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id, GXTexMtxType type) { gxb_GXLoadTexMtxImm(R, mtx, id, type); }
void gxr_GXSetCurrentMtx(GxqCtx& R, u32 id) { gxb_GXSetCurrentMtx(R, id); }
void gxr_GXSetZMode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) { gxb_GXSetZMode(R, compareEnable, func, updateEnable); }
void gxr_GXSetBlendMode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) { gxb_GXSetBlendMode(R, type, srcFactor, dstFactor, op); }
void gxr_GXSetAlphaCompare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) { gxb_GXSetAlphaCompare(R, comp0, ref0, op, comp1, ref1); }
void gxr_GXSetColorUpdate(GxqCtx& R, GXBool updateEnable) { gxb_GXSetColorUpdate(R, updateEnable); }
void gxr_GXSetZCompLoc(GxqCtx& R, GXBool beforeTex) { gxb_GXSetZCompLoc(R, beforeTex); }
void gxr_GXSetCullMode(GxqCtx& R, GXCullMode mode) { gxb_GXSetCullMode(R, mode); }
void gxr_GXSetLineWidth(GxqCtx& R, u8 width, GXTexOffset texOffset) { gxb_GXSetLineWidth(R, width, texOffset); }
void gxr_GXSetPointSize(GxqCtx& R, u8 size, GXTexOffset texOffset) { gxb_GXSetPointSize(R, size, texOffset); }
void gxr_GXSetTevOrder(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) { gxb_GXSetTevOrder(R, stage, coord, map, chan); }
void gxr_GXSetTevColorIn(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) { gxb_GXSetTevColorIn(R, stage, a, b, c, d); }
void gxr_GXSetTevAlphaIn(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) { gxb_GXSetTevAlphaIn(R, stage, a, b, c, d); }
void gxr_GXSetTevColorOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) { gxb_GXSetTevColorOp(R, stage, op, bias, scale, clamp, outReg); }
void gxr_GXSetTevAlphaOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) { gxb_GXSetTevAlphaOp(R, stage, op, bias, scale, clamp, outReg); }
void gxr_GXSetTevColor(GxqCtx& R, GXTevRegID reg, GXColor color) { gxb_GXSetTevColor(R, reg, color); }
void gxr_GXSetTevColorS10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) { gxb_GXSetTevColorS10(R, reg, color); }
void gxr_GXSetTevKColor(GxqCtx& R, GXTevKColorID id, GXColor color) { gxb_GXSetTevKColor(R, id, color); }
void gxr_GXSetTevKColorSel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) { gxb_GXSetTevKColorSel(R, stage, sel); }
void gxr_GXSetTevKAlphaSel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) { gxb_GXSetTevKAlphaSel(R, stage, sel); }
void gxr_GXSetNumTevStages(GxqCtx& R, u8 num) { gxb_GXSetNumTevStages(R, num); }
void gxr_GXSetNumTexGens(GxqCtx& R, u8 num) { gxb_GXSetNumTexGens(R, num); }
void gxr_GXSetTexCoordGen2(GxqCtx& R, GXTexCoordID dstCoord, GXTexGenType func, GXTexGenSrc srcParam, u32 mtx, GXBool normalize, u32 postMtx) { gxb_GXSetTexCoordGen2(R, dstCoord, func, srcParam, mtx, normalize, postMtx); }
void gxr_GXSetNumChans(GxqCtx& R, u8 num) { gxb_GXSetNumChans(R, num); }
void gxr_GXSetChanMatColor(GxqCtx& R, GXChannelID chan, GXColor matColor) { gxb_GXSetChanMatColor(R, chan, matColor); }
void gxr_GXSetChanAmbColor(GxqCtx& R, GXChannelID chan, GXColor ambColor) { gxb_GXSetChanAmbColor(R, chan, ambColor); }
void gxr_GXLoadTexObj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) { gxb_GXLoadTexObj(R, obj, id); }
void gxr_GXPosition3f32(GxqCtx& R, f32 x, f32 y, f32 z) { gxb_GXPosition3f32(R, x, y, z); }
void gxr_GXNormal3f32(GxqCtx& R, f32 x, f32 y, f32 z) { gxb_GXNormal3f32(R, x, y, z); }
void gxr_GXColor1u32(GxqCtx& R, u32 c) { gxb_GXColor1u32(R, c); }
void gxr_GXTexCoord2f32(GxqCtx& R, f32 u, f32 v) { gxb_GXTexCoord2f32(R, u, v); }
void gxr_pc_gfx_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) { gxb_pc_gfx_set_blend_mode(R, type, srcFactor, dstFactor, op); }
void gxr_pc_gfx_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) { gxb_pc_gfx_set_alpha_compare(R, comp0, ref0, op, comp1, ref1); }
void gxr_pc_gfx_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) { gxb_pc_gfx_set_z_mode(R, compareEnable, func, updateEnable); }
void gxr_pc_gfx_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) { gxb_pc_gfx_set_tev_kcolor(R, id, color); }
void gxr_pc_gfx_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) { gxb_pc_gfx_set_tev_color_in(R, stage, a, b, c, d); }
void gxr_pc_gfx_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) { gxb_pc_gfx_set_tev_alpha_in(R, stage, a, b, c, d); }
void gxr_pc_gfx_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) { gxb_pc_gfx_set_tev_color_op(R, stage, op, bias, scale, clamp, outReg); }
void gxr_pc_gfx_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) { gxb_pc_gfx_set_tev_alpha_op(R, stage, op, bias, scale, clamp, outReg); }
void gxr_pc_gfx_set_num_tev_stages(GxqCtx& R, u8 num) { gxb_pc_gfx_set_num_tev_stages(R, num); }
void gxr_pc_gfx_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) { gxb_pc_gfx_set_tev_order(R, stage, coord, map, chan); }
void gxr_pc_gfx_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) { gxb_pc_gfx_set_tev_kcolor_sel(R, stage, sel); }
void gxr_pc_gfx_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) { gxb_pc_gfx_set_tev_kalpha_sel(R, stage, sel); }
void gxr_pc_gfx_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) { gxb_pc_gfx_set_tev_color_s10(R, reg, color); }
#endif
void gxr_set_vtx_attr_fmtv_calls(GxqCtx& R, int fmt, const _GXVtxAttrFmtList* list) {
    gxb_set_vtx_attr_fmtv_calls(R, fmt, list);
}
void gxr_set_vtx_descv_calls(GxqCtx& R, const _GXVtxDescList* attrList) {
    gxb_set_vtx_descv_calls(R, attrList);
}
void gxr_GXClearVtxDesc(GxqCtx& R) {
    gxb_GXClearVtxDesc(R);
}
void gxr_GXSetVtxAttrFmtv(GxqCtx& R, GXVtxFmt fmt, const _GXVtxAttrFmtList* list) {
    gxb_GXSetVtxAttrFmtv(R, fmt, list);
}
void gxr_GXSetVtxDescv(GxqCtx& R, const _GXVtxDescList* list) {
    gxb_GXSetVtxDescv(R, list);
}
void gxr_GXInvalidateVtxCache(GxqCtx& R) {
    gxb_GXInvalidateVtxCache(R);
}
void gxr_GXSetProjection(GxqCtx& R, const f32 mtx[4][4], GXProjectionType type) {
    gxb_GXSetProjection(R, mtx, type);
}
void gxr_GXSetFog(GxqCtx& R, GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    gxb_GXSetFog(R, type, startz, endz, nearz, farz, color);
}
void gxr_GXSetTevOp(GxqCtx& R, GXTevStageID stage, GXTevMode mode) {
    gxb_GXSetTevOp(R, stage, mode);
}
void gxr_GXSetChanCtrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn) {
    gxb_GXSetChanCtrl(R, chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}
void gxr_GXLoadLightObjImm(GxqCtx& R, GXLightObj* ltObj, GXLightID ltId) {
    gxb_GXLoadLightObjImm(R, ltObj, ltId);
}
void gxr_GXCallDisplayList(GxqCtx& R, const void* list, u32 nbytes) {
    gxb_GXCallDisplayList(R, list, nbytes);
}
void gxr_GXBegin(GxqCtx& R, GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    gxb_GXBegin(R, type, vtxfmt, nverts);
}
void gxr_GXEnd(GxqCtx& R) {
    gxb_GXEnd(R);
}
void gxr_pc_gfx_set_chan_ctrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn) {
    gxb_pc_gfx_set_chan_ctrl(R, chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}
// Frame-level: frame count and copy.
void gxr_GXCopyDisp(GxqCtx& R, void* dest, GXBool clear) {
    R.d.gameFrame = ++g_pcGameFrame;
    pc_gfx_copy_disp(dest, clear);
}

// Draw-record segment
static void gxq_fence(GxqCtx& R) {
    gxq_shadow_reset(R);
    GxqDecision& d = R.d;
    memset(d.shDesc, 0xFF, sizeof(d.shDesc));
    memset(d.shFmt, 0xFF, sizeof(d.shFmt));
    memset(d.shArrayBase, 0xFF, sizeof(d.shArrayBase));
    memset(d.shArrayStride, 0xFF, sizeof(d.shArrayStride));
    memset(d.pendDesc, 0xFF, sizeof(d.pendDesc));
    memset(d.pendFmt, 0xFF, sizeof(d.pendFmt));
    d.pendDescMask = 0;
    memset(d.pendFmtMask, 0, sizeof(d.pendFmtMask));
    d.pendDirty = false;
    ++d.vtxGen;
    ++d.matGen;
    d.gxCurMtx = 0xFFFFFFFFu;
}
uint16_t vita_gxq_seg_fork(void) {
    GxqCtx& M = gGxqMain;
    GxqCtx& S = gGxqSub;
    uint16_t seg = ++sSegNext;
    if (seg == 0) seg = ++sSegNext;   // 0 is never a segment
    // 1. The CALL.
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_CALL_SUBSTREAM, 4);
        if (p) wr_u32(p, seg);
    });
    // 2. The segment context.
    memcpy(&S.d, &M.d, sizeof(S.d));
    S.primOpen = false; S.primBegun = false; S.primEnded = false; S.primHasPos = false;
    S.primUsed = 0; S.primEndOps = 0;
    S.primHdr[0] = S.primHdr[1] = S.primHdr[2] = 0;
    S.segBlk = -1; S.wr = S.end = nullptr;
    S.seg = seg; S.ops = 0; S.bytes = 0;
    // 3. The main context fenced.
    gxq_fence(M);
    sSegCallChunkCloses = 0;
    sSegCallInCurChunk  = true;
    return seg;
}
// Later part of the open chain.
uint16_t vita_gxq_seg_part_fork(PcDrPartStage& st) {
    GxqCtx& M = gGxqMain;
    uint16_t seg = ++sSegNext;
    if (seg == 0) seg = ++sSegNext;
    st.seg        = seg;
    st.gameFrame  = M.d.gameFrame;
    st.vtxImageOn = M.d.vtxImageOn;
    st.chanRule   = M.d.chanRule;
    st.genOnEmit  = M.d.genOnEmit;
    memcpy(st.gxProj, M.d.gxProj, sizeof(st.gxProj));
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_CALL_SUBSTREAM, 4);
        if (p) wr_u32(p, seg);
    });
    gxq_fence(M);
    sSegCallInCurChunk = true;
    return seg;
}
// Segment recorder at a part's first record.
void vita_gxq_seg_part_begin(const PcDrPartStage& st) {
    GxqCtx& S = gGxqSub;
    gxq_fence(S);
    memcpy(S.d.gxProj, st.gxProj, sizeof(S.d.gxProj));
    S.d.gameFrame  = st.gameFrame;
    S.d.vtxImageOn = uint8_t(S.d.vtxImageOn & st.vtxImageOn);
    S.d.chanRule   = uint8_t(S.d.chanRule & st.chanRule);
    S.d.genOnEmit  = uint8_t(S.d.genOnEmit & st.genOnEmit);
    S.primOpen = false; S.primBegun = false; S.primEnded = false; S.primHasPos = false;
    S.primUsed = 0; S.primEndOps = 0;
    S.primHdr[0] = S.primHdr[1] = S.primHdr[2] = 0;
    S.segBlk = -1; S.wr = S.end = nullptr;
    S.seg = st.seg;
}
void vita_gxq_seg_finish(uint32_t hasTexGen, uint32_t texMtxBase) {
    GxqCtx& S = gGxqSub;
    flush_vtx_desc(S);   // pending ops go out in this segment
    if (S.primOpen) { prim_stream_emit(S, true); S.primOpen = false; S.primEnded = false; }
    uint8_t* p = gxq_emit(S, GXQ_SUB_END, 12);
    if (p) { wr_u32(p, S.seg); wr_u32(p, hasTexGen); wr_u32(p, texMtxBase); }
    if (S.segBlk < 0) gxq_seg_take(S);
    gxq_seg_seal(S, true);
}
void vita_gxq_seg_retire(void) {
    GxqCtx& S = gGxqSub;
    if (S.deferred) {
        static bool said = false;   // logged once
        if (!said) {
            said = true;
            printf("[VITA DRAWREC] the segment recorder deferred a message (flags 0x%x): %s\n", unsigned(S.deferred),
                   S.deferredWhy ? S.deferredWhy : "?");
        }
        S.deferred    = 0;
        S.deferredWhy = nullptr;
    }
    sSegCallChunkCloses = -1;
    sSegCallInCurChunk  = false;
}
uint32_t vita_gxq_seg_bytes(void) { return gGxqSub.bytes; }
// Main latches follow the sticky switches.
void vita_gxq_seg_sync_latches(void) {
    GxqDecision& d = gGxqMain.d;
    d.vtxImageOn &= gVtxImageOn;
    d.chanRule   &= gChanRule;
    d.genOnEmit  &= gGenOnEmit;
}
void vita_gxq_seg_rescue(void) {
    if (!sSegCallInCurChunk) return;
    close_chunk();
}
// Texgen base repair: re-send the next texture-matrix load to every slot.
void vita_gxq_forget_tex_mtx(void) {
    for (GxqSh<12>& s : gGxqMain.d.texMtx) s.ep = 0;
}
void gxr_load_tex_mtx_splice(GxqCtx& R, const f32 mtx[3][4], u32 gxID, u32 known, u32 has, u32 base, const u32* poison,
                             int npoison) {
    if (gxq_refuse(R)) return;   // off-lock worker
    auto one = [&](GxqCtx& C) GXQ_NI {
        // Slots this load may land in (npoison < 0: every slot).
        if (npoison < 0) {
            for (GxqSh<12>& s : C.d.texMtx) s.ep = 0;
        } else {
            for (int i = 0; i < npoison; ++i) if (poison[i] < 64u) C.d.texMtx[poison[i]].ep = 0;
        }
        uint8_t* p = gxq_emit(C, GXQ_LOAD_TEX_MTX_SPLICE, 68);
        if (!p) return;
        memcpy(p, mtx, 48); p += 48;
        wr_u32(p, gxID); wr_u32(p, known); wr_u32(p, has); wr_u32(p, base); wr_u32(p, sSegNext);
    };
    one(R);
}

// Replay (translator thread)
static struct { bool valid; uint32_t seg, has, base; } sSplice;
// SPLICE resolved "has texgen".
static inline bool gxq_tr_splice_has(const uint8_t* q) {
    uint32_t known, has;
    memcpy(&known, q + 52, 4);
    memcpy(&has, q + 56, 4);
    return (known & 1u) ? has != 0u : (sSplice.valid && sSplice.has != 0u);
}
// Immediate-path fold chain admission (whitelist).
static inline void vita_imm_chain_admit(uint16_t op, const uint8_t* q) {
    VitaImmChain& g = g_vitaImmChain;
    switch (op) {
    case GXQ_NOP: case GXQ_PERF_SCOPE_BEGIN: case GXQ_PERF_SCOPE_END:
    case GXQ_CLEAR_VTX_DESC: case GXQ_SET_VTX_DESC: case GXQ_SET_VTX_ATTR_FMT: case GXQ_SET_ARRAY:
    case GXQ_CALL_SUBSTREAM: case GXQ_SUB_END:   // drawrec: the segment's frame (its ops are admitted one by one as they replay)
    case GXQ_LOAD_POS_MTX: case GXQ_LOAD_NRM_MTX: case GXQ_SET_CURRENT_MTX: case GXQ_LOAD_POSNRM_GROUP:
        return;
    case GXQ_LOAD_TEX_MTX_SPLICE:   // texture-matrix load, if resolved
        if (!gxq_tr_splice_has(q)) return;
        break;
    case GXQ_SET_TEV_COLOR: {    // u8 only; GX_TEVPREV and GX_TEVREG2 never
        int32_t reg; memcpy(&reg, q, 4);
        if (reg == (int32_t)GX_TEVREG0 && g.reg0Ok) return;
        if (reg == (int32_t)GX_TEVREG1 && g.reg1Ok) return;
        break; }
    case GXQ_SET_CHAN_MAT_COLOR: case GXQ_SET_CHAN_AMB_COLOR: {
        int32_t ch; memcpy(&ch, q, 4);
        const bool c0 = ch == (int32_t)GX_COLOR0 || ch == (int32_t)GX_ALPHA0 || ch == (int32_t)GX_COLOR0A0;
        if (c0 && (op == GXQ_SET_CHAN_MAT_COLOR ? g.mat0Ok : g.amb0Ok)) return;
        break; }
    default:
        break;
    }
    g.armed = 0;
}

// GXQ_CALL_SUBSTREAM: replays the segment's blocks in order.
static void replay(const uint8_t* p, const uint8_t* end);
static int      sTrSubDepth = 0;
static unsigned sTrTripSaid = 0;
static void gxq_tr_trip(const char* what, uint32_t a, uint32_t b) {
    if (sTrTripSaid++ == 0) printf("[VITA DRAWREC] translator tripwire: %s (%u, %u); said once\n", what, (unsigned)a, (unsigned)b);
}
static __attribute__((noinline)) void gxq_tr_substream(uint32_t seg) {
    if (sTrSubDepth != 0 || !sSegBlocks) { gxq_tr_trip("a nested or pool-less segment call", seg, sTrSubDepth); return; }
    ++sTrSubDepth;
    for (;;) {
        { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaSegReady, 1, NULL); pc_prof_idle_set(pvIdle); }
        int b = gxq_ring_pop(sSegReadyRing);
        if (b < 0) { gxq_tr_trip("a ready signal without a block", seg, 0); continue; }
        GxqSegBlock& blk    = sSegBlocks[b];
        const uint32_t used = __atomic_load_n(&blk.used, __ATOMIC_ACQUIRE);
        if (blk.seg != uint16_t(seg)) gxq_tr_trip("a block of another segment", seg, blk.seg);
        replay(blk.data, blk.data + used);
        const bool last = blk.last != 0;
        gxq_ring_push(sSegFreeRing, b);
        __atomic_sub_fetch(&sSegInUse, 1u, __ATOMIC_ACQ_REL);
        sceKernelSignalSema(sSemaSegFree, 1);
        if (last) break;
    }
    --sTrSubDepth;
}

static void replay(const uint8_t* p, const uint8_t* end) {
    while (p + sizeof(GxqHeader) <= end) {
        GxqHeader h; memcpy(&h, p, sizeof(h)); p += sizeof(h);
        const uint8_t* q = p;
        p += h.size;
        // Batch-key block dirty marks.
        if (h.op >= GXQ_SET_TEV_ORDER && h.op <= GXQ_SET_TEV_SWAP_MODE_TABLE) vita_key_dirty(1u);
        else if (h.op == GXQ_LOAD_LIGHT) vita_key_dirty(2u);   // chan ctrl / colours: the impl dirties the block
        else if (h.op == GXQ_SET_ALPHA_COMPARE) vita_key_dirty(4u);
        else if (h.op == GXQ_LOAD_TEX_MTX || h.op == GXQ_SET_TEX_COORD_GEN) vita_key_dirty(8u);
        // Fold chain admission.
        if (g_vitaImmChain.armed && h.op != GXQ_PRIM_STREAM) vita_imm_chain_admit(h.op, q);
        switch (h.op) {
        case GXQ_NOP: break;
        case GXQ_BEGIN_FRAME: {
            pc_gfx_begin_frame_impl();
        } break;
        case GXQ_PERF_SCOPE_BEGIN: {
            const char* a0 = (const char*)(uintptr_t)rd_u32(q);
            pc_gfx_perf_scope_begin_impl(a0);
        } break;
        case GXQ_PERF_SCOPE_END: {
            pc_gfx_perf_scope_end_impl();
        } break;
        case GXQ_SET_RENDER_SCALE: {
            const float a0 = rd_f32(q);
            pc_gfx_set_render_scale_impl(a0);
        } break;
        case GXQ_SET_ASPECT_RATIO_MODE: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_aspect_ratio_mode_impl((int)a0);
        } break;
        case GXQ_SET_PROJECTION: {
            const f32 (*a0)[4] = reinterpret_cast<const f32 (*)[4]>(q); q += 64;
            const int32_t a1 = rd_i32(q);
            pc_gfx_set_projection_impl(a0, (GXProjectionType)a1);
        } break;
        case GXQ_SET_VIEWPORT: {
            const float a0 = rd_f32(q);
            const float a1 = rd_f32(q);
            const float a2 = rd_f32(q);
            const float a3 = rd_f32(q);
            const float a4 = rd_f32(q);
            const float a5 = rd_f32(q);
            pc_gfx_set_viewport_impl(a0, a1, a2, a3, a4, a5);
        } break;
        case GXQ_SET_SCISSOR: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            pc_gfx_set_scissor_impl((u32)a0, (u32)a1, (u32)a2, (u32)a3);
        } break;
        case GXQ_LOAD_POS_MTX: {
            const f32 (*a0)[4] = reinterpret_cast<const f32 (*)[4]>(q); q += 48;
            const int32_t a1 = rd_i32(q);
            pc_gfx_load_pos_mtx_impl(a0, (u32)a1);
        } break;
        case GXQ_SET_CURRENT_MTX: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_current_mtx_impl((u32)a0);
        } break;
        case GXQ_LOAD_NRM_MTX: {
            const f32 (*a0)[4] = reinterpret_cast<const f32 (*)[4]>(q); q += 48;
            const int32_t a1 = rd_i32(q);
            pc_gfx_load_nrm_mtx_impl(a0, (u32)a1);
        } break;
        case GXQ_LOAD_TEX_MTX: {
            const f32 (*a0)[4] = reinterpret_cast<const f32 (*)[4]>(q); q += 48;
            const int32_t a1 = rd_i32(q);
            pc_gfx_load_tex_mtx_impl(a0, (u32)a1);
        } break;
        case GXQ_LOAD_POSNRM_GROUP: {
            // Plain recorders' op pairs, in order.
            const uint32_t count = rd_u32(q);
            for (uint32_t e = 0; e < count; ++e) {
                const uint32_t word = rd_u32(q);
                const f32 (*m)[4] = reinterpret_cast<const f32 (*)[4]>(q); q += 48;
                const u32 id = word & 0x3FFFFFFFu;
                if (word & 0x80000000u) pc_gfx_load_pos_mtx_impl(m, id);
                if (word & 0x40000000u) pc_gfx_load_nrm_mtx_impl(m, id);
            }
        } break;
        case GXQ_SET_TEX_COORD_GEN: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            pc_gfx_set_tex_coord_gen_impl((GXTexCoordID)a0, (GXTexGenType)a1, (GXTexGenSrc)a2, (u32)a3);
        } break;
        case GXQ_SET_Z_MODE: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            pc_gfx_set_z_mode_impl((GXBool)a0, (GXCompare)a1, (GXBool)a2);
        } break;
        case GXQ_SET_BLEND_MODE: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            pc_gfx_set_blend_mode_impl((GXBlendMode)a0, (GXBlendFactor)a1, (GXBlendFactor)a2, (GXLogicOp)a3);
        } break;
        case GXQ_SET_CULL_MODE: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_cull_mode_impl((GXCullMode)a0);
        } break;
        case GXQ_SET_COLOR_UPDATE: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_color_update_impl((GXBool)a0);
        } break;
        case GXQ_SET_ALPHA_UPDATE: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_alpha_update_impl((GXBool)a0);
        } break;
        case GXQ_SET_ALPHA_COMPARE: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            pc_gfx_set_alpha_compare_impl((GXCompare)a0, (u8)a1, (GXAlphaOp)a2, (GXCompare)a3, (u8)a4);
        } break;
        case GXQ_SET_CHAN_CTRL: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            const int32_t a5 = rd_i32(q);
            const int32_t a6 = rd_i32(q);
            pc_gfx_set_chan_ctrl_impl((GXChannelID)a0, (GXBool)a1, (GXColorSrc)a2, (GXColorSrc)a3, (u32)a4, (GXDiffuseFn)a5, (GXAttnFn)a6);
        } break;
        case GXQ_SET_CHAN_MAT_COLOR: {
            const int32_t a0 = rd_i32(q);
            GXColor a1; memcpy(&a1, q, 4); q += 4;
            pc_gfx_set_chan_mat_color_impl((GXChannelID)a0, a1);
        } break;
        case GXQ_SET_CHAN_AMB_COLOR: {
            const int32_t a0 = rd_i32(q);
            GXColor a1; memcpy(&a1, q, 4); q += 4;
            pc_gfx_set_chan_amb_color_impl((GXChannelID)a0, a1);
        } break;
        case GXQ_SET_TEV_ORDER: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            pc_gfx_set_tev_order_impl((GXTevStageID)a0, (GXTexCoordID)a1, (GXTexMapID)a2, (GXChannelID)a3);
        } break;
        case GXQ_SET_TEV_OP: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_set_tev_op_impl((GXTevStageID)a0, (GXTevMode)a1);
        } break;
        case GXQ_SET_NUM_TEV_STAGES: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_num_tev_stages_impl((u8)a0);
        } break;
        case GXQ_SET_TEV_COLOR_IN: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            pc_gfx_set_tev_color_in_impl((GXTevStageID)a0, (GXTevColorArg)a1, (GXTevColorArg)a2, (GXTevColorArg)a3, (GXTevColorArg)a4);
        } break;
        case GXQ_SET_TEV_ALPHA_IN: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            pc_gfx_set_tev_alpha_in_impl((GXTevStageID)a0, (GXTevAlphaArg)a1, (GXTevAlphaArg)a2, (GXTevAlphaArg)a3, (GXTevAlphaArg)a4);
        } break;
        case GXQ_SET_TEV_COLOR_OP: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            const int32_t a5 = rd_i32(q);
            pc_gfx_set_tev_color_op_impl((GXTevStageID)a0, (GXTevOp)a1, (GXTevBias)a2, (GXTevScale)a3, (GXBool)a4, (GXTevRegID)a5);
        } break;
        case GXQ_SET_TEV_ALPHA_OP: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            const int32_t a5 = rd_i32(q);
            pc_gfx_set_tev_alpha_op_impl((GXTevStageID)a0, (GXTevOp)a1, (GXTevBias)a2, (GXTevScale)a3, (GXBool)a4, (GXTevRegID)a5);
        } break;
        case GXQ_SET_TEV_COLOR: {
            const int32_t a0 = rd_i32(q);
            GXColor a1; memcpy(&a1, q, 4); q += 4;
            pc_gfx_set_tev_color_impl((GXTevRegID)a0, a1);
        } break;
        case GXQ_SET_TEV_COLOR_S10: {
            const int32_t a0 = rd_i32(q);
            GXColorS10 a1; memcpy(&a1, q, 8); q += 8;
            pc_gfx_set_tev_color_s10_impl((GXTevRegID)a0, a1);
        } break;
        case GXQ_SET_TEV_KCOLOR: {
            const int32_t a0 = rd_i32(q);
            GXColor a1; memcpy(&a1, q, 4); q += 4;
            pc_gfx_set_tev_kcolor_impl((GXTevKColorID)a0, a1);
        } break;
        case GXQ_SET_TEV_KCOLOR_SEL: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_set_tev_kcolor_sel_impl((GXTevStageID)a0, (GXTevKColorSel)a1);
        } break;
        case GXQ_SET_TEV_KALPHA_SEL: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_set_tev_kalpha_sel_impl((GXTevStageID)a0, (GXTevKAlphaSel)a1);
        } break;
        case GXQ_SET_TEV_SWAP_MODE: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            pc_gfx_set_tev_swap_mode_impl((GXTevStageID)a0, (GXTevSwapSel)a1, (GXTevSwapSel)a2);
        } break;
        case GXQ_SET_TEV_SWAP_MODE_TABLE: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            pc_gfx_set_tev_swap_mode_table_impl((GXTevSwapSel)a0, (GXTevColorChan)a1, (GXTevColorChan)a2, (GXTevColorChan)a3, (GXTevColorChan)a4);
        } break;
        case GXQ_INIT_TEX_OBJ: {
            GXTexObj* a0 = (GXTexObj*)(uintptr_t)rd_u32(q);
            void* a1 = (void*)(uintptr_t)rd_u32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            const int32_t a5 = rd_i32(q);
            const int32_t a6 = rd_i32(q);
            const int32_t a7 = rd_i32(q);
            pc_gfx_init_tex_obj_impl(a0, a1, (u16)a2, (u16)a3, (GXTexFmt)a4, (GXTexWrapMode)a5, (GXTexWrapMode)a6, (GXBool)a7);
        } break;
        case GXQ_INIT_TEX_OBJ_CI: {
            GXTexObj* a0 = (GXTexObj*)(uintptr_t)rd_u32(q);
            void* a1 = (void*)(uintptr_t)rd_u32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            const int32_t a4 = rd_i32(q);
            const int32_t a5 = rd_i32(q);
            const int32_t a6 = rd_i32(q);
            const int32_t a7 = rd_i32(q);
            const int32_t a8 = rd_i32(q);
            pc_gfx_init_tex_obj_ci_impl(a0, a1, (u16)a2, (u16)a3, (GXCITexFmt)a4, (GXTexWrapMode)a5, (GXTexWrapMode)a6, (GXBool)a7, (u32)a8);
        } break;
        case GXQ_LOAD_TLUT: {
            GXTlutObj* a0 = (GXTlutObj*)(uintptr_t)rd_u32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_load_tlut_impl(a0, (u32)a1);
        } break;
        case GXQ_LOAD_TEX_OBJ: {
            GXTexObj* a0 = (GXTexObj*)(uintptr_t)rd_u32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_load_tex_obj_impl(a0, (GXTexMapID)a1);
        } break;
        case GXQ_BEGIN: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            pc_gfx_begin_impl((GXPrimitive)a0, (GXVtxFmt)a1, (u16)a2);
        } break;
        case GXQ_PUSH_U8: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_push_u8_impl((u8)a0);
        } break;
        case GXQ_PUSH_U16: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_push_u16_impl((u16)a0);
        } break;
        case GXQ_PUSH_U32: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_push_u32_impl((u32)a0);
        } break;
        case GXQ_PUSH_S8: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_push_s8_impl((s8)a0);
        } break;
        case GXQ_PUSH_S16: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_push_s16_impl((s16)a0);
        } break;
        case GXQ_PUSH_S32: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_push_s32_impl((s32)a0);
        } break;
        case GXQ_PUSH_F32: {
            const float a0 = rd_f32(q);
            pc_gfx_push_f32_impl(a0);
        } break;
        case GXQ_POSITION: {
            const float a0 = rd_f32(q);
            const float a1 = rd_f32(q);
            const float a2 = rd_f32(q);
            pc_gfx_position_impl(a0, a1, a2);
        } break;
        case GXQ_COLOR: {
            const int32_t a0 = rd_i32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const int32_t a3 = rd_i32(q);
            pc_gfx_color_impl((u8)a0, (u8)a1, (u8)a2, (u8)a3);
        } break;
        case GXQ_TEXCOORD: {
            const float a0 = rd_f32(q);
            const float a1 = rd_f32(q);
            pc_gfx_texcoord_impl(a0, a1);
        } break;
        case GXQ_END: {
            pc_gfx_end_impl();
        } break;
        case GXQ_CALL_DISPLAY_LIST: {
            const void* a0 = (const void*)(uintptr_t)rd_u32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_call_display_list_impl(a0, (u32)a1);
        } break;
        case GXQ_COPY_DISP: {
            void* a0 = (void*)(uintptr_t)rd_u32(q);
            const int32_t a1 = rd_i32(q);
            pc_gfx_copy_disp_impl(a0, (GXBool)a1);
        } break;
        case GXQ_SET_COPY_CLEAR: {
            GXColor a0; memcpy(&a0, q, 4); q += 4;
            const int32_t a1 = rd_i32(q);
            pc_gfx_set_copy_clear_impl(a0, (u32)a1);
        } break;
        case GXQ_BEGIN_CAPTURE: {
            uint64_t a0; memcpy(&a0, q, 8); q += 8;
            pc_gfx_begin_capture_impl(a0);
        } break;
        case GXQ_END_CAPTURE: {
            pc_gfx_end_capture_impl();
        } break;
        case GXQ_ENABLE_CAPTURE: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_enable_capture_impl((bool)a0);
        } break;
        case GXQ_SET_SHADER_SPECIALISATION: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_shader_specialisation_impl((bool)a0);
        } break;
        case GXQ_FLUSH_BATCH: {
            pc_gfx_flush_batch_impl();
        } break;
        case GXQ_CLEAR_VTX_DESC: pc_gfx_clear_vtx_desc_impl(); break;
        case GXQ_SET_VTX_DESC: { const int32_t a0 = rd_i32(q); const int32_t a1 = rd_i32(q);
            pc_gfx_set_vtx_desc_impl((GXAttr)a0, (GXAttrType)a1); } break;
        case GXQ_SET_VTX_ATTR_FMT: { const int32_t a0 = rd_i32(q); const int32_t a1 = rd_i32(q); const int32_t a2 = rd_i32(q); const int32_t a3 = rd_i32(q); const int32_t a4 = rd_i32(q);
            pc_gfx_set_vtx_attr_fmt_impl((GXVtxFmt)a0, (GXAttr)a1, (GXCompCnt)a2, (GXCompType)a3, (u8)a4); } break;
        case GXQ_SET_ARRAY: { const int32_t a0 = rd_i32(q); void* a1 = (void*)(uintptr_t)rd_u32(q); const int32_t a2 = rd_i32(q);
            pc_gfx_set_array_impl((GXAttr)a0, a1, (u8)a2);
            pc_gfx_note_array_volatile(a0, (uint32_t(a2) & kGxqArrayVolatile) != 0); } break;
        case GXQ_LOAD_LIGHT: {
            const uint32_t mask = rd_u32(q);
            pc_gfx_load_light_impl(const_cast<uint8_t*>(q), mask);
        } break;
        case GXQ_INIT_TLUT_OBJ: {
            GXTlutObj* obj = (GXTlutObj*)(uintptr_t)rd_u32(q);
            const int32_t fmt = rd_i32(q);
            const int32_t n = rd_i32(q);
            pc_gfx_init_tlut_obj_impl(obj, const_cast<uint8_t*>(q), (GXTlutFmt)fmt, (u16)n);
        } break;
        case GXQ_PRESENT: {
            pc_gfx_present_impl();
            sceKernelSignalSema(sSemaFrame, 1);
        } break;
        case GXQ_SET_FOG: {
            const int32_t en = rd_i32(q);
            const float a1 = rd_f32(q), a2 = rd_f32(q), a3 = rd_f32(q), a4 = rd_f32(q);
            const int32_t r = rd_i32(q), g = rd_i32(q), b = rd_i32(q);
            pc_gfx_set_fog_impl(en != 0, a1, a2, a3, a4, (unsigned char)r, (unsigned char)g, (unsigned char)b);
        } break;
        case GXQ_SET_SECTION_CLIP_43: {
            const int32_t a0 = rd_i32(q);
            pc_gfx_set_section_clip_43_impl(a0);
        } break;
        case GXQ_SET_SCISSOR_PANEL: pc_gfx_set_scissor_panel_impl(); break;
        case GXQ_SET_SCISSOR_UI_BOX: pc_gfx_set_scissor_ui_box_impl(); break;
        case GXQ_SET_MOVIE_PLANES_LINEAR: { const int32_t a0 = rd_i32(q); pc_gfx_set_movie_planes_linear_impl(a0); } break;
        case GXQ_COPY_TEX: {
            void* a0 = (void*)(uintptr_t)rd_u32(q);
            const int32_t clr = rd_i32(q);
            const int32_t sx = rd_i32(q), sy = rd_i32(q), sw = rd_i32(q), sh = rd_i32(q);
            const int32_t dw = rd_i32(q), dh = rd_i32(q);
            const int32_t fmt = rd_i32(q), mip = rd_i32(q);
            pc_gfx_copy_tex_impl(a0, (GXBool)clr, (u16)sx, (u16)sy, (u16)sw, (u16)sh, (u16)dw, (u16)dh, (GXTexFmt)fmt, (GXBool)mip);
        } break;
        case GXQ_LEVEL_FLUSH: {
            const void* ranges = (const void*)(uintptr_t)rd_u32(q);
            const int32_t count = rd_i32(q);
            pc_gfx_level_flush_impl(ranges, count);
        } break;
        case GXQ_EFB_COMPOSITE: {
            const int32_t kind = rd_i32(q);
            const void* d0 = (const void*)(uintptr_t)rd_u32(q);
            const void* d1 = (const void*)(uintptr_t)rd_u32(q);
            const float p0 = rd_f32(q), p1 = rd_f32(q);
            pc_gfx_efb_composite_impl(kind, d0, d1, p0, p1);
        } break;
        case GXQ_EFB_TARGET: {
            const int32_t on = rd_i32(q), w = rd_i32(q), h = rd_i32(q);
            pc_gfx_efb_target_impl(on, w, h);
        } break;
        case GXQ_SET_COPY_FILTER: {
            const int32_t vf = rd_i32(q);
            u8 f[7];
            for (int i = 0; i < 7; ++i) f[i] = (u8)rd_i32(q);
            pc_gfx_set_copy_filter_impl(vf, f);
        } break;
        case GXQ_INIT_TEX_OBJ_LOD: {
            GXTexObj* a0 = (GXTexObj*)(uintptr_t)rd_u32(q);
            const int32_t a1 = rd_i32(q);
            const int32_t a2 = rd_i32(q);
            const float a3 = rd_f32(q);
            const float a4 = rd_f32(q);
            const float a5 = rd_f32(q);
            pc_gfx_init_tex_obj_lod_impl(a0, (GXTexFilter)a1, (GXTexFilter)a2, a3, a4, a5);
        } break;
        case GXQ_SET_LINE_WIDTH: { const int32_t a0 = rd_i32(q); pc_gfx_set_line_width_impl(u8(a0)); } break;
        case GXQ_PRIM_STREAM: {
            const int32_t flags = rd_i32(q);
            const int32_t a0 = rd_i32(q), a1 = rd_i32(q), a2 = rd_i32(q);
            const uint32_t bytes = rd_u32(q);
            pc_gfx_prim_stream_impl((GXPrimitive)a0, (GXVtxFmt)a1, (u16)(a2 > 65535 ? 65535 : a2), q, bytes, flags);
        } break;
        case GXQ_SYNC:
            sceKernelSignalSema(sSemaSync, 1);
            break;
        case GXQ_CALL_SUBSTREAM: {   // draw-record segment
            const uint32_t seg = rd_u32(q);
            gxq_tr_substream(seg);
        } break;
        case GXQ_SUB_END: {          // segment end values
            sSplice.seg   = rd_u32(q);
            sSplice.has   = rd_u32(q);
            sSplice.base  = rd_u32(q);
            sSplice.valid = true;
        } break;
        case GXQ_LOAD_TEX_MTX_SPLICE: {
            // Resolved texgen load.
            const f32 (*m)[4] = reinterpret_cast<const f32 (*)[4]>(q); q += 48;
            const uint32_t gxID = rd_u32(q), known = rd_u32(q), has = rd_u32(q), base = rd_u32(q), seg = rd_u32(q);
            if ((known & 3u) != 3u && (!sSplice.valid || sSplice.seg != (seg & 0xFFFFu))) {
                gxq_tr_trip("a splice without its segment's end values", seg, sSplice.valid ? sSplice.seg : 0xFFFFFFFFu);
                if (!sSplice.valid) break;
            }
            const uint32_t rh = (known & 1u) ? has : sSplice.has;
            const uint32_t rb = (known & 2u) ? base : sSplice.base;
            if (rh) {
                vita_key_dirty(8u);
                pc_gfx_load_tex_mtx_impl(m, rb + gxID);
            }
        } break;
        default:
            {
                static bool said = false;   // logged once
                if (!said) { said = true; printf("[VITA GXQ] unknown op %u\n", (unsigned)h.op); }
            }
            break;
        }
    }
}

static int translator_main(SceSize, void*) {
    pc_prof_register(1);   // NECTAR_PROF only
    sReady = true;
    int readChunk = 0;
    for (;;) {
        { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaReady, 1, NULL); pc_prof_idle_set(pvIdle); }
        GxqChunk& c = sChunks[readChunk];
        replay(c.data, c.data + c.used);
        readChunk = (readChunk + 1) % kChunks;
        sceKernelSignalSema(sSemaFree, 1);
    }
    return 0;
}



bool vita_gxq_start(void) {
    for (int i = 0; i < kChunks; ++i) {
        sChunks[i].data = static_cast<uint8_t*>(malloc(kChunkSize));
        if (!sChunks[i].data) { printf("[VITA GXQ] chunk alloc failed\n"); return false; }
    }
    // Segment pool: 32 blocks of 64 KB.
    gxq_seg_pool_init(32);
    // Main context setup.
    GxqCtx& M = gGxqMain;
    M.isMain  = 1;
    M.sink    = kGxqSinkRing;
    M.primBuf = sPrimBuf;
    M.primCap = sizeof(sPrimBuf);
#if NECTAR_GXQ_LEAN
    M.wr  = sChunks[0].data;   // lean core cursor
    M.end = sChunks[0].data + kChunkSize;
#endif
    M.d.ep    = __atomic_add_fetch(&sEpochSource, 1u, __ATOMIC_RELAXED);
    memset(M.d.shDesc, 0xFF, sizeof(M.d.shDesc));
    memset(M.d.shFmt, 0xFF, sizeof(M.d.shFmt));
    memset(M.d.shArrayBase, 0xFF, sizeof(M.d.shArrayBase));
    memset(M.d.shArrayStride, 0xFF, sizeof(M.d.shArrayStride));
    M.d.staleSeen = sStaleGen;
    sSemaReady = sceKernelCreateSema("gxq_ready", 0, 0, kChunks, NULL);
    sSemaFree  = sceKernelCreateSema("gxq_free", 0, kChunks - 1, kChunks, NULL);
    sSemaFrame = sceKernelCreateSema("gxq_frame", 0, vita_gxq_pipe_depth(), vita_gxq_pipe_depth() + 1, NULL);
    sSemaSync  = sceKernelCreateSema("gxq_sync", 0, 0, 1, NULL);
    sGameThread = sceKernelGetThreadId();
    // Translator thread: core 2, below the audio/DVD workers.
    sThread = sceKernelCreateThread("nectar_gx", translator_main, 0xB0, 0x40000,
                                    0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (sThread < 0) { printf("[VITA GXQ] thread create failed 0x%08X\n", (unsigned)sThread); return false; }
    sceKernelStartThread(sThread, 0, NULL);
    while (!sReady) sceKernelDelayThread(1000);
    memcpy(M.d.pendDesc, M.d.shDesc, sizeof(M.d.pendDesc));
    memcpy(M.d.pendFmt, M.d.shFmt, sizeof(M.d.pendFmt));
    M.d.pendDirty = false;
    // Recording, never cleared.
    sRecording  = true;
    M.recording = 1;
    // Stages S1 and S4 on.
    gVtxImageOn = 1;
    sDlRegWritesAtStart = sDlRegWrites;
    gChanRule   = 1;
    gGenOnEmit  = 1;
    M.d.vtxImageOn = gVtxImageOn; M.d.chanRule = gChanRule; M.d.genOnEmit = gGenOnEmit;   // latches
    return true;
}

// Queued setters for fog, section clip and plate scissors
static bool sGxqSectionClip43 = false;   // game thread view of the flag
GXQ_REC void rec_set_fog(GxqCtx& R, bool enable, float startZ, float endZ, float nearZ, float farZ, unsigned char r, unsigned char g, unsigned char b) {
    if (!sRecording) { pc_gfx_set_fog_impl(enable, startZ, endZ, nearZ, farZ, r, g, b); return; }
    uint8_t* p = gxq_emit(R, GXQ_SET_FOG, 32);
    if (!p) return;
    wr_i32(p, enable ? 1 : 0); wr_f32(p, startZ); wr_f32(p, endZ); wr_f32(p, nearZ); wr_f32(p, farZ);
    wr_i32(p, (int32_t)r); wr_i32(p, (int32_t)g); wr_i32(p, (int32_t)b);
}
void pc_gfx_set_fog(bool enable, float startZ, float endZ, float nearZ, float farZ, unsigned char r, unsigned char g, unsigned char b) {
    rec_set_fog(gGxqMain, enable, startZ, endZ, nearZ, farZ, r, g, b);
}
void pc_gfx_set_section_clip_43(int on) {
    sGxqSectionClip43 = on != 0;
    if (!sRecording) { pc_gfx_set_section_clip_43_impl(on); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_SECTION_CLIP_43, 4);
        if (!p) return;
        wr_i32(p, (int32_t)on);
    });
}
int pc_gfx_get_section_clip_43(void) { return sGxqSectionClip43 ? 1 : 0; }
void pc_gfx_set_scissor_panel(void) {
    if (!sRecording) { pc_gfx_set_scissor_panel_impl(); return; }
    gxq_each([](GxqCtx& R) GXQ_NI { gxq_emit(R, GXQ_SET_SCISSOR_PANEL, 0); });
}
void pc_gfx_set_scissor_ui_box(void) {
    if (!sRecording) { pc_gfx_set_scissor_ui_box_impl(); return; }
    gxq_each([](GxqCtx& R) GXQ_NI { gxq_emit(R, GXQ_SET_SCISSOR_UI_BOX, 0); });
}
void pc_gfx_set_movie_planes_linear(int on) {
    if (!sRecording) { pc_gfx_set_movie_planes_linear_impl(on); return; }
    gxq_each([&](GxqCtx& R) GXQ_NI {
        uint8_t* p = gxq_emit(R, GXQ_SET_MOVIE_PLANES_LINEAR, 4);
        if (!p) return;
        wr_i32(p, (int32_t)on);
    });
}

extern "C" void vita_gxq_sync(void) {
    if (!sRecording) return;
    if (sceKernelGetThreadId() != sGameThread) return;   // only the recording thread may drain
    vita_drawrec_join(kDrJoinSync);   // join an open segment first
    gxq_each([](GxqCtx& R) GXQ_NI { gxq_emit(R, GXQ_SYNC, 0); });
    close_chunk();
    { const int pvIdle = pc_prof_idle_set(1); sceKernelWaitSema(sSemaSync, 1, NULL); pc_prof_idle_set(pvIdle); }
}

extern "C" void vita_gxq_note_free(const void* ptr, size_t bytes) {
    (void)ptr;
    if (!sRecording) return;
    // Join an open segment before a free.
    if (sSegCallChunkCloses >= 0) vita_drawrec_join(kDrJoinFree);
    if (bytes < 65536) return;
    vita_gxq_sync();
}
