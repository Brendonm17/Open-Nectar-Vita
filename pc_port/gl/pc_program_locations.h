#pragma once
// GL program uniform locations and the uniform section writers.
#include <stdint.h>
#include <string.h>
#include "Dolphin/gx.h"
#include "pc_opengl.h"

// Uniform locations of one program.
struct ProgramLocations {
	GLint projMtx = -1;
	GLint posMtx = -1;
	GLint posPal = -1;   // uPosPal[30] palette rows
	GLint nrmPal = -1;   // uNrmPal[30]
	GLint fogParams = -1;   // (start, end, near, far)
	GLint fogColour = -1;
	GLint materialColor = -1;
	GLint matSrc = -1;   // chan0 rgb, chan0 alpha, chan1 rgb material selects
	GLint useMaterialRgb = -1;
	GLint useMaterialAlpha = -1;
	GLint alphaComp0 = -1;
	GLint alphaComp1 = -1;
	GLint alphaOp = -1;
	GLint alphaRef0 = -1;
	GLint alphaRef1 = -1;
	GLint numStages = -1;
	GLint fastPath = -1;
	GLint tevPrev = -1;
	GLint tevReg0 = -1;
	GLint tevReg1 = -1;
	GLint tevReg2 = -1;
	GLint konst[4] = {-1, -1, -1, -1};
	GLint tevKonst[GX_MAXTEVSTAGE] = {};
	GLint tevCSel[GX_MAXTEVSTAGE] = {};
	GLint tevASel[GX_MAXTEVSTAGE] = {};
	GLint tevCOps[GX_MAXTEVSTAGE] = {};
	GLint tevAOps[GX_MAXTEVSTAGE] = {};
	GLint tevTexInfo[GX_MAXTEVSTAGE] = {};
	GLint tevSwapTable[4] = {};
	GLint tevSwapSel[GX_MAXTEVSTAGE] = {};
	GLint tex[8] = {};
	GLint nrmMtx = -1;
	GLint numLights = -1;
	GLint lightPos[4] = {};
	GLint lightColor[4] = {};
	GLint lightK[4] = {};
	GLint ambColor = -1;
	GLint chan0En = -1;
	GLint chan1En = -1;
	GLint chan0AttnFn = -1;
	GLint chan1AttnFn = -1;
	GLint numLights1 = -1;
	GLint lightPos1[4] = {};
	GLint lightColor1[4] = {};
	GLint lightK1[4] = {};
	GLint ambColor1 = -1;
	GLint materialColor1 = -1;
	GLint useMaterialRgb1 = -1;
	GLint specHalf1 = -1;   // specular half-vector for channel 1
	GLint specAttn1 = -1;   // specular a[] coefficients
	GLint tevChan[GX_MAXTEVSTAGE] = {};
	GLint tcMode[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	GLint tcMtx[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
};
static_assert(sizeof(ProgramLocations) % sizeof(GLint) == 0, "ProgramLocations is an array of GLint");

// GX state read by the sections

// Texture coordinate generation
struct TexCoordGen {
    bool active = false;
    int type = 1;      // GX_TG_MTX3X4
    int src = 4;       // GX_TG_TEX0
    u32 mtxIdx = 0;
};

// Lighting state
struct GfxLight {
    float pos[3];    // world position (or half-vector for specular light 7)
    float dir[3];    // direction / half-vector
    float a[3];      // specular attenuation a0,a1,a2 (N.H quadratic curve)
    float color[4];  // RGBA 0-1
    float k[3];      // distance attenuation k0,k1,k2
    bool active;
};

// TEV swap mode state
struct TevSwapMode {
    GXTevColorChan red;
    GXTevColorChan green;
    GXTevColorChan blue;
    GXTevColorChan alpha;
};

struct GfxChannel {
    bool enabled = false;
    GXColorSrc matSrc = GX_SRC_VTX;      // Hardware GXInit default: vertex colors
    GXColorSrc ambSrc = GX_SRC_REG;
    u32 lightMask = 0;
    GXDiffuseFn diffFn = GX_DF_NONE;
    GXAttnFn attnFn = GX_AF_NONE;
    bool alphaEnabled = false;
    GXColorSrc alphaMatSrc = GX_SRC_VTX;
    GXColorSrc alphaAmbSrc = GX_SRC_REG;
    u32 alphaLightMask = 0;
    GXDiffuseFn alphaDiffFn = GX_DF_NONE;
    GXAttnFn alphaAttnFn = GX_AF_NONE;
    float matColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // white
    float ambColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// Per-stage TEV state, GX reset defaults.
struct TevStageState {
	GXTevColorArg colorIn[4] = { GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC };
	GXTevAlphaArg alphaIn[4] = { GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA };
	GXTevOp colorOp = GX_TEV_ADD;
	GXTevBias colorBias = GX_TB_ZERO;
	GXTevScale colorScale = GX_CS_SCALE_1;
	GXBool colorClamp = GX_TRUE;
	GXTevRegID colorOutReg = GX_TEVPREV;
	GXTevOp alphaOp = GX_TEV_ADD;
	GXTevBias alphaBias = GX_TB_ZERO;
	GXTevScale alphaScale = GX_CS_SCALE_1;
	GXBool alphaClamp = GX_TRUE;
	GXTevRegID alphaOutReg = GX_TEVPREV;
	GXTexMapID texMap = GX_TEXMAP_NULL;
	GXTexCoordID texCoord = GX_TEXCOORD_NULL;
	bool textureEnabled = false;
	int rasChannel = 0; // lit channel 0/1, -1 for COLOR_NULL
};

// Konst color for a stage's selectors.
static inline float vgx_konst_fraction(int selector) {
    static const float fractions[8] = { 1.0f, 7.0f / 8.0f, 3.0f / 4.0f, 5.0f / 8.0f,
                                        1.0f / 2.0f, 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 8.0f };
    return selector >= 0 && selector < 8 ? fractions[selector] : 1.0f;
}
static inline void vgx_resolve_konst(GXTevKColorSel kc, GXTevKAlphaSel ka, const float (*konstColors)[4], float out[4]) {
    const int colorSel = static_cast<int>(kc);
    if (colorSel < 8) {
        out[0] = out[1] = out[2] = vgx_konst_fraction(colorSel);
    } else if (colorSel >= 12 && colorSel < 16) {
        const float* color = konstColors[colorSel - 12];
        out[0] = color[0]; out[1] = color[1]; out[2] = color[2];
    } else if (colorSel >= 16 && colorSel < 32) {
        const int component = (colorSel - 16) >> 2;
        const int colorIndex = (colorSel - 16) & 3;
        const float value = konstColors[colorIndex][component];
        out[0] = out[1] = out[2] = value;
    } else {
        out[0] = out[1] = out[2] = 1.0f;
    }

    const int alphaSel = static_cast<int>(ka);
    if (alphaSel < 8) {
        out[3] = vgx_konst_fraction(alphaSel);
    } else if (alphaSel >= 16 && alphaSel < 32) {
        const int component = (alphaSel - 16) >> 2;
        const int colorIndex = (alphaSel - 16) & 3;
        out[3] = konstColors[colorIndex][component];
    } else {
        out[3] = 1.0f;
    }
}

// TEV section scalar inputs.
struct VgxAlphaFog {
    int32_t comp0, comp1, op;
    float   ref0, ref1;
    int32_t fogImg;
    float   fog[4];        // Vita: vertex fog (K0, K1, 0, 0); desktop: start, end, near, far
    float   fogColour[3];
    float   fogA;          // uFogColour.a
};
struct VgxTevHdr { int32_t numStages; int32_t fastPath; };

// Writer inputs.
struct VgxSrc {
    const float*          proj;       // [16]
    int32_t               numStages;
    int32_t               fastPath;
    const TevSwapMode*    swapModes;  // [4]
    const TevStageState*  st;         // [GX_MAXTEVSTAGE]
    const GXTevSwapSel*   rasSwap;    // [GX_MAXTEVSTAGE]
    const GXTevSwapSel*   texSwap;    // [GX_MAXTEVSTAGE]
    const GXTevKColorSel* kc;         // [GX_MAXTEVSTAGE]
    const GXTevKAlphaSel* ka;         // [GX_MAXTEVSTAGE]
    const float         (*reg)[4];    // [4]: PREV, REG0, REG1, REG2
    const float         (*konst)[4];  // [4]
    const VgxAlphaFog*    af;
    const GfxChannel*     ch;         // [2]
    const GfxLight*       light;      // [8]
    const TexCoordGen*    tg;         // [8]
    const float         (*texMtx)[16];// [64]
};

// Writers. A sink has put(loc, kind, words, data), one call per glUniform*.
enum : uint8_t { kVgxPut1i = 1, kVgxPut1f, kVgxPut2i, kVgxPut4i, kVgxPut4f, kVgxPutMat4, kVgxPut4fv };
static inline bool vgx_put_is_int(int kind) { return kind == kVgxPut1i || kind == kVgxPut2i || kind == kVgxPut4i; }
template <class S> static inline void vgx_1i(S& s, GLint l, int v) { const GLint a[1] = { v }; s.put(l, kVgxPut1i, 1u, a); }
template <class S> static inline void vgx_1f(S& s, GLint l, float v) { const float a[1] = { v }; s.put(l, kVgxPut1f, 1u, a); }
template <class S> static inline void vgx_2i(S& s, GLint l, int x, int y) { const GLint a[2] = { x, y }; s.put(l, kVgxPut2i, 2u, a); }
template <class S> static inline void vgx_4i(S& s, GLint l, int x, int y, int z, int w) { const GLint a[4] = { x, y, z, w }; s.put(l, kVgxPut4i, 4u, a); }
template <class S> static inline void vgx_4f(S& s, GLint l, float x, float y, float z, float w) { const float a[4] = { x, y, z, w }; s.put(l, kVgxPut4f, 4u, a); }

// Uniform cache slot count (pc_gfx.cpp PC_UNIFORM_CACHE_SIZE).
static constexpr int kVgxUniformCacheSlots = 4096;

// Uniform sections, in write order.
enum : uint8_t { kVgxSecProj = 1, kVgxSecSlot0 = 2, kVgxSecTev = 4, kVgxSecChan = 8, kVgxSecTexGen = 16 };

static const float kVgxIdentity16[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
static const float kVgxIdentityNrm9[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };

template <class S>
static inline void vgx_write_proj(const VgxSrc& g, const ProgramLocations& loc, S& s) {
    s.put(loc.projMtx, kVgxPutMat4, 16u, g.proj);
}

// Palette slot 0: one position/normal matrix pair as 3x4 rows.
template <class S>
static inline void vgx_write_slot0(const float* posColMajor4x4, const float* nrmColMajor3x3, const ProgramLocations& loc, S& s) {
    if (loc.posPal == -1) return;
    float pos[3][4], nrm[3][4];
    for (int r = 0; r < 3; ++r) {
        pos[r][0] = posColMajor4x4[r];     pos[r][1] = posColMajor4x4[4 + r];
        pos[r][2] = posColMajor4x4[8 + r]; pos[r][3] = posColMajor4x4[12 + r];
        nrm[r][0] = nrmColMajor3x3[r];     nrm[r][1] = nrmColMajor3x3[3 + r];
        nrm[r][2] = nrmColMajor3x3[6 + r]; nrm[r][3] = 0.0f;
    }
    s.put(loc.posPal, kVgxPut4fv, 12u, &pos[0][0]);
    s.put(loc.nrmPal, kVgxPut4fv, 12u, &nrm[0][0]);
}

// Cached list palette, slots [0, slots).
static constexpr int kVgxMaxPalSlots = 10;   // GX_PNMTX0..9
template <class S>
static inline void vgx_write_palette(const float (*posMtx)[16], const float (*nrmMtx)[9], u32 current, int slots, bool useCurrentMatrix,
                                     const ProgramLocations& loc, S& s) {
    if (slots < 0) slots = 0;
    if (slots > kVgxMaxPalSlots) slots = kVgxMaxPalSlots;
    float pos[kVgxMaxPalSlots * 3][4];
    float nrm[kVgxMaxPalSlots * 3][4];
    for (int dst = 0; dst < slots; ++dst) {
        u32 gxSlot = useCurrentMatrix ? current : u32(dst) * 3;
        if (gxSlot >= 64) gxSlot = 0;
        const float* m = posMtx[gxSlot];
        const float* nm = nrmMtx[gxSlot];
        for (int r = 0; r < 3; ++r) {
            pos[dst * 3 + r][0] = m[r];  pos[dst * 3 + r][1] = m[4 + r];
            pos[dst * 3 + r][2] = m[8 + r]; pos[dst * 3 + r][3] = m[12 + r];
            nrm[dst * 3 + r][0] = nm[r]; nrm[dst * 3 + r][1] = nm[3 + r];
            nrm[dst * 3 + r][2] = nm[6 + r]; nrm[dst * 3 + r][3] = 0.0f;
        }
    }
    s.put(loc.posPal, kVgxPut4fv, uint32_t(slots) * 12u, &pos[0][0]);
    s.put(loc.nrmPal, kVgxPut4fv, uint32_t(slots) * 12u, &nrm[0][0]);
}

// TEV section.
template <class S>
static inline void vgx_write_tev(const VgxSrc& g, const ProgramLocations& loc, S& s) {
    const VgxAlphaFog& af = *g.af;
    // Fog. Image-backed locations are below -1.
    const bool fogParamsLive = af.fogImg ? loc.fogParams != -1 : loc.fogParams >= 0;
    const bool fogColourLive = af.fogImg ? loc.fogColour != -1 : loc.fogColour >= 0;
    if (fogParamsLive)
        vgx_4f(s, loc.fogParams, af.fog[0], af.fog[1], af.fog[2], af.fog[3]);
    if (fogColourLive)
        vgx_4f(s, loc.fogColour, af.fogColour[0], af.fogColour[1], af.fogColour[2], af.fogA);
    vgx_1i(s, loc.alphaComp0, af.comp0);
    vgx_1i(s, loc.alphaComp1, af.comp1);
    vgx_1i(s, loc.alphaOp, af.op);
    vgx_1f(s, loc.alphaRef0, af.ref0);
    vgx_1f(s, loc.alphaRef1, af.ref1);
    const float (*reg)[4] = g.reg;
    vgx_4f(s, loc.tevPrev, reg[GX_TEVPREV][0], reg[GX_TEVPREV][1], reg[GX_TEVPREV][2], reg[GX_TEVPREV][3]);
    vgx_4f(s, loc.tevReg0, reg[GX_TEVREG0][0], reg[GX_TEVREG0][1], reg[GX_TEVREG0][2], reg[GX_TEVREG0][3]);
    vgx_4f(s, loc.tevReg1, reg[GX_TEVREG1][0], reg[GX_TEVREG1][1], reg[GX_TEVREG1][2], reg[GX_TEVREG1][3]);
    vgx_4f(s, loc.tevReg2, reg[GX_TEVREG2][0], reg[GX_TEVREG2][1], reg[GX_TEVREG2][2], reg[GX_TEVREG2][3]);
    for (int i = 0; i < 4; i++)
        vgx_4f(s, loc.konst[i], g.konst[i][0], g.konst[i][1], g.konst[i][2], g.konst[i][3]);
    vgx_1i(s, loc.numStages, g.numStages);
    vgx_1i(s, loc.fastPath, g.fastPath);
    for (int stage = 0; stage < g.numStages && stage < GX_MAXTEVSTAGE; ++stage) {
        const TevStageState& st = g.st[stage];
        float konst[4];
        vgx_resolve_konst(g.kc[stage], g.ka[stage], g.konst, konst);
        vgx_4f(s, loc.tevKonst[stage], konst[0], konst[1], konst[2], konst[3]);
        vgx_4i(s, loc.tevCSel[stage], st.colorIn[0], st.colorIn[1], st.colorIn[2], st.colorIn[3]);
        vgx_4i(s, loc.tevASel[stage], st.alphaIn[0], st.alphaIn[1], st.alphaIn[2], st.alphaIn[3]);
        const int texIdx = (st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < GX_MAX_TEXMAP)
                               ? static_cast<int>(st.texMap) : -1;
        vgx_4i(s, loc.tevTexInfo[stage], texIdx, st.texCoord, 0, 0);
        const int packedColorOp = static_cast<int>(st.colorOp) | (st.colorClamp ? 0x100 : 0);
        const int packedAlphaOp = static_cast<int>(st.alphaOp) | (st.alphaClamp ? 0x100 : 0);
        vgx_4i(s, loc.tevCOps[stage], packedColorOp, st.colorBias, st.colorScale, st.colorOutReg);
        vgx_4i(s, loc.tevAOps[stage], packedAlphaOp, st.alphaBias, st.alphaScale, st.alphaOutReg);
        if (loc.tevChan[stage] != -1) {
            // Raster channel: -1, 0 or 1.
            vgx_4i(s, loc.tevChan[stage], st.rasChannel, 0, 0, 0);
        }
        // TEV swap mode
        if (loc.tevSwapSel[stage] != -1) {
            const int rasSel = (int)g.rasSwap[stage];
            const int texSel = (int)g.texSwap[stage];
            vgx_2i(s, loc.tevSwapSel[stage], rasSel, texSel);
        }
    }
    // TEV swap mode tables
    for (int t = 0; t < 4; ++t) {
        if (loc.tevSwapTable[t] != -1) {
            vgx_4i(s, loc.tevSwapTable[t],
                   (int)g.swapModes[t].red, (int)g.swapModes[t].green,
                   (int)g.swapModes[t].blue, (int)g.swapModes[t].alpha);
        }
    }
}

// Channel section.
template <class S>
static inline void vgx_write_chan(const VgxSrc& g, const ProgramLocations& loc, S& s) {
    const GfxChannel* ch = g.ch;
    const GfxLight* lights = g.light;
    vgx_4f(s, loc.materialColor, ch[0].matColor[0], ch[0].matColor[1], ch[0].matColor[2], ch[0].matColor[3]);
    vgx_1i(s, loc.useMaterialRgb, ch[0].matSrc == GX_SRC_REG ? 1 : 0);
    vgx_1i(s, loc.useMaterialAlpha, ch[0].alphaMatSrc == GX_SRC_REG ? 1 : 0);
#ifdef __vita__
    // Ubershader only.
    if (loc.matSrc != -1)
        vgx_4f(s, loc.matSrc, ch[0].matSrc == GX_SRC_REG ? 1.0f : 0.0f,
               ch[0].alphaMatSrc == GX_SRC_REG ? 1.0f : 0.0f,
               ch[1].matSrc == GX_SRC_REG ? 1.0f : 0.0f, 0.0f);
#endif
    int numLights = 0;
    float ambR = 1.0f, ambG = 1.0f, ambB = 1.0f, ambA = 1.0f;
    if (ch[0].enabled) {
        ambR = ch[0].ambColor[0];
        ambG = ch[0].ambColor[1];
        ambB = ch[0].ambColor[2];
        ambA = ch[0].ambColor[3];
        const u32 mask = ch[0].lightMask;
        for (int i = 0; i < 8 && numLights < 4; i++) {
            if (mask & (1u << i)) {
                vgx_4f(s, loc.lightPos[numLights], lights[i].pos[0], lights[i].pos[1], lights[i].pos[2], 0.0f);
                vgx_4f(s, loc.lightColor[numLights], lights[i].color[0], lights[i].color[1], lights[i].color[2], lights[i].color[3]);
                const float* k = lights[i].k;
                const bool attnOn = (ch[0].attnFn != GX_AF_NONE);
                vgx_4f(s, loc.lightK[numLights], k[0], k[1], k[2], attnOn ? 1.0f : 0.0f);
                numLights++;
            }
        }
    }
    if (loc.numLights != -1) vgx_1i(s, loc.numLights, numLights);
    if (loc.ambColor != -1) vgx_4f(s, loc.ambColor, ambR, ambG, ambB, ambA);
    if (loc.chan0En != -1) vgx_1i(s, loc.chan0En, ch[0].enabled ? 1 : 0);
    if (loc.chan0AttnFn != -1) vgx_1i(s, loc.chan0AttnFn, (int)ch[0].attnFn);

    // Channel 1 (specular).
    int numLights1 = 0;
    float a1r = 0.0f, a1g = 0.0f, a1b = 0.0f, a1a = 1.0f;
    if (ch[1].enabled) {
        a1r = ch[1].ambColor[0];
        a1g = ch[1].ambColor[1];
        a1b = ch[1].ambColor[2];
        a1a = ch[1].ambColor[3];
        const u32 mask1 = ch[1].lightMask;
        for (int i = 0; i < 8 && numLights1 < 4; i++) {
            if (mask1 & (1u << i)) {
                vgx_4f(s, loc.lightPos1[numLights1], lights[i].pos[0], lights[i].pos[1], lights[i].pos[2], 0.0f);
                vgx_4f(s, loc.lightColor1[numLights1], lights[i].color[0], lights[i].color[1], lights[i].color[2], lights[i].color[3]);
                const float* k1 = lights[i].k;
                const bool attnOn1 = (ch[1].attnFn != GX_AF_NONE);
                vgx_4f(s, loc.lightK1[numLights1], k1[0], k1[1], k1[2], attnOn1 ? 1.0f : 0.0f);
                numLights1++;
            }
        }
    }
    if (loc.numLights1 != -1) vgx_1i(s, loc.numLights1, numLights1);
    if (loc.ambColor1 != -1) vgx_4f(s, loc.ambColor1, a1r, a1g, a1b, a1a);
    if (loc.chan1En != -1) vgx_1i(s, loc.chan1En, ch[1].enabled ? 1 : 0);
    if (loc.chan1AttnFn != -1) vgx_1i(s, loc.chan1AttnFn, (int)ch[1].attnFn);
    // Specular half-vector: light 7 dir.
    if (ch[1].enabled && ch[1].attnFn == GX_AF_SPEC) {
        const u32 mask1 = ch[1].lightMask;
        for (int i = 7; i < 8; i++) {
            if (mask1 & (1u << i) && lights[i].active) {
                if (loc.specHalf1 != -1) {
                    vgx_4f(s, loc.specHalf1, lights[i].dir[0], lights[i].dir[1], lights[i].dir[2], 0.0f);
                }
                if (loc.specAttn1 != -1) {
                    vgx_4f(s, loc.specAttn1, lights[i].a[0], lights[i].a[1], lights[i].a[2], 0.0f);
                }
                // Specular distance coefficients; overwrites lightK1[0].
                if (loc.lightK1[0] != -1) {
                    vgx_4f(s, loc.lightK1[0], lights[i].k[0], lights[i].k[1], lights[i].k[2], 0.0f);
                }
                break;
            }
        }
    }
    if (loc.materialColor1 != -1)
        vgx_4f(s, loc.materialColor1, ch[1].matColor[0], ch[1].matColor[1], ch[1].matColor[2], ch[1].matColor[3]);
    if (loc.useMaterialRgb1 != -1)
        vgx_1i(s, loc.useMaterialRgb1, ch[1].matSrc == GX_SRC_REG ? 1 : 0);
}

// Texgen mode of one slot.
static inline int vgx_texgen_mode(const TexCoordGen& tg) {
    int mode = 0;
    if (tg.active) {
        const GXTexGenSrc src = static_cast<GXTexGenSrc>(tg.src);
        if (src == GX_TG_POS) mode = 1;
        else if (src == GX_TG_NRM) mode = 2;
        else if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7)
            mode = 3 + int(src - GX_TG_TEX0);
        else if (src >= GX_TG_TEXCOORD0 && src <= GX_TG_TEXCOORD6)
            mode = 11 + int(src - GX_TG_TEXCOORD0);
    }
    return mode;
}
template <class S>
static inline void vgx_write_texgen(const VgxSrc& g, const ProgramLocations& loc, S& s) {
    for (int slot = 0; slot < 8; slot++) {
        const int mode = vgx_texgen_mode(g.tg[slot]);
        if (loc.tcMode[slot] != -1) vgx_1i(s, loc.tcMode[slot], mode);
        if (mode != 0 && loc.tcMtx[slot] != -1) {
            u32 mtxIdx = g.tg[slot].mtxIdx;
            if (mtxIdx >= 64) mtxIdx = 0;
            s.put(loc.tcMtx[slot], kVgxPutMat4, 16u, g.texMtx[mtxIdx]);
        }
    }
}

// All sections of one apply.
template <class S>
static inline void vgx_write_sections(const VgxSrc& g, const ProgramLocations& loc, unsigned sections,
                                      const float* slot0Pos, const float* slot0Nrm, S& s) {
    if (sections & kVgxSecProj)   vgx_write_proj(g, loc, s);
    if (sections & kVgxSecSlot0)  vgx_write_slot0(slot0Pos, slot0Nrm, loc, s);
    if (sections & kVgxSecTev)    vgx_write_tev(g, loc, s);
    if (sections & kVgxSecChan)   vgx_write_chan(g, loc, s);
    if (sections & kVgxSecTexGen) vgx_write_texgen(g, loc, s);
}

#ifdef __vita__
// Uniform images
// Image-backed location: encoded slot below -1.
static inline bool     vgx_slot_is_image(GLint loc) { return loc < -1; }
static inline uint32_t vgx_slot_code(GLint loc)     { return uint32_t(-(loc + 2)); }
static inline int      vgx_slot_stage(GLint loc)    { return int((vgx_slot_code(loc) >> 29) & 1u); }
static inline bool     vgx_slot_int(GLint loc)      { return ((vgx_slot_code(loc) >> 28) & 1u) != 0; }
static inline uint32_t vgx_slot_words(GLint loc)    { return (vgx_slot_code(loc) >> 16) & 0xFFFu; }
static inline uint32_t vgx_slot_offset(GLint loc)   { return vgx_slot_code(loc) & 0xFFFFu; }

// Prepares one image write; false when nothing is written.
static inline bool vgx_img_prepare(GLint slot, const void* src, uint32_t words, bool srcInt, uint32_t size,
                                   uint32_t tmp[128], const void** data, uint32_t* off, uint32_t* bytes) {
    const uint32_t maxWords = vgx_slot_words(slot);
    if (words > maxWords) words = maxWords;
    *data = src;
    if (srcInt != vgx_slot_int(slot)) {
        if (words > 128) words = 128;
        if (srcInt) {
            const int32_t* s = static_cast<const int32_t*>(src);
            for (uint32_t i = 0; i < words; ++i) { const float f = float(s[i]); memcpy(&tmp[i], &f, 4); }
        } else {
            const float* s = static_cast<const float*>(src);
            for (uint32_t i = 0; i < words; ++i) { const int32_t v = int32_t(s[i]); memcpy(&tmp[i], &v, 4); }
        }
        *data = tmp;
    }
    *off = vgx_slot_offset(slot);
    *bytes = words * 4u;
    return !(*bytes == 0 || *off + *bytes > size);
}

// Render-thread uniform mirror
struct VgxMirror {
    float          proj[16];
    VgxTevHdr      hdr;
    TevSwapMode    swapModes[4];
    TevStageState  st[GX_MAXTEVSTAGE];
    GXTevSwapSel   rasSwap[GX_MAXTEVSTAGE];
    GXTevSwapSel   texSwap[GX_MAXTEVSTAGE];
    GXTevKColorSel kc[GX_MAXTEVSTAGE];
    GXTevKAlphaSel ka[GX_MAXTEVSTAGE];
    float          reg[4][4];
    float          konst[4][4];
    VgxAlphaFog    af;
    GfxChannel     ch[2];
    GfxLight       light[8];
    TexCoordGen    tg[8];
    float          texMtx[64][16];
    float          pos[64][16];   // position matrices, column-major
    float          nrm[64][9];    // normal matrices
};
// Translator initial state.
static inline void vgx_mirror_reset(VgxMirror& m) {
    m = VgxMirror();
    for (int i = 0; i < 64; ++i) memcpy(m.pos[i], kVgxIdentity16, sizeof(kVgxIdentity16));
}
static inline VgxSrc vgx_src_of(const VgxMirror& m) {
    VgxSrc g;
    g.proj = m.proj; g.numStages = m.hdr.numStages; g.fastPath = m.hdr.fastPath; g.swapModes = m.swapModes;
    g.st = m.st; g.rasSwap = m.rasSwap; g.texSwap = m.texSwap; g.kc = m.kc; g.ka = m.ka;
    g.reg = m.reg; g.konst = m.konst; g.af = &m.af; g.ch = m.ch; g.light = m.light; g.tg = m.tg; g.texMtx = m.texMtx;
    return g;
}

// Mirror blocks.
enum : uint8_t {
    kVgxBlkProj = 1, kVgxBlkTevHdr, kVgxBlkSwapModes, kVgxBlkStages, kVgxBlkRasSwap, kVgxBlkTexSwap, kVgxBlkKSelC, kVgxBlkKSelA,
    kVgxBlkReg, kVgxBlkKonst, kVgxBlkAlphaFog, kVgxBlkChan, kVgxBlkLight, kVgxBlkTexGen, kVgxBlkTexMtx, kVgxBlkPos, kVgxBlkNrm,
};
static_assert(sizeof(TevStageState) % 4 == 0 && sizeof(GfxLight) % 4 == 0 && sizeof(GfxChannel) % 4 == 0
              && sizeof(TexCoordGen) % 4 == 0 && sizeof(VgxAlphaFog) % 4 == 0 && sizeof(TevSwapMode) % 4 == 0
              && sizeof(GXTevSwapSel) == 4 && sizeof(GXTevKColorSel) == 4 && sizeof(GXTevKAlphaSel) == 4,
              "mirror blocks are whole words (-fno-short-enums)");
// Block destination in a mirror, or NULL if invalid.
static inline void* vgx_block_dst(VgxMirror& m, uint8_t id, uint8_t idx, uint32_t bytes) {
    switch (id) {
    case kVgxBlkProj:      return idx == 0 && bytes == sizeof(m.proj) ? (void*)m.proj : nullptr;
    case kVgxBlkTevHdr:    return idx == 0 && bytes == sizeof(m.hdr) ? (void*)&m.hdr : nullptr;
    case kVgxBlkSwapModes: return idx == 0 && bytes == sizeof(m.swapModes) ? (void*)m.swapModes : nullptr;
    case kVgxBlkStages:    return idx >= 1 && idx <= GX_MAXTEVSTAGE && bytes == idx * sizeof(TevStageState) ? (void*)m.st : nullptr;
    case kVgxBlkRasSwap:   return idx >= 1 && idx <= GX_MAXTEVSTAGE && bytes == idx * 4u ? (void*)m.rasSwap : nullptr;
    case kVgxBlkTexSwap:   return idx >= 1 && idx <= GX_MAXTEVSTAGE && bytes == idx * 4u ? (void*)m.texSwap : nullptr;
    case kVgxBlkKSelC:     return idx >= 1 && idx <= GX_MAXTEVSTAGE && bytes == idx * 4u ? (void*)m.kc : nullptr;
    case kVgxBlkKSelA:     return idx >= 1 && idx <= GX_MAXTEVSTAGE && bytes == idx * 4u ? (void*)m.ka : nullptr;
    case kVgxBlkReg:       return idx == 0 && bytes == sizeof(m.reg) ? (void*)m.reg : nullptr;
    case kVgxBlkKonst:     return idx == 0 && bytes == sizeof(m.konst) ? (void*)m.konst : nullptr;
    case kVgxBlkAlphaFog:  return idx == 0 && bytes == sizeof(m.af) ? (void*)&m.af : nullptr;
    case kVgxBlkChan:      return idx == 0 && bytes == sizeof(m.ch) ? (void*)m.ch : nullptr;
    case kVgxBlkLight:     return idx < 8 && bytes == sizeof(GfxLight) ? (void*)&m.light[idx] : nullptr;
    case kVgxBlkTexGen:    return idx == 0 && bytes == sizeof(m.tg) ? (void*)m.tg : nullptr;
    case kVgxBlkTexMtx:    return idx < 64 && bytes == sizeof(m.texMtx[0]) ? (void*)m.texMtx[idx] : nullptr;
    case kVgxBlkPos:       return idx < 64 && bytes == sizeof(m.pos[0]) ? (void*)m.pos[idx] : nullptr;
    case kVgxBlkNrm:       return idx < 64 && bytes == sizeof(m.nrm[0]) ? (void*)m.nrm[idx] : nullptr;
    default:               return nullptr;
    }
}

// Records: RT_GX_APPLY, RT_GX_PALETTE
// apply:   VgxApplyRec, then nBlocks x { u8 id, u8 idx, u16 bytes, data }, then,
//          with kVgxRecInlineLoc, the translator's ProgramLocations for this record.
// palette: VgxPaletteRec, then the blocks the same way.
enum : uint8_t {
    kVgxRecInlineLoc = 4,   // record carries its own location table
};
struct VgxApplyRec   { uint32_t program; uint8_t sections, slot0, slot0Idx, flags; uint16_t nBlocks, pad; };   // slot0: 0 none, 1 identity, 2 matrix slot0Idx
struct VgxPaletteRec { uint32_t program; uint8_t slots, useCurrent, flags, pad; uint32_t current; uint16_t nBlocks, pad2; };
static_assert(sizeof(VgxApplyRec) == 12 && sizeof(VgxPaletteRec) == 16, "record headers are whole words");
#endif // __vita__
