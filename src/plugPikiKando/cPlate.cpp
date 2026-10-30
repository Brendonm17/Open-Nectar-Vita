#include "CPlate.h"
#if defined(PIKI_PC_PORT)
#include "settings/pc_settings.h"
#endif
#include "AIPerf.h"
#include "DebugLog.h"
#include "GlobalShape.h"
#include "Graphics.h"
#include "Piki.h"
#include "SlotChangeListner.h"
#include "sysNew.h"
#if defined(__vita__)
#include <stdint.h>
#include <string.h>
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(10)

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT("cPlate");

#if defined(__vita__)
/* PS Vita: squad sorts on index arrays, falling back to the original loop. */
static const int kPcPlateSortMax = 1024; // at most 999 Pikmin + 10
static Creature* sPcSortOwner[kPcPlateSortMax];
static SlotChangeListner* sPcSortListener[kPcPlateSortMax];
static int sPcSortColor[kPcPlateSortMax];
static u8 sPcSortRank[kPcPlateSortMax];
static u8 sPcSortScore[kPcPlateSortMax];
static f32 sPcSortDist[kPcPlateSortMax];
static u16 sPcSortFrom[kPcPlateSortMax]; // slot k ends up with what was gathered from slot sPcSortFrom[k]
static u8 sPcSortTouched[kPcPlateSortMax];

// Pointer set for the distinctness check.
static const void* sPcSeenKey[2 * kPcPlateSortMax];
static u32 sPcSeenGen[2 * kPcPlateSortMax];
static u32 sPcSeenCurrent = 0;

static void pcSeenReset()
{
	if (++sPcSeenCurrent == 0) {
		memset(sPcSeenGen, 0, sizeof(sPcSeenGen));
		sPcSeenCurrent = 1;
	}
}

// False if p is already in the set.
static bool pcSeenInsert(const void* p)
{
	u32 h = (u32(uintptr_t(p) >> 2) * 2654435761u) >> 21; // 2048 entries
	while (sPcSeenGen[h] == sPcSeenCurrent) {
		if (sPcSeenKey[h] == p) {
			return false;
		}
		h = (h + 1) & (2 * kPcPlateSortMax - 1);
	}
	sPcSeenGen[h] = sPcSeenCurrent;
	sPcSeenKey[h] = p;
	return true;
}

static bool pcPlateApplyIsExact(int n)
{
	pcSeenReset();
	for (int k = 0; k < n; k++) {
		if (!sPcSortListener[k] || sPcSortOwner[k]->getCnt() < 1 || !pcSeenInsert(sPcSortOwner[k])) {
			return false;
		}
	}
	pcSeenReset();
	for (int k = 0; k < n; k++) {
		if (!pcSeenInsert(sPcSortListener[k])) {
			return false;
		}
	}
	return true;
}

// Rewrite each touched slot once.
static void pcPlateApply(CPlate::Slot* slots, int n)
{
	for (int k = 0; k < n; k++) {
		if (!sPcSortTouched[k]) {
			continue;
		}
		CPlate::Slot* slot = &slots[k];
		const int from     = sPcSortFrom[k];
		slot->mOccupant.clear();
		slot->mOccupant.set(sPcSortOwner[from]);
		slot->mListener = sPcSortListener[from];
		slot->mListener->inform(k);
	}
}
#endif

/**
 * @todo: Documentation
 */
Creature* CPlate::getCreature(int idx)
{
	return mSlotList[idx].mOccupant.getPtr();
}

/**
 * @todo: Documentation
 */
CPlate::CPlate(MapMgr* mgr)
{
#if defined(PIKI_PC_PORT)
	// 110 in the original: the 100 Pikmin field limit plus a margin. The port
	// lets the player raise that limit, and the formation would then run past
	// the end of its slot list, so keep the same relationship to whatever
	// limit is in force.
	mSlotListSize = pc_settings_get_piki_limit() + 10;
#else
	mSlotListSize = 110;
#endif
	mPlateSize    = 10.0f;
	mPlateLength  = 10.0f;
	mOriginPosition.set(0.0f, 0.0f, 0.0f);
	mDirectionAngle = 0.0f;
	mSlotList       = new Slot[mSlotListSize];
	mTotalSlotCount = 0;
	mPlatePikiCount = 0;
	mUsedSlotCount  = 0;
	_C8             = false;
	mIsNeutral      = true;

	load("parms/", "cunit.bin", 1);
	mDevOffsetTest.set(0.0f, 0.0f, 0.0f);
	mHappaCounts[0] = mHappaCounts[1] = mHappaCounts[2] = 0;
}

