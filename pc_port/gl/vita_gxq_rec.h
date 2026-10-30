#pragma once
// PS Vita: inline half of the lean record core
#if !NECTAR_GXQ_LEAN
#error "vita_gxq_rec.h is the lean record core (NECTAR_GXQ_LEAN)"
#endif
#include <string.h>

#define GXQ_HOT static inline __attribute__((always_inline, no_instrument_function))
#ifndef NECTAR_GXQ_SLOW_COLD
#define NECTAR_GXQ_SLOW_COLD 0
#endif
#if NECTAR_GXQ_SLOW_COLD
#define GXQ_SLOW __attribute__((noinline, cold, no_instrument_function))
#else
#define GXQ_SLOW __attribute__((noinline, no_instrument_function))
#endif
#define GXQ_LIKELY(x) __builtin_expect(!!(x), 1)

// Material-state generation categories
enum { kGxqGenPe = 0, kGxqGenKCol = 1, kGxqGenTev = 2, kGxqGenChan = 3, kGxqGenTevReg = 4 };

// Compares
template <int N> GXQ_HOT bool gxq_hit1(const GxqCtx& R, const GxqSh<N>& s, uint32_t a0) {
    return ((s.ep ^ R.d.ep) | (uint32_t(s.v[0]) ^ a0)) == 0u;
}
template <int N> GXQ_HOT bool gxq_hit2(const GxqCtx& R, const GxqSh<N>& s, uint32_t a0, uint32_t a1) {
    return ((s.ep ^ R.d.ep) | (uint32_t(s.v[0]) ^ a0) | (uint32_t(s.v[1]) ^ a1)) == 0u;
}
template <int N> GXQ_HOT bool gxq_hit3(const GxqCtx& R, const GxqSh<N>& s, uint32_t a0, uint32_t a1, uint32_t a2) {
    return ((s.ep ^ R.d.ep) | (uint32_t(s.v[0]) ^ a0) | (uint32_t(s.v[1]) ^ a1) | (uint32_t(s.v[2]) ^ a2)) == 0u;
}
template <int N> GXQ_HOT bool gxq_hit4(const GxqCtx& R, const GxqSh<N>& s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return ((s.ep ^ R.d.ep) | (uint32_t(s.v[0]) ^ a0) | (uint32_t(s.v[1]) ^ a1) | (uint32_t(s.v[2]) ^ a2)
            | (uint32_t(s.v[3]) ^ a3)) == 0u;
}
template <int N>
GXQ_HOT bool gxq_hit5(const GxqCtx& R, const GxqSh<N>& s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    return ((s.ep ^ R.d.ep) | (uint32_t(s.v[0]) ^ a0) | (uint32_t(s.v[1]) ^ a1) | (uint32_t(s.v[2]) ^ a2)
            | (uint32_t(s.v[3]) ^ a3) | (uint32_t(s.v[4]) ^ a4)) == 0u;
}
// 3x4 matrix compare
GXQ_HOT bool gxq_hit12(const GxqCtx& R, const GxqSh<12>& s, const void* m) {
    const uint32_t* a = static_cast<const uint32_t*>(m);
    const uint32_t* v = reinterpret_cast<const uint32_t*>(s.v);
    if (((s.ep ^ R.d.ep) | (v[0] ^ a[0])) != 0u) return false;
    return ((v[1] ^ a[1]) | (v[2] ^ a[2]) | (v[3] ^ a[3]) | (v[4] ^ a[4]) | (v[5] ^ a[5]) | (v[6] ^ a[6])
            | (v[7] ^ a[7]) | (v[8] ^ a[8]) | (v[9] ^ a[9]) | (v[10] ^ a[10]) | (v[11] ^ a[11])) == 0u;
}
GXQ_HOT uint32_t gxq_color_word(GXColor c) {
    uint32_t w;
    memcpy(&w, &c, 4);
    return w;
}

