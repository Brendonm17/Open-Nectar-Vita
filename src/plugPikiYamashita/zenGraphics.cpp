#include "Colour.h"
#include "Dolphin/gx.h"
#include "NaviMgr.h"
#include "nlib/Math.h"
#include "zen/Graphics.h"
#include "zen/Math.h"
#if defined(__vita__)
#include <cstring>
#endif

const f32 zen::DrawLifeCircle::TRI_NUM = 128.0f;

#if defined(__vita__)
/* PS Vita: the navi life circle as one triangle fan */
namespace {
struct PcHudRimPoint {
	f32 x, y, z;
};

struct PcHudRim {
	static const int kPoints = 129; // k = 0..TRI_NUM, no wrap

	PcHudRimPoint mRim[kPoints]; // rim point k
	u8 mHave[kPoints];           // point k captured
	int mHaveCount;              // captured count
	u32 mRadiusBits;             // key: mRadius bits
	bool mUsed;                  // key set
	bool mValid;                 // all points captured
	bool mBad;                   // point differed between its triangles
};
} // namespace

enum {
	kPcHudLoopDraw = 0, // the original loop, drawing
	kPcHudLoopFill = 1, // drawing, and filling the table
};

static PcHudRim sPcHudRims[2];
static int sPcHudRimNext = 0;   // the slot a new radius takes

static __attribute__((no_instrument_function)) PcHudRim& pcHudRimFor(f32 radius)
{
	u32 bits;
	memcpy(&bits, &radius, sizeof(bits));
	for (PcHudRim& rim : sPcHudRims) {
		if (rim.mUsed && rim.mRadiusBits == bits) {
			return rim;
		}
	}
	PcHudRim& rim = sPcHudRims[sPcHudRimNext];
	sPcHudRimNext ^= 1;
	memset(&rim, 0, sizeof(rim));
	rim.mUsed       = true;
	rim.mRadiusBits = bits;
	return rim;
}

static __attribute__((noinline, no_instrument_function)) void pcHudRimCapture(PcHudRim& rim, int i, f32 x1, f32 y1, f32 z1, f32 x2,
                                                                               f32 y2, f32 z2)
{
	const PcHudRimPoint points[2] = { { x1, y1, z1 }, { x2, y2, z2 } };
	for (int j = 0; j < 2; j++) {
		const int k = i + j;
		if (k < 0 || k >= PcHudRim::kPoints) {
			continue;
		}
		if (rim.mHave[k]) {
			if (memcmp(&points[j], &rim.mRim[k], sizeof(PcHudRimPoint)) != 0) {
				rim.mBad = true;
			}
		} else {
			rim.mRim[k]  = points[j];
			rim.mHave[k] = 1;
			rim.mHaveCount++;
		}
	}
}

static __attribute__((noinline, no_instrument_function)) void pcHudRimFilled(PcHudRim& rim)
{
	if (!rim.mBad && rim.mHaveCount == PcHudRim::kPoints) {
		rim.mValid = true;
	}
}

static inline __attribute__((no_instrument_function)) void pcHudDrawFan(immut Vector3f& centre, immut PcHudRim& rim, int n,
                                                                        immut Colour& color)
{
	GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, u16(n + 2));
	const u32 gxColor = *reinterpret_cast<immut u32*>(&color);
	GXPosition3f32(centre.x, centre.y, centre.z);
	GXColor1u32(gxColor);
	for (int k = 0; k <= n; k++) {
		GXPosition3f32(rim.mRim[k].x, rim.mRim[k].y, rim.mRim[k].z);
		GXColor1u32(gxColor);
	}
	GXEnd();
}

#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000A8
 */
void zen::zenGraphics::drawOneTri(immut Vector3f* vertices, immut Colour& color)
{
	GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 3);
	u32 gxColor = *reinterpret_cast<immut u32*>(&color);
	for (int i = 0; i < 3; i++, vertices++) {
		GXPosition3f32(vertices->x, vertices->y, vertices->z);
		GXColor1u32(gxColor);
	}
	GXEnd();
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000D0
 */
