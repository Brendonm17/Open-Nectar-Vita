#include "Graphics.h"
#if defined(PIKI_PC_PORT)
#include "gl/pc_gfx.h"
#include <cstring>
#endif

#include "Collision.h"
#include "Colour.h"
#include "DebugLog.h"
#include "Font.h"
#include "Material.h"
#include "PVW.h"
#include "Plane.h"
#include "Shape.h"
#include "Texture.h"
#include "nlib/Math.h"
#include "sysNew.h"
#include <math.h>
#if defined(PIKI_PC_PORT)
#include "timing/pc_render_phase.h"
#include "GlobalGameOptions.h"
#include "settings/pc_settings.h"
#endif
#if defined(__vita__)
#include "gl/vita_drawrec.h"
#include "../sysDolphin/pcDgxCtx.h"
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(17)

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("Graphics");

/**
 * @todo: Documentation
 * @note UNUSED Size: 000094
 */
void Colour::write(Stream& s)
{
	r = s.readByte();
	g = s.readByte();
	b = s.readByte();
	a = s.readByte();
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000064
 */
void PVWLightingInfo::read(RandomAccessStream& s)
{
	mCtrlFlag = s.readInt();
	_UNUSED08 = s.readFloat();
}

/**
 * @todo: Documentation
 */
void PVWPolygonColourInfo::animate(f32* data, Colour& col)
{
	// If there is no animation loaded, don't do anything
	if (mTotalFrameCount == 0) {
		return;
	}

	if (data) {
		mCurrentFrame = std::fmodf(data[0], mTotalFrameCount);
	} else {
		// If no new data is provided, increment the current frame
#if defined(PIKI_PC_PORT)
		if (pc_render_is_authoritative()) {
#endif
		mCurrentFrame += gsys->getFrameTime() * (30.0f * mSpeed);

		// Wrap around if we've reached the end of the animation
		if (mCurrentFrame >= mTotalFrameCount - 1) {
			mCurrentFrame = 0.0f;
		}
#if defined(PIKI_PC_PORT)
		}
#endif
	}

	// Animate colour and transparency separately
	mColourInfo.extract(mCurrentFrame, col);
	mAlphaInfo.extract(mCurrentFrame, col);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000A0
 */
f32 subExtract(f32 time, immut AKeyInfo& src, immut AKeyInfo& dest)
{
	f32 a = time - src.mKeyframePosition;
	f32 b = 1.0f / (dest.mKeyframePosition - src.mKeyframePosition);
	f32 c = (a * a) * b;
	f32 d = c * b;
	f32 e = a * d;
	f32 f = e * b;

	f32 result = (2.0f * f - 3.0f * d + 1.0f) * src.mValue + (-2.0f * f + 3.0f * d) * dest.mValue + (e - 2.0f * c + a) * src.mStartTangent
	           + (e - c) * dest.mEndTangent;
	return result;
}

/**
 * @todo: Documentation
 */
void PVWColourAnimInfo::extract(f32 value, Colour& target)
{
	// If there is no animation data, return without modification
	if (mAnimInfo.mSize == 0) {
		return;
	}

	if (mAnimInfo.mSize == 1) {
		target.r = mAnimInfo.mKeyframes[0].mRedData.mTime;
		target.g = mAnimInfo.mKeyframes[0].mGreenData.mTime;
		target.b = mAnimInfo.mKeyframes[0].mBlueData.mTime;
		return;
	}

	int idx = 0;
	for (int i = 0; i < (int)mAnimInfo.mSize - 1; i++) {
		if (mAnimInfo.mKeyframes[i].mTime <= value && mAnimInfo.mKeyframes[i + 1].mTime >= value) {
			idx = i;
			break;
		}
	}

	f32 red   = subExtract(value,
	                       AKeyInfo(mAnimInfo.mKeyframes[idx].mTime, mAnimInfo.mKeyframes[idx].mRedData.mTime,
	                                mAnimInfo.mKeyframes[idx].mRedData.mValue, mAnimInfo.mKeyframes[idx].mRedData.mTangent),
	                       AKeyInfo(mAnimInfo.mKeyframes[idx + 1].mTime, mAnimInfo.mKeyframes[idx + 1].mRedData.mTime,
	                                mAnimInfo.mKeyframes[idx + 1].mRedData.mValue, mAnimInfo.mKeyframes[idx + 1].mRedData.mTangent));
	f32 green = subExtract(value,
	                       AKeyInfo(mAnimInfo.mKeyframes[idx].mTime, mAnimInfo.mKeyframes[idx].mGreenData.mTime,
	                                mAnimInfo.mKeyframes[idx].mGreenData.mValue, mAnimInfo.mKeyframes[idx].mGreenData.mTangent),
	                       AKeyInfo(mAnimInfo.mKeyframes[idx + 1].mTime, mAnimInfo.mKeyframes[idx + 1].mGreenData.mTime,
	                                mAnimInfo.mKeyframes[idx + 1].mGreenData.mValue, mAnimInfo.mKeyframes[idx + 1].mGreenData.mTangent));
	f32 blue  = subExtract(value,
	                       AKeyInfo(mAnimInfo.mKeyframes[idx].mTime, mAnimInfo.mKeyframes[idx].mBlueData.mTime,
	                                mAnimInfo.mKeyframes[idx].mBlueData.mValue, mAnimInfo.mKeyframes[idx].mBlueData.mTangent),
	                       AKeyInfo(mAnimInfo.mKeyframes[idx + 1].mTime, mAnimInfo.mKeyframes[idx + 1].mBlueData.mTime,
	                                mAnimInfo.mKeyframes[idx + 1].mBlueData.mValue, mAnimInfo.mKeyframes[idx + 1].mBlueData.mTangent));

	int r;
	if (red < 0.0f) {
		r = 0;
	} else if (red > 255.0f) {
		r = 255;
	} else {
		r = (u8)red;
	}

	target.r = r;

	int g;
	if (green < 0.0f) {
		g = 0;
	} else if (green > 255.0f) {
		g = 255;
	} else {
		g = (u8)green;
	}

	target.g = g;

	int b;
	if (blue < 0.0f) {
		b = 0;
	} else if (blue > 255.0f) {
		b = 255;
	} else {
		b = (u8)blue;
	}

	target.b = b;
}

/**
 * @todo: Documentation
 */
void PVWAlphaAnimInfo::extract(f32 value, Colour& target)
{
	// If there is no animation data, return without modification
	if (mAnimInfo.mSize == 0) {
		return;
	}

	if (mAnimInfo.mSize == 1) {
		target.a = mAnimInfo.mKeyframes[0].mData.mTime;
		return;
	}

	int idx = 0;
	for (int i = 0; i < (int)mAnimInfo.mSize - 1; i++) {
		if (mAnimInfo.mKeyframes[i].mTime <= value && mAnimInfo.mKeyframes[i + 1].mTime >= value) {
			idx = i;
			break;
		}
	}

	f32 alpha = subExtract(value,
	                       AKeyInfo(mAnimInfo.mKeyframes[idx].mTime, mAnimInfo.mKeyframes[idx].mData.mTime,
	                                mAnimInfo.mKeyframes[idx].mData.mValue, mAnimInfo.mKeyframes[idx].mData.mTangent),
	                       AKeyInfo(mAnimInfo.mKeyframes[idx + 1].mTime, mAnimInfo.mKeyframes[idx + 1].mData.mTime,
	                                mAnimInfo.mKeyframes[idx + 1].mData.mValue, mAnimInfo.mKeyframes[idx + 1].mData.mTangent));

	int a;
	if (alpha < 0.0f) {
		a = 0;
	} else if (alpha > 255.0f) {
		a = 255;
	} else {
		a = (u8)alpha;
	}

	target.a = a;
}

/**
 * @todo: Documentation
 */
void PVWColourShortAnimInfo::extract(f32 value, ShortColour& target)
{
	// If there is no animation data, return without modification
	if (mInfo.mSize == 0) {
		return;
	}

	if (mInfo.mSize == 1) {
		target.r = mInfo.mKeyframes[0].mRedData.mTime;
		target.g = mInfo.mKeyframes[0].mGreenData.mTime;
		target.b = mInfo.mKeyframes[0].mBlueData.mTime;
		return;
	}

	int idx = 0;
	for (int i = 0; i < (int)mInfo.mSize - 1; i++) {
		if (mInfo.mKeyframes[i].mTime <= value && mInfo.mKeyframes[i + 1].mTime >= value) {
			idx = i;
			break;
		}
	}

	f32 red   = subExtract(value,
	                       AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mRedData.mTime, mInfo.mKeyframes[idx].mRedData.mValue,
	                                mInfo.mKeyframes[idx].mRedData.mTangent),
	                       AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mRedData.mTime,
	                                mInfo.mKeyframes[idx + 1].mRedData.mValue, mInfo.mKeyframes[idx + 1].mRedData.mTangent));
	f32 green = subExtract(value,
	                       AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mGreenData.mTime,
	                                mInfo.mKeyframes[idx].mGreenData.mValue, mInfo.mKeyframes[idx].mGreenData.mTangent),
	                       AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mGreenData.mTime,
	                                mInfo.mKeyframes[idx + 1].mGreenData.mValue, mInfo.mKeyframes[idx + 1].mGreenData.mTangent));
	f32 blue  = subExtract(value,
	                       AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mBlueData.mTime,
	                                mInfo.mKeyframes[idx].mBlueData.mValue, mInfo.mKeyframes[idx].mBlueData.mTangent),
	                       AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mBlueData.mTime,
	                                mInfo.mKeyframes[idx + 1].mBlueData.mValue, mInfo.mKeyframes[idx + 1].mBlueData.mTangent));

	int r;
	if (red < -1023.0f) {
		r = -1023;
	} else if (red > 1023.0f) {
		r = 1023;
	} else {
		r = (s16)red;
	}

	target.r = r;

	int g;
	if (green < -1023.0f) {
		g = -1023;
	} else if (green > 1023.0f) {
		g = 1023;
	} else {
		g = (s16)green;
	}

	target.g = g;

	int b;
	if (blue < -1023.0f) {
		b = -1023;
	} else if (blue > 1023.0f) {
		b = 1023;
	} else {
		b = (s16)blue;
	}

	target.b = b;
}