// The slow halves (vita_gxq.cpp)
void gxq_s_load_pos_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) GXQ_SLOW;
void gxq_s_load_nrm_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) GXQ_SLOW;
void gxq_s_load_tex_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) GXQ_SLOW;
void gxq_s_set_current_mtx(GxqCtx& R, u32 id) GXQ_SLOW;
void gxq_s_set_tex_coord_gen(GxqCtx& R, GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx) GXQ_SLOW;
void gxq_s_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) GXQ_SLOW;
void gxq_s_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) GXQ_SLOW;
void gxq_s_set_cull_mode(GxqCtx& R, GXCullMode mode) GXQ_SLOW;
void gxq_s_set_line_width(GxqCtx& R, u8 width) GXQ_SLOW;
void gxq_s_set_color_update(GxqCtx& R, GXBool updateEnable) GXQ_SLOW;
void gxq_s_set_alpha_update(GxqCtx& R, GXBool updateEnable) GXQ_SLOW;
void gxq_s_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) GXQ_SLOW;
void gxq_s_set_chan_mat_color(GxqCtx& R, GXChannelID chan, GXColor color) GXQ_SLOW;
void gxq_s_set_chan_amb_color(GxqCtx& R, GXChannelID chan, GXColor color) GXQ_SLOW;
void gxq_s_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) GXQ_SLOW;
void gxq_s_set_num_tev_stages(GxqCtx& R, u8 num) GXQ_SLOW;
void gxq_s_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) GXQ_SLOW;
void gxq_s_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) GXQ_SLOW;
void gxq_s_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                            GXTevRegID outReg) GXQ_SLOW;
void gxq_s_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                            GXTevRegID outReg) GXQ_SLOW;
void gxq_s_set_tev_color(GxqCtx& R, GXTevRegID reg, GXColor color) GXQ_SLOW;
void gxq_s_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) GXQ_SLOW;
void gxq_s_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) GXQ_SLOW;
void gxq_s_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) GXQ_SLOW;
void gxq_s_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) GXQ_SLOW;
void gxq_s_set_tev_swap_mode(GxqCtx& R, GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) GXQ_SLOW;
void gxq_s_set_tev_swap_mode_table(GxqCtx& R, GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                                   GXTevColorChan alpha) GXQ_SLOW;
// stride: bits 0-7 GX stride, bit 8 volatile
enum : uint32_t { kGxqArrayVolatile = 0x100u };
void gxq_s_set_array(GxqCtx& R, GXAttr attr, void* basePtr, uint32_t stride) GXQ_SLOW;
void gxq_s_set_vtx_desc(GxqCtx& R, GXAttr attr, GXAttrType type) GXQ_SLOW;
void gxq_s_set_vtx_attr_fmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) GXQ_SLOW;
void gxq_s_load_tex_obj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) GXQ_SLOW;
void gxq_s_position(GxqCtx& R, f32 x, f32 y, f32 z) GXQ_SLOW;
void gxq_s_color(GxqCtx& R, u8 r, u8 g, u8 b, u8 a) GXQ_SLOW;
void gxq_s_texcoord(GxqCtx& R, f32 u, f32 v) GXQ_SLOW;

