#pragma once
// PS Vita: DGXGraphics records through its own recorder context
#if !defined(__vita__)
#error "vita_gxq_bind.h is the Vita recorder binding"
#endif
#include "vita_gxq_ctx.h"

// Every member function that records
#define PC_DGX_CTX()                   \
	GxqCtx& pcR   = *mPcRec;           \
	PcDgxCtx& pcD = *mPcDgx;           \
	(void)pcR;                         \
	(void)pcD

// (a) GX calls, recorded into pcR
#define GXClearVtxDesc()          gxr_GXClearVtxDesc(pcR)
#define GXSetVtxDesc(...)         gxr_GXSetVtxDesc(pcR, __VA_ARGS__)
#define GXSetVtxAttrFmt(...)      gxr_GXSetVtxAttrFmt(pcR, __VA_ARGS__)
#define GXSetVtxDescv(...)        gxr_GXSetVtxDescv(pcR, __VA_ARGS__)
#define GXSetVtxAttrFmtv(...)     gxr_GXSetVtxAttrFmtv(pcR, __VA_ARGS__)
#define GXSetArray(...)           gxr_GXSetArray(pcR, __VA_ARGS__)
#define GXInvalidateVtxCache()    gxr_GXInvalidateVtxCache(pcR)
#define GXLoadPosMtxImm(...)      gxr_GXLoadPosMtxImm(pcR, __VA_ARGS__)
#define GXLoadNrmMtxImm(...)      gxr_GXLoadNrmMtxImm(pcR, __VA_ARGS__)
#define GXLoadTexMtxImm(...)      gxr_GXLoadTexMtxImm(pcR, __VA_ARGS__)
#define GXSetCurrentMtx(...)      gxr_GXSetCurrentMtx(pcR, __VA_ARGS__)
#define GXSetProjection(...)      gxr_GXSetProjection(pcR, __VA_ARGS__)
#define GXSetZMode(...)           gxr_GXSetZMode(pcR, __VA_ARGS__)
#define GXSetBlendMode(...)       gxr_GXSetBlendMode(pcR, __VA_ARGS__)
#define GXSetAlphaCompare(...)    gxr_GXSetAlphaCompare(pcR, __VA_ARGS__)
#define GXSetColorUpdate(...)     gxr_GXSetColorUpdate(pcR, __VA_ARGS__)
#define GXSetZCompLoc(...)        gxr_GXSetZCompLoc(pcR, __VA_ARGS__)
#define GXSetCullMode(...)        gxr_GXSetCullMode(pcR, __VA_ARGS__)
#define GXSetLineWidth(...)       gxr_GXSetLineWidth(pcR, __VA_ARGS__)
#define GXSetPointSize(...)       gxr_GXSetPointSize(pcR, __VA_ARGS__)
#define GXSetFog(...)             gxr_GXSetFog(pcR, __VA_ARGS__)
#define GXSetTevOrder(...)        gxr_GXSetTevOrder(pcR, __VA_ARGS__)
#define GXSetTevOp(...)           gxr_GXSetTevOp(pcR, __VA_ARGS__)
#define GXSetTevColorIn(...)      gxr_GXSetTevColorIn(pcR, __VA_ARGS__)
#define GXSetTevAlphaIn(...)      gxr_GXSetTevAlphaIn(pcR, __VA_ARGS__)
#define GXSetTevColorOp(...)      gxr_GXSetTevColorOp(pcR, __VA_ARGS__)
#define GXSetTevAlphaOp(...)      gxr_GXSetTevAlphaOp(pcR, __VA_ARGS__)
#define GXSetTevColor(...)        gxr_GXSetTevColor(pcR, __VA_ARGS__)
#define GXSetTevColorS10(...)     gxr_GXSetTevColorS10(pcR, __VA_ARGS__)
#define GXSetTevKColor(...)       gxr_GXSetTevKColor(pcR, __VA_ARGS__)
#define GXSetTevKColorSel(...)    gxr_GXSetTevKColorSel(pcR, __VA_ARGS__)
#define GXSetTevKAlphaSel(...)    gxr_GXSetTevKAlphaSel(pcR, __VA_ARGS__)
#define GXSetNumTevStages(...)    gxr_GXSetNumTevStages(pcR, __VA_ARGS__)
#define GXSetNumTexGens(...)      gxr_GXSetNumTexGens(pcR, __VA_ARGS__)
#define GXSetTexCoordGen2(...)    gxr_GXSetTexCoordGen2(pcR, __VA_ARGS__)
#define GXSetNumChans(...)        gxr_GXSetNumChans(pcR, __VA_ARGS__)
#define GXSetChanCtrl(...)        gxr_GXSetChanCtrl(pcR, __VA_ARGS__)
#define GXSetChanMatColor(...)    gxr_GXSetChanMatColor(pcR, __VA_ARGS__)
#define GXSetChanAmbColor(...)    gxr_GXSetChanAmbColor(pcR, __VA_ARGS__)
#define GXLoadLightObjImm(...)    gxr_GXLoadLightObjImm(pcR, __VA_ARGS__)
#define GXLoadTexObj(...)         gxr_GXLoadTexObj(pcR, __VA_ARGS__)
#define GXCallDisplayList(...)    gxr_GXCallDisplayList(pcR, __VA_ARGS__)
#define GXBegin(...)              gxr_GXBegin(pcR, __VA_ARGS__)
#define GXEnd()                   gxr_GXEnd(pcR)
#define GXPosition3f32(...)       gxr_GXPosition3f32(pcR, __VA_ARGS__)
#define GXNormal3f32(...)         gxr_GXNormal3f32(pcR, __VA_ARGS__)
#define GXColor1u32(...)          gxr_GXColor1u32(pcR, __VA_ARGS__)
#define GXTexCoord2f32(...)       gxr_GXTexCoord2f32(pcR, __VA_ARGS__)

