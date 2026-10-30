// Drawrec S2: item, work, pellet and plant passes as chain parts.
#if defined(__vita__)
#include "AIPerf.h"
#include "BuildingItem.h"
#include "Creature.h"
#include "DynColl.h"
#include "GoalItem.h"
#include "Graphics.h"
#include "ItemMgr.h"
#include "MoviePlayer.h"
#include "ObjType.h"
#include "Pellet.h"
#include "PelletView.h"
#include "Piki.h"
#include "PikiHeadItem.h"
#include "PikiMgr.h"
#include "PlantMgr.h"
#include "WeedsItem.h"
#include "WorkObject.h"
#include "gameflow.h"
#include "pc_photo_mode.h"
#include "system.h"
#include "gl/vita_drawrec.h"
#include <cstring>
#include <malloc.h>

int g_pcDrS2Live         = 0;       // S2 live
Creature* g_pcDrRecObj   = nullptr; // object whose refresh prologue records

namespace {

// Class table.
enum { kObjNoGx, kObjRecord, kObjSerial };
struct PcDrObjCls {
	u8 kind;  // kObj*
	u8 lane;  // kDrLane*
	u8 price; // kDrPc*
};
const PcDrObjCls kNoGx   = { kObjNoGx, 0, 0 };
const PcDrObjCls kSerial = { kObjSerial, 0, 0 };

unsigned sS2Lanes = 0;

inline PcDrObjCls pcDrRec(unsigned bit, int lane, int price)
{
	if (!(sS2Lanes & bit)) {
		return kSerial;
	}
	PcDrObjCls c = { kObjRecord, u8(lane), u8(price) };
	return c;
}

// Object class by mObjType.
PcDrObjCls pcDrObjClassOf(Creature* obj)
{
	switch (obj->mObjType) {
	case OBJTYPE_Rope:
	case OBJTYPE_Fulcrum:
	case OBJTYPE_BombGen:
	case OBJTYPE_SunsetStart:
	case OBJTYPE_SunsetGoal:
		return kNoGx;
	case OBJTYPE_GrassGen:
	case OBJTYPE_RockGen:
		return pcDrRec(kDrBitGrass, kDrLaneGrass, kDrPcGrass);
	case OBJTYPE_Pikihead:
		return pcDrRec(kDrBitPikiHead, kDrLanePikiHead, kDrPcPikiHead);
	case OBJTYPE_Goal:
		return pcDrRec(kDrBitGoal, kDrLaneGoal, kDrPcGoal);
	case OBJTYPE_SluiceSoft:
	case OBJTYPE_SluiceHard:
	case OBJTYPE_SluiceBomb:
	case OBJTYPE_SluiceBombHard:
		return pcDrRec(kDrBitBuilding, kDrLaneBuilding, kDrPcBuilding);
	case OBJTYPE_Bomb:
	case OBJTYPE_Water:
	case OBJTYPE_FallWater: {
		const ItemCreature* item = static_cast<const ItemCreature*>(obj);
		if (item->mItemShapeObject) {
			return pcDrRec(kDrBitBomb, kDrLaneBomb, kDrPcBomb);
		}
		if (!item->mItemShape) {
			return kNoGx;
		}
		return kSerial;
	}
	default:
		return kSerial;
	}
}

// S2 state.
float sS2Remaining        = 0.0f; // expected B not recorded yet (us)
unsigned sS2RemainBytes   = 0;    // expected bytes not recorded yet
bool sS2Broke             = false; // part break since the last part
bool sS2Stop              = false; // no more parts this S2
unsigned sS2Count[kDrPcCount];     // records by price class
unsigned sS2Last[kDrPcCount];      // last S2 frame's records
bool sS2HaveLast          = false;
unsigned sS2Lane[kDrLaneCount];    // pre-scan records by lane

// Lane packets.
template <class P>
struct PcDrPackets {
	P* mItems = nullptr;
	int mCap  = 0;
	int mN    = 0;
	void begin(int need)
	{
		mN = 0;
		if (need <= mCap) {
			return;
		}
		const int cap = need + 16;
		void* mem     = memalign(32, size_t(cap) * sizeof(P));
		if (mem) {
			free(mItems);
			mItems = static_cast<P*>(mem);
			mCap   = cap;
		}
	}
	bool room() const { return mN < mCap; }
	int take()
	{
		const int k = mN++;
		memset(&mItems[k], 0, sizeof(P));
		return k;
	}
};

// Whether a static shape is recordable.
bool pcDrStaticShapeOk(const BaseShape* s, u32 mask)
{
	if (!s || !pcDrShapeInfo(s).ok) {
		return false;
	}
	return !(s->mMeshCount && !(s->mShapeFlags & ShapeFlags::AlwaysRedraw) && (s->mShapeFlags & ShapeFlags::AllowCaching)
	         && (mask & MATFLAG_AlphaBlend));
}

// Part break before a serial object.
void pcDrS2Break(int objType)
{
	if (pcDrawrecPartOpen()) {
		pcDrawrecPartBreak(objType);
		sS2Broke = true;
	}
}

// Pre-scan of the item lists.
void pcDrS2Scan(ObjectMgr* mgr, float& us, unsigned& bytes)
{
	if (!mgr) {
		return;
	}
	Iterator it(mgr);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (!obj) {
			continue;
		}
		const PcDrObjCls c = pcDrObjClassOf(obj);
		if (c.kind == kObjRecord) {
			us += pcDrawrecPrice(c.price);
			bytes += pcDrawrecBytes(c.price);
			++sS2Lane[c.lane];
		}
	}
}