// Fast halves: recorder level
GXQ_HOT void gxq_r_load_pos_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    if (GXQ_LIKELY(mtx && id < 64u && gxq_hit12(R, R.d.posMtx[id], mtx))) return;
    gxq_s_load_pos_mtx(R, mtx, id);
}
GXQ_HOT void gxq_r_load_nrm_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    if (GXQ_LIKELY(mtx && id < 64u && gxq_hit12(R, R.d.nrmMtx[id], mtx))) return;
    gxq_s_load_nrm_mtx(R, mtx, id);
}
GXQ_HOT void gxq_r_load_tex_mtx(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    if (GXQ_LIKELY(mtx && id < 64u && gxq_hit12(R, R.d.texMtx[id], mtx))) return;
    gxq_s_load_tex_mtx(R, mtx, id);
}
GXQ_HOT void gxq_r_set_current_mtx(GxqCtx& R, u32 id) {
    if (GXQ_LIKELY(gxq_hit1(R, R.d.curMtx, id))) return;
    gxq_s_set_current_mtx(R, id);
}
GXQ_HOT void gxq_r_set_tex_coord_gen(GxqCtx& R, GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx) {
    if (GXQ_LIKELY(unsigned(coord) < 8u && gxq_hit3(R, R.d.texGen[coord], uint32_t(type), uint32_t(src), matrixIdx))) {
        return;
    }
    gxq_s_set_tex_coord_gen(R, coord, type, src, matrixIdx);
}
GXQ_HOT void gxq_r_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    ++R.d.stateGen[kGxqGenPe];
    if (GXQ_LIKELY(gxq_hit3(R, R.d.zMode, uint32_t(compareEnable), uint32_t(func), uint32_t(updateEnable)))) return;
    gxq_s_set_z_mode(R, compareEnable, func, updateEnable);
}
GXQ_HOT void gxq_r_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    ++R.d.stateGen[kGxqGenPe];
    if (GXQ_LIKELY(gxq_hit4(R, R.d.blend, uint32_t(type), uint32_t(srcFactor), uint32_t(dstFactor), uint32_t(op)))) {
        return;
    }
    gxq_s_set_blend_mode(R, type, srcFactor, dstFactor, op);
}
GXQ_HOT void gxq_r_set_cull_mode(GxqCtx& R, GXCullMode mode) {
    if (GXQ_LIKELY(gxq_hit1(R, R.d.cull, uint32_t(mode)))) return;
    gxq_s_set_cull_mode(R, mode);
}
GXQ_HOT void gxq_r_set_color_update(GxqCtx& R, GXBool updateEnable) {
    if (GXQ_LIKELY(gxq_hit1(R, R.d.colorUpd, uint32_t(updateEnable)))) return;
    gxq_s_set_color_update(R, updateEnable);
}
GXQ_HOT void gxq_r_set_alpha_update(GxqCtx& R, GXBool updateEnable) {
    if (GXQ_LIKELY(gxq_hit1(R, R.d.alphaUpd, uint32_t(updateEnable)))) return;
    gxq_s_set_alpha_update(R, updateEnable);
}
GXQ_HOT void gxq_r_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    ++R.d.stateGen[kGxqGenPe];
    if (GXQ_LIKELY(gxq_hit5(R, R.d.alphaCmp, uint32_t(comp0), uint32_t(ref0), uint32_t(op), uint32_t(comp1), uint32_t(ref1)))) {
        return;
    }
    gxq_s_set_alpha_compare(R, comp0, ref0, op, comp1, ref1);
}
GXQ_HOT void gxq_r_set_chan_mat_color(GxqCtx& R, GXChannelID chan, GXColor color) {
    if (GXQ_LIKELY(unsigned(chan) < 6u && gxq_hit1(R, R.d.chanMat[unsigned(chan) & 1u], gxq_color_word(color)))) return;
    gxq_s_set_chan_mat_color(R, chan, color);
}
GXQ_HOT void gxq_r_set_chan_amb_color(GxqCtx& R, GXChannelID chan, GXColor color) {
    if (GXQ_LIKELY(unsigned(chan) < 6u && gxq_hit1(R, R.d.chanAmb[unsigned(chan) & 1u], gxq_color_word(color)))) return;
    gxq_s_set_chan_amb_color(R, chan, color);
}
GXQ_HOT void gxq_r_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u && gxq_hit3(R, R.d.tevOrder[stage], uint32_t(coord), uint32_t(map), uint32_t(chan)))) {
        return;
    }
    gxq_s_set_tev_order(R, stage, coord, map, chan);
}
GXQ_HOT void gxq_r_set_num_tev_stages(GxqCtx& R, u8 num) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(gxq_hit1(R, R.d.numStages, uint32_t(num)))) return;
    gxq_s_set_num_tev_stages(R, num);
}
GXQ_HOT void gxq_r_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u && gxq_hit4(R, R.d.tevCIn[stage], uint32_t(a), uint32_t(b), uint32_t(c), uint32_t(d)))) {
        return;
    }
    gxq_s_set_tev_color_in(R, stage, a, b, c, d);
}
GXQ_HOT void gxq_r_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u && gxq_hit4(R, R.d.tevAIn[stage], uint32_t(a), uint32_t(b), uint32_t(c), uint32_t(d)))) {
        return;
    }
    gxq_s_set_tev_alpha_in(R, stage, a, b, c, d);
}
GXQ_HOT void gxq_r_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                    GXTevRegID outReg) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u
        && gxq_hit5(R, R.d.tevCOp[stage], uint32_t(op), uint32_t(bias), uint32_t(scale), uint32_t(clamp), uint32_t(outReg)))) {
        return;
    }
    gxq_s_set_tev_color_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_HOT void gxq_r_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                    GXTevRegID outReg) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u
        && gxq_hit5(R, R.d.tevAOp[stage], uint32_t(op), uint32_t(bias), uint32_t(scale), uint32_t(clamp), uint32_t(outReg)))) {
        return;
    }
    gxq_s_set_tev_alpha_op(R, stage, op, bias, scale, clamp, outReg);
}
// TEV colour registers
GXQ_HOT void gxq_r_set_tev_color(GxqCtx& R, GXTevRegID reg, GXColor color) {
    ++R.d.stateGen[kGxqGenTevReg];
    if (GXQ_LIKELY(unsigned(reg) < 4u && gxq_hit2(R, R.d.tevReg[reg], 0u, gxq_color_word(color)))) return;
    gxq_s_set_tev_color(R, reg, color);
}
GXQ_HOT void gxq_r_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) {
    ++R.d.stateGen[kGxqGenTevReg];
    uint32_t w[2];
    memcpy(w, &color, 8);
    if (GXQ_LIKELY(unsigned(reg) < 4u && gxq_hit3(R, R.d.tevReg[reg], 1u, w[0], w[1]))) return;
    gxq_s_set_tev_color_s10(R, reg, color);
}
GXQ_HOT void gxq_r_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) {
    ++R.d.stateGen[kGxqGenKCol];
    if (GXQ_LIKELY(unsigned(id) < 4u && gxq_hit1(R, R.d.kColor[id], gxq_color_word(color)))) return;
    gxq_s_set_tev_kcolor(R, id, color);
}
GXQ_HOT void gxq_r_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u && gxq_hit1(R, R.d.kcSel[stage], uint32_t(sel)))) return;
    gxq_s_set_tev_kcolor_sel(R, stage, sel);
}
GXQ_HOT void gxq_r_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) {
    ++R.d.stateGen[kGxqGenTev];
    if (GXQ_LIKELY(unsigned(stage) < 16u && gxq_hit1(R, R.d.kaSel[stage], uint32_t(sel)))) return;
    gxq_s_set_tev_kalpha_sel(R, stage, sel);
}
GXQ_HOT void gxq_r_set_tev_swap_mode(GxqCtx& R, GXTevStageID stage, GXTevSwapSel rasSel, GXTevSwapSel texSel) {
    if (GXQ_LIKELY(unsigned(stage) < 16u && gxq_hit2(R, R.d.swap[stage], uint32_t(rasSel), uint32_t(texSel)))) return;
    gxq_s_set_tev_swap_mode(R, stage, rasSel, texSel);
}
GXQ_HOT void gxq_r_set_tev_swap_mode_table(GxqCtx& R, GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                                           GXTevColorChan alpha) {
    const unsigned t = unsigned(int(table) - int(GX_TEV_SWAP0));
    if (GXQ_LIKELY(t < 4u && gxq_hit4(R, R.d.swapTable[t], uint32_t(red), uint32_t(green), uint32_t(blue), uint32_t(alpha)))) {
        return;
    }
    gxq_s_set_tev_swap_mode_table(R, table, red, green, blue, alpha);
}
GXQ_HOT void gxq_r_set_array(GxqCtx& R, GXAttr attr, void* basePtr, uint32_t stride) {
    if (GXQ_LIKELY(unsigned(attr) < 26u && R.d.shArrayBase[attr] == uintptr_t(basePtr) && R.d.shArrayStride[attr] == uint32_t(stride))) {
        return;
    }
    gxq_s_set_array(R, attr, basePtr, stride);
}
GXQ_HOT void gxq_r_set_vtx_desc(GxqCtx& R, GXAttr attr, GXAttrType type) {
    if (__builtin_expect(R.recording != 0, 1)) {
        GxqDecision& d = R.d;
        if (unsigned(attr) < 26u && d.pendDesc[attr] != uint32_t(type)) {
            if (gxq_refuse(R)) return;   // the off-lock guard
            d.pendDesc[attr] = uint32_t(type);
            d.pendDirty      = true;
            d.pendDescMask |= 1u << attr;
        }
        return;
    }
    gxq_s_set_vtx_desc(R, attr, type);
}
GXQ_HOT void gxq_r_set_vtx_attr_fmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    if (__builtin_expect(R.recording != 0, 1)) {
        GxqDecision& d = R.d;
        if (unsigned(fmt) < unsigned(GX_MAX_VTXFMT) && unsigned(attr) < 26u) {
            const uint32_t packed = (uint32_t(cnt) << 16) | (uint32_t(type) << 8) | frac;
            if (d.pendFmt[fmt][attr] != packed) {
                if (gxq_refuse(R)) return;
                d.pendFmt[fmt][attr] = packed;
                d.pendDirty          = true;
                d.pendFmtMask[fmt] |= 1u << attr;
            }
        }
        return;
    }
    gxq_s_set_vtx_attr_fmt(R, fmt, attr, cnt, type, frac);
}
GXQ_HOT void gxq_r_load_tex_obj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) { gxq_s_load_tex_obj(R, obj, id); }
enum : uint8_t { kGxqPsPosition = 1, kGxqPsColor = 2, kGxqPsTexcoord = 3 };   // vita_gxq.cpp PS_* (asserted there)
GXQ_HOT void gxq_r_position(GxqCtx& R, f32 x, f32 y, f32 z) {
    if (GXQ_LIKELY(R.primOpen && R.primUsed + 13u <= R.primCap)) {
        uint8_t* d = R.primBuf + R.primUsed;
        R.primUsed += 13u;
        d[0]              = kGxqPsPosition;
        const float v[3]  = { x, y, z };
        memcpy(d + 1, v, 12);
        R.primHasPos = true;
        return;
    }
    gxq_s_position(R, x, y, z);
}
GXQ_HOT void gxq_r_color(GxqCtx& R, u8 r, u8 g, u8 b, u8 a) {
    if (GXQ_LIKELY(R.primOpen && R.primUsed + 5u <= R.primCap)) {
        uint8_t* d = R.primBuf + R.primUsed;
        R.primUsed += 5u;
        d[0] = kGxqPsColor; d[1] = r; d[2] = g; d[3] = b; d[4] = a;
        return;
    }
    gxq_s_color(R, r, g, b, a);
}
GXQ_HOT void gxq_r_texcoord(GxqCtx& R, f32 u, f32 v) {
    if (GXQ_LIKELY(R.primOpen && R.primUsed + 9u <= R.primCap)) {
        uint8_t* d = R.primBuf + R.primUsed;
        R.primUsed += 9u;
        d[0]             = kGxqPsTexcoord;
        const float t[2] = { u, v };
        memcpy(d + 1, t, 8);
        return;
    }
    gxq_s_texcoord(R, u, v);
}