// (b) Frame-level
#define GXInit(...)                         (pcMainOnly(pcR), ::GXInit(__VA_ARGS__))
#define GXCopyDisp(...)                     (pcMainOnly(pcR), gxr_GXCopyDisp(pcR, __VA_ARGS__))
#define GXSetCopyClear(...)                 (pcMainOnly(pcR), ::GXSetCopyClear(__VA_ARGS__))
#define GXSetCopyFilter(...)                (pcMainOnly(pcR), ::GXSetCopyFilter(__VA_ARGS__))
#define GXSetDispCopySrc(...)               (pcMainOnly(pcR), ::GXSetDispCopySrc(__VA_ARGS__))
#define GXSetDispCopyDst(...)               (pcMainOnly(pcR), ::GXSetDispCopyDst(__VA_ARGS__))
#define GXSetDispCopyYScale(...)            (pcMainOnly(pcR), ::GXSetDispCopyYScale(__VA_ARGS__))
#define GXSetPixelFmt(...)                  (pcMainOnly(pcR), ::GXSetPixelFmt(__VA_ARGS__))
#define GXSetDither(...)                    (pcMainOnly(pcR), ::GXSetDither(__VA_ARGS__))
#define GXSetCoPlanar(...)                  (pcMainOnly(pcR), ::GXSetCoPlanar(__VA_ARGS__))
#define GXSetScissor(...)                   (pcMainOnly(pcR), ::GXSetScissor(__VA_ARGS__))
#define GXSetScissorBoxOffset(...)          (pcMainOnly(pcR), ::GXSetScissorBoxOffset(__VA_ARGS__))
#define GXSetViewport(...)                  (pcMainOnly(pcR), ::GXSetViewport(__VA_ARGS__))
#define GXDrawDone()                        (pcMainOnly(pcR), ::GXDrawDone())
#define GXFlush()                           (pcMainOnly(pcR), ::GXFlush())
#define GXInvalidateTexAll()                (pcMainOnly(pcR), ::GXInvalidateTexAll())
#define GXBeginDisplayList(...)             (pcMainOnly(pcR), ::GXBeginDisplayList(__VA_ARGS__))
#define GXEndDisplayList()                  (pcMainOnly(pcR), ::GXEndDisplayList())
#define pc_gfx_ui_projection_scale_x()      (pcMainOnly(pcR), ::pc_gfx_ui_projection_scale_x())
#define pc_gfx_ui_full_width_extent()       (pcMainOnly(pcR), ::pc_gfx_ui_full_width_extent())
#define pc_gfx_ui_box(...)                  (pcMainOnly(pcR), ::pc_gfx_ui_box(__VA_ARGS__))
#define pc_gfx_set_scissor_panel()          (pcMainOnly(pcR), ::pc_gfx_set_scissor_panel())
#define pc_gfx_set_scissor_ui_box()         (pcMainOnly(pcR), ::pc_gfx_set_scissor_ui_box())
#define pc_gfx_get_section_clip_43()        (pcMainOnly(pcR), ::pc_gfx_get_section_clip_43())
#define pc_gfx_get_current_aspect_ratio()   (pcMainOnly(pcR), ::pc_gfx_get_current_aspect_ratio())