/**
 * @todo: Documentation
 */
void PVWAlphaShortAnimInfo::extract(f32 value, ShortColour& target)
{
	// If there is no animation data, return without modification
	if (mInfo.mSize == 0) {
		return;
	}

	if (mInfo.mSize == 1) {
		target.a = mInfo.mKeyframes[0].mData.mTime;
		return;
	}

	int idx = 0;
	for (int i = 0; i < (int)mInfo.mSize - 1; i++) {
		if (mInfo.mKeyframes[i].mTime <= value && mInfo.mKeyframes[i + 1].mTime >= value) {
			idx = i;
			break;
		}
	}

	f32 alpha = subExtract(value,
	                       AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mData.mTime, mInfo.mKeyframes[idx].mData.mValue,
	                                mInfo.mKeyframes[idx].mData.mTangent),
	                       AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mData.mTime,
	                                mInfo.mKeyframes[idx + 1].mData.mValue, mInfo.mKeyframes[idx + 1].mData.mTangent));

	int a;
	if (alpha < 0.0f) {
		a = 0;
	} else if (alpha > 255.0f) {
		a = 255;
	} else {
		a = (s16)alpha;
	}

	target.a = a;
}

/**
 * @todo: Documentation
 */
void PVWTexAnimInfo::extract(f32 value, Vector3f& target)
{
	// If there is no animation data, return without modification
	if (mInfo.mSize == 0) {
		return;
	}

	if (mInfo.mSize == 1) {
		target.x = mInfo.mKeyframes[0].mRedData.mTime;
		target.y = mInfo.mKeyframes[0].mGreenData.mTime;
		target.z = mInfo.mKeyframes[0].mBlueData.mTime;
		return;
	}

	int idx = 0;
	if (value >= f32(mInfo.mKeyframes[mInfo.mSize - 1].mTime)) {
		target.x = mInfo.mKeyframes[mInfo.mSize - 1].mRedData.mTime;
		target.y = mInfo.mKeyframes[mInfo.mSize - 1].mGreenData.mTime;
		target.z = mInfo.mKeyframes[mInfo.mSize - 1].mBlueData.mTime;
		return;
	}
	for (int i = 0; i < (int)mInfo.mSize - 1; i++) {
		if (mInfo.mKeyframes[i].mTime <= value && mInfo.mKeyframes[i + 1].mTime >= value) {
			idx = i;
			break;
		}
	}

	target.x = subExtract(value,
	                      AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mRedData.mTime, mInfo.mKeyframes[idx].mRedData.mValue,
	                               mInfo.mKeyframes[idx].mRedData.mTangent),
	                      AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mRedData.mTime,
	                               mInfo.mKeyframes[idx + 1].mRedData.mValue, mInfo.mKeyframes[idx + 1].mRedData.mTangent));
	target.y = subExtract(value,
	                      AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mGreenData.mTime,
	                               mInfo.mKeyframes[idx].mGreenData.mValue, mInfo.mKeyframes[idx].mGreenData.mTangent),
	                      AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mGreenData.mTime,
	                               mInfo.mKeyframes[idx + 1].mGreenData.mValue, mInfo.mKeyframes[idx + 1].mGreenData.mTangent));
	target.z = subExtract(value,
	                      AKeyInfo(mInfo.mKeyframes[idx].mTime, mInfo.mKeyframes[idx].mBlueData.mTime,
	                               mInfo.mKeyframes[idx].mBlueData.mValue, mInfo.mKeyframes[idx].mBlueData.mTangent),
	                      AKeyInfo(mInfo.mKeyframes[idx + 1].mTime, mInfo.mKeyframes[idx + 1].mBlueData.mTime,
	                               mInfo.mKeyframes[idx + 1].mBlueData.mValue, mInfo.mKeyframes[idx + 1].mBlueData.mTangent));
}

/**
 * @todo: Documentation
 */
void PVWTextureData::animate(f32* framePtr, Matrix4f& mtx)
{
	if (mAnimationFactor == 255) {
		return;
	}

	Vector3f vec1(1.0f, 1.0f, 0.0f);
	Vector3f vec2(0.0f, 0.0f, 0.0f);
	Vector3f vec3(0.0f, 0.0f, 0.0f);

	if (!mTotalFrameCount) {
		if (!mIsMatrixDirty) {
			vec1.set(mScaleX, mScaleY, 0.0f);
			vec2.set(0.0f, 0.0f, mRotationZ);
			vec3.set(mTranslationX, mTranslationY, 0.0f);
		}
	} else {
		if (framePtr) {
			mCurrentFrame = std::fmodf(*framePtr, (f32)mTotalFrameCount);
		} else {
#if defined(PIKI_PC_PORT)
			if (pc_render_is_authoritative()) {
#endif
			mCurrentFrame += gsys->getFrameTime() * (30.0f * mAnimSpeed);
			if (mCurrentFrame >= f32(mTotalFrameCount - 1)) {
				mCurrentFrame = 0.0f;
			}
#if defined(PIKI_PC_PORT)
			}
#endif
		}

		mScaleInfo.extract(mCurrentFrame, vec1);
		mRotationInfo.extract(mCurrentFrame, vec2);
		mTranslationInfo.extract(mCurrentFrame, vec3);
		mIsMatrixDirty = false;
	}

	if (!mIsMatrixDirty) {
		f32 sinTheta = sinf(vec2.z * PI / 180.0f);
		f32 cosTheta = cosf(vec2.z * PI / 180.0f);

		mtx.mMtx[0][0] = vec1.x * cosTheta;
		mtx.mMtx[0][1] = -vec1.x * sinTheta;
		mtx.mMtx[0][2] = 0.0f;
		mtx.mMtx[0][3] = -vec1.x * cosTheta * mPivotX + vec1.x * sinTheta * mPivotY + mPivotX + vec3.x;

		mtx.mMtx[1][0] = vec1.y * sinTheta;
		mtx.mMtx[1][1] = vec1.y * cosTheta;
		mtx.mMtx[1][2] = 0.0f;
		mtx.mMtx[1][3] = -vec1.y * sinTheta * mPivotX - vec1.y * cosTheta * mPivotY + mPivotY + vec3.y;

		mtx.mMtx[2][0] = 0.0f;
		mtx.mMtx[2][1] = 0.0f;
		mtx.mMtx[2][2] = 1.0f;
		mtx.mMtx[2][3] = 0.0f;

		mtx.mMtx[3][0] = 0.0f;
		mtx.mMtx[3][1] = 0.0f;
		mtx.mMtx[3][2] = 0.0f;
		mtx.mMtx[3][3] = 1.0f;

		mIsMatrixDirty = true;
	}
}

