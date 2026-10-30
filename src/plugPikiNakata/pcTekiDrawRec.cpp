#if defined(__vita__)
#include "teki.h"
#include "AIPerf.h"
#include "Graphics.h"
#include "MoviePlayer.h"
#include "PikiMgr.h"
#include "Shape.h"
#include "TekiParameters.h"
#include "gameflow.h"
#include "pc_photo_mode.h"
#include "system.h"
#include "gl/vita_drawrec.h"
#include <cstring>
#include <malloc.h>

static_assert(TEKI_TypeCount == kDrTekiTypes, "vita_drawrec.h kDrTekiTypes is TEKI_TypeCount (the price classes)");

namespace {

// Per-type class table
enum {
	kTkClsRec,     // the base path
	kTkClsLight,
	kTkClsNoShape, // no model, draws nothing (Mizigen)
	kTkClsExcl,
};
const unsigned char sTkClass[TEKI_TypeCount] = {
	kTkClsRec,     // Frog
	kTkClsExcl,    // Iwagen
	kTkClsRec,     // Iwagon
	kTkClsRec,     // Chappy
	kTkClsRec,     // Swallow
	kTkClsNoShape, // Mizigen
	kTkClsLight,   // Qurione
	kTkClsRec,     // Palm
	kTkClsRec,     // Collec
	kTkClsRec,     // Kinoko
	kTkClsRec,     // Shell
	kTkClsRec,     // Napkid
	kTkClsRec,     // Hollec
	kTkClsRec,     // Pearl
	kTkClsRec,     // Rocpe
	kTkClsRec,     // Tank
	kTkClsRec,     // Mar
	kTkClsRec,     // Beatle
	kTkClsRec,     // KabekuiA
	kTkClsRec,     // KabekuiB
	kTkClsRec,     // KabekuiC
	kTkClsRec,     // Tamago
	kTkClsRec,     // Dororo
	kTkClsRec,     // HibaA
	kTkClsRec,     // Miurin
	kTkClsRec,     // Otama
	kTkClsExcl,    // Usuba
	kTkClsExcl,    // Yamash3
	kTkClsExcl,    // Yamash4
	kTkClsExcl,    // Yamash5
	kTkClsRec,     // Namazu
	kTkClsRec,     // Chappb
	kTkClsRec,     // Swallob
	kTkClsRec,     // Frow
	kTkClsExcl,    // Nakata1
};
// Lane teki types
bool pcTkInSet(int type)
{
	switch (type) {
	case TEKI_Palm:
	case TEKI_Chappy:
	case TEKI_Swallow:
	case TEKI_Beatle:
	case TEKI_Iwagon:
	case TEKI_KabekuiA:
	case TEKI_KabekuiB:
	case TEKI_Otama:
	case TEKI_Napkid:
		return true;
	default:
		return false;
	}
}

// Records
enum {
	kTkFull     = 1 << 0, // aiCullable()
	kTkAnimCtx  = 1 << 1, // the shape had an animation context
	kTkAnimData = 1 << 2,
	kTkAnimVal  = 1 << 3, // TPI_AnimationType 1
	kTkLit      = 1 << 4, // TPI_LightType 1
	kTkHoisted  = 1 << 5,
};
// One teki's B inputs
struct __attribute__((aligned(32))) PcTekiDrawPacket {
	Shape* mShape;            // its type's shape
	Matrix4f* mPose;          // the shape's mAnimMatrices
	ShapeDynMaterials* mDyn;  // the teki's instance materials
	f32 mAnimVal;             // kTkAnimVal
	f32 mLightVal;            // kTkLit
	u8 mFlags;                // kTk
	u8 mType;                 // TekiTypes
	u16 mPad;
};
static_assert(sizeof(PcTekiDrawPacket) == 32, "a packet is one A9 cache line");

PcTekiDrawPacket* sTkPackets = nullptr;
int sTkCap                   = 0;
int sTkN                     = 0;       // this chain's teki records
bool sTkVisSeeded            = false;
float sTkVisFrac[TEKI_TypeCount];
unsigned sTkFull[TEKI_TypeCount], sTkMtx[TEKI_TypeCount]; // the last chain's records by type

// B of record k
void pcTekiDrawrecB(Graphics& gW, int k)
{
	const PcTekiDrawPacket& p = sTkPackets[k];
	gW.useMatrix(Matrix4f::ident, 0);
	if (!(p.mFlags & kTkFull)) {
		return;
	}
	if (p.mFlags & kTkLit) {
		gW.calcLighting(p.mLightVal);
	}
	f32 val = p.mAnimVal;
	p.mDyn->pcAnimateB((p.mFlags & kTkAnimVal) ? &val : nullptr);
	gW.mPcPose          = p.mPose;
	gW.mPcPoseShape     = p.mShape;
	gW.mPcPoseAnimCtx   = (p.mFlags & kTkAnimCtx) ? 1 : 0;
	gW.mPcPoseAnimData  = (p.mFlags & kTkAnimData) ? 1 : 0;
	gW.mPcCacheHoisted  = (p.mFlags & kTkHoisted) ? 1 : 0;
	p.mShape->drawshape(gW, *gW.mCamera, p.mDyn);
	gW.mPcPose         = nullptr;
	gW.mPcPoseShape    = nullptr;
	gW.mPcCacheHoisted = 0;
	if (p.mFlags & kTkLit) {
		gW.calcLighting(1.0f);
	}
}
const bool sTkDrawrecLane = (pcDrawrecRegisterLane(kDrLaneTeki, PcDrLaneDef { &pcTekiDrawrecB, "teki" }), true);

// Game thread, between chains
bool pcTkEnsureCapacity(int count)
{
	if (count <= sTkCap) {
		return true;
	}
	void* mem = memalign(32, size_t(count) * sizeof(PcTekiDrawPacket));
	if (!mem) {
		return false;
	}
	free(sTkPackets);
	sTkPackets = static_cast<PcTekiDrawPacket*>(mem);
	sTkCap     = count;
	return true;
}

} // namespace

