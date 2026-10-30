#include "ViewPiki.h"
#include "AIPerf.h"
#include "DebugLog.h"
#include "Font.h"
#include "Graphics.h"
#include "ItemMgr.h"
#include "MapCode.h"
#include "MemStat.h"
#include "MoviePlayer.h"
#include "PikiAI.h"
#include "PikiMgr.h"
#include "PikiState.h"
#include "Shape.h"
#include "UfoItem.h"
#include "bugprint.h"
#include "gameflow.h"
#include "sysNew.h"
#if defined(PIKI_PC_PORT)
#include "timing/pc_render_phase.h"
#endif
#if defined(__vita__) && defined(__ARM_NEON)
void pcCalcWeightedMatricesDirty(BaseShape* shape, u32 dirtyJoints);
#endif

// Pikmin draw split into A (simulation) and B (GX) halves.
#if defined(PIKI_PC_PORT) && defined(__vita__)
#define PC_PIKIA 1
static void* sPcViewPikiVtbl = nullptr; // ViewPiki vtable
#else
#define PC_PIKIA 0
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(31)

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("viewPiki")

PikiShapeObject* PikiShapeObject::_instances[4];
bool PikiShapeObject::firstTime = true;

/**
 * @todo: Documentation
 */
void PikiShapeObject::exitCourse()
{
	firstTime     = true;
	_instances[0] = _instances[1] = _instances[2] = _instances[3] = nullptr;
}

/**
 * @todo: Documentation
 */