// GrassGen / RockGen lane.
struct PcGrassPacket {
	ItemCreature* mGen;
	u8 mRock;
};
PcDrPackets<PcGrassPacket> sGrPk;

void pcGrassDrawrecB(Graphics& gW, int k)
{
	PcGrassPacket& p       = sGrPk.mItems[k];
	const Matrix4f* before = gW.mActiveMatrix;
	if (p.mRock) {
		static_cast<RockGen*>(p.mGen)->pcRefreshBody(gW);
	} else {
		static_cast<GrassGen*>(p.mGen)->pcRefreshBody(gW);
	}
	if (gW.mActiveMatrix != before) {
		gW.mActiveMatrix = &g_pcDrDeadMtx;
	}
}
const bool sGrassLane = (pcDrawrecRegisterLane(kDrLaneGrass, PcDrLaneDef { &pcGrassDrawrecB, "grass" }), true);

// PikiHeadItem lane.
struct PcPikiHeadPacket {
	Shape* mShape;
	Shape* mLeaf;
	Matrix4f* mPose;
	Colour mColour;
	u8 mOnCamera, mAnimCtx, mAnimData, mHoisted;
};
PcDrPackets<PcPikiHeadPacket> sPhPk;

void pcPikiHeadDrawrecB(Graphics& gW, int k)
{
	const PcPikiHeadPacket& p = sPhPk.mItems[k];
	p.mShape->mMaterialList->setColour(p.mColour);
	if (p.mOnCamera) {
		gW.useMatrix(Matrix4f::ident, 0);
		gW.mPcPose         = p.mPose;
		gW.mPcPoseShape    = p.mShape;
		gW.mPcPoseAnimCtx  = p.mAnimCtx;
		gW.mPcPoseAnimData = p.mAnimData;
		gW.mPcCacheHoisted = p.mHoisted;
		p.mShape->drawshape(gW, *gW.mCamera, nullptr);
		gW.mPcPose         = nullptr;
		gW.mPcPoseShape    = nullptr;
		gW.mPcCacheHoisted = 0;
	}
	gW.useMatrix(p.mPose[3], 0);
	if (p.mOnCamera) {
		p.mLeaf->drawshape(gW, *gW.mCamera, nullptr);
	}
}
const bool sPikiHeadLane = (pcDrawrecRegisterLane(kDrLanePikiHead, PcDrLaneDef { &pcPikiHeadDrawrecB, "pikihead" }), true);

