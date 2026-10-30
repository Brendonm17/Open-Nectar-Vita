#pragma once
// GX recorder context.
#include <stddef.h>
#include <stdint.h>
#include "Dolphin/gx.h"
#include "vita_gxq.h"

#ifndef NECTAR_GXQ_LEAN
#define NECTAR_GXQ_LEAN 1
#endif

// One shadow: the last N argument words, valid iff ep == d.ep.
template <int N> struct GxqSh {
    uint32_t ep;
    int32_t  v[N];
};

// Recorder decision state.
struct GxqDecision {
    uint32_t  ep;
    uint32_t  shArrayStride[26];
    // Generations DGXGraphics compares its caches with.
    unsigned  stateGen[5], stateGenEmit[5], vtxGen, matGen;
    uint8_t   vtxImageOn, chanRule, genOnEmit;   // latches of gVtxImageOn, gChanRule, gGenOnEmit
    uint32_t  staleSeen;           // last seen stale counter
    // Pre-record cull state.
    unsigned  gxCurMtx, gameFrame;
    GxqSh<5>  alphaCmp;
    GxqSh<3>  zMode;
    GxqSh<4>  blend;
    GxqSh<1>  cull, colorUpd, alphaUpd, curMtx, numStages;
    GxqSh<4>  tevCIn[16];
    GxqSh<5>  tevCOp[16];
    GxqSh<4>  tevAIn[16];
    GxqSh<5>  tevAOp[16];
    GxqSh<3>  tevOrder[16];
    GxqSh<1>  kcSel[16], kaSel[16], tevOp[16];
    GxqSh<2>  swap[16];
    GxqSh<3>  tevReg[4];           // tag word, then one or two colour words
    GxqSh<1>  kColor[4];
    GxqSh<4>  swapTable[4];
    GxqSh<6>  chanCtrl[6];
    GxqSh<1>  chanMat[2], chanAmb[2];
    GxqSh<3>  texGen[8];
    // Vertex format: sent and pending.
    uintptr_t shArrayBase[26];
    uint32_t  pendDesc[26], pendDescMask, pendFmtMask[8];
    bool      pendDirty;
    uint32_t  shDesc[26];
    float     gxProj[4][4];        // projection
    GxqSh<12> posMtx[64], nrmMtx[64], texMtx[64];   // 12 floats each, as bits
    uint32_t  pendFmt[8][26], shFmt[8][26];
    uint8_t   light[8][0x40];
    uint32_t  lightEp[8];
    int32_t   proj[17];            // 16 floats and the type, as bits
    uint32_t  projEp;
};

// Sinks.
enum : uint8_t {
    kGxqSinkRing = 0,   // main chunk ring
    kGxqSinkSeg  = 1    // segment blocks
};

struct alignas(32) GxqCtx {
    uint8_t*    wr;                 // ring write cursor
    uint8_t*    end;                // end of write chunk
    uint32_t    ops;                // ops recorded
    uint8_t     isMain;             // game thread recorder
    uint8_t     sink;               // kGxqSink*
    uint8_t     recording;
    bool        primOpen;           // immediate-mode primitive open
    uint8_t*    primBuf;            // immediate-mode primitive bytes
    uint32_t    primUsed, primCap;
    GxqDecision d;
    uint32_t    bytes;              // segment bytes
    uint32_t    primEndOps;
    bool        primBegun, primEnded, primHasPos;
    int32_t     primHdr[3];
    // Deferred logs (kGxqDefer*).
    uint32_t    deferred;
    const char* deferredWhy;
    // Segment block (-1: none) and segment number.
    int32_t     segBlk;
    uint16_t    seg;
};
enum { kGxqDeferPayload = 1u << 0, kGxqDeferChanRule = 1u << 3, kGxqDeferMainOnly = 1u << 4, kGxqDeferDgx = 1u << 5 };

extern GxqCtx   gGxqMain;      // game thread recorder

// Frame-level call on a non-main context.
void gxq_main_only_violation(GxqCtx& R);
static inline void pcMainOnly(GxqCtx& R) {
    if (__builtin_expect(!R.isMain, 0)) gxq_main_only_violation(R);
}

