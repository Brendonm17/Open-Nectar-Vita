#include "PlantMgr.h"
#include "Age.h"
#include "CreatureProp.h"
#include "DebugLog.h"
#include "Dolphin/os.h"
#include "Generator.h"
#include "Graphics.h"
#include "MapMgr.h"
#include "SoundMgr.h"
#include "gameflow.h"
#include "sysNew.h"
#if defined(__vita__)
void pcNodeListWatch(CoreNode* root);
#endif
#if defined(PIKI_PC_PORT) && defined(__vita__)
#include "Collision.h"
#include "PaniAnimator.h"
#include "Shape.h"
#include <cstring>
bool pcPlantDrawRun(Graphics& gfx, PlantMgr* mgr);
int pcSplitCountPartUpdaters(CollInfo* info);
static void* sPcPlantVtbl = nullptr;               // Plant vtable
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("plantMgr");

PlantMgr* plantMgr;

/**
 * @todo: Documentation
 */
Plant::Plant()
    : AICreature(nullptr)
{
	mObjType   = OBJTYPE_Plant;
	mPlantType = PLANT_NULL;
	mCollInfo  = new CollInfo(8);
	mSearchBuffer.init(mPlantSearchData, 3);
#if defined(PIKI_PC_PORT) && defined(__vita__)
	sPcPlantVtbl = *reinterpret_cast<void* const*>(this);
#endif
}

/**
 * @todo: Documentation
 */