// gameCoreSection.cpp, at T0
void pcTekiDrawrecBegin(Graphics& gfx, TekiMgr* mgr, int hideType)
{
	g_pcDrTekiRec = 0;
	(void)sTkDrawrecLane;
	const unsigned lanes = pcDrawrecLanes();
	if (!(lanes & kDrBitTeki)) {
		return;
	}
	if (!sTkVisSeeded) {
		sTkVisSeeded = true;
		for (int t = 0; t < TEKI_TypeCount; t++) {
			sTkVisFrac[t] = 0.6f;
		}
	}
	for (int t = 0; t < TEKI_TypeCount; t++) {
		const unsigned all = sTkFull[t] + sTkMtx[t];
		if (all) {
			sTkVisFrac[t] += (float(sTkFull[t]) / float(all) - sTkVisFrac[t]) * 0.125f;
		}
		sTkFull[t] = sTkMtx[t] = 0;
	}
	sTkN = 0;
	// The lane's gates
	if (!mgr || hideType != 0 || gameflow.mMoviePlayer->mIsActive || AIPerf::kandoOnly || gsys->mToggleDebugInfo
	    || pc_photo_mode_active()) {
		return;
	}
	const int slots        = mgr->pcSlotCount();
	Creature** const list  = mgr->pcSlots();
	const int* const alive = mgr->pcSlotStatus();
	if (!pcTkEnsureCapacity(slots)) {
		return;
	}
	// The pre-scan
	float expectedUs       = 0.0f;
	unsigned expectedBytes = 0;
	int counted            = 0;
	for (int i = 0; i < slots; i++) {
		if (alive[i] != 0) {
			continue;
		}
		Teki* t = static_cast<Teki*>(list[i]);
		if (!mgr->isVisibleType(t->mTekiType) || t->mDeadState != 0 || !t->getStrategy() || !t->mTekiShape) {
			continue;
		}
		const int type = int(t->mTekiType);
		const int cls  = (type >= 0 && type < TEKI_TypeCount) ? sTkClass[type] : int(kTkClsExcl);
		if (cls == kTkClsExcl || cls == kTkClsNoShape || !pcTkInSet(type)
		    || (cls == kTkClsLight && !(lanes & kDrBitTekiLight))) {
			return;
		}
		if (!t->mTekiShape->mShape || !pcDrShapeInfo(t->mTekiShape->mShape).ok) {
			return;
		}
		const float vis = sTkVisFrac[type];
		expectedUs += pcDrawrecPrice(kDrPcTeki0 + type) * vis + pcDrawrecPrice(kDrPcTekiMtx) * (1.0f - vis);
		expectedBytes += pcDrawrecBytes(kDrPcTeki0 + type);
		++counted;
	}
	if (counted == 0) {
		return;
	}
	if (lanes & kDrBitPiki) {
		const int piki = pikiMgr ? pikiMgr->getSize() : 0;
		expectedUs += pcDrawrecPrice(kDrPcPiki) * float(piki);
		expectedBytes += pcDrawrecBytes(kDrPcPiki) * unsigned(piki);
	}
	if (pcDrawrecForkChain(gfx, expectedUs, expectedBytes)) {
		g_pcDrTekiRec = 1;
	}
}

