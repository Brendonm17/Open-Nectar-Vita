/**
 * @file gx_stubs.cpp
 * @brief Stub implementations for all Dolphin GX (Graphics) functions.
 * All signatures match the exact declarations in the Dolphin/GX/ headers.
 *
 * In Stage 4 these will be replaced with real OpenGL calls.
 */
#include "Dolphin/gx.h"
#include "Dolphin/vi.h"
#include "pc_gfx.h"
#include "settings/pc_settings.h"   // the Fog row (GXSetFog)

#include <cstdio>
#include <cstring>

/* ── Render mode objects ── */
GXRenderModeObj GXNtsc480IntDf    = {};
GXRenderModeObj GXNtsc480Int      = {};
GXRenderModeObj GXMpal480IntDf    = {};
GXRenderModeObj GXMpal480Int      = {};
GXRenderModeObj GXPal528IntDf     = {};
GXRenderModeObj GXEurgb60Hz480IntDf = {};
GXRenderModeObj* sRenderMode = nullptr;

unsigned g_pcGameFrame = 0;   // bumped by GXCopyDisp, once per frame
#if defined(__vita__)
// Vita: the recording GX wrappers are gGxqMain's GX-level entries (vita_gxq_ctx.h).
#include "vita_gxq_ctx.h"
extern "C" unsigned pc_gx_vtx_gen(void) { return gGxqMain.d.vtxGen; }
extern "C" unsigned pc_gx_mat_gen(void) { return gGxqMain.d.matGen; }
#define VTXGEN() gxq_main_vtxgen()
#define MATGEN() gxq_main_matgen()
#else
// State generations.
unsigned g_pcGxVtxGen = 1;
unsigned g_pcMeshCulled = 0;   // meshes skipped before recording (DGXGraphics::drawSingleMatpoly)
unsigned g_pcMatSkipped = 0;   // setMaterial calls the cache answered (DGXGraphics::setMaterial)
unsigned g_pcMeshTested = 0;   // meshes the pre-record cull was allowed to test
unsigned g_pcMeshNoBounds = 0; // of those, meshes a display list had no published bounds for
// Why the material cache last missed, by key component.
unsigned g_pcMatMissWhy[8] = {};
// Game-thread mirrors of the translator's frustum-test state.
float g_pcGxProj[4][4] = {};
unsigned g_pcGxCurMtx = 0;
unsigned g_pcMeshPass = 0;    // bumped by DGXGraphics::initMesh, once per shape draw
unsigned g_pcGxMatGen = 1;
extern "C" unsigned pc_gx_vtx_gen(void) { return g_pcGxVtxGen; }
extern "C" unsigned pc_gx_mat_gen(void) { return g_pcGxMatGen; }
#define VTXGEN() (++g_pcGxVtxGen)
#define MATGEN() (++g_pcGxMatGen)
#endif
volatile PPCWGPipe GXWGFifo;

#if defined(__vita__)
// Vertex-state images (vita_gxq.h).
void pc_gx_vtx_image_init(GxqVtxImage& img) {
    memset(&img, 0, sizeof(img));
}
void pc_gx_vtx_image_add_fmt_list(GxqVtxImage& img, int fmt, const _GXVtxAttrFmtList* list) {
    ++img.gens;
    img.fmtIdx = uint32_t(fmt);
    for (; list->attr != GX_VA_NULL; ++list) {
        // pc_gfx_set_vtx_attr_fmt's range test.
        if (unsigned(fmt) < unsigned(GX_MAX_VTXFMT) && unsigned(list->attr) < unsigned(GX_VA_MAX_ATTR)) {
            img.fmt[list->attr] = gxq_vtx_fmt_pack(uint32_t(list->count), uint32_t(list->type), list->frac);
            img.fmtMask |= 1u << list->attr;
        }
    }
}
void pc_gx_vtx_image_add_desc_list(GxqVtxImage& img, const _GXVtxDescList* list) {
    ++img.gens;
    img.descMask = (1u << GX_VA_MAX_ATTR) - 1u;   // GXSetVtxDescv clears first
    for (int i = 0; i < GX_VA_MAX_ATTR; ++i) img.desc[i] = uint32_t(GX_NONE);
    for (; list->attr != GX_VA_NULL; ++list) {
        if (unsigned(list->attr) < unsigned(GX_VA_MAX_ATTR)) img.desc[list->attr] = uint32_t(list->type);
    }
}
// The original list-setter bodies (vita_gxq_ctx.h gxr_set_vtx_*_calls).
void pc_gx_set_vtx_attr_fmtv_calls(int fmt, const _GXVtxAttrFmtList* list) { gxr_set_vtx_attr_fmtv_calls(gGxqMain, fmt, list); }
void pc_gx_set_vtx_descv_calls(const _GXVtxDescList* attrList) { gxr_set_vtx_descv_calls(gGxqMain, attrList); }
#endif

