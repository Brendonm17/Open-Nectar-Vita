#ifndef PC_GFX_H
#include <cstddef>
#define PC_GFX_H

#include "Dolphin/gx.h"
#include "pc_opengl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialization
void pc_gfx_init(void);
void pc_gfx_begin_frame(void);
void pc_gfx_present(void);
void pc_gfx_perf_scope_begin(const char* name);
void pc_gfx_perf_scope_end(void);

// Internal 3D render resolution scale (multiplier on the native 640x480).
// begin_frame picks the change up and resizes the internal framebuffer.
void pc_gfx_set_render_scale(float scale);
float pc_gfx_get_render_scale(void);

// Aspect ratio support.
// Mode: 0=auto (detect from window), 1=4:3, 2=16:10, 3=16:9, 4=21:9
void pc_gfx_set_aspect_ratio_mode(int mode);
int pc_gfx_get_aspect_ratio_mode(void);
float pc_gfx_get_current_aspect_ratio(void);

// Interface pass X scale for a centred 4:3 box; 1.0 on 4:3.
float pc_gfx_ui_projection_scale_x(void);
// Full-width plate overhang in GX pixels; 0 on 4:3.
float pc_gfx_ui_full_width_extent(void);
// Interface 4:3 box in full-panel GX scissor space (x0, width).
void pc_gfx_ui_box(int* x0, int* w);
// Whole-screen 4:3 menu section.
void pc_gfx_set_section_clip_43(int on);
int pc_gfx_get_section_clip_43(void);
// Full-screen plate scissor helpers.
void pc_gfx_set_scissor_panel(void);
void pc_gfx_set_scissor_ui_box(void);
// Linear (untiled) movie plane uploads.
void pc_gfx_set_movie_planes_linear(int on);
// GXSetLineWidth (GX units are 1/6 pixel).
void pc_gfx_set_line_width(u8 width);
// GXCopyTex: EFB rectangle into the texture at dest.
void pc_gfx_copy_tex(void* dest, GXBool clear, u16 srcX, u16 srcY, u16 srcW, u16 srcH, u16 dstW, u16 dstH, GXTexFmt fmt, GXBool mipmap);
// Offscreen w x h EFB in GX pixels (on = 1), or the display (on = 0).
void pc_gfx_efb_target(int on, int w, int h);
// Blended full-screen pass from copy targets (kind: 0 blur, 1 grey-out).
void pc_gfx_efb_composite(int kind, const void* dest0, const void* dest1, float p0, float p1);
// Drops caches keyed by addresses in the freed [start, end) ranges. Takes ownership of ranges.
void pc_gfx_level_flush(const void* ranges, int count);
// GXSetCopyFilter vertical filter.
void pc_gfx_set_copy_filter(int vertFilter, const u8 vfilter[7]);
// Immediate-mode primitive byte stream.
void pc_gfx_prim_stream(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts, const unsigned char* stream, unsigned bytes, int flags);


// Linear fog (GXSetFog).
void pc_gfx_set_fog(bool enable, float startZ, float endZ, float nearZ, float farZ,
                    unsigned char r, unsigned char g, unsigned char b);


// Viewport / Scissor / Matrices
void pc_gfx_set_projection(const Mtx44 mtx, GXProjectionType type);
void pc_gfx_set_viewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ);
void pc_gfx_set_scissor(u32 xOrig, u32 yOrig, u32 wd, u32 ht);
void pc_gfx_load_pos_mtx(const Mtx mtx, u32 id);
void pc_gfx_set_current_mtx(u32 id);
void pc_gfx_load_nrm_mtx(const Mtx mtx, u32 id);
void pc_gfx_load_tex_mtx(const Mtx mtx, u32 id);
void pc_gfx_set_tex_coord_gen(GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx);

// State Settings
void pc_gfx_set_z_mode(GXBool compareEnable, GXCompare func, GXBool updateEnable);
void pc_gfx_set_blend_mode(GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op);
void pc_gfx_set_cull_mode(GXCullMode mode);
void pc_gfx_set_color_update(GXBool updateEnable);
void pc_gfx_set_alpha_update(GXBool updateEnable);
void pc_gfx_set_alpha_compare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1);
void pc_gfx_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn);
void pc_gfx_set_chan_mat_color(GXChannelID chan, GXColor color);
void pc_gfx_set_chan_amb_color(GXChannelID chan, GXColor color);
void pc_gfx_init_light_pos(void* ltObj, f32 x, f32 y, f32 z);
void pc_gfx_init_light_dir(void* ltObj, f32 x, f32 y, f32 z);
void pc_gfx_init_light_color(void* ltObj, GXColor color);
void pc_gfx_init_light_attn(void* ltObj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2);
void pc_gfx_init_light_attn_a(void* ltObj, f32 a0, f32 a1, f32 a2);
void pc_gfx_init_light_attn_k(void* ltObj, f32 k0, f32 k1, f32 k2);
void pc_gfx_init_specular_dir(void* ltObj, f32 x, f32 y, f32 z);
void pc_gfx_load_light(void* ltObj, u32 lightMask);
void pc_gfx_set_tev_order(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan);
void pc_gfx_set_tev_op(GXTevStageID stage, GXTevMode mode);
void pc_gfx_set_num_tev_stages(u8 num);
void pc_gfx_set_tev_color_in(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d);
void pc_gfx_set_tev_alpha_in(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d);
void pc_gfx_set_tev_color_op(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg);
void pc_gfx_set_tev_alpha_op(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID outReg);
void pc_gfx_set_tev_color(GXTevRegID reg, GXColor color);
void pc_gfx_set_tev_color_s10(GXTevRegID reg, GXColorS10 color);
void pc_gfx_set_tev_kcolor(GXTevKColorID id, GXColor color);
void pc_gfx_set_tev_kcolor_sel(GXTevStageID stage, GXTevKColorSel sel);
void pc_gfx_set_tev_kalpha_sel(GXTevStageID stage, GXTevKAlphaSel sel);
void pc_gfx_set_tev_swap_mode(GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel);
void pc_gfx_set_tev_swap_mode_table(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha);