// Fast halves: GX level
GXQ_HOT void gxr_GXSetVtxDesc(GxqCtx& R, GXAttr attr, GXAttrType type) {
    ++R.d.vtxGen;
    gxq_r_set_vtx_desc(R, attr, type);
}
GXQ_HOT void gxr_GXSetVtxAttrFmt(GxqCtx& R, GXVtxFmt fmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    ++R.d.vtxGen;
    gxq_r_set_vtx_attr_fmt(R, fmt, attr, cnt, type, frac);
}
GXQ_HOT void gxr_GXSetArray(GxqCtx& R, GXAttr attr, const void* basePtr, u8 stride) {
    ++R.d.vtxGen;
    gxq_r_set_array(R, attr, const_cast<void*>(basePtr), stride);
}
GXQ_HOT void gxr_GXSetArrayVolatile(GxqCtx& R, GXAttr attr, const void* basePtr, u8 stride, bool contentsChange) {
    ++R.d.vtxGen;
    gxq_r_set_array(R, attr, const_cast<void*>(basePtr), uint32_t(stride) | (contentsChange ? uint32_t(kGxqArrayVolatile) : 0u));
}
GXQ_HOT void gxr_GXLoadPosMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    gxq_r_load_pos_mtx(R, mtx, id);
}
GXQ_HOT void gxr_GXLoadNrmMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id) {
    gxq_r_load_nrm_mtx(R, mtx, id);
}
GXQ_HOT void gxr_GXLoadTexMtxImm(GxqCtx& R, const f32 mtx[3][4], u32 id, GXTexMtxType type) {
    gxq_r_load_tex_mtx(R, mtx, id);
}
GXQ_HOT void gxr_GXSetCurrentMtx(GxqCtx& R, u32 id) {
    if (id < 64u) R.d.gxCurMtx = id;
    gxq_r_set_current_mtx(R, id);
}
GXQ_HOT void gxr_GXSetZMode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    ++R.d.matGen;
    gxq_r_set_z_mode(R, compareEnable, func, updateEnable);
}
GXQ_HOT void gxr_GXSetBlendMode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    ++R.d.matGen;
    gxq_r_set_blend_mode(R, type, srcFactor, dstFactor, op);
}
GXQ_HOT void gxr_GXSetAlphaCompare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    ++R.d.matGen;
    gxq_r_set_alpha_compare(R, comp0, ref0, op, comp1, ref1);
}
GXQ_HOT void gxr_GXSetColorUpdate(GxqCtx& R, GXBool updateEnable) {
    ++R.d.matGen;
    gxq_r_set_color_update(R, updateEnable);
}
GXQ_HOT void gxr_GXSetZCompLoc(GxqCtx& R, GXBool beforeTex) {
    ++R.d.matGen;
    (void)beforeTex;
}
GXQ_HOT void gxr_GXSetCullMode(GxqCtx& R, GXCullMode mode) {
    gxq_r_set_cull_mode(R, mode);
}
GXQ_HOT void gxr_GXSetLineWidth(GxqCtx& R, u8 width, GXTexOffset texOffset) { (void)texOffset; gxq_s_set_line_width(R, width); }
GXQ_HOT void gxr_GXSetPointSize(GxqCtx& R, u8 size, GXTexOffset texOffset) { (void)R; (void)size; (void)texOffset; }
GXQ_HOT void gxr_GXSetTevOrder(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    ++R.d.matGen;
    gxq_r_set_tev_order(R, stage, coord, map, chan);
}
GXQ_HOT void gxr_GXSetTevColorIn(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    ++R.d.matGen;
    gxq_r_set_tev_color_in(R, stage, a, b, c, d);
}
GXQ_HOT void gxr_GXSetTevAlphaIn(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    ++R.d.matGen;
    gxq_r_set_tev_alpha_in(R, stage, a, b, c, d);
}
GXQ_HOT void gxr_GXSetTevColorOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID outReg) {
    ++R.d.matGen;
    gxq_r_set_tev_color_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_HOT void gxr_GXSetTevAlphaOp(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID outReg) {
    ++R.d.matGen;
    gxq_r_set_tev_alpha_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_HOT void gxr_GXSetTevColor(GxqCtx& R, GXTevRegID reg, GXColor color) {
    ++R.d.matGen;
    gxq_r_set_tev_color(R, reg, color);
}
GXQ_HOT void gxr_GXSetTevColorS10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) {
    ++R.d.matGen;
    gxq_r_set_tev_color_s10(R, reg, color);
}
GXQ_HOT void gxr_GXSetTevKColor(GxqCtx& R, GXTevKColorID id, GXColor color) {
    ++R.d.matGen;
    gxq_r_set_tev_kcolor(R, id, color);
}
GXQ_HOT void gxr_GXSetTevKColorSel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) {
    ++R.d.matGen;
    gxq_r_set_tev_kcolor_sel(R, stage, sel);
}
GXQ_HOT void gxr_GXSetTevKAlphaSel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) {
    ++R.d.matGen;
    gxq_r_set_tev_kalpha_sel(R, stage, sel);
}
GXQ_HOT void gxr_GXSetNumTevStages(GxqCtx& R, u8 num) {
    ++R.d.matGen;
    gxq_r_set_num_tev_stages(R, num);
}
GXQ_HOT void gxr_GXSetNumTexGens(GxqCtx& R, u8 num) {
    ++R.d.matGen;
    (void)num;
}
GXQ_HOT void gxr_GXSetTexCoordGen2(GxqCtx& R, GXTexCoordID dstCoord, GXTexGenType func, GXTexGenSrc srcParam, u32 mtx, GXBool normalize,
                                   u32 postMtx) {
    ++R.d.matGen;
    gxq_r_set_tex_coord_gen(R, dstCoord, func, srcParam, mtx);
    (void)normalize; (void)postMtx;
}
GXQ_HOT void gxr_GXSetNumChans(GxqCtx& R, u8 num) {
    ++R.d.matGen;
    (void)num;
}
GXQ_HOT void gxr_GXSetChanMatColor(GxqCtx& R, GXChannelID chan, GXColor matColor) {
    ++R.d.matGen;
    gxq_r_set_chan_mat_color(R, chan, matColor);
}
GXQ_HOT void gxr_GXSetChanAmbColor(GxqCtx& R, GXChannelID chan, GXColor ambColor) {
    ++R.d.matGen;
    gxq_r_set_chan_amb_color(R, chan, ambColor);
}
GXQ_HOT void gxr_GXLoadTexObj(GxqCtx& R, GXTexObj* obj, GXTexMapID id) {
    ++R.d.matGen;
    gxq_r_load_tex_obj(R, obj, id);
}
GXQ_HOT void gxr_GXPosition3f32(GxqCtx& R, f32 x, f32 y, f32 z) {
    gxq_r_position(R, x, y, z);
}
GXQ_HOT void gxr_GXNormal3f32(GxqCtx& R, f32 x, f32 y, f32 z) { (void)R; (void)x; (void)y; (void)z; }
// GXColor1u32 (GXFifo.h)
GXQ_HOT void gxr_GXColor1u32(GxqCtx& R, u32 c) {
    gxq_r_color(R, u8(c), u8(c >> 8), u8(c >> 16), u8(c >> 24));
}
GXQ_HOT void gxr_GXTexCoord2f32(GxqCtx& R, f32 u, f32 v) {
    gxq_r_texcoord(R, u, v);
}