/**
 * @todo: Documentation
 */
bool CPlate::canNaviRunFast()
{
	return true;
}

/**
 * @todo: Documentation
 */
void CPlate::init(immut Vector3f&)
{
}

/**
 * @todo: Documentation
 */
void CPlate::setPos(immut Vector3f& position, f32 angle, immut Vector3f& velocity)
{
	f32 offset = mCPlateParms.mStartOffset();
	if (speedy_sqrtf(mCurrentVelocity.x * mCurrentVelocity.x + mCurrentVelocity.z * mCurrentVelocity.z) > 5.0f) {
		offset = 0.0f;
	}

	f32 totalRadius  = mPlateLength + offset;
	mDirectionAngle  = angle;
	mOriginPosition  = position;
	mPlateCenter     = mOriginPosition + Vector3f(totalRadius * sinf(angle), 0.0f, totalRadius * cosf(angle));
	mCurrentVelocity = velocity;
	mPlateOffset     = position + Vector3f(mInnerRadius * sinf(angle), 0.0f, mInnerRadius * cosf(angle));
	mIsNeutral       = false;

	STACK_PAD_VAR(2);
}

/**
 * @todo: Documentation
 */
void CPlate::setPosGray(immut Vector3f& position, f32 angle, immut Vector3f& velocity)
{
	f32 offset = mCPlateParms.mStartOffset();
	if (speedy_sqrtf(mCurrentVelocity.x * mCurrentVelocity.x + mCurrentVelocity.z * mCurrentVelocity.z) > 5.0f) {
		offset = 0.0f;
	}

	f32 dist         = mPlateLength + offset;
	mOriginPosition  = position;
	mPlateCenter     = mOriginPosition + Vector3f(dist * sinf(angle), 0.0f, dist * cosf(angle));
	mCurrentVelocity = velocity;
	mPlateOffset     = position + Vector3f(mInnerRadius * sinf(angle), 0.0f, mInnerRadius * cosf(angle));
	mIsNeutral       = false;

	STACK_PAD_VAR(2);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0001E0
 */
void CPlate::setPosNeutral(immut Vector3f& position, f32 angle, immut Vector3f& velocity)
{
	setPos(position, angle, velocity);
	mIsNeutral   = true;
	mPlateCenter = mOriginPosition;
}

/**
 * @todo: Documentation
 */
int CPlate::getSlot(Creature* occupant, SlotChangeListner* listener)
{
	mHappaCounts[static_cast<Piki*>(occupant)->mHappa]++;
	int idx = mUsedSlotCount;
	mSlotList[idx].mOccupant.set(occupant);
	mSlotList[idx].mListener = listener;
	mUsedSlotCount++;
	return idx;
}

/**
 * @todo: Documentation
 */
void CPlate::changeFlower(Piki* piki)
{
	int oldHappa = (piki->mHappa + 2) % PikiHappaCount;
	mHappaCounts[piki->mHappa]++;
	mHappaCounts[oldHappa]--;
}

/**
 * @todo: Documentation
 */
void CPlate::releaseSlot(Creature* occupant, int idx)
{
	Slot* slot = &mSlotList[idx];
	if (slot->mOccupant.getPtr() != occupant) {
		PRINT(" %x try to release slot (%d) but owner is %x\n", occupant, idx, slot->mOccupant.getPtr());
		ERROR(" sorry ...\n");
	}

	mHappaCounts[static_cast<Piki*>(occupant)->mHappa]--;
	slot->mOccupant.clear();
	mUsedSlotCount--;
	mTotalSlotCount--;

	for (int i = idx; i < mUsedSlotCount; i++) {
		mSlotList[i].mOccupant.clear();
		mSlotList[i].mOccupant.set(mSlotList[i + 1].mOccupant.getPtr());
		mSlotList[i].mListener = mSlotList[i + 1].mListener;
		mSlotList[i].mListener->inform(i);
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000118
 */
void CPlate::swapSlot(int idx1, int idx2)
{
	Slot* slot1 = &mSlotList[idx1];
	Slot* slot2 = &mSlotList[idx2];

	Creature* owner1             = slot1->mOccupant.getPtr();
	SlotChangeListner* listener1 = slot1->mListener;
	Creature* owner2             = slot2->mOccupant.getPtr();
	SlotChangeListner* listener2 = slot2->mListener;

	slot1->mOccupant.clear();
	slot1->mOccupant.set(owner2);
	slot1->mListener = listener2;
	slot1->mListener->inform(idx1);

	slot2->mOccupant.clear();
	slot2->mOccupant.set(owner1);
	slot2->mListener = listener1;
	slot2->mListener->inform(idx2);
}

/**
 * @todo: Documentation
 */
bool CPlate::validSlot(int idx)
{
	if (idx < 0 || idx >= mUsedSlotCount) {
		PRINT("slot %d : out of range [0..%d]\n", idx, mUsedSlotCount);
		return false;
	}

	return true;
}

/**
 * @todo: Documentation
 */
void CPlate::sortByColor(Piki* piki)
{
	bool hasBomb  = piki->hasBomb() ? 1 : 0;
	int color     = piki->mColor;
	int nextColor = (color + 1) % PikiColorCount;
	int happa     = piki->mHappa;

#if defined(__vita__)
	const int n = mUsedSlotCount;
	if (n <= kPcPlateSortMax) {
		// Per-occupant sort keys.
		bool gathered      = true;
		bool ranksDistinct = color >= 0 && color < PikiColorCount; // then colour, next colour and the third get 1, 2, 3
		for (int k = 0; k < n; k++) {
			Piki* owner = static_cast<Piki*>(mSlotList[k].mOccupant.getPtr());
			if (!owner) {
				gathered = false;
				break;
			}
			const int ownerColor = owner->mColor;
			int bomb;
			if (hasBomb) {
				bomb = owner->hasBomb() ? 0 : 1;
			} else {
				bomb = owner->hasBomb() ? 1 : 0;
			}
			bomb += (happa == owner->mHappa) ? 0 : 1;
			sPcSortOwner[k]    = owner;
			sPcSortListener[k] = mSlotList[k].mListener;
			sPcSortColor[k]    = ownerColor;
			sPcSortRank[k]     = (ownerColor == color) ? 1 : (ownerColor == nextColor) ? 2 : 3;
			sPcSortScore[k]    = u8(bomb);
			sPcSortFrom[k]     = u16(k);
			sPcSortTouched[k]  = 0;
			if (ownerColor >= PikiColorCount) {
				ranksDistinct = false; // two different colours could share rank 3
			}
		}
		if (gathered) {
			// Already in order.
			if (ranksDistinct) {
				bool ordered = true;
				for (int k = 1; k < n; k++) {
					if (sPcSortRank[k] < sPcSortRank[k - 1]
					    || (sPcSortRank[k] == sPcSortRank[k - 1] && sPcSortScore[k] < sPcSortScore[k - 1])) {
						ordered = false;
						break;
					}
				}
				if (ordered) {
					return;
				}
			}
			// The loop below, on indices.
			bool swapped = false;
			for (int i = 0; i < n; i++) {
				for (int j = i + 1; j < n; j++) {
					const int iFrom = sPcSortFrom[i];
					const int jFrom = sPcSortFrom[j];
					const bool swap = (sPcSortColor[iFrom] != sPcSortColor[jFrom]) ? sPcSortRank[jFrom] < sPcSortRank[iFrom]
					                                                               : sPcSortScore[jFrom] < sPcSortScore[iFrom];
					if (swap) {
						sPcSortFrom[i]    = u16(jFrom);
						sPcSortFrom[j]    = u16(iFrom);
						sPcSortTouched[i] = 1;
						sPcSortTouched[j] = 1;
						swapped           = true;
					}
				}
			}
			if (!swapped) {
				return;
			}
			if (pcPlateApplyIsExact(n)) {
				pcPlateApply(mSlotList, n);
				return;
			}
		}
	}
#endif

	for (int i = 0; i < mUsedSlotCount; i++) {
		for (int j = 0; j < mUsedSlotCount; j++) {
			Piki* iOwner = static_cast<Piki*>(mSlotList[i].mOccupant.getPtr());
			Piki* jOwner = static_cast<Piki*>(mSlotList[j].mOccupant.getPtr());

			int iColor = iOwner->mColor;
			int jColor = jOwner->mColor;

			if (iColor != jColor) {
				int iCheck = (iColor == color) ? 1 : (iColor == nextColor) ? 2 : 3;
				int jCheck = (jColor == color) ? 1 : (jColor == nextColor) ? 2 : 3;

				if (j > i && jCheck < iCheck) {
					swapSlot(j, i);
				}

				continue;
			}

			int iBomb;
			int jBomb;
			if (hasBomb) {
				iBomb = iOwner->hasBomb() ? 0 : 1;
				jBomb = jOwner->hasBomb() ? 0 : 1;
			} else {
				iBomb = iOwner->hasBomb() ? 1 : 0;
				jBomb = jOwner->hasBomb() ? 1 : 0;
			}

			int iCheck = (happa == iOwner->mHappa) ? 0 : 1;

			iBomb += iCheck;

			int jCheck = (happa == jOwner->mHappa) ? 0 : 1;
			jBomb += jCheck;

			if (j > i && jBomb < iBomb) {
				swapSlot(j, i);
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void CPlate::rearrangeSlot(immut Vector3f& position, f32, immut Vector3f&)
{
#if defined(__vita__)
	const int n = mUsedSlotCount;
	if (n <= kPcPlateSortMax) {
		if (n < 2) {
			return;
		}
		// Per-occupant distance.
		bool gathered = true;
		for (int k = 0; k < n; k++) {
			Creature* owner = mSlotList[k].mOccupant.getPtr();
			if (!owner) {
				gathered = false;
				break;
			}
			Vector3f sep       = position - owner->mSRT.t;
			sPcSortDist[k]     = sep.length();
			sPcSortOwner[k]    = owner;
			sPcSortListener[k] = mSlotList[k].mListener;
			sPcSortFrom[k]     = u16(k);
			sPcSortTouched[k]  = 0;
		}
		if (gathered) {
			// The loop below, on indices.
			bool swapped = false;
			for (int i = n - 1; i >= 1; i--) {
				for (int j = i; j >= 1; j--) {
					const int jFrom    = sPcSortFrom[j];
					const int prevFrom = sPcSortFrom[j - 1];
					if (sPcSortDist[jFrom] < sPcSortDist[prevFrom]) {
						sPcSortFrom[j]        = u16(prevFrom);
						sPcSortFrom[j - 1]    = u16(jFrom);
						sPcSortTouched[j]     = 1;
						sPcSortTouched[j - 1] = 1;
						swapped               = true;
					}
				}
			}
			if (!swapped) {
				return;
			}
			if (pcPlateApplyIsExact(n)) {
				pcPlateApply(mSlotList, n);
				return;
			}
		}
	}
#endif
	for (int i = mUsedSlotCount - 1; i >= 1; i--) {
		for (int j = i; j >= 1; j--) {
			Vector3f sep = position - mSlotList[j].mOccupant.getPtr()->mSRT.t;
			f32 jDist    = sep.length();

			sep          = position - mSlotList[j - 1].mOccupant.getPtr()->mSRT.t;
			f32 prevDist = sep.length();

			if (jDist < prevDist) {
				swapSlot(j, j - 1);
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void CPlate::refresh(int slotCount, f32 interpT)
{
	if (slotCount < mTotalSlotCount) {
		mUsedSlotCount -= mTotalSlotCount - slotCount;
	}

	mTotalSlotCount = slotCount;
	mInnerRadius    = (2.0f * mCPlateParms.mMaxPosSize()) * speedy_sqrtf(slotCount / PI);
	mInnerRadius    = (2.1f * mCPlateParms.mMaxPosSize()) * speedy_sqrtf(slotCount / PI);

	f32 maxRadius;
	f32 minRadius;
	f32 baseSize = 1.9f * mCPlateParms.mMaxPosSize();
	if (baseSize > mInnerRadius) {
		maxRadius = baseSize;
		minRadius = mInnerRadius;
	} else {
		maxRadius = mInnerRadius;
		minRadius = baseSize;
	}

	mPlateSize   = -(maxRadius - minRadius) * interpT + maxRadius;
	mPlateLength = ((4.0f * slotCount) * mCPlateParms.mMaxPosSize() * mCPlateParms.mMaxPosSize()) / (mPlateSize * PI);

	f32 limit = mCPlateParms.mLengthLimit();
	if (mPlateLength > limit) {
		mPlateLength = limit;
		mPlateSize   = ((4.0f * slotCount) * mCPlateParms.mMaxPosSize() * mCPlateParms.mMaxPosSize()) / (mPlateLength * PI);
	}

	refreshSlot();
}

/**
 * @todo: Documentation
 */
void CPlate::refreshSlot()
{
	int slotsInRow, slotIndex;
	BOOL flip;

	f32 zExtent = -mPlateLength;
	slotIndex   = 0;

	Matrix4f transformMtx;

	Vector3f centerOffset(mPlateCenter);
	centerOffset.multiply(-1.0f);

	transformMtx.makeSRT(Vector3f(1.0f, 1.0f, 1.0f), Vector3f(0.0f, mDirectionAngle, 0.0f), Vector3f(0.0f, 0.0f, 0.0f));

	Vector3f devOffsetTest(0.0f, 0.0f, 0.0f);
	devOffsetTest.multMatrixTo(transformMtx, mDevOffsetTest);

	flip = TRUE;
	while (slotIndex < mTotalSlotCount) {
		int slotsPerSize = (speedy_sqrtf(mPlateLength * mPlateLength - zExtent * zExtent) * mPlateSize / mPlateLength)
		                 / (2.0f * mCPlateParms.mMaxPosSize());
		if (slotsPerSize < 0) {
			slotsPerSize = 0;
		}

		f32 xExtent = f32(flip) * (2.0f * (f32(slotsPerSize) * mCPlateParms.mMaxPosSize()));
		slotsInRow  = 2 * slotsPerSize + 1;
		while (slotsInRow > 0) {
			if (slotIndex < mTotalSlotCount) {
				mSlotList[slotIndex].mPosition.set(xExtent, 0.0f, zExtent);
				mSlotList[slotIndex].mPosition.multMatrixTo(transformMtx, mSlotList[slotIndex].mOffsetFromCenter);
				slotIndex++;
			}

			slotsInRow--;
			xExtent -= (2.0f * (f32(flip) * mCPlateParms.mMaxPosSize()));
		}

		zExtent += 2.0f * mCPlateParms.mMaxPosSize();
		flip *= -1;
	}
}

/**
 * @todo: Documentation
 */
void CPlate::update()
{
	// the hell was this even for kando
	Vector3f vec;
	f32 val = mPlateOffset.y + 2.0f;
	vec.set(sinf(mDirectionAngle), 0.0f, cosf(mDirectionAngle));
	if (mHappaCounts[Bud] != 0) {
		val = 0.0f;
	}

	STACK_PAD_VAR(12);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000004
 */
void CPlate::postUpdate(f32)
{
}

/**
 * @todo: Documentation
 */
void CPlate::render(Graphics& gfx)
{
	if (!AIPerf::kandoOnly) {
		return;
	}

	Matrix4f mtx1;
	Matrix4f mtx2;
	Vector3f scale;
	f32 s = 0.25f;
	scale.set(s, s, s);

	mtx1.makeSRT(scale, Vector3f(0.0f, 0.0f, 0.0f), mPlateOffset);

	gfx.calcViewMatrix(mtx1, mtx2);
	gfx.useMatrix(mtx2, 0);

	Colour colour;
	colour.set(125, 255, 0, 255);

	GlobalShape::markerShape->mMaterialList->colour() = colour;
	GlobalShape::markerShape->drawshape(gfx, *gfx.mCamera, nullptr);

	mtx1.makeSRT(scale, Vector3f(0.0f, 0.0f, 0.0f), mPlateCenter);

	gfx.calcViewMatrix(mtx1, mtx2);
	gfx.useMatrix(mtx2, 0);

	colour.set(125, 175, 200, 255);
	GlobalShape::markerShape->mMaterialList->colour() = colour;
	GlobalShape::markerShape->drawshape(gfx, *gfx.mCamera, nullptr);

	for (int i = 0; i < mUsedSlotCount; i++) {
		Vector3f globalPos(mSlotList[i].mOffsetFromCenter);
		globalPos = globalPos + mPlateCenter;

		s = 0.08f;
		scale.set(s, s, s);

		mtx1.makeSRT(scale, Vector3f(0.0f, 4.0f, 0.0f), globalPos);

		gfx.calcViewMatrix(mtx1, mtx2);
		gfx.useMatrix(mtx2, 0);

		colour.set(0, 255, 10, 255);
		GlobalShape::markerShape->mMaterialList->colour() = colour;
		GlobalShape::markerShape->drawshape(gfx, *gfx.mCamera, nullptr);
	}
}