extern "C" {

/* ── GX Init ── */
GXFifoObj* GXInit(void* base, u32 size) {
    (void)base; (void)size;
    pc_gfx_init();
    return nullptr;
}
void __GXInitGX()  { }
void __GXPEInit()  { }

/* ── Fifo (from GXFifo.h) ── */
void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size)   { (void)fifo; (void)base; (void)size; }
void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) { (void)fifo; (void)readPtr; (void)writePtr; }
void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWatermark, u32 loWatermark) { (void)fifo; (void)hiWatermark; (void)loWatermark; }
GXFifoObj* GXGetCPUFifo()           { return nullptr; }
GXFifoObj* GXGetGPFifo()            { return nullptr; }
void GXSetCPUFifo(GXFifoObj* fifo)  { (void)fifo; }
void GXSetGPFifo(GXFifoObj* fifo)   { (void)fifo; }

#if defined(__vita__)
/* Recording GX wrappers: gGxqMain's GX-level entries (vita_gxq_ctx.h) */
void GXClearVtxDesc()                                        { gxr_GXClearVtxDesc(gGxqMain); }
void GXSetVtxDesc(GXAttr attr, GXAttrType type)              { gxr_GXSetVtxDesc(gGxqMain, attr, type); }
void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    gxr_GXSetVtxAttrFmt(gGxqMain, fmt, attr, cnt, type, frac);
}
void GXSetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* list)  { gxr_GXSetVtxAttrFmtv(gGxqMain, fmt, list); }
void GXSetVtxDescv(GXVtxDescList* attrList)                  { gxr_GXSetVtxDescv(gGxqMain, attrList); }
void GXSetArray(GXAttr attr, void* basePtr, u8 stride)       { gxr_GXSetArray(gGxqMain, attr, basePtr, stride); }
void GXInvalidateVtxCache()                                  { gxr_GXInvalidateVtxCache(gGxqMain); }
void GXSetNumTexGens(u8 num)                                 { gxr_GXSetNumTexGens(gGxqMain, num); }
void GXSetNumChans(u8 num)                                   { gxr_GXSetNumChans(gGxqMain, num); }
void GXSetNumTevStages(u8 num)                               { gxr_GXSetNumTevStages(gGxqMain, num); }
void GXSetProjection(const Mtx44 mtx, GXProjectionType type) { gxr_GXSetProjection(gGxqMain, mtx, type); }
void GXLoadPosMtxImm(const Mtx mtx, u32 id)                  { gxr_GXLoadPosMtxImm(gGxqMain, mtx, id); }
void GXLoadNrmMtxImm(const Mtx mtx, u32 id)                  { gxr_GXLoadNrmMtxImm(gGxqMain, mtx, id); }
void GXSetCurrentMtx(u32 id)                                 { gxr_GXSetCurrentMtx(gGxqMain, id); }
// No MATGEN.
void GXLoadTexMtxImm(const Mtx mtx, u32 id, GXTexMtxType type) { gxr_GXLoadTexMtxImm(gGxqMain, mtx, id, type); }
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    gxr_GXSetTevOrder(gGxqMain, stage, coord, map, chan);
}
void GXSetTevOp(GXTevStageID stage, GXTevMode mode)          { gxr_GXSetTevOp(gGxqMain, stage, mode); }
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    gxr_GXSetTevColorIn(gGxqMain, stage, a, b, c, d);
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    gxr_GXSetTevAlphaIn(gGxqMain, stage, a, b, c, d);
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    gxr_GXSetTevColorOp(gGxqMain, stage, op, bias, scale, clamp, outReg);
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) {
    gxr_GXSetTevAlphaOp(gGxqMain, stage, op, bias, scale, clamp, outReg);
}
void GXSetTevColor(GXTevRegID reg, GXColor color)            { gxr_GXSetTevColor(gGxqMain, reg, color); }
void GXSetTevColorS10(GXTevRegID reg, GXColorS10 color)      { gxr_GXSetTevColorS10(gGxqMain, reg, color); }
void GXSetTevKColor(GXTevKColorID id, GXColor color)         { gxr_GXSetTevKColor(gGxqMain, id, color); }
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) { gxr_GXSetTevKColorSel(gGxqMain, stage, sel); }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) { gxr_GXSetTevKAlphaSel(gGxqMain, stage, sel); }
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    gxr_GXSetAlphaCompare(gGxqMain, comp0, ref0, op, comp1, ref1);
}
void GXLoadTexObj(GXTexObj* obj, GXTexMapID id)              { gxr_GXLoadTexObj(gGxqMain, obj, id); }
void GXSetTexCoordGen2(GXTexCoordID dstCoord, GXTexGenType func, GXTexGenSrc srcParam, u32 mtx, GXBool normalize, u32 postMtx) {
    gxr_GXSetTexCoordGen2(gGxqMain, dstCoord, func, srcParam, mtx, normalize, postMtx);
}
void GXLoadLightObjImm(GXLightObj* ltObj, GXLightID ltId)    { gxr_GXLoadLightObjImm(gGxqMain, ltObj, ltId); }
void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn,
                   GXAttnFn attnFn) {
    gxr_GXSetChanCtrl(gGxqMain, chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}
void GXSetChanAmbColor(GXChannelID chan, GXColor ambColor)   { gxr_GXSetChanAmbColor(gGxqMain, chan, ambColor); }
void GXSetChanMatColor(GXChannelID chan, GXColor matColor)   { gxr_GXSetChanMatColor(gGxqMain, chan, matColor); }
void GXSetBlendMode(GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    gxr_GXSetBlendMode(gGxqMain, type, srcFactor, dstFactor, op);
}
void GXSetColorUpdate(GXBool updateEnable)                   { gxr_GXSetColorUpdate(gGxqMain, updateEnable); }
void GXSetZMode(GXBool compareEnable, GXCompare func, GXBool updateEnable) { gxr_GXSetZMode(gGxqMain, compareEnable, func, updateEnable); }
void GXSetZCompLoc(GXBool beforeTex)                         { gxr_GXSetZCompLoc(gGxqMain, beforeTex); }
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    gxr_GXSetFog(gGxqMain, type, startz, endz, nearz, farz, color);
}
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)  { gxr_GXBegin(gGxqMain, type, vtxfmt, nverts); }
// No MATGEN.
void GXSetCullMode(GXCullMode mode)                          { gxr_GXSetCullMode(gGxqMain, mode); }
void GXSetLineWidth(u8 width, GXTexOffset texOffset)         { gxr_GXSetLineWidth(gGxqMain, width, texOffset); }
void GXSetPointSize(u8 size, GXTexOffset texOffset)          { gxr_GXSetPointSize(gGxqMain, size, texOffset); }
void GXCopyDisp(void* dest, GXBool clear)                    { gxr_GXCopyDisp(gGxqMain, dest, clear); }
void GXCallDisplayList(void* list, u32 nbytes)               { gxr_GXCallDisplayList(gGxqMain, list, nbytes); }
#else
/* Vertex Format (from GXGeometry.h) */
void GXClearVtxDesc()               { VTXGEN(); pc_gfx_clear_vtx_desc(); }
void GXSetVtxDesc(GXAttr attr, GXAttrType type)             { VTXGEN(); pc_gfx_set_vtx_desc(attr, type); }
void GXSetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) { VTXGEN();
    pc_gfx_set_vtx_attr_fmt(fmt, attr, cnt, type, frac);
}
void GXSetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* list) {
	VTXGEN();
	if (!list) return;
	for (; list->attr != GX_VA_NULL; ++list) {
		pc_gfx_set_vtx_attr_fmt(fmt, list->attr, list->count, list->type, list->frac);
	}
}
void GXSetArray(GXAttr attr, void* basePtr, u8 stride) { VTXGEN(); pc_gfx_set_array(attr, basePtr, stride); }
void GXInvalidateVtxCache()         { pc_gfx_begin_frame(); }
void GXSetNumTexGens(u8 num)        { MATGEN(); (void)num; }
void GXSetNumChans(u8 num)          { MATGEN(); (void)num; }
void GXSetNumTevStages(u8 num)      { MATGEN(); pc_gfx_set_num_tev_stages(num); }