// Posed-item record (goal, building, bomb, pellet, carcass, work, bridge, stick).
enum {
	kItLight   = 1 << 0,
	kItAnim    = 1 << 1,
	kItAnimVal = 1 << 2, // with value, else null frame
	kItIdent   = 1 << 3,
	kItDraw    = 1 << 4,
	kItHoisted = 1 << 5,
};
struct PcItemPacket {
	Shape* mShape;
	Matrix4f* mPose;
	ShapeDynMaterials* mDyn;     // dynamic materials, or null
	ShapeDynMaterials* mAnimDyn;
	DynCollShape* mBuild;        // joint visibility, or null
	f32 mVal;
	u8 mFlags, mAnimCtx, mAnimData;
};
PcDrPackets<PcItemPacket> sItPk;

// B of a posed-item record.
void pcItemRunB(Graphics& g, const PcItemPacket& p, bool serial)
{
	if (p.mFlags & kItLight) {
		g.setLightingNoPrev(true, nullptr);
	}
	if (p.mFlags & kItAnim) {
		f32 v = p.mVal;
		if (serial) {
			p.mAnimDyn->animate((p.mFlags & kItAnimVal) ? &v : nullptr);
		} else {
			p.mAnimDyn->pcAnimateB((p.mFlags & kItAnimVal) ? &v : nullptr);
		}
	}
	if (p.mFlags & kItIdent) {
		g.useMatrix(Matrix4f::ident, 0);
	}
	if (!(p.mFlags & kItDraw)) {
		return;
	}
	if (p.mBuild) {
		p.mBuild->updateContext();
	}
	if (serial) {
		p.mShape->drawshape(g, *g.mCamera, p.mDyn);
		return;
	}
	g.mPcPose          = p.mPose;
	g.mPcPoseShape     = p.mShape;
	g.mPcPoseAnimCtx   = p.mAnimCtx;
	g.mPcPoseAnimData  = p.mAnimData;
	g.mPcCacheHoisted  = (p.mFlags & kItHoisted) ? 1 : 0;
	p.mShape->drawshape(g, *g.mCamera, p.mDyn);
	g.mPcPose          = nullptr;
	g.mPcPoseShape     = nullptr;
	g.mPcCacheHoisted  = 0;
}
void pcItemDrawrecB(Graphics& gW, int k)
{
	pcItemRunB(gW, sItPk.mItems[k], false);
}
const bool sItemLanes = (pcDrawrecRegisterLane(kDrLaneGoal, PcDrLaneDef { &pcItemDrawrecB, "goal" }),
                         pcDrawrecRegisterLane(kDrLaneBuilding, PcDrLaneDef { &pcItemDrawrecB, "building" }),
                         pcDrawrecRegisterLane(kDrLaneBomb, PcDrLaneDef { &pcItemDrawrecB, "bomb" }),
                         pcDrawrecRegisterLane(kDrLanePellet, PcDrLaneDef { &pcItemDrawrecB, "pellet" }),
                         pcDrawrecRegisterLane(kDrLaneCarcass, PcDrLaneDef { &pcItemDrawrecB, "carcass" }),
                         pcDrawrecRegisterLane(kDrLaneWork, PcDrLaneDef { &pcItemDrawrecB, "work" }),
                         pcDrawrecRegisterLane(kDrLaneBridge, PcDrLaneDef { &pcItemDrawrecB, "bridge" }),
                         pcDrawrecRegisterLane(kDrLaneStick, PcDrLaneDef { &pcItemDrawrecB, "stick" }), true);

} // namespace


bool pcDrItemBegin(Creature& obj, Shape* shape, Graphics& gfx)
{
	(void)gfx;
	g_pcDrRecObj = nullptr;
	if (!shape || !pcDrShapeInfo(shape).ok || !sItPk.room()) {
		pcDrS2Break(int(obj.mObjType));
		return false;
	}
	return pcDrawrecCanPublish();
}