// Off-lock workers.
struct GxqOffLockBound { uintptr_t lo, span; };
extern GxqOffLockBound gGxqOffLockBound;
bool vita_offlock_precise(uintptr_t sp);
extern "C" void vita_offlock_register(uintptr_t lo, uintptr_t hi);
extern volatile uint8_t gDrawrecKill;
static inline __attribute__((always_inline, no_instrument_function)) bool vita_offlock_worker(void) {
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    if (__builtin_expect(sp - gGxqOffLockBound.lo >= gGxqOffLockBound.span, 1)) return false;
    return vita_offlock_precise(sp);
}
void gxq_foreign_refused(GxqCtx& R) __attribute__((noinline, cold));
// Refuses a call into the main context from an off-lock worker.
static inline __attribute__((always_inline, no_instrument_function)) bool gxq_refuse(GxqCtx& R) {
    if (__builtin_expect(!vita_offlock_worker(), 1) || !R.isMain) return false;
    gxq_foreign_refused(R);
    return true;
}

// Main-context MATGEN bump.
static inline void gxq_main_matgen(void) {
    if (gxq_refuse(gGxqMain)) return;
    ++gGxqMain.d.matGen;
}
static inline void gxq_main_vtxgen(void) {
    if (gxq_refuse(gGxqMain)) return;
    ++gGxqMain.d.vtxGen;
}

// GX-level entries.
struct _GXVtxDescList;
struct _GXVtxAttrFmtList;
void gxr_GXClearVtxDesc(GxqCtx& R);
void gxr_GXSetVtxAttrFmtv(GxqCtx& R, GXVtxFmt fmt, const _GXVtxAttrFmtList* list);
void gxr_GXSetVtxDescv(GxqCtx& R, const _GXVtxDescList* list);
void gxr_GXInvalidateVtxCache(GxqCtx& R);
void gxr_GXSetProjection(GxqCtx& R, const f32 mtx[4][4], GXProjectionType type);
void gxr_GXSetFog(GxqCtx& R, GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color);
void gxr_GXSetTevOp(GxqCtx& R, GXTevStageID stage, GXTevMode mode);
void gxr_GXSetChanCtrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask,
                       GXDiffuseFn diffFn, GXAttnFn attnFn);
void gxr_GXLoadLightObjImm(GxqCtx& R, GXLightObj* ltObj, GXLightID ltId);
void gxr_GXCallDisplayList(GxqCtx& R, const void* list, u32 nbytes);
void gxr_GXBegin(GxqCtx& R, GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void gxr_GXEnd(GxqCtx& R);
void gxr_GXCopyDisp(GxqCtx& R, void* dest, GXBool clear);   // main context only
void gxr_pc_gfx_set_chan_ctrl(GxqCtx& R, GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask,
                              GXDiffuseFn diffFn, GXAttnFn attnFn);
#if !NECTAR_GXQ_LEAN
void gxr_GXSetVtxDesc(GxqCtx& R, GXAttr attr, GXAttrType type);
void gxr_GXSetVtxAttrFmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
void gxr_GXSetArray(GxqCtx& R, GXAttr attr, const void* basePtr, u8 stride);
void gxr_GXLoadPosMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id);
void gxr_GXLoadNrmMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id);
void gxr_GXLoadTexMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id, GXTexMtxType type);
void gxr_GXSetCurrentMtx(GxqCtx& R, u32 id);
void gxr_GXSetZMode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable);
void gxr_GXSetBlendMode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op);
void gxr_GXSetAlphaCompare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1);
void gxr_GXSetColorUpdate(GxqCtx& R, GXBool updateEnable);
void gxr_GXSetZCompLoc(GxqCtx& R, GXBool beforeTex);
void gxr_GXSetCullMode(GxqCtx& R, GXCullMode mode);
void gxr_GXSetLineWidth(GxqCtx& R, u8 width, GXTexOffset texOffset);
void gxr_GXSetPointSize(GxqCtx& R, u8 size, GXTexOffset texOffset);
void gxr_GXSetTevOrder(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan);
void gxr_GXSetTevColorIn(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d);
void gxr_GXSetTevAlphaIn(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d);
void gxr_GXSetTevColorOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg);
void gxr_GXSetTevAlphaOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg);
void gxr_GXSetTevColor(GxqCtx& R, GXTevRegID reg, GXColor color);
void gxr_GXSetTevColorS10(GxqCtx& R, GXTevRegID reg, GXColorS10 color);
void gxr_GXSetTevKColor(GxqCtx& R, GXTevKColorID id, GXColor color);
void gxr_GXSetTevKColorSel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel);
void gxr_GXSetTevKAlphaSel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel);
void gxr_GXSetNumTevStages(GxqCtx& R, u8 num);
void gxr_GXSetNumTexGens(GxqCtx& R, u8 num);
void gxr_GXSetTexCoordGen2(GxqCtx& R, GXTexCoordID dstCoord, GXTexGenType func, GXTexGenSrc srcParam, u32 mtx, GXBool normalize,
                           u32 postMtx);