/* ── Transform / Viewport (from GXTransform.h) ── */
void GXSetProjection(const Mtx44 mtx, GXProjectionType type) {
    if (mtx) for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) g_pcGxProj[r][c] = mtx[r][c];
    pc_gfx_set_projection(mtx, type);
}
void GXLoadPosMtxImm(const Mtx mtx, u32 id)  { pc_gfx_load_pos_mtx(mtx, id); }
void GXLoadNrmMtxImm(const Mtx mtx, u32 id)  { pc_gfx_load_nrm_mtx(mtx, id); }
void GXSetCurrentMtx(u32 id)        { if (id < 64) g_pcGxCurMtx = id; pc_gfx_set_current_mtx(id); }
// No MATGEN.
void GXLoadTexMtxImm(const Mtx mtx, u32 id, GXTexMtxType type) { pc_gfx_load_tex_mtx(mtx, id); }

/* ── TEV (from GXTev.h) ── */
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) { MATGEN();
	(void)chan;
	pc_gfx_set_tev_order(stage, coord, map, chan);
}

// Expands the high-level TEV mode convenience call into the exact low-level
// ColorIn/AlphaIn/Op configuration the real hardware SDK programs, matching
// libogc's reference implementation. Stage 0 cannot read CPREV (nothing has
// been written yet), so it uses zeroed/texture-based variants instead.
void GXSetTevOp(GXTevStageID stage, GXTevMode mode) { MATGEN();
	const bool first = (stage == GX_TEVSTAGE0);
	switch (mode) {
	case GX_MODULATE:
		pc_gfx_set_tev_color_in(stage,
		    first ? GX_CC_ZERO : GX_CC_CPREV,
		    GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
		pc_gfx_set_tev_alpha_in(stage,
		    first ? GX_CA_ZERO : GX_CA_APREV,
		    GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
		break;
	case GX_DECAL:
		pc_gfx_set_tev_color_in(stage,
		    GX_CC_TEXA, GX_CC_TEXC, GX_CC_RASC,
		    first ? GX_CC_TEXA : GX_CC_CPREV);
		pc_gfx_set_tev_alpha_in(stage,
		    GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
		    first ? GX_CA_TEXA : GX_CA_APREV);
		break;
	case GX_BLEND:
		pc_gfx_set_tev_color_in(stage,
		    first ? GX_CC_ZERO : GX_CC_CPREV,
		    GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
		pc_gfx_set_tev_alpha_in(stage,
		    first ? GX_CA_ZERO : GX_CA_APREV,
		    GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO);
		break;
	case GX_REPLACE:
		pc_gfx_set_tev_color_in(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
		pc_gfx_set_tev_alpha_in(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
		break;
	case GX_PASSCLR:
	default:
		// "Pass color": hand the rasterized vertex color through untouched.
		pc_gfx_set_tev_color_in(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
		pc_gfx_set_tev_alpha_in(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
		break;
	}
	pc_gfx_set_tev_color_op(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	pc_gfx_set_tev_alpha_op(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) { MATGEN();
    pc_gfx_set_tev_color_in(stage, a, b, c, d);
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) { MATGEN();
    pc_gfx_set_tev_alpha_in(stage, a, b, c, d);
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) { MATGEN();
    pc_gfx_set_tev_color_op(stage, op, bias, scale, clamp, outReg);
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg) { MATGEN();
    pc_gfx_set_tev_alpha_op(stage, op, bias, scale, clamp, outReg);
}
void GXSetTevColor(GXTevRegID reg, GXColor color) { MATGEN(); pc_gfx_set_tev_color(reg, color); }
void GXSetTevColorS10(GXTevRegID reg, GXColorS10 color) { MATGEN(); pc_gfx_set_tev_color_s10(reg, color); }
void GXSetTevKColor(GXTevKColorID id, GXColor color)    { MATGEN(); pc_gfx_set_tev_kcolor(id, color); }
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) { MATGEN(); pc_gfx_set_tev_kcolor_sel(stage, sel); }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) { MATGEN(); pc_gfx_set_tev_kalpha_sel(stage, sel); }
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) { MATGEN();
    pc_gfx_set_alpha_compare(comp0, ref0, op, comp1, ref1);
}
void GXLoadTexObj(GXTexObj* obj, GXTexMapID id)     { MATGEN(); pc_gfx_load_tex_obj(obj, id); }
void GXSetTexCoordGen2(GXTexCoordID dstCoord, GXTexGenType func, GXTexGenSrc srcParam,
                       u32 mtx, GXBool normalize, u32 postMtx) { MATGEN();
    (void)normalize; (void)postMtx;
    pc_gfx_set_tex_coord_gen(dstCoord, func, srcParam, mtx);
}
void GXLoadLightObjImm(GXLightObj* ltObj, GXLightID ltId) { pc_gfx_load_light(ltObj, static_cast<u32>(ltId)); }
void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc,
                   u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn) { MATGEN();
	pc_gfx_set_chan_ctrl(chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}
void GXSetChanAmbColor(GXChannelID chan, GXColor ambColor) { MATGEN(); pc_gfx_set_chan_amb_color(chan, ambColor); }
void GXSetChanMatColor(GXChannelID chan, GXColor matColor) { MATGEN(); pc_gfx_set_chan_mat_color(chan, matColor); }

/* Pixel / Blend / Z (from GXPixel.h) */
void GXSetBlendMode(GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) { MATGEN();
    pc_gfx_set_blend_mode(type, srcFactor, dstFactor, op);
}
void GXSetColorUpdate(GXBool updateEnable)  { MATGEN(); pc_gfx_set_color_update(updateEnable); }
void GXSetZMode(GXBool compareEnable, GXCompare func, GXBool updateEnable) { MATGEN();
    pc_gfx_set_z_mode(compareEnable, func, updateEnable);
}
void GXSetZCompLoc(GXBool beforeTex)        { MATGEN(); (void)beforeTex; }
void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) { MATGEN();
    pc_gfx_set_fog(type != GX_FOG_NONE && pc_settings_get_fog() != 0, startz, endz, nearz, farz,
                   color.r, color.g, color.b);
}

/* Geometry (from GXGeometry.h) */
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) { pc_gfx_begin(type, vtxfmt, nverts); }
// No MATGEN.
void GXSetCullMode(GXCullMode mode)         { pc_gfx_set_cull_mode(mode); }
void GXSetLineWidth(u8 width, GXTexOffset texOffset) { (void)texOffset; pc_gfx_set_line_width(width); }
void GXSetPointSize(u8 size, GXTexOffset texOffset)  { (void)size; (void)texOffset; }

/* Display / Copy (from GXFrameBuffer.h) */
void GXCopyDisp(void* dest, GXBool clear)   { ++g_pcGameFrame; pc_gfx_copy_disp(dest, clear); }
void GXCallDisplayList(void* list, u32 nbytes) { pc_gfx_call_display_list(list, nbytes); }
void GXSetVtxDescv(GXVtxDescList* attrList) {
	VTXGEN();
	if (!attrList) return;
	pc_gfx_clear_vtx_desc();
	for (; attrList->attr != GX_VA_NULL; ++attrList) {
		pc_gfx_set_vtx_desc(attrList->attr, attrList->type);
	}
}
#endif

void GXSetProjectionv(const f32* ptr) { (void)ptr; }
void __GXSetMatrixIndex(GXAttr index) { (void)index; }

void __GXSetViewport()              { }
void GXSetViewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ) {
    pc_gfx_set_viewport(xOrig, yOrig, wd, ht, nearZ, farZ);
}
void GXSetViewportJitter(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ, u32 field) {
    (void)xOrig; (void)yOrig; (void)wd; (void)ht; (void)nearZ; (void)farZ; (void)field;
}
static u32 sGxScissorBox[4] = { 0, 0, 640, 480 };   // the last GXSetScissor, for GXGetScissor
void GXSetScissor(u32 xOrig, u32 yOrig, u32 wd, u32 ht) {
    sGxScissorBox[0] = xOrig; sGxScissorBox[1] = yOrig; sGxScissorBox[2] = wd; sGxScissorBox[3] = ht;
    pc_gfx_set_scissor(xOrig, yOrig, wd, ht);
}
void GXSetScissorBoxOffset(s32 xOrig, s32 yOrig) { (void)xOrig; (void)yOrig; }
void GXGetScissor(u32* l, u32* t, u32* w, u32* h) {
    *l = sGxScissorBox[0]; *t = sGxScissorBox[1]; *w = sGxScissorBox[2]; *h = sGxScissorBox[3];
}
void GXSetClipMode(GXClipMode mode) { (void)mode; }
void GXLoadPosMtxIndx(u16 mtxIndx, u32 id) { (void)mtxIndx; (void)id; }
void GXLoadNrmMtxImm3x3(const Mtx33 mtx, u32 id) { (void)mtx; (void)id; }
void GXLoadNrmMtxIndx3x3(u16 mtxIndx, u32 id) { (void)mtxIndx; (void)id; }
void GXLoadTexMtxIndx(u16 index, u32 id, GXTexMtxType type) { (void)index; (void)id; (void)type; }

/* TEV (from GXTev.h), not recorded */
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) { MATGEN();
    pc_gfx_set_tev_swap_mode(stage, rasSel, texSel);
}
void GXSetTevSwapModeTable(GXTevSwapSel sel, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a) { MATGEN();
    pc_gfx_set_tev_swap_mode_table(sel, r, g, b, a);
}
void GXSetTevDirect(GXTevStageID stage) { MATGEN(); (void)stage; }

/* ── Texture (from GXTexture.h) ── */
void GXInitTexObj(GXTexObj* obj, void* imagePtr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap) {
    pc_gfx_init_tex_obj(obj, imagePtr, width, height, format, wrapS, wrapT, mipmap);
}
void GXInitTexObjCI(GXTexObj* obj, void* imagePtr, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlutName) {
    pc_gfx_init_tex_obj_ci(obj, imagePtr, width, height, format, wrapS, wrapT, mipmap, tlutName);
}
void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter minFilt, GXTexFilter magFilt, f32 minLOD, f32 maxLOD,
                     f32 lodBias, GXBool biasClampEnable, GXBool edgeLODEnable, GXAnisotropy maxAniso) {
    (void)biasClampEnable; (void)edgeLODEnable; (void)maxAniso;
    pc_gfx_init_tex_obj_lod(obj, minFilt, magFilt, minLOD, maxLOD, lodBias);
}
void GXInvalidateTexAll()                           { }
void GXInitTlutObj(GXTlutObj* obj, void* lut, GXTlutFmt fmt, u16 nEntries) {
    pc_gfx_init_tlut_obj(obj, lut, fmt, nEntries);
}
void GXLoadTlut(GXTlutObj* obj, u32 tlutName)      { pc_gfx_load_tlut(obj, tlutName); }
u32  GXGetTexBufferSize(u16 width, u16 height, u32 fmt, GXBool mipMap, u8 maxLOD) {
    (void)fmt; (void)mipMap; (void)maxLOD;
    return width * height * 4;
}

/* ── Lighting (from GXLight.h) ── */
void GXInitLightAttn(GXLightObj* ltObj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    pc_gfx_init_light_attn(ltObj, a0, a1, a2, k0, k1, k2);
}
void GXInitLightSpot(GXLightObj* ltObj, f32 cutoff, GXSpotFn spotFunc) {
    (void)ltObj; (void)cutoff; (void)spotFunc;
}
void GXInitLightColor(GXLightObj* ltObj, GXColor color) { pc_gfx_init_light_color(ltObj, color); }
void GXInitLightPos(GXLightObj* ltObj, f32 x, f32 y, f32 z) { pc_gfx_init_light_pos(ltObj, x, y, z); }
void GXInitLightDir(GXLightObj* ltObj, f32 nx, f32 ny, f32 nz) { pc_gfx_init_light_dir(ltObj, nx, ny, nz); }
void GXInitSpecularDir(GXLightObj* ltObj, f32 nx, f32 ny, f32 nz) { pc_gfx_init_specular_dir(ltObj, nx, ny, nz); }
void GXInitLightAttnA(GXLightObj* ltObj, f32 a0, f32 a1, f32 a2) { pc_gfx_init_light_attn_a(ltObj, a0, a1, a2); }
void GXInitLightAttnK(GXLightObj* ltObj, f32 k0, f32 k1, f32 k2) { pc_gfx_init_light_attn_k(ltObj, k0, k1, k2); }

/* Pixel / Blend / Z (from GXPixel.h), not recorded */
void GXSetAlphaUpdate(GXBool updateEnable)  { MATGEN(); pc_gfx_set_alpha_update(updateEnable); }
void GXSetDither(GXBool dither)             { (void)dither; }
void GXSetDstAlpha(GXBool enable, u8 alpha) { (void)enable; (void)alpha; }
void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) { (void)pix_fmt; (void)z_fmt; }
void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table) {
    (void)enable; (void)center; (void)table;
}