/**
 * @todo: Documentation
 */
void PVWTextureData::read(RandomAccessStream& stream)
{
	mSourceAttrIndex = stream.readInt();
	_UNUSED0C        = stream.readShort();
	_UNUSED0E        = stream.readShort();
	_UNUSED10        = stream.readByte();
	_UNUSED11        = stream.readByte();
	_UNUSED12        = stream.readByte();
	_UNUSED13        = stream.readByte();
	mAnimationFactor = stream.readInt();
	mTotalFrameCount = stream.readInt();
	mAnimSpeed       = stream.readFloat();
	mScaleX          = stream.readFloat();
	mScaleY          = stream.readFloat();
	mRotationZ       = stream.readFloat();
	mTranslationX    = stream.readFloat();
	mTranslationY    = stream.readFloat();
	mPivotX          = stream.readFloat();
	mPivotY          = stream.readFloat();

	mScaleInfo.mInfo.read(stream);
	mRotationInfo.mInfo.read(stream);
	mTranslationInfo.mInfo.read(stream);

	if (mAnimationFactor != 0xFF) {
		(mAnimationFactor); // fake
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000094
 */
void PVWTexGenData::read(RandomAccessStream& input)
{
	mTexCoordID = input.readByte();
	mTexGenType = input.readByte();
	mTexGenSrc  = input.readByte();
	mMatrixType = input.readByte();
}

/**
 * @todo: Documentation
 */
void PVWTextureInfo::read(RandomAccessStream& input)
{
	mUseScale = input.readInt();
	mScale.read(input);
	mTexGenDataCount = input.readInt();
	if (mTexGenDataCount) {
		mTexGenData = new PVWTexGenData[mTexGenDataCount];

		for (int i = 0; i < mTexGenDataCount; i++) {
			mTexGenData[i].read(input);
		}
	}

	mTevStageCount    = 0;
	mTextureDataCount = input.readInt();
	if (mTextureDataCount) {
		mTextureData = new PVWTextureData[mTextureDataCount];

		for (int i = 0; i < mTextureDataCount; i++) {
			mTextureData[i].read(input);
			if (mTextureData[i]._UNUSED11 == 2) {
				mTevStageCount++;
			}
		}
	}
}

/**
 * @todo: Documentation
 */
void PVWTevColReg::animate(f32* framePtr, ShortColour& color)
{
	if (!mAnimFrameCount) {
		return;
	}

	if (framePtr) {
		mCurrentAnimFrame = std::fmodf(*framePtr, mAnimFrameCount);
	} else {
#if defined(PIKI_PC_PORT)
		if (pc_render_is_authoritative()) {
#endif
		mCurrentAnimFrame += gsys->getFrameTime() * (30.0f * mAnimSpeed);
		if (mCurrentAnimFrame >= f32(mAnimFrameCount - 1)) {
			mCurrentAnimFrame = 0.0f;
		}
#if defined(PIKI_PC_PORT)
		}
#endif
	}

	mColorAnimData.extract(mCurrentAnimFrame, color);
	mAlphaAnimData.extract(mCurrentAnimFrame, color);
}

/**
 * @todo: Documentation
 */
void Material::attach()
{
	gsys->mDGXGfx->useDList(gsys->mDGXGfx->compileMaterial(this));
}

/**
 * @todo: Documentation
 */
void Material::read(RandomAccessStream& input)
{
	mFlags        = input.readInt();
	mTextureIndex = input.readInt();
	colour().read(input);

	if (mFlags & MATFLAG_PVW) {
		mTevInfoIndex = input.readInt();
		mColourInfo.read(input);
		mLightingInfo.read(input);
		mPeInfo.read(input);
		mTextureInfo.read(input);
	}
}

/**
 * @todo: Documentation
 */
void Font::setTexture(Texture* tex, int numRows, int numCols)
{
	mTexture    = tex;
	mChars      = new FontChar[numRows * numCols];
	mCharWidth  = mTexture->mWidth / numRows;
	mCharHeight = mTexture->mHeight / numCols;

	int charIndex = 0;
	for (int col = 0; col < numCols; col++) {
		for (int row = 0; row < numRows; row++) {
			int leftEdge  = 0;
			int rightEdge = 0;

			// Find left boundary
			for (int pixelX1 = 0; pixelX1 < mCharWidth; pixelX1++) {
				int alphaCount = 0;
				for (int pixelY1 = 0; pixelY1 < mCharHeight - 1; pixelY1++) {
					u8 alpha = tex->getAlpha(row * mCharWidth + pixelX1, col * mCharHeight + pixelY1);
					if (alpha == 0) {
						alphaCount++;
					}
				}

				if (alphaCount != mCharHeight - 1) {
					break;
				}

				leftEdge = pixelX1;
			}

			// Find right boundary
			for (int pixelX2 = mCharWidth - 1; pixelX2 >= 0; pixelX2--) {
				int alphaCount = 0;
				for (int pixelY2 = 0; pixelY2 < mCharHeight - 1; pixelY2++) {
					u8 alpha = tex->getAlpha(row * mCharWidth + pixelX2, col * mCharHeight + pixelY2);
					if (alpha == 0) {
						alphaCount++;
					}
				}

				if (alphaCount != mCharHeight - 1) {
					break;
				}

				rightEdge = mCharWidth - pixelX2;
			}

			// Find baseline
			int baseline    = -1;
			int baselinePos = mCharWidth;
			for (int pixelX3 = 0; pixelX3 < mCharWidth; pixelX3++) {
				u8 alpha = tex->getAlpha(row * mCharWidth + pixelX3, col * mCharHeight + (mCharHeight - 1));
				if (baseline < 0) {
					if (alpha == 0) {
						baseline = pixelX3;
					}
				} else if (alpha != 0) {
					baselinePos = pixelX3;
					break;
				}
			}

			// This is ridiculous, seriously? why?
			// so much indexing, this isn't even an inline function
			mChars[charIndex].mCharSpacing         = baselinePos - baseline;
			mChars[charIndex].mLeftOffset          = baseline - leftEdge;
			mChars[charIndex].mTextureX            = row * mCharWidth + leftEdge;
			mChars[charIndex].mWidth               = mCharWidth - leftEdge - rightEdge;
			mChars[charIndex].mTextureY            = col * mCharHeight;
			mChars[charIndex].mHeight              = mCharHeight - 1;
			mChars[charIndex].mTextureCoords.mMinX = (s16)mChars[charIndex].mTextureX;
			mChars[charIndex].mTextureCoords.mMinY = (s16)mChars[charIndex].mTextureY;
			mChars[charIndex].mTextureCoords.mMaxX = (s16)mChars[charIndex].mTextureX + (s16)mChars[charIndex].mWidth;
			mChars[charIndex].mTextureCoords.mMaxY = (s16)mChars[charIndex].mTextureY + (s16)mChars[charIndex].mHeight - 1;

			charIndex++;
		}
	}
}

// this needs to be in a function, but it straight up never gets used, so.
static void unusedStatic()
{
	static const char* mappingTypes[3] = { "Normal", "Projection", "Environ" };
}

static u16 sjis_convert_table[0x258] = {
	0,     0x118, 0x119, 0xC,   0xE,   0x60,  0x1A,  0x1B,  0x1F,  0x1,   0x2,   0,     0,     0,     0,     0x3E,  0,     0,     0,
	0,     0,     0,     0,     0,     0x123, 0,     0,     0xD,   0xD,   0xD,   0xF,   0x3C,  0x11A, 0,     0,     0x117, 0,     0x7,
	0x7,   0x2,   0x2,   0x8,   0x9,   0x0,   0,     0x3B,  0x3D,  0,     0,     0,     0,     0,     0,     0x11F, 0x120, 0,     0,
	0,     0,     0xB,   0xD,   0,     0,     0,     0,     0x1D,  0,     0x1C,  0x1E,  0,     0,     0,     0,     0,     0,     0,
	0x7,   0x2,   0,     0,     0x4,   0,     0,     0x5,   0x3,   6,     0xA,   0x20,  0,     0,     0x122, 0,     0x121, 0,     0,
	0,     0,     0x5C,  0,     0,     0,     0,     0,     0,     0x11C, 0x11B, 0x11E, 0x11D, 0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     3,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0x10,  0x11,  0x12,  0x13,  0x14,  0x15,  0x16,  0x17,  0x18,  0x19,  0,     0,     0,     0,
	0,     0,     0,     0x21,  0x22,  0x23,  0x24,  0x25,  0x26,  0x27,  0x28,  0x29,  0x2A,  0x2B,  0x2C,  0x2D,  0x2E,  0x2F,  0x30,
	0x31,  0x32,  0x33,  0x34,  0x35,  0x36,  0x37,  0x38,  0x39,  0x3A,  0,     0,     0,     0,     0,     0,     0,     0x41,  0x42,
	0x43,  0x44,  0x45,  0x46,  0x47,  0x48,  0x49,  0x4A,  0x4B,  0x4C,  0x4D,  0x4E,  0x4F,  0x50,  0x51,  0x52,  0x53,  0x54,  0x55,
	0x56,  0x57,  0x58,  0x59,  0x5A,  0,     0,     0,     0,     0x61,  0x62,  0x63,  0x64,  0x65,  0x66,  0x67,  0x68,  0x69,  0x6A,
	0x6B,  0x6C,  0x6D,  0x6E,  0x6F,  0x70,  0x71,  0x72,  0x73,  0x74,  0x75,  0x76,  0x77,  0x78,  0x79,  0x7A,  0x7B,  0x7C,  0x7D,
	0x7E,  0x7F,  0x80,  0x81,  0x82,  0x83,  0x84,  0x85,  0x86,  0x87,  0x88,  0x89,  0x8A,  0x8B,  0x8C,  0x8D,  0x8E,  0x8F,  0x90,
	0x91,  0x92,  0x93,  0x94,  0x95,  0x96,  0x97,  0x98,  0x99,  0x9A,  0x9B,  0x9C,  0x9D,  0x9E,  0x9F,  0xA0,  0xA1,  0xA2,  0xA3,
	0xA4,  0xA5,  0xA6,  0xA7,  0xA8,  0xA9,  0xAA,  0xAB,  0xAC,  0xAD,  0xAE,  0xAF,  0xB0,  0xB1,  0xB2,  0xB3,  0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
	0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0xC1,
	0xC2,  0xC3,  0xC4,  0xC5,  0xC6,  0xC7,  0xC8,  0xC9,  0xCA,  0xCB,  0xCC,  0xCD,  0xCE,  0xCF,  0xD0,  0xD1,  0xD2,  0xD3,  0xD4,
	0xD5,  0xD6,  0xD7,  0xD8,  0xD9,  0xDA,  0xDB,  0xDC,  0xDD,  0xDE,  0xDF,  0xE0,  0xE1,  0xE2,  0xE3,  0xE4,  0xE5,  0xE6,  0xE7,
	0xE8,  0xE9,  0xEA,  0xEB,  0xEC,  0xED,  0xEE,  0xEF,  0xF0,  0xF1,  0xF2,  0xF3,  0xF4,  0xF5,  0xF6,  0xF7,  0xF8,  0xF9,  0xFA,
	0xFB,  0xFC,  0xFD,  0xFE,  0xFF,  0,     0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10A, 0x10B, 0x10C,
	0x10D, 0x10E, 0x10F, 0x110, 0x111, 0x112, 0x113, 0x114, 0x115, 0x116, 0x0,
};

// Shift-JIS bytes of the kanji named by two-byte message codes.
static char kanji_convert_table[] ATTRIBUTE_ALIGN(32) = {
#if defined(VERSION_GPIP01)
	"\223\372\226\332\216\236\216\204\226\242\222\155\220\257\221\345\222\156\x85\x20\226\274\221\117\211\106\222\210\227\267\223\162\222\206\227\254\222\304\227\216\210\244\213\100\215\206\221\275\216\270\226\263\216\143\216\160\223\256\210\352\217\144\214\175\213\263\213\103\226\322\223\305\216\137\221\146\212\334\220\266\226\275\210\333\216\235\221\225\222\165\214\300\223\170\217\103\225\234\215\154\221\104\222\124\214\237\212\357\226\255\225\250\221\314\221\322\227\247\217\343\211\275\212\102\212\317\216\100"
	"\211\350\214\365\225\372"
#else
	"\223\372\226\332\216\236\216\204\226\242\222\155\220\257\221\345\222\156\211\241\226\274\221\117\211\106\222\210\227\267\223\162\222\206\227\254\222\304\227\216\210\244\213\100\215\206\221\275\216\270\226\263\216\143\216\160\223\256\210\352\217\144\214\175\213\263\213\103\226\322\223\305\216\137\221\146\212\334\220\266\226\275\210\333\216\235\221\225\222\165\214\300\223\170\217\103\225\234\215\154\221\104\222\124\214\237\212\357\226\255\225\250\221\314\221\322\227\247\217\343\211\275\212\102\212\317\216\100\211\350\214\365\225\372"
#endif
	"\216\166\216\216\210\370\224\262\220\101\214\251\212\121\215\104\226\354\215\330\214\304\223\212\224\275\211\236\221\200\215\354\211\361\223\135\215\241\227\210\223\174\216\346\221\235\220\102\225\352\224\336\221\324\225\163\213\143\217\255\224\134\227\315\222\262\225\113\227\166\213\273\226\241\217\127\222\143\215\207\215\163\217\113\220\253\227\230\227\160\214\112\212\363\226\135\223\224\213\337\211\237\211\360\216\125\221\340\227\361\213\114\230\136\211\346\226\312\210\300\220\123\215\113\211\136\216\226\227\243\227\244\211\302"
	"\224\362\226\276\221\173\215\365\214\343\223\374\220\130\215\360\217\157\211\272\221\123\226\305\216\355\222\315\220\330\220\106\223\257\220\324\222\215\210\323\220\133\214\373\225\224\225\252\221\274\210\341\211\251\213\101\223\301\214\212\214\100\220\316\212\204\223\340\215\202\216\373\214\302\224\315\210\315\226\166\221\276\227\172\216\144\210\303\213\216\220\110\224\255\211\327\223\140\216\277\222\264\211\310\212\167\214\213\217\273\217\234\211\230\210\332\211\151\213\166\224\122\227\277\223\144\215\317\210\312\212\155\224\106"
	"\212\324\222\100\223\226\215\305\214\203\226\173\215\310\216\161\213\237\212\347\225\202\216\313\220\374\211\223\212\117\211\271\226\302\226\360\221\247\215\300\226\272\214\263\213\363\214\207\227\222\213\255\225\254\222\105\221\254\211\301\224\357\227\307\211\306\214\166\217\325\214\202\213\172\210\237\225\241\222\312\216\107\217\352\217\212\225\203\220\145\220\154\224\374\215\347\225\312\212\264\221\261\213\340\221\256\225\151\224\203\227\235\227\126\211\152\215\230\224\323\214\311\210\310\217\363\216\300\221\147\210\331\217\355"
	"\225\267\211\365\224\172\217\346\226\361\221\251\224\366\214\273\224\117\214\314\217\111\212\252\223\252\223\171\220\273\222\154\221\305\226\351\212\171\220\147\214\374\216\132\211\312\217\304\225\326\220\254\214\367\213\301\217\217\225\351\225\164\216\330\226\337\217\270\221\213\215\114\226\236\221\321\216\156\230\146\221\230\223\357\213\246\216\322\213\275\222\267\226\260\226\373\222\146\225\236\212\105\222\102\224\361\221\227\223\254\215\266\225\134\216\246\221\260\221\266\215\335\222\245\216\250\211\234\212\112\223\264\214\101"
	"\220\302\211\357\216\213\216\350\221\253\217\254\220\205\216\251\217\225\217\301\214\264\216\374\225\323\211\146\217\134\216\232\214\370\227\246\212\341\222\350\215\176\216\367\212\103\224\365\221\201\217\101\216\274\214\271\222\251\220\151\212\256\227\373\220\121\216\167\220\263\222\274\227\105\217\211\213\260\214\151\210\365\214\257\230\142\217\356\225\361\220\224\225\317\211\273\227\136\225\373\220\256\225\261\220\126\223\271\214\142\222\120\224\273\227\164\211\324\222\207\221\122\212\154\226\140\224\152\211\363\223\107\214\227"
	"\225\275\225\340\211\212\223\126\227\221\223\356\220\332\220\107\216\212\216\200\215\125\213\354\224\311\211\150\213\220\212\333\223\333\222\145\224\274\220\334\226\105\223\146\230\101\215\366\227\220\220\125\225\245\214\230\226\262\222\307\222\213\225\174\227\150\217\344\220\346\216\176\223\267\216\343\223\137\212\310\215\142\227\205\212\342\227\312\215\236\225\337\223\247\212\152\226\100\223\173\223\172\223\231\215\134\226\357\211\356\213\264\213\352\230\112\214\156\212\114\220\136\216\354\211\277\343\251\213\326\221\203\225\227"
	"\221\242\220\250\220\355\217\120\214\276\216\325\230\141\214\265\226\247\222\270\222\205\213\170\224\116\220\314\221\172\221\234\213\277\225\324\224\222\225\161\215\304\212\322\211\344\214\234\221\220\222\116\215\110\214\216\216\151\224\257\226\321\215\327\224\324\227\134\216\352\211\145\211\337\212\326\214\127\214\326\210\347\225\240\216\237\226\356\222\202\220\241\212\157\224\140\211\256\226\131\214\214\215\161\216\217\226\123\220\362\227\133\216\147\212\372\224\163\227\145\220\247\221\175\223\307\210\265\220\340\217\221\225\302"
	"\220\321\225\103\227\335\221\326\212\160\215\130\225\357\340\326\220\201\213\366\210\253\216\227\223\344\216\347\227\114\214\171\227\154\215\220\225\220\212\355\212\353\225\366\217\207\214\140\227\173\213\253\223\315\227\274\211\161\212\321\211\121\215\323\225\243\227\364\226\175\213\140\226\261\213\115\226\210\221\257\223\313\221\252\222\353\211\176\343\304\226\243\225\166\222\201\222\341\224\232\217\132\217\316"
};

/**
 * @todo: Documentation
 */
int Font::charToIndex(char c)
{
	return c - 0x20;
}

/**
 * @todo: Documentation
 */
static s16 searchKanjiCode(u16 code)
{
	u32 codeBytes[2];
	codeBytes[0] = u16(code >> 8) & 0xFF;
	codeBytes[1] = code & 0xFF;

	s16 len = strlen(kanji_convert_table) >> 1;
	s16 res = -1;

	for (s16 i = 0; i < len; i++) {
		if ((u8)kanji_convert_table[2 * i] == codeBytes[0] && (u8)kanji_convert_table[2 * i + 1] == codeBytes[1]) {
			res = i;
			break;
		}
	}

	return res;
}

/**
 * @todo: Documentation
 */
int Font::charToIndex(u16 c)
{
	if (c >= 0x8140 && c <= 0x8396) {
		return sjis_convert_table[c - 0x8140];
	}

	int kanji = searchKanjiCode(c);
	return ((s16)kanji != -1) ? (s16)kanji + 0x126 : '@' + 31; // Return the index for '@' if an error occurred.
}

/**
 * @todo: Documentation
 */
int Font::stringWidth(immut char* str)
{
	int width = 0;
	while (str[0] != '\0') {
		int idx;
#if defined(VERSION_GPIP01)
		STACK_PAD_VAR(2);
		// Avoid impossible condition by converting to `u8`.
		u8 extendedAsciiCodepoint = str[0];
		if (extendedAsciiCodepoint >= 0xa0) {
			idx = charToIndex(str[0]);
			str += 1;
		} else
#endif
		    if (str[0] & 0x80) {
			u16 c = (static_cast<u8>(str[0]) << 8) | (static_cast<u8>(str[1]) << 0);
			idx   = charToIndex(c);
			str += 2;
		} else {
			idx = charToIndex(str[0]);
			str += 1;
		}
		width += mChars[idx].mCharSpacing;
	}

	return width + 1;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0003A0
 */
void GfxInfo::createCollData(immut Vector3f* pos, f32)
{
	mTriangles = new CollTriInfo[mTriangleCountPtr[0]];
	mBox.resetBound();
	pos->length(); // this is load-bearing for sdata2 ordering
	               // more here, but this is enough to get the weak ctors to spawn
}

/**
 * @todo: Documentation
 */
void MaterialHandler::setMaterial(Material* mat)
{
	mGfx->setMaterial(mat, false);
}

/**
 * @todo: Documentation
 */
void MaterialHandler::setTexMatrix(bool enable)
{
	mGfx->initReflectTex(enable);
}

/**
 * @todo: Documentation
 */
Graphics::Graphics()
{
	PRINT("dgxgraphics constructor\n");

	mRenderMode = 0;
	mIsLightingEnabled = false;
	mIsDepthEnabled    = true;
	mHasTexGen         = FALSE;
	mBlendMode         = 0;
	mCullMode          = 0;
	mCullFlip          = 0;

	for (int i = 0; i < 0x1000; i++) {
		sintable[i] = NMathF::sin(TAU * (i / 4096.0f));
		costable[i] = NMathF::cos(TAU * (i / 4096.0f));
	}

	mActiveTexture[0] = nullptr;
	mActiveTexture[1] = nullptr;
	mActiveTexture[2] = nullptr;
	mActiveTexture[3] = nullptr;
	mActiveTexture[4] = nullptr;
	mActiveTexture[5] = nullptr;
	mActiveTexture[6] = nullptr;
	mActiveTexture[7] = nullptr;

	_308 = 0;

	mCurrentMaterial = nullptr;
	mLightCam        = nullptr;

	// Well I suppose this has to initialize *somewhere*.
	const_cast<Matrix4f&>(Matrix4f::ident).makeIdentity();

	mCurrentMaterialHandler = nullptr;
	mDefaultMaterialHandler = new MaterialHandler();

	mMaxMatrixCount = gsys->mMatrixCount;
	mMatrixBuffer   = gsys->mMatrices;

	mCachedShapeMax = 1000;
#if defined(PIKI_PC_PORT)
	{
		const int limit = pc_settings_get_piki_limit();
		if (limit > MAX_PIKI_ON_FIELD) {
			const int scaled = int((long long)mCachedShapeMax * limit / MAX_PIKI_ON_FIELD);
			if (scaled > mCachedShapeMax) {
				mCachedShapeMax = scaled;
			}
		}
	}
#endif
	mCachedShapes   = new CachedShape[mCachedShapeMax];
#if defined(PIKI_PC_PORT)
	mPcPose         = nullptr;
	mPcPoseShape    = nullptr;
	mPcPoseAnimCtx  = 0;
	mPcPoseAnimData = 0;
#endif

	mAmbientColour.set(0, 0, 48, 255);
	mLightIntensity = 1.0f;
#if defined(__vita__)
	mPcNoPrev       = 0;
	mPcIsClone      = 0;
	mPcDebugSnap    = 0;
	mPcCacheHoisted = 0;
#endif
}

#if defined(__vita__)
// drawrec: gW, the Pikmin B segment's Graphics.
Graphics::Graphics(PcCloneTag)
{
	mRenderMode         = 0;
	mMatRenderMask      = 0;
	mLastModelMatrix    = nullptr;
	mActiveMatrix       = nullptr;
	mCamera             = nullptr;
	for (int i = 0; i < 8; i++) {
		mActiveTexture[i] = nullptr;
	}
	_308                = 0;
	mScreenWidth        = 0;
	mScreenHeight       = 0;
	mIsLightingEnabled  = false;
	mIsDepthEnabled     = true;
	mHasTexGen          = FALSE;
	mMtxDepIdx          = 0;
	mBlendMode          = 0;
	mCullMode           = 0;
	mCullFlip           = 0;
	mLightCam           = nullptr;
	_348                = 0;
	mDefaultMaterialHandler = nullptr;
	mCurrentMaterialHandler = nullptr;
	mCurrentMaterial    = nullptr;
	mFogStart = mFogEnd = mFogDensity = 0.0f;
	mLightDistance      = 0.0f;
	mLightIntensity     = 1.0f;
	mActiveLightMask    = 0;
	mLineWidth          = 1.0f;
	mCustomScale        = nullptr;
	mMatrixBuffer       = nullptr;
	mMaxMatrixCount     = 0;
	mNextFreeMatrixIdx  = 0;
	mCachedShapes       = nullptr;
	mCachedShapeMax     = 0;
	mCachedShapeCount   = 0;
	mPcPose             = nullptr;
	mPcPoseShape        = nullptr;
	mPcPoseAnimCtx      = 0;
	mPcPoseAnimData     = 0;
	mPcNoPrev           = 0;
	mPcIsClone          = 1;
	mPcDebugSnap        = 0;
	mPcCacheHoisted     = 0;
}

bool Graphics::pcDebugInfo() const
{
	return mPcIsClone ? mPcDebugSnap != 0 : gsys->mToggleDebugInfo != 0;
}

// drawrec: the fields calcLighting(1.0f) leaves, without the GX loads.
void Graphics::pcLightingModel()
{
	mLightIntensity  = 1.0f;
	s32 lightIndex   = 0;
	mActiveLightMask = 0;
	FOREACH_NODE(Light, mLight.mChild, l)
	{
		mActiveLightMask |= 1 << lightIndex;
		if (++lightIndex == 7u) {
			break;
		}
	}
	if (mLight.mChild) {
		mLightDistance = 50.0f;
	}
	GColor& c    = static_cast<DGXGraphics*>(this)->mPcDgx->colors[0];
	c.mAmbColor.r = mAmbientColour.r;
	c.mAmbColor.g = mAmbientColour.g;
	c.mAmbColor.b = mAmbientColour.b;
	c.mAmbColor.a = f32(mAmbientColour.a) * mLightIntensity;
}
#endif

/**
 * @todo: Documentation
 */
void Graphics::initRender(int screenWidth, int screenHeight)
{
	mActiveLightMask = 0;
	mLight.initCore("");
	resetMatrixBuffer();
	resetCacheBuffer();
}

/**
 * @todo: Documentation
 */
void Graphics::resetMatrixBuffer()
{
	mNextFreeMatrixIdx = 0;
}

/**
 * @todo: Documentation
 */
Matrix4f* Graphics::getMatrices(int requestedMatrixCount)
{
	if (mNextFreeMatrixIdx + requestedMatrixCount > mMaxMatrixCount) {
		ERROR("using too many matrices!! (%d + %d > %d)\n", mNextFreeMatrixIdx, requestedMatrixCount, mMaxMatrixCount);
	}

	Matrix4f* mtx = &mMatrixBuffer[mNextFreeMatrixIdx];
	mNextFreeMatrixIdx += requestedMatrixCount;
	return mtx;
}

/**
 * @todo: Documentation
 */
void Graphics::resetCacheBuffer()
{
	mShapeCache.mNext = &mShapeCache;
	mShapeCache.mPrev = &mShapeCache;

	mCachedShapeCount = 0;
}

/**
 * @todo: Documentation
 */
void Graphics::cacheShape(BaseShape* shape, ShapeDynMaterials* mats)
{
	if (mCachedShapeCount >= mCachedShapeMax) {
		ERROR("using too many shapes!! (%d >= %d)\n", mCachedShapeCount, mCachedShapeMax);
	}

	CachedShape* cache = &mCachedShapes[mCachedShapeCount];
#if defined(PIKI_PC_PORT)
	// The pose being drawn.
	Matrix4f* const anim = (shape == mPcPoseShape) ? mPcPose : shape->mAnimMatrices;
	Vector3f pos(anim[0].mMtx[0][3], anim[0].mMtx[1][3], anim[0].mMtx[2][3]);
#else
	Vector3f pos(shape->getAnimMatrix(0).mMtx[0][3], shape->getAnimMatrix(0).mMtx[1][3], shape->getAnimMatrix(0).mMtx[2][3]);
#endif
	cache->mDistanceFromOrigin = pos.length();

	bool insertHappened = false;
	for (CachedShape* i = mShapeCache.mPrev; i != &mShapeCache; i = i->mPrev) {
		if (cache->mDistanceFromOrigin >= i->mDistanceFromOrigin) {
			i->insertAfter(cache);
			insertHappened = true;
			break;
		}
	}

	if (!insertHappened) {
		mShapeCache.insertAfter(cache);
	}

	cache->mAnimatedMaterials = mats;
	cache->mParentShape       = (Shape*)shape;
#if defined(PIKI_PC_PORT)
	cache->mAnimMatrices = anim;
#else
	cache->mAnimMatrices = shape->mAnimMatrices;
#endif
	mCachedShapeCount++;
}

/**
 * @todo: Documentation
 */
void Graphics::flushCachedShapes()
{
#if defined(__vita__)
	// drawrec: join the chain before the flush.
	if (!mPcIsClone && g_pcDrChain != 0) {
		bool own = g_pcDrChainHoisted != 0;
		for (CachedShape* i = mShapeCache.mPrev; !own && i != &mShapeCache; i = i->mPrev) {
			own = i->mParentShape->mPcSegChain == g_pcDrChain;
			for (ShapeDynMaterials* j = i->mAnimatedMaterials; !own && j; j = j->mNext) {
				own = j->mModel && j->mModel->mPcSegChain == g_pcDrChain;
			}
		}
		if (own) {
			vita_drawrec_join(kDrJoinFlush);
		}
	}
#endif
	u32 oldRenderState = mMatRenderMask;

	mMatRenderMask = MATFLAG_AlphaBlend;
	for (CachedShape* i = mShapeCache.mPrev; i != &mShapeCache; i = i->mPrev) {
		if (i->mAnimatedMaterials) {
			for (ShapeDynMaterials* j = i->mAnimatedMaterials; j; j = j->mNext) {
				j->updateContext();
			}
		}

		i->mParentShape->mAnimMatrices = i->mAnimMatrices;
		drawMeshes(*mCamera, i->mParentShape);
	}
	mMatRenderMask = oldRenderState;
}

static TexImgFormat convFmts[] = {
	TEX_FMT_I4,   TEX_FMT_I8,   TEX_FMT_IA4,  TEX_FMT_IA8,  TEX_FMT_RGB565, TEX_FMT_RGB5A3, TEX_FMT_RGBA8, TEX_FMT_NULL,
	TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_NULL, TEX_FMT_NULL,   TEX_FMT_NULL,   TEX_FMT_S3TC,
};

/**
 * @todo: Documentation
 * @note UNUSED Size: 000018 (Matching by size)
 */
TexImgFormat TexImg::convFormat(u32 inFmt)
{
	return convFmts[inFmt];
}

static immut char* fmtNames[] = {
	"TEX_FMT_RGB565", "TEX_FMT_S3TC", "TEX_FMT_RGB5A3", "TEX_FMT_I4", "TEX_FMT_I8", "TEX_FMT_IA4", "TEX_FMT_IA8", "TEX_FMT_RGBA8",
};

/**
 * @todo: Documentation
 * @note UNUSED Size: 000018
 */
immut char* TexImg::formatName(u32 format)
{
	return fmtNames[format];
}

/**
 * @todo: Documentation
 */
int TexImg::calcDataSize(int format, int width, int height)
{
	s32 size = 0;

	switch (format) {
	case TEX_FMT_S3TC:
	{
		size = (width * height / 8) * 4;
		break;
	}
	case TEX_FMT_RGB5A3:
	{
		size = width * height * 2;
		break;
	}
	case TEX_FMT_RGB565:
	{
		size = width * height * 2;
		break;
	}
	case TEX_FMT_I4:
	{
		size = width * height / 2;
		break;
	}
	case TEX_FMT_I8:
	{
		size = width * height;
		break;
	}
	case TEX_FMT_IA4:
	{
		size = width * height;
		break;
	}
	case TEX_FMT_IA8:
	{
		size = width * height * 2;
		break;
	}
	case TEX_FMT_RGBA8:
	{
		size = width * height * 4;
		break;
	}
	case TEX_FMT_Z8:
	{
		size = width * height;
		break;
	}
	case TEX_FMT_COUNT: // what?
	{
		size = width * height;
		break;
	}
	}

	return size;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00005C (Matching by size)
 */
void TexImg::setColour(immut Colour& colour)
{
	mFormat     = TEX_FMT_RGB565;
	u16* pixels = static_cast<u16*>(mTextureData);
	for (int i = 0; i < mWidth * mHeight; ++i) {
		u16 pixel = ((colour.r >> 3) << 11) | ((colour.g >> 2) << 5) | ((colour.b) >> 3);
		// In the DLL, there is a byteswap here apparently not present in the DOL.
		pixels[i] = pixel;
	}
}

/**
 * @todo: Documentation
 */
void TexImg::read(RandomAccessStream& stream)
{
	mWidth         = stream.readShort();
	mHeight        = stream.readShort();
	mFormat        = static_cast<TexImgFormat>(stream.readInt());
	mImageCount    = stream.readInt();

	s32 _ = stream.readInt();
	_     = stream.readInt();
	_     = stream.readInt();
	_     = stream.readInt();

	mDataSize    = stream.readInt();
	mTextureData = new (PIKI_ALIGNED(0x20)) u8[mDataSize];
	stream.read(mTextureData, mDataSize);
}

/**
 * @todo: Documentation
 */
void TexImg::getTileSize(int format, u32& xSize, u32& ySize)
{
	switch (format) {
	case 3:
	{
		xSize = 8;
		ySize = 8;
		break;
	}
	case 4:
	case 5:
	case 9:
	{
		xSize = 8;
		ySize = 4;
		break;
	}
	default:
	{
		xSize = 4;
		ySize = 4;
		break;
	}
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00014C
 */
void TexImg::readTexData(Texture* tex, RandomAccessStream& stream, u8* data)
{
	mDataSize = TexImg::calcDataSize(mFormat, mWidth, mHeight);

	if (!data) {
		mTextureData = new (PIKI_ALIGNED(0x20)) u8[mDataSize];
	} else {
		mTextureData = data;
	}

	stream.read(mTextureData, mDataSize);
	TexImg::getTileSize(mFormat, tex->mTileSizeX, tex->mTileSizeY);
	tex->mWidthFactor  = 1.0f / mWidth;
	tex->mHeightFactor = 1.0f / mHeight;
	tex->decodeData(this);
}

static const char* btitexFmts[] = {
	"GX_TF_I4", "GX_TF_I8", "GX_TF_IA4", "GX_TF_IA8", "GX_TF_RGB565", "GX_TF_RGB5A3", "GX_TF_RGBA8", "INVALID",
	"INVALID",  "INVALID",  "INVALID",   "INVALID",   "INVALID",      "INVALID",      "GX_TF_CMPR",
};

static const char* btipalFmts[] = {
	"GX_TF_C4",
	"GX_TF_C8",
	"GX_TF_C14X2",
};

/**
 * @todo: Documentation
 * @note UNUSED Size: 000004 (Matching by size, technically, but not in a satisfying way)
 */
void TexImg::dumpBti(Texture* tex, immut char* name, RandomAccessStream& input, RandomAccessStream& output)
{
#ifdef WIN32
	// read BTI function
	// blah blah

	// if some bollocks
	// now lets get to the fun parts

	// this is wrong btw, I can't be bothered to check if this shows up
	// but im doing it anyway, **** it
	output.print("// %s (%d colour) format %s : data = %d bytes\n", output.mPath, 0, btipalFmts[tex->mTexFormat]);
	output.print("// texture size = %d x %d\n");
	output.print("static u16 %s_palette[%d] ATTRIBUTE_ALIGN(32) = {\n");
	output.print("\t");
	PRINT("sending colour %d, %d, %d\n");
	output.print("0x%04x,");
	output.print("\n");
	output.print("\t};\n\n");

	// so much code i skipped over lol
	output.print("// %s got normal image of format %s : data = %d bytes\n");
	output.print("// texture size = %d x %d\n");

	output.print("static u8 %s_data[%d] ATTRIBUTE_ALIGN(32) = {\n");
	output.print("\t");
	output.print("0x%02x,");
	output.print("\n");
	output.print("\t};\n\n");

#endif
}

/**
 * @todo: Documentation
 */
void TexImg::importBti(Texture* tex, RandomAccessStream& input, u8* data)
{
	BtiHeader bti;
	bti.read(input);

	mFormat = convFormat(bti.mImageFormat);
	mWidth  = bti.mWidth;
	mHeight = bti.mHeight;

	u32 x;
	u32 y;
	getTileSize(mFormat, x, y);
	if (mWidth % x || mHeight % y) {
		ERROR("Texture %s is not correct size %d x %d\n", input.mPath, mWidth, mHeight);
	}
	if (bti.mImageDataOffset != sizeof(BtiHeader)) {
		ERROR("Bti file has imageData at offset %d!!!\n", bti.mImageDataOffset);
	}

	tex->mTexFlags = (!bti.mWrapS ? Texture::TEX_CLAMP_S : 0) | (!bti.mWrapT ? Texture::TEX_CLAMP_T : 0);

	readTexData(tex, input, data);
}

/**
 * @todo: Documentation
 */
void TexImg::importTxe(Texture* tex, RandomAccessStream& stream)
{
	mWidth         = stream.readShort();
	mHeight        = stream.readShort();
	mFormat        = static_cast<TexImgFormat>((u16)stream.readShort());
	tex->mTexFlags = mFormat >> 8;

	mFormat = static_cast<TexImgFormat>(mFormat & 0xFF);

	s16 _  = stream.readShort();
	s32 __ = stream.readInt();
	for (int i = 0; i < 10; i++) {
		stream.readShort();
	}

	readTexData(tex, stream, nullptr);
}

/**
 * @todo: Documentation
 */
void TexAttr::initImage()
{
	mTexture = new Texture();

	if (!mTexture) {
		ERROR("could not make texture!!\n");
	}

	TexImg::getTileSize(mImage->mFormat, mTexture->mTileSizeX, mTexture->mTileSizeY);

	if (mUseOffsetImgData) {
		mTexture->mLODCount    = mImage->mImageCount - 1;
		mTexture->mLODBias     = mLODBias;
	} else {
		mTexture->mLODCount    = 0;
		mTexture->mLODBias     = 0.0f;
	}

	mTexture->mWidthFactor  = 1.0f / mImage->mWidth;
	mTexture->mHeightFactor = 1.0f / mImage->mHeight;

	mTexture->mTexFlags = mTilingType;

	int i = strlen(gsys->mCurrentShape->mName) - 1;
	for (i; i >= 0; i--) {
		if (gsys->mCurrentShape->mName[i] == '/') {
			break;
		}
	}

	char path[PATH_MAX];
	sprintf(path, "%s:%d", &gsys->mCurrentShape->mName[i + 1], mTextureIndex);

	gsys->addTexture(mTexture, path);
	mTexture->decodeData(mImage);
}

/**
 * @todo: Documentation
 */
void TexAttr::read(RandomAccessStream& stream)
{
	mTextureIndex = stream.readShort();
	stream.readShort();
	mTilingType       = stream.readShort();
	mUseOffsetImgData = stream.readShort();
	mLODBias          = stream.readFloat();
}

/**
 * @todo: Documentation
 */
void Graphics::drawCylinder(immut Vector3f& start, immut Vector3f& end, f32 radius, immut Matrix4f& transformMtx)
{
	useTexture(nullptr, GX_TEXMAP0);

	Vector3f vec1(start);
	Vector3f vec2 = end - start;
	f32 distance  = vec2.normalise();

	vec2 = vec2 * (distance / 16.0f);
	for (int i = 0; i < 16; i++) {
		Matrix4f mtx1;
		Matrix4f mtx2;

		mtx1.makeSRT(Vector3f(1.0f, 1.0f, 1.0f), Vector3f(0.0f, i * (PI / 8.0f), 0.0f), vec1);
		transformMtx.multiplyTo(mtx1, mtx2);
		useMatrix(mtx2, 0);

		for (int j = 0; j < 16; j++) {
			f32 theta0 = j * (PI / 8.0f);
			f32 theta1 = (j + 1) % 32 * (PI / 8.0f);

			drawLine(Vector3f(sinf(theta0) * radius, 0.0f, cosf(theta0) * radius),
			         Vector3f(sinf(theta1) * radius, 0.0f, cosf(theta1) * radius));
		}

		vec1 = vec1 + vec2;
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0001E8 (Matching by size)
 */
void Graphics::drawCircle(immut Vector3f& center, f32 radius, immut Matrix4f& transformMtx)
{
	useTexture(nullptr, GX_TEXMAP0);

	for (int i = 0; i < 1; i++) {
		Matrix4f mtx1;
		Matrix4f mtx2;

		mtx1.makeSRT(Vector3f(1.0f, 1.0f, 1.0f), Vector3f(0.0f, i * (PI / 8.0f), 0.0f), center);
		transformMtx.multiplyTo(mtx1, mtx2);
		useMatrix(mtx2, 0);

		for (int j = 0; j < 16; j++) {
			f32 theta0 = j * (PI / 8.0f);
			f32 theta1 = (j + 1) % 32 * (PI / 8.0f);

			drawLine(Vector3f(sinf(theta0) * radius, 0.0f, cosf(theta0) * radius),
			         Vector3f(sinf(theta1) * radius, 0.0f, cosf(theta1) * radius));
		}
	}
}

/**
 * @todo: Documentation
 */
void Graphics::drawSphere(immut Vector3f& center, f32 radius, immut Matrix4f& transformMtx)
{
	useTexture(nullptr, GX_TEXMAP0);

	for (int i = 0; i < 16; i++) {
		Matrix4f mtx1;
		Matrix4f mtx2;

		mtx1.makeSRT(Vector3f(1.0f, 1.0f, 1.0f), Vector3f(0.0f, i * (PI / 8.0f), 0.0f), center);
		transformMtx.multiplyTo(mtx1, mtx2);
		useMatrix(mtx2, 0);

		for (int j = 0; j < 16; j++) {
			f32 theta0 = j * (PI / 8.0f);
			f32 theta1 = (j + 1) % 32 * (PI / 8.0f);

			drawLine(Vector3f(sinf(theta0) * radius, cosf(theta0) * radius, 0.0f),
			         Vector3f(sinf(theta1) * radius, cosf(theta1) * radius, 0.0f));
		}
	}
}

/**
 * @todo: Documentation
 */
void Graphics::perspPrintf(Font* font, immut Vector3f& pos, int p3, int p4, immut char* fmt, ...)
{
	char buf[PATH_MAX];
	va_list vlist;
	va_start(vlist, fmt);
	vsprintf(buf, fmt, vlist);
	useTexture(font->mTexture, GX_TEXMAP0);
	int x = p3;
	int y = p4;
	for (int i = 0; i < (int)strlen(buf); i++) {
		RectArea rect(x - font->mChars[buf[i] - 32].mLeftOffset, font->mChars[buf[i] - 32].mHeight + y,
		              (x - font->mChars[buf[i] - 32].mLeftOffset) + font->mChars[buf[i] - 32].mWidth, y);
		drawRectangle(rect, font->mChars[buf[i] - 32].mTextureCoords, &pos);
		x += font->mChars[buf[i] - 32].mCharSpacing;
	}
}

/**
 * @todo: Documentation
 */
int Graphics::calcLighting(f32 intensity)
{
	mLightIntensity = intensity;

	setAmbient();

	s32 lightIndex   = 0;
	mActiveLightMask = 0;

	FOREACH_NODE(Light, mLight.mChild, l)
	{
		mActiveLightMask |= 1 << lightIndex;
		setLight(l, lightIndex);

		// 8 light maximum, restricted by GX
		if (++lightIndex == 7u) {
			break;
		}
	}

	if (mLight.mChild) {
		mLightDistance = 50.0f;
		setLight((Light*)mLight.mChild, 7);
	}

#if defined(__vita__)
	s32& alc = mPcIsClone ? *reinterpret_cast<s32*>(&gPcDgxSub.local[5]) : gsys->mActiveLightCount;
	if (lightIndex > alc) {
		alc = lightIndex;
	}
#else
	if (lightIndex > gsys->mActiveLightCount) {
		gsys->mActiveLightCount = lightIndex;
	}
#endif

#if defined(__vita__)
	setLightingNoPrev(true, nullptr);
#else
	setLighting(true, nullptr);
#endif
	return lightIndex;

	STACK_PAD_VAR(2);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0001D0
 */
int Graphics::calcSphereLighting(immut Vector3f&, f32)
{
	TRAP_UNIMPLEMENTED;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0003A4
 */
int Graphics::calcBoxLighting(immut BoundBox&)
{
	TRAP_UNIMPLEMENTED;
}

/**
 * @todo: Documentation
 */
void CacheTexture::makeResident()
{
	if (gsys->mCacher && !gsys->mIsLoadingActive) {
		if (!mActiveCache) {
			gsys->mCacher->cacheTexture(this);
			gsys->copyCacheToTexture(this);
		} else {
			gsys->mCacher->updateInfo(this);
		}
	}
}
