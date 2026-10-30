#include "Dolphin/pad.h"
#include "RumbleMgr.h"
#if defined(PIKI_PC_PORT)
#include "pc_window.h"
#endif

/**
 * @todo: Documentation
 */
RumbleSample::RumbleSample(int chan)
{
	mCurrentIntensity = 0.0f;
	mTotalIntensity   = 0.0f;
	mChannel          = chan;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000010 (Matching by size)
 */
void RumbleSample::init()
{
	mCurrentIntensity = 0.0f;
	mTotalIntensity   = 0.0f;
}

/**
 * @todo: Documentation
 */
void RumbleSample::simpleStop()
{
#if defined(PIKI_PC_PORT)
	pc_window_rumble(mChannel, 0.0f);
#else
	PADStopMotorHard(mChannel);
#endif
}

/**
 * @todo: Documentation
 */
void RumbleSample::simpleStart(f32 intensity)
{
	mCurrentIntensity = intensity;
	mTotalIntensity += mCurrentIntensity;
#if defined(PIKI_PC_PORT)
	// Controller motor: level, not pulse train.
	pc_window_rumble(mChannel, mCurrentIntensity);
	if (mTotalIntensity >= 1.0f) mTotalIntensity -= 1.0f;
	return;
#endif
	if (mCurrentIntensity <= 0.0f) {
		PADStopMotorHard(mChannel);

	} else if (mTotalIntensity < 1.0f) {
		PADStopMotor(mChannel);

	} else {
		mTotalIntensity -= 1.0f;
		PADStartMotor(mChannel);
	}
}