/* ── Geometry (from GXGeometry.h) ── */
void __GXSetDirtyState()                    { }
void __GXSendFlushPrim()                    { }
/* GXEnd() is typically an inline/macro in GXHardware.h - we don't redefine it */
void GXSetCoPlanar(GXBool doEnable)         { (void)doEnable; }
void GXEnableTexOffsets(GXTexCoordID coord, GXBool lineOffset, GXBool pointOffset) {
    (void)coord; (void)lineOffset; (void)pointOffset;
}
void __GXSetGenMode()                       { }

/* ── Display / Copy (from GXFrameBuffer.h) ── */
// Copy parameters for the next GXCopyTex.
static u16 sTexCopySrc[4] = { 0, 0, 640, 480 };
static u16 sTexCopyDstW = 640, sTexCopyDstH = 480;
static GXTexFmt sTexCopyFmt = GX_TF_RGBA8;
static GXBool sTexCopyMip = GX_FALSE;
void GXCopyTex(void* dest, GXBool clear) {
    pc_gfx_copy_tex(dest, clear, sTexCopySrc[0], sTexCopySrc[1], sTexCopySrc[2], sTexCopySrc[3],
                    sTexCopyDstW, sTexCopyDstH, sTexCopyFmt, sTexCopyMip);
}
void GXSetCopyClear(GXColor clearColor, u32 clearZ) { pc_gfx_set_copy_clear(clearColor, clearZ); }
void GXSetCopyFilter(GXBool useAA, const u8 samplePattern[12][2], GXBool doVertFilt, const u8 vFilt[7]) {
    (void)useAA; (void)samplePattern;
    pc_gfx_set_copy_filter(doVertFilt ? 1 : 0, vFilt);
}
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) { sTexCopySrc[0] = left; sTexCopySrc[1] = top; sTexCopySrc[2] = wd; sTexCopySrc[3] = ht; }
void GXSetDispCopyDst(u16 wd, u16 ht) { (void)wd; (void)ht; }
void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) { sTexCopyDstW = wd; sTexCopyDstH = ht; sTexCopyFmt = fmt; sTexCopyMip = mipmap; }
u32  GXSetDispCopyYScale(f32 yscale) { (void)yscale; return 0; }
void GXSetDispCopyGamma(GXGamma gamma) { (void)gamma; }
void GXSetDispCopyFrame2Field(GXCopyMode mode) { (void)mode; }
void GXSetCopyClamp(GXFBClamp clamp) { (void)clamp; }
void GXSetFieldMode(GXBool fieldMode, GXBool halfAspectRatio) { (void)fieldMode; (void)halfAspectRatio; }
void GXSetFieldMask(GXBool oddMask, GXBool evenMask) { (void)oddMask; (void)evenMask; }
u16  GXGetNumXfbLines(u16 efbHeight, f32 yScale) { (void)efbHeight; (void)yScale; return 480; }
f32  GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { (void)efbHeight; (void)xfbHeight; return 1.0f; }
void GXClearBoundingBox()                   { }

