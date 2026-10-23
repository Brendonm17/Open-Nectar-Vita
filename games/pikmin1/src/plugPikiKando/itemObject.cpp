#include "ItemObject.h"
#include "DebugLog.h"
#include "Dolphin/os.h"
#ifdef PIKI_PC_PORT
#include "system.h"
#include "settings/pc_settings.h"
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
DEFINE_PRINT("itemObject");

/**
 * @todo: Documentation
 * @note UNUSED Size: 000074 (Matching by size)
 */
ItemBall::ItemBall(Shape* shape, CreatureProp* props)
    : ItemCreature(OBJTYPE_Secret1, props, shape)
    , mBallCollision(0)
{
}

/**
 * @todo: Documentation
 */
void ItemBall::startAI(int)
{
	mCollInfo = &mBallCollision;
	mCollInfo->initInfo(mItemShape, mBallParts, mPartIDs);
}

/**
 * @todo: Documentation
 */
bool ItemBall::isAlive()
{
	return true;
}

/**
 * @todo: Documentation
 */
bool ItemBall::isVisible()
{
	return true;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00005C (Matching by size)
 */
ItemObject::ItemObject(int objType, Shape* shape)
    : ItemCreature(objType, nullptr, shape)
{
	mObjType = (EObjType)objType;
}

/**
 * @todo: Documentation
 */
void ItemObject::update()
{
}

/**
 * @todo: Documentation
 */
BombGenItem::BombGenItem(Shape* shape)
    : ItemObject(OBJTYPE_BombGen, shape)
{
	mCapacity  = -1;
	mRemaining = 1;
#ifdef PIKI_PC_PORT
	mPcRespawnTimer = 0.0f;
#endif
}

#ifdef PIKI_PC_PORT
/**
 * Mod "Bomb Rock Respawn" (issue #78): en el original una pila gastada solo se
 * repone al día siguiente. Con el mod vuelve una roca cada cierto tiempo
 * dentro del mismo día, hasta su capacidad original.
 */
void BombGenItem::update()
{
	ItemObject::update();
	const int interval = pc_settings_get_bomb_respawn();
	if (interval <= 0 || mCapacity <= 0 || mRemaining >= mCapacity) {
		mPcRespawnTimer = 0.0f;
		return;
	}
	mPcRespawnTimer += gsys->getFrameTime();
	if (mPcRespawnTimer >= f32(interval)) {
		mPcRespawnTimer = 0.0f;
		++mRemaining;
	}
}
#endif

/**
 * @todo: Documentation
 */
bool BombGenItem::pickable()
{
	return isValid();
}

/**
 * @todo: Documentation
 */
bool BombGenItem::pick()
{
	if (!pickable()) {
		return false;
	}

	if (mCapacity > 0) {
		if (mRemaining > 0) {
			--mRemaining;
			PRINT("BOMB LEFT %d\n", mRemaining);
		} else {
			return false;
		}
	}

	return true;
}

/**
 * @todo: Documentation
 */
Fulcrum::Fulcrum()
    : ItemObject(OBJTYPE_Fulcrum, nullptr)
{
}

/**
 * @todo: Documentation
 */
NaviDemoSunsetStart::NaviDemoSunsetStart()
    : ItemObject(OBJTYPE_SunsetStart, nullptr)
{
}

/**
 * @todo: Documentation
 */
NaviDemoSunsetGoal::NaviDemoSunsetGoal()
    : ItemObject(OBJTYPE_SunsetGoal, nullptr)
{
}