// Recorder level
GXQ_HOT void gxr_pc_gfx_set_blend_mode(GxqCtx& R, GXBlendMode type, GXBlendFactor srcFactor, GXBlendFactor dstFactor, GXLogicOp op) {
    gxq_r_set_blend_mode(R, type, srcFactor, dstFactor, op);
}
GXQ_HOT void gxr_pc_gfx_set_alpha_compare(GxqCtx& R, GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    gxq_r_set_alpha_compare(R, comp0, ref0, op, comp1, ref1);
}
GXQ_HOT void gxr_pc_gfx_set_z_mode(GxqCtx& R, GXBool compareEnable, GXCompare func, GXBool updateEnable) {
    gxq_r_set_z_mode(R, compareEnable, func, updateEnable);
}
GXQ_HOT void gxr_pc_gfx_set_tev_kcolor(GxqCtx& R, GXTevKColorID id, GXColor color) {
    gxq_r_set_tev_kcolor(R, id, color);
}
GXQ_HOT void gxr_pc_gfx_set_tev_color_in(GxqCtx& R, GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    gxq_r_set_tev_color_in(R, stage, a, b, c, d);
}
GXQ_HOT void gxr_pc_gfx_set_tev_alpha_in(GxqCtx& R, GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    gxq_r_set_tev_alpha_in(R, stage, a, b, c, d);
}
GXQ_HOT void gxr_pc_gfx_set_tev_color_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                         GXTevRegID outReg) {
    gxq_r_set_tev_color_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_HOT void gxr_pc_gfx_set_tev_alpha_op(GxqCtx& R, GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                         GXTevRegID outReg) {
    gxq_r_set_tev_alpha_op(R, stage, op, bias, scale, clamp, outReg);
}
GXQ_HOT void gxr_pc_gfx_set_num_tev_stages(GxqCtx& R, u8 num) {
    gxq_r_set_num_tev_stages(R, num);
}
GXQ_HOT void gxr_pc_gfx_set_tev_order(GxqCtx& R, GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    gxq_r_set_tev_order(R, stage, coord, map, chan);
}
GXQ_HOT void gxr_pc_gfx_set_tev_kcolor_sel(GxqCtx& R, GXTevStageID stage, GXTevKColorSel sel) {
    gxq_r_set_tev_kcolor_sel(R, stage, sel);
}
GXQ_HOT void gxr_pc_gfx_set_tev_kalpha_sel(GxqCtx& R, GXTevStageID stage, GXTevKAlphaSel sel) {
    gxq_r_set_tev_kalpha_sel(R, stage, sel);
}
GXQ_HOT void gxr_pc_gfx_set_tev_color_s10(GxqCtx& R, GXTevRegID reg, GXColorS10 color) {
    gxq_r_set_tev_color_s10(R, reg, color);
}