void Plant::startMotion(int motionID)
{
	mPlantAnimator.startMotion(PaniMotionInfo(motionID, this));
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000090
 */
void Plant::reset(int plantType)
{
	_394                    = true;
	mObjType                = OBJTYPE_Plant;
	mPlantType              = plantType;
	PlantShapeObject* shape = plantMgr->mPlantShapes[plantType];
	mPlantAnimator.init(&shape->mAnimContext, shape->mAnimMgr, plantMgr->mMotionTable);
	mMotionSpeed = 0.0f;
	mCollInfo->initInfo(shape->mShape, nullptr, nullptr);
	mSAICtx.mStateMachine = plantMgr->mAI;
}

/**
 * @todo: Documentation
 */
void Plant::startAI(int)
{
	mIsCulled = false;
	mapMgr    = plantMgr->mMapMgr;
	mProps    = plantMgr->mPlantProps;
	startMotion(PlantMotion::Touch);
	mMotionSpeed = 0.0f;
	mPlantAnimator.startMotion(PaniMotionInfo(PlantMotion::Touch));
	mSRT.t.y = mapMgr->getMinY(mSRT.t.x, mSRT.t.z, true);
	plantMgr->mAI->start(this, PlantAI::STATE_Wait);
	if (mPlantType == PLANT_Mizukusa) {
		mMotionSpeed = 30.0f;
	}
}

/**
 * @todo: Documentation
 */
void Plant::doAnimation()
{
	mPlantAnimator.animate(mMotionSpeed);
}

/**
 * @todo: Documentation
 */
void Plant::update()
{
	if (mIsCulled) {
		return;
	}

	mGrid.updateGrid(mSRT.t);
	mGrid.updateAIGrid(mSRT.t, false);
	if (mPlantType == PLANT_Hae || mPlantType == PLANT_Mizukusa || !mGrid.aiCulling()) {
		doAnimation();
		plantMgr->mAI->exec(this);
	}
}

/**
 * @todo: Documentation
 */
void Plant::refresh(Graphics& gfx)
{
	PlantShapeObject* shape = plantMgr->mPlantShapes[mPlantType];
	if (!mPlantAnimator.mAnimInfo) {
		return;
	}

	if (mCollInfo->hasInfo()) {

		f32 rad = 2.0f * getBoundingSphereRadius();
		if (rad > 0.0f) {
			BoundBox box(Vector3f(mSRT.t.x - rad, mSRT.t.y, mSRT.t.z - rad),
			             Vector3f(mSRT.t.x + rad, mSRT.t.y + 2.0f * rad, mSRT.t.z + rad));

			if (!gfx.mCamera->isBoundVisible(box, 0x8000 | 0x20 | 0x10 | 0x1 | 0x2 | 0x4 | 0x8) && !_394) {
				C_SAI(this)->start(this, PlantAI::STATE_Wait);
				mIsCulled = true;
				return;
			}
		}
	}

	mIsCulled = false;
	_394      = false;
	mWorldMtx.makeSRT(mSRT.s, mSRT.r, mSRT.t);
	Matrix4f mtx;
	gfx.mCamera->mLookAtMtx.multiplyTo(mWorldMtx, mtx);

	mPlantAnimator.updateContext();
	shape->mShape->updateAnim(gfx, mtx, nullptr, this);

	gfx.useMatrix(Matrix4f::ident, 0);
	shape->mShape->drawshape(gfx, *gfx.mCamera, nullptr);
	mCollInfo->updateInfo(gfx, false);
}

#if defined(PIKI_PC_PORT) && defined(__vita__)
// PS Vita plant A/B flip.

/**
 * A: refresh without its GX half.
 */
bool Plant::pcRefreshA(Graphics& gfx, Shape*& shapeOut)
{
	PlantShapeObject* shape = plantMgr->mPlantShapes[mPlantType];
	if (!mPlantAnimator.mAnimInfo) {
		return false;
	}

	if (mCollInfo->hasInfo()) {

		f32 rad = 2.0f * getBoundingSphereRadius();
		if (rad > 0.0f) {
			BoundBox box(Vector3f(mSRT.t.x - rad, mSRT.t.y, mSRT.t.z - rad),
			             Vector3f(mSRT.t.x + rad, mSRT.t.y + 2.0f * rad, mSRT.t.z + rad));

			if (!gfx.mCamera->isBoundVisible(box, 0x8000 | 0x20 | 0x10 | 0x1 | 0x2 | 0x4 | 0x8) && !_394) {
				C_SAI(this)->start(this, PlantAI::STATE_Wait);
				mIsCulled = true;
				return false;
			}
		}
	}

	mIsCulled = false;
	_394      = false;
	mWorldMtx.makeSRT(mSRT.s, mSRT.r, mSRT.t);
	Matrix4f mtx;
	gfx.mCamera->mLookAtMtx.multiplyTo(mWorldMtx, mtx);

	mPlantAnimator.updateContext();
	shape->mShape->updateAnim(gfx, mtx, nullptr, this);

	mCollInfo->updateInfo(gfx, false);
	shapeOut = shape->mShape;
	return true;
}

/**
 * Whether this plant's A would hit an ERROR.
 */
bool Plant::pcWouldError(Graphics& gfx)
{
	if (!mPlantAnimator.mAnimInfo) {
		return false;
	}
	BaseShape* shape = plantMgr->mPlantShapes[mPlantType]->mShape;
	if (gfx.mNextFreeMatrixIdx + int(shape->mAnimMtxCount) > gfx.mMaxMatrixCount) {
		return true;
	}
	AnimContext* base = shape->mCurrentAnimation;
	if (!base || !base->mData) {
		return true; // bind-pose branch
	}
	const bool baseBad = base->mData->mTotalFrameCount <= 0 || !(base->mCurrentFrame >= 0.0f) || !(base->mAnimSpeed >= 0.0f);
	const PaniPlantAnimator& anim = mPlantAnimator;
	for (int i = 0; i < shape->mJointCount; i++) {
		AnimContext* ctx = shape->mAnimOverrides[i];
		AnimData* data;
		f32 frame;
		if (anim.mContext == ctx) {
			data  = anim.mAnimInfo->mData;
			frame = anim.mAnimationCounter;
		} else if (ctx == base) {
			if (baseBad) {
				return true;
			}
			continue;
		} else {
			data  = ctx->mData;
			frame = ctx->mCurrentFrame;
		}
		if (!data) {
			return true;
		}
		const int frameNum = int(frame);
		if (frameNum < 0 || frameNum >= data->mTotalFrameCount) {
			return true;
		}
	}
	return false;
}

#endif

/**
 * @todo: Documentation
 */
void Plant::doKill()
{
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000170
 */
PlantAI::PlantAI()
{
	OpponentMove* oppMove = new OpponentMove();
	create(STATE_COUNT);
	addState(STATE_Wait, PANI_NO_MOTION, new WaitInit());
	addState(STATE_Touch, PANI_NO_MOTION, new TouchInit());
	addArrow(STATE_Wait, saiCollideEvent, STATE_Touch)->addCondition(oppMove);
	addArrow(STATE_Touch, saiMotionDoneEvent, STATE_Wait);
}

/**
 * @todo: Documentation
 */
bool PlantAI::OpponentMove::satisfy(AICreature* plant)
{
	Creature* collider = plant->mSAICtx.mCollidingCreature;
	f32 dist           = collider->mVelocity.length();
	if (dist > 40.0f) {
		return true;
	}

	return false;
}

/**
 * @todo: Documentation
 */
void PlantAI::WaitInit::act(AICreature* plant)
{
	STACK_PAD_VAR(1);

	if (static_cast<Plant*>(plant)->mPlantType == PLANT_Mizukusa) {
		plant->startMotion(PlantMotion::Touch);
		plant->setMotionSpeed(30.0f);
	} else {
		plant->startMotion(PlantMotion::Touch);
		plant->stopMotion();
	}
}

/**
 * @todo: Documentation
 */
void PlantAI::TouchInit::act(AICreature* plant)
{
	STACK_PAD_VAR(1);
	if (static_cast<Plant*>(plant)->mPlantType != PLANT_Mizukusa) {
		if (plant->mSAICtx.mCollidingCreature->mObjType == OBJTYPE_Navi) {
			SeSystem::playPlayerSe(SE_ORIMA_TOUCHPLANTS);
		}

		plant->setMotionSpeed(30.0f);
	}
}

/**
 * @todo: Documentation
 */
PlantMgr::PlantMgr(MapMgr* mgr)
{
#if defined(__vita__)
	pcNodeListWatch(&mRootNode);
#endif
	mMapMgr = mgr;
	gameflow.addGenNode("plantMgr", this);
	mMotionTable = PaniPlantAnimator::createMotionTable();
	mPlantShapes = new PlantShapeObject*[PLANT_COUNT];
	for (int i = PLANT_START; i < PLANT_COUNT; i++) {
		mPlantShapes[i] = nullptr;
	}

	mAI         = new PlantAI();
	mPlantProps = new CreatureProp();

	mAnimFrameCacher = new AnimFrameCacher(1000);
}

// see PlantTypes enum for more info on what these are
static immut char* plantNames[PLANT_COUNT] = {
	"clover",   // 0
	"chidome",  // 1
	"hutaba",   // 2
	"ine",      // 3
	"tanpopo",  // 4
	"ooinu_l",  // 5
	"ooinu_s",  // 6
	"mizukusa", // 7
	"wakame_l", // 8
	"wakame_s", // 9
	"kinokolt", // 10
	"hae",      // 11
};

/**
 * @todo: Documentation
 * @note UNUSED Size: 000018
 */
immut char* PlantMgr::getPlantName(int plantType)
{
	return plantNames[plantType];
}

/**
 * @todo: Documentation
 */
void PlantMgr::initialise()
{
	for (int i = PLANT_START; i < PLANT_COUNT; i++) {
		if (!usePlantType(i)) {
			continue;
		}

		immut char* name = plantNames[i];
		char shapeFile[256];
		sprintf(shapeFile, "objects/plants/%s.mod", name);

		char unused[256];
		sprintf(unused, "%s", name);

		char plantFileName[256];
		sprintf(plantFileName, "%s.bin", name);

		mPlantShapes[i] = new PlantShapeObject(gameflow.loadShape(shapeFile, true), plantNames[i], plantFileName);
	}
}

/**
 * @todo: Documentation
 */
Creature* PlantMgr::createObject()
{
	return new Plant();
}

/**
 * @todo: Documentation
 */
bool PlantMgr::usePlantType(int type)
{
	FOREACH_NODE(UseNode, mRootUseNode.mChild, node)
	{
		if (node->mPlantType == type) {
			return true;
		}
	}

	return false;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4 (Matching by size)
 */
void PlantMgr::addUseList(int plantType)
{
	if (!usePlantType(plantType)) {
		UseNode* node    = new UseNode();
		node->mPlantType = plantType;
		mRootUseNode.add(node);
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000150 (Matching by size)
 */
Creature* PlantMgr::birth()
{
	CreatureNode* cnode = new CreatureNode();
	cnode->mCreature    = new Plant();
	mRootNode.add(cnode);
	return cnode->mCreature;
}

/**
 * @todo: Documentation
 */
PlantShapeObject::PlantShapeObject(Shape* shape, immut char* plantName, immut char* fileName)
{
	mShape               = shape;
	mShape->mFrameCacher = plantMgr->mAnimFrameCacher;
	if (plantName) {
		char buf[128];
		sprintf(buf, "objects/plants/%s", fileName);
		mAnimMgr        = new AnimMgr(shape, buf, ANIMMGR_LOAD_BUNDLE, nullptr);
		mAnimMgr->mName = plantName;
	} else {
		mAnimMgr = new AnimMgr(shape, nullptr, ANIMMGR_LOAD_NOSKIP, nullptr);
	}

	mShape->overrideAnim(0, &mAnimContext);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000068
 */
GenObjectPlant::GenObjectPlant()
    : GenObject('plnt', "create Plant")
{
	mPlantType = PLANT_Clover;
}

/**
 * @todo: Documentation
 */
static GenObject* makeObjectPlant()
{
	return new GenObjectPlant();
}

/**
 * @todo: Documentation
 */
void GenObjectPlant::initialise()
{
	GenObjectFactory::factory->registerMember('plnt', &makeObjectPlant, "Generate PLANT", 'v0.0');
}

/**
 * @todo: Documentation
 */
void GenObjectPlant::doRead(RandomAccessStream& input)
{
	mPlantType = input.readInt();
}

/**
 * @todo: Documentation
 */
void GenObjectPlant::updateUseList(Generator* gen, int)
{
	plantMgr->addUseList(mPlantType);
}

/**
 * @todo: Documentation
 */
Creature* GenObjectPlant::birth(BirthInfo& info)
{
	Creature* plant = plantMgr->birth();
	if (plant) {
		plant->init(info.mPosition);
		plant->mSRT.r         = info.mRotation;
		plant->mFaceDirection = plant->mSRT.r.y;

		Plant* pPlant = static_cast<Plant*>(plant);
		pPlant->reset(mPlantType);

		plant->mGenerator = info.mGenerator;
		plant->mHealth    = 1.0f;
		plant->mMaxHealth = plant->mHealth;
		plant->startAI(PlantAI::STATE_Wait);
	}

	return plant;
}

/**
 * @todo: Documentation
 */
void GenObjectPlant::render(Graphics&, Generator*)
{
	plantMgr->usePlantType(mPlantType);
}

#if defined(PIKI_PC_PORT) && defined(__vita__)
// PS Vita plant A/B flip: manager side.

/**
 * The plant draw.
 */
void PlantMgr::refresh(Graphics& gfx)
{
	if (pcPlantDrawRun(gfx, this)) {
		return;
	}
	ObjectMgr::refresh(gfx);
}

/**
 * The plants in ObjectMgr::refresh order.
 */
int PlantMgr::pcSplitSnapshot(Plant** out, int cap)
{
	int count = 0;
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (!sPcPlantVtbl || *reinterpret_cast<void* const*>(obj) != sPcPlantVtbl) {
			return -1;
		}
		if (count < cap) {
			out[count] = static_cast<Plant*>(obj);
		}
		count++;
	}
	return count;
}

/**
 * Why a plant type's culled path cannot run on the worker, or null.
 */
static immut char* pcPlantAnimProblem(PlantShapeObject* obj, PaniMotionTable* table)
{
	AnimMgr* mgr = obj->mAnimMgr;
	if (!mgr) {
		return "no animation manager";
	}
	for (AnimInfo* info = static_cast<AnimInfo*>(mgr->mAnimList.mChild); info; info = static_cast<AnimInfo*>(info->mNext)) {
		if (!info->mData) {
			return "an animation not loaded";
		}
	}
	PaniMotion* motion = (table && PlantMotion::Touch < table->mMotionCount) ? table->getMotion(PlantMotion::Touch) : nullptr;
	if (!motion) {
		return "no touch motion";
	}
	AnimInfo* touch = nullptr;
	int idx         = 0;
	for (AnimInfo* info = static_cast<AnimInfo*>(mgr->mAnimList.mChild); info; info = static_cast<AnimInfo*>(info->mNext), idx++) {
		if (idx == motion->mAnimID) {
			touch = info;
			break;
		}
	}
	if (!touch) {
		return "no touch animation";
	}
	const f32 start = f32(touch->getKeyValue(0));
	if (start < 0.0f || start >= touch->mData->mTotalFrameCount) {
		return "an illegal touch start frame";
	}
	return nullptr;
}

/**
 * Load-time test of the plant types for the A/B flip.
 */
bool PlantMgr::pcSplitShapesOk()
{
	struct Key {
		PlantMgr* mMgr;
		PlantShapeObject* mShapes[PLANT_COUNT];
		int mCount;
	};
	static Key sKey;
	static bool sKeyValid = false;
	static bool sOk       = false;
	Key key;
	memset(&key, 0, sizeof(key));
	key.mMgr = this;
	for (int i = PLANT_START; i < PLANT_COUNT; i++) {
		key.mShapes[i] = mPlantShapes[i];
	}
	key.mCount = getSize();
	if (sKeyValid && memcmp(&key, &sKey, sizeof(key)) == 0) {
		return sOk;
	}
	sKey      = key;
	sKeyValid = true;

	bool ok = true;
	for (int i = PLANT_START; i < PLANT_COUNT; i++) {
		PlantShapeObject* obj = mPlantShapes[i];
		if (!obj || !obj->mShape) {
			continue;
		}
		Shape* shape        = obj->mShape;
		const bool lights   = shape->mLightGroup.Child() != nullptr;
		const bool bindPose = !shape->mCurrentAnimation || !shape->mCurrentAnimation->mData;
		ok                  = ok && !lights && !bindPose && !pcPlantAnimProblem(obj, mMotionTable);
	}

	int others = 0, updaters = 0;
	Iterator it(this);
	CI_LOOP(it)
	{
		Creature* obj = *it;
		if (!sPcPlantVtbl || *reinterpret_cast<void* const*>(obj) != sPcPlantVtbl) {
			++others;
			continue;
		}
		updaters += pcSplitCountPartUpdaters(obj->mCollInfo);
	}
	ok  = ok && others == 0 && updaters == 0;
	sOk = ok;
	return ok;
}
#endif

#ifdef WIN32

void GenObjectPlant::doGenAge(AgeServer& server)
{
	server.StartOptionBox("草のタイプ", &mPlantType, 252);
	for (int i = 0; i < PLANT_COUNT; i++) {
		server.NewOption(plantMgr->getPlantName(i), i);
	}
	server.EndOptionBox();
}

#endif