/* ── Misc (from GXMisc.h) ── */
void GXFlush()                              { }
void GXPixModeSync()                        { }
void GXTexModeSync()                        { }
void GXSetDrawSync(u16 token)               { (void)token; }
void GXSetDrawDone()                        { }
void GXWaitDrawDone()                       { }
void GXAbortFrame()                         { }
void GXDrawDone()                           { }
void __GXAbort()                            { }
void GXSetMisc(GXMiscToken token, u32 val)  { (void)token; (void)val; }
void GXResetWriteGatherPipe()               { }
u16  GXReadDrawSync()                       { return 0; }
GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) { (void)cb; return nullptr; }
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) { (void)cb; return nullptr; }
struct OSThread* GXSetCurrentGXThread()     { return nullptr; }

/* ── Performance (from GXPerf.h) ── */
void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1) { (void)perf0; (void)perf1; }
void GXReadGPMetric(u32* cnt0, u32* cnt1) { *cnt0 = 0; *cnt1 = 0; }
void GXClearGPMetric() { }

/* ── Indirect Textures / Bump (from GXBump.h) ── */
void GXSetTevIndirect(GXTevStageID stage, GXIndTexStageID indStage, GXIndTexFormat fmt,
                      GXIndTexBiasSel biasSel, GXIndTexMtxID mtxID, GXIndTexWrap wrapS,
                      GXIndTexWrap wrapT, GXBool addPrev, GXBool utcLod, GXIndTexAlphaSel alphaSel) { MATGEN();
    (void)stage; (void)indStage; (void)fmt; (void)biasSel; (void)mtxID;
    (void)wrapS; (void)wrapT; (void)addPrev; (void)utcLod; (void)alphaSel;
}
void GXSetIndTexOrder(GXIndTexStageID indStage, GXTexCoordID texCoord, GXTexMapID texMap) {
    (void)indStage; (void)texCoord; (void)texMap;
}
void GXSetNumIndStages(u8 num) { MATGEN(); (void)num; }
void GXSetIndTexCoordScale(GXIndTexStageID indStage, GXIndTexScale scaleS, GXIndTexScale scaleT) {
    (void)indStage; (void)scaleS; (void)scaleT;
}
void GXSetIndTexMtx(GXIndTexMtxID id, const f32 offset[2][3], s8 scaleExp) {
    (void)id; (void)offset; (void)scaleExp;
}
void GXSetTevIndBumpST(GXTevStageID tevStage, GXIndTexStageID indStage, GXIndTexMtxID mtxID) {
    (void)tevStage; (void)indStage; (void)mtxID;
}
void GXSetTevIndBumpXYZ(GXTevStageID tevStage, GXIndTexStageID indStage, GXIndTexMtxID mtxID) {
    (void)tevStage; (void)indStage; (void)mtxID;
}
void GXSetTevIndRepeat(GXTevStageID tevStage) { (void)tevStage; }