void pcDrItemRecord(const PcDrItemRec& r, Graphics& gfx)
{
	PcItemPacket q;
	memset(&q, 0, sizeof(q));
	q.mShape   = r.shape;
	q.mPose    = r.shape->mAnimMatrices;
	q.mDyn     = r.dyn;
	q.mAnimDyn = r.animDyn;
	q.mBuild   = r.draw ? r.build : nullptr;
	q.mVal     = r.val;
	q.mFlags   = u8((r.light ? kItLight : 0) | (r.animDyn ? kItAnim : 0) | (r.animDyn && r.animVal ? kItAnimVal : 0)
	                | (r.ident ? kItIdent : 0) | (r.draw ? kItDraw : 0));
	q.mAnimCtx  = 1;
	q.mAnimData = 1;
	const bool data = !r.draw || (r.shape->mCurrentAnimation && r.shape->mCurrentAnimation->mData);
	if (!pcDrawrecPartOpen() || !data || !pcDrawrecCanPublish()) {
		if (pcDrawrecPartOpen()) {
			pcDrawrecBail();
		}
		pcItemRunB(gfx, q, true);
		return;
	}
	const u32 mask = gfx.mMatRenderMask;
	if (r.draw && pcDrHoistCache(gfx, r.shape, r.dyn)) {
		q.mFlags |= kItHoisted;
	}
	// Stamps.
	if (r.animDyn) {
		r.shape->mPcSegChain = g_pcDrChain;
		if (r.animDyn->mModel) {
			r.animDyn->mModel->mPcSegChain = g_pcDrChain;
		}
	}
	if (r.draw && r.dyn) {
		FOREACH_NODE(ShapeDynMaterials, r.dyn, d)
		{
			if (d->mModel) {
				d->mModel->mPcSegChain = g_pcDrChain;
			}
		}
		r.shape->mPcSegChain = g_pcDrChain;
	}
	if (q.mBuild && q.mBuild->mCollisionModel) {
		q.mBuild->mCollisionModel->mPcSegChain = g_pcDrChain;
	}
	if (r.stamp) {
		r.shape->mPcSegChain = g_pcDrChain;
	}
	if (r.light) {
		pcDrawrecAddLightingWrite(1u);
	}
	if (r.draw) {
		pcDrawrecAddSets(pcDrShapeInfo(r.shape).writes);
	}
	const int k       = sItPk.take();
	sItPk.mItems[k]   = q;
	PcDrEffect e;
	e.meshes     = (r.draw && pcDrShapeDrawsMeshes(r.shape, mask)) ? 1 : 0;
	e.curMtx0    = r.ident ? 1 : 0;
	e.active     = r.ident ? 1 : 0;
	e.lightModel = 0;
	e.activePtr  = r.ident ? &Matrix4f::ident : nullptr;
	pcDrawrecPublishRec(r.lane, k, e, r.priceClass);
	pcDrS2Spent(r.priceClass);
}

bool pcDrPikiHeadBegin(PikiHeadItem& item, Graphics& gfx)
{
	g_pcDrRecObj = nullptr;
	Shape* s     = item.mItemShapeObject ? item.mItemShapeObject->mShape : nullptr;
	Shape* leaf  = (pikiMgr && unsigned(item.mFlowerStage) < 3u) ? pikiMgr->mLeafModel[item.mFlowerStage] : nullptr;
	if (!s || !pcDrShapeInfo(s).ok || !pcDrStaticShapeOk(leaf, gfx.mMatRenderMask) || !sPhPk.room()) {
		pcDrS2Break(int(item.mObjType));
		return false;
	}
	return pcDrawrecCanPublish();
}

