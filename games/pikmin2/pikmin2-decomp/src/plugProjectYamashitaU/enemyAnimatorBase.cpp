#include "Game/EnemyAnimatorBase.h"
#include "System.h" // PC_ORIG_DT_SCALE / PC_DAMP

namespace Game {
const f32 EnemyAnimatorBase::defaultAnimSpeed = 30.0f;

/**
 * @note Address: 0x80127974
 * @note Size: 0x44
 */
EnemyAnimatorBase::EnemyAnimatorBase()
    : mSpeed(30.0f)
    , mNormalizedTime(1.0f)
{
	reset();
}

/**
 * @note Address: 0x801279B8
 * @note Size: 0x124
 */
void EnemyAnimatorBase::animate(f32 speed)
{
	if (!(mFlags.isSet(SysShape::Animator::AnimCompleted))) {
		if (mFlags.isSet(SysShape::Animator::AnimFinishMotion)) {
			mNormalizedTime *= PC_DAMP(0.9f); // FPS Mode: amortiguacion por frame
			if (mNormalizedTime < 0.1f) {
				mNormalizedTime = 0.0f;

				mFlags.unset(SysShape::Animator::AnimInProgress);
				mFlags.set(SysShape::Animator::AnimCompleted);
			}
		} else if (mFlags.isSet(SysShape::Animator::AnimInProgress)) {
			mNormalizedTime *= 1.1f;
			if (mNormalizedTime > 1.0f) {
				mNormalizedTime = 1.0f;

				mFlags.unset(SysShape::Animator::AnimFinishMotion | SysShape::Animator::AnimCompleted);
				mNormalizedTime = 1.0f;
			}
		}

		getAnimator().animate(speed * mNormalizedTime);
	} else {
		getAnimator().animate(0.0f);
	}
}

/**
 * @note Address: 0x80127ADC
 * @note Size: 0x124
 */
void EnemyAnimatorBase::animate(int animatorNum, f32 speed)
{
	if (!(mFlags.isSet(SysShape::Animator::AnimCompleted))) {
		if (mFlags.isSet(SysShape::Animator::AnimFinishMotion)) {
			mNormalizedTime *= PC_DAMP(0.9f); // FPS Mode: amortiguacion por frame
			if (mNormalizedTime < 0.1f) {
				mNormalizedTime = 0.0f;

				mFlags.unset(SysShape::Animator::AnimInProgress);
				mFlags.set(SysShape::Animator::AnimCompleted);
			}
		} else if (mFlags.isSet(SysShape::Animator::AnimInProgress)) {
			mNormalizedTime *= 1.1f;
			if (mNormalizedTime > 1.0f) {
				mNormalizedTime = 1.0f;

				mFlags.unset(SysShape::Animator::AnimFinishMotion | SysShape::Animator::AnimCompleted);
				mNormalizedTime = 1.0f;
			}
		}

		getAnimator(animatorNum).animate(speed * mNormalizedTime);
	} else {
		getAnimator().animate(0.0f);
	}
}
} // namespace Game