void gxr_GXSetNumChans(GxqCtx& R, u8 num);
void gxr_GXSetChanMatColor(GxqCtx& R, GXChannelID chan, GXColor matColor);
void gxr_GXSetChanAmbColor(GxqCtx& R, GXChannelID chan, GXColor ambColor);
void gxr_GXLoadTexObj(GxqCtx& R, GXTexObj* obj, GXTexMapID id);
void gxr_GXPosition3f32(GxqCtx& R, f32 x, f32 y, f32 z);
void gxr_GXNormal3f32(GxqCtx& R, f32 x, f32 y, f32 z);
void gxr_GXColor1u32(GxqCtx& R, u32 c);
void gxr_GXTexCoord2f32(GxqCtx& R, f32 u, f32 v);

// Recorder-level entries.
void gxr_pc_gfx_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op);
void gxr_pc_gfx_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1);
void gxr_pc_gfx_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable);
void gxr_pc_gfx_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color);
void gxr_pc_gfx_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d);
void gxr_pc_gfx_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d);
void gxr_pc_gfx_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID outReg);
void gxr_pc_gfx_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID outReg);
void gxr_pc_gfx_set_num_tev_stages(GxqCtx& R, u8 num);
void gxr_pc_gfx_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan);
void gxr_pc_gfx_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel);
void gxr_pc_gfx_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel);
void gxr_pc_gfx_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color);
#endif

// DGXGraphics helpers.
int      gxr_vtx_image(GxqCtx& R, const GxqVtxImage& img);
void     gxr_load_posnrm_group(GxqCtx& R, int n, const u32* ids, const f32 (*const* mtx)[4]);
void     gxr_note_register_list(GxqCtx& R);
void     gxr_set_vtx_attr_fmtv_calls(GxqCtx& R, int fmt, const _GXVtxAttrFmtList* list);
void     gxr_set_vtx_descv_calls(GxqCtx& R, const _GXVtxDescList* list);

// Spliced segment.
extern GxqCtx gGxqSub;
bool     vita_gxq_seg_pool_ready(void);
unsigned vita_gxq_seg_free_blocks(void);
unsigned vita_gxq_seg_block_bytes(void);
// Forks a segment on the main stream.
uint16_t vita_gxq_seg_fork(void);
// Ends the segment.
void     vita_gxq_seg_finish(uint32_t hasTexGen, uint32_t texMtxBase);
// Retires the segment at the join.
void     vita_gxq_seg_retire(void);
// The segment's size so far (bytes recorded into it since the fork).
uint32_t vita_gxq_seg_bytes(void);
// Syncs the stage latches.
void     vita_gxq_seg_sync_latches(void);
// Closes the main chunk so the translator reaches the segment CALL.
void     vita_gxq_seg_rescue(void);
bool     vita_gxq_seg_call_open(void);          // CALL still in the open main chunk
bool     vita_gxq_seg_take_starved(void);
void     vita_gxq_seg_signal_j(void);
int      vita_gxq_seg_wait_j(unsigned timeoutUs);   // < 0 on timeout
// Texgen matrix load resolved by the translator.
void     gxr_load_tex_mtx_splice(GxqCtx& R, const f32 mtx[3][4], u32 gxID, u32 known, u32 has, u32 base, const u32* poison,
                                 int npoison);
// Forgets every texture-matrix shadow of the main context.
void     vita_gxq_forget_tex_mtx(void);
// Chain parts.
struct PcDrPartStage {
    uint16_t seg;
    uint8_t  vtxImageOn, chanRule, genOnEmit;
    unsigned gameFrame;
    float    gxProj[4][4];
};
// Forks a chain part.
uint16_t vita_gxq_seg_part_fork(PcDrPartStage& st);
// Begins a chain part on the segment recorder.
void     vita_gxq_seg_part_begin(const PcDrPartStage& st);
// Main chunk hand-overs since the open chain's first CALL (-1: none open).
int      vita_gxq_seg_call_chunk_closes(void);

#if NECTAR_GXQ_LEAN
#include "vita_gxq_rec.h"
#endif