void zen::zenGraphics::setTevFillPolygon()
{
	GXSetNumTevStages(1);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_NONE);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
}

/**
 * @todo: Documentation
 */
void zen::DrawLifeCircle::drawLifeCircle(f32 healthRatio)
{
	Vector3f vecs[3];
	Colour color;
	zenGraphics::setTevFillPolygon();
	int drawCount = RoundOff(TRI_NUM * healthRatio);

	if (mDisplayedTriCount != drawCount) {
		f32 v;
		if (drawCount < mDisplayedTriCount) {
			v = 0.02f;
		} else {
			v = 0.05f;
		}

		mUpdateTimer += gsys->getFrameTime();
		if (mUpdateTimer > v) {
			int a = RoundOff(gsys->getFrameTime() * 30.0f) * (mDisplayedTriCount < drawCount ? 1 : -1);
			if (Abs(mDisplayedTriCount - drawCount) < Abs(a)) {
				a = Abs(mDisplayedTriCount - drawCount) * (mDisplayedTriCount < drawCount ? 1 : -1);
			}
			mDisplayedTriCount += a;
			mUpdateTimer -= v;
		}
	}

	if (healthRatio < 0.2f) {
		color.set(255, 0, 0, 255); // red for low health
	} else if (healthRatio < 0.5f) {
		color.set(255, 255, 0, 255); // yellow for middling health
	} else {
		color.set(0, 255, 0, 255); // green for good health
	}

	vecs[0].x = mRadius + 0.0f;
	vecs[0].y = mRadius + 0.0f;
	vecs[0].z = 0.0f;

#if defined(__vita__)
	PcHudRim* pcRim = nullptr; // the table the loop fills
	int pcMode      = kPcHudLoopDraw;
	{
		const int n = mDisplayedTriCount;
		if (n <= 0) {
			return; // nothing to draw
		}
		PcHudRim& rim = pcHudRimFor(mRadius);
		if (rim.mValid && n <= PcHudRim::kPoints - 1) {
			pcHudDrawFan(vecs[0], rim, n, color);
			return;
		}
		if (!rim.mValid && !rim.mBad) {
			pcRim  = &rim; // not complete yet
			pcMode = kPcHudLoopFill;
		}
	}
#endif

	for (int i = 0; i < mDisplayedTriCount; i++) {
#if defined(__vita__)
		__asm__ volatile("" : "+r"(pcMode));
#endif
		f32 angle = -HALF_PI - 1.0f / TRI_NUM * f32(i) * TAU;
		vecs[1].x = NMathF::cos(angle) * mRadius + vecs[0].x;
		vecs[1].y = NMathF::sin(angle) * mRadius + vecs[0].y;
		vecs[1].z = 0.0f + vecs[0].z;

		angle     = -HALF_PI - 1.0f / TRI_NUM * f32(i + 1) * TAU;
		vecs[2].x = NMathF::cos(angle) * mRadius + vecs[0].x;
		vecs[2].y = NMathF::sin(angle) * mRadius + vecs[0].y;
		vecs[2].z = 0.0f + vecs[0].z;

#if defined(__vita__)
		if (pcMode != kPcHudLoopDraw) {
			pcHudRimCapture(*pcRim, i, vecs[1].x, vecs[1].y, vecs[1].z, vecs[2].x, vecs[2].y, vecs[2].z);
		}
#endif
		zenGraphics::drawOneTri(vecs, color);
	}
#if defined(__vita__)
	if (pcMode == kPcHudLoopFill) {
		pcHudRimFilled(*pcRim);
	}
#endif
}

/**
 * @todo: Documentation
 */
void zen::DrawNaviLifeCircle::drawLifeCircle()
{
	if (naviMgr) {
		Navi* navi = naviMgr->getNavi(mNaviIndex);
		DrawLifeCircle::drawLifeCircle(navi->mHealth / C_NAVI_PARM(navi, mHealth)); // ratio of current health to parameter health
	} else {
		DrawLifeCircle::drawLifeCircle(1.0f);
	}
}