void PikiShapeObject::init()
{
	firstTime     = true;
	_instances[0] = _instances[1] = _instances[2] = _instances[3] = nullptr;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00006C
 */
PikiShapeObject* PikiShapeObject::create(int index)
{
	memStat->start("piki animmgr");
	if (firstTime) {
		initOnce();
	}
	memStat->end("piki animmgr");
	if (index < 0 || index >= 4) {
		ERROR("illegal index %d\n", index);
	}
	return _instances[index];
}

/**
 * @todo: Documentation
 */
AnimMgr* PikiShapeObject::getAnimMgr()
{
	if (firstTime) {
		initOnce();
	}

	return _instances[0]->mAnimMgr;
}

/**
 * @todo: Documentation
 */
void PikiShapeObject::initOnce()
{
	_instances[Blue]           = new PikiShapeObject(gameflow.loadShape("pikis/bluModel.mod", true));
	_instances[Blue]->mAnimMgr = new AnimMgr(_instances[Blue]->mShape, "pikis/animMgr.bin", ANIMMGR_LOAD_BUNDLE, nullptr);

	gameflow.addGenNode("pikiAnimMgr", _instances[Blue]->mAnimMgr);

	_instances[Red]                   = new PikiShapeObject(gameflow.loadShape("pikis/redModel.mod", true));
	_instances[Red]->mAnimMgr         = _instances[Blue]->mAnimMgr;
	_instances[Yellow]                = new PikiShapeObject(gameflow.loadShape("pikis/yelModel.mod", true));
	_instances[Yellow]->mAnimMgr      = _instances[Blue]->mAnimMgr;
	_instances[PIKI_Kinoko]           = new PikiShapeObject(gameflow.loadShape("pikis/kinModel.mod", true));
	_instances[PIKI_Kinoko]->mAnimMgr = _instances[Blue]->mAnimMgr;

	firstTime = false;
}

/**
 * @todo: Documentation
 */
PikiShapeObject::PikiShapeObject(Shape* shape)
{
	mShape               = shape;
	mShape->mFrameCacher = gameflow.mFrameCacher;
	mAnimMgr             = nullptr;
	mShape->overrideAnim(0, &mAnimatorA);
	mShape->overrideAnim(1, &mAnimatorB);
}

/**
 * @todo: Documentation
 */
bool ViewPiki::isKinoko()
{
	return mPikiShape == PikiShapeObject::_instances[PIKI_Kinoko];
}

/**
 * @todo: Documentation
 */
void ViewPiki::changeShape(int index)
{
	if (index == -1) {
		index = mFormationPriority;
	}

	mPikiShape = PikiShapeObject::create(index);
	mPikiAnimMgr.changeContext(&mPikiShape->mAnimatorB, &mPikiShape->mAnimatorA);
	mCollInfo->initInfo(mPikiShape->mShape, nullptr, nullptr);

	if (isKinoko()) {
		setColor(mColor);
		mHappaModel = nullptr;
	} else {
		setFlower(mHappa);
	}

	f32 scale = 1.0f;
	mSRT.s.set(scale, scale, scale);
	setLeaves(index + 1);
}

/**
 * @todo: Documentation
 */
ViewPiki::ViewPiki(CreatureProp* prop)
    : Piki(prop)
{
	mPikiShape = nullptr;
	mHappa     = 0;
#if PC_PIKIA
	sPcViewPikiVtbl = *reinterpret_cast<void* const*>(this);
#endif
}

/**
 * @todo: Documentation
 */
void ViewPiki::initBirth()
{
	mHappaModel = nullptr;
	mPikiShape  = PikiShapeObject::create(0);
	mPikiAnimMgr.init(mPikiShape->mAnimMgr, &mPikiShape->mAnimatorB, &mPikiShape->mAnimatorA, pikiMgr->mMotionTable);

	f32 scale = 1.0f;
	mSRT.s.set(scale, scale, scale);
	setFlower(0);

#if defined(VERSION_GPIJ01) || defined(VERSION_DPIJ01_PIKIDEMO)
	STACK_PAD_VAR(2);
#else
	STACK_PAD_VAR(4);
#endif
}

/**
 * @todo: Documentation
 */
void ViewPiki::init(Shape* shp, MapMgr*, Navi* navi)
{
	mPikiShape  = PikiShapeObject::create(0);
	mHappaModel = nullptr;

	mCollInfo = new CollInfo(4);
	mCollInfo->initInfo(mPikiShape->mShape, nullptr, nullptr);
	mPikiAnimMgr.init(mPikiShape->mAnimMgr, &mPikiShape->mAnimatorB, &mPikiShape->mAnimatorA, pikiMgr->mMotionTable);

	f32 scale = 1.0f;
	scale *= pikiMgr->mPikiParms->mPikiParms.mPluckStrength0();
	mSRT.s.set(scale, scale, scale);
	setLeaves(1);

	mPikiAnimMgr.startMotion(PaniMotionInfo(PIKIANIM_StillJump, this), PaniMotionInfo(PIKIANIM_StillJump));

	mSRT.s.set(1.0f, 1.0f, 1.0f);
	mSRT.r.set(0.0f, 0.0f, 0.0f);
	mSRT.t.set((gsys->getRand(1.0f) - 0.5f) * 300.0f, 0.0f, (gsys->getRand(1.0f) - 0.5f) * 300.0f);

	mWorldMtx.makeSRT(mSRT.s, mSRT.r, mSRT.t);
	_268 = 0.0f;
	Piki::init(navi);
	mLastPosition       = mSRT.t;
	mLastEffectPosition = mSRT.t;
}

/**
 * @todo: Documentation
 */
void ViewPiki::setFlower(int id)
{
	if (!isKinoko()) {
		mHappa      = id;
		mHappaModel = pikiMgr->mLeafModel[id];
	}
}

/**
 * @todo: Documentation
 */
void ViewPiki::setLeaves(int)
{
}

/**
 * @todo: Documentation
 */
void ViewPiki::postUpdate(int unused, f32 deltaTime)
{
	if (mMode == PikiMode::FreeMode && !pikiMgr->isUpdating(PMUPDATE_FreePiki)) {
		return;
	}

	if (mMode == PikiMode::FormationMode && !pikiMgr->isUpdating(PMUPDATE_FormationPiki)) {
		return;
	}

	if (!pikiMgr->isUpdating(PMUPDATE_WorkPiki)) {
		return;
	}

	if (gameflow.mMoviePlayer->mIsActive && pikiMgr->isUpdating(PMUPDATE_Unk4)) {
		Vector3f toShip = mSRT.t - itemMgr->getUfo()->getGoalPos();
		if (toShip.length() > 100.0f) {
			return;
		}
	}

	Creature::postUpdate(unused, deltaTime);

	STACK_PAD_VAR(2);
}

/**
 * @todo: Documentation
 */
void ViewPiki::update()
{
	if (mMode == PikiMode::FreeMode && !pikiMgr->isUpdating(PMUPDATE_FreePiki)) {
		mVolatileVelocity.set(0.0f, 0.0f, 0.0f);
		return;
	}
	if (mMode == PikiMode::FormationMode && !pikiMgr->isUpdating(PMUPDATE_FormationPiki)) {
		mVolatileVelocity.set(0.0f, 0.0f, 0.0f);
		return;
	}
	if (!pikiMgr->isUpdating(PMUPDATE_WorkPiki)) {
		mVolatileVelocity.set(0.0f, 0.0f, 0.0f);
		return;
	}

	if (gameflow.mMoviePlayer->mIsActive && pikiMgr->isUpdating(PMUPDATE_Unk4)) {
		Vector3f toShip = mSRT.t - itemMgr->getUfo()->getGoalPos();
		if (toShip.length() > 100.0f) {
			mVolatileVelocity.set(0.0f, 0.0f, 0.0f);
			return;
		}
	}

	updateColor();
	mHasCollChangedVelocity = 0;
	Creature::update();
	realAI();

	if (mColor == Blue && gameflow.mDemoFlags & CinePlayerFlags::HideBluePiki) {
		return;
	}

	if (mColor == Red && gameflow.mDemoFlags & CinePlayerFlags::HideRedPiki) {
		return;
	}

	if (mColor == Yellow && gameflow.mDemoFlags & CinePlayerFlags::HideYellowPiki) {
		return;
	}

	if (AIPerf::optLevel == 0 && mGroundTriangle && gsys->getRand(1.0f) > 0.99f) {
		Vector3f toLastPosition = mLastEffectPosition - mSRT.t;

		// If we've moved enough, create an effect
		if (toLastPosition.length() > 40.0f) {
			Vector3f pos(mSRT.t.x, mSRT.t.y + 1.0f, mSRT.t.z);
			Vector3f rot(mVelocity.x * 0.01667f, 1.0f, mVelocity.z * 0.01667f);
			EffectParm parm(pos, rot);

			int attr = MapCode::getAttribute(mGroundTriangle);
			if (attr >= ATTR_Solid && attr <= ATTR_Wood) {
				utEffectMgr->cast(attr + KandoEffect::SmokeOffset, parm);
			}

			mLastEffectPosition = mSRT.t;
		}
	}
}

/**
 * @todo: Documentation
 */
void Piki::startHimaLook(immut Vector3f* pos)
{
	mLookatPosPtr = pos;
	mLookTimer    = false;
	mIsLooking    = false;
	mLookAtCreature.clear();
	mIsLooking = true;
	_334       = gsys->getRand(1.0f) * 3.0f + 4.0f;
}

/**
 * @todo: Documentation
 */
void Piki::finishLook()
{
	mLookatPosPtr = nullptr;
	mLookTimer    = 10;
	mIsLooking    = false;
}

/**
 * @todo: Documentation
 */
bool Piki::isLooking()
{
	return mLookatPosPtr != nullptr;
}

/**
 * @todo: Add documentation
 * Address:	100DE380 in DLL
 */
void Piki::updateLook()
{
	f32 horizontalAngle;
	f32 rotationSpeed = 0.05f;
	f32 horizontalDistance;
	f32 verticalAngle;
	if (mLookatPosPtr) {
		Vector3f targetOffset = *mLookatPosPtr - mSRT.t;
		horizontalAngle       = atan2f(targetOffset.x, targetOffset.z);
		horizontalDistance    = std::sqrtf(targetOffset.x * targetOffset.x + targetOffset.z * targetOffset.z);
		verticalAngle         = atan2f(targetOffset.y, horizontalDistance);
	} else {
		rotationSpeed       = 0.2f;
		f32 diffH           = angDist(0.0f, mHorizontalRotation);
		mHorizontalRotation = roundAng(diffH * rotationSpeed + mHorizontalRotation);
		f32 diffV           = angDist(0.0f, mVerticalRotation);
		mVerticalRotation   = roundAng(diffV * rotationSpeed + mVerticalRotation);
		if (absF(mHorizontalRotation) < 0.1f && absF(mVerticalRotation) < 0.1f) {
			forceFinishLook();
		}
		return;
	}

	f32 currentFacingAngle = roundAng(mHorizontalRotation + mFaceDirection);
	f32 targetAngleDiff    = roundAng(horizontalAngle - mFaceDirection);
	f32 angleAdjustment;

	if (targetAngleDiff < PI) {
		if (mHorizontalRotation > PI) {
			angleAdjustment = TAU - (mHorizontalRotation - targetAngleDiff);
		} else {
			angleAdjustment = angDist(horizontalAngle, currentFacingAngle);
		}
	} else if (mHorizontalRotation <= PI) {
		angleAdjustment = (mHorizontalRotation - targetAngleDiff);
		angleAdjustment = (TAU - angleAdjustment);
		angleAdjustment *= -1.0f;
	} else {
		angleAdjustment = angDist(horizontalAngle, currentFacingAngle);
	}

	if (absF(angleAdjustment) < PI / 20.0f) {
		angleAdjustment = 0.0f;
	}

	mHorizontalRotation = roundAng(angleAdjustment * rotationSpeed + mHorizontalRotation);
	if (mHorizontalRotation > (2.0f * THIRD_PI) && mHorizontalRotation < PI) {
		mHorizontalRotation = (2.0f * THIRD_PI);
	} else if (mHorizontalRotation < (4.0f * THIRD_PI) && mHorizontalRotation >= PI) {
		mHorizontalRotation = (4.0f * THIRD_PI);
	}

	angleAdjustment = angDist(verticalAngle, mVerticalRotation);
	if (absF(angleAdjustment) < PI / 20.0f) {
		angleAdjustment = 0.0f;
	}

	mVerticalRotation = roundAng(angleAdjustment * rotationSpeed + mVerticalRotation);
	if (mVerticalRotation > THIRD_PI && mVerticalRotation < PI) {
		mVerticalRotation = THIRD_PI;
	} else if (mVerticalRotation < (5.0f * THIRD_PI) && mVerticalRotation >= PI) {
		mVerticalRotation = 5.0f * THIRD_PI;
	}

	if (mLookTimer) {
		mLookTimer--;
		if (mLookTimer == 0) {
			forceFinishLook();
		}
	}
}

/**
 * @todo: Documentation
 */
void ViewPiki::demoDraw(Graphics& gfx, immut Matrix4f* mtx)
{
	Vector3f pos;
	if (AIPerf::optLevel <= 2 || mOptUpdateContext.updatable()) {
		pos.set(0.0f, 0.0f, 0.0f);
		mPikiShape->mShape->calcJointWorldPos(gfx, 0, pos);
		mShadowPos = pos;
		pos.set(0.0f, 4.0f, 0.0f);
		mPikiShape->mShape->calcJointWorldPos(gfx, 1, pos);
		mCatchPos = pos;
	}

	STACK_PAD_VAR(1);
	if (mHappa == Bud) {
		pos.set(4.0f, 0.0f, 0.0f);
	} else if (mHappa == Flower) {
		pos.set(3.0f, 0.0f, 0.0f);
	} else {
		pos.set(6.0f, 0.0f, 0.0f);
	}
	mPikiShape->mShape->calcJointWorldPos(gfx, 6, pos);
	mEffectPos = pos;

	if (isDamaged() && gsys->getRand(1.0f) > 0.5f) {
		mPikiShape->mShape->mMaterialList->setColour(COLOUR_WHITE);
	} else {
		mPikiShape->mShape->mMaterialList->setColour(mCurrentColour);
	}

	if (aiCullable()) {
		mPikiShape->mShape->drawshape(gfx, *gfx.mCamera, nullptr);
	}

	if (mIsPanicked) {
		mPanickedEffect->updatePos(mShadowPos);
	}

	if (aiCullable() && AIPerf::optLevel < 3 && mHappaModel) {
		gfx.useMatrix(mPikiShape->mShape->getAnimMatrix(6), 0);
		mHappaModel->drawshape(gfx, *gfx.mCamera, nullptr);
	}

	if (mMode == PikiMode::FormationMode && AIPerf::kandoOnly) {
		gfx.useMatrix(Matrix4f::ident, 0);
		ActCrowd* act = (ActCrowd*)mActiveAction->getCurrAction();
		if (act) {
			Vector3f pos = mSRT.t;
			int slotID   = act->getSlotID();
			pos.y += 2.0f * f32(slotID) + 50.0f;
			bool light = gfx.setLighting(false, nullptr);
			gfx.useMatrix(Matrix4f::ident, 0);
			if (act->mState == ActCrowd::STATE_Sort) {
				gfx.setColour(Colour(255, 10, 50, 255), 1);
			} else if (act->mState == ActCrowd::STATE_Unk0) {
				gfx.setColour(Colour(200, 255, 255, 255), 1);
			} else {
				gfx.setColour(COLOUR_WHITE, 1);
			}

			const char* strs[6] = { "A", "B", "C", "D", "E", "F" };
			char buf[256];
			int formMode = act->mMode;
			int cb       = gfx.setCBlending(BLEND_Alpha);
			pos.multMatrix(gfx.mCamera->mLookAtMtx);
			sprintf(buf, "%s%d", strs[formMode], slotID);
			gfx.perspPrintf(gsys->mConsFont, pos, -(gsys->mConsFont->stringWidth(buf) / 2), 0, buf);
			gfx.setCBlending(cb);
			gfx.setLighting(light, nullptr);
		}
	}

	if (AIPerf::kandoOnly && mActiveAction) {
		if (mActiveAction->getCurrAction()) {
			gfx.useMatrix(Matrix4f::ident, 0);
			mActiveAction->getCurrAction()->draw(gfx);
		}
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000004
 */
static void printMatrix(immut char* name, immut Matrix4f& mat)
{
	// I can't find this function in the DLL at the moment and nothing in this decomp currently uses it.  See also: goalItem.cpp where this
	// implementation is copied from and dynCreature.cpp wherein the function has known uses but the DLL shows an empty implementation.
	PRINT("++++ MATRIX %s ++++\n", name);
	for (int i = 0; i < 4; ++i) {
		PRINT("  %f %f %f %f\n", mat.mMtx[i][0], mat.mMtx[i][1], mat.mMtx[i][2], mat.mMtx[i][3]);
	}
	PRINT("-------------------------\n");
}

/**
 * @todo: Documentation
 */
void ViewPiki::refresh(Graphics& gfx)
{
	STACK_PAD_VAR(2);
	u32 color = mColor;
	if (color == Blue && gameflow.mDemoFlags & CinePlayerFlags::HideBluePiki) {
		return;
	}
	if (color == Red && gameflow.mDemoFlags & CinePlayerFlags::HideRedPiki) {
		return;
	}
	if (color == Yellow && gameflow.mDemoFlags & CinePlayerFlags::HideYellowPiki) {
		return;
	}
	if (mMode == PikiMode::FreeMode && !pikiMgr->isUpdating(PMUPDATE_FreePiki)) {
		return;
	}
	if (mMode == PikiMode::FormationMode && !pikiMgr->isUpdating(PMUPDATE_FormationPiki)) {
		return;
	}
	if (!pikiMgr->isUpdating(PMUPDATE_WorkPiki)) {
		return;
	}

	if (gameflow.mMoviePlayer->mIsActive && pikiMgr->isUpdating(PMUPDATE_Unk4)) {
		Vector3f diff = mSRT.t - itemMgr->getUfo()->getGoalPos();
		if (diff.length() > 100.0f) {
			return;
		}
	}
	Matrix4f mtx;
	f32 size = getSize();
	if (!gfx.mCamera->isPointVisible(mSRT.t, size * 4.0f)) {
		enableAICulling();
	} else {
		disableAICulling();
		if (AIPerf::useLOD) {
			Vector3f diff = gfx.mCamera->mPosition - mSRT.t;
			_528          = diff.length();
		} else {
			_528 = 12800.0f;
		}
	}

	if (getState() == PIKISTATE_Pressed) {
		f32 scaleFactor = pikiMgr->mPikiParms->mPikiParms.mPikiDisplayScale();
		f32 scaleXZ, scaleY;
		f32 norm = pikiMgr->mPikiParms->mPikiParms.mPressDeathFactor() - pikiMgr->mPikiParms->mPikiParms.mNormalPikiPressFactor();
		if (mDeathTimer > pikiMgr->mPikiParms->mPikiParms.mPressDeathFactor() - pikiMgr->mPikiParms->mPikiParms.mNormalPikiPressFactor()) {
			scaleXZ = 2.0f;
			scaleY  = 0.01f;
		} else {
			scaleXZ = (mDeathTimer / norm) + 1.0f;
			scaleY  = (1.0f - (mDeathTimer / norm)) * 0.99f + 0.01f;
			if (scaleY < 0.0f) {
				ERROR("sacle minus!\n");
			}
		}
		mSRT.s.set(scaleFactor * scaleXZ, scaleFactor * scaleY, scaleFactor * scaleXZ);
	}

	if ((mStickTarget && isStickToPlatform()) || mRope || (mStickTarget && mStickPart && mStickPart->isTubeType())) {
		mWorldMtx = mConstrainedMoveMtx;
		for (int i = 0; i < 3; i++) {
			mWorldMtx.mMtx[i][0] *= mSRT.s.x;
			mWorldMtx.mMtx[i][1] *= mSRT.s.y;
			mWorldMtx.mMtx[i][2] *= mSRT.s.z;
		}

		mWorldMtx.setTranslation(mSRT.t.x, mSRT.t.y, mSRT.t.z);
	} else {
		mWorldMtx.makeSRT(mSRT.s, mSRT.r, mSRT.t);
		mConstrainedMoveMtx = mWorldMtx;
	}

	gfx.mCamera->mLookAtMtx.multiplyTo(mWorldMtx, mtx);

	if (getState() == PIKISTATE_Swallowed && mSwallowMouthPart) {
		Matrix4f mouthMtx = mSwallowMouthPart->getJointMatrix();
		f32 swallowScale  = 1.0f / reinterpret_cast<Vector3f&>(mouthMtx).length();
		if (swallowScale <= 0.0f) {
			BUGPRINT("swallow scale=%f", swallowScale);
			swallowScale = 1.0f;
		}

		Matrix4f mtx2;
		mtx2.makeSRT(Vector3f(swallowScale, swallowScale, swallowScale), Vector3f(0.0f, 0.0f, HALF_PI), Vector3f(0.0f, 0.0f, 0.0f));
		mouthMtx.multiplyTo(mtx2, mtx);
		mSRT.t = mSwallowMouthPart->mCentre;
	}

	mPikiAnimMgr.updateContext();
	mPikiShape->mShape->updateAnim(gfx, mtx, nullptr, this);

	if ((AIPerf::useLOD && _528 < 1200.0f && aiCullable()) || !AIPerf::useLOD) {
		bool updateHeadPose = (mLookatPosPtr || mLookTimer) && getState() != PIKISTATE_Swallowed;
#if !defined(VERSION_PIKIDEMO)
		updateHeadPose = updateHeadPose && mMode != PikiMode::ExitMode;
#endif
#if defined(PIKI_PC_PORT)
		updateHeadPose = updateHeadPose && pc_render_is_authoritative();
#endif
		if (updateHeadPose)
		{
			updateLook();

			Matrix4f animMtx1 = mPikiShape->mShape->getAnimMatrix(2);
#if defined(VERSION_PIKIDEMO)
#else
			animMtx1.mMtx[3][0] = animMtx1.mMtx[3][1] = animMtx1.mMtx[3][2] = 0.0f;
			animMtx1.mMtx[3][3]                                             = 1.0f;
#endif

			Matrix4f mtx2;
			Matrix4f mtx3;
			animMtx1.inverse(&mtx2);
			Matrix4f mtx4;
			mtx4.makeIdentity();

			f32 sinV = sinf(-mVerticalRotation);
			f32 cosV = cosf(-mVerticalRotation);
			f32 sinH = sinf(mHorizontalRotation);
			f32 cosH = cosf(mHorizontalRotation);

			mtx4.mMtx[0][0] = cosV;
			mtx4.mMtx[0][1] = -sinV;
			mtx4.mMtx[0][2] = 0.0f;

			mtx4.mMtx[1][0] = cosH * sinV;
			mtx4.mMtx[1][1] = cosH * cosV;
			mtx4.mMtx[1][2] = -sinH;

			mtx4.mMtx[2][0] = sinH * sinV;
			mtx4.mMtx[2][1] = sinH * cosV;
			mtx4.mMtx[2][2] = cosH;

			animMtx1.multiplyTo(mtx4, mtx3);

			mPikiShape->mShape->getAnimMatrix(2) = mtx3;

			for (int j = 3; j <= 6; j++) {
				Matrix4f animMtx = mPikiShape->mShape->getAnimMatrix(j);
#if defined(VERSION_PIKIDEMO)
#else
				animMtx.mMtx[3][0] = animMtx.mMtx[3][1] = animMtx.mMtx[3][2] = 0.0f;
				animMtx.mMtx[3][3]                                           = 1.0f;
#endif

				Matrix4f tmp1;
				Matrix4f tmp2;
				mtx2.multiplyTo(animMtx, tmp1);
				mtx3.multiplyTo(tmp1, tmp2);
				mPikiShape->mShape->getAnimMatrix(j) = tmp2;
			}

#if defined(__vita__) && defined(__ARM_NEON)
			// Only joints 2..6 changed.
			pcCalcWeightedMatricesDirty(mPikiShape->mShape, (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6));
#else
			mPikiShape->mShape->calcWeightedMatrices();
#endif
		}
	}

	mCollInfo->updateInfo(gfx, false);
	demoDraw(gfx, nullptr);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00001C (Matching by size)
 */
void ViewPiki::refresh2d(Graphics&, int)
{
	mLifeGauge.mPosition = mSRT.t;
}

#if PC_PIKIA
#include "PlantMgr.h"
#include "pc_photo_mode.h"
#include "vita_drawrec.h"
#include "timing/pc_prof.h"
#include "Material.h"
#include "Mesh.h"
#include "PVW.h"
#include "Texture.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <malloc.h>

// Pikmin A-phase worker.

// One Pikmin's B inputs.
enum {
	kPktDrawBody = 1 << 0, // body drawn
	kPktDamaged  = 1 << 1, // damage flash
	kPktAnimCtx  = 1 << 3, // has animation context
	kPktAnimData = 1 << 4, // has animation data
	kPktHoisted  = 1 << 5, // plant record with hoisted cacheShape
};
struct __attribute__((aligned(32))) PcPikiDrawPacket {
	Shape* mBody;       // per-colour shape
	Matrix4f* mPose;    // pose
	Shape* mHappa;      // happa, or null
	Colour mColour;
	Colour mDrawColour; // resolved draw colour
	u8 mFlags;          // kPkt*
};
static_assert(sizeof(PcPikiDrawPacket) == 32, "a packet is one A9 cache line");

// One plant's B inputs.
struct __attribute__((aligned(32))) PcPlantDrawPacket {
	Shape* mShape;
	Matrix4f* mPose; // pose
	u8 mFlags;
};

// The run in flight.
struct PcSplitJob {
	void (*mWorkerBody)();
	Graphics* mGfx;
	int mCount;            // item count
	int mStart;            // first worker item
	int mBail;             // first item handed back, or -1
};
static PcSplitJob sPkJob;

// Lane inputs.
static Creature** sPkList = nullptr;
static int* sPkStatus     = nullptr;
static Plant** sPlItems   = nullptr;
static int sPlItemCap     = 0;

// Shared atomics.
struct __attribute__((aligned(32))) PcPikiAtomic {
	std::atomic<int> v;
};
enum { kPkIdle = 0, kPkRunning = 1, kPkDone = 2 };
// sPkHand values.
enum {
	kHandGame        = 0,
	kHandAsked       = 1,
	kHandAskedAsleep = 2,
	kHandGiven       = 3,
	kHandKept        = 4,
};
static PcPikiAtomic sPkPublished;   // packets published
static PcPikiAtomic sPkState;       // kPk* state
static PcPikiAtomic sPkMainWaiting;
static PcPikiAtomic sPkHand;        // kHand*

static PcPikiDrawPacket* sPkPackets  = nullptr;
static int sPkCapacity               = 0;
static PcPlantDrawPacket* sPlPackets = nullptr;
static int sPlCapacity               = 0;
static PcPikiDrawPacket sPkOwn;
static PcPlantDrawPacket sPlOwn;

static const int kPkPrio         = 0xB1;
static const unsigned kPkSpinUs  = 300;
static const int kPkMinItems     = 4;
static const int kPkWarmupFrames = 30;

static SceUID sPkThread = -1, sPkGo = -1, sPkPkt = -1, sPkHandSema = -1;
static int sPkThreadState   = 0;  // 0 not tried, 1 running, -1 failed
static SceUID sPkForkThread = -1;
// Worker stack bounds.
static volatile uintptr_t sPkStackLo = 0, sPkStackHi = 0;
static std::atomic<int> sPkStackReady { 0 };

// Per-lane warmup state.
struct PcSplitLane {
	int mWarmup;
};
static PcSplitLane sPkLane;
static PcSplitLane sPlLane;

static inline SceUInt64 pcPkNow()
{
	return sceKernelGetProcessTimeWide();
}

// Whether the calling thread is the worker.
extern "C" int vita_pikia_on_worker(void)
{
	if (!sPkStackReady.load(std::memory_order_acquire)) {
		return 0;
	}
	uintptr_t sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	return sp - sPkStackLo < sPkStackHi - sPkStackLo;
}

// Publishes the worker's stack range.
extern "C" void vita_offlock_register(uintptr_t lo, uintptr_t hi);
static bool pcPikiPublishStack(const SceKernelThreadInfo& info)
{
	if (!info.stack || info.stackSize <= 0) {
		return false;
	}
	sPkStackLo = uintptr_t(info.stack);
	sPkStackHi = uintptr_t(info.stack) + uintptr_t(info.stackSize);
	vita_offlock_register(sPkStackLo, sPkStackHi);
	sPkStackReady.store(1, std::memory_order_release);
	return true;
}

// Whether this Pikmin's A would hit an ERROR.
static __attribute__((noinline)) bool pcPikiAWouldError(ViewPiki* piki, Graphics& gfx)
{
	if (piki->getState() == PIKISTATE_Pressed) {
		f32 norm = pikiMgr->mPikiParms->mPikiParms.mPressDeathFactor() - pikiMgr->mPikiParms->mPikiParms.mNormalPikiPressFactor();
		if (!(piki->mDeathTimer
		      > pikiMgr->mPikiParms->mPikiParms.mPressDeathFactor() - pikiMgr->mPikiParms->mPikiParms.mNormalPikiPressFactor())) {
			f32 scaleY = (1.0f - (piki->mDeathTimer / norm)) * 0.99f + 0.01f;
			if (scaleY < 0.0f) {
				return true;
			}
		}
	}

	BaseShape* shape = piki->mPikiShape->mShape;
	if (gfx.mNextFreeMatrixIdx + int(shape->mAnimMtxCount) > gfx.mMaxMatrixCount) {
		return true;
	}
	if (!shape->mCurrentAnimation->mData) {
		return false;
	}

	const PaniPikiAnimator& upper = piki->mPikiAnimMgr.mUpperAnimator;
	const PaniPikiAnimator& lower = piki->mPikiAnimMgr.mLowerAnimator;
	for (int i = 0; i < shape->mJointCount; i++) {
		AnimContext* ctx = shape->mAnimOverrides[i];
		AnimData* data   = ctx->mData;
		f32 frame        = ctx->mCurrentFrame;
		if (upper.mAnimInfo && upper.mContext == ctx) {
			data  = upper.mAnimInfo->mData;
			frame = upper.mAnimationCounter;
		}
		if (lower.mAnimInfo && lower.mContext == ctx) {
			data  = lower.mAnimInfo->mData;
			frame = lower.mAnimationCounter;
		}
		if (!data) {
			return true;
		}
		const int frameNum = int(frame);
		if (shape->mFrameCacher && (data->mAnimFlags & ANIMFLAG_UseCache) && (frameNum < 0 || frameNum >= data->mTotalFrameCount)) {
			return true;
		}
		if (data->mTotalFrameCount && (frameNum < 0 || frameNum >= data->mTotalFrameCount)) {
			return true;
		}
	}
	return false;
}

// A: the simulation half of ViewPiki::refresh and demoDraw.
static __attribute__((noinline)) bool pcPikiRefreshA(ViewPiki* piki, Graphics& gfx, PcPikiDrawPacket& pkt)
{
	u32 color = piki->mColor;
	if (color == Blue && gameflow.mDemoFlags & CinePlayerFlags::HideBluePiki) {
		return false;
	}
	if (color == Red && gameflow.mDemoFlags & CinePlayerFlags::HideRedPiki) {
		return false;
	}
	if (color == Yellow && gameflow.mDemoFlags & CinePlayerFlags::HideYellowPiki) {
		return false;
	}
	if (piki->mMode == PikiMode::FreeMode && !pikiMgr->isUpdating(PMUPDATE_FreePiki)) {
		return false;
	}
	if (piki->mMode == PikiMode::FormationMode && !pikiMgr->isUpdating(PMUPDATE_FormationPiki)) {
		return false;
	}
	if (!pikiMgr->isUpdating(PMUPDATE_WorkPiki)) {
		return false;
	}

	if (gameflow.mMoviePlayer->mIsActive && pikiMgr->isUpdating(PMUPDATE_Unk4)) {
		Vector3f diff = piki->mSRT.t - itemMgr->getUfo()->getGoalPos();
		if (diff.length() > 100.0f) {
			return false;
		}
	}
	Matrix4f mtx;
	f32 size = piki->getSize();
	if (!gfx.mCamera->isPointVisible(piki->mSRT.t, size * 4.0f)) {
		piki->enableAICulling();
	} else {
		piki->disableAICulling();
		if (AIPerf::useLOD) {
			Vector3f diff = gfx.mCamera->mPosition - piki->mSRT.t;
			piki->_528    = diff.length();
		} else {
			piki->_528 = 12800.0f;
		}
	}

	if (piki->getState() == PIKISTATE_Pressed) {
		f32 scaleFactor = pikiMgr->mPikiParms->mPikiParms.mPikiDisplayScale();
		f32 scaleXZ, scaleY;
		f32 norm = pikiMgr->mPikiParms->mPikiParms.mPressDeathFactor() - pikiMgr->mPikiParms->mPikiParms.mNormalPikiPressFactor();
		if (piki->mDeathTimer > pikiMgr->mPikiParms->mPikiParms.mPressDeathFactor() - pikiMgr->mPikiParms->mPikiParms.mNormalPikiPressFactor()) {
			scaleXZ = 2.0f;
			scaleY  = 0.01f;
		} else {
			scaleXZ = (piki->mDeathTimer / norm) + 1.0f;
			scaleY  = (1.0f - (piki->mDeathTimer / norm)) * 0.99f + 0.01f;
			if (scaleY < 0.0f) {
				ERROR("sacle minus!\n");
			}
		}
		piki->mSRT.s.set(scaleFactor * scaleXZ, scaleFactor * scaleY, scaleFactor * scaleXZ);
	}

	if ((piki->mStickTarget && piki->isStickToPlatform()) || piki->mRope
	    || (piki->mStickTarget && piki->mStickPart && piki->mStickPart->isTubeType())) {
		piki->mWorldMtx = piki->mConstrainedMoveMtx;
		for (int i = 0; i < 3; i++) {
			piki->mWorldMtx.mMtx[i][0] *= piki->mSRT.s.x;
			piki->mWorldMtx.mMtx[i][1] *= piki->mSRT.s.y;
			piki->mWorldMtx.mMtx[i][2] *= piki->mSRT.s.z;
		}

		piki->mWorldMtx.setTranslation(piki->mSRT.t.x, piki->mSRT.t.y, piki->mSRT.t.z);
	} else {
		piki->mWorldMtx.makeSRT(piki->mSRT.s, piki->mSRT.r, piki->mSRT.t);
		piki->mConstrainedMoveMtx = piki->mWorldMtx;
	}

	gfx.mCamera->mLookAtMtx.multiplyTo(piki->mWorldMtx, mtx);

	if (piki->getState() == PIKISTATE_Swallowed && piki->mSwallowMouthPart) {
		Matrix4f mouthMtx = piki->mSwallowMouthPart->getJointMatrix();
		f32 swallowScale  = 1.0f / reinterpret_cast<Vector3f&>(mouthMtx).length();
		if (swallowScale <= 0.0f) {
			BUGPRINT("swallow scale=%f", swallowScale);
			swallowScale = 1.0f;
		}

		Matrix4f mtx2;
		mtx2.makeSRT(Vector3f(swallowScale, swallowScale, swallowScale), Vector3f(0.0f, 0.0f, HALF_PI), Vector3f(0.0f, 0.0f, 0.0f));
		mouthMtx.multiplyTo(mtx2, mtx);
		piki->mSRT.t = piki->mSwallowMouthPart->mCentre;
	}

	piki->mPikiAnimMgr.updateContext();
	piki->mPikiShape->mShape->updateAnim(gfx, mtx, nullptr, piki);

	if ((AIPerf::useLOD && piki->_528 < 1200.0f && piki->aiCullable()) || !AIPerf::useLOD) {
		bool updateHeadPose = (piki->mLookatPosPtr || piki->mLookTimer) && piki->getState() != PIKISTATE_Swallowed;
#if !defined(VERSION_PIKIDEMO)
		updateHeadPose = updateHeadPose && piki->mMode != PikiMode::ExitMode;
#endif
		updateHeadPose = updateHeadPose && pc_render_is_authoritative();
		if (updateHeadPose) {
			piki->updateLook();

			Matrix4f animMtx1 = piki->mPikiShape->mShape->getAnimMatrix(2);
#if defined(VERSION_PIKIDEMO)
#else
			animMtx1.mMtx[3][0] = animMtx1.mMtx[3][1] = animMtx1.mMtx[3][2] = 0.0f;
			animMtx1.mMtx[3][3]                                             = 1.0f;
#endif

			Matrix4f mtx2;
			Matrix4f mtx3;
			animMtx1.inverse(&mtx2);
			Matrix4f mtx4;
			mtx4.makeIdentity();

			f32 sinV = sinf(-piki->mVerticalRotation);
			f32 cosV = cosf(-piki->mVerticalRotation);
			f32 sinH = sinf(piki->mHorizontalRotation);
			f32 cosH = cosf(piki->mHorizontalRotation);

			mtx4.mMtx[0][0] = cosV;
			mtx4.mMtx[0][1] = -sinV;
			mtx4.mMtx[0][2] = 0.0f;

			mtx4.mMtx[1][0] = cosH * sinV;
			mtx4.mMtx[1][1] = cosH * cosV;
			mtx4.mMtx[1][2] = -sinH;

			mtx4.mMtx[2][0] = sinH * sinV;
			mtx4.mMtx[2][1] = sinH * cosV;
			mtx4.mMtx[2][2] = cosH;

			animMtx1.multiplyTo(mtx4, mtx3);

			piki->mPikiShape->mShape->getAnimMatrix(2) = mtx3;

			for (int j = 3; j <= 6; j++) {
				Matrix4f animMtx = piki->mPikiShape->mShape->getAnimMatrix(j);
#if defined(VERSION_PIKIDEMO)
#else
				animMtx.mMtx[3][0] = animMtx.mMtx[3][1] = animMtx.mMtx[3][2] = 0.0f;
				animMtx.mMtx[3][3]                                           = 1.0f;
#endif

				Matrix4f tmp1;
				Matrix4f tmp2;
				mtx2.multiplyTo(animMtx, tmp1);
				mtx3.multiplyTo(tmp1, tmp2);
				piki->mPikiShape->mShape->getAnimMatrix(j) = tmp2;
			}

#if defined(__ARM_NEON)
			pcCalcWeightedMatricesDirty(piki->mPikiShape->mShape, (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6));
#else
			piki->mPikiShape->mShape->calcWeightedMatrices();
#endif
		}
	}

	piki->mCollInfo->updateInfo(gfx, false);

	Vector3f pos;
	if (AIPerf::optLevel <= 2 || piki->mOptUpdateContext.updatable()) {
		pos.set(0.0f, 0.0f, 0.0f);
		piki->mPikiShape->mShape->calcJointWorldPos(gfx, 0, pos);
		piki->mShadowPos = pos;
		pos.set(0.0f, 4.0f, 0.0f);
		piki->mPikiShape->mShape->calcJointWorldPos(gfx, 1, pos);
		piki->mCatchPos = pos;
	}

	if (piki->mHappa == Bud) {
		pos.set(4.0f, 0.0f, 0.0f);
	} else if (piki->mHappa == Flower) {
		pos.set(3.0f, 0.0f, 0.0f);
	} else {
		pos.set(6.0f, 0.0f, 0.0f);
	}
	piki->mPikiShape->mShape->calcJointWorldPos(gfx, 6, pos);
	piki->mEffectPos = pos;

	if (piki->mIsPanicked) {
		piki->mPanickedEffect->updatePos(piki->mShadowPos);
	}

	const bool cullable   = piki->aiCullable();
	pkt.mBody             = piki->mPikiShape->mShape;
	pkt.mPose             = piki->mPikiShape->mShape->mAnimMatrices;
	pkt.mHappa            = (cullable && AIPerf::optLevel < 3 && piki->mHappaModel) ? piki->mHappaModel : nullptr;
	pkt.mColour           = piki->mCurrentColour;
	BaseShape* const body = pkt.mBody;
	pkt.mFlags            = u8((cullable ? kPktDrawBody : 0) | (piki->isDamaged() ? kPktDamaged : 0)
                    | (body->mCurrentAnimation ? kPktAnimCtx : 0)
                    | ((body->mCurrentAnimation && body->mCurrentAnimation->mData) ? kPktAnimData : 0));
	return true;
}

// B: the GX half of demoDraw.
static __attribute__((noinline)) void pcPikiDrawB(Graphics& gfx, const PcPikiDrawPacket& pkt)
{
	if ((pkt.mFlags & kPktDamaged) && gsys->getRand(1.0f) > 0.5f) {
		pkt.mBody->mMaterialList->setColour(COLOUR_WHITE);
	} else {
		pkt.mBody->mMaterialList->setColour(pkt.mColour);
	}

	if (pkt.mFlags & kPktDrawBody) {
		gfx.mPcPose          = pkt.mPose;
		gfx.mPcPoseShape     = pkt.mBody;
		gfx.mPcPoseAnimCtx   = (pkt.mFlags & kPktAnimCtx) ? 1 : 0;
		gfx.mPcPoseAnimData  = (pkt.mFlags & kPktAnimData) ? 1 : 0;
		pkt.mBody->drawshape(gfx, *gfx.mCamera, nullptr);
		gfx.mPcPose          = nullptr;
		gfx.mPcPoseShape     = nullptr;
	}

	if (pkt.mHappa) {
		gfx.useMatrix(pkt.mPose[6], 0);
		pkt.mHappa->drawshape(gfx, *gfx.mCamera, nullptr);
	}
}

// Plant A/B flip.

// Lanes.

struct PcPikiLane {
	typedef PcPikiDrawPacket Packet;
	static PcSplitLane& lane() { return sPkLane; }
	static Packet* packets() { return sPkPackets; }
	static Packet& own() { return sPkOwn; }
	static bool live(int i) { return sPkStatus[i] == 0; }
	static bool wouldError(int i, Graphics& gfx) { return pcPikiAWouldError(static_cast<ViewPiki*>(sPkList[i]), gfx); }
	static bool runA(int i, Graphics& gfx, Packet& pkt) { return pcPikiRefreshA(static_cast<ViewPiki*>(sPkList[i]), gfx, pkt); }
	static void drawB(Graphics& gfx, const Packet& pkt) { pcPikiDrawB(gfx, pkt); }
	static void serial(int i, Graphics& gfx) { sPkList[i]->refresh(gfx); }
};

struct PcPlantLane {
	typedef PcPlantDrawPacket Packet;
	static PcSplitLane& lane() { return sPlLane; }
	static Packet* packets() { return sPlPackets; }
	static Packet& own() { return sPlOwn; }
	static bool live(int) { return true; }
	static bool wouldError(int i, Graphics& gfx) { return sPlItems[i]->pcWouldError(gfx); }
	static bool runA(int i, Graphics& gfx, Packet& pkt)
	{
		Plant* plant = sPlItems[i];
		Shape* shape = nullptr;
		if (!plant->pcRefreshA(gfx, shape)) {
			return false;
		}
		pkt.mShape = shape;
		pkt.mPose  = shape->mAnimMatrices;
		pkt.mFlags = u8((shape->mCurrentAnimation ? kPktAnimCtx : 0)
		                | ((shape->mCurrentAnimation && shape->mCurrentAnimation->mData) ? kPktAnimData : 0));
		return true;
	}
	static void drawB(Graphics& gfx, const Packet& pkt)
	{
		gfx.useMatrix(Matrix4f::ident, 0);
		gfx.mPcPose         = pkt.mPose;
		gfx.mPcPoseShape    = pkt.mShape;
		gfx.mPcPoseAnimCtx  = (pkt.mFlags & kPktAnimCtx) ? 1 : 0;
		gfx.mPcPoseAnimData = (pkt.mFlags & kPktAnimData) ? 1 : 0;
		pkt.mShape->drawshape(gfx, *gfx.mCamera, nullptr);
		gfx.mPcPose         = nullptr;
		gfx.mPcPoseShape    = nullptr;
	}
	static void serial(int i, Graphics& gfx) { sPlItems[i]->refresh(gfx); }
};

// Machinery.

// Wakes the game thread. Worker only.
static inline void pcSplitWakeGame()
{
	if (sPkMainWaiting.v.load(std::memory_order_seq_cst) && sPkMainWaiting.v.exchange(0, std::memory_order_seq_cst) == 1) {
		sceKernelSignalSema(sPkPkt, 1);
	}
}

// Publishes packets [0, n). Worker only.
static inline void pcSplitPublish(int n)
{
	sPkPublished.v.store(n, std::memory_order_seq_cst);
	pcSplitWakeGame();
}

// Worker A loop.
template <class L>
static void pcSplitARun(int start)
{
	Graphics& gfx = *sPkJob.mGfx;
	int n         = 0;
	int bail      = -1;
	for (int i = start; i < sPkJob.mCount; i++) {
		if (!L::live(i)) {
			continue;
		}
		if (L::wouldError(i, gfx)) {
			bail = i;
			break;
		}
		typename L::Packet& pkt = L::packets()[n];
		if (L::runA(i, gfx, pkt)) {
			++n;
			pcSplitPublish(n);
		}
	}
	sPkJob.mBail = bail;
}

// Worker hand-over claim.
static bool pcSplitWorkerClaim(SceUInt64 t0)
{
	int v = kHandGame;
	if (!sPkHand.v.compare_exchange_strong(v, kHandAsked, std::memory_order_seq_cst)) {
		return false;
	}
	for (unsigned n = 1;; n++) {
		v = sPkHand.v.load(std::memory_order_acquire);
		if (v != kHandAsked) {
			break;
		}
		if ((n & 63u) == 0 && unsigned(pcPkNow() - t0) > kPkSpinUs) {
			int e = kHandAsked;
			if (sPkHand.v.compare_exchange_strong(e, kHandAskedAsleep, std::memory_order_seq_cst)) {
				const int pvIdle = pc_prof_idle_set(1);
				sceKernelWaitSema(sPkHandSema, 1, NULL);
				pc_prof_idle_set(pvIdle);
				v = sPkHand.v.load(std::memory_order_acquire);
			} else {
				v = e;
			}
			break;
		}
		__asm__ volatile("yield" ::: "memory");
	}
	return v == kHandGiven;
}

template <class L>
static void pcSplitWorkerBody()
{
	if (!pcSplitWorkerClaim(pcPkNow())) {
		return;
	}
	pcSplitARun<L>(sPkJob.mStart);
}

static int pcSplitThreadMain(SceSize, void*)
{
	if (!sPkStackReady.load(std::memory_order_acquire)) {
		SceKernelThreadInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0) {
			pcPikiPublishStack(info);
		}
	}
	pc_prof_register(2);
	for (;;) {
		pc_prof_idle_set(1);
		sceKernelWaitSema(sPkGo, 1, NULL);
		pc_prof_idle_set(0);
		sPkJob.mWorkerBody();
		sPkState.v.store(kPkDone, std::memory_order_seq_cst);
		pcSplitWakeGame();
	}
	return 0;
}

// Worker thread creation.
static bool pcSplitThreadReady()
{
	if (sPkThreadState != 0) {
		return sPkThreadState > 0;
	}
	sPkThreadState = -1;
	sPkGo          = sceKernelCreateSema("nectar_pikiA_go", 0, 0, 1, NULL);
	sPkPkt         = sceKernelCreateSema("nectar_pikiA_pkt", 0, 0, 8, NULL);
	sPkHandSema    = sceKernelCreateSema("nectar_pikiA_hand", 0, 0, 1, NULL);
	sPkThread = sceKernelCreateThread("nectar_pikiA", pcSplitThreadMain, kPkPrio, 64 * 1024, 0,
	                                  SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL);
	if (sPkGo < 0 || sPkPkt < 0 || sPkHandSema < 0 || sPkThread < 0) {
		printf("[VITA PIKIA] ERROR: the worker could not be created (0x%08X 0x%08X 0x%08X 0x%08X): the Pikmin and plant draws "
		       "stay on the original path\n",
		       unsigned(sPkGo), unsigned(sPkPkt), unsigned(sPkHandSema), unsigned(sPkThread));
		return false;
	}
	{
		SceKernelThreadInfo info;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetThreadInfo(sPkThread, &info) >= 0) {
			pcPikiPublishStack(info);
		}
	}
	sceKernelStartThread(sPkThread, 0, NULL);
	for (int i = 0; i < 2000 && !sPkStackReady.load(std::memory_order_acquire); ++i) {
		sceKernelDelayThread(100);
	}
	if (!sPkStackReady.load(std::memory_order_acquire)) {
		printf("[VITA PIKIA] ERROR: the worker could not report its stack: the Pikmin and plant draws stay on the original path\n");
		return false;
	}
	sPkThreadState = 1;
	return true;
}

// Unbounded wait on a semaphore.
static void pcSplitWaitSema(SceUID sema)
{
	bool raised = false;
	for (;;) {
		SceUInt timeout = 250000;
		if (sceKernelWaitSema(sema, 1, &timeout) >= 0) {
			if (raised) {
				sceKernelChangeThreadPriority(sPkThread, kPkPrio);
			}
			return;
		}
		if (!raised) {
			raised = true;
			sceKernelChangeThreadPriority(sPkThread, sceKernelGetThreadCurrentPriority());
		}
	}
}

// Waits until ready() holds.
template <class Ready>
static void pcSplitWaitUntil(Ready ready)
{
	for (int spin = 0; spin < 64; spin++) {
		if (ready()) {
			return;
		}
	}
	sPkMainWaiting.v.store(1, std::memory_order_seq_cst);
	if (ready()) {
		if (sPkMainWaiting.v.exchange(0, std::memory_order_seq_cst) == 0) {
			pcSplitWaitSema(sPkPkt);
		}
		return;
	}
	pcSplitWaitSema(sPkPkt);
}

// Hands the lane over from item `next`.
static inline bool pcSplitOffer(int next)
{
	int v = sPkHand.v.load(std::memory_order_acquire);
	while (v == kHandAsked || v == kHandAskedAsleep) {
		sPkJob.mStart = next;
		if (sPkHand.v.compare_exchange_weak(v, kHandGiven, std::memory_order_seq_cst, std::memory_order_acquire)) {
			if (v == kHandAskedAsleep) {
				sceKernelSignalSema(sPkHandSema, 1);
			}
			return true;
		}
	}
	return false;
}

// Keeps the lane on the game thread.
static bool pcSplitKeep()
{
	int v = sPkHand.v.load(std::memory_order_acquire);
	while (v == kHandGame || v == kHandAsked || v == kHandAskedAsleep) {
		if (sPkHand.v.compare_exchange_weak(v, kHandKept, std::memory_order_seq_cst, std::memory_order_acquire)) {
			if (v == kHandAskedAsleep) {
				sceKernelSignalSema(sPkHandSema, 1);
			}
			return false;
		}
	}
	return v == kHandGiven;
}

static void pcSplitWaitDone()
{
	while (sPkState.v.load(std::memory_order_acquire) == kPkRunning) {
		pcSplitWaitUntil([] { return sPkState.v.load(std::memory_order_seq_cst) != kPkRunning; });
	}
	sPkState.v.store(kPkIdle, std::memory_order_release);
}

// Joins the run in flight.
extern "C" void vita_pikia_join(void)
{
	if (sPkState.v.load(std::memory_order_acquire) == kPkIdle) {
		return;
	}
	if (sPkHand.v.load(std::memory_order_acquire) == kHandKept) {
		return;
	}
	if (sceKernelGetThreadId() != sPkForkThread) {
		return;
	}
	if (!pcSplitKeep()) {
		return;
	}
	pcSplitWaitDone();
}

// Collects the previous run's worker side.
static void pcSplitCollect()
{
	if (sPkState.v.load(std::memory_order_acquire) != kPkIdle) {
		pcSplitKeep();
		pcSplitWaitDone();
	}
}

// Runs the remaining items the original way.
template <class L>
static void pcSplitSerialFrom(Graphics& gfx, int count, int from)
{
	for (int i = from; i < count; i++) {
		if (L::live(i)) {
			L::serial(i, gfx);
		}
	}
}

static void pcSplitSetJob(Graphics& gfx, int count, void (*body)())
{
	sPkJob.mWorkerBody = body;
	sPkJob.mGfx        = &gfx;
	sPkJob.mCount      = count;
	sPkJob.mStart      = 0;
	sPkJob.mBail       = -1;
	sPkPublished.v.store(0, std::memory_order_relaxed);
	sPkMainWaiting.v.store(0, std::memory_order_relaxed);
	sPkHand.v.store(kHandGame, std::memory_order_relaxed);
}

// Runs a lane split between the game thread and the worker.
template <class L>
static void pcSplitRunThreaded(Graphics& gfx, int count, int live)
{
	pcSplitSetJob(gfx, count, &pcSplitWorkerBody<L>);
	sPkForkThread = sceKernelGetThreadId();
	sPkState.v.store(kPkRunning, std::memory_order_release);
	sceKernelSignalSema(sPkGo, 1);

	typename L::Packet& own = L::own();
	bool given              = false;
	int seen                = 0;
	for (int i = 0; i < count && !given; i++) {
		if (!L::live(i)) {
			continue;
		}
		++seen;
		const bool drawn = L::runA(i, gfx, own);
		if (drawn && seen < live) {
			given = pcSplitOffer(i + 1);
		}
		if (drawn) {
			L::drawB(gfx, own);
		}
	}

	if (!given) {
		pcSplitKeep();
		return;
	}

	int consumed = 0;
	for (;;) {
		if (consumed < sPkPublished.v.load(std::memory_order_acquire)) {
			L::drawB(gfx, L::packets()[consumed]);
			++consumed;
			continue;
		}
		if (sPkState.v.load(std::memory_order_acquire) != kPkRunning) {
			if (consumed < sPkPublished.v.load(std::memory_order_acquire)) {
				continue;
			}
			break;
		}
		pcSplitWaitUntil([consumed] {
			return sPkPublished.v.load(std::memory_order_seq_cst) > consumed || sPkState.v.load(std::memory_order_seq_cst) != kPkRunning;
		});
	}
	vita_pikia_join();
	if (sPkJob.mBail >= 0) {
		pcSplitSerialFrom<L>(gfx, count, sPkJob.mBail);
	}
}

// Whether this frame's lane may go to the worker.
template <class L>
static bool pcSplitWorkerOk(int count, int live, bool shapesOk, bool (*ensureCapacity)(int))
{
	PcSplitLane& lane = L::lane();
	if (gameflow.mMoviePlayer->mIsActive) {
		return false;
	}
	if (AIPerf::kandoOnly || gsys->mToggleDebugInfo || pc_photo_mode_active() || !shapesOk || live < kPkMinItems) {
		return false;
	}
	if (!ensureCapacity(count)) {
		return false;
	}
	if (lane.mWarmup < kPkWarmupFrames) {
		++lane.mWarmup;
		return false;
	}
	return pcSplitThreadReady();
}

// Grows the packet arrays.
static bool pcPikiAEnsureCapacity(int count)
{
	if (count <= sPkCapacity) {
		return true;
	}
	void* mem = memalign(32, size_t(count) * sizeof(PcPikiDrawPacket));
	if (!mem) {
		return false;
	}
	free(sPkPackets);
	sPkPackets  = static_cast<PcPikiDrawPacket*>(mem);
	sPkCapacity = count;
	return true;
}

static bool pcPlantEnsureCapacity(int count)
{
	if (count <= sPlCapacity) {
		return true;
	}
	void* mem = memalign(32, size_t(count) * sizeof(PcPlantDrawPacket));
	if (!mem) {
		return false;
	}
	free(sPlPackets);
	sPlPackets  = static_cast<PcPlantDrawPacket*>(mem);
	sPlCapacity = count;
	return true;
}

// Counts collision parts with an updater.
int pcSplitCountPartUpdaters(CollInfo* info)
{
	if (!info || !info->hasInfo()) {
		return 0;
	}
	int updaters = 0;
	CollPart* stack[64];
	int depth      = 0;
	stack[depth++] = info->getBoundingSphere();
	for (int visited = 0; depth > 0 && visited < 256; visited++) {
		CollPart* part = stack[--depth];
		if (part->mPartUpdater) {
			++updaters;
		}
		CollPart* next  = part->getNext();
		CollPart* child = part->getChild();
		if (next && depth < 64) {
			stack[depth++] = next;
		}
		if (child && depth < 64) {
			stack[depth++] = child;
		}
	}
	return updaters;
}

// Drawrec eligibility.
extern const void* g_pcPlainTextureVtbl;
struct PcDrElig {
	bool mEligible;
	const char* mWhy;
	PcDrSets mSets;
};
static PcDrElig sDrElig;
static inline bool pcDrPlainTexture(const Texture* tex)
{
	return !tex || (g_pcPlainTextureVtbl && *reinterpret_cast<const void* const*>(tex) == g_pcPlainTextureVtbl);
}
static void pcPikiDrawrecEligibility(BaseShape* const* shapes, int count)
{
	memset(&sDrElig, 0, sizeof(sDrElig));
	const char* reason = nullptr;
	for (int s = 0; s < count; s++) {
		BaseShape* shape = shapes[s];
		if (!shape) {
			continue;
		}
		const bool body = s < 4;
		int hidden      = 0;
		for (int m = 0; m < shape->mMeshCount; m++) {
			const Mesh& mesh = shape->mMeshList[m];
			if (mesh.mJointList && !JOINT_IS_VISIBLE(mesh.mJointList[0])) {
				++hidden;
			}
		}
		const bool animated = shape->mCurrentAnimation && shape->mCurrentAnimation->mData;
		const bool caching  = (shape->mShapeFlags & ShapeFlags::AllowCaching) != 0;
		const bool redraw   = (shape->mShapeFlags & ShapeFlags::AlwaysRedraw) != 0;
		const bool lights   = shape->mLightGroup.Child() != nullptr;
		if (!reason && lights) {
			reason = "a light group on a segment shape (drawlights)";
		}
		if (!reason && caching && !redraw) {
			reason = "a cached segment shape (its alpha pass is flushed by flushCachedShapes: a join)";
		}
		if (!reason && body && !animated) {
			reason = "a body without animation data (the pose override does not cover the joint branch)";
		}
		if (!reason && hidden) {
			reason = "a hidden mesh joint on a segment shape (its visibility is not frozen per record)";
		}
		for (int m = 0; m < shape->mMaterialCount && !reason; m++) {
			const Material& mat = shape->mMaterialList[m];
			bool plain          = pcDrPlainTexture(mat.mTexture) && pcDrPlainTexture(mat.mEnvMapTexture);
			for (u32 k = 0; k < mat.mTextureInfo.mTextureDataCount; k++) {
				plain = plain && pcDrPlainTexture(mat.mTextureInfo.mTextureData[k].mTexture);
			}
			if (!(mat.mFlags & MATFLAG_PVW)) {
				reason = "a non-PVW material (useTexture, the environment-map path)";
			} else if (mat.mDisplayListPtr != nullptr) {
				reason = "a register display-list material (it can write state the fence keeps)";
			} else if (mat.mTextureInfo.mUseScale != 0) {
				reason = "a scaled texture (useMatrixQuick's custom-scale normal matrix)";
			} else if (mat.mEnvMapTexture != nullptr) {
				reason = "an environment map";
			} else if (!plain) {
				reason = g_pcPlainTextureVtbl ? "a texture that is not a plain Texture (makeResident)" : "no plain Texture built yet to compare with";
			}
		}
		const PcDrShapeInfo& info = pcDrShapeInfo(shape);
		sDrElig.mSets.addAll(info.writes);
		if (!reason && !info.ok) {
			reason = info.why;
		}
		if (!reason && info.nonPvw) {
			reason = "a non-PVW material (useTexture, the environment-map path)";
		}
		if (!reason && info.scaled) {
			reason = "a scaled texture (useMatrixQuick's custom-scale normal matrix)";
		}
		if (!reason && info.env) {
			reason = "an environment map";
		}
	}
	sDrElig.mEligible = reason == nullptr;
	sDrElig.mWhy      = reason;
}

// Load-time test of the Pikmin lane's shapes.
static bool pcPikiAShapesOk(Creature** list, int count)
{
	struct Key {
		Creature** mList;
		int mCount;
		PikiShapeObject* mBody[4];
		Shape* mHappa[3];
	};
	static Key sKey;
	static bool sKeyValid = false;
	static bool sOk       = false;
	Key key;
	memset(&key, 0, sizeof(key));
	key.mList  = list;
	key.mCount = count;
	for (int s = 0; s < 4; s++) {
		key.mBody[s] = PikiShapeObject::_instances[s];
	}
	for (int h = 0; h < 3; h++) {
		key.mHappa[h] = pikiMgr->mLeafModel[h];
	}
	if (sKeyValid && memcmp(&key, &sKey, sizeof(key)) == 0) {
		return sOk;
	}
	sKey              = key;
	sKeyValid         = true;
	sOk               = false;
	sDrElig.mEligible = false;
	sDrElig.mWhy      = "the Pikmin shapes are not loaded yet";
	for (int s = 0; s < 4; s++) {
		if (!key.mBody[s] || !key.mBody[s]->mShape) {
			return false;
		}
	}

	BaseShape* shapes[7] = { key.mBody[0]->mShape, key.mBody[1]->mShape, key.mBody[2]->mShape, key.mBody[3]->mShape,
		                     key.mHappa[0],        key.mHappa[1],        key.mHappa[2] };
	bool lights = false;
	for (int s = 0; s < 7; s++) {
		lights = lights || (shapes[s] && shapes[s]->mLightGroup.Child() != nullptr);
	}

	int viewPikis = 0, updaters = 0;
	for (int i = 0; i < count; i++) {
		Creature* obj    = list[i];
		const void* vtbl = obj ? *reinterpret_cast<void* const*>(obj) : nullptr;
		if (!obj || vtbl != sPcViewPikiVtbl) {
			continue;
		}
		++viewPikis;
		updaters += pcSplitCountPartUpdaters(static_cast<ViewPiki*>(obj)->mCollInfo);
	}
	sOk = !lights && viewPikis == count && updaters == 0 && sPcViewPikiVtbl;
	pcPikiDrawrecEligibility(shapes, 7);
	return sOk;
}

// Drawrec Pikmin lane.
// B of packet k on the record worker.
static void pcPikiDrawrecB(Graphics& gW, int k)
{
	const PcPikiDrawPacket& pkt = sPkPackets[k];
	pkt.mBody->mMaterialList->setColour(pkt.mDrawColour);
	if (pkt.mFlags & kPktDrawBody) {
		gW.mPcPose         = pkt.mPose;
		gW.mPcPoseShape    = pkt.mBody;
		gW.mPcPoseAnimCtx  = (pkt.mFlags & kPktAnimCtx) ? 1 : 0;
		gW.mPcPoseAnimData = (pkt.mFlags & kPktAnimData) ? 1 : 0;
		pkt.mBody->drawshape(gW, *gW.mCamera, nullptr);
		gW.mPcPose      = nullptr;
		gW.mPcPoseShape = nullptr;
	}
	if (pkt.mHappa) {
		gW.useMatrix(pkt.mPose[6], 0);
		pkt.mHappa->drawshape(gW, *gW.mCamera, nullptr);
	}
}
static void pcPikiDrawrecSerialFrom(Graphics& gfx, int count, int from)
{
	for (int i = from; i < count; i++) {
		if (sPkStatus[i] == 0) {
			sPkList[i]->refresh(gfx);
		}
	}
}
static const bool sPkDrawrecLane = (pcDrawrecRegisterLane(kDrLanePiki, PcDrLaneDef { &pcPikiDrawrecB, "piki" }), true);
// Runs the Pikmin lane as drawrec records.
static bool pcPikiDrawrecRun(Graphics& gfx, int count, int live, bool shapesOk)
{
	(void)sPkDrawrecLane;
	const bool laneOk = shapesOk && !gameflow.mMoviePlayer->mIsActive && !AIPerf::kandoOnly && !gsys->mToggleDebugInfo
	                 && !pc_photo_mode_active() && pcPikiAEnsureCapacity(count);
	const float expectedUs       = pcDrawrecPrice(kDrPcPiki) * float(live);
	const unsigned expectedBytes = pcDrawrecBytes(kDrPcPiki) * unsigned(live);
	if (!pcDrawrecEnter(gfx, kDrLanePiki, sDrElig.mSets, expectedUs, expectedBytes, laneOk && sDrElig.mEligible, sDrElig.mWhy)) {
		return false;
	}
	const u32 mask = gfx.mMatRenderMask;
	int n          = 0;
	for (int i = 0; i < count; i++) {
		if (sPkStatus[i] != 0) {
			continue;
		}
		if (!pcDrawrecPartOpen()) {
			pcPikiDrawrecSerialFrom(gfx, count, i);
			return true;
		}
		ViewPiki* piki = static_cast<ViewPiki*>(sPkList[i]);
		if (pcPikiAWouldError(piki, gfx)) {
			pcDrawrecBail();
			pcPikiDrawrecSerialFrom(gfx, count, i);
			return true;
		}
		PcPikiDrawPacket& pkt = sPkPackets[n];
		if (!pcPikiRefreshA(piki, gfx, pkt)) {
			continue;
		}
		if (!pcDrawrecPartOpen()) {
			pcPikiDrawB(gfx, pkt);
			pcPikiDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		if ((pkt.mFlags & kPktDrawBody) && !(pkt.mFlags & kPktAnimData)) {
			pcDrawrecBail();
			pcPikiDrawB(gfx, pkt);
			pcPikiDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		if (!pcDrawrecCanPublish()) {
			pcPikiDrawB(gfx, pkt);
			pcPikiDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		// Damage flash.
		pkt.mDrawColour = ((pkt.mFlags & kPktDamaged) && gsys->getRand(1.0f) > 0.5f) ? COLOUR_WHITE : pkt.mColour;
		const bool meshes = ((pkt.mFlags & kPktDrawBody) && pcDrShapeDrawsMeshes(pkt.mBody, mask))
		                 || (pkt.mHappa && pcDrShapeDrawsMeshes(pkt.mHappa, mask));
		PcDrEffect e;
		e.meshes     = meshes ? 1 : 0;
		e.curMtx0    = (meshes || pkt.mHappa) ? 1 : 0;
		e.active     = pkt.mHappa ? 1 : 0;
		e.lightModel = 0;
		e.activePtr  = pkt.mHappa ? &pkt.mPose[6] : nullptr;
		pkt.mBody->mPcSegChain = g_pcDrChain;
		if (!pcDrawrecPublishRec(kDrLanePiki, n, e, kDrPcPiki)) {
			pcPikiDrawrecB(gfx, n);
			pcPikiDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		++n;
	}
	return true;
}

// Drawrec plant lane.
static void pcPlantDrawrecB(Graphics& gW, int k)
{
	const PcPlantDrawPacket& pkt = sPlPackets[k];
	gW.mPcCacheHoisted           = (pkt.mFlags & kPktHoisted) ? 1 : 0;
	PcPlantLane::drawB(gW, pkt);
	gW.mPcCacheHoisted = 0;
}
static const bool sPlDrawrecLane = (pcDrawrecRegisterLane(kDrLanePlant, PcDrLaneDef { &pcPlantDrawrecB, "plant" }), true);
static void pcPlantDrawrecSerialFrom(Graphics& gfx, int count, int from)
{
	for (int i = from; i < count; i++) {
		sPlItems[i]->refresh(gfx);
	}
}
// Runs the plant pass as drawrec records.
static bool pcPlantDrawrecRun(Graphics& gfx, PlantMgr* mgr)
{
	(void)sPlDrawrecLane;
	int count = mgr->pcSplitSnapshot(sPlItems, sPlItemCap);
	if (count > sPlItemCap) {
		const int cap = count + 32;
		void* mem     = malloc(size_t(cap) * sizeof(Plant*));
		if (!mem) {
			return false;
		}
		free(sPlItems);
		sPlItems   = static_cast<Plant**>(mem);
		sPlItemCap = cap;
		count      = mgr->pcSplitSnapshot(sPlItems, sPlItemCap);
	}
	if (count <= 0 || count > sPlItemCap || !mgr->pcSplitShapesOk() || !pcPlantEnsureCapacity(count)) {
		return false;
	}
	for (int t = PLANT_START; t < PLANT_COUNT; t++) {
		const Shape* s = mgr->pcPlantShape(t);
		if (s && !pcDrShapeInfo(s).ok) {
			return false;
		}
	}
	if (!pcDrS2EnsurePart(gfx, kDrLanePlant)) {
		return false;
	}
	const u32 mask = gfx.mMatRenderMask;
	int n          = 0;
	for (int i = 0; i < count; i++) {
		if (!pcDrawrecPartOpen()) {
			pcPlantDrawrecSerialFrom(gfx, count, i);
			return true;
		}
		if (sPlItems[i]->pcWouldError(gfx)) {
			pcDrawrecBail();
			pcPlantDrawrecSerialFrom(gfx, count, i);
			return true;
		}
		PcPlantDrawPacket& pkt = sPlPackets[n];
		if (!PcPlantLane::runA(i, gfx, pkt)) {
			continue;
		}
		if (!pcDrawrecPartOpen()) {
			PcPlantLane::drawB(gfx, pkt);
			pcPlantDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		if (!(pkt.mFlags & kPktAnimData) || !pcDrawrecCanPublish()) {
			pcDrawrecBail();
			PcPlantLane::drawB(gfx, pkt);
			pcPlantDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		if (pcDrHoistCache(gfx, pkt.mShape, nullptr)) {
			pkt.mFlags |= kPktHoisted;
		}
		pcDrawrecAddSets(pcDrShapeInfo(pkt.mShape).writes);
		PcDrEffect e;
		e.meshes     = pcDrShapeDrawsMeshes(pkt.mShape, mask) ? 1 : 0;
		e.curMtx0    = 1;
		e.active     = 1;
		e.lightModel = 0;
		e.activePtr  = &Matrix4f::ident;
		if (!pcDrawrecPublishRec(kDrLanePlant, n, e, kDrPcPlant)) {
			gfx.mPcCacheHoisted = 0;
			PcPlantLane::drawB(gfx, pkt);
			pcPlantDrawrecSerialFrom(gfx, count, i + 1);
			return true;
		}
		pcDrS2Spent(kDrPcPlant);
		++n;
	}
	return true;
}

// PikiMgr::refresh draw.
bool pcPikiDrawRun(Graphics& gfx, Creature** list, int* status, int count)
{
	if (count <= 0) {
		return false;
	}
	const bool shapesOk = pcPikiAShapesOk(list, count);
	if (pcDrawrecLanes() & kDrBitPiki) {
		pcSplitCollect();
		int live = 0;
		for (int i = 0; i < count; i++) {
			live += status[i] == 0 ? 1 : 0;
		}
		sPkList   = list;
		sPkStatus = status;
		if (pcPikiDrawrecRun(gfx, count, live, shapesOk)) {
			return true;
		}
	}
	pcDrawrecSeal();
	if (pcDrawrecChainOpen()) {
		return false;
	}
	pcSplitCollect();
	int live = 0;
	for (int i = 0; i < count; i++) {
		live += status[i] == 0 ? 1 : 0;
	}
	if (!pcSplitWorkerOk<PcPikiLane>(count, live, shapesOk, &pcPikiAEnsureCapacity)) {
		return false;
	}
	sPkList   = list;
	sPkStatus = status;
	pcSplitRunThreaded<PcPikiLane>(gfx, count, live);
	return true;
}

// PlantMgr::refresh draw.
bool pcPlantDrawRun(Graphics& gfx, PlantMgr* mgr)
{
	if (g_pcDrS2Live && (pcDrawrecLanes() & kDrBitPlant) && pcPlantDrawrecRun(gfx, mgr)) {
		return true;
	}
	pcSplitCollect();
	if (pcDrawrecChainOpen()) {
		return false;
	}
	int count = mgr->pcSplitSnapshot(sPlItems, sPlItemCap);
	if (count > sPlItemCap) {
		const int cap = count + 32;
		void* mem     = malloc(size_t(cap) * sizeof(Plant*));
		if (mem) {
			free(sPlItems);
			sPlItems   = static_cast<Plant**>(mem);
			sPlItemCap = cap;
			count      = mgr->pcSplitSnapshot(sPlItems, sPlItemCap);
		}
	}
	if (count == 0) {
		return false;
	}
	const bool listOk   = count > 0 && count <= sPlItemCap; // not all Plants
	const bool shapesOk = listOk && mgr->pcSplitShapesOk();
	if (!pcSplitWorkerOk<PcPlantLane>(listOk ? count : 0, listOk ? count : 0, shapesOk, &pcPlantEnsureCapacity)) {
		return false;
	}
	pcSplitRunThreaded<PcPlantLane>(gfx, count, count);
	return true;
}
#endif