void pcDrPikiHeadPublish(PikiHeadItem& item, Graphics& gfx, bool onCamera)
{
	Shape* s            = item.mItemShapeObject->mShape;
	Shape* leaf         = pikiMgr->mLeafModel[item.mFlowerStage];
	const Colour colour = Piki::pikiColors[item.mSeedColor];
	const bool data     = s->mCurrentAnimation && s->mCurrentAnimation->mData;
	if (!pcDrawrecPartOpen() || !data || !pcDrawrecCanPublish()) {
		if (pcDrawrecPartOpen()) {
			pcDrawrecBail();
		}
		pcDrStampCheck(s, kDrStampSerial);
		s->mMaterialList->setColour(colour);
		if (onCamera) {
			gfx.useMatrix(Matrix4f::ident, 0);
			s->drawshape(gfx, *gfx.mCamera, nullptr);
		}
		gfx.useMatrix(s->getAnimMatrix(3), 0);
		if (onCamera) {
			leaf->drawshape(gfx, *gfx.mCamera, nullptr);
		}
		return;
	}
	const int k         = sPhPk.take();
	PcPikiHeadPacket& p = sPhPk.mItems[k];
	p.mShape            = s;
	p.mLeaf             = leaf;
	p.mPose             = s->mAnimMatrices;
	p.mColour           = colour;
	p.mOnCamera         = onCamera ? 1 : 0;
	p.mAnimCtx          = 1;
	p.mAnimData         = 1;
	p.mHoisted          = (onCamera && pcDrHoistCache(gfx, s, nullptr)) ? 1 : 0;
	s->mPcSegChain      = g_pcDrChain;
	const u32 mask = gfx.mMatRenderMask;
	bool meshes    = false;
	if (onCamera) {
		pcDrawrecAddSets(pcDrShapeInfo(s).writes);
		pcDrawrecAddSets(pcDrShapeInfo(leaf).writes);
		meshes = pcDrShapeDrawsMeshes(s, mask) || pcDrShapeDrawsMeshes(leaf, mask);
	}
	PcDrEffect e;
	e.meshes     = meshes ? 1 : 0;
	e.curMtx0    = 1;
	e.active     = 1;
	e.lightModel = 0;
	e.activePtr  = &p.mPose[3];
	pcDrawrecPublishRec(kDrLanePikiHead, k, e, kDrPcPikiHead);
	pcDrS2Spent(kDrPcPikiHead);
}


bool pcDrGrassRecord(ItemCreature& gen, bool rock, Graphics& gfx)
{
	g_pcDrRecObj = nullptr;
	Shape* shapes[3];
	int shapeCount  = 0;
	const bool drew = rock ? static_cast<RockGen&>(gen).pcDrDraws(gfx, shapes, shapeCount)
	                       : static_cast<GrassGen&>(gen).pcDrDraws(gfx, shapes, shapeCount);
	for (int i = 0; i < shapeCount; i++) {
		if (!pcDrStaticShapeOk(shapes[i], gfx.mMatRenderMask)) {
			pcDrS2Break(int(gen.mObjType));
			return false;
		}
	}
	if (!sGrPk.room()) {
		pcDrS2Break(int(gen.mObjType));
		return false;
	}
	if (!pcDrawrecCanPublish()) {
		return false;
	}
	const int k      = sGrPk.take();
	PcGrassPacket& p = sGrPk.mItems[k];
	p.mGen           = &gen;
	p.mRock          = rock ? 1 : 0;
	PcDrEffect e;
	memset(&e, 0, sizeof(e));
	if (drew) {
		PcDrSets sets;
		memset(&sets, 0, sizeof(sets));
		sets.mHasTexGen.add(1u);
		pcDrawrecAddSets(sets);
		bool meshes = false;
		for (int i = 0; i < shapeCount; i++) {
			pcDrawrecAddSets(pcDrShapeInfo(shapes[i]).writes);
			meshes = meshes || pcDrShapeDrawsMeshes(shapes[i], gfx.mMatRenderMask);
		}
		e.meshes  = meshes ? 1 : 0;
		e.curMtx0 = 1;
		e.active  = 2; // dead local stand-in
	}
	pcDrawrecPublishRec(kDrLaneGrass, k, e, kDrPcGrass);
	pcDrS2Spent(kDrPcGrass);
	return true;
}