#pragma GCC poison g_pcGxVtxGen g_pcGxMatGen g_pcGxStateGen g_pcGxStateGenEmit g_pcGxGenOnEmit
#pragma GCC poison g_pcGxProj g_pcGxCurMtx g_pcMeshPass g_pcGameFrame
#pragma GCC poison g_pcGxqSite g_pcMatBlocksSkipped g_pcMatBlocksSent
#pragma GCC poison g_pcMeshCulled g_pcMeshTested g_pcMeshNoBounds g_pcMatSkipped g_pcMatMissWhy
#pragma GCC poison g_pcPrecullTests g_pcPrecullOnscreen g_pcPrecullLookups
#pragma GCC poison g_pcUnionTried g_pcUnionTests g_pcUnionHits g_pcUnionMisses g_pcUnionAvoided g_pcUnionExtraLookups
#pragma GCC poison g_pcUnionChecked g_pcUnionMismatches
#pragma GCC poison pc_gx_vtx_gen pc_gx_mat_gen
#pragma GCC poison pc_gfx_vtx_image pc_gfx_vtx_image_verify pc_gx_set_vtx_attr_fmtv_calls pc_gx_set_vtx_descv_calls
#pragma GCC poison pc_gfx_load_posnrm_group
#pragma GCC poison vita_gxq_expect_drop_begin vita_gxq_expect_drop_end vita_gxq_sample_extra_skip vita_gxq_note_register_list
#pragma GCC poison pc_gfx_set_blend_mode pc_gfx_set_alpha_compare pc_gfx_set_z_mode pc_gfx_set_tev_kcolor
#pragma GCC poison pc_gfx_set_tev_color_in pc_gfx_set_tev_alpha_in pc_gfx_set_tev_color_op pc_gfx_set_tev_alpha_op
#pragma GCC poison pc_gfx_set_num_tev_stages pc_gfx_set_tev_order pc_gfx_set_tev_kcolor_sel pc_gfx_set_tev_kalpha_sel
#pragma GCC poison pc_gfx_set_tev_color_s10 pc_gfx_set_chan_ctrl
#pragma GCC poison pc_gfx_set_cull_mode pc_gfx_set_color_update pc_gfx_set_alpha_update pc_gfx_set_tex_coord_gen
#pragma GCC poison pc_gfx_load_pos_mtx pc_gfx_load_nrm_mtx pc_gfx_load_tex_mtx pc_gfx_set_current_mtx pc_gfx_set_projection
#pragma GCC poison pc_gfx_set_chan_mat_color pc_gfx_set_chan_amb_color pc_gfx_load_light pc_gfx_set_tev_op pc_gfx_set_tev_color
#pragma GCC poison pc_gfx_set_tev_swap_mode pc_gfx_set_tev_swap_mode_table pc_gfx_load_tex_obj pc_gfx_set_fog
#pragma GCC poison pc_gfx_clear_vtx_desc pc_gfx_set_vtx_desc pc_gfx_set_vtx_attr_fmt pc_gfx_set_array
#pragma GCC poison pc_gfx_begin pc_gfx_end pc_gfx_position pc_gfx_color pc_gfx_texcoord pc_gfx_call_display_list
#pragma GCC poison pc_gfx_begin_frame
// The moved file statics
#pragma GCC poison GColors oldCull oldTevGroup sendMtxIndx sendTxIndx sendColIndx sendNbtIndx sendTxUVIndx
#pragma GCC poison sPcPe sPcKCol sPcTev sPcChan sPcTevReg sPcUnionHints sPcUnionTick