// BTeki::drawTekiShape, after its A
bool pcTekiDrawrecPublish(BTeki& t, Graphics& gfx)
{
	if (!pcDrawrecCanPublish()) {
		return false;
	}
	const int type = int(t.mTekiType);
	if (sTkN >= sTkCap || type < 0 || type >= TEKI_TypeCount) {
		pcDrawrecBail();
		return false;
	}
	const int n        = sTkN;
	PcTekiDrawPacket& p = sTkPackets[n];
	Shape* s           = t.mTekiShape->mShape;
	p.mShape           = s;
	p.mPose            = nullptr;
	p.mDyn             = nullptr;
	p.mAnimVal         = 0.0f;
	p.mLightVal        = 0.0f;
	p.mFlags           = 0;
	p.mType            = u8(type);
	p.mPad             = 0;
	PcDrEffect e;
	e.meshes     = 0;
	e.curMtx0    = 1;
	e.active     = 1;
	e.lightModel = 0;
	e.activePtr  = &Matrix4f::ident;
	if (!t.aiCullable()) {
		// AI-culled
		++sTkMtx[type];
		++sTkN;
		return pcDrawrecPublishRec(kDrLaneTeki, n, e, kDrPcTekiMtx);
	}
	if (!s->mCurrentAnimation || !s->mCurrentAnimation->mData) {
		pcDrawrecBail();
		return false;
	}
	const int lightType = t.getParameterI(TPI_LightType);
	if (lightType == 1 && (!(pcDrawrecLanes() & kDrBitTekiLight) || sTkClass[type] != kTkClsLight)) {
		pcDrawrecBail();
		return false;
	}
	p.mFlags = kTkFull | kTkAnimCtx | kTkAnimData;
	p.mPose  = s->mAnimMatrices;
	p.mDyn   = &t.mAnimatedMaterials;
	if (t.getParameterI(TPI_AnimationType) == 1) {
		p.mFlags |= kTkAnimVal;
		p.mAnimVal = f32(t._3BC);
	}
	if (lightType == 1) {
		// Qurione
		p.mFlags |= kTkLit;
		p.mLightVal  = t._3C0;
		e.lightModel = 1;
		pcDrawrecAddLightingWrite(1u);
	}
	if (pcDrHoistCache(gfx, s, p.mDyn)) {
		p.mFlags |= kTkHoisted;
	}
	s->mPcSegChain = g_pcDrChain;
	pcDrawrecAddSets(pcDrShapeInfo(s).writes);
	e.meshes = pcDrShapeDrawsMeshes(s, gfx.mMatRenderMask) ? 1 : 0;
	++sTkFull[type];
	++sTkN;
	return pcDrawrecPublishRec(kDrLaneTeki, n, e, kDrPcTeki0 + type);
}
#endif