// S2 begin, plant gate, end.
void pcDrS2Begin(Graphics& gfx, int hideType)
{
	(void)gfx;
	g_pcDrS2Live = 0;
	g_pcDrRecObj = nullptr;
	sS2Lanes     = pcDrawrecLanes();
	if (!(sS2Lanes & kDrBitsS2)) {
		pcDrawrecChainComplete();
		return;
	}
	if (hideType != 0 || AIPerf::generatorMode || gameflow.mMoviePlayer->mIsActive || AIPerf::kandoOnly || gsys->mToggleDebugInfo
	    || pc_photo_mode_active()) {
		pcDrawrecChainComplete();
		return;
	}
	// Expected B.
	float us       = 0.0f;
	unsigned bytes = 0;
	memset(sS2Lane, 0, sizeof(sS2Lane));
	(void)sGrassLane;
	if (itemMgr) {
		pcDrS2Scan(itemMgr, us, bytes);
		pcDrS2Scan(itemMgr->getPikiHeadMgr(), us, bytes);
		pcDrS2Scan(itemMgr->getMeltingPotMgr(), us, bytes);
	}
	static const int kTail[] = { kDrPcPlant };
	unsigned seed[kDrPcCount];
	memset(seed, 0, sizeof(seed));
	if (plantMgr && (sS2Lanes & kDrBitPlant)) {
		seed[kDrPcPlant] = unsigned(plantMgr->getSize() / 2);
	}
	const unsigned* tail = sS2HaveLast ? sS2Last : seed;
	for (int c : kTail) {
		us += float(tail[c]) * pcDrawrecPrice(c);
		bytes += tail[c] * pcDrawrecBytes(c);
	}
	if (us < pcDrawrecMinB()) {
		if (sS2HaveLast) {
			for (int c : kTail) {
				if (sS2Last[c] < seed[c]) {
					sS2Last[c] += (seed[c] - sS2Last[c] + 1u) / 2u;
				}
			}
		}
		pcDrawrecChainComplete();
		return;
	}
	sS2Remaining   = us;
	sS2RemainBytes = bytes;
	sS2Broke       = false;
	sS2Stop        = false;
	memset(sS2Count, 0, sizeof(sS2Count));
	sGrPk.begin(int(sS2Lane[kDrLaneGrass]));
	sPhPk.begin(int(sS2Lane[kDrLanePikiHead]));
	(void)sPikiHeadLane;
	sItPk.begin(int(sS2Lane[kDrLaneGoal] + sS2Lane[kDrLaneBuilding] + sS2Lane[kDrLaneBomb] + sS2Lane[kDrLaneStick])
	            + (workObjectMgr ? workObjectMgr->getSize() : 0) + (pelletMgr ? pelletMgr->getSize() : 0));
	(void)sItemLanes;
	g_pcDrS2Live = 1;
}

void pcDrS2PlantGate(void)
{
	if (g_pcDrS2Live && !(sS2Lanes & kDrBitPlant)) {
		pcDrawrecSeal();
		pcDrS2End();
	}
}

void pcDrS2End(void)
{
	if (!g_pcDrS2Live) {
		return;
	}
	pcDrawrecSeal();
	pcDrawrecChainComplete();
	g_pcDrS2Live = 0;
	g_pcDrRecObj = nullptr;
	memcpy(sS2Last, sS2Count, sizeof(sS2Last));
	sS2HaveLast = true;
}

void pcDrS2Reset(void)
{
	memset(sS2Last, 0, sizeof(sS2Last));
	sS2HaveLast = false;
}

// Per-object gate.
bool pcDrS2EnsurePart(Graphics& gfx, int lane)
{
	if (pcDrawrecPartOpen()) {
		return true;
	}
	if (sS2Stop) {
		return false;
	}
	bool retry = false;
	if (!pcDrawrecForkPart(gfx, lane, sS2Remaining, sS2RemainBytes, sS2Broke, &retry)) {
		sS2Stop = !retry;
		return false;
	}
	sS2Broke = false;
	return true;
}

void pcDrS2Spent(int priceClass)
{
	if (priceClass < 0 || priceClass >= kDrPcCount) {
		return;
	}
	++sS2Count[priceClass];
	sS2Remaining -= pcDrawrecPrice(priceClass);
	const unsigned b = pcDrawrecBytes(priceClass);
	sS2RemainBytes   = sS2RemainBytes > b ? sS2RemainBytes - b : 0u;
}

void pcDrObj(Creature* obj, Graphics& gfx)
{
	const PcDrObjCls c = pcDrObjClassOf(obj);
	if (c.kind == kObjRecord && pcDrS2EnsurePart(gfx, c.lane)) {
		g_pcDrRecObj = obj;
		obj->refresh(gfx);
		g_pcDrRecObj = nullptr;
		return;
	}
	if (c.kind == kObjSerial) {
		pcDrS2Break(int(obj->mObjType));
	}
	obj->refresh(gfx);
}
#endif