/* ── Attr (from GXAttr.h) ── */
void GXSetNumIndStages_2(u8 num) { (void)num; } // alias if needed

/* ── Poke functions (from GXMisc.h) ── */
void GXPokeAlphaMode(GXCompare func, u8 threshold) { (void)func; (void)threshold; }
void GXPokeAlphaRead(GXAlphaReadMode mode) { (void)mode; }
void GXPokeAlphaUpdate(GXBool doUpdate) { (void)doUpdate; }
void GXPokeBlendMode(GXBlendMode mode, GXBlendFactor srcFactor, GXBlendFactor destFactor, GXLogicOp op) {
    (void)mode; (void)srcFactor; (void)destFactor; (void)op;
}
void GXPokeColorUpdate(GXBool doUpdate) { (void)doUpdate; }
void GXPokeDstAlpha(GXBool doEnable, u8 alpha) { (void)doEnable; (void)alpha; }
void GXPokeDither(GXBool doDither) { (void)doDither; }
void GXPokeZMode(GXBool doCompare, GXCompare func, GXBool doUpdate) {
    (void)doCompare; (void)func; (void)doUpdate;
}
void GXPeekARGB(u16 x, u16 y, u32* color) { (void)x; (void)y; *color = 0; }
void GXPokeARGB(u16 x, u16 y, u32 color) { (void)x; (void)y; (void)color; }
void GXPeekZ(u16 x, u16 y, u32* z) { (void)x; (void)y; *z = 0; }
void GXPokeZ(u16 x, u16 y, u32 z) { (void)x; (void)y; (void)z; }

void GXBeginDisplayList(void* list, u32 size) { (void)list; (void)size; }
u32 GXEndDisplayList(void) { return 0; }

} // extern "C"