// Texture Management
void pc_gfx_init_tex_obj(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap);
void pc_gfx_init_tex_obj_lod(GXTexObj* obj, GXTexFilter minFilt, GXTexFilter magFilt, f32 minLOD, f32 maxLOD, f32 lodBias);
void pc_gfx_init_tex_obj_ci(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXCITexFmt format,
                            GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlutName);
void pc_gfx_init_tlut_obj(GXTlutObj* obj, void* lut, GXTlutFmt format, u16 numEntries);
void pc_gfx_load_tlut(GXTlutObj* obj, u32 tlutName);
void pc_gfx_load_tex_obj(GXTexObj* obj, GXTexMapID id);

// Vertex Descriptor & Stream Setup
void pc_gfx_clear_vtx_desc(void);
void pc_gfx_set_vtx_desc(GXAttr attr, GXAttrType type);
void pc_gfx_set_vtx_attr_fmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
void pc_gfx_set_array(GXAttr attr, void* basePtr, u8 stride);

// Drawing & FIFO Stream Parser
void pc_gfx_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void pc_gfx_push_u8(u8 val);
void pc_gfx_push_u16(u16 val);
void pc_gfx_push_u32(u32 val);
void pc_gfx_push_s8(s8 val);
void pc_gfx_push_s16(s16 val);
void pc_gfx_push_s32(s32 val);
void pc_gfx_push_f32(f32 val);
void pc_gfx_position(f32 x, f32 y, f32 z);
void pc_gfx_color(u8 r, u8 g, u8 b, u8 a);
void pc_gfx_texcoord(f32 u, f32 v);
void pc_gfx_end(void);
void pc_gfx_call_display_list(const void* list, u32 nbytes);

// Frame presentation
void pc_gfx_copy_disp(void* dest, GXBool clear);
void pc_gfx_set_copy_clear(GXColor color, u32 clearZ);

// Render packet capture (for immutable replay without re-entering game code)
void pc_gfx_begin_capture(uint64_t serial);
void pc_gfx_end_capture();
void pc_gfx_enable_capture(bool enabled);
bool pc_gfx_is_capture_enabled();
void pc_gfx_replay_display_list(const void* list, u32 nbytes);
bool pc_gfx_replay_captured_frame(void);

#ifdef __cplusplus
// TEV shader specialisation: one generated program per material configuration
// instead of a single interpreting ubershader. Enabled by default; the
// ubershader remains available for A/B comparison and as an automatic fallback.
void pc_gfx_set_shader_specialisation(bool enabled);
bool pc_gfx_get_shader_specialisation(void);
size_t pc_gfx_get_specialised_program_count(void);

void pc_gfx_flush_batch(void);

class PcRenderPacketStore;
PcRenderPacketStore& pc_gfx_get_packet_store();
}
#endif

// GX state generations: vertex descriptor and material state.
#ifdef __cplusplus
extern "C" {
#endif
unsigned pc_gx_vtx_gen(void);
unsigned pc_gx_mat_gen(void);
// Bounds of a decoded display list, or 0 if unknown.
int pc_gx_dl_bounds(const void* list, u32 nbytes, const void* posBase, float* bmin, float* bmax, int* usesMtxIdx);
#ifdef __cplusplus
}
extern unsigned g_pcMeshCulled;   // meshes the pre-record cull skipped, per frame
extern unsigned g_pcMatSkipped;   // setMaterial calls the material cache answered, per frame
extern unsigned g_pcMeshTested;   // meshes the pre-record cull reached, per frame
extern unsigned g_pcMeshNoBounds; // of those, meshes whose display lists had no published bounds
extern unsigned g_pcMatMissWhy[8]; // material-cache misses by key component
extern float g_pcGxProj[4][4];     // the projection exactly as GXSetProjection received it
extern unsigned g_pcGxCurMtx;      // the current position matrix id (GXSetCurrentMtx)
extern unsigned g_pcGameFrame;     // frames, counted at GXCopyDisp
extern unsigned g_pcMeshPass;      // shape draws, counted at DGXGraphics::initMesh
// Per-category material state generations.
enum { PCGX_GEN_PE, PCGX_GEN_KCOL, PCGX_GEN_TEV, PCGX_GEN_CHAN, PCGX_GEN_TEVREG, PCGX_GEN_COUNT };
extern unsigned g_pcGxStateGen[PCGX_GEN_COUNT];
extern unsigned g_pcMatBlocksSkipped;   // material blocks not re-sent, per frame
extern unsigned g_pcMatBlocksSent;      // material blocks sent, per frame
extern "C" {
#endif
#ifdef __cplusplus
}
#endif

#endif // PC_GFX_H
